#pragma once

// FIS-B global block representation (docs/fisb-reference.md §5): products
// 63 (NEXRAD regional), 64 (NEXRAD CONUS), 70/71 (icing), 84 (cloud tops),
// 90/91 (turbulence) and 103 (lightning). One APDU carries one block: either
// a run-length block of 128 bins or an empty-block list.

#include "fisb/apdu.hpp"
#include "fisb/products.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skycued {
namespace fisb {

// A block's NW corner and bin size, in degrees (see RadarBlock).
struct BlockGeom {
    double lat0, lon0, dlat, dlon;
};

// Block number + scale factor (0 high, 1 medium, 2 low; 3 reserved) to
// bounds, northern hemisphere. nullopt for scale 3, a medium/low block
// number off its lattice (§5.2), a block beyond 90°N, and a block at or
// above 60°N (block >= 405000) unless experimental_high_lat (§10.4 [U]).
std::optional<BlockGeom> blockGeometry(uint32_t block, uint8_t scale, bool experimental_high_lat);

struct DecodedBlocks {
    uint16_t product_id = 0;
    uint8_t scale = 0;
    std::vector<RadarBlock> blocks;  // run-length blocks (E=1)
    // The empty-block list (E=0), header block first, in bitmap order. An
    // empty block carries no bins: the store decides what it shows (spec
    // §10.6: level 1 for CONUS, reference §5.4); blockGeometry() gives its
    // bounds.
    std::vector<uint32_t> empty_blocks;
    bool conus = false;  // product 64
    // Products 70/71/90/91 only: payload bits 6-4 are an altitude code, not
    // NS/scale (§5.1). FD maps it to n*2000+2000 ft (low) / n*2000+18000 ft
    // (high); the high-level range is [U], so the raw code is passed on.
    std::optional<uint8_t> altitude_code;
};

// What parseGlobalBlockProduct saw, accumulated across calls.
struct GlobalBlockStats {
    uint64_t rle_blocks = 0;        // run-length blocks decoded
    uint64_t empty_lists = 0;       // empty-block lists decoded
    uint64_t empty_lists_l0 = 0;    // ... of which had L = 0 (the case MUT/STX drop, §5.4)
    uint64_t empty_blocks = 0;      // empty blocks listed
    uint64_t rejected = 0;          // blocks that failed to decode (FisbCounters::blocks_rejected)
    uint64_t high_lat_skipped = 0;  // blocks/lists at or above 60°N, experimental off
};

// Decode one global-block APDU payload (p, n: after the APDU header). A
// run-length block whose runs do not total exactly 128 bins, or that leaves
// bytes unused, is rejected, as is an empty-block list whose length is not
// 4 + L, a southern-hemisphere block (NS=1, untested), a reserved scale or a
// block with no geometry. Returns nullopt when nothing valid is left (and
// for products outside the global-block family). Never reads past p + n;
// never throws.
std::optional<DecodedBlocks> parseGlobalBlockProduct(const ApduHeader& hdr, const uint8_t* p, std::size_t n,
                                                     bool experimental, GlobalBlockStats* stats = nullptr);

}  // namespace fisb
}  // namespace skycued
