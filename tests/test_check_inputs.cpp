#define BOOST_TEST_MODULE check_inputs
#include <boost/test/unit_test.hpp>

#include "check_inputs.hpp"
#include "engine_test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace skycued;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

uint16_t closedPort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

}  // namespace

BOOST_AUTO_TEST_CASE(listening_socket_connected)
{
    enginetest::TestServer s;
    const auto r = checkInputs({{"readsb", "127.0.0.1", s.port()}}, 3s);
    BOOST_REQUIRE(r.size() == 1u);
    BOOST_TEST(r[0].connected);
    BOOST_TEST(inputCheckLine(r[0]) == "readsb 127.0.0.1:" + std::to_string(s.port()) + ": connected");
}

BOOST_AUTO_TEST_CASE(closed_port_reports_error)
{
    const uint16_t p = closedPort();
    const auto r = checkInputs({{"readsb", "127.0.0.1", p}}, 3s);
    BOOST_REQUIRE(r.size() == 1u);
    BOOST_TEST(!r[0].connected);
    BOOST_TEST(!r[0].error.empty());
    const std::string line = inputCheckLine(r[0]);
    const std::string prefix = "readsb 127.0.0.1:" + std::to_string(p) + ": ";
    BOOST_TEST(line.rfind(prefix, 0) == 0u);
    BOOST_TEST(line != prefix + "connected");
}

BOOST_AUTO_TEST_CASE(unresolvable_host_reports_error)
{
    const auto start = std::chrono::steady_clock::now();
    const auto r = checkInputs({{"readsb", "no-such-host.invalid", 30047}}, 3s);
    BOOST_TEST((std::chrono::steady_clock::now() - start < 4s));
    BOOST_REQUIRE(r.size() == 1u);
    BOOST_TEST(!r[0].connected);
    BOOST_TEST(!r[0].error.empty());
}

BOOST_AUTO_TEST_CASE(check_inputs_bounded_by_timeout)
{
    enginetest::TestServer s;
    const uint16_t c1 = closedPort(), c2 = closedPort();
    const auto start = std::chrono::steady_clock::now();
    const auto r = checkInputs(
        {{"a", "127.0.0.1", c1}, {"b", "127.0.0.1", s.port()}, {"c", "127.0.0.1", c2}}, 1s);
    BOOST_TEST((std::chrono::steady_clock::now() - start < 2s));
    BOOST_REQUIRE(r.size() == 3u);
    BOOST_TEST(r[0].input.id == "a");
    BOOST_TEST(r[1].input.id == "b");
    BOOST_TEST(r[2].input.id == "c");
    BOOST_TEST(!r[0].connected);
    BOOST_TEST(r[1].connected);
    BOOST_TEST(!r[2].connected);
}

// 192.0.2.1 (TEST-NET-1) is not routable: the connect either hangs (then the
// timeout reports it) or is rejected at once (then the error says why).
BOOST_AUTO_TEST_CASE(unroutable_address_bounded)
{
    const auto start = std::chrono::steady_clock::now();
    const auto r = checkInputs({{"readsb", "192.0.2.1", 30047}}, 1s);
    BOOST_TEST((std::chrono::steady_clock::now() - start < 3s));
    BOOST_REQUIRE(r.size() == 1u);
    BOOST_TEST(!r[0].connected);
    BOOST_TEST(!r[0].error.empty());
}

// A listener that accepts and closes at once is reachable: the probe asks
// whether the connect succeeds, not whether the peer keeps talking.
BOOST_AUTO_TEST_CASE(check_inputs_connect_only)
{
    asio::io_context io;
    tcp::acceptor acc(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = acc.local_endpoint().port();
    std::function<void()> again = [&] {
        acc.async_accept([&](const boost::system::error_code& ec, tcp::socket s) {
            if (ec)
                return;
            boost::system::error_code ignored;
            s.close(ignored);
            again();
        });
    };
    again();
    std::thread t([&] { io.run(); });
    const auto r = checkInputs({{"readsb", "127.0.0.1", port}}, 3s);
    io.stop();
    t.join();
    BOOST_REQUIRE(r.size() == 1u);
    BOOST_TEST(r[0].connected);
    BOOST_TEST(inputCheckLine(r[0]) == "readsb 127.0.0.1:" + std::to_string(port) + ": connected");
}

// The old wording survives the move to the library: a refusal says just
// "Connection refused", a failed lookup "cannot resolve host: ...".
BOOST_AUTO_TEST_CASE(check_inputs_error_wording)
{
    const uint16_t p = closedPort();
    // A host name getaddrinfo rejects at once, so the wording is exact.
    const auto r = checkInputs({{"a", "127.0.0.1", p}, {"b", "bad_host!", 30047}}, 3s);
    BOOST_REQUIRE(r.size() == 2u);
    BOOST_TEST(r[0].error == "Connection refused");
    BOOST_TEST(r[1].error.rfind("cannot resolve host: ", 0) == 0u, "got: " << r[1].error);
}
