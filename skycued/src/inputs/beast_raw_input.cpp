#include "inputs/beast_raw_input.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

decode::DecoderConfig decoderConfigFrom(const RawDecodeOptions& o)
{
    decode::DecoderConfig cfg;
    if (o.reference)
        cfg.reference = decode::cpr::Position{o.reference->first, o.reference->second};
    return cfg;
}

std::chrono::system_clock::time_point RawDecodeOptions::now() const
{
    return clock ? clock() : std::chrono::system_clock::now();
}

BeastRawInput::BeastRawInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record,
                             RawDecodeOptions decode)
    : TransportInput(io, std::move(def)), avr_(this->def().format == "avr"),
      datagrams_(this->def().transport == InputTransport::udp)
{
    if (this->def().capture && !record.record_dir.empty())
        capture_ = std::make_unique<BeastCaptureWriter>(record.record_dir, this->def().id, record.max_bytes);
    if (decode.enabled) {
        decode_ = std::move(decode);
        decoder_ = std::make_unique<decode::ModesDecoder>(decoderConfigFrom(decode_), this->def().id);
        decoder_->onObservation([this](const TrackObservation& o) { emitObservation(o); });
    }
}

std::chrono::system_clock::time_point BeastRawInput::arrival() const
{
    return decode_.now();
}

void BeastRawInput::decodeFrame(const beast::Frame& f)
{
    if (const auto raw = decode::fromBeast(f, source_counters_))
        decoder_->feed(raw->bytes, arrival(), raw->signal);
}

void BeastRawInput::decodeAvr(const std::string& line)
{
    if (const auto raw = decode::fromAvrLine(line, source_counters_))
        decoder_->feed(raw->bytes, arrival(), raw->signal);
}

BeastRawInput::~BeastRawInput() = default;

InputState BeastRawInput::state() const
{
    InputState s = TransportInput::state();
    if (avr_)
        s.counters.rejected += avr_rejected_ + lineRejects();
    if (decoder_) {
        const decode::DecodeCounters& d = decoder_->counters();
        InputCounters& c = s.counters;
        s.decoding = true;
        s.decoder_keys = decoder_->keys();
        addDecodeCounters(c, d);
        c.mlat_skipped = source_counters_.mlat_skipped;
        c.modeac_skipped = source_counters_.modeac_skipped;
        c.other_skipped = source_counters_.other_skipped;
    }
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
        const auto key = std::make_pair(data.peer, data.peer_port);
        auto it = senders_.find(key);
        if (it == senders_.end()) {
            if (senders_.size() >= kMaxSenders)
                senders_.clear();  // a partial frame of any sender is forgotten
            it = senders_.emplace(key, beast::FrameSplitter()).first;
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
        if (decoder_ && running())  // a tap sink may have stopped the input
            decodeFrame(f);
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
        if (decoder_ && running())
            decodeAvr(text);
    });
}

}  // namespace skycued
