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
// errors are noted in last_error. A status_url is polled over HTTP by
// TransportInput (spec §5.3).
//
// `capture: true` with a record_dir (spec §5.4) writes every UDP datagram, or
// every block a TCP framer cut (the runs it dropped are not captured), to a
// CaptureWriter, flushed on tick() and stop().

#include "asterix/cat021.hpp"
#include "asterix/cat021_framer.hpp"
#include "inputs/capture_writer.hpp"
#include "inputs/input.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace skycued {

class PingCat021Input : public TransportInput {
public:
    PingCat021Input(boost::asio::io_context& io, InputDef def, const RecordOptions& record = {});
    ~PingCat021Input() override;

    void stop() override;
    void tick(std::chrono::system_clock::time_point now) override;

    // Framers held, one per open TCP stream (for tests).
    std::size_t framerCount() const { return framers_.size(); }

protected:
    void handleData(const TransportData& data) override;
    void handleStreamClosed(uint64_t stream) override;
    void resetStreams() override;

private:
    void handleBlock(const uint8_t* block, std::size_t size);
    void capture(const uint8_t* data, std::size_t size);

    asterix::Edition edition_;
    std::map<uint64_t, asterix::Cat021StreamFramer> framers_;  // per TCP stream
    std::unique_ptr<CaptureWriter> capture_;  // null unless capture and a record_dir
};

}  // namespace skycued
