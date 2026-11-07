#pragma once

// The 1090 MHz decoder: Mode S frames in, TrackObservations out. Pure: no I/O
// and no clock, every call carries the arrival time. One decoder per input.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "decode/cpr.hpp"
#include "decode/frame.hpp"
#include "decode/key_state.hpp"
#include "observation.hpp"

namespace skycued::decode {

struct DecoderConfig {
    std::optional<cpr::Position> reference;  // the input's reference position (spec section 8.1), may be absent
    std::size_t max_keys = 20000;            // our choice (design section 3)
    std::chrono::seconds idle_evict{300};    // our choice (design section 3)
};

struct DecodeCounters {
    uint64_t frames = 0, crc_fail = 0, crc_fixed = 0, ap_unconfirmed = 0, no_position = 0, cpr_pending = 0,
             cpr_rejected = 0, decoded = 0, unmapped = 0, unknown_type = 0, evicted = 0, observations = 0;
};

class ModesDecoder {
public:
    using Clock = std::chrono::system_clock;
    explicit ModesDecoder(DecoderConfig cfg, std::string input_id);
    void onObservation(std::function<void(const TrackObservation&)> h);
    // One frame: 7 or 14 bytes, its arrival time, and the Beast signal byte (nullopt for AVR/pingStation).
    void feed(const std::vector<uint8_t>& bytes, Clock::time_point arrival, std::optional<uint8_t> signal);
    const DecodeCounters& counters() const;
    std::size_t keys() const;

private:
    KeyState* find(const StateKey& k);
    KeyState& obtain(const StateKey& k, skycue::Source src);  // find or create, then mark as seen
    void evictIdle();
    void onEs(const Frame& f, const StateKey& key, skycue::Source src, Clock::time_point arrival,
              std::optional<uint8_t> signal);
    void onAp(const Frame& f, Clock::time_point arrival, std::optional<uint8_t> signal);
    void onDf11(const Frame& f, uint32_t residual, std::optional<uint8_t> signal);
    bool positionStep(KeyState& ks, const CprFields& c, cpr::Position& out);
    void reject(KeyState& ks);
    void setGround(KeyState& ks, bool g);  // stores it and notes that this frame asserted it
    void emit(KeyState& ks, const Frame& f, Clock::time_point arrival, const cpr::Position* pos);

    DecoderConfig cfg_;
    std::string input_id_;
    std::function<void(const TrackObservation&)> handler_;
    DecodeCounters counters_;
    TimePoint now_{};  // max-so-far arrival time
    bool have_now_ = false;
    std::optional<bool> frame_ground_;  // what the frame being processed said about on_ground
    std::unordered_map<StateKey, KeyState, StateKeyHash> keys_;
    std::list<StateKey> lru_;  // oldest first
};

}  // namespace skycued::decode
