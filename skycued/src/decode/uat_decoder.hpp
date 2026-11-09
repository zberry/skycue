#pragma once

// The 978 MHz decoder: UAT downlink frames in, TrackObservations out. Pure: no
// I/O and no clock, every call carries the arrival time. One decoder per input.
// Modelled on ModesDecoder (same key LRU, field ages and counters) but there is
// no CPR: every state vector carries a whole position.
// Counters: each frame counts exactly one of unknown_type, no_position, unmapped
// (types 7..10) or decoded. A position dropped by the range check counts in both
// cpr_rejected and no_position; the frame's other fields still merge. rs_errors
// sums the frames' ";rs=" counts. The gate in decoder_types.hpp (shared with the
// 1090 decoder): a change without a position emits only after 10 s without an
// accepted one and at most once per second (spec 7.2). Callsign,
// category, squawk and emergency live with the key; all other fields expire at 60 s.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "decode/decoder_types.hpp"
#include "decode/key_state.hpp"
#include "decode/key_table.hpp"
#include "decode/uat_frame.hpp"
#include "observation.hpp"

namespace skycued::decode {

class UatDecoder {
public:
    using Clock = std::chrono::system_clock;
    explicit UatDecoder(DecoderConfig cfg, std::string input_id);
    void onObservation(std::function<void(const skycued::TrackObservation&)> h);
    // One frame and its arrival time. rssi_dbfs, when given, overrides the frame's own
    // ";rssi=" value (nullopt: use the frame's, which may be absent).
    void feed(const uat::UatFrame& frame, Clock::time_point arrival, std::optional<double> rssi_dbfs = std::nullopt);
    const DecodeCounters& counters() const;
    std::size_t keys() const;
    // Replaces the reference position (a pingStation status moved); per-aircraft state is kept.
    void setReference(std::optional<cpr::Position> reference) { cfg_.reference = reference; }

private:

    DecoderConfig cfg_;
    std::string input_id_;
    std::function<void(const skycued::TrackObservation&)> handler_;
    DecodeCounters counters_;
    TimePoint now_{};  // max-so-far arrival time
    bool have_now_ = false;
    KeyTable<UatKeyState> keys_;
};

}  // namespace skycued::decode
