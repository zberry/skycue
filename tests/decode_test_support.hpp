#pragma once

// Test-only frame builders for the 1090 decoder tests: Mode S frames with a
// correct parity field, extended-squitter message (ME) bodies, and a CPR
// encoder. Layouts follow the same public descriptions as the decoder
// (ICAO Annex 10 Vol IV, The 1090 MHz Riddle).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coding/modes_crc.hpp"
#include "decode/acas.hpp"
#include "decode/cpr.hpp"
#include "decode/decoder_types.hpp"
#include "decode/frame.hpp"
#include "observation.hpp"

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

// The 56-bit MB (Comm-B) and MV (DF16) fields have the same shape as the ME
// field, so Me is reused for them: bit 0 is the field's first (most
// significant) bit, i.e. amendment bit N is index N - 1. Mb adds 1-based
// setters that take the amendment's bit numbers directly, and a two's
// complement setter for the signed register fields.
struct Mb : Me {
    Mb& at(unsigned first1, unsigned n, uint64_t val) { set(first1 - 1, n, val); return *this; }
    // n-bit two's complement value (the sign bit is the first bit).
    Mb& sat(unsigned first1, unsigned n, int64_t val) {
        return at(first1, n, static_cast<uint64_t>(val) & ((1ull << n) - 1));
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

// Long frames carrying a 56-bit field at frame bits 32..87. field13 sits at
// frame bits 19..31 as in shortAp; the parity is XORed with the address.
inline Bytes longAp(int df, int low3, uint32_t field13, uint32_t addr, uint64_t field56) {
    Bytes f = shortAp(df, low3, field13, addr, true);
    for (int i = 0; i < 7; ++i) f[4 + i] = static_cast<uint8_t>(field56 >> (8 * (6 - i)));
    putParity(f, addr);
    return f;
}
inline Bytes df20(uint32_t addr, uint32_t ac13, uint64_t mb, int fs = 0) { return longAp(20, fs, ac13, addr, mb); }
inline Bytes df21(uint32_t addr, uint32_t id13, uint64_t mb, int fs = 0) { return longAp(21, fs, id13, addr, mb); }
// DF16: VS at bit 5, SL at bits 8..10, RI at bits 13..16, AC13 at 19..31, MV after.
inline Bytes df16(uint32_t addr, uint32_t ac13, uint64_t mv, bool vs_ground = false, uint8_t sl = 2, uint8_t ri = 3) {
    Bytes f = longAp(16, vs_ground ? 4 : 0, ac13, addr, mv);
    f[1] = static_cast<uint8_t>(((sl & 7) << 5) | ((ri & 0xF) >> 1));
    f[2] = static_cast<uint8_t>((f[2] & 0x1F) | ((ri & 1) << 7));
    putParity(f, addr);
    return f;
}

// ---- ME bodies ----

inline Me airbornePosition(int tc, const cpr_msg& m, int32_t alt_ft, bool bit8 = false) {
    Me me;
    // TC 20-22 carry GNSS height in whole metres (no Q bit); TC 9-18 the barometric AC12.
    const uint64_t field = (tc >= 20 && tc <= 22) ? static_cast<uint64_t>(std::lround(alt_ft / 3.28084))
                                                  : ac12Alt(alt_ft);
    me.set(0, 5, static_cast<uint64_t>(tc)).set(7, 1, bit8 ? 1 : 0).set(8, 12, field);
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

// TC19 subtype 3 (1 kt) or 4 (4 kt) heading and airspeed; rate_fpm as in velocity().
inline Me velocityAirspeed(int subtype, float heading_deg, uint16_t speed_kt, bool tas, int rate_fpm = 0) {
    Me me;
    me.set(0, 5, 19).set(5, 3, static_cast<uint64_t>(subtype));
    me.set(13, 1, 1).set(14, 10, static_cast<uint64_t>(std::lround(heading_deg * 1024.0 / 360.0)) & 0x3FFu);
    me.set(24, 1, tas ? 1 : 0).set(25, 10, static_cast<uint64_t>(speed_kt / (subtype == 4 ? 4 : 1) + 1));
    me.set(35, 1, 1);
    if (rate_fpm != 0) {
        me.set(36, 1, rate_fpm < 0 ? 1 : 0).set(37, 9, static_cast<uint64_t>(std::abs(rate_fpm) / 64 + 1));
    }
    return me;
}

// TC29 subtype 1 from a TargetState (sel_alt_fms_ft selects the FMS altitude type).
inline Me targetState(const skycued::decode::TargetState& t) {
    Me me;
    me.set(0, 5, 29).set(5, 2, 1);
    if (t.sel_alt_ft || t.sel_alt_fms_ft) {
        me.set(8, 1, t.sel_alt_fms_ft ? 1 : 0);
        me.set(9, 11, static_cast<uint64_t>((t.sel_alt_fms_ft ? *t.sel_alt_fms_ft : *t.sel_alt_ft) / 32 + 1));
    }
    if (t.qnh_mb) me.set(20, 9, static_cast<uint64_t>(std::lround((*t.qnh_mb - 800.0) / 0.8)) + 1);
    if (t.sel_heading_deg) {
        me.set(29, 1, 1).set(30, 9, static_cast<uint64_t>(std::lround(*t.sel_heading_deg * 256.0 / 180.0)) & 0x1FFu);
    }
    me.set(39, 4, t.nac_p.value_or(0));
    me.set(46, 1, t.modes_valid ? 1 : 0).set(47, 1, t.autopilot).set(48, 1, t.vnav).set(49, 1, t.althold);
    me.set(51, 1, t.approach).set(52, 1, t.tcas).set(53, 1, t.lnav);
    return me;
}

// The 48-bit ACAS RA body shared by the DF16 MV field and TC28 subtype 2 (bits
// 8..55 of the 56-bit field; 0-based): ARA, RAC, RAT, MTE, TTI, TID. TID for
// TTI 1: the address in the low 24 bits; TTI 2: AC13 altitude, range, bearing.
inline Me acasBody(const skycued::decode::AcasRa& ra) {
    Me me;
    me.set(8, 14, ra.ara).set(22, 4, ra.rac).set(26, 1, ra.rat).set(27, 1, ra.mte).set(28, 2, ra.tti);
    if (ra.tti == 1 && ra.threat_address) me.set(32, 24, *ra.threat_address);
    if (ra.tti == 2) {
        if (ra.threat_alt_ft) me.set(30, 13, ac13Alt(*ra.threat_alt_ft));
        if (ra.threat_range_nm) me.set(43, 7, static_cast<uint64_t>(std::lround(*ra.threat_range_nm * 10.0f)) + 1);
        if (ra.threat_bearing_deg) me.set(50, 6, static_cast<uint64_t>(std::lround(*ra.threat_bearing_deg / 6.0f)) + 1);
    }
    return me;
}
inline Me acasRaTc28(const skycued::decode::AcasRa& ra) {
    Me me = acasBody(ra);
    me.set(0, 5, 28).set(5, 3, 2);
    return me;
}
inline Me acasRaMv(const skycued::decode::AcasRa& ra) {
    Me me = acasBody(ra);
    me.set(0, 8, 0x30);
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

// A decoder with its observations collected into obs; a test derives from it and adds its own feed().
template <class Dec>
struct DecoderRig {
    explicit DecoderRig(skycued::decode::DecoderConfig cfg = {}, std::string input_id = "in1")
        : dec(cfg, std::move(input_id))
    {
        dec.onObservation([this](const skycued::TrackObservation& o) { obs.push_back(o); });
    }
    Dec dec;
    std::vector<skycued::TrackObservation> obs;
};

}  // namespace skycued::testing
