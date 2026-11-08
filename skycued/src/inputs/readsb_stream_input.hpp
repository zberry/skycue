#pragma once

// ReadsbStreamInput: readsb's JSON lines over udp or tcp_listen (over
// tcp_connect a readsb input stays a SourceInput over ReadsbJsonSource). Each
// line is published to the raw tap as `json` before it is parsed with
// parseReadsbLine; a line that does not parse is counted in rejected (as are
// lines over 64 KiB) and logged at debug level. messages = observations +
// rejected. Lines are cut as TransportInput::feedLines does.

#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

namespace skycued {

class ReadsbStreamInput : public TransportInput {
public:
    ReadsbStreamInput(boost::asio::io_context& io, InputDef def);

    InputState state() const override;

protected:
    void handleData(const TransportData& data) override;

private:
    void handleLine(const uint8_t* line, std::size_t size);
};

}  // namespace skycued
