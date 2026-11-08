#include "inputs/input.hpp"

#include "inputs/ping_json.hpp"
#include "log.hpp"

#include <utility>

namespace skycued {

Input::Input(InputDef def) : def_(std::move(def)) {}

Input::~Input() = default;

InputState Input::state() const
{
    InputState s;
    s.connected = connected_;
    s.receiving = receiving_;
    s.last_data_time = last_data_time_;
    s.counters = counters_;
    s.device_status = device_status_;
    s.clock_offset_s = clock_offset_s_;
    s.last_error = last_error_;
    return s;
}

void Input::tick(std::chrono::system_clock::time_point now)
{
    receiving_ = last_data_time_ && now - *last_data_time_ < kReceivingWindow;
}

void Input::onObservation(std::function<void(const TrackObservation&)> handler) { on_observation_ = std::move(handler); }

void Input::onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string&)> handler)
{
    on_uplink_ = std::move(handler);
}

void Input::onConnection(std::function<void(bool)> handler) { on_connection_ = std::move(handler); }

void Input::onStatus(std::function<void()> handler) { on_status_ = std::move(handler); }

void Input::emitObservation(TrackObservation obs)
{
    ++counters_.observations;
    if (obs.input_id.empty())
        obs.input_id = def_.id;
    if (on_observation_)
        on_observation_(obs);
}

void Input::emitUplink(std::chrono::system_clock::time_point time, const std::string& hex)
{
    if (on_uplink_)
        on_uplink_(time, hex);
}

void Input::setConnected(bool connected)
{
    if (connected == connected_)
        return;
    connected_ = connected;
    if (on_connection_)
        on_connection_(connected);
}

void Input::markStopped() { connected_ = false; }

void Input::noteData(std::size_t bytes)
{
    counters_.bytes += bytes;
    last_data_time_ = std::chrono::system_clock::now();
    receiving_ = true;
}

void Input::noteError(const std::string& message)
{
    last_error_ = message;
    const auto now = std::chrono::steady_clock::now();
    if (last_error_logged_at_ && now - *last_error_logged_at_ < kErrorLogInterval)
        return;
    last_error_logged_at_ = now;
    LOG_WARN("input " << def_.id << ": " << message);
}

void Input::noteStatus(DeviceStatus status)
{
    device_status_ = std::move(status);
    if (on_status_)
        on_status_();
}

void Input::noteClockOffset(std::optional<double> offset_s)
{
    if (offset_s)
        clock_offset_s_ = offset_s;
}

TransportInput::TransportInput(boost::asio::io_context& io, InputDef def) : Input(std::move(def)), io_(io)
{
    if (const auto url = parseHttpUrl(this->def().status_url)) {
        status_poller_ =
            std::make_unique<StatusPoller>(io_, *url, std::chrono::seconds(this->def().status_interval_s));
        status_poller_->onResult(
            [this](const std::string* body, const std::string& error) { handleStatusResult(body, error); });
    }
}

TransportInput::~TransportInput() = default;

InputState TransportInput::state() const
{
    InputState s = Input::state();
    s.counters.connects = connects_before_ + (transport_ ? transport_->connects() : 0);
    s.counters.rejected_peer = rejected_peer_before_ + (transport_ ? transport_->rejectedPeer() : 0);
    s.peers = transport_ && transport_->connected() ? transport_->peers() : 0;
    s.status_fail_streak = status_fail_streak_;
    return s;
}

void TransportInput::start()
{
    if (running_)
        return;
    running_ = true;
    if (transport_) {
        connects_before_ += transport_->connects();
        rejected_peer_before_ += transport_->rejectedPeer();
    }
    for (const auto& f : line_framers_)
        line_rejects_closed_ += f.second.rejected();
    line_framers_.clear();
    resetStreams();
    transport_ = makeTransport(io_, def());
    transport_->onData([this](const TransportData& d) {
        noteData(d.size);
        handleData(d);
    });
    transport_->onStreamClosed([this](uint64_t stream) {
        const auto it = line_framers_.find(stream);
        if (it != line_framers_.end()) {
            line_rejects_closed_ += it->second.rejected();
            line_framers_.erase(it);
        }
        handleStreamClosed(stream);
    });
    transport_->onConnection([this](bool up) {
        syncError();
        setConnected(up);
        handleConnection(up);
    });
    transport_->start();
    if (status_poller_ && running_)
        status_poller_->start();
}

void TransportInput::stop()
{
    if (!running_)
        return;
    running_ = false;
    if (status_poller_)
        status_poller_->stop();
    transport_->stop();
    syncError();
    markStopped();
}

void TransportInput::tick(std::chrono::system_clock::time_point now)
{
    Input::tick(now);
    if (transport_)
        syncError();
}

void TransportInput::feedLines(const TransportData& data, const skycue::net::Framer::Out& out)
{
    const bool datagram = transport_->datagrams();
    auto it = line_framers_.find(datagram ? 0 : data.stream);
    if (it == line_framers_.end())
        it = line_framers_.emplace(datagram ? 0 : data.stream, skycue::net::LineFramer(64 * 1024)).first;
    it->second.feed(data.data, data.size, out);
    if (datagram) {
        static const uint8_t newline = '\n';
        it->second.feed(&newline, 1, out);
    }
}

uint64_t TransportInput::lineRejects() const
{
    uint64_t n = line_rejects_closed_;
    for (const auto& f : line_framers_)
        n += f.second.rejected();
    return n;
}

void TransportInput::syncError()
{
    const std::string& e = transport_->lastError();
    if (e == copied_error_)
        return;
    copied_error_ = e;
    if (!e.empty() && reportsTransportErrors())
        noteError(e);
}

void TransportInput::handleStatusResult(const std::string* body, const std::string& error)
{
    std::optional<DeviceStatus> status;
    std::string reason = error;
    if (body) {
        status = parsePingStatusDocument(*body, std::chrono::system_clock::now());
        if (!status)
            reason = "not a pingStation status document";
    }
    if (!status) {
        ++counters_.status_fail;
        ++status_fail_streak_;
        noteError("status: " + reason);
        return;
    }
    ++counters_.status_ok;
    status_fail_streak_ = 0;
    noteStatus(std::move(*status));
}

}  // namespace skycued
