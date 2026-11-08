#define BOOST_TEST_MODULE net_udp
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/buffer.hpp"
#include "skycue/net/udp_receiver.hpp"
#include "skycue/net/udp_sender.hpp"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace skycue::net;
using namespace skycue::net::test;
using std::chrono::milliseconds;
namespace asio = boost::asio;
using asio::ip::udp;

namespace {

struct Got {
    std::mutex m;
    std::vector<std::pair<std::string, udp::endpoint>> items;
    std::size_t count()
    {
        std::lock_guard<std::mutex> l(m);
        return items.size();
    }
    std::string at(std::size_t i)
    {
        std::lock_guard<std::mutex> l(m);
        return items.at(i).first;
    }
    udp::endpoint from(std::size_t i)
    {
        std::lock_guard<std::mutex> l(m);
        return items.at(i).second;
    }
};

void hook(UdpReceiver& r, const std::shared_ptr<Got>& g)
{
    r.onDatagram([g](const uint8_t* p, std::size_t n, const udp::endpoint& from) {
        std::lock_guard<std::mutex> l(g->m);
        g->items.emplace_back(std::string(reinterpret_cast<const char*>(p), n), from);
    });
}

std::shared_ptr<UdpReceiver> startReceiver(IoThread& t, const std::string& bind, uint16_t port,
                                           UdpReceiveOptions o, const std::shared_ptr<Got>& g)
{
    return t.call([&] {
        auto r = UdpReceiver::create(t.io, bind, port, o);
        hook(*r, g);
        r->start();
        return r;
    });
}

// A plain blocking sender, optionally bound to a local address.
void sendPlain(const std::string& to, uint16_t port, const std::string& data, const std::string& from = "")
{
    asio::io_context io;
    udp::socket s(io);
    s.open(udp::v4());
    if (!from.empty())
        s.bind(udp::endpoint(asio::ip::make_address(from), 0));
    s.send_to(asio::buffer(data), udp::endpoint(asio::ip::make_address(to), port));
}

UdpTarget target(const std::string& host, uint16_t port)
{
    UdpTarget t;
    t.host = host;
    t.port = port;
    return t;
}

constexpr const char* kGroup = "239.255.77.1";

// True when the receiver could not join because this host has no usable
// multicast interface; the multicast cases then skip.
bool noMulticast(IoThread& t, const std::shared_ptr<UdpReceiver>& r)
{
    const std::string e = t.call([&] { return r->counters().last_error; });
    return e.find("join") != std::string::npos &&
           (e.find("No such device") != std::string::npos || e.find("Cannot assign requested address") != std::string::npos);
}

template <class T, class F>
auto onIo(IoThread& t, const std::shared_ptr<T>& p, F f)
{
    return t.call([&] { return f(*p); });
}

int getIntOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof v;
    BOOST_REQUIRE_EQUAL(::getsockopt(fd, level, name, &v, &len), 0);
    return v;
}

} // namespace

BOOST_AUTO_TEST_CASE(udp_delivers_datagrams_and_peer)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    auto r = startReceiver(t, "127.0.0.1", 0, {}, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    const uint16_t port = r->localPort();
    BOOST_REQUIRE(port != 0);

    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        x->addTarget(target("127.0.0.1", port));
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    t.call([&] { s->send(makeBuf("hello")); return 0; });
    t.call([&] { s->send(makeBuf("world!")); return 0; });
    BOOST_REQUIRE(eventually([&] { return got->count() == 2; }));
    BOOST_CHECK_EQUAL(got->at(0), "hello");
    BOOST_CHECK_EQUAL(got->at(1), "world!");
    BOOST_CHECK_EQUAL(got->from(0).address().to_string(), "127.0.0.1");
    BOOST_CHECK_EQUAL(t.call([&] { return r->counters().messages_in; }), 2u);
    BOOST_CHECK_EQUAL(t.call([&] { return r->counters().bytes_in; }), 11u);
    BOOST_CHECK_EQUAL(t.call([&] { return s->counters().messages_out; }), 2u);
    BOOST_CHECK_EQUAL(t.call([&] { return s->counters().bytes_out; }), 11u);
    t.call([&] { s->stop(); r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_receive_buffer_is_enlarged)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    auto r = startReceiver(t, "127.0.0.1", 0, {}, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    const int fd = t.call([&] { return r->nativeHandleForTest(); });
    BOOST_REQUIRE(fd >= 0);
    BOOST_CHECK_GE(getIntOpt(fd, SOL_SOCKET, SO_RCVBUF), 256 * 1024);
    t.call([&] { r->stop(); return 0; });
    BOOST_CHECK_EQUAL(t.call([&] { return r->nativeHandleForTest(); }), -1);
}

BOOST_AUTO_TEST_CASE(udp_peer_filter)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    UdpReceiveOptions o;
    o.peer = "127.0.0.2";
    auto r = startReceiver(t, "127.0.0.1", 0, o, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    sendPlain("127.0.0.1", r->localPort(), "nope", "127.0.0.1");
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return r->counters().rejected_peer; }) == 1u; }));
    BOOST_CHECK_EQUAL(got->count(), 0u);
    const std::string err = t.call([&] { return r->counters().last_error; });
    BOOST_CHECK(err.find("127.0.0.1") != std::string::npos);
    sendPlain("127.0.0.1", r->localPort(), "yes", "127.0.0.2");
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    BOOST_CHECK_EQUAL(got->at(0), "yes");
    BOOST_CHECK_EQUAL(t.call([&] { return r->counters().rejected_peer; }), 1u);
    BOOST_CHECK_THROW(UdpReceiver::create(t.io, "127.0.0.1", 0, [] { UdpReceiveOptions x; x.peer = "host"; return x; }()),
                      std::invalid_argument);
    t.call([&] { r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_bind_failure_reported_and_retried)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    asio::io_context io2;
    auto holder = std::make_unique<udp::socket>(io2, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = holder->local_endpoint().port();
    UdpReceiveOptions o;
    o.rebind_delay = milliseconds(50);
    auto r = startReceiver(t, "127.0.0.1", port, o, got);
    BOOST_CHECK(!r->bound());
    BOOST_CHECK(!t.call([&] { return r->counters().last_error; }).empty());
    BOOST_CHECK(holds([&] { return !r->bound(); }, slow(milliseconds(150)))); // still held by the other socket
    holder.reset();
    BOOST_REQUIRE(eventually([&] { return r->bound(); }, slow(milliseconds(1000))));
    BOOST_CHECK_EQUAL(r->localPort(), port);
    BOOST_CHECK(t.call([&] { return r->counters().last_error; }).empty());
    sendPlain("127.0.0.1", port, "late");
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    t.call([&] { r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_state_callback_across_a_rebind)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    asio::io_context io2;
    auto holder = std::make_unique<udp::socket>(io2, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = holder->local_endpoint().port();
    UdpReceiveOptions o;
    o.rebind_delay = milliseconds(50);
    o.name = "udp-test";
    std::mutex m;
    std::vector<bool> states;
    auto r = t.call([&] {
        auto x = UdpReceiver::create(t.io, "127.0.0.1", port, o);
        hook(*x, got);
        x->onState([&](bool up) {
            std::lock_guard<std::mutex> l(m);
            states.push_back(up);
        });
        x->start();
        return x;
    });
    // The failed first bind is not an event, and nothing runs inside start().
    BOOST_CHECK(holds([&] { std::lock_guard<std::mutex> l(m); return states.empty(); }, slow(milliseconds(150))));
    BOOST_CHECK_EQUAL(r->receiveBufferBytes(), 0);
    holder.reset();
    BOOST_REQUIRE(eventually([&] { std::lock_guard<std::mutex> l(m); return states.size() == 1; }, slow(milliseconds(1000))));
    BOOST_CHECK(r->bound());
    BOOST_CHECK_GE(r->receiveBufferBytes(), 256 * 1024);
    t.call([&] { r->stop(); return 0; });
    BOOST_CHECK_EQUAL(r->receiveBufferBytes(), 0);
    BOOST_CHECK(holds([&] { std::lock_guard<std::mutex> l(m); return states == std::vector<bool>{true}; },
                      slow(milliseconds(100)))); // stop() is silent
}

// Makes the receiver's socket fail the way a real one can: with IP_RECVERR
// set, an ICMP port-unreachable for a datagram sent from the socket itself is
// reported by the next receive as ECONNREFUSED, which the receiver treats as a loss.
void provokeReceiveError(int fd)
{
    int on = 1;
    BOOST_REQUIRE_EQUAL(::setsockopt(fd, IPPROTO_IP, IP_RECVERR, &on, sizeof on), 0);
    // Nothing listens on the port of a socket that was just closed.
    asio::io_context io;
    udp::socket probe(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const udp::endpoint dead = probe.local_endpoint();
    probe.close();
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(dead.port());
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::sendto(fd, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to), 1);
}

BOOST_AUTO_TEST_CASE(udp_state_callback_bind_loss_rebind)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    UdpReceiveOptions o;
    o.rebind_delay = milliseconds(50);
    std::mutex m;
    std::vector<bool> states;
    auto r = t.call([&] {
        auto x = UdpReceiver::create(t.io, "127.0.0.1", 0, o);
        hook(*x, got);
        x->onState([&](bool up) {
            std::lock_guard<std::mutex> l(m);
            states.push_back(up);
        });
        x->start();
        return x;
    });
    auto seen = [&] { std::lock_guard<std::mutex> l(m); return states; };
    BOOST_REQUIRE(eventually([&] { return seen().size() == 1; }));
    const int fd = t.call([&] { return r->nativeHandleForTest(); });
    BOOST_REQUIRE(fd >= 0);
    provokeReceiveError(fd);
    BOOST_REQUIRE(eventually([&] { return seen().size() == 3; }, slow(milliseconds(2000))));
    BOOST_CHECK((seen() == std::vector<bool>{true, false, true}));
    BOOST_CHECK(r->bound());
    t.call([&] { r->stop(); return 0; });
    BOOST_CHECK(holds([&] { return seen().size() == 3; }, slow(milliseconds(100))));
}

BOOST_AUTO_TEST_CASE(udp_multicast_join_and_receive_on_loopback)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    UdpReceiveOptions o;
    o.group = kGroup;
    o.iface = "127.0.0.1";
    const uint16_t port = freeUdpPort();
    auto r = startReceiver(t, "0.0.0.0", port, o, got);
    if (noMulticast(t, r)) {
        BOOST_TEST_MESSAGE("SKIP: no multicast-capable loopback interface on this host");
        t.call([&] { r->stop(); return 0; });
        return;
    }
    BOOST_REQUIRE(r->bound());
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget tg = target(kGroup, port);
        tg.iface = "127.0.0.1";
        tg.loopback = true;
        x->addTarget(tg);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    t.call([&] { s->send(makeBuf("mc")); return 0; });
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    BOOST_CHECK_EQUAL(got->at(0), "mc");
    t.call([&] { s->stop(); r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_two_multicast_receivers_share_a_port)
{
    IoThread t;
    auto g1 = std::make_shared<Got>();
    auto g2 = std::make_shared<Got>();
    UdpReceiveOptions o;
    o.group = kGroup;
    o.iface = "127.0.0.1";
    const uint16_t port = freeUdpPort();
    auto r1 = startReceiver(t, "0.0.0.0", port, o, g1);
    if (noMulticast(t, r1)) {
        BOOST_TEST_MESSAGE("SKIP: no multicast-capable loopback interface on this host");
        t.call([&] { r1->stop(); return 0; });
        return;
    }
    auto r2 = startReceiver(t, "0.0.0.0", port, o, g2);
    BOOST_REQUIRE(r1->bound());
    BOOST_REQUIRE(r2->bound());
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget tg = target(kGroup, port);
        tg.iface = "127.0.0.1";
        x->addTarget(tg);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    t.call([&] { s->send(makeBuf("both")); return 0; });
    BOOST_REQUIRE(eventually([&] { return g1->count() == 1 && g2->count() == 1; }));
    BOOST_CHECK_EQUAL(g1->at(0), "both");
    BOOST_CHECK_EQUAL(g2->at(0), "both");
    t.call([&] { s->stop(); r1->stop(); r2->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_two_multicast_receivers_different_groups_isolated)
{
    IoThread t;
    auto g1 = std::make_shared<Got>();
    auto g2 = std::make_shared<Got>();
    UdpReceiveOptions o1;
    o1.group = "239.255.77.2";
    o1.iface = "127.0.0.1";
    UdpReceiveOptions o2 = o1;
    o2.group = "239.255.77.3";
    const uint16_t port = freeUdpPort();
    auto r1 = startReceiver(t, "0.0.0.0", port, o1, g1);
    if (noMulticast(t, r1)) {
        BOOST_TEST_MESSAGE("SKIP: no multicast-capable loopback interface on this host");
        t.call([&] { r1->stop(); return 0; });
        return;
    }
    auto r2 = startReceiver(t, "0.0.0.0", port, o2, g2);
    BOOST_REQUIRE(r1->bound());
    BOOST_REQUIRE(r2->bound());
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget a = target("239.255.77.2", port);
        a.iface = "127.0.0.1";
        UdpTarget b = target("239.255.77.3", port);
        b.iface = "127.0.0.1";
        x->addTarget(a);
        x->addTarget(b);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 2u);
    t.call([&] { s->send(makeBuf("x")); return 0; });
    BOOST_REQUIRE(eventually([&] { return g1->count() >= 1 && g2->count() >= 1; }));
    // Each receiver is bound to its own group, so each sees exactly one datagram.
    BOOST_CHECK(holds([&] { return g1->count() == 1 && g2->count() == 1; }, slow(milliseconds(200))));
    t.call([&] { s->stop(); r1->stop(); r2->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_unicast_and_multicast_do_not_share)
{
    IoThread t;
    auto g1 = std::make_shared<Got>();
    auto g2 = std::make_shared<Got>();
    auto uni = startReceiver(t, "0.0.0.0", 0, {}, g1);
    BOOST_REQUIRE(eventually([&] { return uni->bound(); }));
    const uint16_t port = uni->localPort();
    UdpReceiveOptions o;
    o.group = kGroup;
    o.iface = "127.0.0.1";
    o.rebind_delay = milliseconds(50);
    auto mc = startReceiver(t, "0.0.0.0", port, o, g2);
    // Unicast holds the port without SO_REUSEADDR, so the group bind must fail.
    BOOST_CHECK(!mc->bound());
    BOOST_CHECK(!t.call([&] { return mc->counters().last_error; }).empty());
    BOOST_CHECK(uni->bound());
    t.call([&] { uni->stop(); mc->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_multicast_join_failure_reported_and_retried)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    UdpReceiveOptions o;
    o.group = kGroup;
    o.iface = "192.0.2.1";
    o.rebind_delay = milliseconds(50);
    std::shared_ptr<UdpReceiver> r;
    BOOST_REQUIRE_NO_THROW(r = startReceiver(t, "0.0.0.0", freeUdpPort(), o, got));
    BOOST_CHECK(!r->bound());
    const std::string first = t.call([&] { return r->counters().last_error; });
    BOOST_CHECK(!first.empty());
    BOOST_CHECK(first.find("join") != std::string::npos);
    BOOST_CHECK(holds([&] { return !r->bound(); }, slow(milliseconds(200)))); // the join keeps failing
    BOOST_CHECK(!t.call([&] { return r->counters().last_error; }).empty());
    t.call([&] { r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_sender_sets_ttl_interface_loopback)
{
    IoThread t;
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget tg = target(kGroup, 4000);
        tg.iface = "127.0.0.1";
        tg.ttl = 4;
        tg.loopback = false;
        x->addTarget(tg);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    const int fd = t.call([&] { return s->nativeHandleForTest(); });
    BOOST_REQUIRE(fd >= 0);
    BOOST_CHECK_EQUAL(getIntOpt(fd, IPPROTO_IP, IP_MULTICAST_TTL), 4);
    BOOST_CHECK_EQUAL(getIntOpt(fd, IPPROTO_IP, IP_MULTICAST_LOOP), 0);
    in_addr ifa{};
    socklen_t len = sizeof ifa;
    BOOST_REQUIRE_EQUAL(::getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, &len), 0);
    BOOST_CHECK_EQUAL(ifa.s_addr, htonl(INADDR_LOOPBACK));
    t.call([&] { s->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_sender_sets_interface_per_target)
{
    IoThread t;
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget a = target(kGroup, 4000);
        a.iface = "127.0.0.1";
        UdpTarget b = target(kGroup, 4000);
        x->addTarget(a);
        x->addTarget(b);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 2u);
    const int fd = t.call([&] { return s->nativeHandleForTest(); });
    BOOST_REQUIRE(fd >= 0);
    auto ifaddr = [&] {
        in_addr ifa{};
        socklen_t len = sizeof ifa;
        BOOST_REQUIRE_EQUAL(::getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, &len), 0);
        return ifa.s_addr;
    };
    // After start() the second target's setting is in force.
    BOOST_CHECK_EQUAL(ifaddr(), htonl(INADDR_ANY));
    // A send visits both targets; the interface is re-set for each, so the
    // socket is left as the last target set it.
    t.call([&] { s->send(makeBuf("x")); return 0; });
    BOOST_CHECK_EQUAL(ifaddr(), htonl(INADDR_ANY));
    auto s1 = t.call([&] {
        auto x = UdpSender::create(t.io);
        UdpTarget a = target(kGroup, 4000);
        a.iface = "127.0.0.1";
        x->addTarget(a);
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s1->start(); }), 1u);
    const int fd1 = t.call([&] { return s1->nativeHandleForTest(); });
    in_addr ifa{};
    socklen_t len = sizeof ifa;
    BOOST_REQUIRE_EQUAL(::getsockopt(fd1, IPPROTO_IP, IP_MULTICAST_IF, &ifa, &len), 0);
    BOOST_CHECK_EQUAL(ifa.s_addr, htonl(INADDR_LOOPBACK));
    t.call([&] { s->stop(); s1->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_sender_skips_unresolvable_target)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    auto r = startReceiver(t, "127.0.0.1", 0, {}, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io);
        x->addTarget(target("no-such-host.invalid", 1));
        x->addTarget(target("127.0.0.1", r->localPort()));
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    BOOST_CHECK_EQUAL(t.call([&] { return s->targets(); }), 1u);
    BOOST_CHECK(t.call([&] { return s->counters().last_error; }).find("no-such-host.invalid") != std::string::npos);
    t.call([&] { s->send(makeBuf("ok")); return 0; });
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    BOOST_CHECK_EQUAL(got->at(0), "ok");
    t.call([&] { s->stop(); r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(udp_sender_drops_oversize)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    auto r = startReceiver(t, "127.0.0.1", 0, {}, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    auto s = t.call([&] {
        auto x = UdpSender::create(t.io, 100);
        x->addTarget(target("127.0.0.1", r->localPort()));
        return x;
    });
    BOOST_CHECK_EQUAL(t.call([&] { return s->start(); }), 1u);
    t.call([&] { s->send(makeBuf(std::string(200, 'x'))); return 0; });
    BOOST_CHECK_EQUAL(t.call([&] { return s->counters().dropped; }), 1u);
    BOOST_CHECK_EQUAL(t.call([&] { return s->counters().messages_out; }), 0u);
    BOOST_CHECK(holds([&] { return got->count() == 0; }, slow(milliseconds(100))));
    t.call([&] { s->send(makeBuf(std::string(100, 'y'))); return 0; });
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    t.call([&] { s->stop(); r->stop(); return 0; });
}

BOOST_AUTO_TEST_CASE(no_callback_after_stop)
{
    IoThread t;
    auto got = std::make_shared<Got>();
    auto r = startReceiver(t, "127.0.0.1", 0, {}, got);
    BOOST_REQUIRE(eventually([&] { return r->bound(); }));
    const uint16_t port = r->localPort();
    sendPlain("127.0.0.1", port, "one");
    BOOST_REQUIRE(eventually([&] { return got->count() == 1; }));
    t.call([&] { r->stop(); return 0; });
    BOOST_CHECK(!r->bound());
    sendPlain("127.0.0.1", port, "two");
    BOOST_CHECK(holds([&] { return got->count() == 1; }, slow(milliseconds(100))));

    // A callback that stops its receiver ends the loop at once.
    auto got2 = std::make_shared<Got>();
    std::atomic<int> calls{0};
    auto r2 = t.call([&] {
        auto x = UdpReceiver::create(t.io, "127.0.0.1", 0);
        x->onDatagram([&calls, wx = std::weak_ptr<UdpReceiver>(x)](const uint8_t*, std::size_t, const udp::endpoint&) {
            ++calls;
            if (auto p = wx.lock())
                p->stop();
        });
        x->start();
        return x;
    });
    const uint16_t port2 = r2->localPort();
    sendPlain("127.0.0.1", port2, "a");
    sendPlain("127.0.0.1", port2, "b");
    BOOST_REQUIRE(eventually([&] { return calls.load() >= 1; }));
    BOOST_CHECK(holds([&] { return calls.load() == 1; }, slow(milliseconds(100))));
}
