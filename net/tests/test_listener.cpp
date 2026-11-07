#define BOOST_TEST_MODULE net_listener
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/buffer.hpp"
#include "skycue/net/listener.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace skycue::net;
using namespace skycue::net::test;
using std::chrono::milliseconds;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

// A blocking-style test client: connects synchronously and reads with a
// poll() deadline so no test can hang.
struct Client {
    asio::io_context io;
    tcp::socket s{io};

    void connect(const tcp::endpoint& to, const asio::ip::address* from = nullptr)
    {
        s.open(to.protocol());
        if (from)
            s.bind(tcp::endpoint(*from, 0));
        s.connect(to);
    }
    void write(const std::string& d) { asio::write(s, asio::buffer(d)); }
    // Reads until `want` bytes arrived or the time ran out.
    std::string read(std::size_t want, milliseconds limit)
    {
        std::string out;
        const auto end = std::chrono::steady_clock::now() + limit;
        char tmp[4096];
        while (out.size() < want) {
            const auto left = std::chrono::duration_cast<milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                break;
            pollfd p{s.native_handle(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0)
                break;
            const ssize_t n = ::recv(s.native_handle(), tmp, sizeof tmp, 0);
            if (n <= 0)
                break;
            out.append(tmp, static_cast<std::size_t>(n));
        }
        return out;
    }
    // True when the server closed (or reset) the connection within the limit.
    bool closedByPeer(milliseconds limit)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        char tmp[4096];
        for (;;) {
            const auto left = std::chrono::duration_cast<milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                return false;
            pollfd p{s.native_handle(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0)
                return false;
            const ssize_t n = ::recv(s.native_handle(), tmp, sizeof tmp, 0);
            if (n <= 0)
                return true;
        }
    }
};

struct Rec {
    std::mutex m;
    std::vector<std::pair<uint64_t, bool>> peers;
    std::map<uint64_t, std::string> data;
    std::size_t peerEvents()
    {
        std::lock_guard<std::mutex> l(m);
        return peers.size();
    }
    std::size_t dataBytes()
    {
        std::lock_guard<std::mutex> l(m);
        std::size_t n = 0;
        for (auto& kv : data)
            n += kv.second.size();
        return n;
    }
    std::string dataOf(uint64_t id)
    {
        std::lock_guard<std::mutex> l(m);
        return data[id];
    }
};

void hook(Listener& l, const std::shared_ptr<Rec>& r)
{
    l.onData([r](uint64_t id, const uint8_t* p, std::size_t n) {
        std::lock_guard<std::mutex> g(r->m);
        r->data[id].append(reinterpret_cast<const char*>(p), n);
    });
    l.onPeer([r](uint64_t id, bool up, const tcp::endpoint&) {
        std::lock_guard<std::mutex> g(r->m);
        r->peers.emplace_back(id, up);
    });
}

tcp::endpoint loopback(uint16_t port) { return {asio::ip::make_address("127.0.0.1"), port}; }

std::shared_ptr<Listener> startListener(IoThread& t, uint16_t port, ListenOptions o, const std::shared_ptr<Rec>& r)
{
    return t.call([&] {
        auto l = Listener::create(t.io, "127.0.0.1", port, o);
        hook(*l, r);
        l->start();
        return l;
    });
}

} // namespace

BOOST_AUTO_TEST_CASE(tcp_listen_counts_peers_and_streams)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    auto l = startListener(t, port, {}, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    BOOST_CHECK_EQUAL(l->localPort(), port);

    Client c[3];
    for (int i = 0; i < 3; ++i) {
        c[i].connect(loopback(port));
        BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == static_cast<std::size_t>(i + 1); }));
    }
    for (int i = 0; i < 3; ++i)
        c[i].write("line-" + std::to_string(i + 1) + "\n");
    BOOST_REQUIRE(eventually([&] { return rec->dataBytes() == 3 * 7; }));
    {
        std::lock_guard<std::mutex> g(rec->m);
        BOOST_REQUIRE_EQUAL(rec->peers.size(), 3u);
        for (std::size_t i = 0; i < 3; ++i) {
            BOOST_CHECK_EQUAL(rec->peers[i].first, i + 1);
            BOOST_CHECK(rec->peers[i].second);
        }
    }
    for (uint64_t id = 1; id <= 3; ++id)
        BOOST_CHECK_EQUAL(rec->dataOf(id), "line-" + std::to_string(id) + "\n");
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 3u);
    BOOST_CHECK_EQUAL(t.call([&] { return l->counters().connects; }), 3u);

    c[1].s.close();
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 4; }));
    {
        std::lock_guard<std::mutex> g(rec->m);
        BOOST_CHECK_EQUAL(rec->peers[3].first, 2u);
        BOOST_CHECK(!rec->peers[3].second);
    }
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 2u);
    BOOST_CHECK_EQUAL(t.call([&] { return l->counters().peers; }), 2u);
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_peer_cap)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    ListenOptions o;
    o.max_peers = 2;
    auto l = startListener(t, port, o, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client a, b, c;
    a.connect(loopback(port));
    b.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 2; }));
    c.connect(loopback(port));
    BOOST_CHECK(c.closedByPeer(milliseconds(500)));
    BOOST_CHECK_EQUAL(t.call([&] { return l->counters().rejected_peer; }), 1u);
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 2u);
    BOOST_CHECK_EQUAL(rec->peerEvents(), 2u); // the rejected one is never reported
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_peer_filter)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    ListenOptions o;
    o.peer = "127.0.0.2";
    auto l = startListener(t, port, o, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client a;
    a.connect(loopback(port));
    BOOST_CHECK(a.closedByPeer(milliseconds(500)));
    BOOST_CHECK_EQUAL(t.call([&] { return l->counters().rejected_peer; }), 1u);
    BOOST_CHECK_EQUAL(rec->peerEvents(), 0u);

    Client b;
    const auto from = asio::ip::make_address("127.0.0.2");
    try {
        b.connect(loopback(port), &from);
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE(std::string("SKIP second half: ") + e.what());
        t.call([&] { l->stop(); });
        return;
    }
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 1u);
    BOOST_CHECK_EQUAL(t.call([&] { return l->counters().rejected_peer; }), 1u);
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_bind_failure_reported_and_retried)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    asio::io_context io2;
    auto blocker = std::make_unique<tcp::acceptor>(io2, loopback(0));
    const uint16_t port = blocker->local_endpoint().port();
    ListenOptions o;
    o.rebind_delay = milliseconds(50);
    auto l = startListener(t, port, o, rec);
    BOOST_REQUIRE(eventually([&] { return !t.call([&] { return l->counters().last_error; }).empty(); }));
    BOOST_CHECK(!l->listening());
    blocker.reset();
    BOOST_CHECK(eventually([&] { return l->listening(); }, milliseconds(1000) * testSlowFactor()));
    Client c;
    c.connect(loopback(port));
    BOOST_CHECK(eventually([&] { return rec->peerEvents() == 1; }));
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_fail_fast_throws)
{
    IoThread t;
    asio::io_context io2;
    tcp::acceptor blocker(io2, loopback(0));
    const uint16_t port = blocker.local_endpoint().port();
    ListenOptions o;
    o.fail_fast = true;
    auto l = Listener::create(t.io, "127.0.0.1", port, o);
    try {
        t.call([&] { l->start(); });
        BOOST_ERROR("start() did not throw");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string(e.what()).find(std::to_string(port)) != std::string::npos);
    }
    BOOST_CHECK(!l->listening());
}

BOOST_AUTO_TEST_CASE(stop_then_rebind_same_port)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    auto l = startListener(t, port, {}, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client c;
    c.connect(loopback(port)); // leaves a connection behind, as a real stop would
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    t.call([&] { l->stop(); });
    BOOST_CHECK(!l->listening());
    auto l2 = startListener(t, port, {}, std::make_shared<Rec>());
    BOOST_CHECK(eventually([&] { return l2->listening(); }, milliseconds(1000) * testSlowFactor()));
    t.call([&] { l2->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_send_and_sendall)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    auto l = startListener(t, port, {}, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client a, b;
    a.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    b.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 2; }));

    t.call([&] { l->send(1, makeBuf(std::string_view("hello"))); });
    BOOST_CHECK_EQUAL(a.read(5, slow(std::chrono::seconds(2))), "hello");
    BOOST_CHECK_EQUAL(b.read(1, milliseconds(200)), "");
    t.call([&] { l->sendAll(makeBuf(std::string_view("all"))); });
    BOOST_CHECK_EQUAL(a.read(3, slow(std::chrono::seconds(2))), "all");
    BOOST_CHECK_EQUAL(b.read(3, slow(std::chrono::seconds(2))), "all");
    t.call([&] { l->send(99, makeBuf(std::string_view("nobody"))); }); // unknown stream: ignored
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(tcp_listen_send_cap_closes_slow_peer)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    ListenOptions o;
    o.tcp.send_cap = 4096;
    auto l = startListener(t, port, o, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client slowc, good;
    slowc.connect(loopback(port)); // never reads
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    good.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 2; }));

    const std::string chunk(1000, 'x');
    const auto end = std::chrono::steady_clock::now() + slow(std::chrono::seconds(20));
    auto dropped = [&] { return t.call([&] { return l->counters().dropped; }); };
    while (dropped() == 0 && std::chrono::steady_clock::now() < end) {
        t.call([&] {
            for (int i = 0; i < 200; ++i)
                l->send(1, makeBuf(chunk));
        });
        std::this_thread::sleep_for(milliseconds(1));
    }
    BOOST_CHECK_GE(dropped(), 1u);
    BOOST_CHECK(eventually([&] { return t.call([&] { return l->peers(); }) == 1; }));
    BOOST_CHECK(eventually([&] { return rec->peerEvents() == 3; }));
    t.call([&] { l->sendAll(makeBuf(std::string_view("still"))); });
    BOOST_CHECK_EQUAL(good.read(5, slow(std::chrono::seconds(2))), "still");
    t.call([&] { l->stop(); });
}

BOOST_AUTO_TEST_CASE(no_callback_after_stop)
{
    IoThread t;
    auto rec = std::make_shared<Rec>();
    const uint16_t port = freeTcpPort();
    auto l = startListener(t, port, {}, rec);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client a, b;
    a.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    b.connect(loopback(port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 2; }));
    a.write("before\n");
    BOOST_REQUIRE(eventually([&] { return rec->dataBytes() == 7; }));

    // stop() with sends and a peer-down notification in flight.
    t.call([&] {
        l->sendAll(makeBuf(std::string_view("x")));
        l->stop();
    });
    const auto events = rec->peerEvents();
    const auto bytes = rec->dataBytes();
    try {
        a.write("after\n");
    } catch (const std::exception&) {
    }
    a.s.close();
    b.s.close();
    std::this_thread::sleep_for(milliseconds(300));
    BOOST_CHECK_EQUAL(rec->peerEvents(), events);
    BOOST_CHECK_EQUAL(rec->dataBytes(), bytes);
    BOOST_CHECK(!l->listening());
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 0u);
}

BOOST_AUTO_TEST_CASE(tcp_listen_ipv6_loopback)
{
    if (!haveIpv6Loopback()) {
        BOOST_TEST_MESSAGE("SKIP no IPv6 loopback");
        return;
    }
    IoThread t;
    auto rec = std::make_shared<Rec>();
    auto l = t.call([&] {
        auto x = Listener::create(t.io, "::1", 0);
        hook(*x, rec);
        x->start();
        return x;
    });
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    const uint16_t port = l->localPort();
    BOOST_REQUIRE(port != 0);
    Client c;
    c.connect(tcp::endpoint(asio::ip::make_address("::1"), port));
    BOOST_REQUIRE(eventually([&] { return rec->peerEvents() == 1; }));
    c.write("v6\n");
    BOOST_CHECK(eventually([&] { return rec->dataOf(1) == "v6\n"; }));
    t.call([&] { l->stop(); });
}
