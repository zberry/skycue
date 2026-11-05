#define BOOST_TEST_MODULE check_inputs
#include <boost/test/unit_test.hpp>

#include "check_inputs.hpp"
#include "engine_test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <string>

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
