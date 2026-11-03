#pragma once

// PingJsonInput: pingStation 3 JSON over UDP (spec §5.1). Each datagram is
// one message, parsed with parsePingDatagram: its observations are passed
// on, a status updates device_status and fires onStatus, a datagram or
// aircraft that does not parse is counted in rejected and noted in
// last_error. A squawk that is not a display code is logged once per input.
// A status_url is polled over HTTP by TransportInput (spec §5.3).
//
// With a record_dir (spec §5.4): `capture: true` writes every datagram to a
// CaptureWriter, and record_raw writes the rawData frames to a
// RawFrameWriter (rawData of another length is counted in raw_unknown_len).
// Both are flushed on tick() and stop().

#include "inputs/capture_writer.hpp"
#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>

namespace skycued {

class PingJsonInput : public TransportInput {
public:
    PingJsonInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record = {});
    ~PingJsonInput() override;

    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

protected:
    void handleData(const TransportData& data) override;

private:
    void flushWriters();

    bool bad_squawk_logged_ = false;
    std::unique_ptr<CaptureWriter> capture_;  // null unless capture and a record_dir
    std::unique_ptr<RawFrameWriter> raw_;     // null unless record_raw and a record_dir
};

}  // namespace skycued
