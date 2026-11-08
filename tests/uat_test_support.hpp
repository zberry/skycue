#pragma once

// Test-only UAT downlink frame builder: the inverse of the layout documented
// in decode/uat_frame.hpp (public UAT ADS-B description, DO-282B), so the
// decoder tests build their vectors from stated values.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued::testing {

using Bytes = std::vector<uint8_t>;

struct UatSpec {
    unsigned type = 1;       // payload type 0..31
    unsigned qualifier = 0;  // 0..7
    uint32_t address = 0xA66EF1;
    double lat = 0.0, lon = 0.0;
    bool zero_position = false;  // lat and lon raw fields both 0
    bool alt_geometric = false;
    std::optional<int> alt_ft;   // nullopt: raw 0
    unsigned nic = 0;
    unsigned ag = 0;             // 0 airborne, 1 supersonic, 2 ground, 3 reserved
    // airborne: signed knots, + north / + east; multiples of 4 when ag == 1
    std::optional<int> ns_kt, ew_kt;
    std::optional<int> vrate_fpm;  // multiples of 64
    bool vrate_geometric = false;
    // ground
    std::optional<int> ground_speed_kt;
    unsigned track_type = 0;
    double track_deg = 0.0;
    // mode status
    unsigned emitter = 0;        // 0..39
    std::string call = "        ";  // 8 chars: 0-9 A-Z space; '?' = reserved index 37
    bool csid = true;
    unsigned emergency = 0;
    unsigned nacp = 0;
    std::optional<int> aux_alt_ft;
    std::size_t length = 0;  // 0: 18 for type 0, else 34
};

namespace uat_detail {
inline void put(Bytes& b, unsigned first, unsigned count, uint32_t v)
{
    for (unsigned i = 0; i < count; ++i) {
        const unsigned p = first + i;
        const uint8_t bit = static_cast<uint8_t>((v >> (count - 1 - i)) & 1u);
        b[p / 8] = static_cast<uint8_t>((b[p / 8] & ~(0x80u >> (p % 8))) | (bit << (7 - p % 8)));
    }
}
inline uint32_t altRaw(const std::optional<int>& ft)
{
    return ft ? static_cast<uint32_t>((*ft + 1000) / 25 + 1) : 0u;
}
inline unsigned sym(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<unsigned>(c - '0');
    if (c >= 'A' && c <= 'Z')
        return static_cast<unsigned>(c - 'A') + 10u;
    if (c == '?')
        return 37u;
    return 36u;
}
}  // namespace uat_detail

inline Bytes encodeUat(const UatSpec& s)
{
    using namespace uat_detail;
    const std::size_t n = s.length ? s.length : (s.type == 0 ? 18 : 34);
    Bytes b(n < 34 ? 34 : n, 0);
    const double lsb = 360.0 / 16777216.0;
    put(b, 0, 5, s.type);
    put(b, 5, 3, s.qualifier);
    put(b, 8, 24, s.address);
    if (!s.zero_position) {
        const double lat = s.lat < 0 ? s.lat + 180.0 : s.lat;
        const double lon = s.lon < 0 ? s.lon + 360.0 : s.lon;
        put(b, 32, 23, static_cast<uint32_t>(std::llround(lat / lsb)) & 0x7FFFFFu);
        put(b, 55, 24, static_cast<uint32_t>(std::llround(lon / lsb)) & 0xFFFFFFu);
    }
    put(b, 79, 1, s.alt_geometric ? 1u : 0u);
    put(b, 80, 12, altRaw(s.alt_ft));
    put(b, 92, 4, s.nic);
    put(b, 96, 2, s.ag);
    if (s.ag == 0 || s.ag == 1) {
        const int div = s.ag == 1 ? 4 : 1;
        if (s.ns_kt && s.ew_kt) {
            put(b, 99, 1, *s.ns_kt < 0);
            put(b, 100, 10, static_cast<uint32_t>(std::abs(*s.ns_kt) / div + 1));
            put(b, 110, 1, *s.ew_kt < 0);
            put(b, 111, 10, static_cast<uint32_t>(std::abs(*s.ew_kt) / div + 1));
        }
        if (s.vrate_fpm) {
            put(b, 121, 1, !s.vrate_geometric);  // 1 = barometric (golden)
            put(b, 122, 1, *s.vrate_fpm < 0);
            put(b, 123, 9, static_cast<uint32_t>(std::abs(*s.vrate_fpm) / 64 + 1));
        }
    } else if (s.ag == 2) {
        put(b, 99, 11, s.ground_speed_kt ? static_cast<uint32_t>(*s.ground_speed_kt + 1) : 0u);
        put(b, 110, 2, s.track_type);
        put(b, 112, 9, static_cast<uint32_t>(std::llround(s.track_deg * 512.0 / 360.0)) & 0x1FFu);
    }
    if (s.type == 1 || s.type == 3) {
        std::string c = s.call;
        c.resize(8, ' ');
        unsigned v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = sym(c[static_cast<std::size_t>(i)]);
        put(b, 136, 16, s.emitter * 1600 + v[0] * 40 + v[1]);
        put(b, 152, 16, v[2] * 1600 + v[3] * 40 + v[4]);
        put(b, 168, 16, v[5] * 1600 + v[6] * 40 + v[7]);
        put(b, 184, 3, s.emergency);
        put(b, 200, 4, s.nacp);
        put(b, 214, 1, s.csid);
    }
    if (s.type == 1 || s.type == 2 || s.type == 5 || s.type == 6)
        put(b, 232, 12, altRaw(s.aux_alt_ft));
    b.resize(n);
    return b;
}

inline std::string toHex(const Bytes& b)
{
    static const char* const d = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) {
        s += d[x >> 4];
        s += d[x & 15];
    }
    return s;
}

}  // namespace skycued::testing
