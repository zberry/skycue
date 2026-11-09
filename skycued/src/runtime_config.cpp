#include "runtime_config.hpp"

#include "engine.hpp"
#include "json_lite.hpp"
#include "log.hpp"
#include "toml_lite.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace skycued {

namespace {

constexpr std::chrono::seconds kPollInterval{1};
constexpr std::size_t kMaxOverlayBytes = 4 * 1024 * 1024;
const char* const kOverlayName = "skycue.runtime.json";

bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b]))
        ++b;
    while (e > b && isSpace(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

std::string upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string baseName(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirOf(const std::string& path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

// The list file grammar's comment rule (filters.cpp): '#' at the start of the
// line or after whitespace.
std::string stripComment(const std::string& line)
{
    for (size_t i = 0; i < line.size(); ++i)
        if (line[i] == '#' && (i == 0 || isSpace(line[i - 1])))
            return line.substr(0, i);
    return line;
}

bool containsCi(const std::vector<std::string>& v, const std::string& entry)
{
    const std::string key = upper(entry);
    return std::any_of(v.begin(), v.end(), [&](const std::string& e) { return upper(e) == key; });
}

bool eraseCi(std::vector<std::string>& v, const std::string& entry)
{
    const std::string key = upper(entry);
    const auto before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& e) { return upper(e) == key; }), v.end());
    return v.size() != before;
}

// One entry, trimmed and checked by the list grammar. Throws
// std::invalid_argument("bad entry '<text>'").
std::string normalise(const std::string& entry)
{
    IdList one;
    one.add(entry);
    return one.entries().front();
}

std::vector<std::string> normaliseAll(const std::vector<std::string>& entries)
{
    std::vector<std::string> out;
    for (const auto& e : entries) {
        std::string n = normalise(e);
        if (!containsCi(out, n))
            out.push_back(n);
    }
    return out;
}

void checkFilters(std::optional<double> range, std::optional<double> elevation)
{
    if (range && !(std::isfinite(*range) && *range > 0))
        throw std::invalid_argument("max_range_nm must be greater than 0");
    if (elevation && !(*elevation >= -90 && *elevation <= 90))
        throw std::invalid_argument("min_elevation_deg must be in [-90,90]");
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(path + ": cannot open: " + std::strerror(errno));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw std::runtime_error(path + ": read error");
    return ss.str();
}

IdList loadList(const std::string& path) { return path.empty() ? IdList() : IdList::loadFile(path); }

// In paths.state_dir when it is set, else next to skycue.toml.
std::string overlayPathFor(const std::string& conf_path, const std::string& state_dir)
{
    const std::string dir = state_dir.empty() ? dirOf(conf_path) : state_dir;
    return (dir == "/" ? dir : dir + "/") + kOverlayName;
}

// The list file text with `remove` entries commented out and `add` entries
// appended (those not already listed), keeping every other byte.
std::string rewriteList(const std::string& text, const std::vector<std::string>& add,
                        const std::vector<std::string>& remove)
{
    const size_t first_nl = text.find('\n');
    const std::string eol = first_nl != std::string::npos && first_nl > 0 && text[first_nl - 1] == '\r' ? "\r\n" : "\n";
    std::string out;
    std::vector<std::string> kept;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl + 1;
        size_t content_end = nl == std::string::npos ? text.size() : nl;
        if (content_end > pos && text[content_end - 1] == '\r')
            --content_end;
        const std::string content = text.substr(pos, content_end - pos);
        const std::string entry = trim(stripComment(content));
        if (!entry.empty() && containsCi(remove, entry)) {
            size_t b = 0;
            while (b < content.size() && isSpace(content[b]))
                ++b;
            out += "# removed by skycue save-baseline: " + content.substr(b);
            out.append(text, content_end, end - content_end);
        } else {
            if (!entry.empty())
                kept.push_back(entry);
            out.append(text, pos, end - pos);
        }
        pos = end;
    }
    for (const auto& e : add) {
        if (containsCi(kept, e))
            continue;
        if (!out.empty() && out.back() != '\n')
            out += eol;
        out += e + eol;
        kept.push_back(e);
    }
    return out;
}

// have -> want as entry additions and removals, ignoring case and order.
void listDiff(const IdList& have, const IdList& want, std::vector<std::string>& add, std::vector<std::string>& remove)
{
    for (const auto& e : want.entries())
        if (!containsCi(have.entries(), e))
            add.push_back(e);
    for (const auto& e : have.entries())
        if (!containsCi(want.entries(), e))
            remove.push_back(e);
}

void appendArray(std::string& out, const std::vector<std::string>& v)
{
    out += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i)
            out += ", ";
        appendJsonString(out, v[i]);
    }
    out += ']';
}

}  // namespace

// ---- the restart keys ---------------------------------------------------------------

namespace {

struct RestartKey {
    const char* key;
    bool (*differs)(const Config& a, const Config& b);
};

#define SKYCUED_RESTART_KEY(KEY, field) {KEY, [](const Config& a, const Config& b) { return a.field != b.field; }}

// How to tell that a key differs between two Configs. Which keys count as
// restart keys is the class table's (keySpecs()): every key not live.
constexpr RestartKey kRestartKeys[] = {
    SKYCUED_RESTART_KEY("receiver.lat", receiver_lat),
    SKYCUED_RESTART_KEY("receiver.lon", receiver_lon),
    SKYCUED_RESTART_KEY("receiver.alt_m", receiver_alt_m),
    SKYCUED_RESTART_KEY("receiver.antenna_height_m", antenna_height_m),
    SKYCUED_RESTART_KEY("sdr.serial_1090", sdr_1090_serial),
    SKYCUED_RESTART_KEY("sdr.serial_978", sdr_978_serial),
    SKYCUED_RESTART_KEY("sdr.gain_1090", gain_1090),
    SKYCUED_RESTART_KEY("sdr.gain_978", gain_978),
    SKYCUED_RESTART_KEY("inputs.dump978.raw_port", dump978_raw_port),
    SKYCUED_RESTART_KEY("inputs.dump978.json_port", dump978_json_port),
    SKYCUED_RESTART_KEY("inputs.readsb.json_port", readsb_json_port),
    SKYCUED_RESTART_KEY("inputs.readsb.beast_port", readsb_beast_port),
    SKYCUED_RESTART_KEY("inputs.decoder", track_decoder),
    SKYCUED_RESTART_KEY("inputs.readsb.host", readsb_host),
    SKYCUED_RESTART_KEY("inputs.dump978.host", dump978_host),
    SKYCUED_RESTART_KEY("inputs.readsb.enable", inputs_readsb_enable),
    SKYCUED_RESTART_KEY("inputs.dump978.enable", dump978_enable),
    SKYCUED_RESTART_KEY("web.port", web_port),
    SKYCUED_RESTART_KEY("cues.tcp_port", cue_tcp_port),
    SKYCUED_RESTART_KEY("cues.tcp_connect", cue_tcp_connect_targets),
    SKYCUED_RESTART_KEY("cues.udp_targets", cue_udp_targets),
    SKYCUED_RESTART_KEY("recording.dir", record_dir),
    SKYCUED_RESTART_KEY("recording.formats", record_formats),
    SKYCUED_RESTART_KEY("cues.min_interval_s", min_interval_s),
    SKYCUED_RESTART_KEY("cues.max_interval_s", max_interval_s),
    SKYCUED_RESTART_KEY("cues.track_timeout_s", track_timeout_s),
    SKYCUED_RESTART_KEY("cues.keyframe_interval_s", keyframe_interval_s),
    SKYCUED_RESTART_KEY("recording.raw", record_raw),
    SKYCUED_RESTART_KEY("web.bind", web_bind),
    SKYCUED_RESTART_KEY("recording.max_gb", record_max_gb),
    SKYCUED_RESTART_KEY("web.trail_seconds", trail_seconds),
    SKYCUED_RESTART_KEY("web.allow_restart", web_allow_restart),
    SKYCUED_RESTART_KEY("fisb.enable", fisb_enable),
    SKYCUED_RESTART_KEY("fisb.experimental", fisb_experimental),
    SKYCUED_RESTART_KEY("paths.state_dir", state_dir),
    SKYCUED_RESTART_KEY("paths.map_dir", map_dir),
    SKYCUED_RESTART_KEY("inputs.file", inputs_file),
    SKYCUED_RESTART_KEY("paths.locations_file", locations_file),
    SKYCUED_RESTART_KEY("stats.file", stats_file),
    SKYCUED_RESTART_KEY("stats.aircraft_max", stats_aircraft_max),
    SKYCUED_RESTART_KEY("stats.save_s", stats_save_s),
};

#undef SKYCUED_RESTART_KEY

}  // namespace

const std::vector<std::string>& restartConfigKeys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> out;
        for (const auto& s : keySpecs())
            if (s.cls != KeyClass::live)
                out.emplace_back(s.key);
        return out;
    }();
    return keys;
}

std::vector<std::string> restartKeysChanged(const Config& a, const Config& b)
{
    std::vector<std::string> out;
    for (const auto& key : restartConfigKeys()) {
        const auto k = std::find_if(std::begin(kRestartKeys), std::end(kRestartKeys),
                                    [&](const RestartKey& r) { return key == r.key; });
        if (k != std::end(kRestartKeys) && k->differs(a, b))
            out.emplace_back(key);
    }
    return out;
}

// ---- Overlay ----------------------------------------------------------------------

bool Overlay::empty() const
{
    return !max_range_nm && !min_elevation_deg && allow_add.empty() && allow_remove.empty() && block_add.empty() &&
           block_remove.empty();
}

bool Overlay::operator==(const Overlay& o) const
{
    return max_range_nm == o.max_range_nm && min_elevation_deg == o.min_elevation_deg && allow_add == o.allow_add &&
           allow_remove == o.allow_remove && block_add == o.block_add && block_remove == o.block_remove;
}

Overlay parseOverlay(const std::string& text, std::vector<std::string>* warnings)
{
    auto warn = [&](const std::string& w) {
        if (warnings)
            warnings->push_back(w);
    };
    auto unknownKeys = [&](const JsonObject& obj, const std::set<std::string>& known, const std::string& where) {
        for (const auto& k : obj.keys())
            if (!known.count(k))
                warn("unknown key '" + where + k + "' ignored");
    };
    const auto root = JsonObject::parse(text, kMaxOverlayBytes);
    if (!root)
        throw std::runtime_error("not a JSON object");
    unknownKeys(*root, {"version", "filters", "lists"}, "");
    if (root->has("version") && root->getInt("version") != int64_t{1})
        throw std::runtime_error("version must be 1");

    Overlay o;
    if (root->has("filters")) {
        const auto filters = root->getObject("filters");
        if (!filters)
            throw std::runtime_error("filters must be an object");
        unknownKeys(*filters, {"max_range_nm", "min_elevation_deg"}, "filters.");
        auto number = [&](const char* key) -> std::optional<double> {
            if (!filters->has(key) || filters->getString(key) == std::string("null"))
                return std::nullopt;
            const auto v = filters->getDouble(key);
            if (!v)
                throw std::runtime_error(std::string("filters.") + key + " must be a number");
            return v;
        };
        o.max_range_nm = number("max_range_nm");
        o.min_elevation_deg = number("min_elevation_deg");
        try {
            checkFilters(o.max_range_nm, o.min_elevation_deg);
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error(std::string("filters.") + e.what());
        }
    }
    if (root->has("lists")) {
        const auto lists = root->getObject("lists");
        if (!lists)
            throw std::runtime_error("lists must be an object");
        unknownKeys(*lists, {"allow", "block"}, "lists.");
        for (const char* name : {"allow", "block"}) {
            if (!lists->has(name))
                continue;
            const auto list = lists->getObject(name);
            const std::string where = std::string("lists.") + name;
            if (!list)
                throw std::runtime_error(where + " must be an object");
            unknownKeys(*list, {"add", "remove"}, where + ".");
            for (const char* part : {"add", "remove"}) {
                if (!list->has(part))
                    continue;
                const auto entries = list->getStringArray(part);
                if (!entries)
                    throw std::runtime_error(where + "." + part + " must be an array of strings");
                std::vector<std::string> normalised;
                try {
                    normalised = normaliseAll(*entries);
                } catch (const std::invalid_argument& e) {
                    throw std::runtime_error(where + "." + part + ": " + e.what());
                }
                const bool allow = std::strcmp(name, "allow") == 0, add = std::strcmp(part, "add") == 0;
                (allow ? (add ? o.allow_add : o.allow_remove) : (add ? o.block_add : o.block_remove)) =
                    std::move(normalised);
            }
        }
    }
    return o;
}

std::string overlayJson(const Overlay& o)
{
    std::string out = "{\n  \"version\": 1,\n  \"filters\": {";
    bool first = true;
    auto num = [&](const char* key, const std::optional<double>& v) {
        if (!v)
            return;
        if (!first)
            out += ", ";
        first = false;
        appendJsonString(out, key);
        out += ": " + formatRoundTrip(*v);
    };
    num("max_range_nm", o.max_range_nm);
    num("min_elevation_deg", o.min_elevation_deg);
    out += "},\n  \"lists\": {\n    \"allow\": {\"add\": ";
    appendArray(out, o.allow_add);
    out += ", \"remove\": ";
    appendArray(out, o.allow_remove);
    out += "},\n    \"block\": {\"add\": ";
    appendArray(out, o.block_add);
    out += ", \"remove\": ";
    appendArray(out, o.block_remove);
    out += "}\n  }\n}\n";
    return out;
}

// ---- RuntimeConfig -----------------------------------------------------------------

RuntimeConfig::RuntimeConfig(boost::asio::io_context& io, std::string conf_path, Engine& engine)
    : conf_path_(std::move(conf_path)),
      overlay_path_(overlayPathFor(conf_path_, "")),
      engine_(engine),
      env_(environmentOverrides()),
      conf_watch_(conf_path_),
      overlay_watch_(overlay_path_),
      timer_(io)
{
}

// NOLINTNEXTLINE(bugprone-exception-escape): a throw here is a broken process (asio's throwing cancel or close, a failed join), and terminate is the right end for it
RuntimeConfig::~RuntimeConfig()
{
    alive_.reset();
    timer_.cancel();
}

const std::string& RuntimeConfig::listPath(ListKind kind) const
{
    return kind == ListKind::allow ? file_conf_.allow_list_path : file_conf_.block_list_path;
}

void RuntimeConfig::load()
{
    // Stamps before reading: an edit in between is then seen again, never missed.
    conf_watch_.accept();
    // skycue.toml, the environment and --set say where the overrides file is
    // (paths.state_dir, from the grammar pass only: the main file's values may
    // be valid only with the overrides); then all are read once the overrides'
    // stamp is taken. Again if paths.state_dir was edited in between.
    std::string overrides = skycued::overridesPathOf(conf_path_, set_args_, env_);
    for (;;) {
        overrides_watch_ = SettledFile(overrides);
        overrides_watch_.accept();
        file_conf_ = parseConfigFile(conf_path_, overrides, set_args_, env_);
        if (skycued::overridesPath(file_conf_) == overrides)
            break;
        overrides = skycued::overridesPath(file_conf_);
    }
    files_only_ = file_conf_;
    if (!file_conf_.sources.empty()) {
        try {
            files_only_ = parseConfigFile(conf_path_, overrides, {}, {});
        } catch (const std::exception& e) {
            // Valid only with the environment or --set: nothing to compare edits with.
            LOG_DEBUG("runtime: the files alone are not a valid configuration: " << e.what());
        }
    }
    running_ = file_conf_;
    // The overlay's directory follows paths.state_dir as parsed now (a restart key).
    overlay_path_ = overlayPathFor(conf_path_, running_.state_dir);
    overlay_watch_ = SettledFile(overlay_path_);
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        listWatch(kind) = SettledFile(listPath(kind));
        listWatch(kind).accept();
        fileList(kind) = loadList(listPath(kind));
    }
    overlay_watch_.accept();
    overlay_ = Overlay();
    // An install that moved to paths.state_dir: the overlay next to the config is
    // no longer read, and its changes look lost.
    const std::string beside_conf = overlayPathFor(conf_path_, "");
    if (!overlay_watch_.stamp().exists && beside_conf != overlay_path_ && FileStamp::of(beside_conf).exists)
        LOG_WARN(beside_conf << ": not read: paths.state_dir is set, so the overlay is " << overlay_path_
                             << " (move the file there to keep its changes)");
    if (overlay_watch_.stamp().exists) {
        try {
            std::vector<std::string> warnings;
            overlay_ = parseOverlay(readFile(overlay_path_), &warnings);
            for (const auto& w : warnings)
                LOG_WARN(overlay_path_ << ": " << w);
        } catch (const std::exception& e) {
            // Moved aside, so the next change from the UI does not overwrite it.
            const std::string rejected = overlay_path_ + ".rejected";
            if (::rename(overlay_path_.c_str(), rejected.c_str()) == 0)
                LOG_ERROR(overlay_path_ << ": " << e.what() << "; ignoring it (the baseline applies), moved to "
                                        << rejected);
            else
                LOG_ERROR(overlay_path_ << ": " << e.what() << "; ignoring it (the baseline applies); cannot move it to "
                                        << rejected << ": " << std::strerror(errno));
            overlay_watch_.accept();
        }
    }
    applyEffective("startup");
}

FilterConfig RuntimeConfig::effectiveFilters() const
{
    // A value pinned by the environment or --set stays; the overlay never applies above it.
    const bool pin_range = !pinnedBy("filters.max_range_nm").empty(),
               pin_elev = !pinnedBy("filters.min_elevation_deg").empty();
    return FilterConfig{pin_range ? running_.max_range_nm : overlay_.max_range_nm.value_or(running_.max_range_nm),
                        pin_elev ? running_.min_elevation_deg
                                 : overlay_.min_elevation_deg.value_or(running_.min_elevation_deg)};
}

IdList RuntimeConfig::effectiveList(ListKind kind) const
{
    const bool allow = kind == ListKind::allow;
    IdList list = fileList(kind);
    for (const auto& e : allow ? overlay_.allow_remove : overlay_.block_remove)
        list.remove(e);
    for (const auto& e : allow ? overlay_.allow_add : overlay_.block_add)
        list.add(e);
    return list;
}

Config RuntimeConfig::effective() const
{
    Config c = running_;
    const FilterConfig f = effectiveFilters();
    c.max_range_nm = f.max_range_nm;
    c.min_elevation_deg = f.min_elevation_deg;
    return c;
}

void RuntimeConfig::changed()
{
    if (on_change_)
        on_change_();
}

bool RuntimeConfig::applyEffective(const std::string& origin)
{
    const FilterConfig want = effectiveFilters();
    const FilterConfig have = engine_.snapshot().filters;
    bool filters = false;
    if (want.max_range_nm != have.max_range_nm || want.min_elevation_deg != have.min_elevation_deg) {
        engine_.setFilterConfig(want, origin);
        filters = true;
    }

    IdList allow = effectiveList(ListKind::allow), block = effectiveList(ListKind::block);
    std::vector<std::string> allow_add, allow_remove, block_add, block_remove;
    listDiff(engine_.allowList(), allow, allow_add, allow_remove);
    listDiff(engine_.blockList(), block, block_add, block_remove);
    const size_t changes = allow_add.size() + allow_remove.size() + block_add.size() + block_remove.size();
    if (changes == 0)
        return filters;
    if (changes > 1) {
        engine_.setLists(std::move(allow), std::move(block), origin);
        return true;
    }
    // One entry: the record names it.
    if (!allow_add.empty())
        engine_.addListEntry(ListKind::allow, allow_add.front(), origin);
    else if (!allow_remove.empty())
        engine_.removeListEntry(ListKind::allow, allow_remove.front(), origin);
    else if (!block_add.empty())
        engine_.addListEntry(ListKind::block, block_add.front(), origin);
    else
        engine_.removeListEntry(ListKind::block, block_remove.front(), origin);
    return true;
}

void RuntimeConfig::writeFile(SettledFile& file, const std::string& text)
{
    atomicWrite(file.path(), text);
    file.accept();
}

namespace {

// "2026-11-08T12:00:00Z"
std::string isoUtcNow()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    return std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0 ? std::string() : std::string(buf);
}

}  // namespace

void RuntimeConfig::writeOverrides(const std::map<std::string, toml_lite::Value>& set,
                                   const std::set<std::string>& clear)
{
    const std::string& path = overrides_watch_.path();
    std::map<std::string, toml_lite::Value> values;
    if (FileStamp::of(path).exists) {
        // The keys it holds already stay; one that is not valid TOML is not
        // overwritten blind.
        for (const auto& [key, entry] : toml_lite::parse(readFile(path), path).entries)
            if (std::find(overridesFileKeys().begin(), overridesFileKeys().end(), key) != overridesFileKeys().end())
                values[key] = entry.value;
    }
    for (const auto& key : clear)
        values.erase(key);
    for (const auto& [key, value] : set)
        values[key] = value;
    const std::string header = "# written by skycued ";
    std::string out = header + isoUtcNow() +
                      "; settings saved from the web UI; filter keys apply live, the rest on restart\n";
    const size_t body_at = out.size();
    // By table, in keySpecs() order.
    std::string table;
    for (const auto& spec : keySpecs()) {
        const auto it = values.find(spec.key);
        if (it == values.end())
            continue;
        const std::string key = spec.key;
        const auto dot = key.rfind('.');
        const std::string t = key.substr(0, dot);
        if (t != table) {
            out += "\n[" + t + "]\n";
            table = t;
        }
        out += key.substr(dot + 1) + " = " + toml_lite::formatValue(it->second) + "\n";
    }
    // Never write a file skycued would refuse.
    parseConfigLayers(readFile(conf_path_), conf_path_, out, path, set_args_, env_);
    // Nothing changes: leave the file (or its absence) alone.
    std::string body = out.substr(body_at);
    if (FileStamp::of(path).exists) {
        std::string now = readFile(path);
        if (now.compare(0, header.size(), header) == 0) {
            const auto nl = now.find('\n');
            now = nl == std::string::npos ? std::string() : now.substr(nl + 1);
        }
        if (now == body)
            return;
    } else if (body.empty()) {
        return;
    }
    writeFile(overrides_watch_, out);
}

void RuntimeConfig::writeOverlay(const Overlay& overlay)
{
    if (overlay.empty()) {
        if (::unlink(overlay_path_.c_str()) != 0 && errno != ENOENT)
            throw std::runtime_error(overlay_path_ + ": cannot delete: " + std::strerror(errno));
        overlay_watch_.accept();
        return;
    }
    writeFile(overlay_watch_, overlayJson(overlay));
}

std::string RuntimeConfig::pinnedBy(const std::string& key) const
{
    for (const auto& s : running_.sources)
        if (s.key == key && (s.source == "env" || s.source == "--set"))
            return s.source == "env" ? "env" : "set";
    return "";
}

void RuntimeConfig::applyUi(const Overlay& delta, const std::string& origin)
{
    for (const auto& [key, touched] :
         {std::pair<const char*, bool>{"filters.max_range_nm", delta.max_range_nm || delta.clear_max_range_nm},
          {"filters.min_elevation_deg", delta.min_elevation_deg || delta.clear_min_elevation_deg}})
        if (touched && !pinnedBy(key).empty())
            throw PinnedKeyError(key, pinnedBy(key));
    checkFilters(delta.max_range_nm, delta.min_elevation_deg);
    if ((delta.clear_max_range_nm && delta.max_range_nm) || (delta.clear_min_elevation_deg && delta.min_elevation_deg))
        throw std::invalid_argument("a filter value cannot be both set and cleared");
    // Every entry is checked before anything changes.
    const auto allow_add = normaliseAll(delta.allow_add), allow_remove = normaliseAll(delta.allow_remove);
    const auto block_add = normaliseAll(delta.block_add), block_remove = normaliseAll(delta.block_remove);

    Overlay next = overlay_;
    if (delta.max_range_nm)
        next.max_range_nm = delta.max_range_nm;
    if (delta.min_elevation_deg)
        next.min_elevation_deg = delta.min_elevation_deg;
    if (delta.clear_max_range_nm)
        next.max_range_nm.reset();
    if (delta.clear_min_elevation_deg)
        next.min_elevation_deg.reset();
    auto merge = [](const IdList& file, std::vector<std::string>& add, std::vector<std::string>& remove,
                    const std::vector<std::string>& to_add, const std::vector<std::string>& to_remove) {
        for (const auto& e : to_add) {
            eraseCi(remove, e);
            if (!containsCi(file.entries(), e) && !containsCi(add, e))
                add.push_back(e);
        }
        for (const auto& e : to_remove) {
            eraseCi(add, e);
            if (containsCi(file.entries(), e) && !containsCi(remove, e))
                remove.push_back(e);
        }
    };
    merge(allow_file_, next.allow_add, next.allow_remove, allow_add, allow_remove);
    merge(block_file_, next.block_add, next.block_remove, block_add, block_remove);

    if (next != overlay_) {
        writeOverlay(next);
        overlay_ = std::move(next);
    }
    if (!applyEffective(origin))
        changed();
}

ConfigApplyResult RuntimeConfig::applyConfig(const std::map<std::string, toml_lite::Value>& set,
                                             const std::set<std::string>& clear, const std::string& origin)
{
    std::set<std::string> touched(clear);
    for (const auto& kv : set)
        touched.insert(kv.first);
    for (const auto& key : touched)
        keySpec(key);  // std::invalid_argument naming an unknown key
    for (const auto& key : touched)
        if (keyClassOf(key) == KeyClass::bootstrap)
            throw ReadOnlyKeyError(key);
    for (const auto& key : touched)
        if (!pinnedBy(key).empty())
            throw PinnedKeyError(key, pinnedBy(key));
    for (const auto& key : clear)
        if (set.count(key))
            throw std::invalid_argument(key + ": cannot be both set and cleared");

    const Config before = file_conf_;
    try {
        writeOverrides(set, clear);
    } catch (const ConfigError& e) {
        // "<file>[:<line>]: <message>" -> "<message>" (a key name leads it).
        const std::string what = e.what();
        const auto cut = what.find(": ");
        throw std::invalid_argument(cut == std::string::npos ? what : what.substr(cut + 2));
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(std::string(e.what()) + "; the overrides file is " + overrides_watch_.path() +
                                 ", in paths.state_dir when that is set (set paths.state_dir to a directory"
                                 " skycued can write)");
    }
    reloadConf(kOverridesFileName, true);

    // A filter key set here no longer has a runtime overlay value above it.
    Overlay next = overlay_;
    if (touched.count("filters.max_range_nm"))
        next.max_range_nm.reset();
    if (touched.count("filters.min_elevation_deg"))
        next.min_elevation_deg.reset();
    if (next != overlay_) {
        writeOverlay(next);
        overlay_ = std::move(next);
    }

    ConfigApplyResult result;
    for (const auto& key : touched) {
        if (configValueText(before, key) == configValueText(file_conf_, key))
            continue;
        result.applied.push_back(key);
        if (keyClassOf(key) == KeyClass::external)
            result.external.push_back(key);
        else if (keyClassOf(key) == KeyClass::restart)
            result.pending_restart.push_back(key);
    }
    if (!applyEffective(origin))
        changed();
    return result;
}

std::string RuntimeConfig::promoteToBaseline()
{
    std::vector<std::string> problems;
    Overlay next = overlay_;
    bool conf_written = false;
    bool list_written[2] = {false, false};

    // A value pinned by the environment or --set is never written to the
    // baseline; it leaves the overlay too, and is reported.
    skipped_.clear();
    if (next.max_range_nm && !pinnedBy("filters.max_range_nm").empty()) {
        skipped_.emplace_back("filters.max_range_nm", pinnedBy("filters.max_range_nm"));
        next.max_range_nm.reset();
    }
    if (next.min_elevation_deg && !pinnedBy("filters.min_elevation_deg").empty()) {
        skipped_.emplace_back("filters.min_elevation_deg", pinnedBy("filters.min_elevation_deg"));
        next.min_elevation_deg.reset();
    }
    std::map<std::string, toml_lite::Value> values;
    if (next.max_range_nm)
        values["filters.max_range_nm"] = *next.max_range_nm;
    if (next.min_elevation_deg)
        values["filters.min_elevation_deg"] = *next.min_elevation_deg;
    if (!values.empty()) {
        const std::string& path = overrides_watch_.path();
        try {
            writeOverrides(values, {});
            conf_written = true;
            next.max_range_nm.reset();
            next.min_elevation_deg.reset();
        } catch (const std::exception& e) {
            problems.push_back(std::string(kOverridesFileName) + " not updated: " + e.what() + "; it is " + path +
                               ", in paths.state_dir when that is set (set paths.state_dir to a directory"
                               " skycued can write)");
        }
    }

    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        const bool allow = kind == ListKind::allow;
        auto& add = allow ? next.allow_add : next.block_add;
        auto& remove = allow ? next.allow_remove : next.block_remove;
        if (add.empty() && remove.empty())
            continue;
        const char* key = allow ? "filters.allow_list_file" : "filters.block_list_file";
        const std::string& path = listPath(kind);
        if (path.empty()) {
            problems.push_back(std::string(key) + " is not set in skycue.toml: the " + (allow ? "allow" : "block") +
                               " list changes (" + std::to_string(add.size()) + " added, " +
                               std::to_string(remove.size()) + " removed) stay in " + kOverlayName);
            continue;
        }
        try {
            const std::string text = FileStamp::of(path).exists ? readFile(path) : std::string();
            const std::string out = rewriteList(text, add, remove);
            IdList::parse(out, path);  // never write a file skycued would refuse
            writeFile(listWatch(kind), out);
            list_written[allow ? 0 : 1] = true;
            add.clear();
            remove.clear();
        } catch (const std::exception& e) {
            problems.push_back(std::string(key) + " not updated: " + e.what());
        }
    }

    try {
        if (next != overlay_)
            writeOverlay(next);
    } catch (const std::exception& e) {
        problems.push_back(std::string(kOverlayName) + " not updated: " + e.what());
    }
    overlay_ = std::move(next);  // what is in the files now, whether or not the overlay file is current

    // Read back what was written (and anything edited by hand meanwhile).
    if (conf_written)
        reloadConf(kOverridesFileName);
    for (ListKind kind : {ListKind::allow, ListKind::block})
        if (list_written[kind == ListKind::allow ? 0 : 1])
            reloadList(kind);
    applyEffective("save-baseline");

    std::string result;
    for (const auto& p : problems) {
        LOG_WARN("save-baseline: " << p);
        result += (result.empty() ? "" : "\n") + p;
    }
    LOG_INFO("save-baseline: " << (result.empty() ? "the overlay is now the baseline" : "partly done"));
    changed();  // overlay_active changed even when no effective value did
    return result;
}

// ---- watcher -------------------------------------------------------------------------

void RuntimeConfig::startWatching()
{
    if (watching_)
        return;
    watching_ = true;
    arm();
}

void RuntimeConfig::arm()
{
    timer_.expires_after(kPollInterval);
    std::weak_ptr<char> alive = alive_;
    timer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (ec || alive.expired())
            return;
        poll();
        arm();
    });
}

void RuntimeConfig::poll()
{
    bool quiet_change = false;  // a file reloaded without an Engine change
    if (conf_watch_.poll()) {
        reloadConf(baseName(conf_path_));
        quiet_change |= !applyEffective("file:" + baseName(conf_path_));
    }
    if (!overrides_watch_.path().empty() && overrides_watch_.poll()) {
        reloadConf(kOverridesFileName);
        quiet_change |= !applyEffective(std::string("file:") + kOverridesFileName);
    }
    if (overlay_watch_.poll()) {
        reloadOverlay();
        quiet_change |= !applyEffective(std::string("file:") + kOverlayName);
    }
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        if (listPath(kind).empty())
            continue;
        if (listWatch(kind).poll()) {
            reloadList(kind);
            quiet_change |= !applyEffective("file:" + baseName(listPath(kind)));
        }
    }
    if (quiet_change)
        changed();
}

void RuntimeConfig::reloadConf(const std::string& edited, bool quiet)
{
    Config c;
    try {
        // The overrides file of the running paths.state_dir (a changed one needs
        // a restart); the environment and --set stay above both files.
        c = parseConfigFile(conf_path_, overrides_watch_.path(), set_args_, env_);
    } catch (const std::exception& e) {
        LOG_ERROR(e.what() << "; keeping the previous configuration");
        return;
    }
    // Against the running values: a key edited back to what is running needs no restart.
    const auto restart = restartKeysChanged(running_, c);
    if (!restart.empty() && !quiet) {
        std::string keys;
        for (const auto& k : restart)
            keys += (keys.empty() ? "" : ", ") + k;
        LOG_WARN(conf_path_ << ": " << keys << " differ from the running values; requires restart");
    }
    // A filter value edited by hand that the overlay replaces changes nothing
    // the Engine runs with: say so, or the edit looks ignored.
    const std::pair<const char*, bool> overridden[] = {
        {"filters.max_range_nm", c.max_range_nm != file_conf_.max_range_nm && overlay_.max_range_nm.has_value()},
        {"filters.min_elevation_deg",
         c.min_elevation_deg != file_conf_.min_elevation_deg && overlay_.min_elevation_deg.has_value()},
    };
    for (const auto& o : overridden)
        if (o.second && !quiet)
            LOG_WARN("runtime: " << o.first << " edited in " << edited << " but overridden by " << kOverlayName
                                 << " (use the UI or \"save as baseline\")");
    // An edit of a filter key that the environment or --set sets changes
    // nothing either: say which, or the edit looks ignored.
    Config files = c;
    if (!c.sources.empty()) {
        try {
            files = parseConfigFile(conf_path_, overrides_watch_.path(), {}, {});
        } catch (const std::exception&) {
            files = files_only_;  // valid only with the environment or --set: no comparison
        }
    }
    for (const auto& src : c.sources) {
        if ((src.source != "env" && src.source != "--set") ||
            keyClassOf(src.key) != KeyClass::live)
            continue;
        if (!quiet && configValueText(files, src.key) != configValueText(files_only_, src.key))
            LOG_WARN("runtime: " << src.key << " edited in " << edited << " but pinned by "
                                 << (src.source == "env" ? envNameOf(src.key) : std::string("--set")) << " ("
                                 << configValueText(c, src.key) << "); that value stays");
    }
    files_only_ = files;
    // An edit of skycue.toml that the overrides file replaces changes nothing
    // either: one line per key, so the edit does not look ignored.
    if (edited == baseName(conf_path_) && !quiet)
        for (const auto& key : overridesFileKeys()) {
            if (keyClassOf(key) != KeyClass::live)
                continue;
            const auto now = c.file_values.find(key), before = file_conf_.file_values.find(key);
            const bool same = (now == c.file_values.end()) == (before == file_conf_.file_values.end()) &&
                              (now == c.file_values.end() || now->second == before->second);
            if (!same && c.override_keys.count(key))
                LOG_INFO("runtime: " << key << " edited in " << edited << " but " << kOverridesFileName
                                     << " sets it (" << configValueText(c, key) << "); that value stays");
        }
    // A relative list file resolves against paths.state_dir as running: a changed
    // paths.state_dir is a restart key, so it does not move the lists before then.
    c.allow_list_path = listFilePath(running_.state_dir, c.allow_list_file);
    c.block_list_path = listFilePath(running_.state_dir, c.block_list_file);
    const Config old = file_conf_;
    file_conf_ = c;
    running_.max_range_nm = c.max_range_nm;
    running_.min_elevation_deg = c.min_elevation_deg;
    running_.allow_list_file = c.allow_list_file;
    running_.block_list_file = c.block_list_file;
    running_.allow_list_path = c.allow_list_path;
    running_.block_list_path = c.block_list_path;
    // Whether the file sets a live key follows the file, like its value; the
    // overrides file holds live keys only.
    for (const auto& key : overridesFileKeys()) {
        if (keyClassOf(key) != KeyClass::live)
            continue;
        if (c.file_keys.count(key))
            running_.file_keys.insert(key);
        else
            running_.file_keys.erase(key);
    }
    running_.override_keys = c.override_keys;
    running_.sources = c.sources;
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        const bool allow = kind == ListKind::allow;
        if ((allow ? old.allow_list_path : old.block_list_path) == listPath(kind))
            continue;
        // A new list file: read it now and watch it from here on. If it
        // cannot be read the previous entries stay until it can.
        listWatch(kind) = SettledFile(listPath(kind));
        listWatch(kind).accept();
        reloadList(kind);
    }
}

void RuntimeConfig::reloadOverlay()
{
    if (!overlay_watch_.stamp().exists) {
        overlay_ = Overlay();
        return;
    }
    try {
        std::vector<std::string> warnings;
        Overlay o = parseOverlay(readFile(overlay_path_), &warnings);
        for (const auto& w : warnings)
            LOG_WARN(overlay_path_ << ": " << w);
        overlay_ = std::move(o);
    } catch (const std::exception& e) {
        LOG_ERROR(overlay_path_ << ": " << e.what() << "; keeping the previous overlay");
    }
}

void RuntimeConfig::reloadList(ListKind kind)
{
    try {
        fileList(kind) = loadList(listPath(kind));
    } catch (const std::exception& e) {
        LOG_ERROR(e.what() << "; keeping the previous " << (kind == ListKind::allow ? "allow" : "block") << " list");
    }
}

}  // namespace skycued
