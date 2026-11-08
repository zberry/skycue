#include "inputs/readsb_stream_input.hpp"

#include "log.hpp"
#include "readsb_json_source.hpp"

#include <string>
#include <utility>

namespace skycued {

ReadsbStreamInput::ReadsbStreamInput(boost::asio::io_context& io, InputDef def) : TransportInput(io, std::move(def)) {}

InputState ReadsbStreamInput::state() const
{
    InputState s = TransportInput::state();
    s.counters.rejected += lineRejects();
    s.counters.messages = s.counters.observations + s.counters.rejected;
    return s;
}

void ReadsbStreamInput::handleData(const TransportData& data)
{
    feedLines(data, [this](const uint8_t* line, std::size_t size) {
        if (running())
            handleLine(line, size);
    });
}

void ReadsbStreamInput::handleLine(const uint8_t* line, std::size_t size)
{
    publishRaw(RawKind::json, line, size);
    const std::string text(reinterpret_cast<const char*>(line), size);
    auto obs = parseReadsbLine(text);
    if (!obs) {
        ++counters_.rejected;
        LOG_DEBUG("input " << def().id << ": skipping a line that does not parse: " << text.substr(0, 200));
        return;
    }
    emitObservation(std::move(*obs));
}

}  // namespace skycued
