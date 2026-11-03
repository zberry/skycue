#include "replay_source.hpp"

#include "log.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace skycued {

namespace {

using Duration = std::chrono::system_clock::duration;
using Rep = Duration::rep;

Duration saturate(bool overflowed, Rep value, bool negative)
{
    if (!overflowed)
        return Duration(value);
    return negative ? Duration::min() : Duration::max();
}

// a + b, saturated: loop mode adds a shift of up to the whole recording's
// span per pass, so it may not wrap.
Duration satAdd(Duration a, Duration b)
{
    Rep r = 0;
    const bool over = __builtin_add_overflow(a.count(), b.count(), &r);
    return saturate(over, r, b.count() < 0);
}

std::chrono::system_clock::time_point shifted(std::chrono::system_clock::time_point t, Duration shift)
{
    return std::chrono::system_clock::time_point(satAdd(t.time_since_epoch(), shift));
}

}  // namespace

Duration replayLoopShift(Duration shift, uint64_t pass)
{
    // A zero factor is 0 whatever the other (pass 0, or a one-record span).
    if (shift.count() == 0 || pass == 0)
        return Duration::zero();
    Rep r = 0;
    const bool over = pass > static_cast<uint64_t>(INT64_MAX) ||
                      __builtin_mul_overflow(shift.count(), static_cast<Rep>(pass), &r);
    return saturate(over, over ? 0 : r, shift.count() < 0);
}

Duration replayOffset(std::chrono::system_clock::time_point t, std::chrono::system_clock::time_point first)
{
    Rep r = 0;
    const bool over = __builtin_sub_overflow(t.time_since_epoch().count(), first.time_since_epoch().count(), &r);
    return saturate(over, r, t < first);
}

std::chrono::steady_clock::duration replayDelay(std::chrono::system_clock::duration offset, double speed)
{
    double seconds = std::chrono::duration<double>(offset).count() / speed;
    // Both bounds, written so a NaN fails them too: an offset before the
    // first record (a file out of order) or a tiny speed would otherwise
    // overflow the conversion to steady_clock's int64 nanoseconds.
    if (!(seconds >= 0))
        seconds = 0;
    if (!(seconds <= kMaxReplayDelaySeconds))
        seconds = kMaxReplayDelaySeconds;
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

ReplaySource::ReplaySource(boost::asio::io_context& io, std::string path, double speed, bool loop)
    : io_(io), timer_(io), path_(std::move(path)), speed_(speed), loop_(loop)
{
    if (!std::isfinite(speed_) || speed_ < 0)
        throw std::invalid_argument("replay: speed must be 0 (unpaced) or positive");
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        throw std::runtime_error("replay: cannot open " + path_);

    std::string line;
    std::size_t line_no = 0, first_bad = 0;
    while (std::getline(in, line)) {
        ++line_no;
        RecordingRecord record;
        const LineKind kind = parseRecordingLine(line, record);
        if (kind == LineKind::ignore ||
            (kind == LineKind::received && record.kind == RecordKind::uat_uplink && record.uplink_hex.empty()))
            continue;
        const bool ok = kind == LineKind::received || kind == LineKind::readsb_raw;
        if (ok) {
            if (record.kind != RecordKind::uat_uplink)
                ++observations_;
            records_.push_back(std::move(record));
        }
        if (!ok && ++skipped_ == 1)
            first_bad = line_no;
    }
    if (skipped_ > 0)
        LOG_WARN("replay: " << path_ << ": skipped " << skipped_ << " of " << line_no
                            << " lines that are not valid received-stream records (first: line " << first_bad
                            << ")");

    if (!records_.empty()) {
        std::vector<std::chrono::system_clock::duration> gaps;
        for (std::size_t i = 1; i < records_.size(); ++i)
            gaps.push_back(replayOffset(records_[i].time, records_[i - 1].time));
        std::chrono::system_clock::duration median{};
        if (!gaps.empty()) {
            std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
            median = gaps[gaps.size() / 2];
        }
        const auto min_gap = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(1));
        // max - min, not back - front: a hand-edited file need not be in order.
        const auto by_time = [](const RecordingRecord& a, const RecordingRecord& b) { return a.time < b.time; };
        const auto range = std::minmax_element(records_.begin(), records_.end(), by_time);
        const auto span = replayOffset(range.second->time, range.first->time);
        loop_shift_ = satAdd(span, std::max(median, min_gap));
    }
}

ReplaySource::~ReplaySource() = default;  // alive_ goes first, so pending handlers do nothing

void ReplaySource::onObservation(std::function<void(const TrackObservation&)> handler)
{
    on_observation_ = std::move(handler);
}

void ReplaySource::onUplink(
    std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler)
{
    on_uplink_ = std::move(handler);
}

void ReplaySource::onEnd(std::function<void()> handler) { on_end_ = std::move(handler); }

void ReplaySource::start()
{
    if (started_)
        return;
    started_ = true;
    start_ = std::chrono::steady_clock::now();
    if (records_.empty()) {
        if (!loop_) {
            std::weak_ptr<char> alive = alive_;
            boost::asio::post(io_, [this, alive] {
                if (!alive.expired() && on_end_)
                    on_end_();
            });
        }
        return;
    }
    scheduleNext();
}

void ReplaySource::scheduleNext()
{
    if (next_ == records_.size()) {
        if (!loop_) {
            std::weak_ptr<char> alive = alive_;
            boost::asio::post(io_, [this, alive] {
                if (!alive.expired() && on_end_)
                    on_end_();
            });
            return;
        }
        next_ = 0;
        ++pass_;
    }

    std::weak_ptr<char> alive = alive_;
    if (speed_ == 0) {
        boost::asio::post(io_, [this, alive] {
            if (!alive.expired())
                emitNext();
        });
        return;
    }
    // Absolute schedule from start_, so per-record delays never accumulate.
    const auto offset =
        satAdd(replayOffset(records_[next_].time, records_.front().time), replayLoopShift(loop_shift_, pass_));
    timer_.expires_at(start_ + replayDelay(offset, speed_));
    timer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (!ec && !alive.expired())
            emitNext();
    });
}

void ReplaySource::emitNext()
{
    const RecordingRecord& record = records_[next_++];
    std::weak_ptr<char> alive = alive_;
    if (record.kind == RecordKind::uat_uplink) {
        if (on_uplink_)
            on_uplink_(shifted(record.time, replayLoopShift(loop_shift_, pass_)), record.uplink_hex);
    } else if (on_observation_) {
        if (pass_ == 0) {
            on_observation_(record.obs);
        } else {
            const auto shift = replayLoopShift(loop_shift_, pass_);
            TrackObservation obs = record.obs;
            obs.received_time = shifted(obs.received_time, shift);
            if (obs.position_time)
                *obs.position_time = shifted(*obs.position_time, shift);
            on_observation_(obs);
        }
    }
    if (!alive.expired())  // the handler may have destroyed us
        scheduleNext();
}

ReplayObservationSource::ReplayObservationSource(boost::asio::io_context& io, std::string path, double speed,
                                                 bool loop)
    : replay_(std::make_unique<ReplaySource>(io, std::move(path), speed, loop)), count_(replay_->recordCount())
{
    replay_->onObservation([this](const TrackObservation& obs) {
        if (on_observation_)
            on_observation_(obs);
    });
    replay_->onUplink([this](std::chrono::system_clock::time_point when, const std::string& hex) {
        if (on_uplink_)
            on_uplink_(when, hex);
    });
    replay_->onEnd([this] {
        LOG_INFO("replay: end of file");
        if (on_connection_)
            on_connection_(false);
    });
}

void ReplayObservationSource::onObservation(std::function<void(const TrackObservation&)> handler)
{
    on_observation_ = std::move(handler);
}

void ReplayObservationSource::onConnection(std::function<void(bool)> handler)
{
    on_connection_ = std::move(handler);
}

void ReplayObservationSource::onUplink(
    std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler)
{
    on_uplink_ = std::move(handler);
}

void ReplayObservationSource::start()
{
    if (started_ || !replay_)
        return;
    started_ = true;
    if (on_connection_)
        on_connection_(true);
    if (replay_)  // the handler may have stopped us
        replay_->start();
}

void ReplayObservationSource::stop() { replay_.reset(); }

}  // namespace skycued
