#include "hub/raw_tap.hpp"

#include <utility>

namespace skycued {

void RawTap::setSink(Sink sink, Wanted wanted)
{
    sink_ = std::move(sink);
    wanted_ = std::move(wanted);
}

void RawTap::clear()
{
    sink_ = nullptr;
    wanted_ = nullptr;
}

bool RawTap::wanted(RawKind kind) const
{
    if (!sink_)
        return false;
    return !wanted_ || wanted_(kind);
}

void RawTap::publish(RawKind kind, const uint8_t* data, std::size_t size, std::string_view input)
{
    if (!wanted(kind))
        return;
    RawMessage message;
    message.kind = kind;
    message.bytes = skycue::net::makeBuf(data, size);
    message.arrived = std::chrono::system_clock::now();
    message.input = input;
    sink_(message);
}

}  // namespace skycued
