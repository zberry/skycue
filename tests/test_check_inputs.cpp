#define BOOST_TEST_MODULE check_inputs
#include <boost/test/unit_test.hpp>

#include "check_inputs.hpp"
#include "conf_support.hpp"
#include "engine_test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace skycued;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

using skycue_test::tomlConf;

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

namespace {

// Runs skycued --check-inputs on `conf` and `inputs`; returns stdout and the exit status.
std::string runCheck(const std::string& conf, const std::string& inputs, int& status, const std::string& flags = "--no-uat")
{
    char dir[] = "/tmp/skycue-check-XXXXXX";
    BOOST_REQUIRE(mkdtemp(dir) != nullptr);
    const std::string d = dir;
    std::ofstream(d + "/c.toml") << conf;
    std::ofstream(d + "/i.json") << inputs;
    const std::string cmd = std::string(SKYCUED_EXE) + " --config " + d + "/c.toml --inputs-file " + d +
                            "/i.json " + flags + " --check-inputs 2>&1";
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    BOOST_REQUIRE(p != nullptr);
    char buf[512];
    while (fgets(buf, sizeof buf, p))
        out += buf;
    status = pclose(p);
    std::remove((d + "/c.toml").c_str());
    std::remove((d + "/i.json").c_str());
    rmdir(d.c_str());
    return out;
}

size_t countLines(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(check_inputs_skycued_probes_beast_port_and_warns)
{
    const uint16_t beast = closedPort();
    const std::string inputs =
        R"({"version":1,"inputs":[{"id":"rs2","type":"readsb","host":"127.0.0.1","port":)" + std::to_string(closedPort()) + "}]}";
    const std::string common = tomlConf({{"receiver.lat", "51.5"}});
    int status = 0;
    const std::string sky = runCheck(tomlConf({{"receiver.lat", "51.5"},
                                               {"inputs.decoder", "\"skycued\""},
                                               {"inputs.readsb.beast_port", std::to_string(beast)}}),
                                     inputs, status);
    BOOST_TEST(countLines(sky, "WARN: TRACK_DECODER=skycued: input 'rs2' (readsb JSON) and input 'readsb' (beast_raw)") == 1u, sky);
    BOOST_TEST(sky.find("readsb 127.0.0.1:" + std::to_string(beast) + ": ") != std::string::npos, sky);
    BOOST_TEST((WIFEXITED(status) && WEXITSTATUS(status) == 1));  // nothing listens: unchanged rule

    // The default decoder: no WARN, the JSON port probed.
    const std::string def = runCheck(common, inputs, status);
    BOOST_TEST(def.find("TRACK_DECODER") == std::string::npos, def);
    BOOST_TEST(def.find("readsb 127.0.0.1:30047: ") != std::string::npos, def);
}

BOOST_AUTO_TEST_CASE(check_inputs_skycued_warns_for_readsb_json_with_uat_raw)
{
    const std::string common = tomlConf({{"receiver.lat", "51.5"},
                                         {"inputs.decoder", "\"skycued\""},
                                         {"inputs.readsb.beast_port", std::to_string(closedPort())}});
    const std::string rs = R"({"id":"rs2","type":"readsb","host":"127.0.0.1","port":)" + std::to_string(closedPort()) + "}";
    const std::string uat = R"({"id":"u1","type":"uat_raw","transport":"udp","bind":"127.0.0.1","port":)" +
                            std::to_string(closedPort()) + "}";
    int with = 0, without = 0, builtin = 0;
    const std::string a = runCheck(common, R"({"version":2,"inputs":[)" + rs + "," + uat + "]}", with);
    const std::string needle = "WARN: TRACK_DECODER=skycued: input 'rs2' (readsb JSON) and input 'u1' (uat_raw)";
    BOOST_TEST(countLines(a, needle) == 1u, a);
    // The same inputs without the uat_raw: no such line, and the exit status is the connection rule's alone.
    const std::string b = runCheck(common, R"({"version":2,"inputs":[)" + rs + "]}", without);
    BOOST_TEST(b.find("(uat_raw)") == std::string::npos, b);
    BOOST_TEST((WIFEXITED(with) && WEXITSTATUS(with) == WEXITSTATUS(without)));
    // The built-in dump978 (UAT not disabled) counts as a UAT feed too.
    const std::string c = runCheck(common, R"({"version":2,"inputs":[)" + rs + "]}", builtin, "");
    BOOST_TEST(countLines(c, "input 'rs2' (readsb JSON) and input 'dump978' (dump978)") == 1u, c);
    // Under the default decoder there is no WARN at all.
    int def = 0;
    const std::string d = runCheck(tomlConf({{"receiver.lat", "51.5"}}), R"({"version":2,"inputs":[)" + rs + "," + uat + "]}", def);
    BOOST_TEST(d.find("TRACK_DECODER") == std::string::npos, d);
}
