#include "inputs/uat_raw_input.hpp"

#include "log.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace skycued {

UatRawInput::UatRawInput(boost::asio::io_context& io, InputDef def) : TransportInput(io, std::move(def)) {}

InputState UatRawInput::state() const
{
    InputState s = TransportInput::state();
    s.counters.uplinks = uplink_;
    s.counters.messages = downlink_ + uplink_;
    s.counters.rejected += lineRejects();
    return s;
}

void UatRawInput::handleData(const TransportData& data)
{
    feedLines(data, [this](const uint8_t* line, std::size_t size) {
        if (running())
            handleLine(line, size);
    });
}

void UatRawInput::handleConnection(bool up)
{
    if (def().transport != InputTransport::tcp_connect)
        return;
    if (up) {
        LOG_INFO(def().id << ": connected to " << def().host << ":" << def().port);
    } else {
        // As LineClient wrote it: the library's reason without its "read: " prefix.
        std::string why = lastCloseReason();
        if (why.compare(0, 6, "read: ") == 0)
            why.erase(0, 6);
        LOG_INFO(def().id << ": connection closed (" << why << ")");
    }
}

bool UatRawInput::reportsTransportErrors() const
{
    return !(def().type == InputType::dump978 && def().transport == InputTransport::tcp_connect);
}

void UatRawInput::handleLine(const uint8_t* line, std::size_t size)
{
    if (size == 0 || (line[0] != '-' && line[0] != '+'))
        return;  // '!' metadata or unknown
    std::size_t end = 1;
    while (end < size && std::isxdigit(line[end]))
        ++end;
    if (end == 1 || (end < size && line[end] != ';'))
        return;
    publishRaw(RawKind::uat, line, size);
    if (line[0] == '-') {
        ++downlink_;
        return;
    }
    ++uplink_;
    emitUplink(std::chrono::system_clock::now(), std::string(reinterpret_cast<const char*>(line) + 1, end - 1));
}

}  // namespace skycued
