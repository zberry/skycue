#include "decode/uat_frame.hpp"

#include <skycue/types.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace skycued::decode::uat {

namespace {

// Big-endian bit field over the frame; callers guarantee first + count <= size*8.
uint32_t bitsAt(const std::vector<uint8_t>& b, unsigned first, unsigned count)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned p = first + i;
        v = (v << 1) | ((b[p / 8] >> (7 - p % 8)) & 1u);
    }
    return v;
}

// Layout constants: public UAT ADS-B description (RTCA DO-282B / FAA UAT ADS-B
// message summary) unless the comment says otherwise; the golden comparison is tests/test_decode_uat_golden.cpp.
constexpr double kAngleLsb = 360.0 / 16777216.0;  // lat/lon LSB, 360/2^24 (DO-282B SV)
constexpr unsigned kLatBit = 32, kLatLen = 23;    // DO-282B SV
constexpr unsigned kLonBit = 55, kLonLen = 24;    // DO-282B SV
constexpr unsigned kAltTypeBit = 79;              // DO-282B SV
constexpr unsigned kAltBit = 80, kAltLen = 12;    // DO-282B SV
constexpr unsigned kNicBit = 92, kNicLen = 4;     // DO-282B SV
constexpr unsigned kAgBit = 96;                   // DO-282B SV, 2 bits
// Airborne velocity block, bits 99-131; bit 98 is not part of it (confirmed
// against the dump978-fa golden: velocity and vertical rate agree on all 439 frames).
constexpr unsigned kNsSignBit = 99;
constexpr unsigned kNsBit = 100, kEwSignBit = 110, kEwBit = 111, kVelLen = 10;
// Vertical-rate source bit: 1 = barometric, 0 = geometric (confirmed against the golden).
constexpr unsigned kVrSrcBit = 121, kVrSignBit = 122, kVrBit = 123, kVrLen = 9;
// Surface block, in the same bits 99-120 as the airborne velocity. Confirmed
// against the Stratux golden (551 surface frames: speed and track agree on all).
constexpr unsigned kGndSpeedBit = 99, kGndSpeedLen = 11;
// Track/heading type: 0 n/a, 1 true track, 2 magnetic heading, 3 true heading;
// only 1 is a track (a deliberate ruling); 2 and 3 are the magnetic and true heading fields. The Stratux golden has types 1 (426 frames, track
// agrees) and 0 (125, no track); types 2 and 3 are confirmed by the synthetic golden
// (dump978-fa prints magnetic_heading / true_heading from the same 9 bits, x 360/512,
// and no track; ours gives no track_deg for them, heading_mag_deg / heading_true_deg instead).
constexpr uint32_t kGndTypeTrack = 1;
constexpr unsigned kGndTypeBit = 110, kGndTrackBit = 112, kGndTrackLen = 9;
constexpr unsigned kMsBit = 136;                  // DO-282B MS: byte 17
constexpr unsigned kEmergencyBit = 184;           // confirmed by the synthetic golden (codes 0..2, 4..6 by name; 3 and 7 differ, see decodeModeStatus)
constexpr unsigned kNacpBit = 200;                // confirmed against the dump978-fa golden
constexpr unsigned kCsidBit = 214;                // set for call signs, clear for squawks (golden); was 213
constexpr unsigned kAuxBit = 232;                 // DO-282B AUX SV: byte 29

int hexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::optional<int32_t> altitudeFt(uint32_t raw)
{
    if (raw == 0)
        return std::nullopt;
    return (static_cast<int32_t>(raw) - 1) * 25 - 1000;  // DO-282B altitude encoding
}

// Base-40 alphabet: 0-9, A-Z, space, three reserved (DO-282B call sign).
char base40(unsigned v, bool& ok)
{
    if (v < 10)
        return static_cast<char>('0' + v);
    if (v < 36)
        return static_cast<char>('A' + (v - 10));
    if (v == 36)
        return ' ';
    ok = false;
    return ' ';
}

}  // namespace

std::optional<QualifierMap> mapQualifier(AddressQualifier q)
{
    using skycue::AddressType;
    using skycue::Source;
    switch (q) {
    case AddressQualifier::adsb_icao: return QualifierMap{AddressType::icao, Source::uat_978, false};
    case AddressQualifier::adsb_selfassigned: return QualifierMap{AddressType::non_icao, Source::uat_978, false};
    case AddressQualifier::tisb_adsr_icao: return QualifierMap{AddressType::icao, Source::tisb, true};
    case AddressQualifier::tisb_trackfile: return QualifierMap{AddressType::tisb_trackfile, Source::tisb, true};
    case AddressQualifier::surface_vehicle:
    case AddressQualifier::fixed_beacon: return QualifierMap{AddressType::non_icao, Source::uat_978, false};
    case AddressQualifier::adsr_nonicao: return QualifierMap{AddressType::non_icao, Source::tisb, true};
    case AddressQualifier::reserved: break;
    }
    return std::nullopt;
}

uint8_t UatFrame::payload_type() const { return bytes.empty() ? 0 : static_cast<uint8_t>(bytes[0] >> 3); }
AddressQualifier UatFrame::qualifier() const
{
    return bytes.empty() ? AddressQualifier::reserved : static_cast<AddressQualifier>(bytes[0] & 7u);
}
uint32_t UatFrame::address() const
{
    if (bytes.size() < 4)
        return 0;
    return (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

bool UatFrame::lengthMatchesType() const
{
    const unsigned t = payload_type();
    if (bytes.size() == 18)
        return t == 0;
    return bytes.size() == 34 && t >= 1 && t <= 10;
}

bool UatFrame::hasMs() const
{
    const unsigned t = payload_type();
    return lengthMatchesType() && (t == 1 || t == 3);
}

bool UatFrame::hasAux() const
{
    const unsigned t = payload_type();
    return lengthMatchesType() && (t == 1 || t == 2 || t == 5 || t == 6);
}

std::optional<UatFrame> parseDownlinkLine(std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.remove_suffix(1);
    if (!line.empty() && line.front() == '-')
        line.remove_prefix(1);
    const std::size_t semi = line.find(';');
    const std::string_view hex = line.substr(0, semi);
    if (hex.size() % 2 != 0 || (hex.size() != 36 && hex.size() != 68))
        return std::nullopt;
    UatFrame f;
    f.bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        f.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    std::string_view rest = semi == std::string_view::npos ? std::string_view{} : line.substr(semi + 1);
    while (!rest.empty()) {
        const std::size_t end = rest.find(';');
        const std::string_view kv = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const std::size_t eq = kv.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string key(kv.substr(0, eq)), val(kv.substr(eq + 1));
        if ((key != "rssi" && key != "rs") || val.empty())
            continue;
        char* e = nullptr;
        if (key == "rssi") {
            const double d = std::strtod(val.c_str(), &e);
            if (e == val.c_str() + val.size() && std::isfinite(d))
                f.rssi_dbfs = d;
        } else {
            errno = 0;
            const long n = std::strtol(val.c_str(), &e, 10);
            if (e == val.c_str() + val.size() && errno == 0 && n >= 0 && n <= 1000)
                f.rs_errors = static_cast<int>(n);
        }
    }
    return f;
}

StateVector decodeStateVector(const UatFrame& f)
{
    const auto& b = f.bytes;
    StateVector sv;
    if (b.size() < 18)  // a UatFrame from parseDownlinkLine always has 18 or 34 bytes
        return sv;
    const uint32_t latRaw = bitsAt(b, kLatBit, kLatLen), lonRaw = bitsAt(b, kLonBit, kLonLen);
    sv.nic = static_cast<uint8_t>(bitsAt(b, kNicBit, kNicLen));
    if (!(latRaw == 0 && lonRaw == 0 && sv.nic == 0)) {
        double lat = latRaw * kAngleLsb;
        if (lat > 90.0)
            lat -= 180.0;  // 90-degree fold of the 23-bit field
        double lon = lonRaw * kAngleLsb;
        if (lon >= 180.0)
            lon -= 360.0;
        sv.lat = lat;
        sv.lon = lon;
    }
    sv.alt_type = bitsAt(b, kAltTypeBit, 1) ? AltType::geometric : AltType::baro;
    sv.altitude_ft = altitudeFt(bitsAt(b, kAltBit, kAltLen));
    sv.ag = static_cast<StateVector::Ag>(bitsAt(b, kAgBit, 2));

    if (sv.ag == StateVector::Ag::airborne_subsonic || sv.ag == StateVector::Ag::airborne_supersonic) {
        const uint32_t ns = bitsAt(b, kNsBit, kVelLen), ew = bitsAt(b, kEwBit, kVelLen);
        if (ns != 0 && ew != 0) {  // 0 = no velocity information
            const double k = sv.ag == StateVector::Ag::airborne_supersonic ? 4.0 : 1.0;
            const double vn = (static_cast<double>(ns) - 1.0) * k * (bitsAt(b, kNsSignBit, 1) ? -1.0 : 1.0);
            const double ve = (static_cast<double>(ew) - 1.0) * k * (bitsAt(b, kEwSignBit, 1) ? -1.0 : 1.0);
            const double gs = std::hypot(vn, ve);
            sv.gs_kt = static_cast<float>(gs);
            if (gs > 0.0) {
                double trk = std::atan2(ve, vn) * 180.0 / 3.14159265358979323846;
                if (trk < 0.0)
                    trk += 360.0;
                sv.track_deg = static_cast<float>(trk);
            }
        }
        const uint32_t vr = bitsAt(b, kVrBit, kVrLen);
        if (vr != 0) {
            const int32_t mag = (static_cast<int32_t>(vr) - 1) * 64;
            sv.vrate_fpm = bitsAt(b, kVrSignBit, 1) ? -mag : mag;
            sv.vrate_geometric = bitsAt(b, kVrSrcBit, 1) == 0;
        }
    } else if (sv.ag == StateVector::Ag::ground) {
        const uint32_t sp = bitsAt(b, kGndSpeedBit, kGndSpeedLen);
        if (sp != 0)
            sv.gs_kt = static_cast<float>(sp - 1);
        const uint32_t type = bitsAt(b, kGndTypeBit, 2);
        const float deg = static_cast<float>(bitsAt(b, kGndTrackBit, kGndTrackLen) * 360.0 / 512.0);
        if (type == kGndTypeTrack) sv.track_deg = deg;
        else if (type == 2) sv.heading_mag_deg = deg;
        else if (type == 3) sv.heading_true_deg = deg;
    }
    return sv;
}

std::optional<ModeStatus> decodeModeStatus(const UatFrame& f)
{
    if (!f.hasMs())
        return std::nullopt;
    const auto& b = f.bytes;
    ModeStatus ms;
    const uint32_t g1 = bitsAt(b, kMsBit, 16), g2 = bitsAt(b, kMsBit + 16, 16), g3 = bitsAt(b, kMsBit + 32, 16);
    const unsigned cat = g1 / 1600;
    // Sets A-D, 0 = "A0" (no information, as the golden prints it and as the 1090 decoder does).
    // 32..39: dump978-fa prints E0..E7 for them (synthetic golden), so the letter E; the public
    // spec text this layout follows names no set E, so that is the tool's reading, taken as is.
    // GDL-90 and the public numbering treat 22..39 as reserved; "E0".."E7" follows dump978-fa's
    // convention only.
    if (cat <= 39) {
        ms.emitter_category = std::string(1, "ABCDE"[cat / 8]);
        ms.emitter_category += static_cast<char>('0' + cat % 8);
    }
    bool ok = g1 < 64000 && g2 < 64000 && g3 < 64000;
    if (ok) {
        const unsigned idx[8] = {(g1 % 1600) / 40, g1 % 40, g2 / 1600, (g2 % 1600) / 40,
                                 g2 % 40,          g3 / 1600, (g3 % 1600) / 40, g3 % 40};
        if (bitsAt(b, kCsidBit, 1)) {
            std::string cs;
            for (unsigned v : idx)
                cs += base40(v, ok);
            while (ok && !cs.empty() && cs.back() == ' ')
                cs.pop_back();
            if (ok && !cs.empty())
                ms.callsign = cs;
        } else {
            // A squawk is four octal digits; the other four symbols are padding, which is
            // symbol 37 in the golden. A space (36) is accepted too; 38 and 39 are reserved
            // and never seen, so a field holding them is not read as a squawk.
            std::string sq;
            for (unsigned i = 0; i < 4; ++i)
                sq += idx[i] < 8 ? static_cast<char>('0' + idx[i]) : '?';
            bool pad = true;
            for (unsigned i = 4; i < 8; ++i)
                pad = pad && (idx[i] == 36 || idx[i] == 37);
            if (pad && sq.find('?') == std::string::npos)
                ms.squawk = sq;
        }
    }
    // Codes 0..2 and 4..6 agree with dump978-fa (which names 4..6 nordo, unlawful, downed). Code 3
    // is minimum fuel in the public spec text; dump978-fa prints "invalid" for it, ours keeps the
    // spec name. Code 7 is reserved: dump978-fa prints "reserved", ours has no value.
    const uint32_t em = bitsAt(b, kEmergencyBit, 3);
    if (em <= 6)
        ms.emergency = skycue::emergencyName(static_cast<uint8_t>(em));
    ms.nac_p = static_cast<uint8_t>(bitsAt(b, kNacpBit, 4));
    return ms;
}

std::optional<int32_t> decodeAuxSv(const UatFrame& f)
{
    if (!f.hasAux())
        return std::nullopt;
    return altitudeFt(bitsAt(f.bytes, kAuxBit, 12));
}

}  // namespace skycued::decode::uat
