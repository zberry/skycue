#include "stats_store.hpp"

#include "engine_events.hpp"
#include "geo.hpp"
#include "log.hpp"
#include "recorder.hpp"
#include "time_util.hpp"
#include "track_names.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace skycued {

namespace {

struct HistSpec {
    double start, bin;
    std::size_t size;
};

HistSpec histSpec(StatsHist h)
{
    switch (h) {
    case StatsHist::range: return HistSpec{0, 10, 50};
    case StatsHist::altitude: return HistSpec{0, 1000, 60};
    case StatsHist::speed: return HistSpec{0, 20, 40};
    case StatsHist::vertical_rate: return HistSpec{-8000, 500, 32};
    case StatsHist::elevation: return HistSpec{0, 2, 45};
    case StatsHist::rssi: return HistSpec{-50, 1, 50};
    }
    return HistSpec{0, 1, 1};
}

constexpr StatsHist kAllHists[] = {StatsHist::range,         StatsHist::altitude,  StatsHist::speed,
                                   StatsHist::vertical_rate, StatsHist::elevation, StatsHist::rssi};

int64_t toSeconds(std::chrono::system_clock::time_point t)
{
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

int64_t bucketStart(int64_t t_s, int64_t len)
{
    int64_t q = t_s / len;
    if (t_s % len != 0 && t_s < 0)
        --q;
    return q * len;
}

// The observation's own altitude: 0 on the ground, else baro, else geometric.
std::optional<int32_t> histAltitude(const TrackObservation& obs)
{
    if (obs.on_ground.value_or(false))
        return 0;
    if (obs.alt_baro_ft)
        return obs.alt_baro_ft;
    return obs.alt_geom_ft;
}

bool validCategory(const std::string& c)
{
    return c.size() == 2 && c[0] >= 'A' && c[0] <= 'D' && c[1] >= '0' && c[1] <= '7';
}

bool emergencySquawk(const std::optional<std::string>& sq)
{
    return sq && (*sq == "7500" || *sq == "7600" || *sq == "7700");
}

// Distinct values, newest last, at most kStatsListMax; a repeat moves to the end.
void pushDistinct(std::vector<std::string>& list, const std::string& v)
{
    auto it = std::find(list.begin(), list.end(), v);
    if (it != list.end()) {
        if (it + 1 == list.end())
            return;
        list.erase(it);
    }
    list.push_back(v);
    if (list.size() > kStatsListMax)
        list.erase(list.begin());
}

// floor(x) as a bin index in [0, n): clamped in double before the cast, so
// a huge or non-finite x is never converted out of int's range.
int clampedBin(double x, int n)
{
    if (!(x >= 0))
        return 0;
    if (x >= double(n))
        return n - 1;
    return static_cast<int>(x);
}

// A position the store uses: finite and on the globe.
bool usablePosition(const TrackObservation& obs)
{
    return obs.lat && obs.lon && std::isfinite(*obs.lat) && std::isfinite(*obs.lon) && std::fabs(*obs.lat) <= 90.0 &&
           std::fabs(*obs.lon) <= 180.0;
}

// The positive change of a counter since `base`, which becomes v (a counter
// that went down is a new base).
uint64_t counterDelta(uint64_t& base, uint64_t v)
{
    const uint64_t d = v > base ? v - base : 0;
    base = v;
    return d;
}

}  // namespace

int coverageSector(double bearing_deg)
{
    if (!std::isfinite(bearing_deg))
        return 0;
    double m = std::fmod(std::floor(bearing_deg / 5.0), double(kStatsSectors));
    if (m < 0)
        m += kStatsSectors;
    return static_cast<int>(m);
}

int altitudeBand(std::optional<int32_t> alt_ft, bool on_ground)
{
    if (on_ground)
        return 0;
    if (!alt_ft)
        return 7;
    static const int32_t kEdges[] = {2500, 5000, 10000, 20000, 30000, 40000};
    int band = 0;
    for (int32_t e : kEdges) {
        if (*alt_ft < e)
            return band;
        ++band;
    }
    return band;  // 6: 40k+
}

std::size_t histBin(StatsHist h, double v)
{
    const HistSpec s = histSpec(h);
    const double x = (v - s.start) / s.bin;
    if (!(x >= 0))
        return 0;
    if (x >= double(s.size))
        return s.size - 1;
    return static_cast<std::size_t>(x);
}

std::size_t histSize(StatsHist h) { return histSpec(h).size; }

const char* bandName(int band)
{
    static const char* const kNames[] = {"0-2.5k", "2.5-5k", "5-10k", "10-20k", "20-30k", "30-40k", "40k+", "unknown"};
    if (band < 0 || band >= kStatsBands)
        return "unknown";
    return kNames[band];
}

StatsStore::StatsStore(std::optional<ReceiverPosition> receiver, StatsLimits limits)
    : receiver_(receiver), limits_(limits),
      rssi_by_range_(std::size_t(kStatsRssiRangeBins) * std::size_t(kStatsRssiBins), 0)
{
    minute_.len = 60;
    minute_.cap = kStatsMinuteBuckets;
    hour_.len = 3600;
    hour_.cap = kStatsHourBuckets;
    for (StatsHist h : kAllHists) {
        const HistSpec s = histSpec(h);
        StatsHistogram& hist = hist_[static_cast<std::size_t>(h)];
        hist.start = s.start;
        hist.bin = s.bin;
        hist.counts.assign(s.size, 0);
    }
}

StatsStore::Slot StatsStore::placeIn(Ring& r, int64_t t_s)
{
    const int64_t s = bucketStart(t_s, r.len);
    if (r.buckets.empty()) {
        r.ids_newest.clear();
        r.ids_prev.clear();
        r.buckets.emplace_back();
        r.buckets.back().start_s = s;
        return Slot{&r.buckets.back(), &r.ids_newest};
    }
    const int64_t newest = r.buckets.back().start_s;
    if (s > newest) {
        const int64_t steps = (s - newest) / r.len;
        if (steps >= static_cast<int64_t>(r.cap)) {
            r.buckets.clear();
            r.buckets.emplace_back();
            r.buckets.back().start_s = s;
        } else {
            for (int64_t k = 1; k <= steps; ++k) {
                r.buckets.emplace_back();
                r.buckets.back().start_s = newest + k * r.len;
                if (r.buckets.size() > r.cap)
                    r.buckets.pop_front();
            }
        }
        if (steps == 1) {
            r.ids_prev.swap(r.ids_newest);
            r.ids_newest.clear();
        } else {
            r.ids_prev.clear();
            r.ids_newest.clear();
        }
        return Slot{&r.buckets.back(), &r.ids_newest};
    }
    if (s == newest - r.len) {
        if (r.buckets.size() == 1) {
            r.ids_prev.clear();
            r.buckets.emplace_front();
            r.buckets.front().start_s = s;
        }
        return Slot{&r.buckets[r.buckets.size() - 2], &r.ids_prev};
    }
    // Inside the newest bucket, or (place() clamps) older.
    return Slot{&r.buckets.back(), &r.ids_newest};
}

StatsStore::Placed StatsStore::place(int64_t t_s, Older older)
{
    if (!since_s_)
        since_s_ = t_s;
    if (minute_.buckets.empty()) {
        if (!series_since_s_ || t_s < *series_since_s_)
            series_since_s_ = t_s;
    } else {
        const int64_t newest = minute_.buckets.back().start_s;
        if (older == Older::clamp)
            t_s = std::max(t_s, newest);
        else if (bucketStart(t_s, minute_.len) < newest - minute_.len)
            restartSeries(t_s);
    }
    // A late time can make a bucket before the first one (placeIn's front
    // case): the series then begins there. (since stays the first stored
    // value: it is when accumulation began, not the oldest data time.)
    if (older == Older::restart && series_since_s_ && t_s < *series_since_s_)
        series_since_s_ = t_s;
    Placed p;
    p.minute = placeIn(minute_, t_s);
    p.hour = placeIn(hour_, t_s);
    return p;
}

void StatsStore::restartSeries(int64_t t_s)
{
    for (Ring* r : {&minute_, &hour_}) {
        r->buckets.clear();
        r->ids_newest.clear();
        r->ids_prev.clear();
    }
    series_since_s_ = t_s;
    const auto now = std::chrono::steady_clock::now();
    if (!last_restart_log_ || now - *last_restart_log_ >= std::chrono::minutes(1)) {
        last_restart_log_ = now;
        LOG_INFO("stats: the data clock moved backwards to "
                 << formatIso8601Ms(std::chrono::system_clock::time_point(std::chrono::seconds(t_s)))
                 << "; the series restart");
    }
}

void StatsStore::countInput(std::map<std::string, uint64_t>& m, const std::string& key)
{
    auto it = m.find(key);
    if (it != m.end()) {
        ++it->second;
        return;
    }
    // "other" is the 17th key, never one of the 16 ids.
    const std::size_t ids = m.size() - m.count("other");
    ++m[ids < kStatsInputsMax ? key : std::string("other")];
}

void StatsStore::observe(const TrackObservation& obs, const TrackState& state, const FilterResult& result)
{
    const int64_t t = toSeconds(obs.received_time);
    const Placed p = place(t, Older::restart);
    dirty_ = true;

    auto entry = ledger_.find(obs.id);
    const bool is_new = entry == ledger_.end();
    // A position off the globe (|lat| > 90, |lon| > 180) or not finite counts as none.
    const bool has_pos = usablePosition(obs);
    const bool ranged = has_pos && receiver_ && std::isfinite(result.range_nm);
    const double range = result.range_nm;
    const TrackObservation& id_fields = state.latest;
    const bool emergency =
        (id_fields.emergency && *id_fields.emergency != "none") || emergencySquawk(id_fields.squawk);
    const std::string input = obs.input_id.empty() ? std::string("unknown") : obs.input_id;
    const std::optional<int32_t> alt = histAltitude(obs);

    // The series.
    for (const Slot& slot : {p.minute, p.hour}) {
        StatsBucket& b = *slot.bucket;
        ++b.observations;
        countInput(b.by_input, input);
        if (has_pos)
            ++b.positions;
        if (emergency)
            ++b.emergencies;
        if (slot.ids->insert(obs.id).second)
            ++b.aircraft;
        if (is_new)
            ++b.new_aircraft;
        if (ranged && (!b.max_range_id || range > b.max_range_nm)) {
            b.max_range_nm = range;
            b.max_range_id = obs.id;
        }
    }

    // Histograms.
    auto count = [this](StatsHist h, double v) {
        if (std::isfinite(v))
            ++hist_[static_cast<std::size_t>(h)].counts[histBin(h, v)];
    };
    if (alt)
        count(StatsHist::altitude, *alt);
    if (obs.gs_kt)
        count(StatsHist::speed, *obs.gs_kt);
    if (obs.baro_rate_fpm)
        count(StatsHist::vertical_rate, *obs.baro_rate_fpm);
    else if (obs.geom_rate_fpm)
        count(StatsHist::vertical_rate, *obs.geom_rate_fpm);
    if (obs.rssi)
        count(StatsHist::rssi, *obs.rssi);
    if (ranged)
        count(StatsHist::range, range);
    if (has_pos && receiver_)
        count(StatsHist::elevation, result.elevation_deg);

    // Coverage and signal against range.
    double bearing = 0;
    if (ranged) {
        bearing = geo::bearingDeg(receiver_->lat, receiver_->lon, *obs.lat, *obs.lon);
        const std::optional<int32_t> band_alt = obs.alt_baro_ft ? obs.alt_baro_ft : obs.alt_geom_ft;
        const int band = altitudeBand(band_alt, obs.on_ground.value_or(false));
        CoverageCell& c = coverage_[std::size_t(coverageSector(bearing)) * kStatsBands + std::size_t(band)];
        ++c.n;
        if (!c.id || range > c.max_range_nm) {
            c.max_range_nm = range;
            c.id = obs.id;
            c.time_s = t;
        }
        if (obs.rssi && std::isfinite(*obs.rssi)) {
            const int rb = clampedBin(range / 10.0, kStatsRssiRangeBins);
            const int sb = clampedBin((*obs.rssi + 50.0) / 2.0, kStatsRssiBins);
            ++rssi_by_range_[std::size_t(rb) * kStatsRssiBins + std::size_t(sb)];
        }
    }

    // The position grid.
    if (has_pos) {  // |lat| <= 90, |lon| <= 180: the cell indices fit int32_t
        const auto lat_i = static_cast<int32_t>(std::floor(*obs.lat / kStatsGridCellDeg));
        const auto lon_i = static_cast<int32_t>(std::floor(*obs.lon / kStatsGridCellDeg));
        const int64_t key = gridKey(lat_i, lon_i);
        auto cell = grid_.find(key);
        if (cell != grid_.end())
            ++cell->second;
        else if (grid_.size() < limits_.grid_cells_max)
            grid_.emplace(key, 1);
    }

    // Categorical counts.
    ++counts_[std::size_t(StatsCount::category)]
             [id_fields.category && validCategory(*id_fields.category) ? *id_fields.category : std::string("none")];
    ++counts_[std::size_t(StatsCount::source)][sourceName(obs.source)];
    ++counts_[std::size_t(StatsCount::addr_type)][addressTypeName(obs.id.type)];
    countInput(counts_[std::size_t(StatsCount::input)], input);
    ++counts_[std::size_t(StatsCount::decision)][decisionName(result.decision)];
    if (emergencySquawk(id_fields.squawk))
        ++counts_[std::size_t(StatsCount::squawk_emergency)][*id_fields.squawk];
    if (obs.source == skycue::Source::uat_978)
        uat_seen_ = true;

    // The ledger.
    if (is_new) {
        AircraftStats fresh;
        fresh.id = obs.id;
        fresh.first_seen_s = t;
        fresh.last_seen_s = t;
        fresh.sessions = 1;
        entry = ledger_.emplace(obs.id, std::move(fresh)).first;
    }
    AircraftStats& a = entry->second;
    if (!is_new) {
        if (t - a.last_seen_s > kStatsSessionGapS)
            ++a.sessions;
        a.first_seen_s = std::min(a.first_seen_s, t);
        a.last_seen_s = std::max(a.last_seen_s, t);
    }
    ++a.observations;
    if (has_pos)
        ++a.positions;
    if (emergency)
        ++a.emergencies;
    if (ranged && (!a.max_range_nm || range > *a.max_range_nm)) {
        a.max_range_nm = range;
        a.max_range_time_s = t;
        a.max_range_lat = *obs.lat;
        a.max_range_lon = *obs.lon;
        a.max_range_bearing_deg = bearing;
    }
    if (alt) {
        a.min_alt_ft = a.min_alt_ft ? std::min(*a.min_alt_ft, *alt) : *alt;
        a.max_alt_ft = a.max_alt_ft ? std::max(*a.max_alt_ft, *alt) : *alt;
    }
    if (obs.gs_kt && std::isfinite(*obs.gs_kt)) {
        const double gs = *obs.gs_kt;
        a.max_gs_kt = a.max_gs_kt ? std::max(*a.max_gs_kt, gs) : gs;
    }
    if (obs.rssi && std::isfinite(*obs.rssi)) {
        a.min_rssi = a.min_rssi ? std::min(*a.min_rssi, *obs.rssi) : *obs.rssi;
        a.max_rssi = a.max_rssi ? std::max(*a.max_rssi, *obs.rssi) : *obs.rssi;
    }
    if (id_fields.callsign && !id_fields.callsign->empty())
        pushDistinct(a.callsigns, *id_fields.callsign);
    if (id_fields.squawk && !id_fields.squawk->empty())
        pushDistinct(a.squawks, *id_fields.squawk);
    if (!obs.input_id.empty())
        pushDistinct(a.inputs, obs.input_id);
    if (id_fields.registration)
        a.registration = id_fields.registration;
    if (id_fields.category)
        a.category = id_fields.category;
    a.source = obs.source;

    if (is_new && ledger_.size() > limits_.aircraft_max)
        evictOldest(evictStep(), obs.id);
}

int64_t StatsStore::gridKey(int32_t lat_i, int32_t lon_i)
{
    return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(lat_i)) << 32) |
                                static_cast<uint32_t>(lon_i));
}

std::size_t StatsStore::evictStep() const { return std::max<std::size_t>(1, limits_.aircraft_max / 20); }

void StatsStore::evictOldest(std::size_t n, std::optional<skycue::TrackId> keep)
{
    // The aircraft being observed is never a candidate, even when the data
    // clock is behind every other entry's last_seen.
    std::vector<std::pair<int64_t, skycue::TrackId>> by_age;
    by_age.reserve(ledger_.size());
    for (const auto& kv : ledger_)
        if (!keep || kv.first != *keep)
            by_age.emplace_back(kv.second.last_seen_s, kv.first);
    const std::size_t k = std::min(n, by_age.size());
    if (k == 0)
        return;
    auto older = [](const std::pair<int64_t, skycue::TrackId>& x, const std::pair<int64_t, skycue::TrackId>& y) {
        return x.first < y.first;
    };
    std::nth_element(by_age.begin(), by_age.begin() + std::ptrdiff_t(k - 1), by_age.end(), older);
    for (std::size_t i = 0; i < k; ++i)
        ledger_.erase(by_age[i].second);
    evicted_ += k;
}

void StatsStore::sent(skycue::TrackId id, Time now)
{
    const Placed p = place(toSeconds(now), Older::clamp);
    ++p.minute.bucket->sent_updates;
    ++p.hour.bucket->sent_updates;
    auto it = ledger_.find(id);
    if (it != ledger_.end())
        ++it->second.sent_updates;
    dirty_ = true;
}

void StatsStore::dropped(skycue::TrackId id, skycue::DropReason reason, std::optional<Decision> decision, Time now)
{
    const Placed p = place(toSeconds(now), Older::clamp);
    countDropIn(*p.minute.bucket, reason, decision);
    countDropIn(*p.hour.bucket, reason, decision);
    auto it = ledger_.find(id);
    if (it != ledger_.end())
        ++it->second.drops;
    dirty_ = true;
}

void StatsStore::tick(Time now, const EngineStats& s)
{
    const uint64_t down = counterDelta(base_downlink_, s.uat_downlink);
    const uint64_t up = counterDelta(base_uplink_, s.uat_uplink);
    const uint64_t products = counterDelta(base_products_, s.fisb_products);
    const bool any = down > 0 || up > 0 || products > 0;
    if (minute_.buckets.empty() && !any)
        return;
    const Placed p = place(toSeconds(now), Older::clamp);
    if (any) {
        for (StatsBucket* b : {p.minute.bucket, p.hour.bucket}) {
            b->uat_downlink += down;
            b->uat_uplink += up;
            b->fisb_products += products;
        }
        uat_seen_ = true;
        dirty_ = true;
    }
    StatsBucket& m = *p.minute.bucket;
    StatsBucket& h = *p.hour.bucket;
    const uint64_t web = s.web_clients;
    if (m.tcp_clients != s.tcp_clients || m.web_clients != web) {
        m.tcp_clients = s.tcp_clients;
        m.web_clients = web;
        dirty_ = true;
    }
    if (s.tcp_clients > h.tcp_clients || web > h.web_clients) {
        h.tcp_clients = std::max(h.tcp_clients, s.tcp_clients);
        h.web_clients = std::max(h.web_clients, web);
        dirty_ = true;
    }
}

void StatsStore::clearAll()
{
    for (Ring* r : {&minute_, &hour_}) {
        r->buckets.clear();
        r->ids_newest.clear();
        r->ids_prev.clear();
    }
    for (StatsHistogram& h : hist_)
        std::fill(h.counts.begin(), h.counts.end(), 0);
    coverage_.fill(CoverageCell());
    std::fill(rssi_by_range_.begin(), rssi_by_range_.end(), 0);
    grid_.clear();
    for (auto& m : counts_)
        m.clear();
    ledger_.clear();
    evicted_ = 0;
    uat_seen_ = false;
}

void StatsStore::reset(Time now)
{
    clearAll();
    since_s_ = toSeconds(now);
    series_since_s_ = since_s_;
    dirty_ = true;
}

const std::deque<StatsBucket>& StatsStore::series(StatsRes r) const
{
    return r == StatsRes::minute ? minute_.buckets : hour_.buckets;
}

SeriesTotals StatsStore::totals(StatsRes r, int64_t from_s, int64_t to_s) const
{
    SeriesTotals t;
    for (const StatsBucket& b : series(r)) {
        if (b.start_s < from_s || b.start_s > to_s)
            continue;
        t.observations += b.observations;
        t.positions += b.positions;
        t.new_aircraft += b.new_aircraft;
        t.sent_updates += b.sent_updates;
        t.drops += b.drops_timeout + b.drops_filtered + b.drops_source_lost;
        t.emergencies += b.emergencies;
        if (b.max_range_id && (!t.max_range_id || b.max_range_nm > t.max_range_nm)) {
            t.max_range_nm = b.max_range_nm;
            t.max_range_id = b.max_range_id;
        }
    }
    return t;
}

const StatsHistogram& StatsStore::histogram(StatsHist h) const { return hist_[static_cast<std::size_t>(h)]; }

const CoverageCell& StatsStore::coverage(int sector, int band) const
{
    if (sector < 0 || sector >= kStatsSectors || band < 0 || band >= kStatsBands)
        throw std::out_of_range("StatsStore::coverage: sector or band out of range");
    return coverage_[std::size_t(sector) * kStatsBands + std::size_t(band)];
}

uint64_t StatsStore::rssiByRange(int range_bin, int rssi_bin) const
{
    if (range_bin < 0 || range_bin >= kStatsRssiRangeBins || rssi_bin < 0 || rssi_bin >= kStatsRssiBins)
        return 0;
    return rssi_by_range_[std::size_t(range_bin) * kStatsRssiBins + std::size_t(rssi_bin)];
}

std::vector<GridCell> StatsStore::gridCells() const
{
    std::vector<GridCell> out;
    out.reserve(grid_.size());
    for (const auto& kv : grid_) {
        const auto k = static_cast<uint64_t>(kv.first);
        GridCell c;
        c.lat_i = static_cast<int32_t>(static_cast<uint32_t>(k >> 32));
        c.lon_i = static_cast<int32_t>(static_cast<uint32_t>(k & 0xffffffffu));
        c.n = kv.second;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const GridCell& a, const GridCell& b) {
        return a.lat_i != b.lat_i ? a.lat_i < b.lat_i : a.lon_i < b.lon_i;
    });
    return out;
}

bool StatsStore::gridFull() const { return grid_.size() >= limits_.grid_cells_max; }

const std::map<std::string, uint64_t>& StatsStore::counts(StatsCount c) const
{
    return counts_[static_cast<std::size_t>(c)];
}

const AircraftStats* StatsStore::aircraft(skycue::TrackId id) const
{
    auto it = ledger_.find(id);
    return it == ledger_.end() ? nullptr : &it->second;
}

std::size_t StatsStore::addressSetEntries() const
{
    return minute_.ids_newest.size() + minute_.ids_prev.size() + hour_.ids_newest.size() + hour_.ids_prev.size();
}

}  // namespace skycued
