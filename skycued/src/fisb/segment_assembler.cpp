#include "fisb/segment_assembler.hpp"

namespace skycued {
namespace fisb {

std::optional<SegmentAssembler::Done> SegmentAssembler::add(const ApduHeader& h, const uint8_t* p, std::size_t n,
                                                            TimePoint now)
{
    if (!p)
        n = 0;
    if (!h.segmented)
        return Done{h, std::vector<uint8_t>(p, p + n)};

    const bool twgo = productFamily(h.product_id) == ProductFamily::twgo;
    if (h.segment_count == 0 || h.segment_number == 0 || h.segment_number > h.segment_count ||
        (twgo && h.segment_number > 1 && n < kTwgoHeaderBytes)) {
        ++rejected_;
        return std::nullopt;
    }

    const Key key{h.product_id, h.apdu_number};
    auto it = sets_.find(key);
    if (it != sets_.end() && it->second.slots.size() != h.segment_count) {
        sets_.erase(it);  // the product file changed shape: start again
        it = sets_.end();
    }
    if (it == sets_.end()) {
        if (sets_.size() >= max_open_)
            evictOldest();
        Set s;
        s.first = now;
        s.seq = seq_++;
        s.slots.resize(h.segment_count);
        it = sets_.emplace(key, std::move(s)).first;
    }

    Set& s = it->second;
    auto& slot = s.slots[h.segment_number - 1u];
    if (slot) {
        ++duplicates_;
        return std::nullopt;
    }
    slot.emplace(p, p + n);
    if (h.segment_number == 1)
        s.first_hdr = h;
    if (++s.filled < s.slots.size())
        return std::nullopt;

    Done d;
    d.hdr = s.first_hdr;
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        const auto& seg = *s.slots[i];
        const std::size_t skip = (twgo && i > 0) ? kTwgoHeaderBytes : 0;
        d.payload.insert(d.payload.end(), seg.begin() + static_cast<std::ptrdiff_t>(skip), seg.end());
    }
    sets_.erase(it);
    return d;
}

std::size_t SegmentAssembler::sweep(TimePoint now, std::chrono::minutes timeout)
{
    std::size_t dropped = 0;
    for (auto it = sets_.begin(); it != sets_.end();) {
        if (now - it->second.first > timeout) {
            it = sets_.erase(it);
            ++dropped;
        } else
            ++it;
    }
    timed_out_ += dropped;
    return dropped;
}

void SegmentAssembler::evictOldest()
{
    auto oldest = sets_.end();
    for (auto it = sets_.begin(); it != sets_.end(); ++it)
        if (oldest == sets_.end() || it->second.seq < oldest->second.seq)
            oldest = it;
    if (oldest != sets_.end()) {
        sets_.erase(oldest);
        ++evicted_;
    }
}

}  // namespace fisb
}  // namespace skycued
