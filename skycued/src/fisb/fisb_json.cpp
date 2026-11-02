#include "fisb/fisb_json.hpp"

#include "json_lite.hpp"
#include "time_util.hpp"

#include <cmath>
#include <cstdlib>

namespace skycued {
namespace fisb {

namespace {

// Degrees to 6 decimals without trailing zeros, locale-independent and
// cheap (a mosaic has thousands of these).
void appendFixed6(std::string& out, double v)
{
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    long long n = std::llround(v * 1e6);
    if (n < 0) {
        out += '-';
        n = -n;
    }
    out += std::to_string(n / 1000000);
    long long frac = n % 1000000;
    if (frac == 0)
        return;
    char digits[7];
    for (int i = 5; i >= 0; --i) {
        digits[i] = static_cast<char>('0' + frac % 10);
        frac /= 10;
    }
    int len = 6;
    while (len > 0 && digits[len - 1] == '0')
        --len;
    out += '.';
    out.append(digits, static_cast<std::size_t>(len));
}

std::string fixed6(double v)
{
    std::string s;
    appendFixed6(s, v);
    return s;
}

std::string iso(TimePoint t) { return formatIso8601Ms(t); }

const char* eventTypeName(FisbEvent::Kind k)
{
    switch (k) {
    case FisbEvent::Kind::text: return "fisb_text";
    case FisbEvent::Kind::twgo: return "fisb_twgo";
    case FisbEvent::Kind::radar: return "fisb_radar";
    case FisbEvent::Kind::raster: return "fisb_raster";
    case FisbEvent::Kind::expired: return "fisb_expired";
    }
    return "fisb_expired";
}

void mosaicFields(JsonWriter& w, const Mosaic& m)
{
    w.add("key", m.key());
    w.add("product", static_cast<int64_t>(m.product));
    w.add("name", productName(m.product));
    if (m.altitude_code)
        w.add("altitude_code", static_cast<int64_t>(*m.altitude_code));
    w.add("scale", static_cast<int64_t>(m.scale));
    w.add("product_time", iso(m.product_time));
    w.add("updated", iso(m.updated));
    if (m.published)
        w.add("published", iso(*m.published));
    w.add("expires", iso(m.expires));
    w.add("bins_per_block", static_cast<int64_t>(128));
    w.add("block_count", static_cast<int64_t>(m.blocks.size()));
    w.add("evicted", static_cast<int64_t>(m.evicted));
}

}  // namespace

const char* textKindName(TextRecord::Kind kind)
{
    switch (kind) {
    case TextRecord::metar: return "metar";
    case TextRecord::taf: return "taf";
    case TextRecord::pirep: return "pirep";
    case TextRecord::winds: return "winds";
    case TextRecord::other: break;
    }
    return "other";
}

const char* shapeGeomName(Shape::Geom geom)
{
    switch (geom) {
    case Shape::polygon: return "polygon";
    case Shape::polyline: return "polyline";
    case Shape::point: return "point";
    case Shape::circle: return "circle";
    case Shape::prism: return "prism";
    }
    return "polygon";
}

std::string rleJson(const std::array<uint8_t, 128>& bins)
{
    std::string out = "[";
    std::size_t i = 0;
    while (i < bins.size()) {
        std::size_t j = i + 1;
        while (j < bins.size() && bins[j] == bins[i])
            ++j;
        if (out.size() > 1)
            out += ',';
        out += '[';
        out += std::to_string(bins[i]);
        out += ',';
        out += std::to_string(j - i);
        out += ']';
        i = j;
    }
    out += ']';
    return out;
}

std::string textRecordJson(const TextRecord& r)
{
    JsonWriter w;
    w.add("key", textKey(r));
    w.add("kind", textKindName(r.kind));
    w.add("station", r.station);
    w.add("issued", iso(r.issued));
    w.add("expires", iso(r.expires));
    if (r.period_h)
        w.add("period_h", static_cast<int64_t>(*r.period_h));
    w.add("text", r.text);
    return w.str();
}

std::string shapeJson(const Shape& s)
{
    std::string points = "[";
    for (const auto& p : s.points) {
        if (points.size() > 1)
            points += ',';
        points += '[';
        appendFixed6(points, p.first);
        points += ',';
        appendFixed6(points, p.second);
        points += ']';
    }
    points += ']';
    JsonWriter w;
    w.add("geom", shapeGeomName(s.geom));
    w.addRaw("points", points);
    if (s.geom == Shape::circle || s.geom == Shape::prism)
        w.addRaw("radius_nm", fixed6(s.radius_nm));
    if (s.alt_low_ft)
        w.add("alt_low_ft", static_cast<int64_t>(*s.alt_low_ft));
    if (s.alt_high_ft)
        w.add("alt_high_ft", static_cast<int64_t>(*s.alt_high_ft));
    w.add("agl", s.agl);
    return w.str();
}

std::string twgoRecordJson(const TwgoRecord& r)
{
    JsonWriter w;
    w.add("key", r.key.str());
    w.add("product", static_cast<int64_t>(r.key.pid));
    w.add("name", productName(r.key.pid));
    w.add("report", static_cast<int64_t>(r.key.report));
    w.add("location", r.location);
    if (!r.label.empty())
        w.add("label", r.label);
    if (r.text)
        w.add("text", *r.text);
    w.add("has_graphics", r.has_graphics);
    w.add("complete", r.complete);
    w.add("cancelled", r.cancelled);
    if (r.start)
        w.add("start", iso(*r.start));
    if (r.end)
        w.add("end", iso(*r.end));
    w.add("received", iso(r.received));
    w.add("last_received", iso(r.last_received));
    w.add("expires", iso(r.expires));
    std::vector<std::string> shapes;
    shapes.reserve(r.shapes.size());
    for (const auto& s : r.shapes)
        shapes.push_back(shapeJson(s));
    w.addRaw("shapes", jsonArray(shapes));
    return w.str();
}

std::string mosaicHeadJson(const Mosaic& m)
{
    JsonWriter w;
    mosaicFields(w, m);
    return w.str();
}

std::string mosaicJson(const Mosaic& m)
{
    JsonWriter w;
    mosaicFields(w, m);
    std::string blocks = "[";
    for (const auto& b : m.blocks) {
        if (blocks.size() > 1)
            blocks += ',';
        blocks += "{\"block\":";
        blocks += std::to_string(b.block);
        blocks += ",\"lat0\":";
        appendFixed6(blocks, b.lat0);
        blocks += ",\"lon0\":";
        appendFixed6(blocks, b.lon0);
        blocks += ",\"dlat\":";
        appendFixed6(blocks, b.dlat);
        blocks += ",\"dlon\":";
        appendFixed6(blocks, b.dlon);
        blocks += ",\"rle\":";
        blocks += rleJson(b.bins);
        blocks += '}';
    }
    blocks += ']';
    w.addRaw("blocks", blocks);
    return w.str();
}

std::string summaryJson(const FisbSummary& s)
{
    JsonWriter w;
    w.add("enabled", s.enabled);
    w.add("text_records", static_cast<int64_t>(s.text_records));
    w.add("twgo_records", static_cast<int64_t>(s.twgo_records));
    w.add("mosaics", static_cast<int64_t>(s.mosaics));
    w.add("mosaic_blocks", static_cast<int64_t>(s.mosaic_blocks));
    w.add("stations", static_cast<int64_t>(s.stations));
    if (s.radar_age_s)
        w.add("radar_age_s", *s.radar_age_s);
    return w.str();
}

std::string eventRecordJson(const FisbEvent& e, TimePoint time)
{
    JsonWriter head;
    head.add("time", iso(time));
    head.add("type", eventTypeName(e.kind));
    std::string body;
    switch (e.kind) {
    case FisbEvent::Kind::text:
        if (e.text)
            body = textRecordJson(*e.text);
        break;
    case FisbEvent::Kind::twgo:
        if (e.twgo)
            body = twgoRecordJson(*e.twgo);
        break;
    case FisbEvent::Kind::radar:
    case FisbEvent::Kind::raster:
        if (e.mosaic)
            body = mosaicJson(*e.mosaic);
        break;
    case FisbEvent::Kind::expired: {
        JsonWriter w;
        w.add("key", e.key);
        w.add("product", static_cast<int64_t>(e.product));
        w.add("reason", e.reason);
        w.add("expires", iso(e.expires));
        body = w.str();
        break;
    }
    }
    std::string out = head.str();
    if (body.size() > 2) {  // splice "{...}" into the head object
        out.pop_back();
        out += ',';
        out.append(body, 1, std::string::npos);
    }
    return out;
}

}  // namespace fisb
}  // namespace skycued
