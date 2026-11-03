#include "asterix/cat021_framer.hpp"

#include <algorithm>

namespace skycued {
namespace asterix {

Cat021StreamFramer::Cat021StreamFramer(Edition edition) : edition_(edition) {}

void Cat021StreamFramer::reset() {
  buf_.clear();
  skip_ = 0;
  in_sync_ = true;
}

void Cat021StreamFramer::dropByte(std::size_t& pos) {
  if (in_sync_) {
    in_sync_ = false;
    ++dropped_runs_;
  }
  ++dropped_bytes_;
  ++pos;
}

void Cat021StreamFramer::feed(const uint8_t* data, std::size_t size,
                              std::vector<std::vector<uint8_t>>& blocks_out) {
  if (skip_ > 0) {
    const std::size_t n = std::min(skip_, size);
    skip_ -= n;
    data += n;
    size -= n;
  }
  buf_.insert(buf_.end(), data, data + size);

  std::size_t pos = 0;
  while (buf_.size() - pos >= 3) {
    const uint8_t* p = buf_.data() + pos;
    const std::size_t avail = buf_.size() - pos;
    const uint8_t cat = p[0];
    const std::size_t len = (std::size_t{p[1]} << 8) | p[2];

    if (cat == 21 && len >= 3) {
      if (avail >= len) {
        if (detail::walkRecords(p + 3, len - 3, true, edition_) == detail::WalkStatus::ok) {
          blocks_out.emplace_back(p, p + len);
          pos += len;
          in_sync_ = true;
        } else {
          dropByte(pos);
        }
        continue;
      }
      if (in_sync_) break;  // wait for the rest of the block
      if (detail::walkRecords(p + 3, avail - 3, false, edition_) == detail::WalkStatus::need_more) {
        break;  // a plausible candidate: wait
      }
      dropByte(pos);
      continue;
    }

    if (in_sync_ && cat != 0 && len >= 3) {
      ++other_category_blocks_;
      const std::size_t take = std::min(len, avail);
      pos += take;
      skip_ = len - take;
      continue;
    }

    dropByte(pos);
  }
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

}  // namespace asterix
}  // namespace skycued
