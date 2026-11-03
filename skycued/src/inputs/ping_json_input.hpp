#pragma once

// PingJsonInput: pingStation 3 JSON over UDP (spec §5.1). Each datagram is
// one message, parsed with parsePingDatagram: its observations are passed
// on, a status updates device_status and fires onStatus, a datagram or
// aircraft that does not parse is counted in rejected and noted in
// last_error. A squawk that is not a display code is logged once per input.

#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

namespace skycued {

class PingJsonInput : public TransportInput {
public:
    PingJsonInput(boost::asio::io_context& io, InputDef def);

protected:
    void handleData(const TransportData& data) override;

private:
    bool bad_squawk_logged_ = false;
};

}  // namespace skycued
