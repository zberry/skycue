#include "stats_report.hpp"

#include "geo.hpp"
#include "json_lite.hpp"
#include "recorder.hpp"
#include "time_util.hpp"
#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>

namespace skycued {

using skycue::AddressType;
using skycue::DropReason;
using skycue::Source;
using skycue::TrackId;
using Time = StatsReport::Time;

namespace {

constexpr int kBarWidth = 40;
constexpr double kLevelFpm = 100;
constexpr double kElevationBinDeg = 5;
constexpr int kElevationBins = 18;
constexpr double kSectorDeg = 30;
constexpr int kSectors = 12;
const char* const kSkipped = "(skipped: receiver position unknown; pass --lat/--lon or --config)";

const DropReason kDropReasons[] = {DropReason::timeout, DropReason::filtered, DropReason::source_lost,
                                   DropReason::shutdown, DropReason::stale};

double seconds(Time::duration d) { return std::chrono::duration<double>(d).count(); }

// Median of an unsorted vector (sorted on return); 0 when empty.
double median(std::vector<double>& v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

std::optional<int32_t> altitudeFt(const TrackObservation& o) { return o.alt_baro_ft ? o.alt_baro_ft : o.alt_geom_ft; }

std::optional<float> verticalRate(const RecordingRecord& r)
{
    if (r.vrate_fpm)
        return r.vrate_fpm;
    return r.obs.baro_rate_fpm ? r.obs.baro_rate_fpm : r.obs.geom_rate_fpm;
}

bool isEmergencySquawk(const std::optional<std::string>& s)
{
    return s && (*s == "7500" || *s == "7600" || *s == "7700");
}

// Per-aircraft totals behind the top-N table and the per-aircraft counts.
struct Acc {
    uint64_t obs = 0;
    std::map<Source, uint64_t> sources;
    std::optional<std::string> callsign, registration, category;
    Time first{}, last{};
    std::optional<int32_t> min_alt, max_alt;
    std::optional<double> max_range;
    double gs_sum = 0;
    uint64_t gs_n = 0;
};

// Bins of `width` starting at index `lo` .. `hi` (inclusive), with obs and distinct aircraft.
std::vector<StatsReport::Bin> makeBins(const std::map<long, std::pair<uint64_t, std::set<TrackId>>>& by_index,
                                       double width)
{
    std::vector<StatsReport::Bin> bins;
    if (by_index.empty())
        return bins;
    const long lo = by_index.begin()->first, hi = by_index.rbegin()->first;
    for (long i = lo; i <= hi; ++i) {
        StatsReport::Bin b;
        b.start = static_cast<double>(i) * width;
        b.end = static_cast<double>(i + 1) * width;
        auto it = by_index.find(i);
        if (it != by_index.end()) {
            b.obs = it->second.first;
            b.aircraft = it->second.second.size();
        }
        bins.push_back(b);
    }
    return bins;
}

void updateMax(std::optional<StatsReport::Extreme>& max, const RecordingRecord& r, double value)
{
    if (!max || value > max->value)
        max = StatsReport::Extreme{r.obs.id, r.time, value};
}

}  // namespace

namespace {

// The FIS-B section: every uat_uplink record, in time order, through one
// FisbDecoder and FisbStore on the records' clock (the store's clock is the
// newest record time so far, as in skycued and skycue-fisb), then a final
// sweep kMosaicQuiet after the last, which publishes mosaics still pending.
void computeFisb(const std::vector<const RecordingRecord*>& recs, StatsReport::Fisb& out)
{
    using namespace fisb;
    FisbDecoder decoder;  // experimental layouts off, as skycued by default
    FisbStore store;
    std::optional<Time> clock;
    std::map<std::string, uint64_t> published;
    std::map<int, uint64_t> text_decoded;
    auto take = [&](const std::vector<FisbEvent>& events) {
        for (const auto& e : events)
            if ((e.kind == FisbEvent::Kind::radar || e.kind == FisbEvent::Kind::raster) && e.mosaic)
                ++published[e.mosaic->key()];
    };
    decoder.onProduct([&](const FisbProduct& p) {
        ++out.products_by_id[p.id];
        if (const auto* text = std::get_if<std::vector<TextRecord>>(&p.body)) {
            for (const auto& r : *text)
                ++text_decoded[r.kind];
        } else if (const auto* t = std::get_if<TwgoParsed>(&p.body)) {
            StatsReport::FisbTwgo& h = out.twgo[p.id];
            if (t->cancel)
                ++h.cancels;
            if (t->text)
                ++(t->text->empty() ? h.keepalive : h.text);
            if (t->has_graphics)
                ++h.graphics;
        }
        take(store.apply(p, *clock));
    });
    for (const RecordingRecord* r : recs) {
        if (r->kind != RecordKind::uat_uplink)
            continue;
        out.present = true;
        if (!clock || r->time > *clock)
            clock = r->time;
        decoder.feed(r->time, r->uplink_hex);
        decoder.sweep(*clock);
        take(store.sweep(*clock));
    }
    if (!out.present)
        return;
    const Time last = *clock + kMosaicQuiet;
    decoder.sweep(last);
    take(store.sweep(last));

    const FisbDecoderCounters& c = decoder.counters();
    out.uplinks = c.uplinks;
    out.apdus = c.apdus;
    out.products = c.products;
    const FisbSnapshot snap = store.snapshot();
    const std::pair<TextRecord::Kind, std::size_t> stored[] = {{TextRecord::metar, snap.metar.size()},
                                                               {TextRecord::taf, snap.taf.size()},
                                                               {TextRecord::pirep, snap.pirep.size()},
                                                               {TextRecord::winds, snap.winds.size()},
                                                               {TextRecord::other, snap.other.size()}};
    for (const auto& s : stored) {
        const auto it = text_decoded.find(s.first);
        const uint64_t decoded = it == text_decoded.end() ? 0 : it->second;
        if (decoded || s.second)
            out.text[textKindName(s.first)] = {decoded, s.second};
    }
    for (const auto& rec : snap.twgo) {
        StatsReport::FisbTwgo& h = out.twgo[rec.key.pid];
        ++h.stored;
        h.cancelled += rec.cancelled;
    }
    for (const auto* list : {&snap.radar, &snap.rasters})
        for (const auto& m : *list) {
            const auto pub = published.find(m.key());
            out.mosaics.push_back({m.key(), m.product, m.product_time, m.blocks.size(),
                                   pub == published.end() ? 0 : pub->second});
        }
    const GlobalBlockStats& bs = decoder.blockStats();
    const FisbStoreStats ss = store.stats();
    out.counters = {
        {"bad_hex", c.bad_hex},
        {"bad_frames", c.bad_frames},
        {"not_app_data", c.not_app_data},
        {"bad_apdu", c.bad_apdu},
        {"unsupported_products", c.unsupported_products},
        {"skipped_experimental", c.skipped_experimental},
        {"segments_open", decoder.segmentsOpen()},
        {"segments_timed_out", c.segments_timed_out},
        {"bad_products", c.bad_products},
        {"rejected_shapes", c.rejected_shapes},
        {"graphics_rejected", c.graphics_rejected},
        {"empty_text", c.empty_text},
        {"blocks_rejected", c.blocks_rejected},
        {"blocks.high_lat_skipped", bs.high_lat_skipped},
        {"store.applied", ss.applied},
        {"store.stale", ss.stale},
        {"store.blocks_merged", ss.blocks_merged},
        {"store.blocks_too_old", ss.blocks_too_old},
        {"store.mosaics_published", ss.mosaics_published},
        {"store.expired", ss.expired},
        {"store.cancelled", ss.cancelled},
    };
}

}  // namespace

std::string trackIdText(const TrackId& id)
{
    return (id.type == AddressType::icao ? "" : "~") + hexAddress(id.address);
}

StatsReport computeStats(const std::vector<RecordingRecord>& input, const StatsOptions& options)
{
    StatsReport rep;
    rep.options = options;
    const bool have_rx = options.rx_lat && options.rx_lon;
    const double range_bin = options.range_bin_nm > 0 ? options.range_bin_nm : 10;
    const int alt_bin = options.alt_bin_ft > 0 ? options.alt_bin_ft : 5000;

    std::vector<const RecordingRecord*> recs;
    recs.reserve(input.size());
    for (const auto& r : input)
        recs.push_back(&r);
    std::stable_sort(recs.begin(), recs.end(),
                     [](const RecordingRecord* a, const RecordingRecord* b) { return a->time < b->time; });

    // ---- overview, gaps ----
    auto& ov = rep.overview;
    ov.records = recs.size();
    {
        std::set<std::string> kinds;
        for (const auto& in : options.inputs) {
            std::size_t start = 0;
            while (start <= in.kind.size()) {
                const std::size_t plus = in.kind.find('+', start);
                const std::string k =
                    in.kind.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                if (!k.empty() && k != "empty")
                    kinds.insert(k);
                if (plus == std::string::npos)
                    break;
                start = plus + 1;
            }
        }
        for (const char* k : {"received", "sent", "readsb-raw"})
            if (kinds.count(k))
                ov.stream_kinds.push_back(k);
    }
    uint64_t obs_records = 0, track_records = 0;
    {
        int64_t bucket = 0;
        uint64_t in_bucket = 0;
        for (std::size_t i = 0; i < recs.size(); ++i) {
            const RecordingRecord& r = *recs[i];
            ++ov.by_kind[recordKindName(r.kind)];
            if (r.kind == RecordKind::obs)
                ++obs_records;
            if (r.kind == RecordKind::update || r.kind == RecordKind::keyframe)
                ++track_records;
            const int64_t sec = static_cast<int64_t>(
                std::floor(seconds(r.time.time_since_epoch())));
            if (i == 0 || sec != bucket) {
                bucket = sec;
                in_bucket = 0;
            }
            ov.peak_per_s = std::max(ov.peak_per_s, ++in_bucket);
            if (i > 0) {
                const double gap = seconds(r.time - recs[i - 1]->time);
                if (gap > options.gap_s)
                    rep.gaps.push_back(StatsReport::Gap{recs[i - 1]->time, r.time, gap});
            }
        }
    }
    if (!recs.empty()) {
        ov.first = recs.front()->time;
        ov.last = recs.back()->time;
        ov.duration_s = seconds(*ov.last - *ov.first);
        ov.mean_per_s = ov.duration_s > 0 ? static_cast<double>(ov.records) / ov.duration_s
                                          : static_cast<double>(ov.records);
    }

    // ---- which records are the observations ----
    const bool use_received = obs_records > 0;
    rep.basis = use_received ? "received" : track_records > 0 ? "sent" : "";
    const auto isObservation = [use_received](const RecordingRecord& r) {
        return use_received ? r.kind == RecordKind::obs
                            : r.kind == RecordKind::update || r.kind == RecordKind::keyframe;
    };

    std::map<TrackId, Acc> acc;
    std::map<std::string, std::set<TrackId>> source_ac, type_ac;
    std::map<std::pair<TrackId, std::string>, std::size_t> squawk_index;
    std::map<long, std::pair<uint64_t, std::set<TrackId>>> range_bins, alt_bins;
    std::vector<double> ranges, speeds;
    std::vector<std::set<TrackId>> sector_ac(kSectors), elev_ac(kElevationBins);
    rep.range.computed = rep.elevation.computed = rep.azimuth.computed = have_rx;
    if (have_rx) {
        for (int i = 0; i < kElevationBins; ++i)
            rep.elevation.bins.push_back(
                StatsReport::Bin{i * kElevationBinDeg, (i + 1) * kElevationBinDeg, 0, 0});
        for (int i = 0; i < kSectors; ++i) {
            StatsReport::Sector s;
            s.start = i * kSectorDeg;
            s.end = (i + 1) * kSectorDeg;
            rep.azimuth.sectors.push_back(s);
        }
    }

    auto& ac = rep.aircraft;
    for (const RecordingRecord* rp : recs) {
        const RecordingRecord& r = *rp;
        if (!isObservation(r))
            continue;
        const TrackObservation& o = r.obs;
        ++ac.observations;
        ++(o.on_ground ? ac.on_ground : ac.airborne);
        const char* src = recording::sourceName(o.source);
        const char* typ = recording::addressTypeName(o.id.type);
        ++ac.by_source[src].obs;
        source_ac[src].insert(o.id);
        ++ac.by_addr_type[typ].obs;
        type_ac[typ].insert(o.id);

        Acc& a = acc[o.id];
        if (a.obs++ == 0)
            a.first = r.time;
        a.last = r.time;
        ++a.sources[o.source];
        if (o.callsign && !o.callsign->empty())
            a.callsign = o.callsign;
        if (o.registration && !o.registration->empty())
            a.registration = o.registration;
        if (o.category && !o.category->empty())
            a.category = o.category;

        if (isEmergencySquawk(o.squawk)) {
            const auto key = std::make_pair(o.id, *o.squawk);
            auto it = squawk_index.find(key);
            if (it == squawk_index.end()) {
                squawk_index.emplace(key, ac.emergency_squawks.size());
                ac.emergency_squawks.push_back(StatsReport::SquawkHit{o.id, *o.squawk, 1, r.time, r.time});
            } else {
                auto& hit = ac.emergency_squawks[it->second];
                ++hit.obs;
                hit.last = r.time;
            }
        }

        const auto alt = altitudeFt(o);
        if (alt) {
            ++rep.altitude.with_altitude;
            auto& bin = alt_bins[static_cast<long>(std::floor(static_cast<double>(*alt) / alt_bin))];
            ++bin.first;
            bin.second.insert(o.id);
            updateMax(rep.altitude.max, r, *alt);
            a.min_alt = a.min_alt ? std::min(*a.min_alt, *alt) : *alt;
            a.max_alt = a.max_alt ? std::max(*a.max_alt, *alt) : *alt;
        }
        if (o.gs_kt && std::isfinite(*o.gs_kt)) {
            speeds.push_back(*o.gs_kt);
            a.gs_sum += *o.gs_kt;
            ++a.gs_n;
        }
        if (auto vr = verticalRate(r)) {
            if (std::isfinite(*vr)) {
                ++rep.speed.vr_samples;
                if (*vr > kLevelFpm)
                    ++rep.speed.climbing;
                else if (*vr < -kLevelFpm)
                    ++rep.speed.descending;
                else
                    ++rep.speed.level;
            }
        }

        if (!have_rx || !o.lat || !o.lon)
            continue;
        const double nm = geo::greatCircleNm(*options.rx_lat, *options.rx_lon, *o.lat, *o.lon);
        ++rep.range.positions;
        ranges.push_back(nm);
        auto& rbin = range_bins[static_cast<long>(std::floor(nm / range_bin))];
        ++rbin.first;
        rbin.second.insert(o.id);
        updateMax(rep.range.max, r, nm);
        a.max_range = a.max_range ? std::max(*a.max_range, nm) : nm;

        const double bearing = geo::bearingDeg(*options.rx_lat, *options.rx_lon, *o.lat, *o.lon);
        const int si = std::min(kSectors - 1, std::max(0, static_cast<int>(std::floor(bearing / kSectorDeg))));
        auto& sector = rep.azimuth.sectors[static_cast<std::size_t>(si)];
        ++sector.obs;
        sector_ac[static_cast<std::size_t>(si)].insert(o.id);
        sector.max_range_nm = sector.max_range_nm ? std::max(*sector.max_range_nm, nm) : nm;

        if (alt) {
            const double e = geo::elevationDeg(*options.rx_lat, *options.rx_lon, options.rx_alt_m, *o.lat, *o.lon,
                                               geo::ftToM(*alt));
            ++rep.elevation.samples;
            if (e < 0) {
                ++rep.elevation.below_zero;
            } else {
                const int ei =
                    std::min(kElevationBins - 1, static_cast<int>(std::floor(e / kElevationBinDeg)));
                ++rep.elevation.bins[static_cast<std::size_t>(ei)].obs;
                elev_ac[static_cast<std::size_t>(ei)].insert(o.id);
            }
        }
    }

    ac.distinct = acc.size();
    for (auto& kv : ac.by_source)
        kv.second.aircraft = source_ac[kv.first].size();
    for (auto& kv : ac.by_addr_type)
        kv.second.aircraft = type_ac[kv.first].size();
    for (const auto& kv : acc) {
        if (kv.second.callsign)
            ++ac.with_callsign;
        if (kv.second.registration)
            ++ac.with_registration;
    }

    if (have_rx) {
        if (!range_bins.empty())
            range_bins[0];  // the histogram always starts at 0 nm
        rep.range.bins = makeBins(range_bins, range_bin);
        rep.range.median_nm = median(ranges);
        for (std::size_t i = 0; i < rep.elevation.bins.size(); ++i)
            rep.elevation.bins[i].aircraft = elev_ac[i].size();
        for (std::size_t i = 0; i < rep.azimuth.sectors.size(); ++i)
            rep.azimuth.sectors[i].aircraft = sector_ac[i].size();
    }
    rep.altitude.bins = makeBins(alt_bins, alt_bin);

    rep.speed.gs_samples = speeds.size();
    if (!speeds.empty()) {
        rep.speed.gs_median = median(speeds);  // sorts
        rep.speed.gs_min = speeds.front();
        rep.speed.gs_max = speeds.back();
    }

    // ---- top N ----
    {
        std::vector<std::pair<TrackId, const Acc*>> order;
        for (const auto& kv : acc)
            order.emplace_back(kv.first, &kv.second);
        std::stable_sort(order.begin(), order.end(),
                         [](const std::pair<TrackId, const Acc*>& x, const std::pair<TrackId, const Acc*>& y) {
                             return x.second->obs > y.second->obs;
                         });  // acc is ordered by id, so ties stay by id
        for (std::size_t i = 0; i < order.size() && i < options.top_n; ++i) {
            const Acc& a = *order[i].second;
            StatsReport::AircraftRow row;
            row.id = order[i].first;
            Source best = Source::other;
            uint64_t best_n = 0;
            for (const auto& s : a.sources)
                if (s.second > best_n) {
                    best = s.first;
                    best_n = s.second;
                }
            row.source = recording::sourceName(best);
            row.callsign = a.callsign;
            row.registration = a.registration;
            row.category = a.category;
            row.obs = a.obs;
            row.first = a.first;
            row.last = a.last;
            row.min_alt_ft = a.min_alt;
            row.max_alt_ft = a.max_alt;
            row.max_range_nm = a.max_range;
            if (a.gs_n)
                row.mean_gs_kt = a.gs_sum / static_cast<double>(a.gs_n);
            rep.top.push_back(row);
        }
    }

    // ---- sent stream ----
    auto& st = rep.sent;
    for (DropReason d : kDropReasons)
        st.drops[recording::dropReasonName(d)] = 0;
    std::map<TrackId, uint64_t> per_ac;
    for (const RecordingRecord* rp : recs) {
        const RecordingRecord& r = *rp;
        switch (r.kind) {
        case RecordKind::update:
        case RecordKind::keyframe:
            st.present = true;
            ++(r.kind == RecordKind::update ? st.updates : st.keyframes);
            if (r.keyframe_flag)
                ++st.keyframe_flagged;
            ++per_ac[r.obs.id];
            break;
        case RecordKind::drop:
            st.present = true;
            ++st.drops[r.drop_reason];
            break;
        case RecordKind::filter_change:
            st.present = true;
            st.filter_changes.emplace_back(r.time, r.note);
            break;
        case RecordKind::obs:
        case RecordKind::uat_uplink: break;
        }
    }
    {
        std::vector<std::pair<TrackId, uint64_t>> order(per_ac.begin(), per_ac.end());
        std::stable_sort(order.begin(), order.end(),
                         [](const std::pair<TrackId, uint64_t>& x, const std::pair<TrackId, uint64_t>& y) {
                             return x.second > y.second;
                         });
        if (order.size() > options.top_n)
            order.resize(options.top_n);
        st.updates_per_aircraft = order;
    }
    if (obs_records > 0 && st.updates > 0) {
        st.compared = true;
        st.received_obs = obs_records;
        st.compression = static_cast<double>(obs_records) / static_cast<double>(st.updates);
        for (const auto& kv : acc)  // acc holds the received aircraft here (use_received)
            if (!per_ac.count(kv.first))
                st.never_sent.push_back(kv.first);
    }
    computeFisb(recs, rep.fisb);
    return rep;
}

// ======================================================================
// Text
// ======================================================================

namespace {

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string fmt(const char* format, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0)
        return "";
    if (static_cast<std::size_t>(n) < sizeof buf)
        return std::string(buf, static_cast<std::size_t>(n));
    std::string out(static_cast<std::size_t>(n) + 1, '\0');
    va_start(ap, format);
    std::vsnprintf(&out[0], out.size(), format, ap);
    va_end(ap);
    out.resize(static_cast<std::size_t>(n));
    return out;
}

unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

std::string timeText(Time t) { return formatIso8601Ms(t); }
// "2026-11-01T15:00:00Z": to the second, for tables.
std::string timeShort(Time t) { return formatIso8601Ms(t).substr(0, 19) + "Z"; }

// "12", "2.5": a bin edge without trailing zeros.
std::string edge(double v) { return fmt("%g", v); }

std::string bar(uint64_t value, uint64_t max)
{
    if (value == 0 || max == 0)
        return "";
    const double len = std::round(static_cast<double>(value) * kBarWidth / static_cast<double>(max));
    return std::string(static_cast<std::size_t>(std::max(1.0, len)), '#');
}

std::string barD(double value, double max)
{
    if (!(value > 0) || !(max > 0))
        return "";
    const double len = std::round(value * kBarWidth / max);
    return std::string(static_cast<std::size_t>(std::max(1.0, len)), '#');
}

int digits(uint64_t v) { return static_cast<int>(std::to_string(v).size()); }

void histogram(std::string& out, const std::vector<StatsReport::Bin>& bins, const char* unit)
{
    std::vector<std::string> labels;
    std::size_t lw = 0;
    uint64_t max_obs = 0, max_ac = 0;
    for (const auto& b : bins) {
        labels.push_back(edge(b.start) + "-" + edge(b.end) + " " + unit);
        lw = std::max(lw, labels.back().size());
        max_obs = std::max(max_obs, b.obs);
        max_ac = std::max(max_ac, b.aircraft);
    }
    const int ow = digits(max_obs), aw = digits(max_ac);
    for (std::size_t i = 0; i < bins.size(); ++i) {
        std::string line = fmt("  %*s | %*llu obs | %*llu ac | ", static_cast<int>(lw), labels[i].c_str(), ow,
                               ull(bins[i].obs), aw, ull(bins[i].aircraft));
        line += bar(bins[i].obs, max_obs);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out += line + "\n";
    }
}

std::string orDash(const std::optional<std::string>& s) { return s ? *s : "-"; }
std::string orDash(const std::optional<int32_t>& v) { return v ? std::to_string(*v) : "-"; }
std::string orDash1(const std::optional<double>& v) { return v ? fmt("%.1f", *v) : "-"; }

std::string pct(uint64_t part, uint64_t whole)
{
    return whole ? fmt("%.1f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole)) : "0.0%";
}

const char* kindOrder[] = {"obs", "uat_uplink", "update", "keyframe", "drop", "filter_change"};

// ---- 11. FIS-B (only when the input holds uat_uplink records) ----
std::string fisbText(const StatsReport::Fisb& f)
{
    std::string out = "\n== FIS-B products by ID ==\n";
    out += fmt("Uplinks: %llu, APDUs: %llu, products decoded: %llu\n", ull(f.uplinks), ull(f.apdus),
               ull(f.products));
    out += "Products by ID (a segmented set counts once):\n";
    if (f.products_by_id.empty())
        out += "  none\n";
    for (const auto& p : f.products_by_id)
        out += fmt("  %5u  %-16s %8llu\n", static_cast<unsigned>(p.first), fisb::productName(p.first),
                   ull(p.second));
    out += "Text records (product 413):\n";
    if (f.text.empty())
        out += "  none\n";
    for (const char* k : {"metar", "taf", "pirep", "winds", "other"}) {
        const auto it = f.text.find(k);
        if (it != f.text.end())
            out += fmt("  %-6s decoded %6llu  stored %6llu\n", k, ull(it->second.first), ull(it->second.second));
    }
    out += "TWGO records by product (decoded halves; records held at the end):\n";
    if (f.twgo.empty())
        out += "  none\n";
    for (const auto& t : f.twgo)
        out += fmt("  %5u  %-16s %llu text, %llu keep-alive, %llu graphics, %llu cancel; stored %llu (%llu "
                   "cancelled)\n",
                   static_cast<unsigned>(t.first), fisb::productName(t.first), ull(t.second.text),
                   ull(t.second.keepalive), ull(t.second.graphics), ull(t.second.cancels), ull(t.second.stored),
                   ull(t.second.cancelled));
    out += "Mosaics (held at the end):\n";
    if (f.mosaics.empty())
        out += "  none\n";
    for (const auto& m : f.mosaics)
        out += fmt("  %-5s  %-16s %s  %llu blocks, published %llu\n", m.key.c_str(), fisb::productName(m.product),
                   timeText(m.product_time).c_str(), ull(m.blocks), ull(m.published));
    out += "Decoder counters:\n";
    for (const auto& c : f.counters)
        out += fmt("  %-28s %llu\n", c.first.c_str(), ull(c.second));
    return out;
}

std::string formatStatsTextBody(const StatsReport& rep);

}  // namespace

std::string formatStatsText(const StatsReport& rep)
{
    std::string out = formatStatsTextBody(rep);
    if (rep.fisb.present)
        out += fisbText(rep.fisb);
    return out;
}

namespace {

std::string formatStatsTextBody(const StatsReport& rep)
{
    const StatsOptions& opt = rep.options;
    std::string out = "skycue-stats report\n";

    // ---- 1. Overview ----
    const auto& ov = rep.overview;
    out += "\n== Overview ==\n";
    out += "Files:\n";
    {
        std::size_t nw = 0, kw = 0;
        for (const auto& in : opt.inputs) {
            nw = std::max(nw, in.name.size());
            kw = std::max(kw, in.kind.size());
        }
        for (const auto& in : opt.inputs) {
            out += fmt("  %-*s  %-*s  %llu records", static_cast<int>(nw), in.name.c_str(), static_cast<int>(kw),
                       in.kind.c_str(), ull(in.records));
            if (in.skipped)
                out += fmt(" (%llu lines skipped)", ull(in.skipped));
            out += "\n";
        }
        if (opt.inputs.empty())
            out += "  (none)\n";
    }
    std::string kinds;
    for (const auto& k : ov.stream_kinds)
        kinds += (kinds.empty() ? "" : ", ") + k;
    out += "Stream kinds: " + (kinds.empty() ? std::string("none") : kinds) + "\n";
    if (ov.first) {
        out += "First record: " + timeText(*ov.first) + "\n";
        out += "Last record:  " + timeText(*ov.last) + "\n";
    }
    out += fmt("Duration: %.1f s\n", ov.duration_s);
    out += fmt("Records: %llu\n", ull(ov.records));
    for (const char* k : kindOrder) {
        auto it = ov.by_kind.find(k);
        if (it != ov.by_kind.end())
            out += fmt("  %-13s %llu\n", k, ull(it->second));
    }
    out += fmt("Records per second: mean %.2f, peak %llu\n", ov.mean_per_s, ull(ov.peak_per_s));
    if (opt.rx_lat && opt.rx_lon)
        out += fmt("Receiver: %.6f, %.6f (from %s); antenna altitude %.1f m\n", *opt.rx_lat, *opt.rx_lon,
                   opt.receiver_from.c_str(), opt.rx_alt_m);
    else
        out += "Receiver: unknown\n";
    out += "Observations: " +
           std::string(rep.basis == "received" ? "received-stream obs records"
                       : rep.basis == "sent"   ? "sent-stream update/keyframe records (no received stream given)"
                                               : "none") +
           "\n";

    // ---- 2. Aircraft ----
    const auto& ac = rep.aircraft;
    out += "\n== Aircraft ==\n";
    out += fmt("Aircraft: %llu\n", ull(ac.distinct));
    out += fmt("Observations: %llu (airborne %llu, on ground %llu)\n", ull(ac.observations), ull(ac.airborne),
               ull(ac.on_ground));
    const auto counts = [&out](const char* title, const char* col, const std::map<std::string, StatsReport::Count>& m) {
        out += std::string(title) + ":\n";
        out += fmt("  %-15s %8s %8s\n", col, "aircraft", "obs");
        for (const auto& kv : m)
            out += fmt("  %-15s %8llu %8llu\n", kv.first.c_str(), ull(kv.second.aircraft), ull(kv.second.obs));
    };
    counts("By source", "source", ac.by_source);
    counts("By address type", "addr_type", ac.by_addr_type);
    out += fmt("With callsign: %llu aircraft\n", ull(ac.with_callsign));
    out += fmt("With registration: %llu aircraft\n", ull(ac.with_registration));
    if (ac.emergency_squawks.empty()) {
        out += "Emergency squawks (7500/7600/7700): none\n";
    } else {
        out += fmt("Emergency squawks (7500/7600/7700): %llu\n", ull(ac.emergency_squawks.size()));
        out += fmt("  %-8s %-6s %8s  %-20s  %-20s\n", "hex", "squawk", "obs", "first", "last");
        for (const auto& h : ac.emergency_squawks)
            out += fmt("  %-8s %-6s %8llu  %-20s  %-20s\n", trackIdText(h.id).c_str(), h.squawk.c_str(), ull(h.obs),
                       timeShort(h.first).c_str(), timeShort(h.last).c_str());
    }

    // ---- 3. Range ----
    out += "\n== Range histogram (" + edge(opt.range_bin_nm) + " nm bins) ==\n";
    if (!rep.range.computed) {
        out += std::string(kSkipped) + "\n";
    } else {
        out += fmt("Positions: %llu obs", ull(rep.range.positions));
        if (rep.range.max)
            out += fmt("; max %.1f nm (%s at %s); median %.1f nm", rep.range.max->value,
                       trackIdText(rep.range.max->id).c_str(), timeText(rep.range.max->time).c_str(),
                       rep.range.median_nm);
        out += "\n";
        histogram(out, rep.range.bins, "nm");
    }

    // ---- 4. Altitude ----
    out += "\n== Altitude histogram (" + std::to_string(opt.alt_bin_ft) + " ft bins; baro, else geometric) ==\n";
    out += fmt("With altitude: %llu obs", ull(rep.altitude.with_altitude));
    if (rep.altitude.max)
        out += fmt("; max %.0f ft (%s at %s)", rep.altitude.max->value, trackIdText(rep.altitude.max->id).c_str(),
                   timeText(rep.altitude.max->time).c_str());
    out += "\n";
    histogram(out, rep.altitude.bins, "ft");

    // ---- 5. Elevation ----
    out += "\n== Elevation histogram (5 deg bins) ==\n";
    if (!rep.elevation.computed) {
        out += std::string(kSkipped) + "\n";
    } else {
        out += fmt("Samples: %llu obs with position and altitude\n", ull(rep.elevation.samples));
        out += fmt("Below 0 deg: %llu\n", ull(rep.elevation.below_zero));
        histogram(out, rep.elevation.bins, "deg");
    }

    // ---- 6. Azimuth ----
    out += "\n== Coverage by azimuth (30 deg sectors) ==\n";
    if (!rep.azimuth.computed) {
        out += std::string(kSkipped) + "\n";
    } else {
        out += fmt("  %-7s %8s %8s %8s\n", "sector", "obs", "aircraft", "max nm");
        double max_nm = 0;
        for (const auto& s : rep.azimuth.sectors) {
            out += fmt("  %03d-%03d %8llu %8llu %8s\n", static_cast<int>(s.start), static_cast<int>(s.end),
                       ull(s.obs), ull(s.aircraft), orDash1(s.max_range_nm).c_str());
            if (s.max_range_nm)
                max_nm = std::max(max_nm, *s.max_range_nm);
        }
        out += "Max range by sector:\n";
        for (const auto& s : rep.azimuth.sectors) {
            std::string line = fmt("  %03d-%03d | %6s nm | ", static_cast<int>(s.start), static_cast<int>(s.end),
                                   orDash1(s.max_range_nm).c_str());
            line += barD(s.max_range_nm.value_or(0), max_nm);
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
            out += line + "\n";
        }
    }

    // ---- 7. Speed ----
    const auto& sp = rep.speed;
    out += "\n== Speed and vertical rate ==\n";
    if (sp.gs_samples)
        out += fmt("Ground speed (%llu obs): min %.1f kt, median %.1f kt, max %.1f kt\n", ull(sp.gs_samples),
                   sp.gs_min, sp.gs_median, sp.gs_max);
    else
        out += "Ground speed: no samples\n";
    if (sp.vr_samples)
        out += fmt("Vertical rate (%llu obs; level = |rate| <= 100 ft/min): climbing %s (%llu), descending %s "
                   "(%llu), level %s (%llu)\n",
                   ull(sp.vr_samples), pct(sp.climbing, sp.vr_samples).c_str(), ull(sp.climbing),
                   pct(sp.descending, sp.vr_samples).c_str(), ull(sp.descending),
                   pct(sp.level, sp.vr_samples).c_str(), ull(sp.level));
    else
        out += "Vertical rate: no samples\n";

    // ---- 8. Gaps ----
    out += "\n== Gaps (> " + edge(opt.gap_s) + " s without records) ==\n";
    if (rep.gaps.empty()) {
        out += "none\n";
    } else {
        out += fmt("  %-24s  %-24s  %10s\n", "start", "end", "length s");
        for (const auto& g : rep.gaps)
            out += fmt("  %-24s  %-24s  %10.1f\n", timeText(g.start).c_str(), timeText(g.end).c_str(), g.length_s);
    }

    // ---- 9. Top N ----
    out += "\n== Top " + std::to_string(opt.top_n) + " aircraft by observations ==\n";
    if (rep.top.empty()) {
        out += "none\n";
    } else {
        out += fmt("  %-8s %-14s %-10s %-4s %-8s %-8s %7s  %-20s  %-20s %7s %7s %7s %7s\n", "hex", "addr_type",
                   "source", "cat", "callsign", "reg", "obs", "first seen", "last seen", "min alt", "max alt",
                   "max nm", "mean gs");
        for (const auto& r : rep.top)
            out += fmt("  %-8s %-14s %-10s %-4s %-8s %-8s %7llu  %-20s  %-20s %7s %7s %7s %7s\n",
                       trackIdText(r.id).c_str(), recording::addressTypeName(r.id.type), r.source.c_str(),
                       orDash(r.category).c_str(), orDash(r.callsign).c_str(), orDash(r.registration).c_str(),
                       ull(r.obs), timeShort(r.first).c_str(), timeShort(r.last).c_str(),
                       orDash(r.min_alt_ft).c_str(), orDash(r.max_alt_ft).c_str(), orDash1(r.max_range_nm).c_str(),
                       orDash1(r.mean_gs_kt).c_str());
    }

    // ---- 10. Sent stream ----
    const auto& st = rep.sent;
    out += "\n== Sent stream ==\n";
    if (!st.present) {
        out += "(no sent-stream records)\n";
        return out;
    }
    out += fmt("Updates: %llu\n", ull(st.updates));
    out += fmt("Keyframe events: %llu %s\n", ull(st.keyframes),
               st.keyframes == 0 ? "(as expected: skycued records every sent track as an update event)"
                                 : "(unexpected: skycued writes only update events)");
    out += fmt("Updates with the keyframe flag: %llu\n", ull(st.keyframe_flagged));
    out += "Drops by reason:\n";
    for (DropReason d : kDropReasons) {
        const char* name = recording::dropReasonName(d);
        out += fmt("  %-12s %8llu\n", name, ull(st.drops.at(name)));
    }
    out += fmt("Filter changes: %llu\n", ull(st.filter_changes.size()));
    for (const auto& f : st.filter_changes)
        out += "  " + timeText(f.first) + "  " + f.second + "\n";
    out += "Updates per aircraft (top " + std::to_string(opt.top_n) + "):\n";
    if (st.updates_per_aircraft.empty()) {
        out += "  none\n";
    } else {
        out += fmt("  %-8s %-14s %8s\n", "hex", "addr_type", "updates");
        for (const auto& u : st.updates_per_aircraft)
            out += fmt("  %-8s %-14s %8llu\n", trackIdText(u.first).c_str(), recording::addressTypeName(u.first.type),
                       ull(u.second));
    }
    if (!st.compared) {
        out += "Compression: needs a received stream with the sent stream\n";
    } else {
        out += fmt("Compression: %llu received obs / %llu updates = %.2f\n", ull(st.received_obs), ull(st.updates),
                   st.compression);
        out += fmt("Received aircraft never sent: %llu\n", ull(st.never_sent.size()));
        for (std::size_t i = 0; i < st.never_sent.size() && i < opt.top_n; ++i)
            out += "  " + trackIdText(st.never_sent[i]) + "\n";
        if (st.never_sent.size() > opt.top_n)
            out += fmt("  ... and %llu more\n", ull(st.never_sent.size() - opt.top_n));
    }
    return out;
}

}  // namespace

// ======================================================================
// JSON
// ======================================================================

namespace {

int64_t i64(uint64_t v) { return static_cast<int64_t>(v); }

void addId(JsonWriter& w, const TrackId& id)
{
    w.add("hex", trackIdText(id));
    w.add("addr_type", recording::addressTypeName(id.type));
}

std::string binsJson(const std::vector<StatsReport::Bin>& bins)
{
    std::vector<std::string> items;
    for (const auto& b : bins) {
        JsonWriter w;
        w.add("bin_start", b.start).add("bin_end", b.end).add("obs", i64(b.obs)).add("aircraft", i64(b.aircraft));
        items.push_back(w.str());
    }
    return jsonArray(items);
}

std::string extremeJson(const std::optional<StatsReport::Extreme>& e, const char* value_key)
{
    if (!e)
        return "null";
    JsonWriter w;
    addId(w, e->id);
    w.add("time", formatIso8601Ms(e->time));
    w.add(value_key, e->value);
    return w.str();
}

std::string countsJson(const std::map<std::string, StatsReport::Count>& m)
{
    JsonWriter w;
    for (const auto& kv : m) {
        JsonWriter c;
        c.add("aircraft", i64(kv.second.aircraft)).add("obs", i64(kv.second.obs));
        w.addRaw(kv.first.c_str(), c.str());
    }
    return w.str();
}

template <class T, class F>
void addOpt(JsonWriter& w, const char* key, const std::optional<T>& v, F conv)
{
    if (v)
        w.add(key, conv(*v));
    else
        w.addRaw(key, "null");
}

}  // namespace

std::string formatStatsJson(const StatsReport& rep)
{
    const StatsOptions& opt = rep.options;
    JsonWriter top;

    // ---- overview ----
    {
        const auto& ov = rep.overview;
        JsonWriter w;
        std::vector<std::string> files;
        for (const auto& in : opt.inputs) {
            JsonWriter f;
            f.add("name", in.name).add("kind", in.kind).add("records", i64(in.records)).add("skipped", i64(in.skipped));
            files.push_back(f.str());
        }
        w.addRaw("files", jsonArray(files));
        std::vector<std::string> kinds;
        for (const auto& k : ov.stream_kinds) {
            std::string s;
            appendJsonString(s, k);
            kinds.push_back(s);
        }
        w.addRaw("stream_kinds", jsonArray(kinds));
        const auto iso = [](Time t) { return formatIso8601Ms(t); };
        addOpt(w, "first", ov.first, iso);
        addOpt(w, "last", ov.last, iso);
        w.add("duration_s", ov.duration_s);
        w.add("records", i64(ov.records));
        JsonWriter bk;
        for (const char* k : kindOrder) {
            auto it = ov.by_kind.find(k);
            if (it != ov.by_kind.end())
                bk.add(k, i64(it->second));
        }
        w.addRaw("by_kind", bk.str());
        w.add("mean_per_s", ov.mean_per_s);
        w.add("peak_per_s", i64(ov.peak_per_s));
        if (opt.rx_lat && opt.rx_lon) {
            JsonWriter rx;
            rx.add("lat", *opt.rx_lat).add("lon", *opt.rx_lon).add("alt_m", opt.rx_alt_m);
            rx.add("from", opt.receiver_from);
            w.addRaw("receiver", rx.str());
        } else {
            w.addRaw("receiver", "null");
        }
        if (rep.basis.empty())
            w.addRaw("basis", "null");
        else
            w.add("basis", rep.basis);
        top.addRaw("overview", w.str());
    }

    // ---- aircraft ----
    {
        const auto& ac = rep.aircraft;
        JsonWriter w;
        w.add("distinct", i64(ac.distinct)).add("observations", i64(ac.observations));
        w.add("airborne", i64(ac.airborne)).add("on_ground", i64(ac.on_ground));
        w.addRaw("by_source", countsJson(ac.by_source));
        w.addRaw("by_addr_type", countsJson(ac.by_addr_type));
        w.add("with_callsign", i64(ac.with_callsign)).add("with_registration", i64(ac.with_registration));
        std::vector<std::string> hits;
        for (const auto& h : ac.emergency_squawks) {
            JsonWriter e;
            addId(e, h.id);
            e.add("squawk", h.squawk).add("obs", i64(h.obs));
            e.add("first", formatIso8601Ms(h.first)).add("last", formatIso8601Ms(h.last));
            hits.push_back(e.str());
        }
        w.addRaw("emergency_squawks", jsonArray(hits));
        top.addRaw("aircraft", w.str());
    }

    // ---- range ----
    if (!rep.range.computed) {
        top.addRaw("range_histogram", "null");
    } else {
        JsonWriter w;
        w.add("bin_nm", opt.range_bin_nm).add("positions", i64(rep.range.positions));
        w.addRaw("bins", binsJson(rep.range.bins));
        w.addRaw("max", extremeJson(rep.range.max, "nm"));
        if (rep.range.positions)
            w.add("median_nm", rep.range.median_nm);
        else
            w.addRaw("median_nm", "null");
        top.addRaw("range_histogram", w.str());
    }

    // ---- altitude ----
    {
        JsonWriter w;
        w.add("bin_ft", static_cast<int64_t>(opt.alt_bin_ft)).add("with_altitude", i64(rep.altitude.with_altitude));
        w.addRaw("bins", binsJson(rep.altitude.bins));
        w.addRaw("max", extremeJson(rep.altitude.max, "ft"));
        top.addRaw("altitude_histogram", w.str());
    }

    // ---- elevation ----
    if (!rep.elevation.computed) {
        top.addRaw("elevation_histogram", "null");
    } else {
        JsonWriter w;
        w.add("bin_deg", kElevationBinDeg).add("samples", i64(rep.elevation.samples));
        w.add("below_zero", i64(rep.elevation.below_zero));
        w.addRaw("bins", binsJson(rep.elevation.bins));
        top.addRaw("elevation_histogram", w.str());
    }

    // ---- azimuth ----
    if (!rep.azimuth.computed) {
        top.addRaw("azimuth", "null");
    } else {
        JsonWriter w;
        w.add("sector_deg", kSectorDeg);
        std::vector<std::string> sectors;
        for (const auto& s : rep.azimuth.sectors) {
            JsonWriter e;
            e.add("bin_start", s.start).add("bin_end", s.end).add("obs", i64(s.obs)).add("aircraft", i64(s.aircraft));
            addOpt(e, "max_range_nm", s.max_range_nm, [](double v) { return v; });
            sectors.push_back(e.str());
        }
        w.addRaw("sectors", jsonArray(sectors));
        top.addRaw("azimuth", w.str());
    }

    // ---- speed ----
    {
        const auto& sp = rep.speed;
        JsonWriter w;
        w.add("gs_samples", i64(sp.gs_samples));
        for (const auto& kv : {std::make_pair("gs_min_kt", sp.gs_min), std::make_pair("gs_median_kt", sp.gs_median),
                               std::make_pair("gs_max_kt", sp.gs_max)}) {
            if (sp.gs_samples)
                w.add(kv.first, kv.second);
            else
                w.addRaw(kv.first, "null");
        }
        w.add("vrate_samples", i64(sp.vr_samples)).add("level_fpm", kLevelFpm);
        w.add("climbing", i64(sp.climbing)).add("descending", i64(sp.descending)).add("level", i64(sp.level));
        top.addRaw("speed", w.str());
    }

    // ---- gaps ----
    {
        JsonWriter w;
        w.add("threshold_s", opt.gap_s);
        std::vector<std::string> gaps;
        for (const auto& g : rep.gaps) {
            JsonWriter e;
            e.add("start", formatIso8601Ms(g.start)).add("end", formatIso8601Ms(g.end)).add("length_s", g.length_s);
            gaps.push_back(e.str());
        }
        w.addRaw("gaps", jsonArray(gaps));
        top.addRaw("gaps", w.str());
    }

    // ---- top aircraft ----
    {
        std::vector<std::string> rows;
        const auto same = [](const std::string& s) { return s; };
        const auto dbl = [](double v) { return v; };
        const auto i32 = [](int32_t v) { return static_cast<int64_t>(v); };
        for (const auto& r : rep.top) {
            JsonWriter e;
            addId(e, r.id);
            e.add("source", r.source);
            addOpt(e, "category", r.category, same);
            addOpt(e, "callsign", r.callsign, same);
            addOpt(e, "registration", r.registration, same);
            e.add("obs", i64(r.obs));
            e.add("first", formatIso8601Ms(r.first)).add("last", formatIso8601Ms(r.last));
            addOpt(e, "min_alt_ft", r.min_alt_ft, i32);
            addOpt(e, "max_alt_ft", r.max_alt_ft, i32);
            addOpt(e, "max_range_nm", r.max_range_nm, dbl);
            addOpt(e, "mean_gs_kt", r.mean_gs_kt, dbl);
            rows.push_back(e.str());
        }
        top.addRaw("top_aircraft", jsonArray(rows));
    }

    // ---- sent ----
    const auto& st = rep.sent;
    if (!st.present) {
        top.addRaw("sent", "null");
    } else {
        JsonWriter w;
        w.add("updates", i64(st.updates)).add("keyframes", i64(st.keyframes));
        w.add("keyframe_flagged", i64(st.keyframe_flagged));
        JsonWriter drops;
        for (DropReason d : kDropReasons) {
            const char* name = recording::dropReasonName(d);
            drops.add(name, i64(st.drops.at(name)));
        }
        w.addRaw("drops", drops.str());
        std::vector<std::string> changes;
        for (const auto& f : st.filter_changes) {
            JsonWriter e;
            e.add("time", formatIso8601Ms(f.first)).add("note", f.second);
            changes.push_back(e.str());
        }
        w.addRaw("filter_changes", jsonArray(changes));
        std::vector<std::string> per;
        for (const auto& u : st.updates_per_aircraft) {
            JsonWriter e;
            addId(e, u.first);
            e.add("updates", i64(u.second));
            per.push_back(e.str());
        }
        w.addRaw("updates_per_aircraft", jsonArray(per));
        if (st.compared) {
            w.add("received_obs", i64(st.received_obs)).add("compression", st.compression);
            std::vector<std::string> never;
            for (const auto& id : st.never_sent) {
                JsonWriter e;
                addId(e, id);
                never.push_back(e.str());
            }
            w.addRaw("never_sent", jsonArray(never));
        } else {
            w.addRaw("received_obs", "null").addRaw("compression", "null").addRaw("never_sent", "null");
        }
        top.addRaw("sent", w.str());
    }

    // ---- FIS-B ----
    const auto& fb = rep.fisb;
    if (!fb.present) {
        top.addRaw("fisb", "null");
    } else {
        JsonWriter w;
        w.add("uplinks", i64(fb.uplinks)).add("apdus", i64(fb.apdus)).add("products", i64(fb.products));
        JsonWriter by_id;
        for (const auto& p : fb.products_by_id)
            by_id.add(std::to_string(p.first).c_str(), i64(p.second));
        w.addRaw("products_by_id", by_id.str());
        JsonWriter text;
        for (const char* k : {"metar", "taf", "pirep", "winds", "other"}) {
            const auto it = fb.text.find(k);
            if (it == fb.text.end())
                continue;
            JsonWriter e;
            e.add("decoded", i64(it->second.first)).add("stored", i64(it->second.second));
            text.addRaw(k, e.str());
        }
        w.addRaw("text", text.str());
        std::vector<std::string> twgo;
        for (const auto& t : fb.twgo) {
            JsonWriter e;
            e.add("product", static_cast<int64_t>(t.first)).add("name", fisb::productName(t.first));
            e.add("text", i64(t.second.text)).add("keepalive", i64(t.second.keepalive));
            e.add("graphics", i64(t.second.graphics)).add("cancels", i64(t.second.cancels));
            e.add("stored", i64(t.second.stored)).add("cancelled", i64(t.second.cancelled));
            twgo.push_back(e.str());
        }
        w.addRaw("twgo", jsonArray(twgo));
        std::vector<std::string> mosaics;
        for (const auto& m : fb.mosaics) {
            JsonWriter e;
            e.add("key", m.key).add("product", static_cast<int64_t>(m.product));
            e.add("name", fisb::productName(m.product)).add("product_time", formatIso8601Ms(m.product_time));
            e.add("blocks", i64(m.blocks)).add("published", i64(m.published));
            mosaics.push_back(e.str());
        }
        w.addRaw("mosaics", jsonArray(mosaics));
        JsonWriter counters;
        for (const auto& c : fb.counters)
            counters.add(c.first.c_str(), i64(c.second));
        w.addRaw("counters", counters.str());
        top.addRaw("fisb", w.str());
    }
    return top.str();
}

}  // namespace skycued
