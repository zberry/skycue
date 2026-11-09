#include "engine.hpp"

#include "file_util.hpp"
#include "inputs/uat_raw_input.hpp"
#include "log.hpp"
#include "time_util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

ReceiverPosition makeReceiver(const Config& c)
{
    return ReceiverPosition{c.receiver_lat, c.receiver_lon, c.receiver_alt_m + c.antenna_height_m};
}

FilterSet makeFilters(const Config& c)
{
    if (!std::isfinite(c.receiver_lat) || !std::isfinite(c.receiver_lon))
        throw std::runtime_error("the receiver position (receiver.lat, receiver.lon) is required");
    return FilterSet(makeReceiver(c), FilterConfig{c.max_range_nm, c.min_elevation_deg}, loadList(c.allow_list_path),
                     loadList(c.block_list_path));
}

StatsLimits makeStatsLimits(const Config& c)
{
    StatsLimits l;
    l.aircraft_max = static_cast<std::size_t>(std::max(1, c.stats_aircraft_max));
    return l;
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
    EngineEvent ev;
    ev.kind = kind;
    return ev;
}

// The whole regular file; nullopt with errno set when it cannot be opened
// or read, or is not a regular file (EISDIR for a directory, EINVAL for
// anything else: a FIFO is opened non-blocking, never read).
std::optional<std::string> readWholeFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    struct ::stat st{};
    std::optional<std::string> text;
    int err = 0;
    if (::fstat(fd, &st) != 0) {
        err = errno;
    } else if (!S_ISREG(st.st_mode)) {
        err = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
    } else {
        text.emplace();
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) {
                text->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                err = errno;
                text.reset();
                break;
            }
        }
    }
    ::close(fd);
    errno = err;
    return text;
}

// "20261103T120000Z": a UTC stamp for a file name.
std::string fileStamp()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    ::gmtime_r(&now, &tm);
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return std::string(buf, n);  // empty, not indeterminate, if it ever did not fit
}

}  // namespace

Engine::Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<InputManager> inputs,
               Forwarder& forwarder, Recorder* received, Recorder* sent, Recorder* fisb)
    : config_(config),
      inputs_(std::move(inputs)),
      forwarder_(forwarder),
      received_(received),
      sent_(sent),
      fisb_rec_(fisb),
      filters_(makeFilters(config_)),
      thresholds_(makeThresholds(config_)),
      track_timeout_(std::max<long long>(1, std::llround(config_.track_timeout_s))),
      stats_store_(makeReceiver(config_), makeStatsLimits(config_)),
      tick_timer_(io),
      stats_timer_(io),
      lost_timer_(io)
{
    if (!inputs_)
        throw std::invalid_argument("Engine: an input manager is required");
    inputs_->onObservation([this](const TrackObservation& obs) { handleObservation(obs); });
    inputs_->onChanged([this] { inputsChanged(); });
    if (config_.fisb_enable) {
        fisb_decoder_ = std::make_unique<fisb::FisbDecoder>(config_.fisb_experimental);
        fisb_decoder_->onProduct([this](const fisb::FisbProduct& p) {
            auto events = fisb_store_.apply(p, dataNow());
            handleFisbEvents(events);
        });
    }
    inputs_->onUplink(
        [this](const std::string& input_id, std::chrono::system_clock::time_point when, const std::string& hex) {
            if (stopped_)
                return;
            if (dedupe_.duplicate(input_id, steady::now(), hex)) {
                ++uplink_duplicates_;
                return;
            }
            handleUplink(when, hex);
        });
    forwarder_.setKeyframeProvider([this] {
        std::vector<skycue::Track> out;
        table_.forEach([&](TrackState& s) {
            if (s.currently_forwarded) {
                const GeomView view(s, resolveGeom(s));
                out.push_back(makeTrack(view, true, s.last_sent ? s.last_sent->alt_is_geom : false));
            }
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
       << (config_.allow_list_path.empty() ? "none" : config_.allow_list_path) << " (" << filters_.allow().size()
       << " entries) block_list=" << (config_.block_list_path.empty() ? "none" : config_.block_list_path) << " ("
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
    if (filter_history_.empty())  // RuntimeConfig::load() may have pushed the startup state already
        pushFilterChange("startup");
    inputs_->start();
    last_locations_save_ = steady::now();
    last_stats_save_ = last_locations_save_;
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
    inputs_->stop();
    ++lost_gen_;
    tick_timer_.cancel();
    stats_timer_.cancel();
    lost_timer_.cancel();

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
    if (!locations_path_.empty() && locations_.dirty())
        saveLocations();
    if (!stats_path_.empty() && stats_store_.dirty())
        saveStats();
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
    // The send decision runs on the steady clock at arrival, not on the data
    // clock's anchor: an observation stamped behind the data clock leaves the
    // anchor where it is, and cues.max_interval_s resends must not stall with it.
    TrackState& state = table_.apply(obs);
    if (table_.lastApplySuppressedPosition())
        ++rebroadcast_suppressed_;
    // A difference counts whoever sent it, rebroadcast copies included (only their
    // positions are set aside). The fit key is the address type and address: one point per aircraft.
    if (const auto diff = learnGeomDiff(state.geom, obs); diff && obs.alt_baro_ft)
        altitude_fit_.add((static_cast<uint64_t>(state.id.type) << 32) | state.id.address, state.geom.diff_time, *obs.alt_baro_ft, *diff);
    process(obs, state, steady::now(), obs.lat.has_value() && !table_.lastApplySuppressedPosition());
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
    last_uplink_ = when;
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
        std::optional<EngineEvent> location;
        if (e.kind == fisb::FisbEvent::Kind::twgo && e.twgo && e.twgo->has_graphics)
            location = learnLocation(*e.twgo);
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
            n.expired_kind = e.expired_kind;
            n.twgo = e.twgo;
            n.text = e.text;
            break;
        }
        EngineEvent ev = kindEvent(kind);
        ev.fisb = std::move(n);
        emit(ev);
        if (location && sink_)
            emit(*location);
    }
}

std::optional<EngineEvent> Engine::learnLocation(const fisb::TwgoRecord& record)
{
    if (!fisb::airportStyleId(record.location))
        return std::nullopt;  // never learned; keeps announced_ to learnable ids
    const auto before = locations_.find(record.location);
    const bool is_new = locations_.learn(record);
    const auto after = locations_.find(record.location);
    if (!after || !announced_.update(record.location, before, *after, is_new))
        return std::nullopt;
    FisbNotice n;
    n.key = record.location;
    n.position = after;
    EngineEvent ev = kindEvent(EventKind::fisb_location);
    ev.fisb = std::move(n);
    return ev;
}

void Engine::loadLocations(const std::string& learned_path, const std::string& operator_csv_path)
{
    locations_path_ = learned_path;
    operator_locations_path_ = operator_csv_path;
    if (!learned_path.empty()) {
        const auto text = readWholeFile(learned_path);
        if (text) {
            try {
                locations_.loadLearned(*text);
                LOG_INFO(learned_path << ": " << locations_.learnedCount() << " learned locations");
            } catch (const std::exception& e) {
                const std::string rejected = learned_path + ".rejected";
                const bool renamed = std::rename(learned_path.c_str(), rejected.c_str()) == 0;
                LOG_ERROR(learned_path << ": " << e.what() << "; "
                                       << (renamed ? "renamed " + rejected
                                                   : "could not rename it to " + rejected + ": " +
                                                         std::strerror(errno))
                                       << "; starting with no learned locations");
            }
        } else if (errno == EISDIR || errno == EINVAL) {
            // Not a file we could replace: leave it, and save nothing this run.
            LOG_ERROR(learned_path << ": not a regular file; learned locations are not loaded or saved this run");
            locations_path_.clear();
        } else if (errno != ENOENT) {
            // Unreadable (permissions, say): set it aside so the first save cannot overwrite it.
            const std::string why = std::strerror(errno);
            const std::string aside = learned_path + ".unreadable-" + fileStamp();
            if (std::rename(learned_path.c_str(), aside.c_str()) == 0) {
                LOG_ERROR(learned_path << ": cannot read: " << why << "; renamed " << aside
                                       << "; starting with no learned locations");
            } else {
                LOG_ERROR(learned_path << ": cannot read: " << why << "; could not rename it to " << aside << ": "
                                       << std::strerror(errno) << "; learned locations are not saved this run");
                locations_path_.clear();
            }
        }
    }
    if (!operator_csv_path.empty()) {
        const auto text = readWholeFile(operator_csv_path);
        if (!text)
            throw std::runtime_error(operator_csv_path + ": cannot read: " + std::strerror(errno));
        const std::size_t n = locations_.loadOperatorCsv(*text);
        if (locations_.operatorRejected() > 0)
            LOG_WARN(operator_csv_path << ": " << locations_.operatorRejected() << " lines rejected (first: line "
                                       << locations_.operatorRejectedFirstLine() << ")");
        LOG_INFO(operator_csv_path << ": " << n << " operator locations");
    }
}

std::map<std::string, std::pair<double, double>> Engine::locationPositions() const { return locations_.merged(); }

void Engine::saveLocations()
{
    last_locations_save_ = steady::now();
    try {
        atomicWrite(locations_path_, locations_.learnedJson());
        locations_.markSaved();
        if (locations_save_failing_)
            LOG_INFO(locations_path_ << ": saved again");
        locations_save_failing_ = false;
    } catch (const std::exception& e) {
        if (!locations_save_failing_)
            LOG_WARN("fisb locations not saved: " << e.what() << "; retrying every " << locations_save_interval_.count()
                                                  << " s");
        locations_save_failing_ = true;
    }
}

void Engine::process(const TrackObservation& obs, TrackState& state, steady::time_point now, bool positioned)
{
    // Evaluated once, before the decision: evaluate() reads only the id and
    // the latest values, so the decision, the stats and the observed event
    // all see the same result; and the stats ledger entry exists before the
    // sent()/dropped() that the decision causes.
    const GeomView view(state, resolveGeom(state));
    const FilterResult result = filters_.evaluate(view.state());
    stats_store_.observe(obs, state, result);
    decide(state, view, result, now);
    if (positioned && sink_ && !stopped_) {
        EngineEvent ev = kindEvent(EventKind::observed);
        ev.track = trackSnapshot(view, result);
        emit(ev);
    }
}

Engine::GeomView::GeomView(const TrackState& state, GeomResult geom) : orig_(&state), geom_(geom)
{
    if (geom_.alt_geom_ft != state.latest.alt_geom_ft) {
        copy_ = state;
        copy_->latest.alt_geom_ft = geom_.alt_geom_ft;
    }
}

GeomResult Engine::resolveGeom(const TrackState& state) const
{
    const auto now = dataNow();
    return resolveGeomAltitude(state, now, altitude_fit_.fit(now));
}

skycue::Track Engine::makeTrack(const GeomView& view, bool keyframe, bool alt_is_geom) const
{
    skycue::Track t = toTrack(view.state(), keyframe, alt_is_geom, dataNow());
    const GeomResult& g = view.geom();
    if (g.source == GeomSource::estimated && g.uncertainty_ft)
        t.alt_geom_uncertainty_ft = static_cast<uint16_t>(std::clamp<int32_t>(*g.uncertainty_ft, 0, 65535));
    return t;
}

void Engine::decide(TrackState& state, const GeomView& view, const FilterResult& result, steady::time_point now)
{
    const SendReason reason = shouldSend(view.state(), now, thresholds_);
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
    if (isSend(result.decision)) {
        if (reason == SendReason::none && state.currently_forwarded)
            return;  // still eligible; nothing worth sending yet
        forward(state, view, result, now);
    } else if (state.currently_forwarded) {
        drop(state, skycue::DropReason::filtered, result.decision);
    }
}

void Engine::forward(TrackState& state, const GeomView& view, const FilterResult& result,
                     steady::time_point now)
{
    const skycue::Track track = makeTrack(view, false, result.alt_is_geom);
    forwarder_.sendUpdate(track);
    markSent(state, track, now);
    state.currently_forwarded = true;
    ++sent_updates_;
    stats_store_.sent(state.id, dataNow());
    if (sent_)
        sent_->writeTrack(track, "update");
    if (sink_) {
        EngineEvent ev = kindEvent(EventKind::update);
        ev.track = trackSnapshot(view, result);
        emit(ev);
    }
}

void Engine::drop(TrackState& state, skycue::DropReason reason, std::optional<Decision> decision)
{
    forwarder_.sendDrop(state.id, reason);
    state.currently_forwarded = false;
    countDrop(reason, decision);
    stats_store_.dropped(state.id, reason, decision, dataNow());
    if (sent_)
        sent_->writeDrop(state.id, reason);
    emitDrop(state, reason, decision);
}

void Engine::emitDrop(const TrackState& state, skycue::DropReason reason, std::optional<Decision> decision)
{
    if (!sink_)
        return;
    EngineEvent ev = kindEvent(EventKind::drop);
    ev.id = state.id;
    ev.reason = reason;
    ev.decision = decision;
    ev.callsign = state.latest.callsign;
    ev.time = dataNow();
    emit(ev);
}

void Engine::emitDrop(skycue::TrackId id, const std::optional<std::string>& callsign, skycue::DropReason reason)
{
    if (!sink_)
        return;
    EngineEvent ev = kindEvent(EventKind::drop);
    ev.id = id;
    ev.reason = reason;
    ev.callsign = callsign;
    ev.time = dataNow();
    emit(ev);
}

void Engine::countDrop(skycue::DropReason reason, std::optional<Decision> decision)
{
    ++sent_drops_;
    countDropIn(drops_, reason, decision);
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
        const GeomView view(s, resolveGeom(s));
        const FilterResult r = filters_.evaluate(view.state());
        if (s.currently_forwarded) {
            if (!isSend(r.decision))
                drop(s, skycue::DropReason::filtered, r.decision);
        } else if (isSend(r.decision)) {  // implies a position
            forward(s, view, r, now);
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
    pushFilterChange(origin);
    std::ostringstream ss;
    ss << origin << ": max_range_nm=" << config.max_range_nm << " min_elevation_deg=" << config.min_elevation_deg;
    applied(ss.str(), EventKind::filters_changed);
}

void Engine::pushFilterChange(const std::string& origin)
{
    // The wall clock, always: an operator audit log, so during a replay the
    // startup entry and later changes do not mix the two clocks.
    filter_history_.push_back(FilterChange{std::chrono::system_clock::now(), origin, filters_.config()});
    if (filter_history_.size() > kFilterHistoryMax)
        filter_history_.pop_front();
}

std::vector<FilterChange> Engine::filterHistory() const
{
    return std::vector<FilterChange>(filter_history_.begin(), filter_history_.end());
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

void Engine::inputsChanged()
{
    if (stopped_)
        return;
    const bool up = inputs_->anyTrackInputConnected();
    const bool was = track_connected_;
    track_connected_ = up;
    if (up && !was) {
        LOG_INFO("source connected");
        ++lost_gen_;
        lost_timer_.cancel();
    } else if (!up && was) {
        armLost();
    }
    emit(kindEvent(EventKind::inputs_changed));
}

void Engine::outputsChanged()
{
    if (stopped_)
        return;
    emit(kindEvent(EventKind::outputs_changed));
}

void Engine::armLost()
{
    LOG_WARN("source lost; forwarded tracks are dropped if it is not back within " << track_timeout_.count()
                                                                                    << " s");
    const unsigned gen = ++lost_gen_;
    lost_timer_.expires_after(track_timeout_);  // the same value expire() uses
    std::weak_ptr<char> alive = alive_;
    lost_timer_.async_wait([this, gen, alive](const error_code& ec) {
        if (ec || alive.expired() || gen != lost_gen_ || stopped_ || track_connected_)
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
        // Forwarded ids with their callsigns, taken before expire() removes them.
        std::unordered_map<skycue::TrackId, std::optional<std::string>> forwarded;
        table_.forEach([&](TrackState& s) {
            if (s.currently_forwarded)
                forwarded.emplace(s.id, s.latest.callsign);
        });
        // A track that ages out while the inputs are lost went because of it.
        const auto reason = track_connected_ ? skycue::DropReason::timeout : skycue::DropReason::source_lost;
        for (const auto& id : table_.expire(now, track_timeout_)) {
            const auto f = forwarded.find(id);
            if (f == forwarded.end()) {
                if (sink_) {
                    EngineEvent ev = kindEvent(EventKind::expired);
                    ev.id = id;
                    emit(ev);
                }
                continue;
            }
            forwarder_.sendDrop(id, reason);
            countDrop(reason);
            stats_store_.dropped(id, reason, std::nullopt, now);
            if (sent_)
                sent_->writeDrop(id, reason);
            emitDrop(id, f->second, reason);
        }
    }
    if (fisb_decoder_ && have_data_clock_) {
        const auto now = dataNow();
        fisb_decoder_->sweep(now);
        auto events = fisb_store_.sweep(now);
        handleFisbEvents(events);
    }
    if (!locations_path_.empty() && locations_.dirty() && steady::now() - last_locations_save_ >= locations_save_interval_)
        saveLocations();
    if (received_)
        received_->flush();
    if (sent_)
        sent_->flush();
    if (fisb_rec_)
        fisb_rec_->flush();
    // One EngineStats for the store and the stats event. The store is fed
    // only on the data clock: before the first observation or uplink there
    // is nothing to place, and a wall-clock bucket would only be "restarted"
    // by a replay of older data.
    const bool want_stats = sink_ || have_data_clock_;
    EngineStats s;
    if (want_stats)
        s = stats();
    if (have_data_clock_)
        stats_store_.tick(dataNow(), s);
    if (!stats_path_.empty() && stats_store_.dirty() && steady::now() - last_stats_save_ >= stats_save_interval_)
        saveStats();
    if (sink_) {
        EngineEvent ev = kindEvent(EventKind::stats);
        ev.stats = std::move(s);
        emit(ev);
    }
}

void Engine::loadStats(const std::string& path, std::chrono::seconds save_interval)
{
    stats_path_ = path;
    stats_save_interval_ = save_interval;
    stats_save_failing_ = false;
    stats_size_warned_ = false;
    last_stats_save_ = steady::now();
    if (path.empty()) {
        LOG_INFO("stats: stats.file is empty; the statistics are not saved");
        return;
    }
    const StatsLoad loaded = loadStatsFile(path, stats_store_);
    if (loaded == StatsLoad::not_regular) {
        stats_path_.clear();  // never written: kept in memory only (loadStatsFile logged it)
        return;
    }
    if (loaded == StatsLoad::loaded) {
        const auto since = stats_store_.sinceS();
        LOG_INFO(path << ": statistics of " << stats_store_.aircraftCount() << " aircraft"
                      << (since ? " since " + formatIso8601Ms(std::chrono::system_clock::time_point(
                                                  std::chrono::seconds(*since)))
                                : std::string()));
    }
}

void Engine::resetStats(const std::string& origin)
{
    if (stopped_)
        return;
    stats_store_.reset(dataNow());
    LOG_INFO("stats: reset by " << origin);
    if (!stats_path_.empty())
        saveStats();
}

void Engine::saveStats()
{
    // On the io thread: toJson of a full store (1440 + 720 buckets, 20000
    // aircraft, 40000 grid cells) is a noticeable fraction of a second, which
    // one save per stats.save_s (default 300 s) accepts.
    last_stats_save_ = steady::now();
    try {
        const std::size_t bytes = saveStatsFile(stats_path_, stats_store_);
        stats_store_.markSaved();
        if (bytes > kStatsFileWarnBytes && !stats_size_warned_)
            LOG_WARN(stats_path_ << ": " << bytes << " bytes, close to the " << kStatsFileMaxBytes
                                 << " a load accepts; consider a lower stats.aircraft_max");
        stats_size_warned_ = bytes > kStatsFileWarnBytes;
        if (stats_save_failing_)
            LOG_INFO(stats_path_ << ": saved again");
        stats_save_failing_ = false;
    } catch (const std::exception& e) {
        if (!stats_save_failing_)
            LOG_WARN("stats not saved: " << e.what() << "; retrying every " << stats_save_interval_.count() << " s");
        stats_save_failing_ = true;
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
    s.rebroadcast_suppressed = rebroadcast_suppressed_;
    s.tracks_total = table_.size();
    table_.forEach([&](const TrackState& t) { s.tracks_forwarded += t.currently_forwarded ? 1 : 0; });
    const Input* readsb = inputs_->builtin("readsb");
    if (!readsb)
        readsb = inputs_->builtin("replay");
    s.readsb_connected = readsb && readsb->state().connected;
    if (const Input* uat = inputs_->builtin("dump978")) {
        s.dump978_connected = uat->state().connected;
        if (const auto* d = dynamic_cast<const UatRawInput*>(uat)) {
            s.uat_downlink = d->downlinkFrames();
            s.uat_uplink = d->uplinkFrames();
        }
    }
    s.inputs = inputs_->statsLines();
    {
        const auto now = dataNow();
        if (const auto f = altitude_fit_.fit(now)) {
            AltitudeFitStats& a = s.altitude_fit;
            a.a = f->a;
            a.b = f->b;
            a.samples = f->samples;
            a.aircraft = f->aircraft;
            a.p90_ft = f->p90_ft;
            a.age_s = std::chrono::duration<double>(now - f->newest).count();
            a.linear = f->linear;
            a.valid = true;
        }
    }
    s.fisb_uplink_duplicates = uplink_duplicates_;
    s.tcp_clients = forwarder_.tcpClientCount();
    s.udp_targets = forwarder_.udpTargetCount();
    s.drops_timeout = drops_.drops_timeout;
    s.drops_filtered = drops_.drops_filtered;
    s.drops_source_lost = drops_.drops_source_lost;
    s.drops_blocked = drops_.drops_blocked;
    s.drops_out_of_range = drops_.drops_out_of_range;
    s.drops_below_elevation = drops_.drops_below_elevation;
    s.drops_no_position = drops_.drops_no_position;
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
        s.fisb_last_uplink = last_uplink_;
    }
    return s;
}

TrackSnapshot Engine::trackSnapshot(const GeomView& view, const FilterResult& result) const
{
    const TrackState& state = view.state();
    TrackSnapshot t;
    t.track = makeTrack(view, false, result.alt_is_geom);
    t.geom = view.geom();
    t.decision = result.decision;
    t.range_nm = result.range_nm;
    t.elevation_deg = result.elevation_deg;
    t.forwarded = state.currently_forwarded;
    t.first_seen = state.first_seen;
    t.last_seen = state.last_seen;
    t.emergency = state.latest.emergency;
    t.category = state.latest.category;
    t.rssi = state.latest.rssi.value_or(std::numeric_limits<double>::quiet_NaN());
    t.input = state.last_input;
    t.commb = state.latest;  // slices to the CommBFields part
    if (t.commb.acas_ra && dataNow() - t.commb.acas_ra->at > kAcasRaLife)
        t.commb.acas_ra.reset();  // expires with no message to carry the news
    return t;
}

Snapshot Engine::snapshot() const
{
    Snapshot s;
    s.now = dataNow();
    s.tracks.reserve(table_.size());
    table_.forEach([&](const TrackState& t) {
        const GeomView view(t, resolveGeom(t));
        s.tracks.push_back(trackSnapshot(view, filters_.evaluate(view.state())));
    });
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

fisb::FisbSnapshot Engine::fisbRecords() const
{
    return fisb_decoder_ ? fisb_store_.records() : fisb::FisbSnapshot{};
}

fisb::FisbSummary Engine::fisbSummary() const
{
    return fisb_decoder_ ? fisb_store_.summary(dataNow()) : fisb::FisbSummary{};
}

std::optional<fisb::Mosaic> Engine::fisbMosaic(uint16_t product, std::optional<uint8_t> altitude_code) const
{
    return fisb_decoder_ ? fisb_store_.mosaic(product, altitude_code) : std::nullopt;
}

uint64_t Engine::fisbBlocksCopied() const { return fisb_store_.blocksCopied(); }

bool Engine::dump978Off() const
{
    const auto d = inputs_->find("dump978");
    return !d || !d->def.enabled;  // the id is reserved for the built-in
}

std::string Engine::statsLine() const
{
    const EngineStats s = stats();
    std::ostringstream ss;
    ss << "stats obs=" << s.observations << " sent=" << s.sent_updates << " drops=" << s.sent_drops
       << " tracks=" << s.tracks_total << " fwd=" << s.tracks_forwarded << " uat_dl=" << s.uat_downlink
       << " uat_ul=" << s.uat_uplink << " readsb=" << (s.readsb_connected ? "up" : "down")
       << " dump978=" << (dump978Off() ? "off" : s.dump978_connected ? "up" : "down") << " inputs="
       << inputs_->connectedCount() << "/" << inputs_->enabledCount();
    return ss.str();
}

}  // namespace skycued
