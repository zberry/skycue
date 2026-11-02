#pragma once

// FisbStore: the current FIS-B picture built from FisbDecoder's products,
// with the rulings of design §10 (2026-11-02):
//
// Rasters (§10.1, §10.6). One mosaic per product ID (for 70/71/90/91 per
// product ID and altitude code, which are separate layers), carrying the
// newest APDU hh:mm seen as its product time. A block merges in when its
// product time is no more than 10 min older than the mosaic's; a block
// already held with a newer time wins (equal times: the later reception
// wins). When the mosaic's time advances, held blocks more than 10 min older
// than the new time are pruned (they belong to an old cycle). An empty-block
// list adds its blocks with every bin 0, or 1 for CONUS (64; reference
// §5.4, ruling §10.6). At most 4096 blocks per mosaic: past that the oldest
// block (by product time, then arrival) is evicted and counted. A mosaic is
// published (a radar/raster FisbEvent carrying a copy of every block) once no
// block has arrived for 10 s, or 30 s after the first block not yet
// published while blocks keep coming.
//
// Text (§10.5). METAR and TAF: one per (kind, station), replaced only by a
// strictly newer `issued`. Winds aloft: one per (station, forecast period),
// replaced by a newer issue. PIREPs (and unclassified 413 text): a set keyed
// by (kind, station, issued, FNV-1a of the text); a repeat changes nothing.
//
// TWGO (§10.2). Records are keyed by TwgoKey (pid-year-report-location-
// month). The text and graphics halves arrive in separate APDUs and are
// paired. A non-empty text that differs from the stored one replaces it and
// drops the old graphics (reference §6.5); an empty text (TFR keep-alive)
// only renews. A record is complete when it has non-empty text (G-AIRMET,
// product 14, which has no text: when it has graphics); only complete records
// are published and listed. Cancellation (text status 0 or object_status 13)
// flags the record cancelled, clears its text and graphics, publishes an
// expired event with reason "cancelled", and keeps only that cancellation for
// 30 min: every APDU for the key meanwhile (a repeated cancel, a keep-alive,
// a graphics half still broadcast active) is ignored and counted. After the
// 30 min the key is free and a full record publishes afresh.
//
// Expiry (§10.3), always against the `now` passed in (the data clock):
// NEXRAD 63/64 and lightning 103 75 min from product time (reference §7);
// other rasters (70/71/84/90/91) 105 min from product time; METAR 120 min from issued; TAF at the end of its ddhh/ddhh validity
// (else 120 min from issued); PIREP 120 min from issued; winds 12 h from
// their valid time unless replaced by the next issue first; TWGO at the
// latest of its end times (the NOTAM text's end of validity, the graphics
// end) when it has one, else 70 min after the last reception; an unmatched
// half 720 min after its last reception (or its graphics end if earlier);
// cancelled 30 min after the cancel. A product already expired when it
// arrives is not stored (counted as stale). Every removal of a published
// item yields an expired event with reason "expired".
//
// Events are returned by apply() (text, twgo, a mosaic reaching its 30 s
// limit) and sweep() (mosaic quiescence, expiry); an unchanged repeat yields
// none.
//
// Threading: not thread-safe; the Engine owns it on its io thread.

#include "fisb/fisb_decoder.hpp"
#include "fisb/products.hpp"
#include "fisb/twgo.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace fisb {

// Spec §10.3 lifetimes and the §10.1 mosaic timings.
namespace expiry {
constexpr std::chrono::minutes kNexrad{75};
constexpr std::chrono::minutes kOtherRaster{105};  // 70, 71, 84, 90, 91 (lightning 103: kNexrad)
constexpr std::chrono::minutes kMetar{120};
constexpr std::chrono::minutes kTafFallback{120};
constexpr std::chrono::minutes kPirep{120};
constexpr std::chrono::hours kWindsFallback{12};
constexpr std::chrono::minutes kTwgo{70};
constexpr std::chrono::minutes kTwgoUnmatched{720};
constexpr std::chrono::minutes kCancelled{30};
}  // namespace expiry
constexpr std::chrono::seconds kMosaicQuiet{10};
constexpr std::chrono::seconds kMosaicMaxInterval{30};
constexpr std::chrono::minutes kMosaicMergeWindow{10};
constexpr std::size_t kMosaicMaxBlocks = 4096;
constexpr std::chrono::minutes kStationForget{60};  // a station unheard this long is dropped

// One TWGO report as the store holds it (both halves).
struct TwgoRecord {
    TwgoKey key;
    std::optional<std::string> text;  // the last non-empty text
    std::vector<Shape> shapes;
    bool has_graphics = false;
    bool complete = false;   // text present (product 14: graphics present)
    bool cancelled = false;
    std::optional<TimePoint> start, graphics_end;  // graphics applicability
    std::optional<TimePoint> text_end;  // NOTAM text "yymmddhhmm-yymmddhhmm" end of validity (not PERM)
    std::string location, label;
    TimePoint received{}, last_received{}, expires{};
};

// One raster layer: every block held, sorted by (scale, block number).
struct Mosaic {
    uint16_t product = 0;
    std::optional<uint8_t> altitude_code;  // 70/71/90/91
    uint8_t scale = 0;  // scale factor of the last block merged (0 high, 1 medium, 2 low)
    TimePoint product_time{}, updated{}, expires{};  // updated: the last merge (store clock)
    std::optional<TimePoint> published;              // the last publish (store clock)
    std::vector<RadarBlock> blocks;
    uint64_t evicted = 0;  // blocks evicted over the 4096 cap, ever
    std::string key() const;  // "63", "70-3" (product-altitude code)
};

struct FisbEvent {
    enum class Kind { text, twgo, radar, raster, expired };
    Kind kind = Kind::text;
    uint16_t product = 0;
    std::string key;  // the item's store key (textKey / TwgoKey::str() / Mosaic::key())
    std::optional<TextRecord> text;   // text
    std::optional<TwgoRecord> twgo;   // twgo (and expired with reason "cancelled")
    std::optional<Mosaic> mosaic;     // radar (63, 64) / raster: the published mosaic
    std::string reason;               // expired: "expired" | "cancelled"
    TimePoint expires{};              // the item's expiry (expired "expired": when it was due)
};

struct StationHeard {
    uint64_t key = 0;
    double lat = 0, lon = 0;
    TimePoint last_heard{};
    uint64_t products = 0;
};

struct FisbStoreStats {
    uint64_t applied = 0;           // products applied
    uint64_t stale = 0;             // already expired on arrival
    uint64_t blocks_merged = 0;     // blocks merged into a mosaic
    uint64_t blocks_too_old = 0;    // more than 10 min older than the mosaic: not merged
    uint64_t blocks_superseded = 0; // older than the held block: not merged
    uint64_t blocks_evicted = 0;    // over the 4096 cap
    uint64_t blocks_pruned = 0;     // dropped when the mosaic time advanced
    uint64_t mosaics_published = 0;
    uint64_t expired = 0;           // items removed by expiry
    uint64_t cancelled = 0;         // TWGO cancels applied
    uint64_t ignored_after_cancel = 0;  // TWGO halves for a key in its 30 min cancelled window
    uint64_t no_product_time = 0;   // raster products whose APDU time did not resolve: dropped
    uint64_t twgo_pending = 0;      // gauge (computed by stats()): TWGO halves held, not yet complete
};

struct FisbSnapshot {
    std::vector<Mosaic> radar;    // 63, 64 (published or not: the current blocks)
    std::vector<Mosaic> rasters;  // 70, 71, 84, 90, 91, 103
    std::vector<TextRecord> metar, taf, pirep, winds, other;  // each sorted by station
    std::vector<TwgoRecord> twgo;  // complete (and cancelled) records, by key
    std::vector<StationHeard> stations;
    FisbStoreStats stats;
};

// What Engine::snapshot() carries: counts only.
struct FisbSummary {
    bool enabled = false;
    std::size_t text_records = 0, twgo_records = 0, mosaics = 0, mosaic_blocks = 0, stations = 0;
    std::optional<double> radar_age_s;  // now - the newest NEXRAD (63/64) product time
};

// The text store key: "metar:KSFO", "taf:KSFO", "winds:SFO:6",
// "pirep:<station>:<issued epoch s>:<fnv1a hex>", "other:...".
std::string textKey(const TextRecord& r);
// When a text record expires (§10.3); TAF: the end of its first ddhh/ddhh
// validity group resolved against issued, else issued + 120 min.
TimePoint textExpiry(const TextRecord& r);

class FisbStore {
public:
    std::vector<FisbEvent> apply(const FisbProduct& product, TimePoint now);
    std::vector<FisbEvent> sweep(TimePoint now);
    FisbSnapshot snapshot() const;
    FisbSummary summary(TimePoint now) const;
    FisbStoreStats stats() const;
    std::size_t mosaicBlocks() const;  // blocks held across every mosaic

private:
    struct HeldBlock {
        RadarBlock block;
        TimePoint time;
        uint64_t seq;
    };
    struct MosaicState {
        Mosaic head;  // blocks left empty; filled by view()
        std::map<uint32_t, HeldBlock> blocks;  // (scale << 24) | block number
        bool dirty = false;
        TimePoint dirty_since{}, last_block{};
    };
    struct TwgoState {
        TwgoRecord rec;
        bool published = false;
    };

    void applyText(const FisbProduct& p, const std::vector<TextRecord>& records, TimePoint now,
                   std::vector<FisbEvent>& out);
    void applyTwgo(const FisbProduct& p, const TwgoParsed& t, TimePoint now, std::vector<FisbEvent>& out);
    void applyBlocks(const FisbProduct& p, const DecodedBlocks& b, TimePoint now, std::vector<FisbEvent>& out);
    bool merge(MosaicState& m, const RadarBlock& block, uint8_t scale, TimePoint time);  // false: superseded
    void publish(MosaicState& m, TimePoint now, std::vector<FisbEvent>& out);
    Mosaic view(const MosaicState& m) const;

    std::map<std::string, MosaicState> mosaics_;
    std::map<std::string, TextRecord> text_;
    std::map<TwgoKey, TwgoState> twgo_;
    std::map<uint64_t, StationHeard> stations_;
    FisbStoreStats stats_;
    uint64_t seq_ = 0;
};

}  // namespace fisb
}  // namespace skycued
