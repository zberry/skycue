#include "inputs/ping_json_input.hpp"

#include "inputs/ping_json.hpp"
#include "log.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

namespace {

constexpr std::chrono::seconds kDuplicateWindow{1}, kDuplicateMemory{10};
constexpr std::size_t kMaxRecent = 4096;

std::vector<uint8_t> bytesOfHex(const std::string& hex)  // PingResult hex is lowercase and even
{
    auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

}  // namespace

PingJsonInput::PingJsonInput(boost::asio::io_context& io, InputDef def, const RecordOptions& record,
                             RawDecodeOptions decode)
    : TransportInput(io, std::move(def)), datagrams_(this->def().transport == InputTransport::udp)
{
    clock_ = std::move(decode.clock);
    if (this->def().source == PingSource::decode) {
        if (decode.reference)
            fallback_reference_ = decode::cpr::Position{decode.reference->first, decode.reference->second};
        reference_ = fallback_reference_;
        decode::DecoderConfig cfg;
        cfg.reference = reference_;
        modes_ = std::make_unique<decode::ModesDecoder>(cfg, this->def().id);
        uat_ = std::make_unique<decode::UatDecoder>(cfg, this->def().id);
        auto emit = [this](const TrackObservation& o) { emitObservation(o); };
        modes_->onObservation(emit);
        uat_->onObservation(emit);
    }
    if (record.record_dir.empty())
        return;
    if (this->def().capture)
        capture_ = std::make_unique<CaptureWriter>(record.record_dir, this->def().id, record.max_bytes);
    if (record.record_raw)
        raw_ = std::make_unique<RawFrameWriter>(record.record_dir, this->def().id, record.max_bytes);
}

PingJsonInput::~PingJsonInput() = default;

InputState PingJsonInput::state() const
{
    InputState s = TransportInput::state();
    if (modes_) {
        const decode::DecodeCounters& m = modes_->counters();
        const decode::DecodeCounters& u = uat_->counters();
        InputCounters& c = s.counters;
        s.decoding = true;
        s.decoder_keys = modes_->keys() + uat_->keys();
        c.frames = m.frames + u.frames;
        c.crc_fail = m.crc_fail + u.crc_fail;
        c.crc_fixed = m.crc_fixed + u.crc_fixed;
        c.ap_unconfirmed = m.ap_unconfirmed + u.ap_unconfirmed;
        c.no_position = m.no_position + u.no_position;
        c.cpr_pending = m.cpr_pending + u.cpr_pending;
        c.cpr_rejected = m.cpr_rejected + u.cpr_rejected;
        c.decoded = m.decoded + u.decoded;
        c.unmapped = m.unmapped + u.unmapped;
        c.unknown_type = m.unknown_type + u.unknown_type;
        c.evicted = m.evicted + u.evicted;
        c.decoder_observations = m.observations + u.observations;
    }
    return s;
}

void PingJsonInput::decodeFrames(const PingResult& r, std::chrono::system_clock::time_point arrival)
{
    // Aircraft whose rawData is missing: not a frame, not an unknown length.
    const std::size_t with_raw = r.raw_frames.size() + r.raw_unknown_len;
    if (r.observations.size() > with_raw)
        counters_.no_raw += r.observations.size() - with_raw;
    if (r.raw_frames.empty())
        return;

    // The device's last status position, else the configured one (spec 8.1).
    std::optional<decode::cpr::Position> ref = fallback_reference_;
    if (const auto& st = deviceStatus(); st && st->lat && st->lon)
        ref = decode::cpr::Position{*st->lat, *st->lon};
    const bool same = ref.has_value() == reference_.has_value() &&
                      (!ref || (ref->lat == reference_->lat && ref->lon == reference_->lon));
    if (!same) {
        reference_ = ref;
        modes_->setReference(ref);
        uat_->setReference(ref);
    }

    for (auto it = recent_.begin(); it != recent_.end();)
        it = arrival - it->second > kDuplicateMemory ? recent_.erase(it) : std::next(it);
    if (recent_.size() > kMaxRecent)
        recent_.clear();

    for (const RawFrame& f : r.raw_frames) {
        if (!running())
            return;
        std::vector<uint8_t> bytes = bytesOfHex(f.hex);
        const auto seen = recent_.find(bytes);
        if (seen != recent_.end() && f.time >= seen->second && f.time - seen->second < kDuplicateWindow) {
            ++counters_.raw_duplicates;
            continue;
        }
        recent_[bytes] = f.time;
        if (f.kind == RawFrame::Kind::adsb_1090) {
            modes_->feed(bytes, f.time, std::nullopt);
        } else {
            decode::uat::UatFrame frame;
            frame.bytes = std::move(bytes);
            uat_->feed(frame, f.time, std::nullopt);
        }
    }
}

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
    const auto arrival = clock_ ? clock_() : std::chrono::system_clock::now();
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
    if (modes_) {
        if (running())
            decodeFrames(r, arrival);
        return;
    }
    for (auto& obs : r.observations) {
        if (!running())
            return;
        emitObservation(std::move(obs));
    }
}

}  // namespace skycued
