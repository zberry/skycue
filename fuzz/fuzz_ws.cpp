// libFuzzer harness: the WebSocket inbound path (web_server.cpp WsSession).
// The server reads and discards whatever a client sends, up to
// kWsMessageLimit (64 KiB) per message; this proves that path stays bounded
// and safe against hostile frames (64-bit lengths, bad opcodes, unmasked
// frames, fragments, close frames).
//
// LLVMFuzzerInitialize starts a real WebServer on 127.0.0.1, on a port the
// OS picks, over the stubbed daemon of api_stub.hpp, its io_context run by a
// background thread. Each input: connect, send a fixed upgrade request, read
// the 101, write the input raw, half-close and drain until the server closes
// (bounded), close. Then a fresh connection's GET /api/stats must answer 200
// within slow(2s): the server neither hung nor crashed. ASan's
// -rss_limit_mb catches an allocation sized from a hostile length. Only the
// ephemeral port the server was given is used.

#include "../tests/test_slow.hpp"
#include "api_stub.hpp"
#include "fuzz_util.hpp"
#include "web_server.hpp"

#include <boost/asio/executor_work_guard.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

uint16_t g_port = 0;
// Live for the whole process (reachable, so LeakSanitizer does not count them).
boost::asio::io_context* g_io = nullptr;
skycuefuzz::ApiStub* g_stub = nullptr;
skycued::WebServer* g_server = nullptr;
boost::asio::executor_work_guard<boost::asio::io_context::executor_type>* g_guard = nullptr;

int connectLoopback(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // libFuzzer's timers interrupt system calls (EINTR): an interrupted
    // connect goes on in the background, so wait for it and read its result.
    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    if (rc != 0 && errno == EINTR) {
        pollfd p{fd, POLLOUT, 0};
        while ((rc = ::poll(&p, 1, 10000)) < 0 && errno == EINTR) {
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (rc == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            rc = 0;
        else
            errno = rc == 1 ? err : ETIMEDOUT;
    }
    if (rc != 0) {
        std::fprintf(stderr, "fuzz_ws: connect to port %u: %s\n", static_cast<unsigned>(port), std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

int msLeft(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// Sends all of s before the deadline; false when the peer closed or time ran out.
bool sendAll(int fd, const std::string& s, Clock::time_point deadline)
{
    std::size_t pos = 0;
    while (pos < s.size()) {
        pollfd p{fd, POLLOUT, 0};
        const int r = ::poll(&p, 1, msLeft(deadline));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        const ssize_t n = ::send(fd, s.data() + pos, s.size() - pos, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        pos += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads into out until `done(out)`, end of stream, an error or the deadline;
// true when it stopped for `done` or end of stream.
template <class Done> bool readUntil(int fd, std::string& out, Clock::time_point deadline, Done done)
{
    char buf[4096];
    while (!done(out)) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, msLeft(deadline));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n == 0)
            return true;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

const char kUpgrade[] =
    "GET /ws HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

const char kStats[] =
    "GET /api/stats HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Connection: close\r\n"
    "\r\n";

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    skycuefuzz::quietLogs();
    g_io = new boost::asio::io_context();
    g_stub = new skycuefuzz::ApiStub(*g_io, skycuefuzz::makeWorkDir("ws"));
    skycued::WebConfig wc;
    wc.bind = "127.0.0.1";
    wc.port = 0;
    wc.www_dir = g_stub->www;
    wc.daemon = &g_stub->daemon;
    g_server = new skycued::WebServer(*g_io, *g_stub->engine, *g_stub->runtime, wc);
    g_server->start();
    g_port = g_server->localPort();
    FUZZ_CHECK(g_port != 0);
    // Runs for the life of the process; libFuzzer exits without joining it.
    g_guard = new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(
        boost::asio::make_work_guard(*g_io));
    std::thread([] { g_io->run(); }).detach();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    const std::string payload(reinterpret_cast<const char*>(data), size);

    // The hostile client.
    {
        const int fd = connectLoopback(g_port);
        FUZZ_CHECK(fd >= 0);
        const auto deadline = Clock::now() + slow(2s);
        FUZZ_CHECK(sendAll(fd, kUpgrade, deadline));
        std::string reply;
        readUntil(fd, reply, deadline, [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; });
        FUZZ_CHECK(reply.rfind("HTTP/1.1 101", 0) == 0);
        (void)sendAll(fd, payload, deadline);  // the server may close part way: that is its right
        ::shutdown(fd, SHUT_WR);
        std::string rest;
        (void)readUntil(fd, rest, Clock::now() + slow(2s), [](const std::string&) { return false; });
        ::close(fd);
    }

    // The server still answers.
    const auto start = Clock::now();
    const auto deadline = start + slow(2s);
    const int fd = connectLoopback(g_port);
    FUZZ_CHECK(fd >= 0);
    FUZZ_CHECK(sendAll(fd, kStats, deadline));
    std::string reply;
    const bool ended = readUntil(fd, reply, deadline, [](const std::string&) { return false; });
    ::close(fd);
    FUZZ_CHECK(ended);
    FUZZ_CHECK(reply.rfind("HTTP/1.1 200", 0) == 0);
    return 0;
}
