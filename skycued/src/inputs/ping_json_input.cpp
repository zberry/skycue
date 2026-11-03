#include "inputs/ping_json_input.hpp"

#include "inputs/ping_json.hpp"
#include "log.hpp"

#include <string>
#include <utility>

namespace skycued {

PingJsonInput::PingJsonInput(boost::asio::io_context& io, InputDef def) : TransportInput(io, std::move(def)) {}

void PingJsonInput::handleData(const TransportData& data)
{
    ++counters_.messages;
    const std::string text(reinterpret_cast<const char*>(data.data), data.size);
    PingResult r = parsePingDatagram(text, PingContext{std::chrono::system_clock::now(), def().time_mode});
    counters_.rejected += r.rejected;
    counters_.clock_rejects += r.clock_rejects;
    counters_.raw_unknown_len += r.raw_unknown_len;
    noteClockOffset(r.clock_offset_s);
    if (r.rejected)
        noteError("datagram from " + data.peer.to_string() + " not understood (" + std::to_string(r.rejected) +
                  " rejected)");
    if (r.bad_squawk && !bad_squawk_logged_) {
        bad_squawk_logged_ = true;
        LOG_WARN("input " << def().id << ": a squawk is not a 4-digit octal code (dropped; logged once)");
    }
    if (r.status && running())
        noteStatus(std::move(*r.status));
    for (auto& obs : r.observations) {
        if (!running())
            return;
        emitObservation(std::move(obs));
    }
}

}  // namespace skycued
