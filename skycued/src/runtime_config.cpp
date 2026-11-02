#include "runtime_config.hpp"

#include "conf_rewrite.hpp"
#include "engine.hpp"
#include "json_lite.hpp"
#include "log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Writes `text` to <file>.tmp and renames it over <file>, where <file> is
// `path` with symlinks resolved (so a symlink stays a symlink and the file it
// points to gets the text), keeping the mode and, best effort, the owner of
// the file it replaces. Throws std::runtime_error.
void atomicWrite(const std::string& path, const std::string& text)
{
    std::string target = path;
    if (char* real = ::realpath(path.c_str(), nullptr)) {  // fails for a file not created yet
        target = real;
        std::free(real);
    }
    const std::string tmp = target + ".tmp";  // same directory: rename stays atomic
    struct stat st{};
    const bool existed = ::stat(target.c_str(), &st) == 0;
    const mode_t mode = existed ? (st.st_mode & 07777) : 0644;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        throw std::runtime_error(tmp + ": cannot create: " + std::strerror(errno));
    auto fail = [&](const std::string& what) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error(tmp + ": " + what + ": " + std::strerror(err));
    };
    if (existed && ::fchmod(fd, mode) != 0)
        fail("cannot set the mode");
    // Keep the owner (when skycued runs as root, an operator's file stays
    // theirs). Without the privilege this fails with EPERM: ignored.
    if (existed && (st.st_uid != ::geteuid() || st.st_gid != ::getegid()) &&
        ::fchown(fd, st.st_uid, st.st_gid) != 0 && errno != EPERM)
        fail("cannot set the owner");
    for (size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail("write error");
        }
        done += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0)
        fail("fsync failed");
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error(tmp + ": close failed: " + std::strerror(err));
    }
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error(target + ": cannot replace: " + std::strerror(err));
    }
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

// The keys whose change needs a restart (everything but the filter keys).
std::vector<std::string> restartKeysChanged(const Config& a, const Config& b)
{
    std::vector<std::string> out;
    auto check = [&](bool differs, const char* key) {
        if (differs)
            out.push_back(key);
    };
    check(a.receiver_lat != b.receiver_lat, "RECEIVER_LAT");
    check(a.receiver_lon != b.receiver_lon, "RECEIVER_LON");
    check(a.receiver_alt_m != b.receiver_alt_m, "RECEIVER_ALT_M");
    check(a.antenna_height_m != b.antenna_height_m, "ANTENNA_HEIGHT_M");
    check(a.sdr_1090_serial != b.sdr_1090_serial, "SDR_1090_SERIAL");
    check(a.sdr_978_serial != b.sdr_978_serial, "SDR_978_SERIAL");
    check(a.gain_1090 != b.gain_1090, "GAIN_1090");
    check(a.gain_978 != b.gain_978, "GAIN_978");
    check(a.dump978_raw_port != b.dump978_raw_port, "DUMP978_RAW_PORT");
    check(a.dump978_json_port != b.dump978_json_port, "DUMP978_JSON_PORT");
    check(a.readsb_json_port != b.readsb_json_port, "READSB_JSON_PORT");
    check(a.web_port != b.web_port, "WEB_PORT");
    check(a.web_bind != b.web_bind, "WEB_BIND");
    check(a.cue_tcp_port != b.cue_tcp_port, "CUE_TCP_PORT");
    check(a.cue_tcp_connect_targets != b.cue_tcp_connect_targets, "CUE_TCP_CONNECT");
    check(a.cue_udp_targets != b.cue_udp_targets, "CUE_UDP_TARGETS");
    check(a.record_dir != b.record_dir, "RECORD_DIR");
    check(a.record_formats != b.record_formats, "RECORD_FORMATS");
    check(a.record_max_gb != b.record_max_gb, "RECORD_MAX_GB");
    check(a.min_interval_s != b.min_interval_s, "MIN_INTERVAL_S");
    check(a.max_interval_s != b.max_interval_s, "MAX_INTERVAL_S");
    check(a.track_timeout_s != b.track_timeout_s, "TRACK_TIMEOUT_S");
    check(a.keyframe_interval_s != b.keyframe_interval_s, "KEYFRAME_INTERVAL_S");
    check(a.trail_seconds != b.trail_seconds, "TRAIL_SECONDS");
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

bool RuntimeConfig::Stamp::operator==(const Stamp& o) const
{
    if (exists != o.exists)
        return false;
    return !exists || (dev == o.dev && ino == o.ino && size == o.size && mtime_ns == o.mtime_ns);
}

RuntimeConfig::Stamp RuntimeConfig::stampOf(const std::string& path)
{
    Stamp s;
    struct stat st{};
    if (path.empty() || ::stat(path.c_str(), &st) != 0)
        return s;
    s.exists = true;
    s.dev = st.st_dev;
    s.ino = st.st_ino;
    s.size = st.st_size;
    s.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    return s;
}

RuntimeConfig::RuntimeConfig(boost::asio::io_context& io, std::string conf_path, Engine& engine)
    : conf_path_(std::move(conf_path)), engine_(engine), timer_(io)
{
    const std::string dir = dirOf(conf_path_);
    overlay_path_ = (dir == "/" ? dir : dir + "/") + kOverlayName;
}

RuntimeConfig::~RuntimeConfig()
{
    alive_.reset();
    timer_.cancel();
}

const std::string& RuntimeConfig::listPath(ListKind kind) const
{
    return kind == ListKind::allow ? file_conf_.allow_list_file : file_conf_.block_list_file;
}

void RuntimeConfig::load()
{
    // Stamps before reading: an edit in between is then seen again, never missed.
    conf_stamp_ = stampOf(conf_path_);
    file_conf_ = parseConfigFile(conf_path_);
    running_ = file_conf_;
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        listStamp(kind) = stampOf(listPath(kind));
        fileList(kind) = loadList(listPath(kind));
    }
    overlay_stamp_ = stampOf(overlay_path_);
    overlay_ = Overlay();
    if (overlay_stamp_.exists) {
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
            overlay_stamp_ = stampOf(overlay_path_);
        }
    }
    applyEffective("startup");
}

FilterConfig RuntimeConfig::effectiveFilters() const
{
    return FilterConfig{overlay_.max_range_nm.value_or(running_.max_range_nm),
                        overlay_.min_elevation_deg.value_or(running_.min_elevation_deg)};
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

void RuntimeConfig::applyEffective(const std::string& origin)
{
    const FilterConfig want = effectiveFilters();
    const FilterConfig have = engine_.snapshot().filters;
    if (want.max_range_nm != have.max_range_nm || want.min_elevation_deg != have.min_elevation_deg)
        engine_.setFilterConfig(want, origin);

    IdList allow = effectiveList(ListKind::allow), block = effectiveList(ListKind::block);
    std::vector<std::string> allow_add, allow_remove, block_add, block_remove;
    listDiff(engine_.allowList(), allow, allow_add, allow_remove);
    listDiff(engine_.blockList(), block, block_add, block_remove);
    const size_t changes = allow_add.size() + allow_remove.size() + block_add.size() + block_remove.size();
    if (changes == 0)
        return;
    if (changes > 1) {
        engine_.setLists(std::move(allow), std::move(block), origin);
        return;
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
}

void RuntimeConfig::writeFile(const std::string& path, const std::string& text, Stamp& stamp)
{
    atomicWrite(path, text);
    stamp = stampOf(path);
}

void RuntimeConfig::writeOverlay(const Overlay& overlay)
{
    if (overlay.empty()) {
        if (::unlink(overlay_path_.c_str()) != 0 && errno != ENOENT)
            throw std::runtime_error(overlay_path_ + ": cannot delete: " + std::strerror(errno));
        overlay_stamp_ = stampOf(overlay_path_);
        return;
    }
    writeFile(overlay_path_, overlayJson(overlay), overlay_stamp_);
}

void RuntimeConfig::applyUi(const Overlay& delta, const std::string& origin)
{
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
    applyEffective(origin);
}

std::string RuntimeConfig::promoteToBaseline()
{
    std::vector<std::string> problems;
    Overlay next = overlay_;
    bool conf_written = false;
    bool list_written[2] = {false, false};

    std::map<std::string, std::string> values;
    if (next.max_range_nm)
        values["MAX_RANGE_NM"] = formatRoundTrip(*next.max_range_nm);
    if (next.min_elevation_deg)
        values["MIN_ELEVATION_DEG"] = formatRoundTrip(*next.min_elevation_deg);
    if (!values.empty()) {
        try {
            const std::string text = readFile(conf_path_);
            const std::string out = conf_rewrite::rewriteKeys(text, values);
            parseConfigText(out, conf_path_);  // never write a file skycued would refuse
            if (out != text) {
                writeFile(conf_path_, out, conf_stamp_);
                conf_written = true;
            }
            next.max_range_nm.reset();
            next.min_elevation_deg.reset();
        } catch (const std::exception& e) {
            problems.push_back(std::string("skycue.conf not updated: ") + e.what());
        }
    }

    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        const bool allow = kind == ListKind::allow;
        auto& add = allow ? next.allow_add : next.block_add;
        auto& remove = allow ? next.allow_remove : next.block_remove;
        if (add.empty() && remove.empty())
            continue;
        const char* key = allow ? "ALLOW_LIST_FILE" : "BLOCK_LIST_FILE";
        const std::string& path = listPath(kind);
        if (path.empty()) {
            problems.push_back(std::string(key) + " is not set in skycue.conf: the " + (allow ? "allow" : "block") +
                               " list changes (" + std::to_string(add.size()) + " added, " +
                               std::to_string(remove.size()) + " removed) stay in " + kOverlayName);
            continue;
        }
        try {
            const std::string text = stampOf(path).exists ? readFile(path) : std::string();
            const std::string out = rewriteList(text, add, remove);
            IdList::parse(out, path);  // never write a file skycued would refuse
            writeFile(path, out, listStamp(kind));
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
        reloadConf();
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

bool RuntimeConfig::settled(const std::string& path, Stamp& known, std::optional<Stamp>& pending)
{
    const Stamp now = stampOf(path);
    if (now == known) {
        pending.reset();
        return false;
    }
    if (!pending || *pending != now) {  // changed since the last poll: maybe still being written
        pending = now;
        return false;
    }
    known = now;
    pending.reset();
    LOG_INFO(path << ": changed on disk; reloading");
    return true;
}

void RuntimeConfig::poll()
{
    if (settled(conf_path_, conf_stamp_, conf_pending_)) {
        reloadConf();
        applyEffective("file:" + baseName(conf_path_));
    }
    if (settled(overlay_path_, overlay_stamp_, overlay_pending_)) {
        reloadOverlay();
        applyEffective(std::string("file:") + kOverlayName);
    }
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        if (listPath(kind).empty())
            continue;
        if (settled(listPath(kind), listStamp(kind), kind == ListKind::allow ? allow_pending_ : block_pending_)) {
            reloadList(kind);
            applyEffective("file:" + baseName(listPath(kind)));
        }
    }
}

void RuntimeConfig::reloadConf()
{
    Config c;
    try {
        c = parseConfigFile(conf_path_);
    } catch (const std::exception& e) {
        LOG_ERROR(e.what() << "; keeping the previous configuration");
        return;
    }
    // Against the running values: a key edited back to what is running needs no restart.
    const auto restart = restartKeysChanged(running_, c);
    if (!restart.empty()) {
        std::string keys;
        for (const auto& k : restart)
            keys += (keys.empty() ? "" : ", ") + k;
        LOG_WARN(conf_path_ << ": " << keys << " differ from the running values; requires restart");
    }
    // A filter value edited by hand that the overlay replaces changes nothing
    // the Engine runs with: say so, or the edit looks ignored.
    const std::pair<const char*, bool> overridden[] = {
        {"MAX_RANGE_NM", c.max_range_nm != file_conf_.max_range_nm && overlay_.max_range_nm.has_value()},
        {"MIN_ELEVATION_DEG",
         c.min_elevation_deg != file_conf_.min_elevation_deg && overlay_.min_elevation_deg.has_value()},
    };
    for (const auto& o : overridden)
        if (o.second)
            LOG_WARN("runtime: " << o.first << " edited in skycue.conf but overridden by " << kOverlayName
                                 << " (use the UI or \"save as baseline\")");
    const Config old = file_conf_;
    file_conf_ = c;
    running_.max_range_nm = c.max_range_nm;
    running_.min_elevation_deg = c.min_elevation_deg;
    running_.allow_list_file = c.allow_list_file;
    running_.block_list_file = c.block_list_file;
    for (ListKind kind : {ListKind::allow, ListKind::block}) {
        const bool allow = kind == ListKind::allow;
        if ((allow ? old.allow_list_file : old.block_list_file) == listPath(kind))
            continue;
        // A new list file: read it now and watch it from here on. If it
        // cannot be read the previous entries stay until it can.
        listStamp(kind) = stampOf(listPath(kind));
        reloadList(kind);
    }
}

void RuntimeConfig::reloadOverlay()
{
    if (!overlay_stamp_.exists) {
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
