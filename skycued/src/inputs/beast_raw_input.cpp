#include "inputs/beast_raw_input.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

BeastRawInput::BeastRawInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record)
    : TransportInput(io, std::move(def)), avr_(this->def().format == "avr"),
      datagrams_(this->def().transport == InputTransport::udp)
{
    if (this->def().capture && !record.record_dir.empty())
        capture_ = std::make_unique<BeastCaptureWriter>(record.record_dir, this->def().id, record.max_bytes);
}

BeastRawInput::~BeastRawInput() = default;

InputState BeastRawInput::state() const
{
    InputState s = TransportInput::state();
    if (avr_)
        s.counters.rejected += avr_rejected_ + lineRejects();
    return s;
}

void BeastRawInput::stop()
{
    TransportInput::stop();
    if (capture_)
        capture_->flush();
}

void BeastRawInput::tick(std::chrono::system_clock::time_point now)
{
    TransportInput::tick(now);
    if (capture_)
        capture_->flush();
}

void BeastRawInput::handleData(const TransportData& data)
{
    if (avr_)
        feedAvr(data);
    else
        feedBeast(data);
}

void BeastRawInput::handleStreamClosed(uint64_t stream) { streams_.erase(stream); }

void BeastRawInput::resetStreams()
{
    streams_.clear();
    senders_.clear();
}

void BeastRawInput::publishFrame(const std::string& frame)
{
    publishRaw(RawKind::beast, frame);
    ++counters_.messages;
    if (capture_ && !capture_->write(frame))
        ++counters_.capture_dropped;
}

void BeastRawInput::feedBeast(const TransportData& data)
{
    beast::FrameSplitter* splitter = nullptr;
    if (datagrams_) {
        auto it = senders_.find(data.peer);
        if (it == senders_.end()) {
            if (senders_.size() >= kMaxSenders)
                senders_.clear();  // a partial frame of any sender is forgotten
            it = senders_.emplace(data.peer, beast::FrameSplitter()).first;
        }
        splitter = &it->second;
    } else {
        splitter = &streams_[data.stream];
    }
    const uint64_t skipped_before = splitter->skippedBytes();
    frames_.clear();
    splitter->feed(data.data, data.size, frames_);
    if (splitter->skippedBytes() > skipped_before)
        ++counters_.rejected;
    // A handler may stop the input mid-delivery; `frames_` is ours to walk.
    for (const beast::Frame& f : frames_) {
        if (!running())
            return;
        publishFrame(f.raw);
    }
}

void BeastRawInput::feedAvr(const TransportData& data)
{
    feedLines(data, [this](const uint8_t* line, std::size_t size) {
        if (!running())
            return;
        const std::string text(reinterpret_cast<const char*>(line), size);
        if (text.find_first_not_of(" \t\r") == std::string::npos)
            return;  // blank
        const auto msg = parseAvrLine(text);
        const std::string frame = msg ? beast::encodeFrame(msg->hex, 0) : std::string();
        if (frame.empty()) {
            ++avr_rejected_;
            return;
        }
        publishFrame(frame);
    });
}

}  // namespace skycued
