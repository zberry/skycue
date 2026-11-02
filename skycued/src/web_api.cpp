#include "web_api.hpp"

#include "config.hpp"
#include "engine.hpp"
#include "json_lite.hpp"
#include "recorder.hpp"
#include "runtime_config.hpp"
#include "time_util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

using recording::addressTypeName;
using recording::dropReasonName;
using recording::sourceName;

const char* decisionName(Decision d)
{
    switch (d) {
    case Decision::send: return "send";
    case Decision::blocked: return "blocked";
    case Decision::allow_listed: return "allow_listed";
    case Decision::out_of_range: return "out_of_range";
    case Decision::below_elevation: return "below_elevation";
    case Decision::no_position: return "no_position";
    }
    return "no_position";
}

JsonWriter start(const char* type)
{
    JsonWriter w;
    if (type)
        w.add("type", type);
    return w;
}

void addFinite(JsonWriter& w, const char* key, double v)
{
    if (std::isfinite(v))
        w.add(key, v);
}

void putStats(JsonWriter& w, const EngineStats& s)
{
    auto n = [](uint64_t v) { return static_cast<int64_t>(v); };
    w.add("observations", n(s.observations))
        .add("sent_updates", n(s.sent_updates))
        .add("sent_drops", n(s.sent_drops))
        .add("tracks_total", n(s.tracks_total))
        .add("tracks_forwarded", n(s.tracks_forwarded))
        .add("uat_downlink", n(s.uat_downlink))
        .add("uat_uplink", n(s.uat_uplink))
        .add("readsb_connected", s.readsb_connected)
        .add("dump978_connected", s.dump978_connected)
        .add("tcp_clients", n(s.tcp_clients))
        .add("udp_targets", n(s.udp_targets))
        .add("drops_timeout", n(s.drops_timeout))
        .add("drops_filtered", n(s.drops_filtered))
        .add("drops_source_lost", n(s.drops_source_lost))
        .add("recorder_dropped_received", n(s.recorder_dropped_received))
        .add("recorder_dropped_sent", n(s.recorder_dropped_sent))
        .add("web_clients", static_cast<int64_t>(s.web_clients));
}

void putFilters(JsonWriter& w, const RuntimeConfig& rc)
{
    const Config c = rc.effective();
    w.add("max_range_nm", c.max_range_nm)
        .add("min_elevation_deg", c.min_elevation_deg)
        .add("overlay_active", !rc.overlay().empty());
}

std::string stringArray(const std::vector<std::string>& values)
{
    std::vector<std::string> quoted;
    quoted.reserve(values.size());
    for (const auto& v : values) {
        std::string s;
        appendJsonString(s, v);
        quoted.push_back(std::move(s));
    }
    return jsonArray(quoted);
}

void putLists(JsonWriter& w, const Engine& e, const RuntimeConfig& rc)
{
    w.addRaw("allow", stringArray(e.allowList().entries()))
        .addRaw("block", stringArray(e.blockList().entries()))
        .add("overlay_active", !rc.overlay().empty());
}

template <class T> void addOpt(JsonWriter& w, const char* key, const std::optional<T>& v)
{
    if (v)
        w.add(key, static_cast<int64_t>(*v));
}
void addOpt(JsonWriter& w, const char* key, const std::optional<float>& v)
{
    if (v && std::isfinite(*v))
        w.add(key, static_cast<double>(*v));
}
void addOpt(JsonWriter& w, const char* key, const std::optional<std::string>& v)
{
    if (v)
        w.add(key, *v);
}

void putTrack(JsonWriter& w, const TrackSnapshot& ts)
{
    const skycue::Track& t = ts.track;
    w.add("hex", hexAddress(t.id.address))
        .add("addr_type", addressTypeName(t.id.type))
        .add("source", sourceName(t.source));
    if (ts.decision != Decision::no_position) {
        w.addExact("lat", t.lat_deg).addExact("lon", t.lon_deg);
        w.add("position_time", formatIso8601Ms(t.position_time));
    }
    addOpt(w, "alt_baro_ft", t.alt_baro_ft);
    addOpt(w, "alt_geom_ft", t.alt_geom_ft);
    addOpt(w, "gs_kt", t.ground_speed_kt);
    addOpt(w, "track_deg", t.track_deg);
    addOpt(w, "vr_fpm", t.vertical_rate_fpm);
    addOpt(w, "callsign", t.callsign);
    addOpt(w, "squawk", t.squawk);
    addOpt(w, "registration", t.registration);
    addOpt(w, "nic", t.nic);
    addOpt(w, "nac_p", t.nac_p);
    w.add("on_ground", t.on_ground).add("alt_is_geom", t.alt_is_geom);
    addFinite(w, "range_nm", ts.range_nm);
    addFinite(w, "elevation_deg", ts.elevation_deg);
    w.add("decision", decisionName(ts.decision))
        .add("forwarded", ts.forwarded)
        .add("first_seen", formatIso8601Ms(ts.first_seen))
        .add("last_seen", formatIso8601Ms(ts.last_seen));
    addOpt(w, "emergency", ts.emergency);
    addOpt(w, "category", ts.category);
    addFinite(w, "rssi", ts.rssi);
}

Response json(unsigned status, std::string body)
{
    Response r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

Response error(unsigned status, const std::string& message) { return json(status, errorJson(message)); }

Response notAllowed(const char* allow)
{
    Response r = error(405, "method not allowed");
    r.allow = allow;
    return r;
}

// Applies `delta` from the UI; the reply is `ok()` or the error.
template <class Ok> Response apply(RuntimeConfig& rc, const Overlay& delta, Ok ok)
{
    try {
        rc.applyUi(delta, "ui");
    } catch (const std::invalid_argument& e) {
        return error(400, e.what());
    } catch (const ConfigError& e) {
        return error(400, e.what());
    } catch (const std::exception& e) {
        return error(500, e.what());
    }
    return json(200, ok());
}

Response putFiltersRoute(const std::string& body, RuntimeConfig& rc)
{
    const auto obj = JsonObject::parse(body);
    if (!obj)
        return error(400, "the body must be a JSON object");
    for (const auto& k : obj->keys())
        if (k != "max_range_nm" && k != "min_elevation_deg")
            return error(400, "unknown key '" + k + "'");
    Overlay delta;
    for (const char* k : {"max_range_nm", "min_elevation_deg"}) {
        if (!obj->has(k))
            continue;
        const bool range = std::strcmp(k, "max_range_nm") == 0;
        // null: back to the baseline value. (PropertyTree keeps null as the
        // text "null", so the string "null" reads the same.)
        if (obj->getString(k).value_or("") == "null") {
            (range ? delta.clear_max_range_nm : delta.clear_min_elevation_deg) = true;
            continue;
        }
        const auto v = obj->getDouble(k);
        if (!v)
            return error(400, std::string(k) + " must be a number or null");
        (range ? delta.max_range_nm : delta.min_elevation_deg) = *v;
    }
    return apply(rc, delta, [&] { return filtersJson(rc); });
}

Response postListRoute(ListKind kind, const std::string& body, const Engine& e, RuntimeConfig& rc)
{
    const auto obj = JsonObject::parse(body);
    if (!obj)
        return error(400, "the body must be a JSON object");
    for (const auto& k : obj->keys())
        if (k != "add" && k != "remove")
            return error(400, "unknown key '" + k + "'");
    Overlay delta;
    const bool allow = kind == ListKind::allow;
    for (const char* k : {"add", "remove"}) {
        if (!obj->has(k))
            continue;
        auto v = obj->getStringArray(k);
        if (!v)
            return error(400, std::string(k) + " must be an array of entries");
        const bool add = std::strcmp(k, "add") == 0;
        (add ? (allow ? delta.allow_add : delta.block_add) : (allow ? delta.allow_remove : delta.block_remove)) =
            std::move(*v);
    }
    return apply(rc, delta, [&] { return listsJson(e, rc); });
}

bool isDirectory(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A small file's text; nullopt when it cannot be read or is larger.
std::optional<std::string> readSmallFile(const std::string& path, size_t max_bytes)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0 || text.size() + static_cast<size_t>(n) > max_bytes) {
            ::close(fd);
            if (n == 0)
                return text;
            return std::nullopt;
        }
        text.append(buf, static_cast<size_t>(n));
    }
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string stateJson(const Engine& engine, const RuntimeConfig& runtime, const char* type)
{
    const Snapshot snap = engine.snapshot();
    const Config c = runtime.effective();
    JsonWriter w = start(type);
    w.add("now", formatIso8601Ms(snap.now));
    w.addRaw("receiver", JsonWriter()
                             .add("lat", c.receiver_lat)
                             .add("lon", c.receiver_lon)
                             .add("alt_m", c.receiver_alt_m)
                             .add("antenna_m", c.antenna_height_m)
                             .str());
    w.addRaw("filters", filtersJson(runtime));
    w.addRaw("lists", listsJson(engine, runtime));
    w.add("trail_seconds", static_cast<int64_t>(c.trail_seconds));
    w.addRaw("stats", statsJson(snap.stats));
    std::string tracks = "[";
    for (const auto& t : snap.tracks) {
        if (tracks.size() > 1)
            tracks += ',';
        tracks += trackJson(t);
    }
    tracks += ']';
    w.addRaw("tracks", tracks);
    return w.str();
}

std::string statsJson(const EngineStats& stats, const char* type)
{
    JsonWriter w = start(type);
    putStats(w, stats);
    return w.str();
}

std::string filtersJson(const RuntimeConfig& runtime, const char* type)
{
    JsonWriter w = start(type);
    putFilters(w, runtime);
    return w.str();
}

std::string listsJson(const Engine& engine, const RuntimeConfig& runtime, const char* type)
{
    JsonWriter w = start(type);
    putLists(w, engine, runtime);
    return w.str();
}

std::string trackJson(const TrackSnapshot& track, const char* type)
{
    JsonWriter w = start(type);
    putTrack(w, track);
    return w.str();
}

std::string dropJson(skycue::TrackId id, skycue::DropReason reason)
{
    return JsonWriter()
        .add("type", "drop")
        .add("hex", hexAddress(id.address))
        .add("addr_type", addressTypeName(id.type))
        .add("reason", dropReasonName(reason))
        .str();
}

std::string eventJson(const EngineEvent& event)
{
    switch (event.kind) {
    case EventKind::update:
        return event.track ? trackJson(*event.track, "update") : std::string();
    case EventKind::observed:
        return event.track ? trackJson(*event.track, "observed") : std::string();
    case EventKind::drop:
        return event.id && event.reason ? dropJson(*event.id, *event.reason) : std::string();
    case EventKind::stats:
        return event.stats ? statsJson(*event.stats, "stats") : std::string();
    case EventKind::filters_changed:
    case EventKind::lists_changed:
        break;
    }
    return std::string();
}

std::string tilesAvailableJson(const std::string& www_dir)
{
    const std::string tiles = www_dir + "/tiles";
    if (const auto text = readSmallFile(tiles + "/meta.json", 64 * 1024)) {
        if (const auto meta = JsonObject::parse(*text)) {
            JsonWriter w;
            w.add("tiles", true);
            for (const char* k : {"minZoom", "maxZoom"})
                if (const auto z = meta->getInt(k))
                    w.add(k, *z);
            return w.str();
        }
    }
    // No (usable) meta.json: the zoom directories tiles/<z>/.
    int min_z = -1, max_z = -1;
    if (DIR* d = ::opendir(tiles.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name.size() > 2 ||
                !std::all_of(name.begin(), name.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                continue;
            if (!isDirectory(tiles + "/" + name))
                continue;
            const int z = std::stoi(name);
            min_z = min_z < 0 ? z : std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
        ::closedir(d);
    }
    if (min_z < 0)
        return JsonWriter().add("tiles", false).str();
    return JsonWriter()
        .add("tiles", true)
        .add("minZoom", static_cast<int64_t>(min_z))
        .add("maxZoom", static_cast<int64_t>(max_z))
        .str();
}

std::string errorJson(const std::string& message) { return JsonWriter().add("error", message).str(); }

bool isApiPath(const std::string& path) { return path == "/api" || path.compare(0, 5, "/api/") == 0; }

Response handleApi(const std::string& method, const std::string& path, const std::string& body, Engine& engine,
                   RuntimeConfig& runtime, const std::string& www_dir)
{
    try {
        const bool get = method == "GET";
        if (path == "/api/state")
            return get ? json(200, stateJson(engine, runtime)) : notAllowed("GET");
        if (path == "/api/stats")
            return get ? json(200, statsJson(engine.stats())) : notAllowed("GET");
        if (path == "/api/filters") {
            if (get)
                return json(200, filtersJson(runtime));
            return method == "PUT" ? putFiltersRoute(body, runtime) : notAllowed("GET, PUT");
        }
        if (path == "/api/filters/save-baseline") {
            if (method != "POST")
                return notAllowed("POST");
            try {
                const std::string unsaved = runtime.promoteToBaseline();
                return json(200, JsonWriter().add("ok", true).add("unsaved", unsaved).str());
            } catch (const std::exception& e) {
                return error(500, e.what());
            }
        }
        if (path == "/api/lists")
            return get ? json(200, listsJson(engine, runtime)) : notAllowed("GET");
        if (path == "/api/lists/allow" || path == "/api/lists/block") {
            if (method != "POST")
                return notAllowed("POST");
            return postListRoute(path == "/api/lists/allow" ? ListKind::allow : ListKind::block, body, engine,
                                 runtime);
        }
        if (path == "/api/tiles-available")
            return get ? json(200, tilesAvailableJson(www_dir)) : notAllowed("GET");
        return error(404, "no such API path: " + path);
    } catch (const std::exception& e) {
        return error(500, e.what());
    }
}

std::optional<std::string> staticPath(const std::string& target)
{
    const std::string raw = target.substr(0, target.find_first_of("?#"));
    if (raw.empty() || raw[0] != '/')
        return std::nullopt;
    std::string path;
    path.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '%') {
            if (i + 2 >= raw.size())
                return std::nullopt;
            const int hi = hexValue(raw[i + 1]), lo = hexValue(raw[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (c == '\0' || c == '\\')
            return std::nullopt;
        path += c;
    }
    if (path == "/")
        return std::string("/index.html");
    // Every segment after the leading '/': non-empty, not "." or "..".
    size_t begin = 1;
    for (;;) {
        const size_t end = path.find('/', begin);
        const std::string seg = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (seg.empty() || seg == "." || seg == "..")
            return std::nullopt;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return path;
}

const char* contentType(const std::string& path)
{
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
        return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == "html" || ext == "htm")
        return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs")
        return "application/javascript";
    if (ext == "css")
        return "text/css";
    if (ext == "png")
        return "image/png";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "json" || ext == "map")
        return "application/json";
    if (ext == "ico")
        return "image/x-icon";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "webp")
        return "image/webp";
    if (ext == "woff2")
        return "font/woff2";
    if (ext == "txt" || ext == "md")
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

const char* cacheControl(const std::string& path)
{
    if (path.compare(0, 7, "/tiles/") == 0 || path.compare(0, 8, "/vendor/") == 0)
        return "max-age=86400";
    return "no-cache";
}

}  // namespace webapi
}  // namespace skycued
