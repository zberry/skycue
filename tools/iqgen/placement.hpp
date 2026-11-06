#pragma once

// Where each recorded frame goes in the output. All positions are in "grid
// units": 12 MHz ticks for Mode S, output samples for UAT.

#include "inputs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace iqgen {

struct PlaceOptions {
    int64_t pad;                      // before the first frame and after the last
    int64_t spacing;                  // between untimed frames, and from one file to the next
    double grid_per_time;             // grid units per unit of TimedFrame::time
    std::optional<std::size_t> limit; // keep the first N placed frames
    std::optional<int64_t> duration;  // drop frames with start - pad >= duration
    std::function<int64_t(const std::vector<uint8_t>&)> length;  // a frame's length in grid units
};

struct Placed {
    int64_t start;
    std::vector<uint8_t> payload;
    bool overlap = false;
};

struct Placement {
    std::vector<Placed> frames;  // in start order
    int64_t end;                 // last frame end + pad; 0 without frames
    uint64_t overlaps;           // intersecting pairs
};

// files: one vector per input file, in command-line order. A file whose
// frames are all timed is stable-sorted by time and placed at
// round((t - t_first) * grid_per_time); a file with any untimed frame is
// placed in file order, `spacing` apart. File k+1 starts `spacing` after
// file k's last frame start. Everything is shifted by `pad`. Two frames
// overlap when [start, start + length) intersect: each such pair adds 1 to
// `overlaps` and marks both.
Placement place(std::vector<std::vector<TimedFrame>> files, const PlaceOptions& opt);

}  // namespace iqgen
