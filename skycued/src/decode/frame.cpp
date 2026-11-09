#include "decode/frame.hpp"

// Sources (public): ICAO Annex 10 Vol IV summaries as laid out in The 1090 MHz
// Riddle (Junzi Sun): the ADS-B chapters "Aircraft identification", "Airborne
// position", "Surface position", "Airborne velocity", "Aircraft operation
// status" and "Uncertainties in ADS-B" (NIC tables), and the Mode S chapter on
// downlink formats. Bit numbers below are 0-based frame bits; the ME field
// starts at bit 32.

#include <cmath>
#include <stdexcept>

namespace skycued::decode {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isEs(const Frame& f) { return (f.df == 17 || f.df == 18) && f.bytes.size() == 14; }

// AIS 6-bit character set (Annex 10 Vol IV, aircraft identification).
// '#' marks values that are not allowed; index 32 is a space.
const char kAis[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos)
        return "";
    const auto e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}

std::optional<uint8_t> nicAirborne(int tc, int version, bool a, bool b)
{
    const bool sup = version >= 2 ? (a && b) : (version == 1 ? a : false);
    switch (tc) {
    case 9: return 11;
    case 10: return 10;
    case 11: return sup ? 9 : 8;
    case 12: return 7;
    case 13: return 6;
    case 14: return 5;
    case 15: return 4;
    case 16: return sup ? 3 : 2;
    case 17: return 1;
    case 18: return 0;
    case 20: return 11;
    case 21: return 10;
    case 22: return 0;
    default: return std::nullopt;
    }
}

std::optional<uint8_t> nicSurface(int tc, int version, bool a, bool c)
{
    switch (tc) {
    case 5: return 11;
    case 6: return 10;
    case 7: return (version >= 1 && a) ? 9 : 8;
    case 8:
        if (version < 2)
            return 0;
        if (a && c)
            return 7;
        return (a || c) ? 6 : 0;
    default: return std::nullopt;
    }
}

}  // namespace

uint32_t Frame::bits(unsigned first, unsigned count) const
{
    if (count > 32 || first + count > bytes.size() * 8)
        throw std::out_of_range("Frame::bits");
    uint32_t v = 0;
    for (unsigned i = first; i < first + count; ++i)
        v = (v << 1) | ((bytes[i / 8] >> (7 - i % 8)) & 1u);
    return v;
}

uint64_t Frame::field56(unsigned first) const
{
    return (static_cast<uint64_t>(bits(first, 28)) << 28) | bits(first + 28, 28);
}

std::optional<Frame> classify(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != 7 && bytes.size() != 14)
        return std::nullopt;
    const int df = bytes[0] >> 3;
    FrameKind kind;
    bool longFrame;
    switch (df) {
    case 0: kind = FrameKind::df0; longFrame = false; break;
    case 4: kind = FrameKind::df4; longFrame = false; break;
    case 5: kind = FrameKind::df5; longFrame = false; break;
    case 11: kind = FrameKind::df11; longFrame = false; break;
    case 16: kind = FrameKind::df16; longFrame = true; break;
    case 17: kind = FrameKind::df17; longFrame = true; break;
    case 18: kind = FrameKind::df18; longFrame = true; break;
    case 20: kind = FrameKind::df20; longFrame = true; break;
    case 21: kind = FrameKind::df21; longFrame = true; break;
    default: return std::nullopt;
    }
    if (longFrame != (bytes.size() == 14))
        return std::nullopt;
    Frame f;
    f.bytes = bytes;
    f.kind = kind;
    f.df = df;
    return f;
}

int typeCode(const Frame& f)
{
    return isEs(f) ? static_cast<int>(f.bits(32, 5)) : -1;
}

std::string decodeCallsignIdent(const Frame& f)
{
    if (!isEs(f))
        return "";
    std::string s;
    for (unsigned i = 0; i < 8; ++i) {
        const char c = kAis[f.bits(40 + 6 * i, 6)];
        if (c == '#')
            return "";
        s.push_back(c);
    }
    return trim(s);
}

std::optional<Identification> decodeIdentification(const Frame& f)
{
    const int tc = typeCode(f);
    if (tc < 1 || tc > 4)
        return std::nullopt;
    Identification id;
    id.callsign = decodeCallsignIdent(f);
    if (id.callsign.empty())
        return std::nullopt;
    // TC 4 is category set A, 3 is B, 2 is C, 1 is D.
    id.category = std::string(1, static_cast<char>('A' + (4 - tc))) + std::to_string(f.bits(37, 3));
    return id;
}

std::optional<Velocity> decodeVelocity(const Frame& f)
{
    if (typeCode(f) != 19)
        return std::nullopt;
    const uint32_t st = f.bits(37, 3);
    if (st < 1 || st > 4)
        return std::nullopt;

    Velocity v;
    if (st <= 2) {
        const double mult = st == 2 ? 4.0 : 1.0;
        const uint32_t vew = f.bits(46, 10), vns = f.bits(57, 10);
        if (vew != 0 && vns != 0) {
            const double vx = (f.bits(45, 1) ? -1.0 : 1.0) * mult * (static_cast<double>(vew) - 1.0);
            const double vy = (f.bits(56, 1) ? -1.0 : 1.0) * mult * (static_cast<double>(vns) - 1.0);
            v.gs_kt = static_cast<float>(std::hypot(vx, vy));
            double trk = std::atan2(vx, vy) * 180.0 / kPi;
            if (trk < 0.0)
                trk += 360.0;
            v.track_deg = static_cast<float>(trk);
        }
    }
    // ST 3/4 carry heading and airspeed, not a ground vector. ME bits (1-based)
    // 14 heading status, 15-24 magnetic heading (360/1024 deg), 25 airspeed type
    // (0 IAS, 1 TAS), 26-35 airspeed ((value - 1) x 1 kt ST3, x 4 kt ST4; 0 = none).
    if (st >= 3) {
        if (f.bits(45, 1))
            v.heading_mag_deg = static_cast<float>(f.bits(46, 10) * 360.0 / 1024.0);
        const uint32_t as = f.bits(57, 10);
        if (as != 0) {
            const uint32_t kt = (as - 1) * (st == 4 ? 4u : 1u);
            if (f.bits(56, 1))
                v.tas_kt = static_cast<uint16_t>(kt);
            else
                v.ias_kt = static_cast<uint16_t>(kt);
        }
    }

    // Vertical rate source bit: 0 GNSS (geometric), 1 barometric (Doc 9871
    // Table A-2-9; The 1090 MHz Riddle, airborne velocity chapter).
    v.rate_is_geom = f.bits(67, 1) == 0;
    const uint32_t vr = f.bits(69, 9);
    if (vr != 0) {
        const int32_t mag = (static_cast<int32_t>(vr) - 1) * 64;
        v.rate_fpm = f.bits(68, 1) ? -mag : mag;
    }
    const uint32_t dif = f.bits(81, 7);
    if (dif != 0) {
        const int32_t mag = (static_cast<int32_t>(dif) - 1) * 25;
        v.gnss_baro_diff_ft = f.bits(80, 1) ? -mag : mag;
    }
    return v;
}

std::optional<TargetState> decodeTargetState(const Frame& f)
{
    if (typeCode(f) != 29 || f.bits(37, 2) != 1)
        return std::nullopt;
    // ME bits (1-based): 9 altitude type, 10-20 altitude ((v-1) x 32 ft), 21-29
    // barometric setting ((v-1) x 0.8 + 800 mb), 30 heading status, 31 sign,
    // 32-39 heading (180/256 deg, two's complement over bits 31-39), 40-43 NACp,
    // 47 mode status, 48 autopilot, 49 VNAV, 50 altitude hold, 52 approach,
    // 53 TCAS operational, 54 LNAV.
    const uint64_t me = f.field56(32);
    TargetState t;
    const uint32_t alt = field56(me, 10, 11);
    if (alt != 0) {
        const int32_t ft = static_cast<int32_t>(alt - 1) * 32;
        if (field56(me, 9, 1))
            t.sel_alt_fms_ft = ft;
        else
            t.sel_alt_ft = ft;
    }
    const uint32_t qnh = field56(me, 21, 9);
    if (qnh != 0)
        t.qnh_mb = static_cast<float>(800.0 + 0.8 * static_cast<double>(qnh - 1));
    if (field56(me, 30, 1))
        t.sel_heading_deg = static_cast<float>(field56(me, 31, 9) * 180.0 / 256.0);
    t.nac_p = static_cast<uint8_t>(field56(me, 40, 4));
    t.modes_valid = field56(me, 47, 1) != 0;
    t.autopilot = field56(me, 48, 1) != 0;
    t.vnav = field56(me, 49, 1) != 0;
    t.althold = field56(me, 50, 1) != 0;
    t.approach = field56(me, 52, 1) != 0;
    t.tcas = field56(me, 53, 1) != 0;
    t.lnav = field56(me, 54, 1) != 0;
    return t;
}

SurfaceMovement decodeSurfaceMovement(const Frame& f)
{
    SurfaceMovement m;
    const int tc = typeCode(f);
    if (tc < 5 || tc > 8)
        return m;

    // Movement quantisation, "Surface position" chapter.
    const uint32_t mov = f.bits(37, 7);
    if (mov == 1)
        m.gs_kt = 0.0f;
    else if (mov >= 2 && mov <= 8)
        m.gs_kt = 0.125f * static_cast<float>(mov - 1);
    else if (mov >= 9 && mov <= 12)
        m.gs_kt = 1.0f + 0.25f * static_cast<float>(mov - 9);
    else if (mov >= 13 && mov <= 38)
        m.gs_kt = 2.0f + 0.5f * static_cast<float>(mov - 13);
    else if (mov >= 39 && mov <= 93)
        m.gs_kt = 15.0f + static_cast<float>(mov - 39);
    else if (mov >= 94 && mov <= 108)
        m.gs_kt = 70.0f + 2.0f * static_cast<float>(mov - 94);
    else if (mov >= 109 && mov <= 123)
        m.gs_kt = 100.0f + 5.0f * static_cast<float>(mov - 109);
    else if (mov == 124)
        m.gs_kt = 175.0f;

    if (f.bits(44, 1))
        m.track_deg = static_cast<float>(f.bits(45, 7)) * 360.0f / 128.0f;
    return m;
}

std::optional<CprFields> decodeCpr(const Frame& f)
{
    const int tc = typeCode(f);
    const bool surface = tc >= 5 && tc <= 8;
    if (!surface && !(tc >= 9 && tc <= 18) && !(tc >= 20 && tc <= 22))
        return std::nullopt;
    CprFields c{};
    c.surface = surface;
    c.odd = f.bits(53, 1) != 0;
    c.lat17 = f.bits(54, 17);
    c.lon17 = f.bits(71, 17);
    return c;
}

std::optional<std::string> decodeEmergency(const Frame& f)
{
    if (typeCode(f) != 28 || f.bits(37, 3) != 1)
        return std::nullopt;
    static const char* const names[] = {"none", "general", "medical", "minimum_fuel",
                                        "no_comms", "unlawful_interference", "downed_aircraft"};
    const uint32_t e = f.bits(40, 3);
    if (e > 6)
        return std::nullopt;
    return std::string(names[e]);
}

std::optional<std::string> decodeSquawkTc28(const Frame& f)
{
    if (typeCode(f) != 28 || f.bits(37, 3) != 1)
        return std::nullopt;
    return squawkId13(f.bits(43, 13));
}

std::optional<OperationalStatus> decodeOperationalStatus(const Frame& f)
{
    if (typeCode(f) != 31)
        return std::nullopt;
    const uint32_t st = f.bits(37, 3);
    if (st > 1)
        return std::nullopt;
    OperationalStatus o{};
    o.surface = st == 1;
    o.version = static_cast<int>(f.bits(72, 3));
    o.nic_a = f.bits(75, 1) != 0;
    o.nic_c = o.surface && f.bits(51, 1) != 0;
    o.nac_p = static_cast<uint8_t>(f.bits(76, 4));
    return o;
}

std::optional<uint8_t> nicFor(int tc, int version, bool nic_a, bool nic_b_or_c, bool surface)
{
    return surface ? nicSurface(tc, version, nic_a, nic_b_or_c)
                   : nicAirborne(tc, version, nic_a, nic_b_or_c);
}

int flightStatus(const Frame& f)
{
    if (f.df == 4 || f.df == 5 || f.df == 20 || f.df == 21)
        return static_cast<int>(f.bits(5, 3));
    return -1;
}

bool verticalStatusGround(const Frame& f)
{
    return (f.df == 0 || f.df == 16) && f.bits(5, 1) != 0;
}

int capability(const Frame& f)
{
    if (f.df == 11 || f.df == 17 || f.df == 18)
        return static_cast<int>(f.bits(5, 3));
    return -1;
}

}  // namespace skycued::decode
