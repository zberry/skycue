#pragma once

// PingJsonInput: pingStation 3 JSON over UDP (spec §5.1), or one message per
// line over tcp_listen and tcp_connect. Each datagram (as received) or line
// is also published to the raw tap as `pingstation_json`. Each message is parsed with parsePingDatagram: its observations are passed
// on, a status updates device_status and fires onStatus, a datagram or
// aircraft that does not parse is counted in rejected and noted in
// last_error. A squawk that is not a display code is logged once per input.
// A status_url is polled over HTTP by TransportInput (spec §5.3).
//
// With a record_dir (spec §5.4): `capture: true` writes every datagram to a
// CaptureWriter, and record_raw writes the rawData frames to a
// RawFrameWriter (rawData of another length is counted in raw_unknown_len).
// Both are flushed on tick() and stop().
//
// With `source: decode` (spec §8.2; RawDecodeOptions::enabled is ignored, the
// input's own choice, also under TRACK_DECODER=readsb) the station's decoded
// fields are not used: every rawData frame is decoded by length (7/14 bytes by
// a ModesDecoder, 18/34 by a UatDecoder), observations leave through
// emitObservation with this input's id, and the parsed observations are
// dropped. The frame's arrival is the datagram's, or the clamped GPS counter
// time when utcSync is 1. A datagram's aircraft without rawData count in
// no_raw; byte-equal rawData within 1 s of the previous identical one is
// skipped and counts in raw_duplicates (the memory is pruned at 10 s). The
// decoders' reference is the device's last status position, else
// RawDecodeOptions::reference. state() reports the two decoders' counters
// summed. In `device` mode nothing changes.

#include "decode/modes_decoder.hpp"
#include "decode/uat_decoder.hpp"
#include "inputs/beast_raw_input.hpp"  // RawDecodeOptions
#include "inputs/capture_writer.hpp"
#include "inputs/input.hpp"
#include "inputs/ping_json.hpp"

#include <boost/asio/io_context.hpp>

#include <functional>
#include <optional>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace skycued {

class PingJsonInput : public TransportInput {
public:
    PingJsonInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record = {},
                  RawDecodeOptions decode = {});
    ~PingJsonInput() override;

    InputState state() const override;
    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

protected:
    void handleData(const TransportData& data) override;

private:
    void flushWriters();
    // One message: a datagram as received, or one line of a TCP stream.
    void handleText(const uint8_t* bytes, std::size_t size, const boost::asio::ip::address& peer);

    // `source: decode`: one result's frames through the decoders.
    void decodeFrames(const PingResult& r, std::chrono::system_clock::time_point arrival);

    bool datagrams_;  // udp: whole datagrams; else lines
    std::function<std::chrono::system_clock::time_point()> clock_;  // arrival; empty = system_clock::now
    std::optional<decode::cpr::Position> fallback_reference_;
    std::optional<decode::cpr::Position> reference_;  // what the decoders hold now
    std::unique_ptr<decode::ModesDecoder> modes_;     // null unless source decode
    std::unique_ptr<decode::UatDecoder> uat_;
    std::map<std::vector<uint8_t>, std::chrono::system_clock::time_point> recent_;  // rawData -> last time

    bool bad_squawk_logged_ = false;
    std::unique_ptr<CaptureWriter> capture_;  // null unless capture and a record_dir
    std::unique_ptr<RawFrameWriter> raw_;     // null unless record_raw and a record_dir
};

}  // namespace skycued
