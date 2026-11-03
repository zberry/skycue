#include "inputs/source_input.hpp"

#include "log.hpp"

#include <utility>

namespace skycued {

namespace {

SourceInput::SourceFactory once(std::unique_ptr<ObservationSource> source)
{
    auto held = std::make_shared<std::unique_ptr<ObservationSource>>(std::move(source));
    return [held]() { return std::move(*held); };
}

}  // namespace

SourceInput::SourceInput(InputDef def, std::unique_ptr<ObservationSource> source)
    : SourceInput(std::move(def), once(std::move(source)))
{
}

SourceInput::SourceInput(InputDef def, SourceFactory make) : Input(std::move(def)), make_(std::move(make)) {}

SourceInput::~SourceInput()
{
    if (source_ && running_)
        source_->stop();
}

InputState SourceInput::state() const
{
    InputState s = Input::state();
    s.counters.rejected = rejected_before_ + (source_ ? source_->rejected() : 0);
    s.counters.messages = s.counters.observations + s.counters.rejected;
    return s;
}

void SourceInput::start()
{
    if (running_)
        return;
    auto next = make_();
    if (!next) {
        LOG_WARN("input " << def().id << ": cannot restart (its source runs once)");
        return;
    }
    if (source_)
        rejected_before_ += source_->rejected();
    source_ = std::move(next);
    running_ = true;
    source_->onObservation([this](const TrackObservation& obs) {
        noteData(0);
        emitObservation(obs);
    });
    source_->onUplink([this](std::chrono::system_clock::time_point t, const std::string& hex) {
        noteData(0);
        ++counters_.uplinks;
        emitUplink(t, hex);
    });
    source_->onConnection([this](bool up) {
        if (up)
            ++counters_.connects;
        setConnected(up);
    });
    source_->start();
}

void SourceInput::stop()
{
    if (!running_)
        return;
    running_ = false;
    source_->stop();
    markStopped();
}

}  // namespace skycued
