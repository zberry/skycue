#include "inputs/ping_json_input.hpp"

#include "inputs/ping_json.hpp"
#include "log.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

PingJsonInput::PingJsonInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record)
    : TransportInput(io, std::move(def)), datagrams_(this->def().transport == InputTransport::udp)
{
    if (record.record_dir.empty())
        return;
    if (this->def().capture)
        capture_ = std::make_unique<CaptureWriter>(record.record_dir, this->def().id, record.max_bytes);
    if (record.record_raw)
        raw_ = std::make_unique<RawFrameWriter>(record.record_dir, this->def().id, record.max_bytes);
}

PingJsonInput::~PingJsonInput() = default;

void PingJsonInput::flushWriters()
{
    if (capture_)
        capture_->flush();
    if (raw_)
        raw_->flush();
}

void PingJsonInput::stop()
{
    TransportInput::stop();
    flushWriters();
}

void PingJsonInput::tick(std::chrono::system_clock::time_point now)
{
    TransportInput::tick(now);
    flushWriters();
}

void PingJsonInput::handleData(const TransportData& data)
{
    if (datagrams_) {  // one datagram is one message, newline or not
        handleText(data.data, data.size, data.peer);
        return;
    }
    feedLines(data, [this, &data](const uint8_t* line, std::size_t size) {
        if (running())
            handleText(line, size, data.peer);
    });
}

void PingJsonInput::handleText(const uint8_t* bytes, std::size_t size, const boost::asio::ip::address& peer)
{
    publishRaw(RawKind::pingstation_json, bytes, size);
    ++counters_.messages;
    const auto arrival = std::chrono::system_clock::now();
    if (capture_ && !capture_->write(CaptureLine{arrival, def().id, transportName(def().transport),
                                                 std::vector<uint8_t>(bytes, bytes + size)}))
        ++counters_.capture_dropped;
    const std::string text(reinterpret_cast<const char*>(bytes), size);
    PingResult r = parsePingDatagram(text, PingContext{arrival, def().time_mode});
    if (raw_)
        for (const auto& frame : r.raw_frames)
            if (!raw_->write(frame))
                ++counters_.capture_dropped;
    counters_.rejected += r.rejected;
    counters_.clock_rejects += r.clock_rejects;
    counters_.raw_unknown_len += r.raw_unknown_len;
    noteClockOffset(r.clock_offset_s);
    if (r.rejected)
        noteError("datagram from " + peer.to_string() + " not understood (" + std::to_string(r.rejected) +
                  " rejected)");
    if (r.bad_squawk && !bad_squawk_logged_) {
        bad_squawk_logged_ = true;
        LOG_WARN("input " << def().id << ": a squawk is not a 4-digit octal code (dropped; logged once)");
    }
    if (r.status && running())
        noteStatus(std::move(*r.status));
    for (auto& obs : r.observations) {
        if (!running())
            return;
        emitObservation(std::move(obs));
    }
}

}  // namespace skycued
