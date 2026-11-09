#include "decode/acas.hpp"

// Layout (1-based bits of the 56-bit MV/ME field; MV bits 41-88 = frame bits,
// ME bits 9-56): 9-22 ARA (14 bits), 23-26 RAC, 27 RAT, 28 MTE, 29-30 TTI,
// 31-56 TID. TTI 1: 24-bit aircraft address (the low 24 bits of the TID);
// TTI 2: 13-bit altitude code, 7-bit range, 6-bit bearing.

#include "decode/altitude.hpp"
#include "decode/field56.hpp"

namespace skycued::decode {

namespace {

AcasRa decodeBody(uint64_t f)
{
    AcasRa ra;
    ra.ara = static_cast<uint16_t>(field56(f, 9, 14));
    ra.rac = static_cast<uint8_t>(field56(f, 23, 4));
    ra.rat = field56(f, 27, 1) != 0;
    ra.mte = field56(f, 28, 1) != 0;
    ra.tti = static_cast<uint8_t>(field56(f, 29, 2));
    if (ra.tti == 1) {
        ra.threat_address = field56(f, 33, 24);
    }
    else if (ra.tti == 2) {
        ra.threat_alt_ft = altitudeAc13(field56(f, 31, 13));
        // Range: (value - 1) / 10 NM and bearing (value - 1) x 6 deg, both lower
        // bounds of the coded interval; 0 = not available.
        if (const uint32_t rng = field56(f, 44, 7); rng != 0)
            ra.threat_range_nm = static_cast<float>(rng - 1) / 10.0f;
        if (const uint32_t brg = field56(f, 51, 6); brg != 0)
            ra.threat_bearing_deg = static_cast<float>((brg - 1) * 6);
    }
    return ra;
}

}  // namespace

std::optional<AcasRa> decodeAcasRaMv(const Frame& f)
{
    if (f.df != 16 || f.bytes.size() != 14 || f.bits(32, 8) != 0x30)
        return std::nullopt;
    return decodeBody(f.field56(32));
}

std::optional<AcasRa> decodeAcasRaTc28(const Frame& f)
{
    if (typeCode(f) != 28 || f.bits(37, 3) != 2)
        return std::nullopt;
    return decodeBody(f.field56(32));
}

// Text. ARA bit k (1-based, bit 1 = 0x2000). With ARA bit 1 set (whatever MTE) bits 2..7
// read pairwise:
//   2 corrective (1) / preventive (0)      3 downward (1) / upward (0) sense
//   4 increased rate   5 sense reversal    6 altitude crossing
//   7 positive (1) / vertical speed limit (0)
// base text by (bit 7, bit 2):
//   positive,  corrective -> "climb" (bit 3 = 0) / "descend" (bit 3 = 1)
//   positive,  preventive -> "maintain vertical speed"
//   VSL,       corrective -> "level off"
//   VSL,       preventive -> "monitor vertical speed"
// then ", increase" (bit 4, climb/descend only), ", reversal" (5), ", crossing" (6).
// With ARA bit 1 clear and MTE = 1 (several threats, differing senses) bits 2..7 are
// independent flags joined by ", ": 2 "correction upward", 3 "climb", 4 "correction
// downward", 5 "descend", 6 "crossing", 7 "sense reversal"; none set -> "advisory".
// ARA bit 1 clear and MTE = 0 -> "no RA". Bits 8..14 are never read.
// RAT: "clear of conflict", whatever the ARA says.
std::string acasRaText(const AcasRa& ra)
{
    if (ra.rat)
        return "clear of conflict";
    auto bit = [&](unsigned k) { return ((ra.ara >> (14u - k)) & 1u) != 0; };
    if (bit(1)) {
        std::string s;
        const bool positive = bit(7), corrective = bit(2);
        if (positive)
            s = corrective ? (bit(3) ? "descend" : "climb") : "maintain vertical speed";
        else
            s = corrective ? "level off" : "monitor vertical speed";
        if (bit(4) && positive && corrective)
            s += ", increase";
        if (bit(5))
            s += ", reversal";
        if (bit(6))
            s += ", crossing";
        return s;
    }
    if (!ra.mte)
        return "no RA";
    static const char* const kMulti[] = {"correction upward", "climb", "correction downward", "descend",
                                         "crossing", "sense reversal"};  // ARA bits 2..7
    std::string s;
    for (unsigned k = 2; k <= 7; ++k) {
        if (bit(k)) {
            if (!s.empty())
                s += ", ";
            s += kMulti[k - 2];
        }
    }
    if (s.empty())
        s = "advisory";
    return s;
}

AcasCommon decodeAcasCommon(const Frame& f)
{
    AcasCommon c;
    if (f.df != 0 && f.df != 16)
        return c;
    c.vs_ground = f.bits(5, 1) != 0;
    c.sl = static_cast<uint8_t>(f.bits(8, 3));
    c.ri = static_cast<uint8_t>(f.bits(13, 4));
    return c;
}

}  // namespace skycued::decode
