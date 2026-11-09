#include "web_stats.hpp"

#include "engine.hpp"
#include "filters.hpp"
#include "icao_class.hpp"
#include "json_lite.hpp"
#include "recorder.hpp"
#include "stats_store.hpp"
#include "time_util.hpp"
#include "web_api.hpp"
#include "web_reply.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

using recording::addressTypeName;
using recording::sourceName;
using skycue::AddressType;
using skycue::TrackId;
using SysTime = std::chrono::system_clock::time_point;

constexpr int64_t kMinuteS = 60, kHourS = 3600;
constexpr std::size_t kCountriesMax = 20;  // by_country entries before "other"
constexpr int64_t kPageDefault = 100, kPageMax = 1000;
const AddressType kAddrTypes[] = {AddressType::icao, AddressType::non_icao, AddressType::tisb_trackfile,
                                  AddressType::other};

int64_t floorDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

int64_t secondsOf(SysTime t)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    return floorDiv(ms, 1000);
}

SysTime timeOf(int64_t s) { return SysTime(std::chrono::seconds(s)); }

std::string isoS(int64_t s) { return formatIso8601Ms(timeOf(s)); }

std::string trackKey(const TrackId& id) { return hexAddress(id.address) + "|" + addressTypeName(id.type); }

std::string lower(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// A whole decimal number, optionally signed; nullopt for anything else.
std::optional<int64_t> parseInt(const std::string& s)
{
    const char* b = s.data();
    const char* e = b + s.size();
    if (b != e && *b == '+')
        ++b;
    if (b != e && (*b == '+' || *b == '-') && b != s.data())
        return std::nullopt;  // "+-5", "++5"
    int64_t v = 0;
    const auto r = std::from_chars(b, e, v);
    if (b == e || r.ec != std::errc() || r.ptr != e)
        return std::nullopt;
    return v;
}

// --- JSON pieces (JsonWriter writes objects; arrays are built here) ---

// As JsonWriter::add(double): %.7g with a '.', null when not finite.
void appendNum(std::string& out, double v)
{
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(7);
    s << v;
    out += s.str();
}

void appendNum(std::string& out, uint64_t v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

// One JSON array, element by element.
class Arr {
public:
    Arr() : s_("[") {}
    Arr& u(uint64_t v)
    {
        sep();
        appendNum(s_, v);
        return *this;
    }
    Arr& d(double v)
    {
        sep();
        appendNum(s_, v);
        return *this;
    }
    Arr& str(const std::string& v)
    {
        sep();
        appendJsonString(s_, v);
        return *this;
    }
    Arr& raw(const std::string& json)
    {
        sep();
        s_ += json;
        return *this;
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

std::string strings(const std::vector<std::string>& v)
{
    Arr a;
    for (const auto& s : v)
        a.str(s);
    return a.done();
}

std::string countsObject(const std::map<std::string, uint64_t>& m)
{
    JsonWriter w;
    for (const auto& kv : m)
        w.add(kv.first.c_str(), static_cast<int64_t>(kv.second));
    return w.str();
}

// The centre of a 0.05° grid cell: (i + 0.5) * 0.05 = (2i + 1) * 25 / 1000,
// written exactly ("38.925", "-76.975").
std::string cellCentre(int32_t i)
{
    int64_t v = (2 * int64_t(i) + 1) * 25;
    std::string out;
    if (v < 0) {
        out += '-';
        v = -v;
    }
    out += std::to_string(v / 1000);
    std::string frac = std::to_string(v % 1000);
    frac.insert(0, 3 - frac.size(), '0');
    while (!frac.empty() && frac.back() == '0')
        frac.pop_back();
    if (!frac.empty())
        out += "." + frac;
    return out;
}

std::optional<std::string> receiverJson(const StatsStore& store)
{
    if (const auto rx = store.receiver())
        return JsonWriter().add("lat", rx->lat).add("lon", rx->lon).str();
    return std::nullopt;
}

// --- the time range of summary and series ---

struct Range {
    SysTime from, to;
    int64_t from_s = 0, to_s = 0;  // whole seconds (floored)
    bool empty = false;            // from > to
};

// `from`/`to` from the query; nullopt when valid, else the 400 message.
std::optional<std::string> parseRange(const std::map<std::string, std::string>& params, const StatsStore& store,
                                      SysTime now, Range& out)
{
    std::optional<SysTime> from, to;
    for (const char* k : {"from", "to"}) {
        const auto it = params.find(k);
        if (it == params.end())
            continue;
        const auto t = parseIso8601Ms(it->second);
        if (!t)
            return std::string(k) + " must be an ISO-8601 time such as 2026-11-03T12:00:00Z";
        (k[0] == 'f' ? from : to) = *t;
    }
    if (!to)
        to = now;
    if (!from) {
        // since, or series_since when a late bucket went before it; else the oldest bucket.
        std::optional<int64_t> s = store.sinceS();
        if (const auto ss = store.seriesSinceS())
            s = s ? std::min(*s, *ss) : *ss;
        if (!s && !store.series(StatsRes::minute).empty())
            s = store.series(StatsRes::minute).front().start_s;
        from = s ? timeOf(*s) : *to;
    }
    out.from = *from;
    out.to = *to;
    out.from_s = secondsOf(*from);
    out.to_s = secondsOf(*to);
    out.empty = *from > *to;
    return std::nullopt;
}

int64_t bucketLen(StatsRes r) { return r == StatsRes::minute ? kMinuteS : kHourS; }

// Whether bucket `b` of length `len` overlaps the range.
bool inRange(const StatsBucket& b, int64_t len, const Range& range)
{
    return !range.empty && b.start_s + len > range.from_s && b.start_s <= range.to_s;
}

// The class of a ledger entry, as the page computes it for a track.
const char* entryClass(const AircraftStats& a)
{
    ClassInput in;
    in.hex = hexAddress(a.id.address);
    in.addr_type = a.id.type;
    if (!a.callsigns.empty())
        in.callsign = a.callsigns.back();
    in.category = a.category.value_or("");
    in.registration = a.registration.value_or("");
    return trackClass(in);
}

std::optional<std::string> entryCountry(const AircraftStats& a)
{
    return trackCountry(hexAddress(a.id.address), a.id.type);
}

// --- routes ---

ApiResponse summaryRoute(const std::map<std::string, std::string>& params, const StatsStore& store, SysTime now)
{
    Range range;
    if (auto err = parseRange(params, store, now, range))
        return fail(400, *err);
    // In whole seconds: a nanosecond time-point difference could overflow
    // for extreme dates (ISO-8601 years 0000..9999 fit int64 seconds).
    const StatsRes res = range.to_s - range.from_s <= 86400 ? StatsRes::minute : StatsRes::hour;
    SeriesTotals t;
    if (!range.empty) {
        const int64_t len = bucketLen(res);
        t = store.totals(res, floorDiv(range.from_s, len) * len, range.to_s);
    }

    // Class and country over the ledger entries last seen in the range.
    std::map<std::string, uint64_t> by_class{{"airline", 0}, {"private", 0}, {"military", 0}, {"unknown", 0}};
    std::map<std::string, uint64_t> by_country;
    uint64_t aircraft = 0;
    if (!range.empty) {
        store.forEachAircraft([&](const AircraftStats& a) {
            if (a.last_seen_s < range.from_s || a.last_seen_s > range.to_s)
                return;
            ++aircraft;
            ++by_class[entryClass(a)];
            ++by_country[entryCountry(a).value_or("unknown")];
        });
    }
    std::vector<std::pair<std::string, uint64_t>> countries(by_country.begin(), by_country.end());
    std::stable_sort(countries.begin(), countries.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });  // ties: name order
    Arr country_arr;
    uint64_t other = 0;
    for (std::size_t i = 0; i < countries.size(); ++i) {
        if (i < kCountriesMax)
            country_arr.raw(JsonWriter()
                                .add("name", countries[i].first)
                                .add("n", static_cast<int64_t>(countries[i].second))
                                .str());
        else
            other += countries[i].second;
    }
    if (other > 0)
        country_arr.raw(JsonWriter().add("name", "other").add("n", static_cast<int64_t>(other)).str());

    JsonWriter rw;
    rw.add("from", formatIso8601Ms(range.from))
        .add("to", formatIso8601Ms(range.to))
        .add("res", res == StatsRes::minute ? "minute" : "hour")
        .add("observations", static_cast<int64_t>(t.observations))
        .add("positions", static_cast<int64_t>(t.positions))
        .add("new_aircraft", static_cast<int64_t>(t.new_aircraft))
        .add("sent_updates", static_cast<int64_t>(t.sent_updates))
        .add("drops", static_cast<int64_t>(t.drops))
        .add("emergencies", static_cast<int64_t>(t.emergencies));
    if (t.max_range_id)
        rw.add("max_range_nm", t.max_range_nm).add("max_range_key", trackKey(*t.max_range_id));
    rw.add("aircraft", static_cast<int64_t>(aircraft))
        .addRaw("by_class", countsObject(by_class))
        .addRaw("by_country", country_arr.done());

    static const std::pair<const char*, StatsHist> kHists[] = {
        {"range", StatsHist::range},       {"altitude", StatsHist::altitude},
        {"speed", StatsHist::speed},       {"vertical_rate", StatsHist::vertical_rate},
        {"elevation", StatsHist::elevation}, {"rssi", StatsHist::rssi}};
    JsonWriter hw;
    for (const auto& h : kHists) {
        const StatsHistogram& hist = store.histogram(h.second);
        Arr counts;
        for (uint64_t n : hist.counts)
            counts.u(n);
        hw.addRaw(h.first, JsonWriter().add("start", hist.start).add("bin", hist.bin).addRaw("counts", counts.done()).str());
    }
    static const std::pair<const char*, StatsCount> kCounts[] = {
        {"by_category", StatsCount::category}, {"by_source", StatsCount::source},
        {"by_addr_type", StatsCount::addr_type}, {"by_input", StatsCount::input},
        {"by_decision", StatsCount::decision}, {"by_squawk_emergency", StatsCount::squawk_emergency}};
    JsonWriter cw;
    for (const auto& c : kCounts)
        cw.addRaw(c.first, countsObject(store.counts(c.second)));

    JsonWriter w;
    if (const auto s = store.sinceS())
        w.add("since", isoS(*s));
    if (const auto s = store.seriesSinceS())
        w.add("series_since", isoS(*s));
    w.add("now", formatIso8601Ms(now));
    if (const auto rx = receiverJson(store))
        w.addRaw("receiver", *rx);
    w.add("aircraft_total", static_cast<int64_t>(store.aircraftCount()))
        .add("evicted", static_cast<int64_t>(store.evicted()))
        .add("uat_seen", store.uatSeen())
        .addRaw("range", rw.str())
        .addRaw("hist", hw.str())
        .addRaw("counts", cw.str());
    return reply(200, w.str());
}

struct BucketColumn {
    const char* name;
    uint64_t StatsBucket::*field;
};

// Every integer counter of a StatsBucket, under its field name (as the stats file).
const BucketColumn kBucketColumns[] = {
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

ApiResponse seriesRoute(const std::map<std::string, std::string>& params, const StatsStore& store, SysTime now)
{
    StatsRes res = StatsRes::minute;
    const auto r = params.find("res");
    if (r != params.end()) {
        if (r->second == "hour")
            res = StatsRes::hour;
        else if (r->second != "minute")
            return fail(400, "res must be minute or hour");
    }
    Range range;
    if (auto err = parseRange(params, store, now, range))
        return fail(400, *err);
    const int64_t len = bucketLen(res);
    std::vector<const StatsBucket*> buckets;
    for (const StatsBucket& b : store.series(res))
        if (inRange(b, len, range))
            buckets.push_back(&b);

    Arr t, range_nm, range_key;
    for (const StatsBucket* b : buckets) {
        t.str(isoS(b->start_s));
        range_nm.d(b->max_range_nm);
        if (b->max_range_id)
            range_key.str(trackKey(*b->max_range_id));
        else
            range_key.raw("null");
    }
    JsonWriter w;
    w.add("res", res == StatsRes::minute ? "minute" : "hour").add("bucket_s", len).addRaw("t", t.done());
    for (const auto& c : kBucketColumns) {
        Arr a;
        for (const StatsBucket* b : buckets)
            a.u(b->*c.field);
        w.addRaw(c.name, a.done());
    }
    w.addRaw("max_range_nm", range_nm.done()).addRaw("max_range_key", range_key.done());
    std::map<std::string, Arr> inputs;
    for (const StatsBucket* b : buckets)
        for (const auto& kv : b->by_input)
            inputs.emplace(kv.first, Arr());
    for (auto& kv : inputs)
        for (const StatsBucket* b : buckets) {
            const auto it = b->by_input.find(kv.first);
            kv.second.u(it == b->by_input.end() ? 0 : it->second);
        }
    JsonWriter iw;
    for (const auto& kv : inputs)
        iw.addRaw(kv.first.c_str(), kv.second.done());
    w.addRaw("by_input", iw.str());
    return reply(200, w.str());
}

ApiResponse coverageRoute(const StatsStore& store)
{
    Arr bands, cells;
    for (int b = 0; b < kStatsBands; ++b)
        bands.str(bandName(b));
    for (int s = 0; s < kStatsSectors; ++s)
        for (int b = 0; b < kStatsBands; ++b) {
            const CoverageCell& c = store.coverage(s, b);
            if (c.n == 0)
                continue;
            JsonWriter cw;
            cw.add("sector", static_cast<int64_t>(s)).add("band", static_cast<int64_t>(b)).add("max_range_nm", c.max_range_nm);
            if (c.id)
                cw.add("key", trackKey(*c.id));
            cw.add("time", isoS(c.time_s)).add("n", static_cast<int64_t>(c.n));
            cells.raw(cw.str());
        }
    std::string grid = "[";
    for (const GridCell& g : store.gridCells()) {
        if (grid.size() > 1)
            grid += ',';
        grid += '[';
        grid += cellCentre(g.lat_i);
        grid += ',';
        grid += cellCentre(g.lon_i);
        grid += ',';
        appendNum(grid, g.n);
        grid += ']';
    }
    grid += ']';
    JsonWriter w;
    w.add("sector_deg", static_cast<int64_t>(360 / kStatsSectors))
        .addRaw("bands", bands.done())
        .addRaw("cells", cells.done())
        .addRaw("grid", JsonWriter()
                            .add("cell_deg", kStatsGridCellDeg)
                            .add("cap", static_cast<int64_t>(store.limits().grid_cells_max))
                            .add("full", store.gridFull())
                            .addRaw("cells", grid)
                            .str());
    if (const auto rx = receiverJson(store))
        w.addRaw("receiver", *rx);
    return reply(200, w.str());
}

ApiResponse rssiRoute(const StatsStore& store)
{
    Arr rows;
    for (int r = 0; r < kStatsRssiRangeBins; ++r) {
        Arr row;
        for (int b = 0; b < kStatsRssiBins; ++b)
            row.u(store.rssiByRange(r, b));
        rows.raw(row.done());
    }
    return reply(200, JsonWriter()
                          .add("range_bin_nm", int64_t(10))
                          .add("rssi_bin_db", int64_t(2))
                          .add("rssi_start", int64_t(-50))
                          .addRaw("counts", rows.done())
                          .str());
}

std::string rowJson(const AircraftStats& a)
{
    JsonWriter w;
    const std::string hex = hexAddress(a.id.address);
    auto n = [](uint64_t v) { return static_cast<int64_t>(v); };
    w.add("key", trackKey(a.id))
        .add("hex", hex)
        .add("addr_type", addressTypeName(a.id.type))
        .add("first_seen", isoS(a.first_seen_s))
        .add("last_seen", isoS(a.last_seen_s))
        .add("observations", n(a.observations))
        .add("positions", n(a.positions))
        .add("sessions", n(a.sessions))
        .add("sent_updates", n(a.sent_updates))
        .add("drops", n(a.drops))
        .add("emergencies", n(a.emergencies));
    if (a.max_range_nm)
        w.add("max_range_nm", *a.max_range_nm)
            .add("max_range_time", isoS(a.max_range_time_s))
            .add("max_range_lat", a.max_range_lat)
            .add("max_range_lon", a.max_range_lon)
            .add("max_range_bearing_deg", a.max_range_bearing_deg);
    if (a.min_alt_ft)
        w.add("min_alt_ft", static_cast<int64_t>(*a.min_alt_ft));
    if (a.max_alt_ft)
        w.add("max_alt_ft", static_cast<int64_t>(*a.max_alt_ft));
    if (a.max_gs_kt)
        w.add("max_gs_kt", *a.max_gs_kt);
    if (a.min_rssi)
        w.add("min_rssi", *a.min_rssi);
    if (a.max_rssi)
        w.add("max_rssi", *a.max_rssi);
    w.addRaw("callsigns", strings(a.callsigns)).addRaw("squawks", strings(a.squawks)).addRaw("inputs", strings(a.inputs));
    if (a.registration)
        w.add("registration", *a.registration);
    if (a.category)
        w.add("category", *a.category);
    w.add("source", sourceName(a.source)).add("class", entryClass(a));
    if (const auto c = entryCountry(a))
        w.add("country", *c);
    return w.str();
}

using SortValue = std::function<std::optional<double>(const AircraftStats&)>;

// The sortable row fields; nullopt: the entry has no such value.
const std::map<std::string, SortValue>& sortFields()
{
    using A = const AircraftStats&;
    auto u = [](uint64_t AircraftStats::*f) { return SortValue([f](A a) { return std::optional<double>(double(a.*f)); }); };
    auto ranged = [](const std::function<double(A)>& f) {
        return SortValue([f](A a) { return a.max_range_nm ? std::optional<double>(f(a)) : std::nullopt; });
    };
    // A non-finite value (never stored; defensive) sorts as no value.
    auto finite = [](const SortValue& f) {
        return SortValue([f](A a) {
            const std::optional<double> v = f(a);
            return v && std::isfinite(*v) ? v : std::nullopt;
        });
    };
    static const std::map<std::string, SortValue> fields = [&] {
        std::map<std::string, SortValue> m{
        {"first_seen", [](A a) { return std::optional<double>(double(a.first_seen_s)); }},
        {"last_seen", [](A a) { return std::optional<double>(double(a.last_seen_s)); }},
        {"observations", u(&AircraftStats::observations)},
        {"positions", u(&AircraftStats::positions)},
        {"sessions", u(&AircraftStats::sessions)},
        {"sent_updates", u(&AircraftStats::sent_updates)},
        {"drops", u(&AircraftStats::drops)},
        {"emergencies", u(&AircraftStats::emergencies)},
        {"max_range_nm", ranged([](A a) { return *a.max_range_nm; })},
        {"max_range_time", ranged([](A a) { return double(a.max_range_time_s); })},
        {"max_range_lat", ranged([](A a) { return a.max_range_lat; })},
        {"max_range_lon", ranged([](A a) { return a.max_range_lon; })},
        {"max_range_bearing_deg", ranged([](A a) { return a.max_range_bearing_deg; })},
        {"min_alt_ft", [](A a) { return a.min_alt_ft ? std::optional<double>(*a.min_alt_ft) : std::nullopt; }},
        {"max_alt_ft", [](A a) { return a.max_alt_ft ? std::optional<double>(*a.max_alt_ft) : std::nullopt; }},
        {"max_gs_kt", [](A a) { return a.max_gs_kt; }},
        {"min_rssi", [](A a) { return a.min_rssi; }},
        {"max_rssi", [](A a) { return a.max_rssi; }},
        };
        for (auto& kv : m)
            kv.second = finite(kv.second);
        return m;
    }();
    return fields;
}

bool matches(const AircraftStats& a, const std::string& q)
{
    if (q.empty())
        return true;
    if (hexAddress(a.id.address).find(q) != std::string::npos)
        return true;
    for (const auto& c : a.callsigns)
        if (lower(c).find(q) != std::string::npos)
            return true;
    return a.registration && lower(*a.registration).find(q) != std::string::npos;
}

ApiResponse aircraftPageRoute(const std::map<std::string, std::string>& params, const StatsStore& store)
{
    auto param = [&](const char* k) -> const std::string* {
        const auto it = params.find(k);
        return it == params.end() ? nullptr : &it->second;
    };
    std::string sort = "last_seen";
    if (const auto* s = param("sort"))
        sort = *s;
    const auto field = sortFields().find(sort);
    if (field == sortFields().end())
        return fail(400, "sort must be a numeric field, first_seen or last_seen: '" + sort + "'");
    bool desc = true;
    if (const auto* d = param("dir")) {
        if (*d == "asc")
            desc = false;
        else if (*d != "desc")
            return fail(400, "dir must be asc or desc");
    }
    int64_t limit = kPageDefault, offset = 0;
    if (const auto* l = param("limit")) {
        const auto v = parseInt(*l);
        if (!v)
            return fail(400, "limit must be a whole number");
        limit = std::min(std::max(*v, int64_t(1)), kPageMax);
    }
    if (const auto* o = param("offset")) {
        const auto v = parseInt(*o);
        if (!v || *v < 0)
            return fail(400, "offset must be a whole number >= 0");
        offset = *v;
    }
    const std::string q = lower(param("q") ? *param("q") : std::string());

    struct Entry {
        const AircraftStats* a;
        std::string key;
        std::optional<double> v;
    };
    std::vector<Entry> entries;
    entries.reserve(store.aircraftCount());
    store.forEachAircraft([&](const AircraftStats& a) {
        if (matches(a, q))
            entries.push_back(Entry{&a, trackKey(a.id), field->second(a)});
    });
    std::sort(entries.begin(), entries.end(), [desc](const Entry& x, const Entry& y) {
        if (x.v.has_value() != y.v.has_value())
            return x.v.has_value();  // without the value: last either way
        if (x.v && *x.v != *y.v)
            return desc ? *x.v > *y.v : *x.v < *y.v;
        return x.key < y.key;
    });
    Arr rows;
    for (std::size_t i = std::size_t(std::min<int64_t>(offset, int64_t(entries.size())));
         i < entries.size() && int64_t(i) < offset + limit; ++i)
        rows.raw(rowJson(*entries[i].a));
    return reply(200, JsonWriter()
                          .add("total", static_cast<int64_t>(entries.size()))
                          .add("offset", offset)
                          .add("limit", limit)
                          .add("evicted", static_cast<int64_t>(store.evicted()))
                          .addRaw("rows", rows.done())
                          .str());
}

ApiResponse aircraftEntryRoute(const std::string& hex, const std::map<std::string, std::string>& params,
                               const StatsStore& store)
{
    const auto addr = parseHexAddress(hex);
    if (!addr)
        return fail(400, "the address must be 6 hex digits: '" + hex + "'");
    std::vector<AddressType> types(std::begin(kAddrTypes), std::end(kAddrTypes));
    const auto it = params.find("addr_type");
    if (it != params.end()) {
        const auto t = std::find_if(types.begin(), types.end(),
                                    [&](AddressType a) { return it->second == addressTypeName(a); });
        if (t == types.end())
            return fail(400, "addr_type must be icao, non_icao, tisb_trackfile or other");
        types = {*t};
    }
    for (AddressType t : types)
        if (const AircraftStats* a = store.aircraft(TrackId{*addr, t}))
            return reply(200, rowJson(*a));
    return fail(404, "no such aircraft in the statistics: " + hex);
}

constexpr std::string_view kAircraftPrefix = "/api/stats/aircraft/";

}  // namespace

bool isStatsPath(const std::string& path) { return path.compare(0, 11, "/api/stats/") == 0; }

ApiResponse statsReadRoute(const ApiRequest& request, const StatsStore& store, SysTime now)
{
    const std::string& path = request.path;
    enum class Route { summary, series, coverage, rssi, aircraft, entry };
    Route route;
    if (path == "/api/stats/summary")
        route = Route::summary;
    else if (path == "/api/stats/series")
        route = Route::series;
    else if (path == "/api/stats/coverage")
        route = Route::coverage;
    else if (path == "/api/stats/rssi")
        route = Route::rssi;
    else if (path == "/api/stats/aircraft")
        route = Route::aircraft;
    else if (path.compare(0, kAircraftPrefix.size(), kAircraftPrefix) == 0 &&
             path.find('/', kAircraftPrefix.size()) == std::string::npos)
        route = Route::entry;
    else
        return fail(404, "no such API path: " + path);
    if (request.method != "GET")
        return notAllowed("GET");
    const auto params = queryParams(request.query);
    switch (route) {
    case Route::summary: return summaryRoute(params, store, now);
    case Route::series: return seriesRoute(params, store, now);
    case Route::coverage: return coverageRoute(store);
    case Route::rssi: return rssiRoute(store);
    case Route::aircraft: return aircraftPageRoute(params, store);
    case Route::entry: return aircraftEntryRoute(path.substr(kAircraftPrefix.size()), params, store);
    }
    return fail(404, "no such API path: " + path);
}

ApiResponse handleStatsApi(const ApiRequest& request, Engine& engine, StatsApiState* state)
{
    if (request.path != "/api/stats/reset")
        return statsReadRoute(request, engine.statsStore(), engine.dataClock());
    if (request.method != "POST")
        return notAllowed("POST");
    if (auto rejected = rejectUnsafe(request))
        return *rejected;
    // PropertyTree reads "[]" as an empty object: the text must start with '{'.
    const auto first = request.body.find_first_not_of(" \t\r\n");
    const auto obj = JsonObject::parse(request.body);
    if (!obj || first == std::string::npos || request.body[first] != '{')
        return fail(400, "the body must be a JSON object");
    const auto keys = obj->keys();
    if (!keys.empty())
        return fail(400, "unknown key '" + keys.front() + "'");
    const auto now = std::chrono::steady_clock::now();
    if (state && state->last_reset && now - *state->last_reset < kStatsResetGap)
        return fail(429, "the statistics were reset less than 5 s ago");
    engine.resetStats("ui " + (request.peer.empty() ? std::string("(unknown)") : request.peer));
    if (state)
        state->last_reset = now;
    return reply(200, JsonWriter().add("ok", true).str());
}

}  // namespace webapi
}  // namespace skycued
