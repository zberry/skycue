#include "replay_source.hpp"

#include "json_lite.hpp"
#include "log.hpp"
#include "recorder.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace skycued {
namespace {

bool blank(const std::string& line)
{
    return line.find_first_not_of(" \t\r") == std::string::npos;
}

// Far enough out to be "never" while keeping steady_clock arithmetic safe.
constexpr double kMaxDelaySeconds = 1e9;

}  // namespace

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
        if (blank(line))
            continue;
        bool ok = false;
        if (auto record = JsonObject::parse(line)) {
            const std::string type = record->getString("type").value_or("");
            if (type == "uat_uplink")
                continue;
            if (type == "obs") {
                if (auto obs = recording::observationFromRecord(*record)) {
                    records_.push_back(Record{obs->received_time, std::move(*obs)});
                    ok = true;
                }
            }
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
            gaps.push_back(records_[i].time - records_[i - 1].time);
        std::chrono::system_clock::duration median{};
        if (!gaps.empty()) {
            std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
            median = gaps[gaps.size() / 2];
        }
        const auto min_gap = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(1));
        const auto span = std::max(records_.back().time - records_.front().time, std::chrono::system_clock::duration{});
        loop_shift_ = span + std::max(median, min_gap);
    }
}

ReplaySource::~ReplaySource() = default;  // alive_ goes first, so pending handlers do nothing

void ReplaySource::onObservation(std::function<void(const TrackObservation&)> handler)
{
    on_observation_ = std::move(handler);
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
        (records_[next_].time - records_.front().time) + loop_shift_ * static_cast<int64_t>(pass_);
    double seconds = std::chrono::duration<double>(offset).count() / speed_;
    seconds = std::min(seconds, kMaxDelaySeconds);
    timer_.expires_at(start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(seconds)));
    timer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (!ec && !alive.expired())
            emitNext();
    });
}

void ReplaySource::emitNext()
{
    const Record& record = records_[next_++];
    std::weak_ptr<char> alive = alive_;
    if (on_observation_) {
        if (pass_ == 0) {
            on_observation_(record.obs);
        } else {
            const auto shift = loop_shift_ * static_cast<int64_t>(pass_);
            TrackObservation obs = record.obs;
            obs.received_time += shift;
            if (obs.position_time)
                *obs.position_time += shift;
            on_observation_(obs);
        }
    }
    if (!alive.expired())  // the handler may have destroyed us
        scheduleNext();
}

}  // namespace skycued
