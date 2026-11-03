#pragma once

// StatusPoller: GET a pingStation status URL over plain HTTP/1.1 (spec §5.3).
//
// One request at a time: the first right after start(), then one every
// `interval` (counted from the start of the previous request; a request
// that runs late delays the next, never overlaps it, and the next waits at
// least min(interval, 1 s) after it finished). Each request sends
// `Host` and `Connection: close` and reads the response with a body limit of
// kMaxStatusBytes. Only a 200 is a success: onResult gets the body and an
// empty error. A 3xx (not followed), 4xx or 5xx, a timeout, a resolve or
// connect error and a body over kMaxStatusBytes give onResult a null body
// and the reason; a non-200 is decided by its status line, its body is not
// read.
//
// The timeout covers the whole exchange except DNS: resolver.cancel() does
// not interrupt a getaddrinfo already running, so with a hostname that
// resolves slowly the timeout result arrives only when the lookup returns.
// An IP literal has no such delay.
//
// Threading: lives on the io_context; call everything from its thread.
// Register onResult before start(). stop() is idempotent and cancels a
// request in flight: no callback runs after stop() or destruction. start()
// may be called again after stop().

#include "inputs/input_def.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace skycued {

constexpr std::size_t kMaxStatusBytes = 64 * 1024;

// The Host header for `url`: an IPv6 literal in brackets, ":port" unless 80.
std::string httpHostHeader(const HttpUrl& url);

class StatusPoller {
public:
    using ResultHandler = std::function<void(const std::string* body, const std::string& error)>;

    StatusPoller(boost::asio::io_context& io, HttpUrl url, std::chrono::seconds interval,
                 std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~StatusPoller();
    StatusPoller(const StatusPoller&) = delete;
    StatusPoller& operator=(const StatusPoller&) = delete;

    void onResult(ResultHandler handler);
    void start();
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
