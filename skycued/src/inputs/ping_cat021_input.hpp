#pragma once

// PingCat021Input: ASTERIX CAT021 from a pingStation 3 (spec §5.2) over the
// transport its definition names. A UDP datagram is cut with splitBlocks; a
// TCP stream has its own Cat021StreamFramer, dropped when the stream closes.
// Each CAT 21 block is decoded once, here, and each record mapped with
// mapCat021Record.
//
// Counters (spec §4.4): messages = ASTERIX records decoded. rejected =
// unparseable or unmapped data: a framer's dropped runs, a truncated
// datagram, a block of another category. records_skipped = SIM/TST/RAB
// records, records without 080, and blocks cut short by a length error (an
// unknown item length loses the rest of the block). Rejections and block
// errors are noted in last_error.

#include "asterix/cat021.hpp"
#include "asterix/cat021_framer.hpp"
#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <map>

namespace skycued {

class PingCat021Input : public TransportInput {
public:
    PingCat021Input(boost::asio::io_context& io, InputDef def);
    ~PingCat021Input() override;

    // Framers held, one per open TCP stream (for tests).
    std::size_t framerCount() const { return framers_.size(); }

protected:
    void handleData(const TransportData& data) override;
    void handleStreamClosed(uint64_t stream) override;
    void resetStreams() override;

private:
    void handleBlock(const uint8_t* block, std::size_t size);

    asterix::Edition edition_;
    std::map<uint64_t, asterix::Cat021StreamFramer> framers_;  // per TCP stream
};

}  // namespace skycued
