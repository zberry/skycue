#include "asterix/cat021_framer.hpp"

#include <algorithm>

namespace skycued {
namespace asterix {

Cat021StreamFramer::Cat021StreamFramer(Edition edition, bool accept_other_categories)
    : edition_(edition), accept_other_categories_(accept_other_categories) {}

void Cat021StreamFramer::reset() {
  buf_.clear();
  skip_ = 0;
  in_sync_ = true;
  walked_ = 0;
}

void Cat021StreamFramer::dropByte(std::size_t& pos) {
  if (in_sync_) {
    in_sync_ = false;
    ++dropped_runs_;
  }
  ++dropped_bytes_;
  ++pos;
  walked_ = 0;
}

void Cat021StreamFramer::feed(const uint8_t* data, std::size_t size,
                              std::vector<std::vector<uint8_t>>& blocks_out) {
  if (skip_ > 0) {
    const std::size_t n = std::min(skip_, size);
    skip_ -= n;
    skipped_bytes_ += n;
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
      // The cached walk belongs to the candidate at buf_[0] (where the last
      // feed() stopped to wait).
      const std::size_t start = pos == 0 ? walked_ : 0;
      const std::size_t body = len - 3;
      if (avail >= len) {
        const bool clean =
            (start > 0 && start == body) ||
            detail::walkRecords(p + 3 + start, body - start, true, edition_) ==
                detail::WalkStatus::ok;
        if (clean) {
          blocks_out.emplace_back(p, p + len);
          pos += len;
          in_sync_ = true;
          walked_ = 0;
        } else {
          dropByte(pos);
        }
        continue;
      }
      std::size_t walked = 0;
      if (detail::walkRecords(p + 3 + start, avail - 3 - start, false, edition_, nullptr, nullptr,
                              &walked) == detail::WalkStatus::need_more) {
        walked_ = start + walked;
        break;  // a plausible candidate: wait for the rest
      }
      dropByte(pos);
      continue;
    }

    if (accept_other_categories_ && in_sync_ && cat != 0 && len >= 3 &&
        len <= kMaxOtherCategoryLen) {
      ++other_category_blocks_;
      const std::size_t take = std::min(len, avail);
      pos += take;
      skipped_bytes_ += take;
      skip_ = len - take;
      walked_ = 0;
      continue;
    }

    dropByte(pos);
  }
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

}  // namespace asterix
}  // namespace skycued
