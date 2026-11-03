#pragma once

// Dump978Input: an Input over Dump978RawSource (dump978-fa's raw port).
// messages = downlinks + uplinks, uplinks = uplinks, bytes = bytes read;
// downlinks carry no
// observation (readsb gets them through its own dump978 connection). A new
// source is made on every start().

#include "dump978_raw_source.hpp"
#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <memory>

namespace skycued {

class Dump978Input : public Input {
public:
    Dump978Input(boost::asio::io_context& io, InputDef def);
    ~Dump978Input() override;

    InputState state() const override;
    void start() override;
    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

    uint64_t downlinkFrames() const;
    uint64_t uplinkFrames() const;

private:
    boost::asio::io_context& io_;
    std::unique_ptr<Dump978RawSource> source_;  // kept after stop() until the next start()
    bool running_ = false;
    uint64_t downlink_before_ = 0, uplink_before_ = 0, bytes_before_ = 0;  // of earlier sources
    uint64_t downlink_seen_ = 0;                        // downlinkFrames() at the last tick
};

}  // namespace skycued
