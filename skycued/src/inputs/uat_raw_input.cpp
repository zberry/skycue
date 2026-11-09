#include "inputs/uat_raw_input.hpp"

#include "log.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace skycued {

UatRawInput::UatRawInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record,
                           RawDecodeOptions decode)
    : TransportInput(io, std::move(def))
{
    if (this->def().capture && !record.record_dir.empty())
        capture_ = std::make_unique<UatCaptureWriter>(record.record_dir, this->def().id, record.max_bytes);
    if (decode.enabled) {
        decode_ = std::move(decode);
        decoder_ = std::make_unique<decode::UatDecoder>(decoderConfigFrom(decode_), this->def().id);
        decoder_->onObservation([this](const TrackObservation& o) { emitObservation(o); });
    }
}

UatRawInput::~UatRawInput() = default;

void UatRawInput::stop()
{
    TransportInput::stop();
    if (capture_)
        capture_->flush();
}

void UatRawInput::tick(std::chrono::system_clock::time_point now)
{
    TransportInput::tick(now);
    if (capture_)
        capture_->flush();
}

InputState UatRawInput::state() const
{
    InputState s = TransportInput::state();
    s.counters.uplinks = uplink_;
    s.counters.messages = downlink_ + uplink_;
    s.counters.rejected += lineRejects();
    if (decoder_) {
        const decode::DecodeCounters& d = decoder_->counters();
        InputCounters& c = s.counters;
        s.decoding = true;
        s.decoder_keys = decoder_->keys();
        addDecodeCounters(c, d);
    }
    return s;
}

void UatRawInput::handleData(const TransportData& data)
{
    feedLines(data, [this](const uint8_t* line, std::size_t size) {
        if (running())
            handleLine(line, size);
    });
}

void UatRawInput::handleConnection(bool up)
{
    if (def().transport != InputTransport::tcp_connect)
        return;
    if (up) {
        LOG_INFO(def().id << ": connected to " << def().host << ":" << def().port);
    } else {
        // As LineClient wrote it: the library's reason without its "read: " prefix.
        std::string why = lastCloseReason();
        if (why.compare(0, 6, "read: ") == 0)
            why.erase(0, 6);
        LOG_INFO(def().id << ": connection closed (" << why << ")");
    }
}

bool UatRawInput::reportsTransportErrors() const
{
    return !(def().type == InputType::dump978 && def().transport == InputTransport::tcp_connect);
}

void UatRawInput::decodeDownlink(const uint8_t* line, std::size_t size)
{
    // handleLine published the line to the tap; a sink may have stopped the input.
    if (!running())
        return;
    if (const auto frame = decode::uat::parseDownlinkLine(std::string_view(reinterpret_cast<const char*>(line), size)))
        decoder_->feed(*frame, decode_.now());
    else
        ++counters_.rejected;  // not hex, odd length, or not an 18/34 byte frame
}

void UatRawInput::handleLine(const uint8_t* line, std::size_t size)
{
    if (size == 0 || (line[0] != '-' && line[0] != '+'))
        return;  // '!' metadata or unknown
    std::size_t end = 1;
    while (end < size && std::isxdigit(line[end]))
        ++end;
    if (end == 1 || (end < size && line[end] != ';'))
        return;
    publishRaw(RawKind::uat, line, size);
    if (capture_ &&
        !capture_->write(std::chrono::system_clock::now(), std::string_view(reinterpret_cast<const char*>(line), size)))
        ++counters_.capture_dropped;
    if (line[0] == '-') {
        ++downlink_;
        if (decoder_)
            decodeDownlink(line, size);
        return;
    }
    ++uplink_;
    emitUplink(std::chrono::system_clock::now(), std::string(reinterpret_cast<const char*>(line) + 1, end - 1));
}

}  // namespace skycued
