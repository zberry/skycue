#include "inputs/ping_cat021_input.hpp"

#include "inputs/cat021_map.hpp"

#include <string>
#include <utility>
#include <vector>

namespace skycued {

PingCat021Input::PingCat021Input(boost::asio::io_context& io, InputDef def)
    : TransportInput(io, std::move(def)),
      edition_(asterix::parseEdition(this->def().edition).value_or(asterix::Edition::e2_5))
{
}

PingCat021Input::~PingCat021Input() = default;

void PingCat021Input::resetStreams() { framers_.clear(); }

void PingCat021Input::handleStreamClosed(uint64_t stream) { framers_.erase(stream); }

void PingCat021Input::handleData(const TransportData& data)
{
    if (def().transport == InputTransport::udp) {  // one datagram
        const asterix::BlockSplit split = asterix::splitBlocks(data.data, data.size);
        for (const auto& b : split.blocks) {
            if (!running())
                return;
            if (b.cat != 21) {
                ++counters_.rejected;
                noteError("CAT " + std::to_string(b.cat) + " block from " + data.peer.to_string() + " rejected");
                continue;
            }
            handleBlock(data.data + b.offset, b.size);
        }
        if (split.truncated) {
            ++counters_.rejected;
            noteError("truncated datagram from " + data.peer.to_string());
        }
        return;
    }
    auto it = framers_.find(data.stream);
    if (it == framers_.end())
        it = framers_.emplace(data.stream, asterix::Cat021StreamFramer(edition_)).first;
    auto& framer = it->second;
    const uint64_t runs_before = framer.droppedRuns();
    std::vector<std::vector<uint8_t>> blocks;
    framer.feed(data.data, data.size, blocks);
    if (const uint64_t runs = framer.droppedRuns() - runs_before) {
        counters_.rejected += runs;
        noteError("stream from " + data.peer.to_string() + " resynchronised (" + std::to_string(runs) +
                  " run(s) dropped)");
    }
    for (const auto& block : blocks) {
        if (!running())
            return;
        handleBlock(block.data(), block.size());
    }
}

void PingCat021Input::handleBlock(const uint8_t* block, std::size_t size)
{
    const InputDef& d = def();
    const asterix::DecodedBlock decoded = asterix::decodeBlock(block, size, edition_);
    const Cat021Context ctx{std::chrono::system_clock::now(), d.time_mode, d.sac, d.sic};
    for (const auto& record : decoded.records) {
        if (!running())
            return;
        ++counters_.messages;
        Cat021Counts counts;
        auto obs = mapCat021Record(record, ctx, counts);
        counters_.records_skipped += counts.records_skipped;
        counters_.nogo_records += counts.nogo_records;
        counters_.sac_sic_mismatch += counts.sac_sic_mismatch;
        counters_.clock_rejects += counts.clock_rejects;
        noteClockOffset(counts.clock_offset_s);
        if (obs)
            emitObservation(std::move(*obs));
    }
    if (decoded.error) {
        ++counters_.records_skipped;
        noteError("CAT021 block cut short: " + decoded.error_text);
    }
}

}  // namespace skycued
