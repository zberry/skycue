#include "fisb/twgo.hpp"

#include "fisb/bit_reader.hpp"
#include "fisb/dlac.hpp"
#include "fisb/fisb_time.hpp"

#include <algorithm>
#include <tuple>

// Field layouts follow docs/fisb-reference.md §6, which documents
// fisb-decode's level0/apdu_twgo.py (BSD-2-Clause Plus Patent, see NOTICE);
// the geometry passes follow its level2/utilities.py. Reimplemented, not
// copied.

namespace skycued {
namespace fisb {

bool TwgoKey::operator<(const TwgoKey& o) const
{
    return std::tie(pid, year, report, location, month) < std::tie(o.pid, o.year, o.report, o.location, o.month);
}

bool TwgoKey::operator==(const TwgoKey& o) const
{
    return std::tie(pid, year, report, location, month) == std::tie(o.pid, o.year, o.report, o.location, o.month);
}

namespace {

std::string trimRight(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\x1e' || s.back() == ' ' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::string locationText(uint32_t raw)
{
    const uint8_t b[3] = {static_cast<uint8_t>(raw >> 16), static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw)};
    std::string s = decodeDlac(b, 3);
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

// raw * 360 / 2^bits, then folded: lon above 180 and lat above 90 wrap negative.
double angle(uint32_t raw, unsigned bits, double limit)
{
    double d = static_cast<double>(raw) * 360.0 / static_cast<double>(1u << bits);
    if (d > limit)
        d -= 2.0 * limit;
    return d;
}

struct Vertex {
    double lat, lon;
    int alt_ft;
    bool samePlace(const Vertex& o) const { return lat == o.lat && lon == o.lon; }
    bool operator==(const Vertex& o) const { return samePlace(o) && alt_ft == o.alt_ft; }
};

struct Prism {
    Vertex bot, top;  // alt_ft: z_bot / z_top
    double r_major_nm, r_minor_nm;
    uint8_t alpha;
};

// One graphic record, decoded.
struct Graphic {
    TwgoRecordInfo info;
    uint32_t report = 0;
    uint8_t year = 0;
    uint8_t status = 0;
    std::string label;
    uint8_t vertex_count = 0;
    std::vector<Vertex> verts;  // 6-byte vertex geometries
    std::vector<Prism> prisms;  // 14-byte geometries
    std::optional<TimePoint> start, end;
};

enum class GeomClass { polygon, polyline, point, prism, unsupported };

GeomClass geomClass(uint8_t g)
{
    switch (g) {
    case 3: case 4: return GeomClass::polygon;
    case 11: case 12: return GeomClass::polyline;
    case 9: case 10: return GeomClass::point;
    case 7: case 8: return GeomClass::prism;
    default: return GeomClass::unsupported;  // 5/6 obsolete ellipses, reserved values
    }
}

bool isAgl(uint8_t g) { return g == 4 || g == 8 || g == 9 || g == 12; }

std::optional<TimePoint> readTime(BitReader& r, uint8_t fmt, TimePoint ref)
{
    if (fmt == 0)  // "no date/time": no bytes (and the record is ignored)
        return std::nullopt;
    std::optional<unsigned> mo, dy;
    if (fmt == 1)
        mo = r.u(8).value_or(0);
    if (fmt == 1 || fmt == 2)
        dy = r.u(8).value_or(0);
    const unsigned hh = r.u(8).value_or(0);
    const unsigned mm = r.u(8).value_or(0);
    if (r.overrun())
        return std::nullopt;
    return resolveUtc(ref, mo, dy, hh, mm);
}

// Decodes one graphic overlay record of exactly len bytes. nullopt when the
// fields run past len.
std::optional<Graphic> parseGraphic(uint16_t pid, const uint8_t* g, std::size_t len, TimePoint ref)
{
    Graphic out;
    BitReader r(g, len);
    out.info.format = 8;
    out.info.declared = r.u(10).value_or(0);
    out.report = r.u(14).value_or(0);
    out.year = r.u8(7).value_or(0);
    r.u(2);  // applicability start-year offset (NOTAMs); dates come from ref instead
    r.u(2);  // end-year offset
    r.u(4);  // overlay record id - 1
    const bool dlac_label = r.u(1).value_or(0) != 0;
    if (r.overrun())
        return std::nullopt;
    if (dlac_label) {
        if (len < 14)
            return std::nullopt;
        out.label = decodeDlac(g + 5, 9);
        while (!out.label.empty() && out.label.back() == ' ')
            out.label.pop_back();
        r.u(32);
        r.u(32);
        r.u(8);
    } else
        r.u(16);
    r.u(1);  // element flag
    const bool qualifier = r.u(1).value_or(0) != 0;
    const bool param = r.u(1).value_or(0) != 0;
    r.u(5);  // object element
    r.u(4);  // object type
    out.status = r.u8(4).value_or(0);
    if (qualifier && pid == 14)  // fisb-decode: only G-AIRMET carries the 3-byte qualifier
        r.u(24);
    if (param)
        r.u(16);
    const uint8_t options = r.u8(2).value_or(0);
    const uint8_t fmt = r.u8(2).value_or(0);
    const uint8_t geometry = r.u8(4).value_or(0);
    const uint8_t op = r.u8(2).value_or(0);
    out.vertex_count = static_cast<uint8_t>(r.u8(6).value_or(0) + 1);
    if (r.overrun())
        return std::nullopt;
    out.info.geometry = geometry;
    out.info.vertices = out.vertex_count;

    if (options == 1 || options == 3)
        out.start = readTime(r, fmt, ref);
    if (options == 2 || options == 3)
        out.end = readTime(r, fmt, ref);
    if (r.overrun())
        return std::nullopt;

    const GeomClass gc = geomClass(geometry);
    if (gc == GeomClass::polygon || gc == GeomClass::polyline || gc == GeomClass::point) {
        for (unsigned i = 0; i < out.vertex_count; ++i) {
            const auto lon = r.u(19), lat = r.u(19), alt = r.u(10);
            if (r.overrun())
                return std::nullopt;
            out.verts.push_back({angle(*lat, 19, 90.0), angle(*lon, 19, 180.0), static_cast<int>(*alt) * 100});
        }
    } else if (gc == GeomClass::prism) {
        for (unsigned i = 0; i < out.vertex_count; ++i) {
            const auto lon_b = r.u(18), lat_b = r.u(18), lon_t = r.u(18), lat_t = r.u(18);
            const auto z_b = r.u(7), z_t = r.u(7), r_maj = r.u(9), r_min = r.u(9);
            const auto alpha = r.u8(8);
            if (r.overrun())
                return std::nullopt;
            Prism pr;
            pr.bot = {angle(*lat_b, 18, 90.0), angle(*lon_b, 18, 180.0), static_cast<int>(*z_b) * 500};
            pr.top = {angle(*lat_t, 18, 90.0), angle(*lon_t, 18, 180.0), static_cast<int>(*z_t) * 500};
            pr.r_major_nm = *r_maj * 0.2;
            pr.r_minor_nm = *r_min * 0.2;
            pr.alpha = *alpha;
            out.prisms.push_back(pr);
        }
    }
    out.info.consumed = r.bytesConsumed();
    // DO-358B (via fisb-decode): ignore a record with a parameter, with
    // times present in format 0 or 2, or with overlay operator 2/3.
    out.info.ignored = param || (options != 0 && (fmt == 0 || fmt == 2)) || op >= 2 || gc == GeomClass::unsupported;
    return out;
}

// Splits a vertex list into rings, each ending where it returns to its
// first vertex. A trailing unclosed run is the last ring.
std::vector<std::vector<Vertex>> rings(const std::vector<Vertex>& v)
{
    std::vector<std::vector<Vertex>> out;
    std::vector<Vertex> cur;
    for (const auto& x : v) {
        cur.push_back(x);
        if (cur.size() > 1 && x == cur.front()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

bool lastRingClosed(const std::vector<Vertex>& v)
{
    auto r = rings(v);
    return !r.empty() && r.back().size() > 1 && r.back().back() == r.back().front();
}

void widen(Shape& s, int alt)
{
    s.alt_low_ft = s.alt_low_ft ? std::min(*s.alt_low_ft, alt) : alt;
    s.alt_high_ft = s.alt_high_ft ? std::max(*s.alt_high_ft, alt) : alt;
}

// Appends the shapes of one (possibly merged) graphic record.
void emitShapes(const Graphic& g, std::vector<Shape>& out)
{
    const GeomClass gc = geomClass(g.info.geometry);
    const bool agl = isAgl(g.info.geometry);
    if (gc == GeomClass::point) {
        for (const auto& v : g.verts) {
            Shape s;
            s.geom = Shape::point;
            s.agl = agl;
            s.points.emplace_back(v.lat, v.lon);
            widen(s, v.alt_ft);
            out.push_back(s);
        }
    } else if (gc == GeomClass::polyline) {
        Shape s;
        s.geom = Shape::polyline;
        s.agl = agl;
        for (const auto& v : g.verts) {
            s.points.emplace_back(v.lat, v.lon);
            widen(s, v.alt_ft);
        }
        if (!s.points.empty())
            out.push_back(s);
    } else if (gc == GeomClass::polygon) {
        // Rings at different altitudes over the same outline are one
        // polygon's top and bottom (reference §6.4).
        const std::size_t first = out.size();
        for (const auto& ring : rings(g.verts)) {
            std::vector<std::pair<double, double>> pts;
            pts.reserve(ring.size());
            for (const auto& v : ring)
                pts.emplace_back(v.lat, v.lon);
            Shape* same = nullptr;
            for (std::size_t i = first; i < out.size(); ++i)
                if (out[i].points == pts)
                    same = &out[i];
            if (!same) {
                Shape s;
                s.geom = Shape::polygon;
                s.agl = agl;
                s.points = std::move(pts);
                out.push_back(std::move(s));
                same = &out.back();
            }
            for (const auto& v : ring)
                widen(*same, v.alt_ft);
        }
    } else if (gc == GeomClass::prism) {
        for (const auto& p : g.prisms) {
            Shape s;
            s.geom = p.bot.alt_ft == p.top.alt_ft ? Shape::circle : Shape::prism;
            s.agl = agl;
            s.points.emplace_back(p.bot.lat, p.bot.lon);
            if (!p.top.samePlace(p.bot))
                s.points.emplace_back(p.top.lat, p.top.lon);
            s.radius_nm = p.r_major_nm;
            s.alt_low_ft = std::min(p.bot.alt_ft, p.top.alt_ft);
            s.alt_high_ft = std::max(p.bot.alt_ft, p.top.alt_ft);
            out.push_back(s);
        }
    }
}

void minTime(std::optional<TimePoint>& acc, const std::optional<TimePoint>& t)
{
    if (t && (!acc || *t < *acc))
        acc = t;
}

void maxTime(std::optional<TimePoint>& acc, const std::optional<TimePoint>& t)
{
    if (t && (!acc || *t > *acc))
        acc = t;
}

}  // namespace

std::string TwgoKey::str() const
{
    return std::to_string(pid) + "-" + std::to_string(year) + "-" + std::to_string(report) + "-" +
           locationText(location) + "-" + std::to_string(month);
}

std::optional<TwgoParsed> parseTwgo(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref)
{
    if (!p || n < 6)
        return std::nullopt;
    TwgoParsed out;
    out.record_format = static_cast<uint8_t>(p[0] >> 4);
    const unsigned count = p[1] >> 4;
    const uint32_t loc = (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 8) | p[4];
    if (count == 0 || (out.record_format != 2 && out.record_format != 8))
        return std::nullopt;
    out.key.pid = h.product_id;
    out.key.location = loc;
    out.key.month = h.month.value_or(0);
    out.location = locationText(loc);

    std::size_t off = 6;
    if (out.record_format == 2) {
        bool first = true;
        for (unsigned i = 0; i < count; ++i) {
            if (n - off < 5)
                return std::nullopt;
            const uint8_t* t = p + off;
            const std::size_t len = (static_cast<std::size_t>(t[0]) << 8) | t[1];
            if (len < 5 || len > n - off)
                return std::nullopt;
            const uint32_t report = (static_cast<uint32_t>(t[2]) << 6) | (t[3] >> 2);
            const auto year = static_cast<uint8_t>(((t[3] & 3u) << 5) | (t[4] >> 3));
            const bool active = ((t[4] >> 2) & 1u) != 0;
            std::string text = trimRight(decodeDlac(t + 5, len - 5));
            if (first) {
                out.key.report = report;
                out.key.year = year;
                out.text = std::move(text);
                first = false;
            } else if (!text.empty())
                *out.text += (out.text->empty() ? "" : "\n") + text;
            if (!active)
                out.cancel = true;
            TwgoRecordInfo info;
            info.format = 2;
            info.declared = len;
            info.consumed = len;  // 5-byte header, then DLAC to the end of the record
            out.records.push_back(info);
            off += len;
        }
        out.leftover = n - off;
        return out;
    }

    out.has_graphics = true;
    std::vector<Graphic> recs;
    for (unsigned i = 0; i < count; ++i) {
        if (n - off < 5)
            return std::nullopt;
        const uint8_t* g = p + off;
        const std::size_t len = (static_cast<std::size_t>(g[0]) << 2) | (g[1] >> 6);
        if (len < 5 || len > n - off)
            return std::nullopt;
        auto rec = parseGraphic(h.product_id, g, len, ref);
        if (!rec)
            return std::nullopt;
        out.records.push_back(rec->info);
        recs.push_back(std::move(*rec));
        off += len;
    }
    out.leftover = n - off;

    out.key.report = recs.front().report;
    out.key.year = recs.front().year;
    out.object_status = recs.front().status;
    out.label = recs.front().label;
    for (const auto& r : recs)
        if (r.status == 13)
            out.cancel = true;

    // A polygon or polyline of more than 64 vertices continues in the next
    // record of the same geometry; the junction vertex may be repeated.
    std::optional<Graphic> pending;
    for (auto& r : recs) {
        if (r.info.ignored)
            continue;
        minTime(out.start, r.start);
        maxTime(out.end, r.end);
        const GeomClass gc = geomClass(r.info.geometry);
        if (pending && pending->info.geometry == r.info.geometry && pending->vertex_count == 64 &&
            (gc == GeomClass::polyline || (gc == GeomClass::polygon && !lastRingClosed(pending->verts)))) {
            auto from = r.verts.begin();
            if (!pending->verts.empty() && !r.verts.empty() && pending->verts.back() == r.verts.front())
                ++from;
            pending->verts.insert(pending->verts.end(), from, r.verts.end());
            pending->vertex_count = r.vertex_count;  // a 64 again means it continues further
            continue;
        }
        if (pending)
            emitShapes(*pending, out.shapes);
        pending = std::move(r);
    }
    if (pending)
        emitShapes(*pending, out.shapes);
    return out;
}

std::optional<TimePoint> notamTextEnd(const std::string& text)
{
    auto digits = [&](std::size_t at, std::size_t n) {
        if (at + n > text.size())
            return false;
        for (std::size_t i = at; i < at + n; ++i)
            if (text[i] < '0' || text[i] > '9')
                return false;
        return true;
    };
    auto num = [&](std::size_t at) { return static_cast<unsigned>((text[at] - '0') * 10 + (text[at + 1] - '0')); };
    for (std::size_t i = 0; i + 11 <= text.size(); ++i) {
        // A 10-digit group not inside a longer run of digits, then '-'.
        if ((i > 0 && text[i - 1] >= '0' && text[i - 1] <= '9') || !digits(i, 10) || text[i + 10] != '-')
            continue;
        const std::size_t e = i + 11;
        if (text.compare(e, 4, "PERM") == 0)
            return std::nullopt;
        if (!digits(e, 10) || (e + 10 < text.size() && text[e + 10] >= '0' && text[e + 10] <= '9'))
            continue;
        const unsigned mo = num(e + 2), dd = num(e + 4), hh = num(e + 6), mi = num(e + 8);
        if (mo < 1 || mo > 12 || hh > 23 || mi > 59)
            return std::nullopt;
        return detail::makeUtc(2000 + num(e), mo, dd, hh, mi);
    }
    return std::nullopt;
}

}  // namespace fisb
}  // namespace skycued
