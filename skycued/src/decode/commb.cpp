#include "decode/commb.hpp"

// Layouts from the public structure of ICAO Annex 10 Vol IV and the public
// descriptions of the registers, as written down in decoder design section 14
// item 8. Signed fields: the sign bit is the first bit of the value and the
// whole value is two's complement (so a track or heading wraps into 0..360).

#include <cmath>

#include "decode/field56.hpp"

namespace skycued::decode {

namespace {

// A status bit at `st` guards the n-bit value starting at st + 1.
bool statusZeroOk(uint64_t mb, unsigned st, unsigned n)
{
    return field56(mb, st, 1) != 0 || field56(mb, st + 1, n) == 0;
}

float wrap360(double deg)
{
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0)
        deg += 360.0;
    return static_cast<float>(deg);
}

double circDiff(double a, double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

bool validAis(uint32_t c) { return (c >= 1 && c <= 26) || c == 32 || (c >= 48 && c <= 57); }

}  // namespace

std::optional<Bds40> decodeBds40(uint64_t mb)
{
    const bool s1 = field56(mb, 1, 1), s2 = field56(mb, 14, 1), s3 = field56(mb, 27, 1);
    if (!(s1 || s2 || s3))
        return std::nullopt;
    if (!statusZeroOk(mb, 1, 12) || !statusZeroOk(mb, 14, 12) || !statusZeroOk(mb, 27, 12))
        return std::nullopt;
    if (field56(mb, 40, 8) != 0 || field56(mb, 52, 2) != 0)  // reserved
        return std::nullopt;
    if (!statusZeroOk(mb, 48, 3) || !statusZeroOk(mb, 54, 2))
        return std::nullopt;

    Bds40 b;
    if (s1)
        b.sel_alt_mcp_ft = static_cast<int32_t>(field56(mb, 2, 12)) * 16;
    if (s2)
        b.sel_alt_fms_ft = static_cast<int32_t>(field56(mb, 15, 12)) * 16;
    if (s3) {
        const double mbar = 800.0 + 0.1 * field56(mb, 28, 12);
        if (mbar < 800.0 || mbar > 1209.5)  // every 12-bit value lies inside; kept as the stated rule
            return std::nullopt;
        b.qnh_mb = static_cast<float>(mbar);
    }
    b.modes_valid = field56(mb, 48, 1) != 0;
    if (b.modes_valid) {
        b.vnav = field56(mb, 49, 1) != 0;
        b.althold = field56(mb, 50, 1) != 0;
        b.approach = field56(mb, 51, 1) != 0;
    }
    return b;
}

std::optional<Bds50> decodeBds50(uint64_t mb)
{
    if (!statusZeroOk(mb, 1, 10) || !statusZeroOk(mb, 12, 11) || !statusZeroOk(mb, 24, 10) ||
        !statusZeroOk(mb, 35, 10) || !statusZeroOk(mb, 46, 10))
        return std::nullopt;
    Bds50 b;
    if (field56(mb, 1, 1)) {
        const double roll = sfield56(mb, 2, 10) * 45.0 / 256.0;
        if (std::fabs(roll) > 50.0)
            return std::nullopt;
        b.roll_deg = static_cast<float>(roll);
    }
    if (field56(mb, 12, 1))
        b.track_deg = wrap360(field56(mb, 13, 11) * 90.0 / 512.0);
    if (field56(mb, 24, 1)) {
        const double gs = field56(mb, 25, 10) * 2.0;
        if (gs > 600.0)
            return std::nullopt;
        b.gs_kt = static_cast<float>(gs);
    }
    if (field56(mb, 35, 1)) {
        const double rate = sfield56(mb, 36, 10) * 8.0 / 256.0;
        if (std::fabs(rate) > 10.0)
            return std::nullopt;
        b.track_rate_dps = static_cast<float>(rate);
    }
    if (field56(mb, 46, 1)) {
        const double tas = field56(mb, 47, 10) * 2.0;
        if (tas > 500.0)
            return std::nullopt;
        b.tas_kt = static_cast<float>(tas);
    }
    if (b.gs_kt && b.tas_kt && std::fabs(*b.gs_kt - *b.tas_kt) > 200.0f)
        return std::nullopt;
    return b;
}

std::optional<Bds60> decodeBds60(uint64_t mb, std::optional<int32_t> alt_ft)
{
    if (!statusZeroOk(mb, 1, 11) || !statusZeroOk(mb, 13, 10) || !statusZeroOk(mb, 24, 10) ||
        !statusZeroOk(mb, 35, 10) || !statusZeroOk(mb, 46, 10))
        return std::nullopt;
    Bds60 b;
    if (field56(mb, 1, 1))
        b.heading_mag_deg = wrap360(field56(mb, 2, 11) * 90.0 / 512.0);
    if (field56(mb, 13, 1)) {
        const uint32_t ias = field56(mb, 14, 10);
        if (ias > 500)
            return std::nullopt;
        b.ias_kt = static_cast<uint16_t>(ias);
    }
    if (field56(mb, 24, 1)) {
        const double mach = field56(mb, 25, 10) * 0.004;
        if (mach > 1.0)
            return std::nullopt;
        b.mach = static_cast<float>(mach);
    }
    if (field56(mb, 35, 1)) {
        const int32_t r = sfield56(mb, 36, 10) * 32;
        if (std::abs(r) > 6000)
            return std::nullopt;
        b.baro_rate_fpm = r;
    }
    if (field56(mb, 46, 1)) {
        const int32_t r = sfield56(mb, 47, 10) * 32;
        if (std::abs(r) > 6000)
            return std::nullopt;
        b.inertial_rate_fpm = r;
    }
    if (b.baro_rate_fpm && b.inertial_rate_fpm && std::abs(*b.baro_rate_fpm - *b.inertial_rate_fpm) > 2000)
        return std::nullopt;
    if (alt_ft && b.ias_kt && b.mach) {
        // Loose cross-check: Mach from IAS at ISA (661.5 kt = speed of sound at
        // sea level, delta = pressure ratio), within 20 %.
        const double delta = std::pow(1.0 - 6.8756e-6 * *alt_ft, 5.2559);
        if (delta > 0.0) {
            const double est = *b.ias_kt / (661.5 * std::sqrt(delta));
            if (est > 0.0 && std::fabs(*b.mach - est) > 0.2 * est)
                return std::nullopt;
        }
    }
    return b;
}

CommbResult inferCommb(uint64_t mb, const CommbContext& ctx)
{
    CommbResult r;
    if (mb == 0) {
        r.kind = CommbKind::empty;
        return r;
    }
    const auto b40 = decodeBds40(mb);
    const auto b50 = decodeBds50(mb);
    const auto b60 = decodeBds60(mb, ctx.alt_ft);
    const int passing = (b40 ? 1 : 0) + (b50 ? 1 : 0) + (b60 ? 1 : 0);

    if (passing == 0) {
        const uint32_t first = field56(mb, 1, 8);
        bool other = first == 0x10 || first == 0x17 || first == 0x30;
        if (first == 0x20) {
            other = true;
            for (unsigned i = 0; i < 8; ++i)
                other = other && validAis(field56(mb, 9 + 6 * i, 6));
        }
        r.kind = other ? CommbKind::other : CommbKind::unknown;
        return r;
    }

    // Tie-break by the track's known state. Per candidate: contradicted when
    // an available comparison fails, supported when one holds. Contradicted
    // candidates drop out; if one remains it wins, else exactly one supported
    // candidate wins, else ambiguous. For 4,0 the only check is the selected
    // altitude range (0..50000 ft), which can contradict but never support.
    bool keep40 = b40.has_value(), keep50 = b50.has_value(), keep60 = b60.has_value();
    bool sup50 = false, sup60 = false;
    if (passing > 1) {
        if (b40) {
            const bool bad = (b40->sel_alt_mcp_ft && *b40->sel_alt_mcp_ft > 50000) ||
                             (b40->sel_alt_fms_ft && *b40->sel_alt_fms_ft > 50000);
            keep40 = !bad;
        }
        if (b50) {
            bool contradicted = false, supported = false;
            if (b50->track_deg && ctx.track_deg) {
                const bool ok = circDiff(*b50->track_deg, *ctx.track_deg) <= 10.0;
                contradicted = contradicted || !ok;
                supported = supported || ok;
            }
            if (b50->gs_kt && ctx.gs_kt) {
                const bool ok = std::fabs(*b50->gs_kt - *ctx.gs_kt) <= 30.0f;
                contradicted = contradicted || !ok;
                supported = supported || ok;
            }
            keep50 = !contradicted;
            sup50 = supported && !contradicted;
        }
        if (b60 && b60->baro_rate_fpm && ctx.baro_rate_fpm) {
            const bool ok = std::abs(*b60->baro_rate_fpm - *ctx.baro_rate_fpm) <= 500;
            keep60 = ok;
            sup60 = ok;
        }
        const int kept = (keep40 ? 1 : 0) + (keep50 ? 1 : 0) + (keep60 ? 1 : 0);
        if (kept > 1) {
            const int sup = (sup50 ? 1 : 0) + (sup60 ? 1 : 0);
            if (sup == 1) {
                keep40 = false;
                keep50 = sup50;
                keep60 = sup60;
            }
            else {
                r.kind = CommbKind::ambiguous;
                return r;
            }
        }
        else if (kept == 0) {
            r.kind = CommbKind::ambiguous;
            return r;
        }
    }
    if (keep40) {
        r.kind = CommbKind::bds40;
        r.b40 = b40;
    }
    else if (keep50) {
        r.kind = CommbKind::bds50;
        r.b50 = b50;
    }
    else {
        r.kind = CommbKind::bds60;
        r.b60 = b60;
    }
    return r;
}

}  // namespace skycued::decode
