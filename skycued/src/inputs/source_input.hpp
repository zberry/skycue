#pragma once

// SourceInput: an Input over an ObservationSource (readsb's JSON port through
// ReadsbJsonSource, a replay, tests). messages = observations +
// source->rejected(), bytes = source->bytesRead(). Connects are counted on
// each onConnection(true).

#include "inputs/input.hpp"
#include "observation.hpp"

#include <functional>
#include <memory>

namespace skycued {

class SourceInput : public Input {
public:
    using SourceFactory = std::function<std::unique_ptr<ObservationSource>()>;

    // One source: start() after stop() cannot restart it (logged at WARN).
    SourceInput(InputDef def, std::unique_ptr<ObservationSource> source);
    // A new source from `make` on every start(), so the input can be
    // disabled and enabled again.
    SourceInput(InputDef def, SourceFactory make);
    ~SourceInput() override;

    InputState state() const override;
    void start() override;
    void stop() override;

private:
    SourceFactory make_;
    std::unique_ptr<ObservationSource> source_;  // kept after stop() until the next start()
    bool running_ = false;
    uint64_t rejected_before_ = 0, bytes_before_ = 0;  // of earlier sources
};

}  // namespace skycued
