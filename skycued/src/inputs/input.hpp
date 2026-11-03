#pragma once

// Input: one running input (spec §4.4): a transport or source plus its
// parser, its counters and its state. InputManager owns them.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. Register callbacks before start(). start() may be called again
// after stop() (the manager re-enables an input that way); stop() is
// idempotent, never invokes a callback, and leaves the input not connected.
// No callback runs after stop().

#include "inputs/input_def.hpp"
#include "inputs/input_types.hpp"
#include "inputs/transport.hpp"
#include "observation.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace skycued {

// receiving: data arrived within this window.
constexpr std::chrono::seconds kReceivingWindow{30};
// noteError logs at most once per input per this interval.
constexpr std::chrono::seconds kErrorLogInterval{10};

struct InputState {
    bool connected = false, receiving = false;
    std::optional<std::chrono::system_clock::time_point> last_data_time;
    InputCounters counters;
    std::optional<DeviceStatus> device_status;
    std::optional<double> clock_offset_s;
    std::string last_error;
    uint32_t status_fail_streak = 0;
    std::size_t peers = 0;
};

class Input {
public:
    explicit Input(InputDef def);
    virtual ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    const InputDef& def() const { return def_; }
    virtual InputState state() const;
    virtual void start() = 0;
    virtual void stop() = 0;
    // Called about once a second: updates `receiving` (and flushes writers).
    virtual void tick(std::chrono::system_clock::time_point now);

    void onObservation(std::function<void(const TrackObservation&)> handler);
    void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler);
    void onConnection(std::function<void(bool)> handler);
    void onStatus(std::function<void()> handler);  // device_status changed

    // When noteError last wrote a log line; nullopt before the first.
    std::optional<std::chrono::steady_clock::time_point> lastErrorLoggedAt() const { return last_error_logged_at_; }

protected:
    // Counts the observation, sets input_id to def().id when it is empty (a
    // replay keeps the recorded id) and passes it on.
    void emitObservation(TrackObservation obs);
    void emitUplink(std::chrono::system_clock::time_point time, const std::string& hex);
    // Calls onConnection on a change only.
    void setConnected(bool connected);
    // For stop(): not connected, without a callback.
    void markStopped();
    // Data arrived: last_data_time is now, `bytes` are counted.
    void noteData(std::size_t bytes);
    // Sets last_error; logs it at WARN unless a line was logged in the last
    // kErrorLogInterval.
    void noteError(const std::string& message);
    void noteStatus(DeviceStatus status);
    void noteClockOffset(std::optional<double> offset_s);

    InputCounters counters_;

private:
    InputDef def_;
    bool connected_ = false, receiving_ = false;
    std::optional<std::chrono::system_clock::time_point> last_data_time_;
    std::optional<DeviceStatus> device_status_;
    std::optional<double> clock_offset_s_;
    std::string last_error_;
    std::optional<std::chrono::steady_clock::time_point> last_error_logged_at_;
    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void(std::chrono::system_clock::time_point, const std::string&)> on_uplink_;
    std::function<void(bool)> on_connection_;
    std::function<void()> on_status_;
};

// The shared half of the pingStation inputs: a transport from makeTransport,
// built on every start(). Counts bytes, connects, rejected_peer and peers,
// and copies the transport's lastError() into last_error whenever it changes.
class TransportInput : public Input {
public:
    TransportInput(boost::asio::io_context& io, InputDef def);
    ~TransportInput() override;

    InputState state() const override;
    void start() override;
    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

protected:
    // False once stop() was called (a handler may stop the input mid-delivery).
    bool running() const { return running_; }
    // One delivery; noteData has been called.
    virtual void handleData(const TransportData& data) = 0;
    virtual void handleStreamClosed(uint64_t /*stream*/) {}
    // A new transport is about to start: forget per-stream state.
    virtual void resetStreams() {}

private:
    void syncError();

    boost::asio::io_context& io_;
    std::unique_ptr<Transport> transport_;  // kept after stop() until the next start()
    bool running_ = false;
    uint64_t connects_before_ = 0, rejected_peer_before_ = 0;  // of earlier transports
    std::string copied_error_;
};

}  // namespace skycued
