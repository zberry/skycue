#include "inputs/dump978_input.hpp"

#include <utility>

namespace skycued {

Dump978Input::Dump978Input(boost::asio::io_context& io, InputDef def) : Input(std::move(def)), io_(io) {}

Dump978Input::~Dump978Input()
{
    if (source_ && running_)
        source_->stop();
}

uint64_t Dump978Input::downlinkFrames() const { return downlink_before_ + (source_ ? source_->downlinkFrames() : 0); }

uint64_t Dump978Input::uplinkFrames() const { return uplink_before_ + (source_ ? source_->uplinkFrames() : 0); }

InputState Dump978Input::state() const
{
    InputState s = Input::state();
    s.counters.uplinks = uplinkFrames();
    s.counters.messages = downlinkFrames() + uplinkFrames();
    s.counters.bytes = bytes_before_ + (source_ ? source_->bytesRead() : 0);
    return s;
}

void Dump978Input::start()
{
    if (running_)
        return;
    running_ = true;
    if (source_) {
        downlink_before_ += source_->downlinkFrames();
        uplink_before_ += source_->uplinkFrames();
        bytes_before_ += source_->bytesRead();
    }
    source_ = std::make_unique<Dump978RawSource>(io_, def().host, def().port);
    source_->onUplink([this](std::chrono::system_clock::time_point t, const std::string& hex) {
        noteData(0);
        emitUplink(t, hex);
    });
    source_->onConnection([this](bool up) {
        if (up)
            ++counters_.connects;
        setConnected(up);
    });
    source_->start();
}

void Dump978Input::stop()
{
    if (!running_)
        return;
    running_ = false;
    source_->stop();
    markStopped();
}

void Dump978Input::tick(std::chrono::system_clock::time_point now)
{
    // Downlinks have no callback: a growing count is data too.
    if (downlinkFrames() != downlink_seen_) {
        downlink_seen_ = downlinkFrames();
        noteData(0);
    }
    Input::tick(now);
}

}  // namespace skycued
