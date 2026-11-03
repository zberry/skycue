#pragma once

#include "line_client.hpp"
#include "observation.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace skycued {

// Parses one line from readsb's JSON output port (--net-json-port): an
// aircraft.json aircraft object with a leading "now" (epoch seconds).
// Requires a valid "hex" (exactly 6 hex digits, optionally "~"-prefixed) and a
// numeric "now"; unknown keys, arrays and nested objects are ignored.
// nullopt -> the caller logs and skips the line. Never throws.
std::optional<TrackObservation> parseReadsbLine(const std::string& line);

// Reads readsb's JSON output port over TCP (reconnecting, see LineClient)
// and turns each line into a TrackObservation. Lines that do not parse are
// counted and logged at debug level.
// Threading: lives on the caller's io_context; call everything from its thread.
class ReadsbJsonSource : public ObservationSource {
public:
    ReadsbJsonSource(boost::asio::io_context& io, std::string host, uint16_t port);

    void onObservation(std::function<void(const TrackObservation&)> handler) override;
    void onConnection(std::function<void(bool)> handler) override;
    void start() override;
    void stop() override;

    uint64_t linesParsed() const { return parsed_; }
    // Lines that did not parse plus oversize lines the LineClient discarded.
    uint64_t linesRejected() const { return bad_ + client_.linesRejected(); }
    uint64_t rejected() const override { return linesRejected(); }

private:
    LineClient client_;
    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void(bool)> on_connection_;
    uint64_t parsed_ = 0, bad_ = 0;
    bool stopped_ = false;
};

}  // namespace skycued
