#pragma once

// Reassembly of segmented APDUs (docs/fisb-reference.md §2.1; design §10.4).
// A set is keyed on (product ID, product file ID). Segments may arrive in
// any order and over several broadcast cycles; a repeated segment is
// ignored. When every slot is filled the payloads are joined: segment 1
// whole, then each later segment minus its first 6 bytes when the product
// is TWGO (every TWGO segment repeats the 6-byte TWGO header).

#include "fisb/apdu.hpp"
#include "fisb/products.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace skycued {
namespace fisb {

class SegmentAssembler {
public:
    struct Done {
        ApduHeader hdr;  // segment 1's header
        std::vector<uint8_t> payload;
    };

    static constexpr std::size_t kDefaultMaxOpen = 512;
    static constexpr std::size_t kTwgoHeaderBytes = 6;

    explicit SegmentAssembler(std::size_t max_open = kDefaultMaxOpen) : max_open_(max_open ? max_open : 1) {}

    // p/n is the APDU payload (after header_bytes). An unsegmented header
    // passes straight through. Returns the joined payload when this segment
    // completes its set; nullopt while the set is incomplete, for a
    // duplicate, and for a rejected segment (number 0 or past the count,
    // count 0, or a TWGO segment > 1 shorter than the repeated header).
    // A segment whose count differs from its open set's restarts the set.
    // When a new set would exceed the cap, the oldest open set is evicted.
    std::optional<Done> add(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint now);

    // Drops sets whose first segment arrived more than timeout before now.
    // Returns how many were dropped (also added to timedOut()).
    std::size_t sweep(TimePoint now, std::chrono::minutes timeout = std::chrono::minutes(60));

    std::size_t open() const { return sets_.size(); }
    uint64_t timedOut() const { return timed_out_; }
    uint64_t evicted() const { return evicted_; }
    uint64_t rejected() const { return rejected_; }
    uint64_t duplicates() const { return duplicates_; }

private:
    struct Set {
        TimePoint first;
        uint64_t seq;  // insertion order, to evict the oldest deterministically
        ApduHeader first_hdr;  // header of segment 1, once seen
        std::vector<std::optional<std::vector<uint8_t>>> slots;
        std::size_t filled = 0;
    };
    using Key = std::pair<uint16_t, uint16_t>;  // (product_id, file id)

    void evictOldest();

    std::size_t max_open_;
    std::map<Key, Set> sets_;
    uint64_t seq_ = 0;
    uint64_t timed_out_ = 0, evicted_ = 0, rejected_ = 0, duplicates_ = 0;
};

}  // namespace fisb
}  // namespace skycued
