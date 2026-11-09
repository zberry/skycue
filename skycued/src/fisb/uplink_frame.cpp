#include "fisb/uplink_frame.hpp"

#include "fisb/bit_reader.hpp"
#include "fisb/products.hpp"
#include "hex.hpp"

#include <algorithm>
#include <cctype>

namespace skycued {
namespace fisb {

std::optional<UplinkHeader> parseUplinkHeader(const uint8_t* p, std::size_t n)
{
    if (!p || n < kUplinkHeaderBytes)
        return std::nullopt;
    BitReader r(p, kUplinkHeaderBytes);
    const auto raw_lat = r.u(23);
    const auto raw_lon = r.u(24);
    const auto pos_valid = r.u(1);
    const auto utc = r.u(1);
    r.u(1);  // reserved
    const auto app_valid = r.u(1);
    const auto slot = r.u8(5);
    const auto site = r.u8(4);
    if (r.overrun())
        return std::nullopt;

    // 360 / 2^24 degrees per unit; latitude above 90 and longitude above 180 wrap negative.
    constexpr double kScale = 360.0 / 16777216.0;
    UplinkHeader h;
    h.lat_deg = *raw_lat * kScale;
    if (h.lat_deg > 90.0)
        h.lat_deg -= 180.0;
    h.lon_deg = *raw_lon * kScale;
    if (h.lon_deg > 180.0)
        h.lon_deg -= 360.0;
    h.position_valid = *pos_valid != 0;
    h.utc_coupled = *utc != 0;
    h.app_data_valid = *app_valid != 0;
    h.slot_id = *slot;
    h.tisb_site_id = *site;
    return h;
}

bool forEachInfoFrame(const uint8_t* payload, std::size_t n, const std::function<void(const InfoFrame&)>& fn)
{
    if (!payload || n < kUplinkHeaderBytes)
        return false;
    const std::size_t end = std::min(n, kUplinkBytes);
    std::size_t p = kUplinkHeaderBytes;
    while (p + 2 <= end) {
        // 9-bit length (data bytes only), 3 reserved bits, 4-bit type.
        const std::size_t len = (std::size_t{payload[p]} << 1) | (payload[p + 1] >> 7);
        const auto type = static_cast<uint8_t>(payload[p + 1] & 0x0f);
        if (len == 0)
            return true;
        if (len > end - (p + 2))
            return false;
        fn(InfoFrame{type, payload + p + 2, len});
        p += 2 + len;
    }
    // A short trailing remnant (< 2 bytes) inside 432 is padding; a buffer cut
    // short of the full uplink with a frame still pending is an overrun.
    return end == kUplinkBytes || p == end;
}

namespace {

bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// [b, e) with surrounding whitespace and any "<digits> " epoch prefix removed.
void trimLine(const std::string& line, std::size_t& b, std::size_t& e)
{
    b = 0;
    e = line.size();
    while (b < e && isSpace(line[b]))
        ++b;
    while (e > b && isSpace(line[e - 1]))
        --e;
    std::size_t d = b;
    while (d < e && line[d] >= '0' && line[d] <= '9')
        ++d;
    if (d > b && d < e && isSpace(line[d])) {
        b = d;
        while (b < e && isSpace(line[b]))
            ++b;
    }
}

}  // namespace

bool hexToBytes(const std::string& line, std::vector<uint8_t>& out)
{
    out.clear();
    std::size_t b = 0, e = 0;
    trimLine(line, b, e);
    if (b < e && (line[b] == '+' || line[b] == '-'))
        ++b;
    const std::size_t semi = line.find(';', b);
    std::size_t hex_end = e;
    if (semi != std::string::npos && semi < e) {
        hex_end = semi;
        // Metadata: "key=value;" fields; reject an empty field such as ";;".
        std::size_t f = semi + 1;
        while (f < e) {
            const std::size_t next = std::min(line.find(';', f), e);
            if (next == f)
                return false;
            f = next + 1;
        }
    }
    const std::size_t len = hex_end - b;
    if (len == 0 || len % 2 != 0)
        return false;
    out.reserve(len / 2);
    for (std::size_t i = b; i < hex_end; i += 2) {
        const int hi = hexDigitValue(line[i]);
        const int lo = hexDigitValue(line[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool isUplinkLine(const std::string& line)
{
    std::size_t b = 0, e = 0;
    trimLine(line, b, e);
    return b < e && line[b] == '+';
}

}  // namespace fisb
}  // namespace skycued
