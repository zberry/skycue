// skycued --check-inputs: connect once to each configured TCP input and say
// whether it answered.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace skycued {

struct InputCheck {
    std::string id, host;
    uint16_t port = 0;
};

struct InputCheckResult {
    InputCheck input;
    bool connected = false;
    std::string error;  // when !connected: why ("Connection refused", "timed out", ...)
};

// Dials every input at once on the network library (one io_context, one
// Dialer each, stopped after its first result), each bounded by `timeout`;
// results in input order. The call returns within about timeout + 0.5 s. A
// name lookup cannot be cancelled: one still running at the deadline is
// reported as timed out and abandoned (its resources are left to the process,
// so call this a bounded number of times), but it keeps running until libc's
// resolver gives up.
std::vector<InputCheckResult> checkInputs(const std::vector<InputCheck>& inputs,
                                          std::chrono::milliseconds timeout);

// "<id> <host>:<port>: connected" or "<id> <host>:<port>: <error>"
std::string inputCheckLine(const InputCheckResult& r);

}  // namespace skycued
