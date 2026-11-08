#pragma once

// The hub: raw messages from the inputs go to the outputs the operator
// defined. Per message kind there are two skycue::net::Fanout buses, one for
// stream destinations (TCP listeners and dialers) and one for datagram
// destinations (UDP senders), so a text kind is framed with its newline once
// for all streams and sent as is to all datagram destinations. The hub only
// reconciles output definitions (the InputManager owns the file); it never
// reads files. One io thread: no locking.

#include "hub/raw_message.hpp"
#include "outputs/output_def.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

class Forwarder;

struct OutputCounters {
    uint64_t peers = 0, bytes = 0, messages = 0, dropped = 0;
    std::string last_error;
};

// up: listening / connected / the sender has a usable target.
struct OutputState {
    OutputDef def;
    bool up = false;
    OutputCounters counters;
};

class Hub {
public:
    explicit Hub(boost::asio::io_context& io, Forwarder* cue = nullptr);
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // The RawTap sink. Never calls back into the tap.
    void publish(const RawMessage& message);
    // An enabled output of that kind exists (the RawTap `Wanted`).
    bool wants(RawKind kind) const;
    // Reconciles the running outputs with `desired` by id: new enabled ones
    // start, missing ones stop, a changed one is stopped and started again
    // (its counters are kept), a disabled one stops. A start that fails
    // (bad group, no route) does not throw: the output is listed with
    // up == false and the library's error. `builtin_enabled` overrides
    // `enabled` of the definitions with `builtin` set.
    void apply(const std::vector<OutputDef>& desired, const std::map<std::string, bool>& builtin_enabled = {});
    // Built-in outputs first, then in apply order.
    std::vector<OutputState> list() const;
    std::optional<OutputState> find(const std::string& id) const;
    // Called when an output started, stopped, or its up flag or error changed
    // (not per byte).
    void onChanged(std::function<void()> callback);
    // Stops every output; idempotent. publish() and apply() do nothing after.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace skycued
