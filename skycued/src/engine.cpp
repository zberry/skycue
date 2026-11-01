#include "engine.hpp"

#include "log.hpp"

#include <cmath>
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

FilterSet makeFilters(const Config& c, size_t& allow_size, size_t& block_size)
{
    if (!std::isfinite(c.receiver_lat) || !std::isfinite(c.receiver_lon))
        throw std::runtime_error("the receiver position (RECEIVER_LAT, RECEIVER_LON) is required");
    IdList allow = loadList(c.allow_list_file);
    IdList block = loadList(c.block_list_file);
    allow_size = allow.size();
    block_size = block.size();
    return FilterSet(ReceiverPosition{c.receiver_lat, c.receiver_lon, c.receiver_alt_m + c.antenna_height_m},
                     FilterConfig{c.max_range_nm, c.min_elevation_deg}, std::move(allow), std::move(block));
}

Thresholds makeThresholds(const Config& c)
{
    Thresholds t;
    t.min_interval_s = c.min_interval_s;
    t.max_interval_s = c.max_interval_s;
    return t;
}

}  // namespace

Engine::Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<ObservationSource> source,
               std::unique_ptr<Dump978RawSource> uat, Forwarder& forwarder, Recorder* received, Recorder* sent)
    : io_(io),
      config_(config),
      source_(std::move(source)),
      uat_(std::move(uat)),
      forwarder_(forwarder),
      received_(received),
      sent_(sent),
      filters_(makeFilters(config_, allow_size_, block_size_)),
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
    if (uat_) {
        uat_->onUplink([this](std::chrono::system_clock::time_point when, const std::string& hex) {
            if (!stopped_ && received_)
                received_->writeUplink(when, hex);
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
       << " alt_m=" << (config_.receiver_alt_m + config_.antenna_height_m) << " max_range_nm=" << config_.max_range_nm
       << " min_elevation_deg=" << config_.min_elevation_deg << " allow_list="
       << (config_.allow_list_file.empty() ? "none" : config_.allow_list_file) << " (" << allow_size_
       << " entries) block_list=" << (config_.block_list_file.empty() ? "none" : config_.block_list_file) << " ("
       << block_size_ << " entries)";
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
    LOG_INFO("stopping: " << shutdown_drops << " shutdown drops; " << statsLine());
}

void Engine::handleObservation(const TrackObservation& obs)
{
    if (stopped_)
        return;
    ++observations_;
    have_data_clock_ = true;
    last_obs_time_ = obs.received_time;
    last_obs_steady_ = steady::now();
    if (received_)
        received_->writeObservation(obs);
    process(table_.apply(obs), last_obs_steady_);
}

void Engine::process(TrackState& state, steady::time_point now)
{
    const SendReason reason = shouldSend(state, now, thresholds_);
    if (reason == SendReason::none) {
        // A track that is not forwarded is re-checked whenever it has a
        // position, so it comes back as soon as it is eligible again; at most
        // once per min_interval_s since its last send, so a track on a filter
        // boundary cannot flap at the observation rate.
        if (state.currently_forwarded || !state.latest.lat)
            return;
        if (state.last_sent && now - state.last_sent_at < std::chrono::duration<double>(thresholds_.min_interval_s))
            return;
    }
    const FilterResult result = filters_.evaluate(state);
    if (isSend(result.decision)) {
        const skycue::Track track = toTrack(state, false, result.alt_is_geom);
        forwarder_.sendUpdate(track);
        markSent(state, track, now);
        state.currently_forwarded = true;
        ++sent_updates_;
        if (sent_)
            sent_->writeTrack(track, "update");
    } else if (state.currently_forwarded) {
        drop(state, skycue::DropReason::filtered);
    }
}

void Engine::drop(TrackState& state, skycue::DropReason reason)
{
    forwarder_.sendDrop(state.id, reason);
    state.currently_forwarded = false;
    ++sent_drops_;
    if (sent_)
        sent_->writeDrop(state.id, reason);
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
    LOG_WARN("source lost; forwarded tracks are dropped if it is not back within " << config_.track_timeout_s
                                                                                    << " s");
    const unsigned gen = ++lost_gen_;
    lost_timer_.expires_after(std::chrono::duration_cast<steady::duration>(
        std::chrono::duration<double>(std::max(0.0, config_.track_timeout_s))));
    std::weak_ptr<char> alive = alive_;
    lost_timer_.async_wait([this, gen, alive](const error_code& ec) {
        if (ec || alive.expired() || gen != lost_gen_ || stopped_ || source_connected_)
            return;
        size_t n = 0;
        table_.forEach([&](TrackState& s) { n += s.currently_forwarded ? 1 : 0; });
        LOG_WARN("source still lost after " << config_.track_timeout_s << " s: dropping " << n
                                            << " forwarded tracks");
        dropAllForwarded(skycue::DropReason::source_lost);
    });
}

void Engine::tick()
{
    if (have_data_clock_) {
        const auto now = last_obs_time_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                              steady::now() - last_obs_steady_);
        std::unordered_set<skycue::TrackId> forwarded;
        table_.forEach([&](TrackState& s) {
            if (s.currently_forwarded)
                forwarded.insert(s.id);
        });
        // A track that ages out while the source is down went because of it.
        const auto reason = source_connected_ ? skycue::DropReason::timeout : skycue::DropReason::source_lost;
        for (const auto& id : table_.expire(now, track_timeout_)) {
            if (!forwarded.count(id))
                continue;
            forwarder_.sendDrop(id, reason);
            ++sent_drops_;
            if (sent_)
                sent_->writeDrop(id, reason);
        }
    }
    if (received_)
        received_->flush();
    if (sent_)
        sent_->flush();
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
    return s;
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
