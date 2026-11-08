#pragma once

// UatRawInput: dump978-fa's raw UAT lines over any transport (spec §5.2 of
// the hub). Each line is "<sign><hex>;" optionally followed by "key=value;"
// metadata: '-' is a downlink frame (counted; readsb gets the UAT traffic
// through its own dump978 connection), '+' an uplink (counted and passed to
// onUplink with the receive time), '!' a metadata line (ignored). Anything
// else, or a frame without hex digits, is ignored. Every '-' and '+' line is
// also published to the raw tap as `uat`, as received without its terminator.
//
// messages = downlinks + uplinks, uplinks = uplinks. The built-in `dump978`
// is an instance of this class (type name `dump978`, log prefix `dump978:`);
// a `dump978` over tcp_connect reports no transport error in last_error, as
// before.
// Lines are cut as TransportInput::feedLines does; on tcp_connect the
// connection is logged at INFO as it always was.

#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>

namespace skycued {

class UatRawInput : public TransportInput {
public:
    UatRawInput(boost::asio::io_context& io, InputDef def);

    InputState state() const override;

    uint64_t downlinkFrames() const { return downlink_; }
    uint64_t uplinkFrames() const { return uplink_; }

protected:
    void handleData(const TransportData& data) override;
    void handleConnection(bool up) override;
    bool reportsTransportErrors() const override;

private:
    void handleLine(const uint8_t* line, std::size_t size);

    uint64_t downlink_ = 0, uplink_ = 0;
};

}  // namespace skycued
