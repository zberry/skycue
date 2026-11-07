#pragma once

// Test-only frame builders for the 1090 decoder tests: Mode S frames with a
// correct parity field, extended-squitter message (ME) bodies, and a CPR
// encoder. Layouts follow the same public descriptions as the decoder
// (ICAO Annex 10 Vol IV, The 1090 MHz Riddle).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "coding/modes_crc.hpp"
#include "decode/cpr.hpp"

namespace skycued::testing {

using Bytes = std::vector<uint8_t>;
using cpr_msg = skycued::decode::cpr::Msg;
using cpr_pos = skycued::decode::cpr::Position;

inline double posMod(double a, double b) { return a - b * std::floor(a / b); }

// 17-bit CPR fields of a position, even or odd, airborne or surface.
inline cpr_msg encodeCpr(const cpr_pos& p, bool odd, bool surface) {
    const double zone = surface ? 90.0 : 360.0;
    const double dlat = zone / (odd ? 59.0 : 60.0);
    const double yz = std::floor(131072.0 * (posMod(p.lat, dlat) / dlat) + 0.5);
    const double rlat = dlat * (std::floor(p.lat / dlat) + yz / 131072.0);
    const int nlv = skycued::decode::cpr::nl(rlat);
    const int ni = std::max(nlv - (odd ? 1 : 0), 1);
    const double dlon = zone / ni;
    const double xz = std::floor(131072.0 * (posMod(p.lon, dlon) / dlon) + 0.5);
    return cpr_msg{static_cast<uint32_t>(yz) & 0x1FFFFu, static_cast<uint32_t>(xz) & 0x1FFFFu, odd};
}

// The 56-bit ME field; bit index 0 is its first (most significant) bit.
struct Me {
    uint64_t v = 0;
    Me& set(unsigned first, unsigned n, uint64_t val) {
        const unsigned shift = 56 - first - n;
        const uint64_t mask = (n >= 64 ? ~0ull : ((1ull << n) - 1)) << shift;
        v = (v & ~mask) | ((val << shift) & mask);
        return *this;
    }
};

// Parity: with the 24 parity bits zeroed, the residual is the CRC to insert;
// for address-parity frames the address is XORed in, for DF11 the interrogator id.
inline void putParity(Bytes& f, uint32_t xorWith) {
    const size_t n = f.size();
    f[n - 3] = f[n - 2] = f[n - 1] = 0;
    const uint32_t p = skycued::coding::modesCrcResidual(f) ^ xorWith;
    f[n - 3] = static_cast<uint8_t>(p >> 16);
    f[n - 2] = static_cast<uint8_t>(p >> 8);
    f[n - 1] = static_cast<uint8_t>(p);
}

inline void putAddr(Bytes& f, uint32_t addr) {
    f[1] = static_cast<uint8_t>(addr >> 16);
    f[2] = static_cast<uint8_t>(addr >> 8);
    f[3] = static_cast<uint8_t>(addr);
}

inline Bytes longFrame(int df, int low3, uint32_t addr, const Me& me) {
    Bytes f(14, 0);
    f[0] = static_cast<uint8_t>((df << 3) | (low3 & 7));
    putAddr(f, addr);
    for (int i = 0; i < 7; ++i) f[4 + i] = static_cast<uint8_t>(me.v >> (8 * (6 - i)));
    putParity(f, 0);
    return f;
}

inline Bytes df17(uint32_t addr, const Me& me, int ca = 5) { return longFrame(17, ca, addr, me); }
inline Bytes df18(int cf, uint32_t addr, const Me& me) { return longFrame(18, cf, addr, me); }

inline Bytes df11(uint32_t addr, int ca = 5, uint32_t iid = 0) {
    Bytes f(7, 0);
    f[0] = static_cast<uint8_t>((11 << 3) | (ca & 7));
    putAddr(f, addr);
    putParity(f, iid);
    return f;
}

// 13-bit altitude code with Q = 1 (25 ft steps); for AC12 drop the M bit.
inline uint32_t ac13Alt(int32_t ft) {
    const uint32_t n = static_cast<uint32_t>((ft + 1000) / 25);
    return ((n >> 5) << 7) | (((n >> 4) & 1u) << 5) | (1u << 4) | (n & 0xFu);
}
inline uint32_t ac12Alt(int32_t ft) {
    const uint32_t n = static_cast<uint32_t>((ft + 1000) / 25);
    return ((n >> 4) << 5) | (1u << 4) | (n & 0xFu);
}

// "1200" -> the 13-bit ID field (C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4).
inline uint32_t id13Squawk(const std::string& sq) {
    const unsigned a = static_cast<unsigned>(sq[0] - '0'), b = static_cast<unsigned>(sq[1] - '0'),
                   c = static_cast<unsigned>(sq[2] - '0'), d = static_cast<unsigned>(sq[3] - '0');
    auto bit = [](unsigned dig, unsigned w) { return (dig & w) ? 1u : 0u; };
    const unsigned order[13] = {bit(c, 1), bit(a, 1), bit(c, 2), bit(a, 2), bit(c, 4), bit(a, 4), 0,
                                bit(b, 1), bit(d, 1), bit(b, 2), bit(d, 2), bit(b, 4), bit(d, 4)};
    uint32_t v = 0;
    for (unsigned o : order) v = (v << 1) | o;
    return v;
}

inline Bytes shortAp(int df, int fs3, uint32_t field13, uint32_t addr, bool isLong) {
    Bytes f(isLong ? 14 : 7, 0);
    f[0] = static_cast<uint8_t>((df << 3) | (fs3 & 7));
    // Field at bits 19..31.
    f[2] = static_cast<uint8_t>(f[2] | ((field13 >> 8) & 0x1F));
    f[3] = static_cast<uint8_t>(field13 & 0xFF);
    putParity(f, addr);
    return f;
}
inline Bytes df4(uint32_t addr, uint32_t ac13, int fs = 0) { return shortAp(4, fs, ac13, addr, false); }
inline Bytes df5(uint32_t addr, uint32_t id13, int fs = 0) { return shortAp(5, fs, id13, addr, false); }

// ---- ME bodies ----

inline Me airbornePosition(int tc, const cpr_msg& m, int32_t alt_ft, bool bit8 = false) {
    Me me;
    me.set(0, 5, static_cast<uint64_t>(tc)).set(7, 1, bit8 ? 1 : 0).set(8, 12, ac12Alt(alt_ft));
    me.set(21, 1, m.odd ? 1 : 0).set(22, 17, m.lat17).set(39, 17, m.lon17);
    return me;
}

// mov: 7-bit movement code; trk: 7-bit track when valid.
inline Me surfacePosition(int tc, const cpr_msg& m, unsigned mov = 0, int trk = -1) {
    Me me;
    me.set(0, 5, static_cast<uint64_t>(tc)).set(5, 7, mov);
    if (trk >= 0) me.set(12, 1, 1).set(13, 7, static_cast<uint64_t>(trk));
    me.set(21, 1, m.odd ? 1 : 0).set(22, 17, m.lat17).set(39, 17, m.lon17);
    return me;
}

inline Me identification(int tc, int cat, const std::string& callsign) {
    Me me;
    me.set(0, 5, static_cast<uint64_t>(tc)).set(5, 3, static_cast<uint64_t>(cat));
    for (unsigned i = 0; i < 8; ++i) {
        const char c = i < callsign.size() ? callsign[i] : ' ';
        uint64_t code = 32;
        if (c >= 'A' && c <= 'Z') code = static_cast<uint64_t>(c - 'A' + 1);
        else if (c >= '0' && c <= '9') code = static_cast<uint64_t>(c - '0' + 48);
        me.set(8 + 6 * i, 6, code);
    }
    return me;
}

// Ground-speed velocity (subtype 1) from east/north components in knots.
// rate_fpm: signed multiple of 64; rate_baro selects the source bit; diff_ft:
// GNSS minus baro in 25 ft steps (0: unknown).
inline Me velocity(int east_kt, int north_kt, int rate_fpm = 0, bool rate_baro = false, int diff_ft = 0) {
    Me me;
    me.set(0, 5, 19).set(5, 3, 1);
    me.set(13, 1, east_kt < 0 ? 1 : 0).set(14, 10, static_cast<uint64_t>(std::abs(east_kt) + 1));
    me.set(24, 1, north_kt < 0 ? 1 : 0).set(25, 10, static_cast<uint64_t>(std::abs(north_kt) + 1));
    me.set(35, 1, rate_baro ? 1 : 0);
    if (rate_fpm != 0) {
        me.set(36, 1, rate_fpm < 0 ? 1 : 0).set(37, 9, static_cast<uint64_t>(std::abs(rate_fpm) / 64 + 1));
    }
    if (diff_ft != 0) {
        me.set(48, 1, diff_ft < 0 ? 1 : 0).set(49, 7, static_cast<uint64_t>(std::abs(diff_ft) / 25 + 1));
    }
    return me;
}

inline Me emergencyStatus(int emergency, const std::string& squawk) {
    Me me;
    me.set(0, 5, 28).set(5, 3, 1).set(8, 3, static_cast<uint64_t>(emergency)).set(11, 13, id13Squawk(squawk));
    return me;
}

// Operational status (TC31): subtype 0 airborne / 1 surface.
inline Me operationalStatus(int version, bool nic_a, int nac_p, bool surface = false, bool nic_c = false) {
    Me me;
    me.set(0, 5, 31).set(5, 3, surface ? 1 : 0);
    if (surface) me.set(19, 1, nic_c ? 1 : 0);
    me.set(40, 3, static_cast<uint64_t>(version)).set(43, 1, nic_a ? 1 : 0).set(44, 4, static_cast<uint64_t>(nac_p));
    return me;
}

inline Me typeOnly(int tc) {
    Me me;
    me.set(0, 5, static_cast<uint64_t>(tc));
    return me;
}

}  // namespace skycued::testing
