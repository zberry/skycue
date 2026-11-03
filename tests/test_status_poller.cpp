#define BOOST_TEST_MODULE skycued_status_poller
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"
#include "inputs/input_def.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json_input.hpp"
#include "inputs/status_poller.hpp"
#include "json_lite.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>

using namespace enginetest;
using asio::ip::udp;
using skycued::HttpUrl;
using skycued::InputState;
using skycued::PingCat021Input;
using skycued::PingJsonInput;
using skycued::StatusPoller;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

template <class F> bool eventuallyWithin(std::chrono::milliseconds limit, F pred)
{
    const auto until = steady::now() + limit;
    while (steady::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}
template <class F> bool eventually(F pred) { return eventuallyWithin(kWait, pred); }

std::string statusFixture()
{
    const std::string text = readAll(std::string(SKYCUE_FIXTURE_DIR) + "/pingstation/icd-status.json");
    BOOST_REQUIRE_MESSAGE(!text.empty(), "cannot open fixture icd-status.json");
    return text;
}

std::string response(const std::string& status_line, const std::string& body, const std::string& headers = "")
{
    return "HTTP/1.1 " + status_line + "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
}

uint16_t freeUdpPort()
{
    asio::io_context io;
    udp::socket s(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return s.local_endpoint().port();
}

// A port nothing listens on.
uint16_t closedTcpPort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

// A blocking HTTP server on 127.0.0.1:<ephemeral> with its own thread. The
// n-th connection gets replies[n] (the last one once they run out): its
// request is read up to the blank line and recorded, then, after `delay`,
// `text` is written and the connection closed. A `silent` reply reads the
// request and never answers (the connection is held until destruction).
struct HttpServer {
    struct Reply {
        std::string text;
        std::chrono::milliseconds delay{0};
        bool silent = false;
    };

    explicit HttpServer(std::vector<Reply> r) : replies(std::move(r))
    {
        acceptor.non_blocking(true);
        thread = std::thread([this] { run(); });
    }
    ~HttpServer()
    {
        done = true;
        thread.join();
    }

    uint16_t port() const { return acceptor.local_endpoint().port(); }
    HttpUrl url() const { return HttpUrl{"127.0.0.1", port(), "/api/v1/status"}; }
    std::string urlText() const { return "http://127.0.0.1:" + std::to_string(port()) + "/api/v1/status"; }
    std::size_t requestCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return requests.size();
    }
    std::string request(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(m);
        return i < requests.size() ? requests[i] : std::string();
    }

private:
    void run()
    {
        std::size_t next = 0;
        std::vector<std::unique_ptr<tcp::socket>> held;
        while (!done) {
            auto s = std::make_unique<tcp::socket>(io);
            boost::system::error_code ec;
            acceptor.accept(*s, ec);
            if (ec) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            timeval tv{2, 0};  // a client that never sends cannot hang the server
            ::setsockopt(s->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            asio::streambuf buf;
            asio::read_until(*s, buf, "\r\n\r\n", ec);
            const Reply r = replies[std::min(next++, replies.size() - 1)];
            {
                std::lock_guard<std::mutex> lock(m);
                requests.emplace_back(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
            }
            if (r.silent) {
                held.push_back(std::move(s));
                continue;
            }
            const auto until = steady::now() + r.delay;
            while (!done && steady::now() < until)
                std::this_thread::sleep_for(5ms);
            asio::write(*s, asio::buffer(r.text), ec);
            s->shutdown(tcp::socket::shutdown_both, ec);
            s->close(ec);
        }
    }

    asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    std::vector<Reply> replies;
    std::mutex m;
    std::vector<std::string> requests;
    std::atomic<bool> done{false};
    std::thread thread;
};

struct Result {
    bool ok = false;
    std::string body, error;
    steady::time_point at;
};

// One StatusPoller, started on the io thread.
struct PollerProbe {
    IoThread& t;
    std::unique_ptr<StatusPoller> poller;
    std::mutex m;
    std::vector<Result> results;
    steady::time_point started;

    PollerProbe(IoThread& thread, HttpUrl url, std::chrono::seconds interval = 30s,
                std::chrono::milliseconds timeout = 5s)
        : t(thread)
    {
        t.call([&] {
            poller = std::make_unique<StatusPoller>(t.io, std::move(url), interval, timeout);
            poller->onResult([this](const std::string* body, const std::string& error) {
                std::lock_guard<std::mutex> lock(m);
                results.push_back(Result{body != nullptr, body ? *body : std::string(), error, steady::now()});
            });
            started = steady::now();
            poller->start();
        });
    }
    ~PollerProbe()
    {
        t.call([&] { poller.reset(); });
    }
    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(m);
        return results.size();
    }
    Result at(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(m);
        return results.at(i);
    }
    void stop()
    {
        t.call([&] { poller->stop(); });
    }
};

skycued::InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

// One pingStation input, started on the io thread.
struct InputProbe {
    IoThread& t;
    std::unique_ptr<Input> in;

    InputProbe(IoThread& thread, std::unique_ptr<Input> input) : t(thread)
    {
        t.call([&] {
            in = std::move(input);
            in->start();
        });
    }
    ~InputProbe()
    {
        t.call([&] { in.reset(); });
    }
    InputState state()
    {
        return t.call([&] { return in->state(); });
    }
    void restart()
    {
        t.call([&] {
            in->stop();
            in->start();
        });
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(status_200_parsed)
{
    HttpServer server({{response("200 OK", statusFixture())}});
    IoThread t;
    InputProbe p(t, std::make_unique<PingJsonInput>(
                        t.io, defOf(R"({"id":"ps","type":"pingstation_json","bind":"127.0.0.1","port":)" +
                                    std::to_string(freeUdpPort()) + R"(,"status_url":")" + server.urlText() +
                                    R"("})")));
    BOOST_REQUIRE(eventually([&] { return p.state().counters.status_ok == 1u; }));
    const InputState s = p.state();
    BOOST_REQUIRE(s.device_status);
    BOOST_TEST(s.device_status->guid == "754104714b10828b");
    BOOST_TEST(s.counters.status_fail == 0u);
    BOOST_TEST(s.status_fail_streak == 0u);
    BOOST_TEST(s.last_error.empty());

    const std::string req = server.request(0);
    BOOST_TEST(contains(req, "GET /api/v1/status HTTP/1.1\r\n"));
    BOOST_TEST(contains(req, "Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n"));
    BOOST_TEST(contains(req, "Connection: close\r\n"));
    // The next poll waits for the interval (30 s by default).
    std::this_thread::sleep_for(200ms);
    BOOST_TEST(server.requestCount() == 1u);
}

BOOST_AUTO_TEST_CASE(cat021_input_polls_status)
{
    HttpServer server({{response("200 OK", statusFixture())}});
    IoThread t;
    InputProbe p(t, std::make_unique<PingCat021Input>(
                        t.io, defOf(R"({"id":"cat","type":"pingstation_cat021","transport":"udp",)"
                                    R"("bind":"127.0.0.1","port":)" +
                                    std::to_string(freeUdpPort()) + R"(,"status_url":")" + server.urlText() +
                                    R"("})")));
    BOOST_REQUIRE(eventually([&] { return p.state().counters.status_ok == 1u; }));
    BOOST_REQUIRE(p.state().device_status);
    BOOST_TEST(p.state().device_status->guid == "754104714b10828b");
}

BOOST_AUTO_TEST_CASE(status_500_fails)
{
    HttpServer server({{response("500 Internal Server Error", "{}")}});
    IoThread t;
    PollerProbe p(t, server.url());
    BOOST_REQUIRE(eventually([&] { return p.count() == 1u; }));
    BOOST_TEST(!p.at(0).ok);
    BOOST_TEST(p.at(0).error == "HTTP 500");
}

BOOST_AUTO_TEST_CASE(status_redirect_not_followed)
{
    HttpServer server({{response("302 Found", "", "Location: http://127.0.0.1:1/api/v1/status\r\n")},
                       {response("200 OK", statusFixture())}});
    IoThread t;
    PollerProbe p(t, server.url());
    BOOST_REQUIRE(eventually([&] { return p.count() == 1u; }));
    BOOST_TEST(!p.at(0).ok);
    BOOST_TEST(p.at(0).error == "HTTP 302 (redirects are not followed)");
    std::this_thread::sleep_for(200ms);
    BOOST_TEST(server.requestCount() == 1u);
    BOOST_TEST(p.count() == 1u);
}

BOOST_AUTO_TEST_CASE(status_timeout)
{
    HttpServer server({{"", 0ms, true}});
    IoThread t;
    PollerProbe p(t, server.url(), 30s, 200ms);
    BOOST_REQUIRE(eventuallyWithin(1s, [&] { return p.count() == 1u; }));
    const Result r = p.at(0);
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error == "no response within 200 ms");
    BOOST_TEST((r.at - p.started >= 190ms));
}

BOOST_AUTO_TEST_CASE(status_body_over_64k_fails)
{
    const std::string at_limit(skycued::kMaxStatusBytes, 'x');
    // With a Content-Length, then without one (the body runs to the close).
    HttpServer server({{response("200 OK", at_limit + "x")},
                       {"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + at_limit + "x"},
                       {response("200 OK", at_limit)}});
    IoThread t;
    for (int i = 0; i < 2; ++i) {
        PollerProbe p(t, server.url());
        BOOST_REQUIRE(eventually([&] { return p.count() == 1u; }));
        BOOST_TEST(!p.at(0).ok);
        BOOST_TEST(p.at(0).error == "response body over 65536 bytes");
    }
    PollerProbe p(t, server.url());  // exactly the limit is accepted
    BOOST_REQUIRE(eventually([&] { return p.count() == 1u; }));
    BOOST_TEST(p.at(0).ok);
    BOOST_TEST(p.at(0).body.size() == skycued::kMaxStatusBytes);
}

BOOST_AUTO_TEST_CASE(status_connection_refused)
{
    IoThread t;
    const uint16_t port = closedTcpPort();
    PollerProbe p(t, HttpUrl{"127.0.0.1", port, "/api/v1/status"});
    BOOST_REQUIRE(eventually([&] { return p.count() == 1u; }));
    BOOST_TEST(!p.at(0).ok);
    BOOST_TEST(contains(p.at(0).error, "cannot connect to 127.0.0.1:" + std::to_string(port) + ": "));
    BOOST_TEST(contains(p.at(0).error, "refused"));
}

BOOST_AUTO_TEST_CASE(poller_repeats_every_interval)
{
    HttpServer server({{response("200 OK", statusFixture())}});
    IoThread t;
    PollerProbe p(t, server.url(), 1s);
    BOOST_REQUIRE(eventuallyWithin(3s, [&] { return p.count() >= 2u; }));
    BOOST_TEST(p.at(0).ok);
    BOOST_TEST(p.at(1).ok);
    BOOST_TEST((p.at(0).at - p.started < 500ms));  // the first right after start()
    BOOST_TEST((p.at(1).at - p.at(0).at >= 900ms));
}

BOOST_AUTO_TEST_CASE(status_streak_resets_on_success)
{
    HttpServer server({{response("200 OK", R"({"aircraft":[]})")},
                       {response("500 Internal Server Error", "")},
                       {response("200 OK", statusFixture())}});
    IoThread t;
    InputProbe p(t, std::make_unique<PingJsonInput>(
                        t.io, defOf(R"({"id":"ps","type":"pingstation_json","bind":"127.0.0.1","port":)" +
                                    std::to_string(freeUdpPort()) + R"(,"status_url":")" + server.urlText() +
                                    R"("})")));
    // Each restart polls at once (the interval is 30 s).
    BOOST_REQUIRE(eventually([&] { return p.state().counters.status_fail == 1u; }));
    InputState s = p.state();
    BOOST_TEST(s.status_fail_streak == 1u);
    BOOST_TEST(s.last_error == "status: not a pingStation status document");
    BOOST_TEST(!s.device_status);

    p.restart();
    BOOST_REQUIRE(eventually([&] { return p.state().counters.status_fail == 2u; }));
    s = p.state();
    BOOST_TEST(s.status_fail_streak == 2u);
    BOOST_TEST(s.last_error == "status: HTTP 500");

    p.restart();
    BOOST_REQUIRE(eventually([&] { return p.state().counters.status_ok == 1u; }));
    s = p.state();
    BOOST_TEST(s.status_fail_streak == 0u);
    BOOST_TEST(s.counters.status_fail == 2u);
    BOOST_REQUIRE(s.device_status);
    BOOST_TEST(s.device_status->guid == "754104714b10828b");
}

BOOST_AUTO_TEST_CASE(poller_stop_cancels_inflight)
{
    HttpServer server({{response("200 OK", statusFixture()), 600ms}});
    IoThread t;
    PollerProbe p(t, server.url());
    BOOST_REQUIRE(eventually([&] { return server.requestCount() == 1u; }));
    p.stop();
    std::this_thread::sleep_for(900ms);  // past the server's answer
    BOOST_TEST(p.count() == 0u);
}
