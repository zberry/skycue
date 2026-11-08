#pragma once

// BeastRawInput: a Beast binary stream (readsb --net-bo-port, dump1090, an
// SDR front end) or AVR text over any transport (hub spec §5.1). It parses
// nothing: each frame is published to the raw tap as `beast` exactly as it
// was received, and, with `capture: true` and a recording.dir, written to
// recording.dir/raw-<id>-<stamp>.bin (BeastCaptureWriter).
//
// format "beast" (default): one beast::FrameSplitter per TCP stream and, for
// UDP, per source address and port (a datagram is fed like a TCP chunk, so a frame may
// span datagrams of one sender and a datagram may hold several frames); at
// most 64 senders are tracked, all splitters reset when a 65th appears.
// `rejected` gains one for every feed that made a splitter skip bytes.
//
// format "avr": lines ('@' + 12 ticks digits + hex + ';' or '*' + hex + ';')
// are cut as TransportInput::feedLines does and parsed with parseAvrLine; a
// Mode S message becomes beast::encodeFrame(hex, 0), which is what is
// published and captured (so the .bin is always Beast). A line parseAvrLine
// refuses (Mode A/C, junk) counts in `rejected`; blank lines are ignored.
//
// messages = frames published, bytes as received. Flushed on tick() and stop().
//
// With RawDecodeOptions::enabled (inputs.decoder=skycued) the input also owns
// one decode::ModesDecoder (frame decoder spec 7, 8) fed after publishFrame,
// so the tap and the capture still see every frame. Its observations leave
// through emitObservation with this input's id; state() copies the decoder's
// and the frame source's counters into InputCounters. Without it nothing
// changes: publish and capture only.

#include "inputs/capture_writer.hpp"
#include "decode/frame_source.hpp"
#include "decode/modes_decoder.hpp"
#include "inputs/input.hpp"
#include "raw_formats.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <memory>

namespace skycued {

struct RawDecodeOptions {
    bool enabled = false;
    std::optional<std::pair<double, double>> reference;  // {lat, lon} (InputManager::inputReference); none: global CPR only
    std::function<std::chrono::system_clock::time_point()> clock;  // arrival time; empty = system_clock::now
};

class BeastRawInput : public TransportInput {
public:
    static constexpr std::size_t kMaxSenders = 64;

    BeastRawInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record = {},
                   RawDecodeOptions decode = {});
    ~BeastRawInput() override;

    InputState state() const override;
    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

protected:
    void handleData(const TransportData& data) override;
    void handleStreamClosed(uint64_t stream) override;
    void resetStreams() override;

private:
    void feedBeast(const TransportData& data);
    void feedAvr(const TransportData& data);
    void publishFrame(const std::string& frame);
    void decodeFrame(const beast::Frame& f);
    void decodeAvr(const std::string& line);
    std::chrono::system_clock::time_point arrival() const;

    bool avr_;
    bool datagrams_;
    std::map<uint64_t, beast::FrameSplitter> streams_;                      // TCP, per stream
    std::map<std::pair<boost::asio::ip::address, uint16_t>, beast::FrameSplitter> senders_;  // UDP, per source address and port
    std::vector<beast::Frame> frames_;                                      // scratch
    uint64_t avr_rejected_ = 0;
    std::unique_ptr<decode::ModesDecoder> decoder_;  // null unless decode_raw
    decode::FrameSourceCounters source_counters_;
    std::function<std::chrono::system_clock::time_point()> clock_;
    std::unique_ptr<BeastCaptureWriter> capture_;  // null unless capture and a record_dir
};

}  // namespace skycued
