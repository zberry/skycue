#define BOOST_TEST_MODULE skycued_hub_core
#include <boost/test/unit_test.hpp>

#include "forwarder.hpp"
#include "hub/hub.hpp"
#include "net_test_support.hpp"

#include <skycue/net/buffer.hpp>
#include <skycue/protocol.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace skycued;
using skycue::net::test::eventually;
using skycue::net::test::freeTcpPort;
using skycue::net::test::freeUdpPort;
using skycue::net::test::holds;
using skycue::net::test::IoThread;

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using std::chrono::milliseconds;

namespace {

// The hub lives on the io thread, as in the daemon.
struct Rig {
    IoThread t;
    std::unique_ptr<Hub> hub;
    std::atomic<int> changes{0};

    Rig()
    {
        t.call([this] {
            hub = std::make_unique<Hub>(t.io);
            hub->onChanged([this] { ++changes; });
        });
    }
    ~Rig()
    {
        t.call([this] { hub.reset(); });
    }
    void apply(const std::vector<OutputDef>& defs) { t.call([&] { hub->apply(defs); }); }
    void publish(RawKind kind, const std::string& bytes)
    {
        t.call([&] {
            RawMessage m;
            m.kind = kind;
            m.bytes = skycue::net::makeBuf(bytes);
            m.input = "test";
            hub->publish(m);
        });
    }
    OutputState state(const std::string& id)
    {
        auto s = t.call([&] { return hub->find(id); });
        BOOST_REQUIRE(s.has_value());
        return *s;
    }
    OutputCounters counters(const std::string& id) { return state(id).counters; }
};

OutputDef tcpListen(const std::string& id, OutputKind kind, uint16_t port, const std::string& bind = "127.0.0.1")
{
    OutputDef d;
    d.id = id;
    d.name = id;
    d.kind = kind;
    d.transport = InputTransport::tcp_listen;
    d.bind = bind;
    d.port = port;
    return d;
}

OutputDef udpTo(const std::string& id, OutputKind kind, uint16_t port)
{
    OutputDef d;
    d.id = id;
    d.name = id;
    d.kind = kind;
    d.transport = InputTransport::udp;
    d.host = "127.0.0.1";
    d.port = port;
    return d;
}

OutputDef multicast(const std::string& id, OutputKind kind, const std::string& group, uint16_t port,
                    const std::string& iface)
{
    OutputDef d;
    d.id = id;
    d.name = id;
    d.kind = kind;
    d.transport = InputTransport::udp;
    d.group = group;
    d.port = port;
    d.iface = iface;
    return d;
}

// A blocking-style test client over a non-blocking socket.
struct Client {
    asio::io_context io;
    tcp::socket sock{io};
    std::string got;
    bool closed = false;

    explicit Client(uint16_t port, int rcvbuf = 0)
    {
        sock.open(tcp::v4());
        if (rcvbuf > 0)
            sock.set_option(asio::socket_base::receive_buffer_size(rcvbuf));
        sock.connect({asio::ip::make_address("127.0.0.1"), port});
        sock.non_blocking(true);
    }

    void pump()
    {
        char buf[8192];
        for (;;) {
            boost::system::error_code ec;
            const std::size_t n = sock.read_some(asio::buffer(buf), ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again)
                return;
            if (ec) {
                closed = true;
                return;
            }
            got.append(buf, n);
        }
    }

    bool readUntil(std::size_t size, milliseconds limit = slow(std::chrono::seconds(5)))
    {
        return eventually(
            [&] {
                pump();
                return got.size() >= size;
            },
            limit);
    }

    bool seesEnd(milliseconds limit = slow(std::chrono::seconds(5)))
    {
        return eventually(
            [&] {
                pump();
                return closed;
            },
            limit);
    }
};

struct UdpSink {
    asio::io_context io;
    udp::socket sock{io};
    uint16_t port = 0;

    UdpSink()
    {
        sock.open(udp::v4());
        sock.bind({asio::ip::make_address("127.0.0.1"), 0});
        port = sock.local_endpoint().port();
        sock.non_blocking(true);
    }

    bool next(std::string& out, milliseconds limit = slow(std::chrono::seconds(5)))
    {
        std::vector<char> buf(70000);
        return eventually(
            [&] {
                boost::system::error_code ec;
                const std::size_t n = sock.receive(asio::buffer(buf), 0, ec);
                if (ec)
                    return false;
                out.assign(buf.data(), n);
                return true;
            },
            limit);
    }
};

std::string frame(int i)
{
    std::string f = "\x1a\x32";
    f.push_back(static_cast<char>(i & 0xff));
    f.append(static_cast<std::size_t>(i % 17), static_cast<char>(0x1a));
    f += std::to_string(i);
    return f;
}

}  // namespace

BOOST_AUTO_TEST_CASE(beast_tcp_listen_delivers_frames_in_order)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    r.apply({tcpListen("b", OutputKind::beast, port)});
    Client c1(port), c2(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 2; }));
    std::string expect;
    for (int i = 0; i < 200; ++i) {
        const std::string f = frame(i);
        expect += f;
        r.publish(RawKind::beast, f);
    }
    BOOST_REQUIRE(c1.readUntil(expect.size()));
    BOOST_REQUIRE(c2.readUntil(expect.size()));
    BOOST_TEST(c1.got == expect);
    BOOST_TEST(c2.got == expect);
    const auto c = r.counters("b");
    BOOST_TEST(c.peers == 2u);
    BOOST_TEST(c.messages == 400u);
    BOOST_TEST(c.bytes == 2 * expect.size());
    BOOST_TEST(c.last_error.empty());
}

BOOST_AUTO_TEST_CASE(kinds_do_not_cross)
{
    Rig r;
    const uint16_t pb = freeTcpPort();
    uint16_t pj = freeTcpPort();
    while (pj == pb)
        pj = freeTcpPort();
    r.apply({tcpListen("b", OutputKind::beast, pb), tcpListen("j", OutputKind::json, pj)});
    Client cb(pb), cj(pj);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1 && r.counters("j").peers == 1; }));
    r.publish(RawKind::beast, "BEAST");
    BOOST_REQUIRE(cb.readUntil(5));
    BOOST_TEST(holds([&] { cj.pump(); return cj.got.empty(); }, slow(milliseconds(200))));
    r.publish(RawKind::json, "{}");
    BOOST_REQUIRE(cj.readUntil(3));
    BOOST_TEST(cj.got == "{}\n");
    cb.pump();
    BOOST_TEST(cb.got == "BEAST");
    BOOST_TEST(r.counters("b").messages == 1u);
    BOOST_TEST(r.counters("j").messages == 1u);
}

BOOST_AUTO_TEST_CASE(line_kinds_newline_on_stream_only)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    UdpSink sink;
    r.apply({tcpListen("t", OutputKind::uat, port), udpTo("u", OutputKind::uat, sink.port)});
    Client c(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("t").peers == 1; }));
    r.publish(RawKind::uat, "-line");
    BOOST_REQUIRE(c.readUntil(6));
    BOOST_TEST(c.got == "-line\n");
    std::string d;
    BOOST_REQUIRE(sink.next(d));
    BOOST_TEST(d == "-line");
}

BOOST_AUTO_TEST_CASE(json_over_65507_dropped_on_udp_counted)
{
    Rig r;
    UdpSink sink;
    r.apply({udpTo("u", OutputKind::json, sink.port)});
    r.publish(RawKind::json, std::string(65508, 'x'));
    BOOST_TEST(r.counters("u").dropped == 1u);
    r.publish(RawKind::json, std::string(65507, 'y'));
    std::string d;
    BOOST_REQUIRE(sink.next(d));
    BOOST_TEST(d.size() == 65507u);
    BOOST_TEST(r.counters("u").dropped == 1u);
    BOOST_TEST(r.counters("u").messages == 1u);
}

BOOST_AUTO_TEST_CASE(pingstation_json_newline_dropped_on_stream_sent_on_datagram)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    UdpSink sink;
    r.apply({tcpListen("t", OutputKind::pingstation_json, port), udpTo("u", OutputKind::pingstation_json, sink.port)});
    Client c(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("t").peers == 1; }));
    const std::string multi = "{\"a\":\n1}";
    r.publish(RawKind::pingstation_json, multi);
    BOOST_TEST(r.counters("t").dropped == 1u);
    std::string d;
    BOOST_REQUIRE(sink.next(d));
    BOOST_TEST(d == multi);
    r.publish(RawKind::pingstation_json, "{\"a\":1}");
    BOOST_REQUIRE(c.readUntil(8));
    BOOST_TEST(c.got == "{\"a\":1}\n");
    BOOST_TEST(r.counters("t").dropped == 1u);
    BOOST_TEST(r.counters("u").dropped == 0u);
}

BOOST_AUTO_TEST_CASE(output_without_input_is_valid)
{
    Rig r;
    r.apply({tcpListen("b", OutputKind::beast, freeTcpPort())});
    const auto s = r.state("b");
    BOOST_TEST(s.up);
    BOOST_TEST(s.counters.peers == 0u);
    BOOST_TEST(s.counters.last_error.empty());
}

BOOST_AUTO_TEST_CASE(wants_follows_enabled_outputs)
{
    Rig r;
    auto wants = [&](RawKind k) { return r.t.call([&] { return r.hub->wants(k); }); };
    BOOST_TEST(!wants(RawKind::json));
    OutputDef j = tcpListen("j", OutputKind::json, freeTcpPort());
    OutputDef u = tcpListen("u", OutputKind::uat, freeTcpPort());
    u.enabled = false;
    r.apply({j, u});
    BOOST_TEST(wants(RawKind::json));
    BOOST_TEST(!wants(RawKind::uat));
    BOOST_TEST(!wants(RawKind::beast));
    u.enabled = true;
    r.apply({j, u});
    BOOST_TEST(wants(RawKind::uat));
    r.apply({u});
    BOOST_TEST(!wants(RawKind::json));
    BOOST_TEST(wants(RawKind::uat));
}

BOOST_AUTO_TEST_CASE(slow_peer_cut_at_cap_others_continue)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    r.apply({tcpListen("b", OutputKind::beast, port)});
    Client stalled(port, 2048);
    Client good(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 2; }));
    std::atomic<std::size_t> received{0};
    std::atomic<bool> stop{false};
    std::thread drain([&] {
        std::vector<char> buf(65536);
        while (!stop) {
            boost::system::error_code ec;
            const std::size_t n = good.sock.read_some(asio::buffer(buf), ec);
            if (!ec)
                received += n;
            else if (ec == asio::error::would_block || ec == asio::error::try_again)
                std::this_thread::sleep_for(milliseconds(1));
            else
                return;
        }
    });
    const std::string chunk(65536, 'z');
    std::size_t sent = 0;
    for (int i = 0; i < 2000 && r.counters("b").dropped == 0; ++i) {
        r.publish(RawKind::beast, chunk);
        sent += chunk.size();
        std::this_thread::sleep_for(milliseconds(1));
    }
    BOOST_TEST(r.counters("b").dropped >= 1u);
    BOOST_TEST(eventually([&] { return r.counters("b").peers == 1; }));
    r.publish(RawKind::beast, "after");
    sent += 5;
    BOOST_TEST(eventually([&] { return received.load() == sent; }));
    stop = true;
    drain.join();
}

BOOST_AUTO_TEST_CASE(apply_edit_rebinds_same_port)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    OutputDef d = tcpListen("b", OutputKind::beast, port);
    r.apply({d});
    Client old(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
    d.bind = "0.0.0.0";
    r.apply({d});
    BOOST_TEST(old.seesEnd());
    BOOST_TEST(r.state("b").def.bind == "0.0.0.0");
    Client fresh(port);  // bound again at once
    BOOST_TEST(eventually([&] { return r.counters("b").peers == 1; }, slow(std::chrono::seconds(1))));
    BOOST_TEST(r.state("b").up);
}

BOOST_AUTO_TEST_CASE(apply_remove_stops_and_frees_port)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    r.apply({tcpListen("b", OutputKind::beast, port)});
    Client c(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
    r.apply({});
    BOOST_TEST(c.seesEnd());
    BOOST_TEST(!r.t.call([&] { return r.hub->find("b").has_value(); }));
    asio::io_context io;
    tcp::acceptor a(io);
    a.open(tcp::v4());
    a.set_option(asio::socket_base::reuse_address(true));
    boost::system::error_code ec;
    a.bind({asio::ip::make_address("127.0.0.1"), port}, ec);
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(remove_output_with_backlogged_peer)
{
    Rig r;
    const uint16_t pa = freeTcpPort();
    uint16_t pb = freeTcpPort();
    while (pb == pa)
        pb = freeTcpPort();
    const OutputDef a = tcpListen("a", OutputKind::beast, pa), b = tcpListen("b", OutputKind::beast, pb);
    r.apply({a, b});
    Client stalled(pa, 2048), other(pb);
    BOOST_REQUIRE(eventually([&] { return r.counters("a").peers == 1 && r.counters("b").peers == 1; }));
    const std::string chunk(32768, 'q');
    // Queue data for the stalled peer and remove its output in the same handler.
    r.t.call([&] {
        for (int i = 0; i < 20; ++i) {
            RawMessage m;
            m.kind = RawKind::beast;
            m.bytes = skycue::net::makeBuf(chunk);
            r.hub->publish(m);
        }
        r.hub->apply({b});
    });
    const int after_removal = r.changes.load();
    BOOST_TEST(stalled.seesEnd());
    BOOST_REQUIRE(other.readUntil(20 * chunk.size()));
    r.publish(RawKind::beast, "more");
    BOOST_REQUIRE(other.readUntil(20 * chunk.size() + 4));
    BOOST_TEST(r.state("b").up);
    BOOST_TEST(!r.t.call([&] { return r.hub->find("a").has_value(); }));
    BOOST_TEST(holds([&] { return r.changes.load() == after_removal; }, slow(milliseconds(300))));
}

BOOST_AUTO_TEST_CASE(counters_survive_edit)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    OutputDef d = tcpListen("b", OutputKind::beast, port);
    r.apply({d});
    {
        Client c(port);
        BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
        for (int i = 0; i < 10; ++i)
            r.publish(RawKind::beast, "0123456789");
        BOOST_REQUIRE(c.readUntil(100));
    }
    d.bind = "0.0.0.0";
    r.apply({d});
    BOOST_TEST(r.counters("b").messages == 10u);
    BOOST_TEST(r.counters("b").bytes == 100u);
    Client c2(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
    for (int i = 0; i < 5; ++i)
        r.publish(RawKind::beast, "abcde");
    BOOST_REQUIRE(c2.readUntil(25));
    BOOST_TEST(r.counters("b").messages == 15u);
    BOOST_TEST(r.counters("b").bytes == 125u);
}

BOOST_AUTO_TEST_CASE(tcp_connect_output_dials_and_reconnects)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    asio::io_context io;
    auto listen = [&] {
        auto a = std::make_unique<tcp::acceptor>(io);
        a->open(tcp::v4());
        a->set_option(asio::socket_base::reuse_address(true));
        a->bind({asio::ip::make_address("127.0.0.1"), port});
        a->listen();
        a->non_blocking(true);
        return a;
    };
    auto accept = [&](tcp::acceptor& a, tcp::socket& s) {
        return eventually([&] {
            boost::system::error_code ec;
            a.accept(s, ec);
            if (ec)
                return false;
            s.non_blocking(true);
            return true;
        });
    };
    auto readN = [&](tcp::socket& s, std::size_t n) {
        std::string got;
        return eventually([&] {
                   char buf[256];
                   boost::system::error_code ec;
                   const std::size_t k = s.read_some(asio::buffer(buf), ec);
                   if (!ec)
                       got.append(buf, k);
                   return got.size() >= n;
               }) && got.substr(0, n) == std::string(n, 'k');
    };
    OutputDef d;
    d.id = "c";
    d.kind = OutputKind::beast;
    d.transport = InputTransport::tcp_connect;
    d.host = "127.0.0.1";
    d.port = port;
    auto acceptor = listen();
    tcp::socket s1(io);
    r.apply({d});
    BOOST_REQUIRE(accept(*acceptor, s1));
    BOOST_REQUIRE(eventually([&] { return r.state("c").up; }));
    r.publish(RawKind::beast, "kkkk");
    BOOST_TEST(readN(s1, 4));
    s1.close();
    acceptor.reset();
    BOOST_REQUIRE(eventually([&] { return !r.state("c").up; }));
    acceptor = listen();
    tcp::socket s2(io);
    BOOST_REQUIRE(accept(*acceptor, s2));
    BOOST_REQUIRE(eventually([&] { return r.state("c").up; }));
    r.publish(RawKind::beast, "kkk");
    BOOST_TEST(readN(s2, 3));
    BOOST_TEST(r.counters("c").messages == 2u);
}

BOOST_AUTO_TEST_CASE(udp_multicast_output_loopback_delivery)
{
    Rig r;
    const uint16_t port = freeUdpPort();
    const std::string group = "239.255.42.7";
    asio::io_context io;
    udp::socket rx(io);
    try {
        rx.open(udp::v4());
        rx.set_option(asio::socket_base::reuse_address(true));
        rx.bind({asio::ip::make_address(group), port});
        rx.set_option(asio::ip::multicast::join_group(asio::ip::make_address_v4(group),
                                                      asio::ip::make_address_v4("127.0.0.1")));
        rx.non_blocking(true);
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE(std::string("multicast skipped: ") + e.what());
        return;
    }
    r.apply({multicast("m", OutputKind::json, group, port, "127.0.0.1")});
    if (!r.state("m").up) {
        BOOST_TEST_MESSAGE("multicast skipped: no usable multicast target: " + r.counters("m").last_error);
        return;
    }
    std::string got;
    const bool arrived = eventually([&] {
        r.publish(RawKind::json, "mc");
        char buf[64];
        boost::system::error_code ec;
        const std::size_t n = rx.receive(asio::buffer(buf), 0, ec);
        if (ec)
            return false;
        got.assign(buf, n);
        return true;
    });
    if (!arrived) {
        BOOST_TEST_MESSAGE("multicast skipped: nothing looped back on this host");
        return;
    }
    BOOST_TEST(got == "mc");
}

BOOST_AUTO_TEST_CASE(multicast_without_route_reports_error_not_throw)
{
    Rig r;
    const uint16_t sibling_port = freeTcpPort();
    const OutputDef sibling = tcpListen("s", OutputKind::beast, sibling_port);
    OutputDef m = multicast("m", OutputKind::json, "239.255.42.8", freeUdpPort(), "192.0.2.77");
    BOOST_CHECK_NO_THROW(r.apply({sibling, m}));
    const auto bad = r.state("m");
    BOOST_TEST(!bad.up);
    BOOST_TEST(!bad.counters.last_error.empty());
    BOOST_TEST(r.state("s").up);
    m.iface = "127.0.0.1";
    BOOST_CHECK_NO_THROW(r.apply({sibling, m}));
    const auto good = r.state("m");
    if (!good.up) {
        BOOST_TEST_MESSAGE("corrected multicast def not usable on this host: " + good.counters.last_error);
        return;
    }
    BOOST_TEST(good.counters.last_error.empty());
    BOOST_TEST(r.state("s").up);
}

BOOST_AUTO_TEST_CASE(cue_output_without_forwarder_is_listed_down)
{
    Rig r;
    OutputDef c;
    c.id = "cue-tcp";
    c.kind = OutputKind::cue;
    c.builtin = true;
    c.port = 30500;
    r.apply({tcpListen("b", OutputKind::beast, freeTcpPort()), c});
    const auto list = r.t.call([&] { return r.hub->list(); });
    BOOST_REQUIRE(list.size() == 2u);
    BOOST_TEST(list[0].def.id == "cue-tcp");  // built-ins first
    BOOST_TEST(!list[0].up);
    BOOST_TEST(list[0].counters.last_error == "cue outputs need the forwarder");
    BOOST_TEST(list[1].up);
}

BOOST_AUTO_TEST_CASE(publish_after_stop_is_noop)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    r.apply({tcpListen("b", OutputKind::beast, port)});
    Client c(port);
    BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
    r.t.call([&] { r.hub->stop(); });
    BOOST_TEST(c.seesEnd());
    BOOST_CHECK_NO_THROW(r.publish(RawKind::beast, "late"));
    BOOST_CHECK_NO_THROW(r.t.call([&] { r.hub->stop(); }));
    BOOST_CHECK_NO_THROW(r.apply({tcpListen("b", OutputKind::beast, port)}));
    BOOST_TEST(c.got.empty());
    BOOST_TEST(!r.t.call([&] { return r.hub->wants(RawKind::beast); }));
    BOOST_TEST(!r.state("b").up);
}

BOOST_AUTO_TEST_CASE(on_changed_fires_on_start_stop_and_error_change_only)
{
    Rig r;
    const uint16_t port = freeTcpPort();
    OutputDef b = tcpListen("b", OutputKind::beast, port);
    r.apply({b});
    BOOST_TEST(r.changes.load() == 1);  // started
    r.apply({b});
    BOOST_TEST(r.changes.load() == 1);  // nothing changed
    {
        Client c(port);
        BOOST_REQUIRE(eventually([&] { return r.counters("b").peers == 1; }));
        for (int i = 0; i < 100; ++i)
            r.publish(RawKind::beast, "x");
        BOOST_REQUIRE(c.readUntil(100));
    }
    BOOST_TEST(holds([&] { return r.changes.load() == 1; }, slow(milliseconds(200))));  // peers and bytes do not count
    OutputDef m = multicast("m", OutputKind::json, "239.255.42.8", freeUdpPort(), "192.0.2.77");
    r.apply({b, m});
    BOOST_TEST(r.changes.load() == 2);  // a new output, with an error
    r.apply({b, m});
    BOOST_TEST(r.changes.load() == 2);  // the same error again
    b.enabled = false;
    r.apply({b, m});
    BOOST_TEST(r.changes.load() == 3);  // stopped
    r.apply({});
    BOOST_TEST(r.changes.load() == 4);  // removed
}

namespace {

// A Forwarder and a Hub over it, on one io thread, as in the daemon.
struct CueRig {
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<Hub> hub;

    explicit CueRig(ForwarderConfig cfg, bool start = true)
    {
        cfg.keyframe_interval = std::chrono::seconds(10);
        cfg.heartbeat_interval = std::chrono::seconds(10);
        t.call([&] {
            fwd = std::make_unique<Forwarder>(t.io, cfg);
            fwd->setKeyframeProvider([] {
                skycue::Track tr;
                tr.id = skycue::TrackId{0xabc123, skycue::AddressType::icao};
                tr.source = skycue::Source::adsb_1090;
                tr.position_time = std::chrono::system_clock::now();
                tr.lat_deg = 38.9;
                tr.lon_deg = -77.0;
                return std::vector<skycue::Track>{tr};
            });
            hub = std::make_unique<Hub>(t.io, fwd.get());
            if (start)
                fwd->start();
        });
    }
    ~CueRig()
    {
        t.call([this] {
            hub.reset();
            fwd.reset();
        });
    }
    void apply(const std::vector<OutputDef>& defs, const std::map<std::string, bool>& builtin = {})
    {
        t.call([&] { hub->apply(defs, builtin); });
    }
    OutputState state(const std::string& id)
    {
        auto s = t.call([&] { return hub->find(id); });
        BOOST_REQUIRE(s.has_value());
        return *s;
    }
};

OutputDef cueListen(const std::string& id, uint16_t port)
{
    OutputDef d = tcpListen(id, OutputKind::cue, port);
    return d;
}

// The first frame type in what a client has received (Hello is 1 in the cue protocol).
bool startsWithHello(const std::string& bytes)
{
    skycue::Frame f;
    std::size_t used = 0;
    const auto st = skycue::decodeFrame(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), f, used);
    return st == skycue::DecodeStatus::ok && f.type == skycue::FrameType::hello;
}

}  // namespace

BOOST_AUTO_TEST_CASE(cue_output_added_at_runtime_greets_peer)
{
    ForwarderConfig cfg;
    cfg.tcp_listen_ephemeral = true;
    CueRig r(cfg);
    const uint16_t port = freeTcpPort();
    r.apply({cueListen("cue-extra", port)});
    BOOST_TEST(r.state("cue-extra").up);
    Client c(port);
    BOOST_REQUIRE(c.readUntil(1));
    BOOST_TEST(startsWithHello(c.got));
    BOOST_REQUIRE(eventually([&] { return r.state("cue-extra").counters.peers == 1; }));
    BOOST_REQUIRE(eventually([&] { return r.state("cue-extra").counters.messages >= 2; }));  // Hello and the keyframe
    BOOST_TEST(r.state("cue-extra").counters.bytes > 0u);
    BOOST_TEST(!r.state("cue-extra").def.builtin);

    // Removal closes the peer and the forwarder no longer has the destination.
    r.apply({});
    BOOST_TEST(c.seesEnd());
    BOOST_TEST(!r.t.call([&] { return r.hub->find("cue-extra").has_value(); }));
    BOOST_TEST(!r.t.call([&] { return r.fwd->destinationStats("cue-extra").has_value(); }));
}

BOOST_AUTO_TEST_CASE(builtin_cue_listed_not_removable)
{
    UdpSink sink;
    ForwarderConfig cfg;
    cfg.tcp_listen_ephemeral = true;
    cfg.udp_targets = {{"127.0.0.1", sink.port}};
    CueRig r(cfg);
    r.apply({tcpListen("b", OutputKind::beast, freeTcpPort())});
    auto list = r.t.call([&] { return r.hub->list(); });
    BOOST_REQUIRE(list.size() == 3u);
    BOOST_TEST(list[0].def.id == "cue-tcp");
    BOOST_TEST(list[1].def.id == "cue-udp-1");
    BOOST_TEST(list[2].def.id == "b");
    for (std::size_t i = 0; i < 2; ++i) {
        BOOST_TEST(list[i].def.builtin);
        BOOST_TEST((list[i].def.kind == OutputKind::cue));
        BOOST_TEST(list[i].def.enabled);
        BOOST_TEST(list[i].up);
    }
    BOOST_TEST((list[1].def.transport == InputTransport::udp));
    BOOST_TEST(list[1].def.port == sink.port);

    // Dropping them from `desired`, or redefining them, changes nothing.
    r.apply({});
    OutputDef imposter = tcpListen("cue-tcp", OutputKind::beast, freeTcpPort());
    r.apply({imposter});
    list = r.t.call([&] { return r.hub->list(); });
    BOOST_REQUIRE(list.size() == 2u);
    BOOST_TEST((list[0].def.kind == OutputKind::cue));
    BOOST_TEST(list[0].up);
    BOOST_TEST(r.t.call([&] { return r.fwd->destinations().size(); }) == 2u);
}

BOOST_AUTO_TEST_CASE(builtin_cue_disabled_before_start_not_bound)
{
    ForwarderConfig cfg;
    cfg.tcp_port = freeTcpPort();
    CueRig r(cfg, false);
    r.apply({}, {{"cue-tcp", false}});
    r.t.call([&] { r.fwd->start(); });  // does not throw and binds nothing
    BOOST_TEST(r.t.call([&] { return r.fwd->tcpPort(); }) == 0u);
    BOOST_TEST(!r.state("cue-tcp").def.enabled);
    BOOST_TEST(!r.state("cue-tcp").up);
    {
        asio::io_context io;
        tcp::acceptor probe(io, {asio::ip::make_address("0.0.0.0"), cfg.tcp_port});  // free: bindable by anyone
    }

    // Enabling it later binds the remembered port.
    r.apply({}, {{"cue-tcp", true}});
    BOOST_REQUIRE(eventually([&] { return r.state("cue-tcp").up; }));
    Client c(cfg.tcp_port);
    BOOST_REQUIRE(c.readUntil(1));
    BOOST_TEST(startsWithHello(c.got));

    // Disabling it again closes the peer and frees the port.
    r.apply({}, {{"cue-tcp", false}});
    BOOST_TEST(c.seesEnd());
    BOOST_TEST(!r.state("cue-tcp").up);
}
