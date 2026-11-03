#pragma once

// Cuts a TCP byte stream into CAT021 data blocks (spec §5.2). It only frames:
// a block is checked with the shared length walk (detail::walkRecords), never
// decoded; the consumer decodes each block it is handed, once.
//
// In sync (at the start, after reset() and after every block passed out):
//  - CAT 21 with LEN >= 3: pass it out once it is complete and walks cleanly.
//    While it is still arriving, it is waited for only as long as the bytes
//    already buffered walk without error. On a walk error, drop one byte and
//    leave sync.
//  - another CAT (1..255) with 3 <= LEN <= kMaxOtherCategoryLen: skip LEN bytes
//    and count it.
//  - anything else (CAT 0, LEN < 3, another CAT with a larger LEN): drop one
//    byte and leave sync.
// Out of sync: drop one byte at a time until a CAT 21 block with LEN >= 3 walks
// cleanly, with the same rule for a candidate still arriving. Each out-of-sync
// period counts one dropped run. The walk of a waiting candidate resumes where
// the previous feed() left it.

#include "asterix/cat021.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace skycued {
namespace asterix {

class Cat021StreamFramer {
 public:
  // Real blocks from the pingStation 3 are under 1 KiB; a larger "other
  // category" header is more likely garbage than a block worth skipping.
  static constexpr std::size_t kMaxOtherCategoryLen = 4096;

  explicit Cat021StreamFramer(Edition edition);

  // Appends every block completed by these bytes to blocks_out.
  void feed(const uint8_t* data, std::size_t size, std::vector<std::vector<uint8_t>>& blocks_out);
  // Forgets buffered bytes (a new connection); the counters are kept.
  void reset();

  uint64_t droppedRuns() const { return dropped_runs_; }
  uint64_t droppedBytes() const { return dropped_bytes_; }
  uint64_t otherCategoryBlocks() const { return other_category_blocks_; }
  // Bytes skipped as other-category blocks (headers included).
  uint64_t skippedBytes() const { return skipped_bytes_; }
  bool inSync() const { return in_sync_; }

 private:
  void dropByte(std::size_t& pos);

  Edition edition_;
  std::vector<uint8_t> buf_;
  std::size_t skip_ = 0;  // bytes of an other-category block still to skip
  bool in_sync_ = true;
  // Body bytes of the candidate at buf_[0] already walked cleanly.
  std::size_t walked_ = 0;
  uint64_t dropped_runs_ = 0;
  uint64_t dropped_bytes_ = 0;
  uint64_t other_category_blocks_ = 0;
  uint64_t skipped_bytes_ = 0;
};

}  // namespace asterix
}  // namespace skycued
