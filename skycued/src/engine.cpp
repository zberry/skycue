#include "engine.hpp"

#include "log.hpp"

#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skycued {

namespace {

using boost::system::error_code;
using steady = std::chrono::steady_clock;

constexpr std::chrono::seconds kTickInterval{1};
constexpr std::chrono::seconds kStatsInterval{10};

IdList loadList(const std::string& path)
{
    return path.empty() ? IdList() : IdList::loadFile(path);
}

FilterSet makeFilters(const Config& c)
{
    if (!std::isfinite(c.receiver_lat) || !std::isfinite(c.receiver_lon))
        throw std::runtime_error("the receiver position (RECEIVER_LAT, RECEIVER_LON) is required");
    return FilterSet(ReceiverPosition{c.receiver_lat, c.receiver_lon, c.receiver_alt_m + c.antenna_height_m},
                     FilterConfig{c.max_range_nm, c.min_elevation_deg}, loadList(c.allow_list_file),
                     loadList(c.block_list_file));
}

Thresholds makeThresholds(const Config& c)
{
    Thresholds t;
    t.min_interval_s = c.min_interval_s;
    t.max_interval_s = c.max_interval_s;
    return t;
}

std::string trimmed(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

const char* listName(ListKind list) { return list == ListKind::allow ? "allow" : "block"; }

EngineEvent kindEvent(EventKind kind)
{
    return EngineEvent{kind, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
}

}  // namespace

Engine::Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<ObservationSource> source,
               std::unique_ptr<Dump978RawSource> uat, Forwarder& forwarder, Recorder* received, Recorder* sent,
               Recorder* fisb)
    : io_(io),
      config_(config),
      source_(std::move(source)),
      uat_(std::move(uat)),
      forwarder_(forwarder),
      received_(received),
      sent_(sent),
      fisb_rec_(fisb),
      filters_(makeFilters(config_)),
      thresholds_(makeThresholds(config_)),
      track_timeout_(std::max<long long>(1, std::llround(config_.track_timeout_s))),
      tick_timer_(io),
      stats_timer_(io),
      lost_timer_(io)
{
    if (!source_)
        throw std::invalid_argument("Engine: a source is required");
    source_->onObservation([this](const TrackObservation& obs) { handleObservation(obs); });
    source_->onConnection([this](bool up) { handleConnection(up); });
    if (config_.fisb_enable) {
        fisb_decoder_ = std::make_unique<fisb::FisbDecoder>(config_.fisb_experimental);
        fisb_decoder_->onProduct([this](const fisb::FisbProduct& p) {
            auto events = fisb_store_.apply(p, dataNow());
            handleFisbEvents(events);
        });
    }
    source_->onUplink([this](std::chrono::system_clock::time_point when, const std::string& hex) {
        handleUplink(when, hex);
    });
    if (uat_) {
        uat_->onUplink([this](std::chrono::system_clock::time_point when, const std::string& hex) {
            handleUplink(when, hex);
        });
        uat_->onConnection([this](bool up) { uat_connected_ = up; });
    }
    forwarder_.setKeyframeProvider([this] {
        std::vector<skycue::Track> out;
        table_.forEach([&](TrackState& s) {
            if (s.currently_forwarded)
                out.push_back(toTrack(s, true, s.last_sent ? s.last_sent->alt_is_geom : false));
        });
        return out;
    });
}

Engine::~Engine()
{
    stop();
    forwarder_.setKeyframeProvider({});
    alive_.reset();  // queued timer handlers do nothing
}

std::string Engine::filterDescription() const
{
    std::ostringstream ss;
    ss << "startup: receiver=" << config_.receiver_lat << "," << config_.receiver_lon
       << " alt_m=" << (config_.receiver_alt_m + config_.antenna_height_m)
       << " max_range_nm=" << filters_.config().max_range_nm
       << " min_elevation_deg=" << filters_.config().min_elevation_deg << " allow_list="
       << (config_.allow_list_file.empty() ? "none" : config_.allow_list_file) << " (" << filters_.allow().size()
       << " entries) block_list=" << (config_.block_list_file.empty() ? "none" : config_.block_list_file) << " ("
       << filters_.block().size() << " entries)";
    return ss.str();
}

void Engine::start()
{
    if (started_ || stopped_)
        return;
    forwarder_.start();  // throws when the port cannot be bound; the Engine stays unstarted
    started_ = true;
    const std::string description = filterDescription();
    LOG_INFO("filters " << description);
    if (sent_)
        sent_->writeFilterChange(description);
    source_->start();
    if (uat_)
        uat_->start();
    armTick();
    armStats();
}

void Engine::stop()
{
    if (stopped_)
        return;
    stopped_ = true;
    sink_ = nullptr;  // no event from here on
    if (!started_)
        return;
    source_->stop();
    if (uat_)
        uat_->stop();
    ++lost_gen_;
    error_code ignored;
    tick_timer_.cancel(ignored);
    stats_timer_.cancel(ignored);
    lost_timer_.cancel(ignored);

    // The Forwarder broadcasts the shutdown drops from the keyframe provider.
    uint64_t shutdown_drops = 0;
    table_.forEach([&](TrackState& s) {
        if (!s.currently_forwarded)
            return;
        ++shutdown_drops;
        if (sent_)
            sent_->writeDrop(s.id, skycue::DropReason::shutdown);
    });
    sent_drops_ += shutdown_drops;
    forwarder_.stop();
    table_.forEach([](TrackState& s) { s.currently_forwarded = false; });
    if (received_)
        received_->flush();
    if (sent_)
        sent_->flush();
    if (fisb_rec_)
        fisb_rec_->flush();
    LOG_INFO("stopping: " << shutdown_drops << " shutdown drops; " << statsLine());
}

void Engine::handleObservation(const TrackObservation& obs)
{
    if (stopped_)
        return;
    ++observations_;
    advanceDataClock(obs.received_time);
    if (received_)
        received_->writeObservation(obs);
    process(table_.apply(obs), last_obs_steady_, obs.lat.has_value());
}

void Engine::handleUplink(std::chrono::system_clock::time_point when, const std::string& hex)
{
    if (stopped_)
        return;
    if (received_)
        received_->writeUplink(when, hex);
    if (!fisb_decoder_)
        return;
    // Uplinks move the data clock too, so a recording of uplinks alone still
    // has its own clock.
    advanceDataClock(when);
    fisb_decoder_->feed(when, hex);
}

void Engine::handleFisbEvents(std::vector<fisb::FisbEvent>& events)
{
    if (events.empty() || stopped_)
        return;
    const auto now = dataNow();
    for (const auto& e : events) {
        if (fisb_rec_)
            fisb_rec_->writeFisb(e, now);
        if (!sink_)
            continue;
        FisbNotice n;
        n.product = e.product;
        n.key = e.key;
        n.reason = e.reason;
        n.expires = e.expires;
        EventKind kind = EventKind::fisb_expired;
        switch (e.kind) {
        case fisb::FisbEvent::Kind::text:
            kind = EventKind::fisb_text;
            n.text = e.text;
            if (e.text)
                n.product_time = e.text->issued;
            break;
        case fisb::FisbEvent::Kind::twgo:
            kind = e.twgo && e.twgo->has_graphics ? EventKind::fisb_shape : EventKind::fisb_text;
            n.twgo = e.twgo;
            break;
        case fisb::FisbEvent::Kind::radar:
        case fisb::FisbEvent::Kind::raster:
            kind = e.kind == fisb::FisbEvent::Kind::radar ? EventKind::fisb_radar : EventKind::fisb_raster;
            if (e.mosaic) {
                n.product_time = e.mosaic->product_time;
                n.block_count = e.mosaic->blocks.size();
            }
            break;
        case fisb::FisbEvent::Kind::expired:
            n.twgo = e.twgo;
            break;
        }
        EngineEvent ev = kindEvent(kind);
        ev.fisb = std::move(n);
        emit(ev);
    }
}

void Engine::process(TrackState& state, steady::time_point now, bool positioned)
{
    decide(state, now);
    if (positioned && sink_ && !stopped_) {
        EngineEvent ev = kindEvent(EventKind::observed);
        ev.track = trackSnapshot(state, filters_.evaluate(state));
        emit(ev);
    }
}

void Engine::decide(TrackState& state, steady::time_point now)
{
    const SendReason reason = shouldSend(state, now, thresholds_);
    if (reason == SendReason::none) {
        if (!state.latest.lat)
            return;
        // A forwarded track is evaluated on every positioned observation, so
        // it is dropped as soon as it leaves the area. One that is not
        // forwarded is re-checked so it comes back as soon as it is eligible
        // again, at most once per min_interval_s since its last send, so a
        // track on a filter boundary cannot flap at the observation rate.
        if (!state.currently_forwarded && state.last_sent &&
            now - state.last_sent_at < std::chrono::duration<double>(thresholds_.min_interval_s))
            return;
    }
    const FilterResult result = filters_.evaluate(state);
    if (isSend(result.decision)) {
        if (reason == SendReason::none && state.currently_forwarded)
            return;  // still eligible; nothing worth sending yet
        forward(state, result, now);
    } else if (state.currently_forwarded) {
        drop(state, skycue::DropReason::filtered);
    }
}

void Engine::forward(TrackState& state, const FilterResult& result, steady::time_point now)
{
    const skycue::Track track = toTrack(state, false, result.alt_is_geom);
    forwarder_.sendUpdate(track);
    markSent(state, track, now);
    state.currently_forwarded = true;
    ++sent_updates_;
    if (sent_)
        sent_->writeTrack(track, "update");
    if (sink_) {
        EngineEvent ev = kindEvent(EventKind::update);
        ev.track = trackSnapshot(state, result);
        emit(ev);
    }
}

void Engine::drop(TrackState& state, skycue::DropReason reason)
{
    forwarder_.sendDrop(state.id, reason);
    state.currently_forwarded = false;
    countDrop(reason);
    if (sent_)
        sent_->writeDrop(state.id, reason);
    emitDrop(state.id, reason);
}

void Engine::emitDrop(skycue::TrackId id, skycue::DropReason reason)
{
    if (!sink_)
        return;
    EngineEvent ev = kindEvent(EventKind::drop);
    ev.id = id;
    ev.reason = reason;
    emit(ev);
}

void Engine::countDrop(skycue::DropReason reason)
{
    ++sent_drops_;
    switch (reason) {
    case skycue::DropReason::timeout: ++drops_timeout_; break;
    case skycue::DropReason::filtered: ++drops_filtered_; break;
    case skycue::DropReason::source_lost: ++drops_source_lost_; break;
    default: break;
    }
}

void Engine::emit(const EngineEvent& event)
{
    if (sink_ && !stopped_)
        sink_(event);
}

void Engine::onEvent(std::function<void(const EngineEvent&)> sink)
{
    if (!stopped_)
        sink_ = std::move(sink);
}

void Engine::applied(const std::string& note, EventKind kind)
{
    LOG_INFO("filters " << note);
    if (sent_)
        sent_->writeFilterChange(note);
    emit(kindEvent(kind));
    reevaluate();
}

void Engine::reevaluate()
{
    // One pass, no allocation beyond the sends themselves. drop() and
    // forward() change only the state they are given, never the table.
    const auto now = steady::now();
    table_.forEach([&](TrackState& s) {
        const FilterResult r = filters_.evaluate(s);
        if (s.currently_forwarded) {
            if (!isSend(r.decision))
                drop(s, skycue::DropReason::filtered);
        } else if (isSend(r.decision)) {  // implies a position
            forward(s, r, now);
        }
    });
}

void Engine::setFilterConfig(FilterConfig config, const std::string& origin)
{
    if (stopped_)
        return;
    if (!std::isfinite(config.max_range_nm) || !(config.max_range_nm > 0))
        throw std::invalid_argument("max_range_nm must be greater than 0");
    if (!(config.min_elevation_deg >= -90 && config.min_elevation_deg <= 90))
        throw std::invalid_argument("min_elevation_deg must be in [-90,90]");
    filters_.setConfig(config);
    std::ostringstream ss;
    ss << origin << ": max_range_nm=" << config.max_range_nm << " min_elevation_deg=" << config.min_elevation_deg;
    applied(ss.str(), EventKind::filters_changed);
}

void Engine::setLists(IdList allow, IdList block, const std::string& origin)
{
    if (stopped_)
        return;
    std::ostringstream ss;
    ss << origin << ": allow_list=" << allow.size() << " entries block_list=" << block.size() << " entries";
    filters_.setLists(std::move(allow), std::move(block));
    applied(ss.str(), EventKind::lists_changed);
}

void Engine::addListEntry(ListKind list, const std::string& entry, const std::string& origin)
{
    if (stopped_ || !filters_.addEntry(list, entry))  // throws std::invalid_argument for a bad entry
        return;
    applied(origin + ": " + listName(list) + " += " + trimmed(entry), EventKind::lists_changed);
}

void Engine::removeListEntry(ListKind list, const std::string& entry, const std::string& origin)
{
    if (stopped_ || !filters_.removeEntry(list, entry))
        return;
    applied(origin + ": " + listName(list) + " -= " + trimmed(entry), EventKind::lists_changed);
}

void Engine::dropAllForwarded(skycue::DropReason reason)
{
    table_.forEach([&](TrackState& s) {
        if (s.currently_forwarded)
            drop(s, reason);
    });
}

void Engine::handleConnection(bool up)
{
    if (stopped_)
        return;
    const bool was = source_connected_;
    source_connected_ = up;
    error_code ignored;
    if (up) {
        if (!was)
            LOG_INFO("source connected");
        ++lost_gen_;
        lost_timer_.cancel(ignored);
        return;
    }
    if (!was)
        return;  // already down; the timer from the first loss keeps running
    LOG_WARN("source lost; forwarded tracks are dropped if it is not back within " << track_timeout_.count()
                                                                                    << " s");
    const unsigned gen = ++lost_gen_;
    lost_timer_.expires_after(track_timeout_);  // the same value expire() uses
    std::weak_ptr<char> alive = alive_;
    lost_timer_.async_wait([this, gen, alive](const error_code& ec) {
        if (ec || alive.expired() || gen != lost_gen_ || stopped_ || source_connected_)
            return;
        size_t n = 0;
        table_.forEach([&](TrackState& s) { n += s.currently_forwarded ? 1 : 0; });
        LOG_WARN("source still lost after " << track_timeout_.count() << " s: dropping " << n
                                            << " forwarded tracks");
        dropAllForwarded(skycue::DropReason::source_lost);
    });
}

void Engine::tick()
{
    if (have_data_clock_) {
        const auto now = dataNow();
        std::unordered_set<skycue::TrackId> forwarded;
        table_.forEach([&](TrackState& s) {
            if (s.currently_forwarded)
                forwarded.insert(s.id);
        });
        // A track that ages out while the source is down went because of it.
        const auto reason = source_connected_ ? skycue::DropReason::timeout : skycue::DropReason::source_lost;
        for (const auto& id : table_.expire(now, track_timeout_)) {
            if (!forwarded.count(id)) {
                if (sink_) {
                    EngineEvent ev = kindEvent(EventKind::expired);
                    ev.id = id;
                    emit(ev);
                }
                continue;
            }
            forwarder_.sendDrop(id, reason);
            countDrop(reason);
            if (sent_)
                sent_->writeDrop(id, reason);
            emitDrop(id, reason);
        }
    }
    if (fisb_decoder_ && have_data_clock_) {
        const auto now = dataNow();
        fisb_decoder_->sweep(now);
        auto events = fisb_store_.sweep(now);
        handleFisbEvents(events);
    }
    if (received_)
        received_->flush();
    if (sent_)
        sent_->flush();
    if (fisb_rec_)
        fisb_rec_->flush();
    if (sink_) {
        EngineEvent ev = kindEvent(EventKind::stats);
        ev.stats = stats();
        emit(ev);
    }
}

void Engine::advanceDataClock(std::chrono::system_clock::time_point t)
{
    // Monotonic: the newest of the observation and uplink times seen (plus
    // the steady time since), never moved back by an older one.
    if (have_data_clock_ && t <= dataNow())
        return;
    have_data_clock_ = true;
    last_obs_time_ = t;
    last_obs_steady_ = steady::now();
}

std::chrono::system_clock::time_point Engine::dataNow() const
{
    if (!have_data_clock_)
        return std::chrono::system_clock::now();
    return last_obs_time_ +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(steady::now() - last_obs_steady_);
}

void Engine::armTick()
{
    tick_timer_.expires_after(kTickInterval);
    std::weak_ptr<char> alive = alive_;
    tick_timer_.async_wait([this, alive](const error_code& ec) {
        if (ec || alive.expired() || stopped_)
            return;
        tick();
        armTick();
    });
}

void Engine::armStats()
{
    stats_timer_.expires_after(kStatsInterval);
    std::weak_ptr<char> alive = alive_;
    stats_timer_.async_wait([this, alive](const error_code& ec) {
        if (ec || alive.expired() || stopped_)
            return;
        LOG_INFO(statsLine());
        armStats();
    });
}

EngineStats Engine::stats() const
{
    EngineStats s;
    s.observations = observations_;
    s.sent_updates = sent_updates_;
    s.sent_drops = sent_drops_;
    s.tracks_total = table_.size();
    table_.forEach([&](const TrackState& t) { s.tracks_forwarded += t.currently_forwarded ? 1 : 0; });
    if (uat_) {
        s.uat_downlink = uat_->downlinkFrames();
        s.uat_uplink = uat_->uplinkFrames();
    }
    s.readsb_connected = source_connected_;
    s.dump978_connected = uat_ && uat_connected_;
    s.tcp_clients = forwarder_.tcpClientCount();
    s.udp_targets = forwarder_.udpTargetCount();
    s.drops_timeout = drops_timeout_;
    s.drops_filtered = drops_filtered_;
    s.drops_source_lost = drops_source_lost_;
    s.recorder_dropped_received = received_ ? received_->droppedRecords() : 0;
    s.recorder_dropped_sent = sent_ ? sent_->droppedRecords() : 0;
    s.web_clients = web_clients_;
    if (fisb_decoder_) {
        const auto& c = fisb_decoder_->counters();
        s.fisb_uplinks = c.uplinks;
        s.fisb_products = c.products;
        s.fisb_bad_frames = c.bad_hex + c.bad_frames;
        s.fisb_segments_open = c.segments_open;
        s.fisb_mosaic_blocks = fisb_store_.mosaicBlocks();
    }
    return s;
}

TrackSnapshot Engine::trackSnapshot(const TrackState& state, const FilterResult& result) const
{
    TrackSnapshot t;
    t.track = toTrack(state, false, result.alt_is_geom);
    t.decision = result.decision;
    t.range_nm = result.range_nm;
    t.elevation_deg = result.elevation_deg;
    t.forwarded = state.currently_forwarded;
    t.first_seen = state.first_seen;
    t.last_seen = state.last_seen;
    t.emergency = state.latest.emergency;
    t.category = state.latest.category;
    t.rssi = state.latest.rssi.value_or(std::numeric_limits<double>::quiet_NaN());
    return t;
}

Snapshot Engine::snapshot() const
{
    Snapshot s;
    s.now = dataNow();
    s.tracks.reserve(table_.size());
    table_.forEach([&](const TrackState& t) { s.tracks.push_back(trackSnapshot(t, filters_.evaluate(t))); });
    s.filters = filters_.config();
    s.allow_entries = filters_.allow().size();
    s.block_entries = filters_.block().size();
    s.stats = stats();
    if (fisb_decoder_)
        s.fisb = fisb_store_.summary(s.now);
    return s;
}

fisb::FisbSnapshot Engine::fisbSnapshot() const
{
    return fisb_decoder_ ? fisb_store_.snapshot() : fisb::FisbSnapshot{};
}

std::string Engine::statsLine() const
{
    const EngineStats s = stats();
    std::ostringstream ss;
    ss << "stats obs=" << s.observations << " sent=" << s.sent_updates << " drops=" << s.sent_drops
       << " tracks=" << s.tracks_total << " fwd=" << s.tracks_forwarded << " uat_dl=" << s.uat_downlink
       << " uat_ul=" << s.uat_uplink << " readsb=" << (s.readsb_connected ? "up" : "down")
       << " dump978=" << (!uat_ ? "off" : s.dump978_connected ? "up" : "down");
    return ss.str();
}

}  // namespace skycued
