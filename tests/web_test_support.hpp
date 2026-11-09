#pragma once

// A synchronous HTTP client (Reply, exchange, request, get, requestWith) and a
// WebSocket client whose thread collects every text message, for the tests that
// talk to the WebServer. Needs Boost.Beast, so only the web tests include it.

#include "test_slow.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/test/unit_test.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace skycue_test {

// The default wait of WsClient::waitFor and friends.
inline const std::chrono::milliseconds kWsWait = slow(std::chrono::seconds(5));

// ---- a minimal synchronous HTTP client ----

struct Reply {
    unsigned status = 0;
    std::string body, content_type, cache_control, allow;
};

inline void setRecvTimeout(boost::asio::ip::tcp::socket& s, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// One request on an open socket; put and post carry `body` as JSON.
inline Reply exchange(boost::asio::ip::tcp::socket& s, boost::beast::http::verb verb, const std::string& target,
                      const std::string& body = "", bool keep_alive = false)
{
    namespace http = boost::beast::http;
    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1");
    if (verb == http::verb::put || verb == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
    }
    req.keep_alive(keep_alive);
    req.prepare_payload();
    http::write(s, req);
    boost::beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    parser.body_limit(64u << 20);
    http::read(s, buf, parser);
    const auto& res = parser.get();
    Reply r;
    r.status = res.result_int();
    r.body = res.body();
    r.content_type = std::string(res[http::field::content_type]);
    r.cache_control = std::string(res[http::field::cache_control]);
    r.allow = std::string(res[http::field::allow]);
    return r;
}

// The read timeout of the one-shot requests below, scaled by SKYCUE_TEST_SLOW.
inline int httpRecvSeconds()
{
    return static_cast<int>(slow(std::chrono::seconds(10)).count() / 1000);
}

// One request on a fresh connection, with a 10 s read timeout (times the slow factor).
inline Reply request(uint16_t port, boost::beast::http::verb verb, const std::string& target,
                     const std::string& body = "")
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket s(io);
    s.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
    setRecvTimeout(s, httpRecvSeconds());
    return exchange(s, verb, target, body);
}

inline Reply get(uint16_t port, const std::string& target)
{
    return request(port, boost::beast::http::verb::get, target);
}

using Headers = std::vector<std::pair<std::string, std::string>>;

// A request with exactly these headers (besides Host, unless given, and
// Content-Length).
inline Reply requestWith(uint16_t port, boost::beast::http::verb verb, const std::string& target,
                         const std::string& body, const Headers& headers)
{
    namespace http = boost::beast::http;
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket s(io);
    s.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
    setRecvTimeout(s, httpRecvSeconds());
    http::request<http::string_body> req{verb, target, 11};
    bool host = false;
    for (const auto& h : headers) {
        req.set(h.first, h.second);
        host = host || h.first == "Host";
    }
    if (!host)
        req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    req.body() = body;
    req.keep_alive(false);
    req.prepare_payload();
    http::write(s, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(s, buf, res);
    Reply r;
    r.status = res.result_int();
    r.body = res.body();
    r.content_type = std::string(res[http::field::content_type]);
    return r;
}

// ---- a minimal WebSocket client: a thread collects every text message ----

struct WsClient {
    boost::asio::io_context io;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws{io};
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> msgs;
    bool closed = false;

    // `read` false: never reads after the handshake (a stalled client), with
    // `rcvbuf` bytes of socket receive buffer when not 0.
    explicit WsClient(uint16_t port, bool read = true, int rcvbuf = 0)
    {
        auto& s = ws.next_layer();
        s.open(boost::asio::ip::tcp::v4());
        if (rcvbuf)
            s.set_option(boost::asio::socket_base::receive_buffer_size(rcvbuf));
        s.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        ws.handshake("127.0.0.1", "/ws");
        if (read)
            reader = std::thread([this] { loop(); });
    }
    ~WsClient() { close(); }

    void close()
    {
        ::shutdown(ws.next_layer().native_handle(), SHUT_RDWR);
        if (reader.joinable())
            reader.join();
        boost::system::error_code ec;
        ws.next_layer().close(ec);
    }

    void loop()
    {
        for (;;) {
            boost::beast::flat_buffer b;
            boost::system::error_code ec;
            ws.read(b, ec);
            std::lock_guard<std::mutex> lk(m);
            if (ec) {
                closed = true;
                cv.notify_all();
                return;
            }
            msgs.emplace_back(boost::asio::buffers_begin(b.data()), boost::asio::buffers_end(b.data()));
            cv.notify_all();
        }
    }

    // Sends one short masked text frame by writing it to the socket itself.
    // The websocket stream is not safe for a write on this thread while the
    // reader thread is inside ws.read() (TSan saw them race on its state);
    // the socket is.
    void sendText(const std::string& text)
    {
        BOOST_REQUIRE(text.size() < 126);
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        std::string frame;
        frame += static_cast<char>(0x81);  // FIN, text
        frame += static_cast<char>(0x80 | text.size());
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < text.size(); ++i)
            frame += static_cast<char>(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
        boost::asio::write(ws.next_layer(), boost::asio::buffer(frame));
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWsWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, p);
    }
    // Waits for a message containing every needle; returns it ("" on timeout).
    std::string waitForMessage(const std::vector<std::string>& needles,
                               std::chrono::milliseconds timeout = kWsWait)
    {
        std::string found;
        waitFor(
            [&] {
                for (const auto& msg : msgs) {
                    if (std::all_of(needles.begin(), needles.end(),
                                    [&](const std::string& n) { return msg.find(n) != std::string::npos; })) {
                        found = msg;
                        return true;
                    }
                }
                return false;
            },
            timeout);
        return found;
    }
    // Waits until some message contains `needle`; returns it ("" on timeout).
    std::string waitForText(const std::string& needle, std::chrono::milliseconds timeout = kWsWait)
    {
        return waitForMessage({needle}, timeout);
    }
    std::vector<std::string> all()
    {
        std::lock_guard<std::mutex> lk(m);
        return msgs;
    }
};

}  // namespace skycue_test
