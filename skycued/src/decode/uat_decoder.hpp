#pragma once

// The 978 MHz decoder: UAT downlink frames in, TrackObservations out. Pure: no
// I/O and no clock, every call carries the arrival time. One decoder per input.
// Modelled on ModesDecoder (same key LRU, field ages and counters) but there is
// no CPR: every state vector carries a whole position.
// Counters: each frame counts exactly one of unknown_type, no_position, unmapped
// (types 7..10) or decoded. A position dropped by the range check counts in both
// cpr_rejected and no_position; the frame's other fields still merge. Callsign,
// category, squawk and emergency live with the key; all other fields expire at 60 s.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

#include "decode/key_state.hpp"
#include "decode/modes_decoder.hpp"  // DecoderConfig, DecodeCounters
#include "decode/uat_frame.hpp"
#include "observation.hpp"

namespace skycued::decode {

struct UatKeyState {
    StateKey key;
    skycue::Source source = skycue::Source::uat_978;
    TimePoint last_seen{};
    std::list<StateKey>::iterator lru;

    std::optional<cpr::Position> first_pos;  // anchor of the range check without a reference

    Timed<std::string> callsign, category, squawk, emergency;
    Timed<int32_t> alt_baro, alt_geom;
    Timed<float> gs, track, baro_rate, geom_rate;
    Timed<uint8_t> nic, nac_p;
    Timed<bool> on_ground;

    // Level (dBFS) of the last 8 frames that carried one.
    std::array<double, 8> rssi_ring{};
    std::size_t rssi_count = 0;
    std::size_t rssi_next = 0;

    bool dirty = false;  // a tracked field was first seen or changed in the frame being processed
};

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

private:
    UatKeyState& obtain(const StateKey& k, skycue::Source src);
    void evictIdle();

    DecoderConfig cfg_;
    std::string input_id_;
    std::function<void(const skycued::TrackObservation&)> handler_;
    DecodeCounters counters_;
    TimePoint now_{};  // max-so-far arrival time
    bool have_now_ = false;
    std::unordered_map<StateKey, UatKeyState, StateKeyHash> keys_;
    std::list<StateKey> lru_;  // oldest first
};

}  // namespace skycued::decode
