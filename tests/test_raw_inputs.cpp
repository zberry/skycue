#define BOOST_TEST_MODULE skycued_raw_inputs
#include <boost/test/unit_test.hpp>

#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "hub/raw_tap.hpp"
#include "inputs/input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json_input.hpp"
#include "inputs/readsb_stream_input.hpp"
#include "inputs/source_input.hpp"
#include "inputs/uat_raw_input.hpp"
#include "json_lite.hpp"
#include "net_test_support.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace ax = skycued::asterix;
using asio::ip::tcp;
using asio::ip::udp;
using skycue::net::test::freeTcpPort;
using skycue::net::test::freeUdpPort;
using skycue::net::test::IoThread;
using skycued::Input;
using skycued::InputDef;
using skycued::RawKind;
using skycued::RawMessage;
using skycued::RawTap;
using skycued::TrackObservation;
using namespace std::chrono_literals;

namespace {

using Bytes = std::vector<uint8_t>;
const std::chrono::milliseconds kWait = slow(5s);

template <class F> bool eventually(F pred)
{
    const auto until = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

std::string udpDef(const std::string& id, const char* type, uint16_t port, const std::string& extra = "")
{
    return R"({"id":")" + id + R"(","type":")" + type + R"(","transport":"udp","bind":"127.0.0.1","port":)" +
           std::to_string(port) + extra + "}";
}

std::string multicastDef(const std::string& id, const char* type, uint16_t port, const std::string& group)
{
    return R"({"id":")" + id + R"(","type":")" + type + R"(","transport":"udp","group":")" + group +
           R"(","interface":"127.0.0.1","port":)" + std::to_string(port) + "}";
}

std::string listenDef(const std::string& id, const char* type, uint16_t port)
{
    return R"({"id":")" + id + R"(","type":")" + type + R"(","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
           std::to_string(port) + "}";
}

struct Published {
    RawKind kind;
    std::string bytes, input;
};

// One input on its own io thread with a recording tap.
struct Rig {
    IoThread t;
    RawTap tap;
    std::mutex m;
    std::vector<Published> got;
    std::vector<std::string> uplinks;
    std::vector<TrackObservation> obs;
    std::unique_ptr<Input> in;

    void record(std::function<bool(RawKind)> wanted = {})
    {
        t.call([&] {
            tap.setSink(
                [this](const RawMessage& msg) {
                    std::lock_guard<std::mutex> lock(m);
                    got.push_back({msg.kind, std::string(msg.bytes->begin(), msg.bytes->end()), std::string(msg.input)});
                },
                std::move(wanted));
        });
    }
    void run(std::unique_ptr<Input> input)
    {
        t.call([&] {
            in = std::move(input);
            in->setRawTap(&tap);
            in->onUplink([this](std::chrono::system_clock::time_point, const std::string& hex) {
                std::lock_guard<std::mutex> lock(m);
                uplinks.push_back(hex);
            });
            in->onObservation([this](const TrackObservation& o) {
                std::lock_guard<std::mutex> lock(m);
                obs.push_back(o);
            });
            in->start();
        });
    }
    ~Rig()
    {
        try {
            t.call([&] { in.reset(); });
        } catch (const std::exception&) {
        }
    }
    skycued::InputState state() { return t.call([&] { return in->state(); }); }
    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(m);
        return got.size();
    }
    Published at(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(m);
        return got.at(i);
    }
    std::size_t uplinkCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return uplinks.size();
    }
    std::size_t obsCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs.size();
    }
    bool connected()
    {
        return state().connected;
    }
};

void sendUdp(uint16_t port, const std::string& s, const char* to = "127.0.0.1")
{
    asio::io_context io;
    udp::socket sock(io, udp::endpoint(udp::v4(), 0));
    sock.send_to(asio::buffer(s), udp::endpoint(asio::ip::make_address(to), port));
}

tcp::socket connectWhenListening(asio::io_context& io, uint16_t port)
{
    const auto until = std::chrono::steady_clock::now() + kWait;
    for (;;) {
        tcp::socket s(io);
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec)
            return s;
        if (std::chrono::steady_clock::now() > until)
            throw std::runtime_error("cannot connect to port " + std::to_string(port));
        std::this_thread::sleep_for(10ms);
    }
}

void writeAll(tcp::socket& s, const std::string& text) { asio::write(s, asio::buffer(text)); }

const std::string kReadsbLine =
    R"({"now":1793545200.052,"hex":"ac5e81","type":"adsb_icao","flight":"RPA4521 ","alt_baro":18000,"gs":379.0,)"
    R"("track":329.67,"squawk":"4711","lat":37.887976,"lon":-76.261396,"seen_pos":0.6,"rssi":-15.8})";

ax::Cat021Record record(uint32_t address)
{
    ax::Cat021Record r;
    r.sac = 0;
    r.sic = 0;
    r.trd = ax::Descriptor040{};
    r.address = address;
    r.lat131 = static_cast<int32_t>(std::lround(42.5 / ax::kLsb131Deg));
    r.lon131 = static_cast<int32_t>(std::lround(-93.25 / ax::kLsb131Deg));
    return r;
}

Bytes block(uint32_t address) { return ax::encodeBlock({record(address)}); }

// CAT/LEN around `body`: another category, which the stream framer drops.
Bytes junkBlock()
{
    const Bytes body{0xAA, 0xBB, 0xCC};
    const std::size_t len = body.size() + 3;
    Bytes b{48, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// Loopback multicast needs an interface that can join; false (and a message)
// when the host cannot, so the case skips.
bool joined(Rig& rig, const char* what)
{
    if (eventually([&] { return rig.connected(); }))
        return true;
    BOOST_TEST_MESSAGE("SKIP " << what << ": this host cannot join a multicast group on loopback ("
                               << rig.state().last_error << ")");
    return false;
}

void sendMulticast(uint16_t port, const std::string& group, const std::string& text)
{
    asio::io_context io;
    udp::socket sock(io, udp::v4());
    sock.set_option(asio::ip::multicast::outbound_interface(asio::ip::make_address_v4("127.0.0.1")));
    sock.set_option(asio::ip::multicast::enable_loopback(true));
    sock.send_to(asio::buffer(text), udp::endpoint(asio::ip::make_address(group), port));
}

}  // namespace

BOOST_AUTO_TEST_CASE(readsb_line_published_before_parse)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<skycued::ReadsbStreamInput>(rig.t.io, defOf(udpDef("rs", "readsb", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, "this is not json\n" + kReadsbLine + "\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    BOOST_TEST((rig.at(0).kind == RawKind::json));
    BOOST_TEST(rig.at(0).bytes == "this is not json");  // published although it does not parse
    BOOST_TEST(rig.at(0).input == "rs");
    BOOST_TEST(rig.at(1).bytes == kReadsbLine);
    BOOST_REQUIRE(eventually([&] { return rig.obsCount() == 1; }));
    const auto s = rig.state();
    BOOST_TEST(s.counters.rejected == 1u);
    BOOST_TEST(s.counters.observations == 1u);
    BOOST_TEST(s.counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(readsb_tcp_listen_and_udp_parse_observations)
{
    Rig tcp_rig, udp_rig;
    const uint16_t tport = freeTcpPort(), uport = freeUdpPort();
    tcp_rig.run(std::make_unique<skycued::ReadsbStreamInput>(tcp_rig.t.io, defOf(listenDef("rt", "readsb", tport))));
    udp_rig.run(std::make_unique<skycued::ReadsbStreamInput>(udp_rig.t.io, defOf(udpDef("ru", "readsb", uport))));
    BOOST_REQUIRE(eventually([&] { return udp_rig.connected(); }));
    sendUdp(uport, kReadsbLine);  // an unterminated line counts
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, tport);
    const std::size_t half = kReadsbLine.size() / 2;
    writeAll(c, kReadsbLine.substr(0, half));  // split across writes
    std::this_thread::sleep_for(50ms);
    writeAll(c, kReadsbLine.substr(half) + "\n");
    BOOST_REQUIRE(eventually([&] { return tcp_rig.obsCount() == 1 && udp_rig.obsCount() == 1; }));
    BOOST_TEST((tcp_rig.obs.at(0).id.address == 0xac5e81u));
    BOOST_TEST(tcp_rig.obs.at(0).input_id == "rt");
    BOOST_TEST(udp_rig.obs.at(0).input_id == "ru");
    BOOST_TEST(tcp_rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(readsb_udp_multicast_receives_on_loopback)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    const std::string group = "239.255.77.2";
    rig.run(std::make_unique<skycued::ReadsbStreamInput>(
        rig.t.io, defOf(multicastDef("rm", "readsb", port, group))));
    if (!joined(rig, "readsb_udp_multicast_receives_on_loopback"))
        return;
    sendMulticast(port, group, kReadsbLine + "\n");
    BOOST_REQUIRE(eventually([&] { return rig.obsCount() == 1; }));
    BOOST_TEST(rig.at(0).bytes == kReadsbLine);
}

BOOST_AUTO_TEST_CASE(uat_raw_downlink_counted_uplink_reaches_fisb_path)
{
    asio::io_context sio;
    tcp::acceptor acc(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    Rig rig;
    rig.record();
    InputDef def = defOf(R"({"id":"uat","type":"uat_raw","host":"127.0.0.1","port":)" +
                         std::to_string(acc.local_endpoint().port()) + "}");
    rig.run(std::make_unique<skycued::UatRawInput>(rig.t.io, def));
    tcp::socket server = acc.accept();
    const std::string payload = "-00a66ef135445d525a0c0519119021204800;rs=1;rssi=-10.0;\n"
                                "!meta;\n"
                                "+3c5e8a;rs=2;\r\n"
                                "-00a66ef1354;\n";
    writeAll(server, payload);
    BOOST_REQUIRE(eventually([&] { return rig.uplinkCount() == 1; }));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 3; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.uplinks.at(0) == "3c5e8a");
    BOOST_TEST(rig.at(0).bytes == "-00a66ef135445d525a0c0519119021204800;rs=1;rssi=-10.0;");
    BOOST_TEST(rig.at(1).bytes == "+3c5e8a;rs=2;");
    BOOST_TEST((rig.at(1).kind == RawKind::uat));
    const auto s = rig.state();
    BOOST_TEST(s.counters.messages == 3u);
    BOOST_TEST(s.counters.uplinks == 1u);
    BOOST_TEST(s.counters.bytes == payload.size());
    const auto* uat = dynamic_cast<skycued::UatRawInput*>(rig.in.get());
    BOOST_REQUIRE(uat);
    BOOST_TEST(uat->downlinkFrames() == 2u);
    BOOST_TEST(uat->uplinkFrames() == 1u);
}

BOOST_AUTO_TEST_CASE(uat_raw_udp_datagram_two_lines_publishes_two)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<skycued::UatRawInput>(rig.t.io, defOf(udpDef("uat", "uat_raw", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, "-0123;rs=1;\n+4567;rs=2;\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    BOOST_TEST(rig.at(0).bytes == "-0123;rs=1;");
    BOOST_TEST(rig.at(1).bytes == "+4567;rs=2;");
    BOOST_TEST(rig.state().counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(uat_raw_unterminated_datagram_line_counts)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<skycued::UatRawInput>(rig.t.io, defOf(udpDef("uat", "uat_raw", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, "+4567;rs=2");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    BOOST_TEST(rig.at(0).bytes == "+4567;rs=2");
    BOOST_REQUIRE(eventually([&] { return rig.uplinkCount() == 1; }));
    BOOST_TEST(rig.uplinks.at(0) == "4567");
    // The next datagram does not inherit the unterminated text.
    sendUdp(port, "-89ab;\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    BOOST_TEST(rig.at(1).bytes == "-89ab;");
}

BOOST_AUTO_TEST_CASE(dump978_alias_keeps_id_type_and_behaviour)
{
    asio::io_context sio;
    tcp::acceptor acc(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    Rig rig;
    rig.record();
    InputDef def;
    def.id = "dump978";
    def.type = skycued::InputType::dump978;
    def.host = "127.0.0.1";
    def.port = acc.local_endpoint().port();
    def.builtin = true;
    rig.run(std::make_unique<skycued::UatRawInput>(rig.t.io, def));
    BOOST_TEST(rig.in->def().id == "dump978");
    BOOST_TEST(std::string(skycued::inputTypeName(rig.in->def().type)) == "dump978");
    BOOST_TEST(!skycued::isTrackType(rig.in->def().type));
    tcp::socket first = acc.accept();
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    writeAll(first, "+0123;\n-4567;\n");
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.messages == 2; }));
    BOOST_TEST(rig.state().counters.uplinks == 1u);
    BOOST_TEST(rig.state().counters.bytes == 14u);
    BOOST_TEST(rig.at(0).input == "dump978");
    BOOST_TEST(rig.uplinks.at(0) == "0123");
    // The connection drops and comes back.
    first.close();
    BOOST_REQUIRE(eventually([&] { return !rig.connected(); }));
    tcp::socket second = acc.accept();
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    writeAll(second, "+89ab;\n");
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.uplinks == 2; }));
    BOOST_TEST(rig.state().counters.connects == 2u);
    BOOST_TEST(rig.state().last_error == "");  // as before: the input notes no transport error
    BOOST_TEST(rig.state().counters.messages == 3u);
}

BOOST_AUTO_TEST_CASE(second_uat_feed_same_uplink_deduped)
{
    IoThread t;
    skycued::UplinkDedupe dedupe;
    std::mutex m;
    int fresh = 0, duplicates = 0;
    std::unique_ptr<Input> a, b;
    const uint16_t pa = freeUdpPort();
    const uint16_t pb = freeUdpPort();
    t.call([&] {
        a = std::make_unique<skycued::UatRawInput>(t.io, defOf(udpDef("uat-a", "uat_raw", pa)));
        b = std::make_unique<skycued::UatRawInput>(t.io, defOf(udpDef("uat-b", "uat_raw", pb)));
        for (Input* in : {a.get(), b.get()}) {
            in->onUplink([&, id = in->def().id](std::chrono::system_clock::time_point, const std::string& hex) {
                std::lock_guard<std::mutex> lock(m);
                (dedupe.duplicate(id, std::chrono::steady_clock::now(), hex) ? duplicates : fresh)++;
            });
            in->start();
        }
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return a->state().connected && b->state().connected; }); }));
    sendUdp(pa, "+3c5e8a;rs=1;\n");
    BOOST_REQUIRE(eventually([&] {
        std::lock_guard<std::mutex> lock(m);
        return fresh == 1;
    }));
    sendUdp(pb, "+3c5e8a;rs=2;\n");
    BOOST_REQUIRE(eventually([&] {
        std::lock_guard<std::mutex> lock(m);
        return duplicates == 1;
    }));
    BOOST_TEST(fresh == 1);
    t.call([&] {
        a.reset();
        b.reset();
    });
}

BOOST_AUTO_TEST_CASE(pingstation_json_tcp_listen_one_text_per_line)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<skycued::PingJsonInput>(rig.t.io, defOf(listenDef("ps", "pingstation_json", port))));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    writeAll(c, R"({"a":1})" "\n" R"({"b")");
    std::this_thread::sleep_for(50ms);
    writeAll(c, ":2}\r\n\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    BOOST_TEST((rig.at(0).kind == RawKind::pingstation_json));
    BOOST_TEST(rig.at(0).bytes == R"({"a":1})");
    BOOST_TEST(rig.at(1).bytes == R"({"b":2})");
    BOOST_TEST(rig.at(1).input == "ps");
    BOOST_TEST(rig.state().counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(pingstation_json_multicast_input)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    const std::string group = "239.255.77.3";
    rig.run(std::make_unique<skycued::PingJsonInput>(
        rig.t.io, defOf(multicastDef("psm", "pingstation_json", port, group))));
    if (!joined(rig, "pingstation_json_multicast_input"))
        return;
    sendMulticast(port, group, R"({"m":1})");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    BOOST_TEST(rig.at(0).bytes == R"({"m":1})");
}

BOOST_AUTO_TEST_CASE(pingstation_json_datagram_with_newline_published_as_is)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<skycued::PingJsonInput>(rig.t.io, defOf(udpDef("ps", "pingstation_json", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    const std::string text = "{\"a\":1}\n";
    sendUdp(port, text);
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    BOOST_TEST(rig.at(0).bytes == text);  // the datagram, newline and all
    BOOST_TEST(rig.state().counters.messages == 1u);
}

BOOST_AUTO_TEST_CASE(cat021_block_published_per_block)
{
    Rig rig;
    rig.record();
    const uint16_t port = freeTcpPort();
    InputDef def = defOf(R"({"id":"cat","type":"pingstation_cat021","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                         std::to_string(port) + "}");
    rig.run(std::make_unique<skycued::PingCat021Input>(rig.t.io, def));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    const Bytes one = block(0xABC001), two = block(0xABC002), junk = junkBlock();
    Bytes stream;
    for (const Bytes* b : {&one, &junk, &two})
        stream.insert(stream.end(), b->begin(), b->end());
    asio::write(c, asio::buffer(stream));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    BOOST_TEST((rig.at(0).kind == RawKind::cat021));
    BOOST_TEST((rig.at(0).bytes == std::string(one.begin(), one.end())));
    BOOST_TEST((rig.at(1).bytes == std::string(two.begin(), two.end())));
    BOOST_TEST(rig.state().counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(unwanted_kind_costs_no_copy)
{
    Rig rig;
    rig.record([](RawKind kind) { return kind == RawKind::cat021; });  // nothing wants uat
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<skycued::UatRawInput>(rig.t.io, defOf(udpDef("uat", "uat_raw", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    BOOST_TEST(!rig.t.call([&] { return rig.tap.wanted(RawKind::uat); }));
    sendUdp(port, "+3c5e8a;\n-0123;\n");
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.messages == 2; }));
    BOOST_TEST(rig.count() == 0u);  // the sink was never called
    BOOST_TEST(rig.uplinkCount() == 1u);  // the input still did its own work
}
