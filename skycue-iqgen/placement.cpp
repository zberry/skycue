#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iqgen {

Placement place(std::vector<std::vector<TimedFrame>> files, const PlaceOptions& opt)
{
    Placement p{{}, 0, 0};
    bool first_file = true;
    int64_t last_start = 0;
    for (auto& file : files) {
        if (file.empty())
            continue;
        const int64_t origin = first_file ? 0 : last_start + opt.spacing;
        first_file = false;
        const bool timed =
            std::all_of(file.begin(), file.end(), [](const TimedFrame& f) { return f.time.has_value(); });
        if (timed) {
            std::stable_sort(file.begin(), file.end(),
                             [](const TimedFrame& a, const TimedFrame& b) { return *a.time < *b.time; });
            const int64_t t0 = *file.front().time;
            for (auto& f : file) {
                const auto offset =
                    static_cast<int64_t>(std::llround(static_cast<double>(*f.time - t0) * opt.grid_per_time));
                p.frames.push_back({origin + offset, std::move(f.payload), false});
            }
        } else {
            int64_t start = origin;
            for (auto& f : file) {
                p.frames.push_back({start, std::move(f.payload), false});
                start += opt.spacing;
            }
        }
        last_start = p.frames.back().start;
    }
    // Already in start order (each file's frames come after the previous
    // file's); the stable sort only guards that.
    std::stable_sort(p.frames.begin(), p.frames.end(),
                     [](const Placed& a, const Placed& b) { return a.start < b.start; });

    if (opt.duration) {
        const auto cut = std::find_if(p.frames.begin(), p.frames.end(),
                                      [&](const Placed& f) { return f.start >= *opt.duration; });
        p.frames.erase(cut, p.frames.end());
    }
    if (opt.limit && p.frames.size() > *opt.limit)
        p.frames.resize(*opt.limit);

    std::vector<int64_t> length(p.frames.size());
    int64_t last_end = 0;
    for (std::size_t i = 0; i < p.frames.size(); ++i) {
        p.frames[i].start += opt.pad;
        length[i] = opt.length(p.frames[i].payload);
    }
    for (std::size_t i = 0; i < p.frames.size(); ++i) {
        const int64_t end_i = p.frames[i].start + length[i];
        last_end = std::max(last_end, end_i);
        for (std::size_t j = i + 1; j < p.frames.size() && p.frames[j].start < end_i; ++j) {
            if (length[j] <= 0)
                continue;
            ++p.overlaps;
            p.frames[i].overlap = p.frames[j].overlap = true;
        }
    }
    if (!p.frames.empty())
        p.end = last_end + opt.pad;
    return p;
}

}  // namespace iqgen
