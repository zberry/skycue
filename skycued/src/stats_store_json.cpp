// The stats file: StatsStore::toJson / fromJson, loadStatsFile, saveStatsFile
// (layout in stats_store.hpp's header comment).

#include "stats_store.hpp"

#include "file_util.hpp"
#include "json_lite.hpp"
#include "log.hpp"
#include "recorder.hpp"
#include "time_util.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstring>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace skycued {

namespace {

using skycue::AddressType;
using skycue::Source;
using skycue::TrackId;

struct BucketField {
    const char* name;
    uint64_t StatsBucket::*field;
};

// Every integer counter of a StatsBucket, under its field name.
const BucketField kBucketFields[] = {
    {"observations", &StatsBucket::observations},
    {"positions", &StatsBucket::positions},
    {"aircraft", &StatsBucket::aircraft},
    {"new_aircraft", &StatsBucket::new_aircraft},
    {"sent_updates", &StatsBucket::sent_updates},
    {"drops_timeout", &StatsBucket::drops_timeout},
    {"drops_filtered", &StatsBucket::drops_filtered},
    {"drops_source_lost", &StatsBucket::drops_source_lost},
    {"drops_blocked", &StatsBucket::drops_blocked},
    {"drops_out_of_range", &StatsBucket::drops_out_of_range},
    {"drops_below_elevation", &StatsBucket::drops_below_elevation},
    {"drops_no_position", &StatsBucket::drops_no_position},
    {"uat_downlink", &StatsBucket::uat_downlink},
    {"uat_uplink", &StatsBucket::uat_uplink},
    {"fisb_products", &StatsBucket::fisb_products},
    {"emergencies", &StatsBucket::emergencies},
    {"tcp_clients", &StatsBucket::tcp_clients},
    {"web_clients", &StatsBucket::web_clients},
};

// Indexed by StatsHist / StatsCount.
const char* const kHistNames[] = {"range", "altitude", "speed", "vertical_rate", "elevation", "rssi"};
const char* const kCountNames[] = {"category", "source", "addr_type", "input", "decision", "squawk_emergency"};

constexpr std::size_t kCoverageCells = std::size_t(kStatsSectors) * std::size_t(kStatsBands);
constexpr std::size_t kRssiCells = std::size_t(kStatsRssiRangeBins) * std::size_t(kStatsRssiBins);
// floor(lat / 0.05) and floor(lon / 0.05) of a position on the globe.
constexpr int64_t kGridLatMax = 1800, kGridLonMax = 3600;

bool alignedTo(int64_t t_s, int64_t len)
{
    int64_t r = t_s % len;
    return r == 0;
}

std::string trackKey(const TrackId& id)
{
    return hexAddress(id.address) + "|" + recording::addressTypeName(id.type);
}

std::optional<TrackId> parseTrackKey(const std::string& s)
{
    const std::size_t bar = s.find('|');
    if (bar == std::string::npos)
        return std::nullopt;
    const std::optional<uint32_t> addr = parseHexAddress(s.substr(0, bar));
    if (!addr)
        return std::nullopt;
    const std::string type = s.substr(bar + 1);
    for (AddressType t : {AddressType::icao, AddressType::non_icao, AddressType::tisb_trackfile, AddressType::other})
        if (type == recording::addressTypeName(t))
            return TrackId{*addr, t};
    return std::nullopt;
}

std::optional<Source> parseSourceName(const std::string& s)
{
    for (Source v : {Source::adsb_1090, Source::uat_978, Source::tisb, Source::adsr, Source::mlat, Source::other})
        if (s == recording::sourceName(v))
            return v;
    return std::nullopt;
}

// The ledger's lists, space-joined; whitespace inside an entry (never seen:
// a callsign is trimmed, squawks are digits) is written as '_', and an empty
// entry is left out, so the list splits back into the same entries.
std::string joinList(const std::vector<std::string>& list)
{
    std::string out;
    for (const std::string& v : list) {
        if (v.empty())
            continue;
        if (!out.empty())
            out += ' ';
        for (char ch : v)
            out += std::isspace(static_cast<unsigned char>(ch)) ? '_' : ch;
    }
    return out;
}

// ---------------------------------------------------------------- writing

// One JSON array, built in place.
class Column {
public:
    Column() : s_("[") {}
    void u(uint64_t v)
    {
        sep();
        s_ += std::to_string(v);
    }
    void i(int64_t v)
    {
        sep();
        s_ += std::to_string(v);
    }
    void d(double v)
    {
        sep();
        s_ += formatRoundTrip(v);
    }
    void f(float v)
    {
        sep();
        s_ += formatRoundTrip(v);
    }
    void str(const std::string& v)
    {
        sep();
        appendJsonString(s_, v);
    }
    std::string done() const { return s_ + "]"; }

private:
    void sep()
    {
        if (s_.size() > 1)
            s_ += ',';
    }
    std::string s_;
};

std::string ringJson(const std::deque<StatsBucket>& ring)
{
    JsonWriter w;
    Column start;
    for (const StatsBucket& b : ring)
        start.i(b.start_s);
    w.addRaw("start", start.done());
    for (const BucketField& f : kBucketFields) {
        Column c;
        for (const StatsBucket& b : ring)
            c.u(b.*(f.field));
        w.addRaw(f.name, c.done());
    }
    Column range, key;
    std::set<std::string> inputs;
    for (const StatsBucket& b : ring) {
        range.d(b.max_range_nm);
        key.str(b.max_range_id ? trackKey(*b.max_range_id) : std::string());
        for (const auto& kv : b.by_input)
            inputs.insert(kv.first);
    }
    w.addRaw("max_range_nm", range.done());
    w.addRaw("max_range_key", key.done());
    JsonWriter by_input;
    for (const std::string& in : inputs) {
        Column c;
        for (const StatsBucket& b : ring) {
            auto it = b.by_input.find(in);
            c.u(it == b.by_input.end() ? 0 : it->second);
        }
        by_input.addRaw(in.c_str(), c.done());
    }
    w.addRaw("by_input", by_input.str());
    return w.str();
}

std::string u64Array(const std::vector<uint64_t>& v)
{
    Column c;
    for (uint64_t x : v)
        c.u(x);
    return c.done();
}

// ---------------------------------------------------------------- reading

[[noreturn]] void fail(const std::string& field, const std::string& why)
{
    throw std::runtime_error("stats file: " + field + ": " + why);
}

std::string at(const std::string& field, std::size_t i) { return field + "[" + std::to_string(i) + "]"; }

template <class T>
std::optional<T> parseInteger(const std::string& s)
{
    T v{};
    const char* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (s.empty() || r.ec != std::errc() || r.ptr != end)
        return std::nullopt;
    return v;
}

// Finite numbers in the classic locale, through one reused stream.
class DoubleParser {
public:
    DoubleParser() { in_.imbue(std::locale::classic()); }
    std::optional<double> operator()(const std::string& s)
    {
        if (s.empty())
            return std::nullopt;
        in_.clear();
        in_.str(s);
        double v = 0;
        in_ >> std::noskipws >> v;
        if (in_.fail() || in_.get() != std::char_traits<char>::eof() || !std::isfinite(v))
            return std::nullopt;
        return v;
    }

private:
    std::istringstream in_;
};

class Reader {
public:
    // Moved out of `o` (not copied): each nested object is read once.
    JsonObject object(JsonObject& o, const std::string& ctx, const char* key)
    {
        std::optional<JsonObject> v = o.takeObject(key);
        if (!v)
            fail(name(ctx, key), "missing or not an object");
        return std::move(*v);
    }

    std::vector<std::string> strings(const JsonObject& o, const std::string& ctx, const char* key,
                                     std::optional<std::size_t> len)
    {
        std::optional<std::vector<std::string>> v = o.getStringArray(key);
        if (!v)
            fail(name(ctx, key), "missing or not an array of values");
        if (len && v->size() != *len)
            fail(name(ctx, key), "length " + std::to_string(v->size()) + ", expected " + std::to_string(*len));
        return std::move(*v);
    }

    std::vector<uint64_t> counts(const JsonObject& o, const std::string& ctx, const char* key, std::size_t len)
    {
        const std::vector<std::string> text = strings(o, ctx, key, len);
        std::vector<uint64_t> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            out[i] = count(text[i], at(name(ctx, key), i));
        return out;
    }

    std::vector<int64_t> ints(const JsonObject& o, const std::string& ctx, const char* key,
                              std::optional<std::size_t> len)
    {
        const std::vector<std::string> text = strings(o, ctx, key, len);
        std::vector<int64_t> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const std::optional<int64_t> v = parseInteger<int64_t>(text[i]);
            if (!v)
                fail(at(name(ctx, key), i), "not an integer");
            out[i] = *v;
        }
        return out;
    }

    std::vector<double> doubles(const JsonObject& o, const std::string& ctx, const char* key, std::size_t len)
    {
        const std::vector<std::string> text = strings(o, ctx, key, len);
        std::vector<double> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            out[i] = number(text[i], at(name(ctx, key), i));
        return out;
    }

    // "" is unset.
    std::vector<std::optional<double>> optDoubles(const JsonObject& o, const std::string& ctx, const char* key,
                                                  std::size_t len)
    {
        const std::vector<std::string> text = strings(o, ctx, key, len);
        std::vector<std::optional<double>> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            if (!text[i].empty())
                out[i] = number(text[i], at(name(ctx, key), i));
        return out;
    }

    std::vector<std::optional<TrackId>> keys(const JsonObject& o, const std::string& ctx, const char* key,
                                             std::size_t len, bool allow_unset)
    {
        const std::vector<std::string> text = strings(o, ctx, key, len);
        std::vector<std::optional<TrackId>> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (allow_unset && text[i].empty())
                continue;
            out[i] = parseTrackKey(text[i]);
            if (!out[i])
                fail(at(name(ctx, key), i), "not \"<hex>|<addr_type>\"");
        }
        return out;
    }

    // Capped at INT64_MAX: the file's integers are written as int64, so a
    // larger count would not survive the next save.
    uint64_t count(const std::string& text, const std::string& field)
    {
        const std::optional<uint64_t> v = parseInteger<uint64_t>(text);
        if (!v)
            fail(field, "not a non-negative integer");
        return std::min<uint64_t>(*v, uint64_t(std::numeric_limits<int64_t>::max()));
    }

    double number(const std::string& text, const std::string& field)
    {
        const std::optional<double> v = parse_(text);
        if (!v)
            fail(field, "not a finite number");
        return *v;
    }

    static std::string name(const std::string& ctx, const char* key) { return ctx.empty() ? key : ctx + "." + key; }

private:
    DoubleParser parse_;
};

std::deque<StatsBucket> readRing(Reader& rd, JsonObject& top, const char* ring_name, int64_t len,
                                 std::size_t cap)
{
    JsonObject o = rd.object(top, "", ring_name);
    const std::string ctx = ring_name;
    const std::vector<int64_t> start = rd.ints(o, ctx, "start", std::nullopt);
    const std::size_t n = start.size();
    if (n > cap)
        fail(ctx + ".start", std::to_string(n) + " buckets, at most " + std::to_string(cap));
    std::deque<StatsBucket> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!alignedTo(start[i], len))
            fail(at(ctx + ".start", i), "not a multiple of " + std::to_string(len));
        if (i > 0 && start[i] != start[i - 1] + len)
            fail(at(ctx + ".start", i), "not contiguous with the bucket before");
        out[i].start_s = start[i];
    }
    for (const BucketField& f : kBucketFields) {
        const std::vector<uint64_t> v = rd.counts(o, ctx, f.name, n);
        for (std::size_t i = 0; i < n; ++i)
            out[i].*(f.field) = v[i];
    }
    const std::vector<double> range = rd.doubles(o, ctx, "max_range_nm", n);
    const std::vector<std::optional<TrackId>> key = rd.keys(o, ctx, "max_range_key", n, true);
    for (std::size_t i = 0; i < n; ++i) {
        out[i].max_range_nm = range[i];
        out[i].max_range_id = key[i];
    }
    const JsonObject by_input = rd.object(o, ctx, "by_input");
    const std::vector<std::string> ids = by_input.keys();
    if (ids.size() > kStatsInputsMax + 1)
        fail(ctx + ".by_input", std::to_string(ids.size()) + " inputs, at most " + std::to_string(kStatsInputsMax + 1));
    for (const std::string& id : ids) {
        if (id.empty())
            fail(ctx + ".by_input", "an empty input id");
        const std::vector<uint64_t> v = rd.counts(by_input, ctx + ".by_input", id.c_str(), n);
        for (std::size_t i = 0; i < n; ++i)
            if (v[i] > 0)
                out[i].by_input[id] = v[i];
    }
    return out;
}

}  // namespace

std::string StatsStore::toJson() const
{
    JsonWriter w;
    w.add("version", int64_t(kStatsFileVersion));
    if (since_s_)
        w.add("since", *since_s_);
    if (series_since_s_)
        w.add("series_since", *series_since_s_);
    w.add("evicted", int64_t(evicted_));
    w.add("uat_seen", uat_seen_);
    w.add("grid_full", gridFull());
    w.addRaw("minutes", ringJson(minute_.buckets));
    w.addRaw("hours", ringJson(hour_.buckets));

    JsonWriter hist;
    for (std::size_t h = 0; h < hist_.size(); ++h)
        hist.addRaw(kHistNames[h], u64Array(hist_[h].counts));
    w.addRaw("hist", hist.str());

    Column cov_range, cov_key, cov_time, cov_n;
    for (const CoverageCell& c : coverage_) {
        cov_range.d(c.max_range_nm);
        cov_key.str(c.id ? trackKey(*c.id) : std::string());
        cov_time.i(c.time_s);
        cov_n.u(c.n);
    }
    JsonWriter cov;
    cov.addRaw("max_range_nm", cov_range.done());
    cov.addRaw("key", cov_key.done());
    cov.addRaw("time", cov_time.done());
    cov.addRaw("n", cov_n.done());
    w.addRaw("coverage", cov.str());
    w.addRaw("rssi_by_range", u64Array(rssi_by_range_));

    Column lat_i, lon_i, grid_n;
    for (const GridCell& c : gridCells()) {
        lat_i.i(c.lat_i);
        lon_i.i(c.lon_i);
        grid_n.u(c.n);
    }
    JsonWriter grid;
    grid.addRaw("lat_i", lat_i.done());
    grid.addRaw("lon_i", lon_i.done());
    grid.addRaw("n", grid_n.done());
    w.addRaw("grid", grid.str());

    JsonWriter counts;
    for (std::size_t k = 0; k < counts_.size(); ++k) {
        JsonWriter m;
        for (const auto& kv : counts_[k])
            m.add(kv.first.c_str(), int64_t(kv.second));
        counts.addRaw(kCountNames[k], m.str());
    }
    w.addRaw("counts", counts.str());

    std::vector<const AircraftStats*> ledger;
    ledger.reserve(ledger_.size());
    for (const auto& kv : ledger_)
        ledger.push_back(&kv.second);
    std::sort(ledger.begin(), ledger.end(),
              [](const AircraftStats* a, const AircraftStats* b) { return a->id < b->id; });
    Column key, first_seen, last_seen, observations, positions, sessions, sent_updates, drops, emergencies;
    Column max_range_nm, max_range_time, max_range_lat, max_range_lon, max_range_bearing;
    Column min_alt, max_alt, max_gs, min_rssi, max_rssi, callsigns, squawks, inputs, registration, category, source;
    auto optD = [](Column& c, const std::optional<double>& v) {
        if (v)
            c.d(*v);
        else
            c.str("");
    };
    auto optI = [](Column& c, const std::optional<int32_t>& v) {
        if (v)
            c.i(*v);
        else
            c.str("");
    };
    for (const AircraftStats* a : ledger) {
        key.str(trackKey(a->id));
        first_seen.i(a->first_seen_s);
        last_seen.i(a->last_seen_s);
        observations.u(a->observations);
        positions.u(a->positions);
        sessions.u(a->sessions);
        sent_updates.u(a->sent_updates);
        drops.u(a->drops);
        emergencies.u(a->emergencies);
        optD(max_range_nm, a->max_range_nm);
        max_range_time.i(a->max_range_time_s);
        max_range_lat.d(a->max_range_lat);
        max_range_lon.d(a->max_range_lon);
        max_range_bearing.d(a->max_range_bearing_deg);
        optI(min_alt, a->min_alt_ft);
        optI(max_alt, a->max_alt_ft);
        // Ground speed comes from a float field: written at float precision
        // (and read back through float), which loses nothing.
        if (a->max_gs_kt)
            max_gs.f(static_cast<float>(*a->max_gs_kt));
        else
            max_gs.str("");
        optD(min_rssi, a->min_rssi);
        optD(max_rssi, a->max_rssi);
        callsigns.str(joinList(a->callsigns));
        squawks.str(joinList(a->squawks));
        inputs.str(joinList(a->inputs));
        registration.str(a->registration.value_or(std::string()));
        category.str(a->category.value_or(std::string()));
        source.str(recording::sourceName(a->source));
    }
    JsonWriter ac;
    ac.addRaw("key", key.done());
    ac.addRaw("first_seen", first_seen.done());
    ac.addRaw("last_seen", last_seen.done());
    ac.addRaw("observations", observations.done());
    ac.addRaw("positions", positions.done());
    ac.addRaw("sessions", sessions.done());
    ac.addRaw("sent_updates", sent_updates.done());
    ac.addRaw("drops", drops.done());
    ac.addRaw("emergencies", emergencies.done());
    ac.addRaw("max_range_nm", max_range_nm.done());
    ac.addRaw("max_range_time", max_range_time.done());
    ac.addRaw("max_range_lat", max_range_lat.done());
    ac.addRaw("max_range_lon", max_range_lon.done());
    ac.addRaw("max_range_bearing_deg", max_range_bearing.done());
    ac.addRaw("min_alt_ft", min_alt.done());
    ac.addRaw("max_alt_ft", max_alt.done());
    ac.addRaw("max_gs_kt", max_gs.done());
    ac.addRaw("min_rssi", min_rssi.done());
    ac.addRaw("max_rssi", max_rssi.done());
    ac.addRaw("callsigns", callsigns.done());
    ac.addRaw("squawks", squawks.done());
    ac.addRaw("inputs", inputs.done());
    ac.addRaw("registration", registration.done());
    ac.addRaw("category", category.done());
    ac.addRaw("source", source.done());
    w.addRaw("aircraft", ac.str());
    return w.str();
}

StatsStore StatsStore::fromJson(const std::string& text, std::optional<ReceiverPosition> receiver, StatsLimits limits)
{
    std::optional<JsonObject> parsed = JsonObject::parse(text, kStatsFileMaxBytes);
    if (!parsed)
        fail("text", "not a JSON object (malformed, over " + std::to_string(kStatsFileMaxBytes) +
                         " bytes or nested too deep)");
    JsonObject& top = *parsed;
    Reader rd;
    StatsStore st(receiver, limits);

    if (!top.has("version"))
        fail("version", "missing");
    const std::optional<int64_t> version = top.getInt("version");
    if (!version || *version != kStatsFileVersion)
        fail("version", "\"" + top.getString("version").value_or("?") + "\", expected " +
                            std::to_string(kStatsFileVersion));
    for (const char* k : {"since", "series_since"}) {
        if (!top.has(k))
            continue;
        const std::optional<int64_t> v = top.getInt(k);
        if (!v)
            fail(k, "not an integer");
        (std::string(k) == "since" ? st.since_s_ : st.series_since_s_) = *v;
    }
    if (!top.has("evicted"))
        fail("evicted", "missing");
    st.evicted_ = rd.count(top.getString("evicted").value_or(""), "evicted");
    const std::optional<bool> uat = top.getBool("uat_seen");
    if (!uat)
        fail("uat_seen", "missing or not true/false");
    st.uat_seen_ = *uat;
    if (!top.getBool("grid_full"))
        fail("grid_full", "missing or not true/false");

    st.minute_.buckets = readRing(rd, top, "minutes", st.minute_.len, st.minute_.cap);
    st.hour_.buckets = readRing(rd, top, "hours", st.hour_.len, st.hour_.cap);

    JsonObject hist = rd.object(top, "", "hist");
    for (std::size_t h = 0; h < st.hist_.size(); ++h)
        st.hist_[h].counts = rd.counts(hist, "hist", kHistNames[h], histSize(StatsHist(h)));

    JsonObject cov = rd.object(top, "", "coverage");
    const std::vector<double> cov_range = rd.doubles(cov, "coverage", "max_range_nm", kCoverageCells);
    const std::vector<std::optional<TrackId>> cov_key = rd.keys(cov, "coverage", "key", kCoverageCells, true);
    const std::vector<int64_t> cov_time = rd.ints(cov, "coverage", "time", kCoverageCells);
    const std::vector<uint64_t> cov_n = rd.counts(cov, "coverage", "n", kCoverageCells);
    for (std::size_t i = 0; i < kCoverageCells; ++i) {
        CoverageCell& c = st.coverage_[i];
        c.max_range_nm = cov_range[i];
        c.id = cov_key[i];
        c.time_s = cov_time[i];
        c.n = cov_n[i];
    }
    st.rssi_by_range_ = rd.counts(top, "", "rssi_by_range", kRssiCells);

    JsonObject grid = rd.object(top, "", "grid");
    const std::vector<int64_t> lat_i = rd.ints(grid, "grid", "lat_i", std::nullopt);
    const std::vector<int64_t> lon_i = rd.ints(grid, "grid", "lon_i", lat_i.size());
    const std::vector<uint64_t> grid_n = rd.counts(grid, "grid", "n", lat_i.size());
    std::vector<std::pair<uint64_t, int64_t>> cells;  // (n, key)
    cells.reserve(lat_i.size());
    st.grid_.reserve(lat_i.size());
    for (std::size_t i = 0; i < lat_i.size(); ++i) {
        if (lat_i[i] < -kGridLatMax || lat_i[i] > kGridLatMax)
            fail(at("grid.lat_i", i), "outside -1800..1800");
        if (lon_i[i] < -kGridLonMax || lon_i[i] > kGridLonMax)
            fail(at("grid.lon_i", i), "outside -3600..3600");
        const int64_t k = gridKey(int32_t(lat_i[i]), int32_t(lon_i[i]));
        if (!st.grid_.emplace(k, grid_n[i]).second)
            fail(at("grid", i), "a repeated cell");
        cells.emplace_back(grid_n[i], k);
    }
    if (st.grid_.size() > limits.grid_cells_max) {
        // Over a lowered cap: keep the busiest cells.
        std::nth_element(cells.begin(), cells.begin() + std::ptrdiff_t(limits.grid_cells_max), cells.end(),
                         [](const std::pair<uint64_t, int64_t>& a, const std::pair<uint64_t, int64_t>& b) {
                             return a.first != b.first ? a.first > b.first : a.second < b.second;
                         });
        for (std::size_t i = limits.grid_cells_max; i < cells.size(); ++i)
            st.grid_.erase(cells[i].second);
        st.dirty_ = true;
    }

    JsonObject counts = rd.object(top, "", "counts");
    for (std::size_t k = 0; k < st.counts_.size(); ++k) {
        const JsonObject m = rd.object(counts, "counts", kCountNames[k]);
        const std::string ctx = std::string("counts.") + kCountNames[k];
        for (const std::string& name : m.keys()) {
            if (name.empty())
                fail(ctx, "an empty name");
            const std::optional<std::string> v = m.getString(name.c_str());
            if (!v)
                fail(ctx + "." + name, "not a count");
            st.counts_[k][name] = rd.count(*v, ctx + "." + name);
        }
    }

    JsonObject ac = rd.object(top, "", "aircraft");
    const std::string actx = "aircraft";
    const std::size_t n = rd.strings(ac, actx, "key", std::nullopt).size();
    const std::vector<std::optional<TrackId>> keys = rd.keys(ac, actx, "key", n, false);
    const std::vector<int64_t> first_seen = rd.ints(ac, actx, "first_seen", n);
    const std::vector<int64_t> last_seen = rd.ints(ac, actx, "last_seen", n);
    const std::vector<uint64_t> observations = rd.counts(ac, actx, "observations", n);
    const std::vector<uint64_t> positions = rd.counts(ac, actx, "positions", n);
    const std::vector<uint64_t> sessions = rd.counts(ac, actx, "sessions", n);
    const std::vector<uint64_t> sent_updates = rd.counts(ac, actx, "sent_updates", n);
    const std::vector<uint64_t> drops = rd.counts(ac, actx, "drops", n);
    const std::vector<uint64_t> emergencies = rd.counts(ac, actx, "emergencies", n);
    const std::vector<std::optional<double>> max_range_nm = rd.optDoubles(ac, actx, "max_range_nm", n);
    const std::vector<int64_t> max_range_time = rd.ints(ac, actx, "max_range_time", n);
    const std::vector<double> max_range_lat = rd.doubles(ac, actx, "max_range_lat", n);
    const std::vector<double> max_range_lon = rd.doubles(ac, actx, "max_range_lon", n);
    const std::vector<double> max_range_bearing = rd.doubles(ac, actx, "max_range_bearing_deg", n);
    const std::vector<std::string> min_alt = rd.strings(ac, actx, "min_alt_ft", n);
    const std::vector<std::string> max_alt = rd.strings(ac, actx, "max_alt_ft", n);
    const std::vector<std::optional<double>> max_gs = rd.optDoubles(ac, actx, "max_gs_kt", n);
    const std::vector<std::optional<double>> min_rssi = rd.optDoubles(ac, actx, "min_rssi", n);
    const std::vector<std::optional<double>> max_rssi = rd.optDoubles(ac, actx, "max_rssi", n);
    const std::vector<std::string> callsigns = rd.strings(ac, actx, "callsigns", n);
    const std::vector<std::string> squawks = rd.strings(ac, actx, "squawks", n);
    const std::vector<std::string> inputs = rd.strings(ac, actx, "inputs", n);
    const std::vector<std::string> registration = rd.strings(ac, actx, "registration", n);
    const std::vector<std::string> category = rd.strings(ac, actx, "category", n);
    const std::vector<std::string> source = rd.strings(ac, actx, "source", n);

    // "" is unset.
    auto altitude = [](const std::string& value, const std::string& field) -> std::optional<int32_t> {
        if (value.empty())
            return std::nullopt;
        const std::optional<int32_t> v = parseInteger<int32_t>(value);
        if (!v)
            fail(field, "not a 32-bit integer");
        return v;
    };
    auto list = [](const std::string& joined, const std::string& field) {
        std::vector<std::string> out;
        std::istringstream in(joined);
        std::string item;
        while (in >> item)
            out.push_back(item);
        if (out.size() > kStatsListMax)
            fail(field, std::to_string(out.size()) + " entries, at most " + std::to_string(kStatsListMax));
        return out;
    };
    st.ledger_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        AircraftStats a;
        a.id = *keys[i];
        a.first_seen_s = first_seen[i];
        a.last_seen_s = last_seen[i];
        if (a.first_seen_s > a.last_seen_s)
            fail(at("aircraft.first_seen", i), "after last_seen");
        a.observations = observations[i];
        a.positions = positions[i];
        a.sessions = sessions[i];
        a.sent_updates = sent_updates[i];
        a.drops = drops[i];
        a.emergencies = emergencies[i];
        a.max_range_nm = max_range_nm[i];
        a.max_range_time_s = max_range_time[i];
        a.max_range_lat = max_range_lat[i];
        a.max_range_lon = max_range_lon[i];
        a.max_range_bearing_deg = max_range_bearing[i];
        a.min_alt_ft = altitude(min_alt[i], at("aircraft.min_alt_ft", i));
        a.max_alt_ft = altitude(max_alt[i], at("aircraft.max_alt_ft", i));
        if (max_gs[i])
            a.max_gs_kt = static_cast<double>(static_cast<float>(*max_gs[i]));
        a.min_rssi = min_rssi[i];
        a.max_rssi = max_rssi[i];
        a.callsigns = list(callsigns[i], at("aircraft.callsigns", i));
        a.squawks = list(squawks[i], at("aircraft.squawks", i));
        a.inputs = list(inputs[i], at("aircraft.inputs", i));
        if (!registration[i].empty())
            a.registration = registration[i];
        if (!category[i].empty())
            a.category = category[i];
        const std::optional<Source> src = parseSourceName(source[i]);
        if (!src)
            fail(at("aircraft.source", i), "not a source name");
        a.source = *src;
        if (!st.ledger_.emplace(a.id, std::move(a)).second)
            fail(at("aircraft.key", i), "a repeated aircraft");
    }
    if (st.ledger_.size() > limits.aircraft_max) {
        // Over a lowered cap: evicted as observe() would, in whole steps.
        const std::size_t step = st.evictStep();
        const std::size_t over = st.ledger_.size() - limits.aircraft_max;
        st.evictOldest((over + step - 1) / step * step, std::nullopt);
        st.dirty_ = true;
    }
    return st;
}

StatsLoad loadStatsFile(const std::string& path, StatsStore& store)
{
    std::string reason;
    struct stat sb {};
    if (::stat(path.c_str(), &sb) != 0) {
        if (errno == ENOENT)
            return StatsLoad::missing;
        reason = std::string("cannot stat: ") + std::strerror(errno);
    } else if (!S_ISREG(sb.st_mode)) {
        reason = "not a regular file";
    } else if (static_cast<uint64_t>(sb.st_size) > kStatsFileMaxBytes) {
        reason = std::to_string(sb.st_size) + " bytes, over the limit of " + std::to_string(kStatsFileMaxBytes);
    } else {
        std::ifstream in(path, std::ios::binary);
        std::string text;
        if (in) {
            text.reserve(static_cast<std::size_t>(sb.st_size));
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (!in && !in.eof()) {
            reason = "cannot read";
        } else {
            try {
                store = StatsStore::fromJson(text, store.receiver(), store.limits());
                return StatsLoad::loaded;
            } catch (const std::exception& e) {
                reason = e.what();
            }
        }
    }
    const std::string bad = path + ".bad";
    if (::rename(path.c_str(), bad.c_str()) == 0)
        LOG_ERROR(path << ": " << reason << "; renamed to " << bad << "; the statistics start empty");
    else
        LOG_ERROR(path << ": " << reason << "; could not rename it to " << bad << " (" << std::strerror(errno)
                       << "); the statistics start empty");
    return StatsLoad::rejected;
}

void saveStatsFile(const std::string& path, const StatsStore& store) { atomicWrite(path, store.toJson()); }

}  // namespace skycued
