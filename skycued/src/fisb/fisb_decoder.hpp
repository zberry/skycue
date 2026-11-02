#pragma once

// FisbDecoder: one dump978 uplink line -> FIS-B products. Runs the Task 1-3
// parsers in order: hex -> 432-byte payload (anything else is a bad frame)
// -> UAT header (application data not valid: nothing to decode) ->
// information frames -> type-0 APDUs -> segment reassembly (keyed by ground
// station) -> TWGO / generic text / global blocks. Each decoded product is
// handed to the onProduct callback at once, synchronously.
//
// Validation beyond the parsers': a TWGO shape with a point outside
// |lat| <= 90, |lon| <= 180 is dropped (counted in rejected_shapes), and a
// global block whose NW corner is outside them is dropped (blocks_rejected).
//
// Threading: not thread-safe; the Engine calls it on its io thread.

#include "fisb/global_block.hpp"
#include "fisb/products.hpp"
#include "fisb/segment_assembler.hpp"
#include "fisb/twgo.hpp"
#include "fisb/uplink_frame.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace skycued {
namespace fisb {

// A stable 64-bit identity for a ground station: its header lat/lon at
// 1e-4 degree (the header resolves about 2e-5 degree).
uint64_t stationKey(const UplinkHeader& h);

// One decoded product, as it left one APDU (or one reassembled set).
struct FisbProduct {
    uint16_t id = 0;                        // product ID
    TimePoint received{};                   // the reference time the uplink was fed with
    std::optional<TimePoint> product_time;  // the APDU's hh:mm (mm/dd) resolved against received
    uint64_t station = 0;                   // stationKey() of the sender
    double station_lat = 0, station_lon = 0;
    // 413: the text reports; TWGO (8, 11-17): one parsed APDU (text or
    // graphics half); global block (63, 64, 70, 71, 84, 90, 91, 103): one block
    // or empty-block list.
    std::variant<std::vector<TextRecord>, TwgoParsed, DecodedBlocks> body;
};

// FisbCounters plus what only the decoder knows.
struct FisbDecoderCounters : FisbCounters {
    uint64_t products = 0;         // FisbProducts emitted
    uint64_t bad_products = 0;     // a complete TWGO payload that did not parse
    uint64_t rejected_shapes = 0;  // TWGO shapes with a point outside lat/lon range
    uint64_t not_app_data = 0;     // uplinks whose header says the application data is not valid
    uint64_t empty_text = 0;       // a 413 product with no report in it: dropped
    uint64_t graphics_rejected = 0;  // a TWGO graphics half whose every shape was out of range
};

class FisbDecoder {
public:
    // experimental: decode APDU time options 1/3 and global blocks at >= 60 N
    // (FISB_EXPERIMENTAL; spec §10.6).
    explicit FisbDecoder(bool experimental = false);

    // One uplink line: bare hex, "+hex;..." (dump978) or "<epoch_ms> +hex;"
    // (rawrec). ref is the reference time for every partial time in it (the
    // Engine's data clock; spec §5.3: never the wall clock during replay).
    // Never throws (a throwing callback propagates).
    void feed(TimePoint ref, const std::string& hex_line);
    void onProduct(std::function<void(const FisbProduct&)> handler);
    // Drops segment sets older than 60 min (spec §10.4).
    void sweep(TimePoint now);

    const FisbDecoderCounters& counters() const { return counters_; }
    const GlobalBlockStats& blockStats() const { return block_stats_; }
    std::size_t segmentsOpen() const { return segments_.open(); }

private:
    void apdu(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref, const UplinkHeader& uh,
              uint64_t station);
    void emit(FisbProduct&& product);

    bool experimental_;
    SegmentAssembler segments_;
    FisbDecoderCounters counters_;
    GlobalBlockStats block_stats_;
    std::function<void(const FisbProduct&)> on_product_;
};

}  // namespace fisb
}  // namespace skycued
