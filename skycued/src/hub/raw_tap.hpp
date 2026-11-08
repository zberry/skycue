#pragma once

// The one place inputs publish raw messages to. Nothing is copied or
// stamped unless a sink is set and wants the kind, so an input costs nothing
// while no output of that kind is enabled. One io thread: no locking.

#include "hub/raw_message.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace skycued {

class RawTap {
public:
    using Sink = std::function<void(const RawMessage&)>;
    using Wanted = std::function<bool(RawKind)>;

    // An empty `wanted` wants every kind.
    void setSink(Sink sink, Wanted wanted);
    void clear();
    bool wanted(RawKind kind) const;  // false with no sink
    // Builds the Buf and stamps `arrived` only when wanted.
    void publish(RawKind kind, const uint8_t* data, std::size_t size, std::string_view input);

private:
    Sink sink_;
    Wanted wanted_;
};

}  // namespace skycued
