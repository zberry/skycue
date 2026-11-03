#include "web_fisb.hpp"

#include "engine.hpp"
#include "fisb/fisb_json.hpp"
#include "fisb/products.hpp"
#include "json_lite.hpp"
#include "time_util.hpp"
#include "web_api.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

using fisb::Mosaic;
using fisb::Shape;
using fisb::TextRecord;
using fisb::TwgoRecord;
using TimePoint = std::chrono::system_clock::time_point;

constexpr uint16_t kRegional = 63, kConus = 64;

std::string iso(TimePoint t) { return formatIso8601Ms(t); }
int64_t num(uint64_t v) { return static_cast<int64_t>(v); }

std::string deg(double v)
{
    std::string s;
    fisb::appendFixed6(s, v);
    return s;
}

// A GeoJSON position: [lon, lat].
std::string position(double lat, double lon) { return "[" + deg(lon) + "," + deg(lat) + "]"; }

std::string upper(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// A non-negative decimal integer no larger than `max`.
std::optional<unsigned> parseUnsigned(const std::string& s, unsigned max)
{
    if (s.empty() || s.size() > 5 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    const unsigned long v = std::stoul(s);
    if (v > max)
        return std::nullopt;
    return static_cast<unsigned>(v);
}

std::string param(const std::map<std::string, std::string>& params, const char* key)
{
    const auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
}

ApiResponse reply(unsigned status, std::string body)
{
    ApiResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

ApiResponse fail(unsigned status, const std::string& message) { return reply(status, errorJson(message)); }

// ---- status ----

std::string mosaicHead(const fisb::MosaicHead& m, TimePoint now)
{
    JsonWriter w;
    w.add("key", m.key);
    w.add("product", static_cast<int64_t>(m.product));
    w.add("name", fisb::productName(m.product));
    if (m.altitude_code)
        w.add("altitude_code", static_cast<int64_t>(*m.altitude_code));
    w.add("product_time", iso(m.product_time));
    w.add("age_s", std::round(std::chrono::duration<double>(now - m.product_time).count() * 10) / 10);
    w.add("blocks", num(m.block_count));
    if (m.published)
        w.add("published", iso(*m.published));
    w.add("expires", iso(m.expires));
    return w.str();
}

std::string siteId(uint64_t key)
{
    char buf[24];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(key));
    return buf;
}

const fisb::MosaicHead* findRadar(const fisb::FisbSummary& s, uint16_t product)
{
    for (const auto& m : s.radar)
        if (m.product == product)
            return &m;
    return nullptr;
}

// ---- GeoJSON ----

// A closed ring approximating a circle of radius_nm around (lat, lon).
std::string circleRing(double lat, double lon, double radius_nm)
{
    constexpr int kSides = 32;
    constexpr double kPi = 3.14159265358979323846;
    const double dlat = radius_nm / 60.0;
    const double dlon = dlat / std::max(std::cos(lat * kPi / 180.0), 0.01);
    std::string ring = "[";
    std::string first;
    for (int i = 0; i < kSides; ++i) {
        const double a = 2 * kPi * i / kSides;
        const double plat = std::max(-90.0, std::min(90.0, lat + dlat * std::cos(a)));
        double plon = lon + dlon * std::sin(a);
        if (plon > 180)
            plon -= 360;
        else if (plon < -180)
            plon += 360;
        const std::string p = position(plat, plon);
        if (i == 0)
            first = p;
        else
            ring += ',';
        ring += p;
    }
    ring += ',' + first + ']';
    return ring;
}

std::string geometry(const char* type, const std::string& coordinates)
{
    return JsonWriter().add("type", type).addRaw("coordinates", coordinates).str();
}

// The shape's GeoJSON geometry; "" when it has no point.
std::string shapeGeometry(const Shape& s)
{
    if (s.points.empty())
        return std::string();
    const auto& p0 = s.points.front();
    auto line = [&](bool close) {
        std::string c = "[";
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            if (i)
                c += ',';
            c += position(s.points[i].first, s.points[i].second);
        }
        if (close && s.points.front() != s.points.back())
            c += ',' + position(p0.first, p0.second);
        return c + ']';
    };
    switch (s.geom) {
    case Shape::point:
    case Shape::circle:
        return geometry("Point", position(p0.first, p0.second));
    case Shape::prism:
        if (s.radius_nm > 0)
            return geometry("Polygon", "[" + circleRing(p0.first, p0.second, s.radius_nm) + "]");
        return geometry("Point", position(p0.first, p0.second));
    case Shape::polyline:
        return s.points.size() >= 2 ? geometry("LineString", line(false))
                                    : geometry("Point", position(p0.first, p0.second));
    case Shape::polygon:
        break;
    }
    std::set<std::pair<double, double>> distinct(s.points.begin(), s.points.end());
    if (distinct.size() >= 3)
        return geometry("Polygon", "[" + line(true) + "]");
    if (s.points.size() >= 2)
        return geometry("LineString", line(false));
    return geometry("Point", position(p0.first, p0.second));
}

void appendFeatures(const TwgoRecord& r, std::vector<std::string>& out)
{
    for (std::size_t i = 0; i < r.shapes.size(); ++i) {
        const Shape& s = r.shapes[i];
        const std::string geom = shapeGeometry(s);
        if (geom.empty())
            continue;
        JsonWriter p;
        p.add("key", r.key.str());
        p.add("product", static_cast<int64_t>(r.key.pid));
        p.add("name", fisb::productName(r.key.pid));
        p.add("report", static_cast<int64_t>(r.key.report));
        p.add("location", r.location);
        if (!r.label.empty())
            p.add("label", r.label);
        if (r.text)
            p.add("text", *r.text);
        if (r.start)
            p.add("start", iso(*r.start));
        if (r.graphics_end)
            p.add("graphics_end", iso(*r.graphics_end));
        if (r.text_end)
            p.add("text_end", iso(*r.text_end));
        p.add("expires", iso(r.expires));
        p.add("cancelled", r.cancelled);
        p.add("pending", r.pending);
        p.add("geom", fisb::shapeGeomName(s.geom));
        if (s.geom == Shape::circle || s.geom == Shape::prism)
            p.addRaw("radius_nm", deg(s.radius_nm));
        if (s.geom == Shape::prism)
            p.addRaw("center", position(s.points.front().first, s.points.front().second));
        if (s.alt_low_ft)
            p.add("alt_low_ft", static_cast<int64_t>(*s.alt_low_ft));
        if (s.alt_high_ft)
            p.add("alt_high_ft", static_cast<int64_t>(*s.alt_high_ft));
        p.add("agl", s.agl);
        p.add("shape", static_cast<int64_t>(i));
        out.push_back(JsonWriter()
                          .add("type", "Feature")
                          .add("id", r.key.str() + "/" + std::to_string(i))
                          .addRaw("geometry", geom)
                          .addRaw("properties", p.str())
                          .str());
    }
}

// ---- palettes and legends ----

struct Rgb {
    int r, g, b;
};

// "#rrggbb" a fraction t of the way from a to b.
std::string ramp(Rgb a, Rgb b, double t)
{
    auto mix = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
    char buf[16];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
    return buf;
}

std::string levelJson(int level, const std::string& label, bool approximate = false)
{
    JsonWriter w;
    w.add("level", static_cast<int64_t>(level)).add("label", label);
    if (approximate)
        w.add("approximate", true);
    return w.str();
}

std::string colorJson(int level, const std::string& color)
{
    return JsonWriter().add("level", static_cast<int64_t>(level)).add("color", color).str();
}

// NEXRAD (reference §5.3): NWS reflectivity colours at each level's lower
// bound. dbz_min <= dBZ < dbz_max. CONUS level 0 is "no data" and level 1
// "< 20 dBZ" (every empty CONUS block): both left undrawn.
const char* const kNexradColors[8] = {"transparent", "#04e9e7", "#02fd02", "#008e00",
                                      "#fdf802",     "#fd9500", "#fd0000", "#f800fd"};
const int kNexradDbz[9] = {-1, 5, 20, 30, 40, 45, 50, 55, -1};  // [level] lower bound, [level+1] upper
const char* const kNexradWords[8] = {"",      "very light", "light to moderate", "moderate", "moderate to heavy",
                                     "heavy", "very heavy", "extreme"};

std::string nexradPalette(uint16_t product)
{
    std::vector<std::string> out;
    for (int l = 0; l < 8; ++l) {
        JsonWriter w;
        w.add("level", static_cast<int64_t>(l));
        const bool conus = product == kConus;
        if (!(conus && l == 0)) {
            if (kNexradDbz[l] >= 0 && !(conus && l == 1))
                w.add("dbz_min", static_cast<int64_t>(kNexradDbz[l]));
            if (kNexradDbz[l + 1] >= 0)
                w.add("dbz_max", static_cast<int64_t>(kNexradDbz[l + 1]));
        }
        w.add("color", conus && l == 1 ? "transparent" : kNexradColors[l]);
        out.push_back(w.str());
    }
    return jsonArray(out);
}

std::string nexradLegend(uint16_t product)
{
    const bool conus = product == kConus;
    std::vector<std::string> levels;
    for (int l = 0; l < 8; ++l) {
        std::string label;
        if (l == 0)
            label = conus ? "no data" : "< 5 dBZ";
        else if (l == 1 && conus)
            label = "< 20 dBZ, or no precipitation (empty blocks)";
        else if (l == 7)
            label = ">= 55 dBZ (extreme)";
        else
            label = std::to_string(kNexradDbz[l]) + "-" + std::to_string(kNexradDbz[l + 1] - 1) + " dBZ (" +
                    kNexradWords[l] + ")";
        levels.push_back(levelJson(l, label, conus && l <= 1));
    }
    return JsonWriter()
        .add("title", conus ? "NEXRAD CONUS reflectivity" : "NEXRAD regional reflectivity")
        .add("units", "dBZ")
        .add("approximate", false)
        .addRaw("levels", jsonArray(levels))
        .str();
}

// The altitude a 70/71/90/91 layer's code stands for (reference §5.1, [U]).
std::optional<int64_t> layerAltitudeFt(const Mosaic& m)
{
    if (!m.altitude_code)
        return std::nullopt;
    const int64_t n = *m.altitude_code;
    if (m.product == 70 || m.product == 90)
        return n * 2000 + 2000;
    if (m.product == 71 || m.product == 91)
        return n * 2000 + 18000;
    return std::nullopt;
}

// {"palette":[...], "legend":{...}} pieces for a raster product (reference
// §5.5: every mapping comes from FD only and is marked approximate).
std::pair<std::string, std::string> rasterPaletteLegend(const Mosaic& m)
{
    std::vector<std::string> palette, levels;
    JsonWriter legend;
    switch (m.product) {
    case 103: {
        const char* const strikes[8] = {"no strikes", "1 strike", "2 strikes",   "3-5 strikes",
                                        "6-10 strikes", "11-15 strikes", "> 15 strikes", "no data"};
        for (int l = 0; l < 16; ++l) {
            const int code = l & 7;
            std::string label = strikes[code];
            if (code != 0 && code != 7)
                label += (l & 8) ? " (positive)" : " (negative)";
            levels.push_back(levelJson(l, label));
            palette.push_back(colorJson(
                l, code == 0 || code == 7 ? "transparent" : ramp({255, 245, 160}, {255, 0, 0}, (code - 1) / 5.0)));
        }
        legend.add("title", "Lightning").add("units", "strikes per bin");
        legend.add("note", "level = polarity (8: positive) + strike-count code (0-7)");
        break;
    }
    case 90:
    case 91: {
        for (int l = 0; l < 16; ++l) {
            char label[48];
            if (l == 0)
                // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
                std::snprintf(label, sizeof label, "EDR < 0.07");
            else if (l == 14)
                // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
                std::snprintf(label, sizeof label, "EDR >= 0.98");
            else if (l == 15)
                // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
                std::snprintf(label, sizeof label, "no data");
            else
                // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
                std::snprintf(label, sizeof label, "EDR %.2f-%.2f", l * 0.07, l * 0.07 + 0.06);
            levels.push_back(levelJson(l, label));
            palette.push_back(colorJson(
                l, l == 0 || l == 15 ? "transparent" : ramp({255, 255, 178}, {177, 0, 38}, (l - 1) / 13.0)));
        }
        legend.add("title", m.product == 90 ? "Turbulence (low)" : "Turbulence (high)").add("units", "EDR");
        break;
    }
    case 70:
    case 71: {
        const char* const severity[8] = {"none", "trace", "light", "moderate", "severe", "heavy", "unused", "no data"};
        const char* const sld[4] = {"<= 5%", "<= 50%", "> 50%", "no data"};
        const char* const probability[8] = {"<= 5%", "<= 20%", "<= 30%", "<= 40%",
                                            "<= 80%", "<= 60%", "> 80%",  "no data"};
        auto field = [](const char* name, int shift, int mask, const char* const* labels, bool approximate) {
            std::vector<std::string> lv;
            for (int i = 0; i <= mask; ++i)
                lv.push_back(levelJson(i, labels[i], approximate));
            return JsonWriter()
                .add("name", name)
                .add("shift", static_cast<int64_t>(shift))
                .add("mask", static_cast<int64_t>(mask))
                .addRaw("levels", jsonArray(lv))
                .str();
        };
        for (int l = 0; l < 8; ++l) {
            levels.push_back(levelJson(l, severity[l]));
            palette.push_back(colorJson(
                l, l == 0 || l >= 6 ? "transparent" : ramp({198, 219, 239}, {84, 39, 143}, (l - 1) / 4.0)));
        }
        legend.add("title", m.product == 70 ? "Icing (low)" : "Icing (high)");
        legend.add("packed", "ddsssppp");
        legend.add("palette_field", "severity");
        legend.add("note", "the value packs SLD (bits 7-6), severity (bits 5-3) and probability (bits 2-0); "
                           "palette and levels are the severity field, (value >> 3) & 7");
        legend.addRaw("fields", jsonArray({field("severity", 3, 7, severity, false), field("sld", 6, 3, sld, false),
                                           field("probability", 0, 7, probability, true)}));
        break;
    }
    default: {  // 84 cloud tops: the value-to-altitude mapping is not known
        for (int l = 0; l < 16; ++l) {
            levels.push_back(levelJson(l, "level " + std::to_string(l)));
            palette.push_back(colorJson(l, l == 0 ? "transparent" : ramp({208, 232, 255}, {8, 48, 107}, (l - 1) / 14.0)));
        }
        legend.add("title", "Cloud tops");
        legend.add("note", "the level-to-altitude mapping is not known; levels are passed through");
        break;
    }
    }
    legend.add("approximate", true);
    if (const auto ft = layerAltitudeFt(m)) {
        legend.add("altitude_ft", *ft);
        legend.add("altitude_approximate", true);
    }
    legend.addRaw("levels", jsonArray(levels));
    return {jsonArray(palette), legend.str()};
}

// mosaicJson plus "palette" and "legend".
std::string withPaletteLegend(std::string mosaic, const std::string& palette, const std::string& legend)
{
    mosaic.pop_back();  // the closing '}'
    mosaic += ",\"palette\":" + palette + ",\"legend\":" + legend + "}";
    return mosaic;
}

// ---- routes ----

}  // namespace

ApiResponse fisbTextRoute(const std::map<std::string, std::string>& params, const fisb::FisbSnapshot& s)
{
    const std::string type = param(params, "type");
    const std::string station = upper(param(params, "station"));
    static const std::set<std::string> kTypes{"metar", "taf", "pirep", "winds", "other", "twgo"};
    if (!type.empty() && !kTypes.count(type))
        return fail(400, "type must be one of metar, taf, pirep, winds, other, twgo");
    std::vector<std::string> records;
    if (type == "twgo") {
        std::vector<const TwgoRecord*> sel;
        for (const auto& r : s.twgo)
            if (r.complete && !r.cancelled && !r.has_graphics && startsWith(upper(r.location), station))
                sel.push_back(&r);
        std::stable_sort(sel.begin(), sel.end(), [](const TwgoRecord* a, const TwgoRecord* b) {
            return a->location != b->location ? a->location < b->location : a->received < b->received;
        });
        for (const auto* r : sel)
            records.push_back(fisb::twgoRecordJson(*r));
    } else {
        std::vector<const TextRecord*> sel;
        auto take = [&](const char* kind, const std::vector<TextRecord>& list) {
            if (!type.empty() && type != kind)
                return;
            for (const auto& r : list)
                if (startsWith(upper(r.station), station))
                    sel.push_back(&r);
        };
        take("metar", s.metar);
        take("taf", s.taf);
        take("pirep", s.pirep);
        take("winds", s.winds);
        take("other", s.other);
        std::stable_sort(sel.begin(), sel.end(), [](const TextRecord* a, const TextRecord* b) {
            return a->station != b->station ? a->station < b->station : a->issued < b->issued;
        });
        for (const auto* r : sel)
            records.push_back(fisb::textRecordJson(*r));
    }
    return reply(200, JsonWriter().addRaw("records", jsonArray(records)).str());
}

namespace {

ApiResponse shapesRoute(const std::map<std::string, std::string>& params, const fisb::FisbSnapshot& s)
{
    std::set<unsigned> products;
    const std::string list = param(params, "product");
    if (!list.empty()) {
        std::size_t pos = 0;
        while (pos <= list.size()) {
            const std::size_t comma = std::min(list.find(',', pos), list.size());
            const auto id = parseUnsigned(list.substr(pos, comma - pos), 65535);
            if (!id)
                return fail(400, "product must be a comma-separated list of product IDs");
            products.insert(*id);
            pos = comma + 1;
        }
    }
    std::vector<std::string> features;
    for (const auto& r : s.twgo)
        if (!r.cancelled && (products.empty() || products.count(r.key.pid)))
            appendFeatures(r, features);
    return reply(200,
                 JsonWriter().add("type", "FeatureCollection").addRaw("features", jsonArray(features)).str());
}

ApiResponse radarRoute(const std::map<std::string, std::string>& params, const Engine& engine)
{
    const std::string which = param(params, "product");
    uint16_t product = 0;
    if (which == "regional" || which == "63")
        product = kRegional;
    else if (which == "conus" || which == "64")
        product = kConus;
    else
        return fail(400, "product must be regional or conus");
    const auto m = engine.fisbMosaic(product, std::nullopt);
    if (!m)
        return fail(404, std::string("no ") + fisb::productName(product) + " mosaic");
    return reply(200, withPaletteLegend(fisb::mosaicJson(*m), nexradPalette(product), nexradLegend(product)));
}

ApiResponse rasterRoute(const std::map<std::string, std::string>& params, const Engine& engine)
{
    const auto product = parseUnsigned(param(params, "product"), 65535);
    static const std::set<unsigned> kRasters{70, 71, 84, 90, 91, 103};
    if (!product || !kRasters.count(*product))
        return fail(400, "product must be one of 70, 71, 84, 90, 91, 103");
    std::optional<unsigned> altitude;
    if (params.count("altitude")) {
        altitude = parseUnsigned(param(params, "altitude"), 7);
        if (!altitude)
            return fail(400, "altitude must be an altitude code, 0-7");
    }
    // Without ?altitude=, the lowest layer.
    const auto best = engine.fisbMosaic(static_cast<uint16_t>(*product),
                                        altitude ? std::optional<uint8_t>(static_cast<uint8_t>(*altitude))
                                                 : std::nullopt);
    if (!best)
        return fail(404, "no " + std::string(fisb::productName(static_cast<uint16_t>(*product))) + " mosaic" +
                             (altitude ? " at altitude code " + std::to_string(*altitude) : std::string()));
    const auto pl = rasterPaletteLegend(*best);
    return reply(200, withPaletteLegend(fisb::mosaicJson(*best), pl.first, pl.second));
}

// {"<id>":[lat,lon],...}: every known location, ids sorted.
ApiResponse locationsRoute(const Engine& engine)
{
    JsonWriter w;
    for (const auto& [id, pos] : engine.locationPositions())
        w.addRaw(id.c_str(), "[" + deg(pos.first) + "," + deg(pos.second) + "]");
    return reply(200, w.str());
}

// What left the store, as the store said (FisbEvent::expired_kind).
const char* expiredKind(const FisbNotice& n)
{
    switch (n.expired_kind) {
    case fisb::FisbEvent::Kind::text: return "text";
    case fisb::FisbEvent::Kind::twgo: return "twgo";
    case fisb::FisbEvent::Kind::radar: return "radar";
    case fisb::FisbEvent::Kind::raster: return "raster";
    case fisb::FisbEvent::Kind::expired: break;
    }
    return "other";
}

}  // namespace

std::string twgoFeaturesJson(const TwgoRecord& record)
{
    std::vector<std::string> out;
    appendFeatures(record, out);
    return jsonArray(out);
}

std::string fisbStatusJson(const Snapshot& snap)
{
    const EngineStats& st = snap.stats;
    const fisb::FisbSummary& f = snap.fisb;
    JsonWriter w;
    w.add("enabled", f.enabled);
    w.add("uplinks", num(st.fisb_uplinks));
    w.add("products", num(st.fisb_products));
    w.add("bad_frames", num(st.fisb_bad_frames));
    w.add("segments_open", num(st.fisb_segments_open));
    w.add("mosaic_blocks", num(st.fisb_mosaic_blocks));
    w.add("uplink_duplicates", num(st.fisb_uplink_duplicates));
    if (st.fisb_last_uplink)
        w.add("last_uplink_time", iso(*st.fisb_last_uplink));
    w.add("now", iso(snap.now));
    if (f.radar_age_s)
        w.add("radar_age_s", std::round(*f.radar_age_s * 10) / 10);
    if (f.clock_skew_s)
        w.add("clock_skew_s", std::round(*f.clock_skew_s * 10) / 10);

    std::vector<std::string> stations;
    for (const auto& s : f.heard)
        stations.push_back(JsonWriter()
                               .add("site_id", siteId(s.key))
                               .addRaw("lat", deg(s.lat))
                               .addRaw("lon", deg(s.lon))
                               .add("count", num(s.products))
                               .add("last_seen", iso(s.last_heard))
                               .str());
    w.addRaw("stations", jsonArray(stations));

    // An absent mosaic is left out, not null (the API's convention).
    JsonWriter radar;
    if (const auto* regional = findRadar(f, kRegional))
        radar.addRaw("regional", mosaicHead(*regional, snap.now));
    if (const auto* conus = findRadar(f, kConus))
        radar.addRaw("conus", mosaicHead(*conus, snap.now));
    w.addRaw("radar", radar.str());
    std::vector<std::string> rasters;
    for (const auto& m : f.rasters)
        rasters.push_back(mosaicHead(m, snap.now));
    w.addRaw("rasters", jsonArray(rasters));

    w.addRaw("counts", JsonWriter()
                           .add("metar", num(f.metar))
                           .add("taf", num(f.taf))
                           .add("pirep", num(f.pirep))
                           .add("winds", num(f.winds))
                           .add("other", num(f.other))
                           .add("twgo", num(f.twgo_records - f.twgo_cancelled))
                           .add("cancelled", num(f.twgo_cancelled))
                           .str());
    return w.str();
}

std::string fisbEventJson(const EngineEvent& event)
{
    if (!event.fisb)
        return std::string();
    const FisbNotice& n = *event.fisb;
    JsonWriter w;
    switch (event.kind) {
    case EventKind::fisb_text:
        w.add("type", "fisb_text").add("product", static_cast<int64_t>(n.product)).add("key", n.key);
        if (n.text)
            return w.addRaw("record", fisb::textRecordJson(*n.text)).str();
        if (n.twgo)
            return w.addRaw("record", fisb::twgoRecordJson(*n.twgo)).str();
        return std::string();
    case EventKind::fisb_shape:
        if (!n.twgo)
            return std::string();
        return w.add("type", "fisb_shape")
            .add("product", static_cast<int64_t>(n.product))
            .add("key", n.key)
            .addRaw("features", twgoFeaturesJson(*n.twgo))
            .str();
    case EventKind::fisb_radar:
    case EventKind::fisb_raster: {
        w.add("type", event.kind == EventKind::fisb_radar ? "fisb_radar" : "fisb_raster");
        w.add("product", static_cast<int64_t>(n.product)).add("key", n.key);
        w.add("name", fisb::productName(n.product));
        const std::size_t dash = n.key.find('-');
        if (dash != std::string::npos)
            if (const auto code = parseUnsigned(n.key.substr(dash + 1), 255))
                w.add("altitude_code", static_cast<int64_t>(*code));
        if (n.product_time)
            w.add("product_time", iso(*n.product_time));
        w.add("blocks", num(n.block_count));
        if (n.expires)
            w.add("expires", iso(*n.expires));
        return w.str();
    }
    case EventKind::fisb_expired: {
        const char* kind = expiredKind(n);
        w.add("type", "fisb_expired").add("kind", kind).add("product", static_cast<int64_t>(n.product));
        w.add("key", n.key);
        if (n.text)
            w.add("station", n.text->station);
        w.add("reason", n.reason);
        if (n.expires)
            w.add("expires", iso(*n.expires));
        return w.str();
    }
    case EventKind::fisb_location:
        if (!n.position)
            return std::string();
        return w.add("type", "fisb_location")
            .add("id", n.key)
            .addRaw("lat", deg(n.position->first))
            .addRaw("lon", deg(n.position->second))
            .str();
    default:
        break;
    }
    return std::string();
}

bool isFisbPath(const std::string& path) { return path.compare(0, 10, "/api/fisb/") == 0; }

ApiResponse handleFisbApi(const ApiRequest& request, const Engine& engine)
{
    const std::string& path = request.path;
    enum class Route { status, text, shapes, radar, raster, locations };
    Route route;
    if (path == "/api/fisb/status")
        route = Route::status;
    else if (path == "/api/fisb/text")
        route = Route::text;
    else if (path == "/api/fisb/shapes")
        route = Route::shapes;
    else if (path == "/api/fisb/radar")
        route = Route::radar;
    else if (path == "/api/fisb/raster")
        route = Route::raster;
    else if (path == "/api/fisb/locations")
        route = Route::locations;
    else
        return fail(404, "no such API path: " + path);
    if (request.method != "GET") {
        ApiResponse r = fail(405, "method not allowed");
        r.allow = "GET";
        return r;
    }
    const auto params = queryParams(request.query);
    switch (route) {
    case Route::status: return reply(200, fisbStatusJson(engine.snapshot()));
    // Text and shapes copy the records only, never a block; radar and
    // raster copy only the one mosaic asked for.
    case Route::text: return fisbTextRoute(params, engine.fisbRecords());
    case Route::shapes: return shapesRoute(params, engine.fisbRecords());
    case Route::radar: return radarRoute(params, engine);
    case Route::raster: return rasterRoute(params, engine);
    case Route::locations: return locationsRoute(engine);
    }
    return fail(404, "no such API path: " + path);
}

}  // namespace webapi
}  // namespace skycued
