#define BOOST_TEST_MODULE skycued_transports
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"
#include "inputs/input_def.hpp"
#include "inputs/tcp_connect_client.hpp"
#include "inputs/tcp_listener.hpp"
#include "inputs/transport.hpp"
#include "inputs/udp_listener.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

using namespace enginetest;
using asio::ip::udp;
using skycued::TcpConnectClient;
using skycued::TcpListener;
using skycued::Transport;
using skycued::TransportData;
using skycued::UdpListener;

namespace {

constexpr std::chrono::milliseconds kRebind{50};

// Everything a transport reported, filled on the io thread, read on the test thread.
struct Rec {
    std::mutex m;
    std::vector<std::pair<uint64_t, std::string>> data;  // stream, bytes
    std::vector<asio::ip::address> from;
    std::vector<uint64_t> closed;
    std::vector<bool> conn;

    void attach(Transport& t)
    {
        t.onData([this](const TransportData& d) {
            std::lock_guard<std::mutex> lock(m);
            data.emplace_back(d.stream, std::string(reinterpret_cast<const char*>(d.data), d.size));
            from.push_back(d.peer);
        });
        t.onStreamClosed([this](uint64_t s) {
            std::lock_guard<std::mutex> lock(m);
            closed.push_back(s);
        });
        t.onConnection([this](bool c) {
            std::lock_guard<std::mutex> lock(m);
            conn.push_back(c);
        });
    }
    template <class F> auto with(F f) -> decltype(f())
    {
        std::lock_guard<std::mutex> lock(m);
        return f();
    }
    // Concatenated bytes of every onData so far.
    std::string all()
    {
        std::lock_guard<std::mutex> lock(m);
        std::string s;
        for (const auto& d : data)
            s += d.second;
        return s;
    }
};

// Polls `pred` until it holds or kWait passes.
template <class F> bool eventually(F pred)
{
    const auto until = steady::now() + kWait;
    while (steady::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// A port that is free right now (bound to 0 and released). Nothing connected
// to it, so a TCP port has no TIME_WAIT either.
uint16_t freeUdpPort()
{
    asio::io_context io;
    udp::socket s(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return s.local_endpoint().port();
}
uint16_t freeTcpPort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

void sendFrom(const std::string& from, uint16_t port, const std::string& payload)
{
    asio::io_context io;
    udp::socket s(io, udp::endpoint(asio::ip::make_address(from), 0));
    s.send_to(asio::buffer(payload), udp::endpoint(asio::ip::make_address("127.0.0.1"), port));
}

// Blocks the io thread until the returned promise is set, so work queues up.
std::shared_ptr<std::promise<void>> blockIo(IoThread& t)
{
    auto release = std::make_shared<std::promise<void>>();
    auto blocked = std::make_shared<std::promise<void>>();
    auto fut = release->get_future().share();
    asio::post(t.io, [fut, blocked] {
        blocked->set_value();
        fut.wait_for(kWait);
    });
    blocked->get_future().wait();
    return release;
}

// True when the peer closed `s` within kWait (read returns EOF or an error).
bool closedByPeer(tcp::socket& s)
{
    pollfd p{s.native_handle(), POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(std::chrono::milliseconds(kWait).count())) != 1)
        return false;
    char c;
    boost::system::error_code ec;
    const auto n = s.read_some(asio::buffer(&c, 1), ec);
    return n == 0 && ec;
}

tcp::socket connectTo(asio::io_context& io, uint16_t port)
{
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    return s;
}

// The fd in this process whose local end is `local` and remote end `remote`:
// the transport's side of a connection the test opened.
int findSocket(const tcp::endpoint& local, const tcp::endpoint& remote)
{
    for (int fd = 0; fd < 4096; ++fd) {
        sockaddr_in a{}, b{};
        socklen_t la = sizeof a, lb = sizeof b;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &la) != 0 || a.sin_family != AF_INET)
            continue;
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&b), &lb) != 0)
            continue;
        if (ntohs(a.sin_port) == local.port() && ntohs(b.sin_port) == remote.port() &&
            ntohl(a.sin_addr.s_addr) == local.address().to_v4().to_uint() &&
            ntohl(b.sin_addr.s_addr) == remote.address().to_v4().to_uint())
            return fd;
    }
    return -1;
}

int sockOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof v;
    if (::getsockopt(fd, level, name, &v, &len) != 0)
        return -1;
    return v;
}

void checkKeepalive(int fd)
{
    BOOST_REQUIRE_GE(fd, 0);
    BOOST_CHECK_EQUAL(sockOpt(fd, SOL_SOCKET, SO_KEEPALIVE), 1);
#ifdef __linux__
    BOOST_CHECK_EQUAL(sockOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE), 30);
    BOOST_CHECK_EQUAL(sockOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL), 10);
    BOOST_CHECK_EQUAL(sockOpt(fd, IPPROTO_TCP, TCP_KEEPCNT), 3);
#endif
}

}  // namespace

BOOST_AUTO_TEST_CASE(udp_delivers_datagrams_and_peer)
{
    IoThread t;
    Rec rec;
    const uint16_t port = freeUdpPort();
    std::unique_ptr<UdpListener> l;
    t.call([&] {
        l = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "", kRebind);
        rec.attach(*l);
        l->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->connected(); }); }));
    BOOST_CHECK(t.call([&] { return l->datagrams(); }));

    sendFrom("127.0.0.1", port, "a");
    sendFrom("127.0.0.1", port, std::string(1000, 'b'));
    sendFrom("127.0.0.1", port, std::string(60000, 'c'));
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.data.size() == 3; }); }));

    rec.with([&] {
        BOOST_CHECK_EQUAL(rec.data[0].second, "a");
        BOOST_CHECK_EQUAL(rec.data[1].second.size(), 1000u);
        BOOST_CHECK_EQUAL(rec.data[2].second.size(), 60000u);
        for (const auto& d : rec.data)
            BOOST_CHECK_EQUAL(d.first, 0u);
        for (const auto& a : rec.from)
            BOOST_CHECK_EQUAL(a.to_string(), "127.0.0.1");
        BOOST_CHECK(rec.conn == std::vector<bool>{true});
    });
    t.call([&] {
        BOOST_CHECK_EQUAL(l->bytes(), 61001u);
        BOOST_CHECK_EQUAL(l->connects(), 1u);
        BOOST_CHECK_EQUAL(l->rejectedPeer(), 0u);
        l.reset();
    });
}

BOOST_AUTO_TEST_CASE(udp_peer_filter)
{
    IoThread t;
    Rec rec;
    const uint16_t port = freeUdpPort();
    std::unique_ptr<UdpListener> l;
    t.call([&] {
        l = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "127.0.0.2", kRebind);
        rec.attach(*l);
        l->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->connected(); }); }));

    sendFrom("127.0.0.1", port, "stranger");
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->rejectedPeer() == 1; }); }));
    sendFrom("127.0.0.2", port, "device");
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.data.size() == 1; }); }));

    rec.with([&] {
        BOOST_CHECK_EQUAL(rec.data[0].second, "device");
        BOOST_CHECK_EQUAL(rec.from[0].to_string(), "127.0.0.2");
    });
    t.call([&] {
        BOOST_CHECK_EQUAL(l->rejectedPeer(), 1u);
        BOOST_CHECK_EQUAL(l->bytes(), 6u);
        BOOST_CHECK_EQUAL(l->lastError(), "datagram from 127.0.0.1 refused (not the configured peer)");
    });
    // Noted at most once a second: a second refusal right after leaves the
    // message as it was (it is the same text either way; the count goes on).
    sendFrom("127.0.0.1", port, "stranger");
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->rejectedPeer() == 2; }); }));
    t.call([&] { l.reset(); });
}

BOOST_AUTO_TEST_CASE(udp_bind_failure_reported_and_retried)
{
    IoThread t;
    Rec rec;
    asio::io_context tio;
    auto blocker = std::make_unique<udp::socket>(tio, udp::endpoint(asio::ip::make_address("0.0.0.0"), 0));
    const uint16_t port = blocker->local_endpoint().port();
    std::unique_ptr<UdpListener> l;
    t.call([&] {
        l = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "", kRebind);
        rec.attach(*l);
        l->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return !l->lastError().empty(); }); }));
    t.call([&] {
        BOOST_CHECK(!l->connected());
        BOOST_CHECK_EQUAL(l->lastError(), "bind 0.0.0.0:" + std::to_string(port) + ": Address already in use");
    });

    blocker.reset();
    const auto freed = steady::now();
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->connected(); }); }));
    BOOST_CHECK(steady::now() - freed < 1s);
    t.call([&] {
        BOOST_CHECK_EQUAL(l->connects(), 1u);
        l.reset();
    });
    rec.with([&] { BOOST_CHECK(rec.conn == std::vector<bool>{true}); });
}

BOOST_AUTO_TEST_CASE(tcp_connect_reconnects_with_backoff)
{
    IoThread t;
    Rec rec;
    asio::io_context sio;
    tcp::acceptor server(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = server.local_endpoint().port();
    std::unique_ptr<TcpConnectClient> c;
    t.call([&] {
        c = std::make_unique<TcpConnectClient>(t.io, "127.0.0.1", port);
        rec.attach(*c);
        c->start();
    });

    {
        tcp::socket s(sio);
        server.accept(s);
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return c->connected(); }); }));
        BOOST_CHECK_EQUAL(t.call([&] { return c->peers(); }), 1u);
    }  // closed by the server
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.conn.size() == 2; }); }));
    BOOST_CHECK(!t.call([&] { return c->connected(); }));

    tcp::socket s2(sio);
    server.accept(s2);
    asio::write(s2, asio::buffer(std::string("x")));
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.data.size() == 1; }); }));

    rec.with([&] {
        BOOST_CHECK((rec.conn == std::vector<bool>{true, false, true}));
        BOOST_CHECK(rec.closed == std::vector<uint64_t>{1});
        BOOST_CHECK_EQUAL(rec.data[0].first, 2u);  // the second connection
    });
    t.call([&] {
        BOOST_CHECK_EQUAL(c->connects(), 2u);
        BOOST_CHECK(!c->datagrams());
        c.reset();
    });
}

BOOST_AUTO_TEST_CASE(tcp_connect_delivers_chunks_in_order)
{
    IoThread t;
    Rec rec;
    asio::io_context sio;
    tcp::acceptor server(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t port = server.local_endpoint().port();
    std::unique_ptr<TcpConnectClient> c;
    t.call([&] {
        c = std::make_unique<TcpConnectClient>(t.io, "127.0.0.1", port);
        rec.attach(*c);
        c->start();
    });
    tcp::socket s(sio);
    server.accept(s);

    std::string sent;
    for (int i = 0; i < 200; ++i) {
        std::string chunk(static_cast<std::size_t>(1 + (i * 37) % 3000), static_cast<char>('A' + i % 26));
        chunk += std::to_string(i);
        asio::write(s, asio::buffer(chunk));
        sent += chunk;
    }
    BOOST_REQUIRE(eventually([&] { return rec.all().size() == sent.size(); }));
    BOOST_CHECK(rec.all() == sent);
    rec.with([&] {
        for (const auto& d : rec.data) {
            BOOST_CHECK_EQUAL(d.first, 1u);
            BOOST_CHECK_LE(d.second.size(), 16384u);
        }
    });
    t.call([&] {
        BOOST_CHECK_EQUAL(c->bytes(), sent.size());
        c.reset();
    });
}

BOOST_AUTO_TEST_CASE(tcp_listen_counts_peers_and_streams)
{
    IoThread t;
    Rec rec;
    const uint16_t port = freeTcpPort();
    std::unique_ptr<TcpListener> l;
    t.call([&] {
        l = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
        rec.attach(*l);
        l->start();
    });
    asio::io_context cio;
    tcp::socket a(cio), b(cio);
    BOOST_REQUIRE(eventually([&] {
        boost::system::error_code ec;
        a.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }));
    b = connectTo(cio, port);
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers() == 2; }); }));
    BOOST_CHECK(t.call([&] { return l->connected(); }));

    asio::write(a, asio::buffer(std::string("from-a")));
    asio::write(b, asio::buffer(std::string("from-b")));
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.data.size() == 2; }); }));
    uint64_t stream_a = 0, stream_b = 0;
    rec.with([&] {
        for (const auto& d : rec.data)
            (d.second == "from-a" ? stream_a : stream_b) = d.first;
    });
    BOOST_CHECK_NE(stream_a, 0u);
    BOOST_CHECK_NE(stream_b, 0u);
    BOOST_CHECK_NE(stream_a, stream_b);

    a.close();
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.closed.size() == 1; }); }));
    rec.with([&] { BOOST_CHECK_EQUAL(rec.closed[0], stream_a); });
    BOOST_CHECK_EQUAL(t.call([&] { return l->peers(); }), 1u);
    rec.with([&] { BOOST_CHECK(rec.conn == std::vector<bool>{true}); });

    b.close();
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.conn.size() == 2; }); }));
    rec.with([&] {
        BOOST_CHECK((rec.conn == std::vector<bool>{true, false}));
        BOOST_CHECK((rec.closed == std::vector<uint64_t>{stream_a, stream_b}));
    });
    t.call([&] {
        BOOST_CHECK(!l->connected());
        BOOST_CHECK_EQUAL(l->connects(), 2u);
        BOOST_CHECK_EQUAL(l->bytes(), 12u);
        l.reset();
    });
}

BOOST_AUTO_TEST_CASE(tcp_listen_peer_cap)
{
    IoThread t;
    const uint16_t port = freeTcpPort();
    std::unique_ptr<TcpListener> l;
    t.call([&] {
        l = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
        l->start();
    });
    asio::io_context cio;
    std::vector<tcp::socket> clients;
    BOOST_REQUIRE(eventually([&] {
        tcp::socket s(cio);
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec)
            clients.push_back(std::move(s));
        return !ec;
    }));
    for (std::size_t i = 1; i < TcpListener::kMaxPeers; ++i)
        clients.push_back(connectTo(cio, port));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers() == TcpListener::kMaxPeers; }); }));

    tcp::socket extra = connectTo(cio, port);
    BOOST_CHECK(closedByPeer(extra));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->rejectedPeer() == 1; }); }));
    t.call([&] {
        BOOST_CHECK_EQUAL(l->peers(), TcpListener::kMaxPeers);
        BOOST_CHECK_EQUAL(l->connects(), TcpListener::kMaxPeers);
        BOOST_CHECK(l->lastError().find("16") != std::string::npos);
        l.reset();
    });
}

BOOST_AUTO_TEST_CASE(tcp_listen_peer_filter)
{
    IoThread t;
    Rec rec;
    const uint16_t port = freeTcpPort();
    std::unique_ptr<TcpListener> l;
    t.call([&] {
        l = std::make_unique<TcpListener>(t.io, "0.0.0.0", port, "127.0.0.2", kRebind);
        rec.attach(*l);
        l->start();
    });
    asio::io_context cio;
    tcp::socket stranger(cio);
    BOOST_REQUIRE(eventually([&] {
        boost::system::error_code ec;
        stranger.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }));
    BOOST_CHECK(closedByPeer(stranger));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->rejectedPeer() == 1; }); }));

    tcp::socket device(cio);
    device.open(tcp::v4());
    device.bind(tcp::endpoint(asio::ip::make_address("127.0.0.2"), 0));
    device.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    asio::write(device, asio::buffer(std::string("ok")));
    BOOST_REQUIRE(eventually([&] { return rec.with([&] { return rec.data.size() == 1; }); }));
    rec.with([&] { BOOST_CHECK_EQUAL(rec.from[0].to_string(), "127.0.0.2"); });
    t.call([&] {
        BOOST_CHECK_EQUAL(l->peers(), 1u);
        BOOST_CHECK_EQUAL(l->connects(), 1u);
        l.reset();
    });
}

BOOST_AUTO_TEST_CASE(no_callback_after_stop)
{
    // UDP: five datagrams queued, the first handler stops the listener.
    {
        IoThread t;
        const uint16_t port = freeUdpPort();
        std::unique_ptr<UdpListener> l;
        int data = 0, closed = 0, conn = 0;
        t.call([&] {
            l = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "", kRebind);
            l->onData([&](const TransportData&) {
                ++data;
                l->stop();
                l->stop();  // idempotent
            });
            l->onStreamClosed([&](uint64_t) { ++closed; });
            l->onConnection([&](bool) { ++conn; });
            l->start();
        });
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->connected(); }); }));
        auto release = blockIo(t);
        for (int i = 0; i < 5; ++i)
            sendFrom("127.0.0.1", port, "d" + std::to_string(i));
        release->set_value();
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return data > 0; }); }));
        std::this_thread::sleep_for(kRebind);
        t.call([&] {
            BOOST_CHECK(!l->connected());
            l.reset();
        });
        t.call([] {});
        t.call([&] {
            BOOST_CHECK_EQUAL(data, 1);
            BOOST_CHECK_EQUAL(closed, 0);
            BOOST_CHECK_EQUAL(conn, 1);
        });
    }
    // TcpConnectClient: data and EOF queued, the data handler stops the client.
    {
        IoThread t;
        asio::io_context sio;
        tcp::acceptor server(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        std::unique_ptr<TcpConnectClient> c;
        int data = 0, closed = 0, conn = 0;
        t.call([&] {
            c = std::make_unique<TcpConnectClient>(t.io, "127.0.0.1", server.local_endpoint().port());
            c->onData([&](const TransportData&) {
                ++data;
                c->stop();
            });
            c->onStreamClosed([&](uint64_t) { ++closed; });
            c->onConnection([&](bool) { ++conn; });
            c->start();
        });
        {
            tcp::socket s(sio);
            server.accept(s);
            BOOST_REQUIRE(eventually([&] { return t.call([&] { return c->connected(); }); }));
            auto release = blockIo(t);
            asio::write(s, asio::buffer(std::string("payload")));
            s.close();
            release->set_value();
        }
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return data > 0; }); }));
        std::this_thread::sleep_for(kRebind);
        t.call([&] { c.reset(); });
        t.call([] {});
        t.call([&] {
            BOOST_CHECK_EQUAL(data, 1);
            BOOST_CHECK_EQUAL(closed, 0);
            BOOST_CHECK_EQUAL(conn, 1);
        });
    }
    // TcpListener: the same with a pushing peer.
    {
        IoThread t;
        const uint16_t port = freeTcpPort();
        std::unique_ptr<TcpListener> l;
        int data = 0, closed = 0, conn = 0;
        t.call([&] {
            l = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
            l->onData([&](const TransportData&) {
                ++data;
                l->stop();
            });
            l->onStreamClosed([&](uint64_t) { ++closed; });
            l->onConnection([&](bool) { ++conn; });
            l->start();
        });
        asio::io_context cio;
        tcp::socket s(cio);
        BOOST_REQUIRE(eventually([&] {
            boost::system::error_code ec;
            s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
            return !ec;
        }));
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers() == 1; }); }));
        auto release = blockIo(t);
        asio::write(s, asio::buffer(std::string("payload")));
        s.close();
        release->set_value();
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return data > 0; }); }));
        std::this_thread::sleep_for(kRebind);
        t.call([&] {
            BOOST_CHECK_EQUAL(l->peers(), 0u);
            l.reset();
        });
        t.call([] {});
        t.call([&] {
            BOOST_CHECK_EQUAL(data, 1);
            BOOST_CHECK_EQUAL(closed, 0);
            BOOST_CHECK_EQUAL(conn, 1);
        });
    }
}

BOOST_AUTO_TEST_CASE(stop_then_rebind_same_port)
{
    IoThread t;
    const uint16_t port = freeUdpPort();
    std::unique_ptr<UdpListener> first, second;
    t.call([&] {
        first = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "", kRebind);
        first->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return first->connected(); }); }));
    t.call([&] {
        first->stop();
        second = std::make_unique<UdpListener>(t.io, "0.0.0.0", port, "", kRebind);
        second->start();
    });
    // Bound by the first attempt: no error, no rebind delay needed.
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return second->connected(); }); }));
    t.call([&] {
        BOOST_CHECK_EQUAL(second->lastError(), "");
        first.reset();
        second.reset();
    });
}

BOOST_AUTO_TEST_CASE(make_transport_by_type)
{
    IoThread t;
    t.call([&] {
        skycued::InputDef d;
        d.type = skycued::InputType::pingstation_json;
        d.transport = skycued::InputTransport::udp;
        d.port = 1;
        auto u = skycued::makeTransport(t.io, d);
        BOOST_CHECK(dynamic_cast<UdpListener*>(u.get()));

        d.type = skycued::InputType::pingstation_cat021;
        d.transport = skycued::InputTransport::tcp_connect;
        d.host = "127.0.0.1";
        BOOST_CHECK(dynamic_cast<TcpConnectClient*>(skycued::makeTransport(t.io, d).get()));
        d.transport = skycued::InputTransport::tcp_listen;
        BOOST_CHECK(dynamic_cast<TcpListener*>(skycued::makeTransport(t.io, d).get()));
        d.transport = skycued::InputTransport::udp;
        BOOST_CHECK(dynamic_cast<UdpListener*>(skycued::makeTransport(t.io, d).get()));

        d.type = skycued::InputType::readsb;
        d.transport = skycued::InputTransport::tcp_connect;
        BOOST_CHECK_THROW(skycued::makeTransport(t.io, d), std::invalid_argument);
    });
}

BOOST_AUTO_TEST_CASE(tcp_sockets_use_keepalive)
{
    IoThread t;
    // Accepted by a TcpListener.
    {
        const uint16_t port = freeTcpPort();
        std::unique_ptr<TcpListener> l;
        t.call([&] {
            l = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
            l->start();
        });
        asio::io_context cio;
        tcp::socket s(cio);
        BOOST_REQUIRE(eventually([&] {
            boost::system::error_code ec;
            s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
            return !ec;
        }));
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers() == 1; }); }));
        t.call([&] { checkKeepalive(findSocket(s.remote_endpoint(), s.local_endpoint())); });
        t.call([&] { l.reset(); });
    }
    // Connected by a TcpConnectClient.
    {
        asio::io_context sio;
        tcp::acceptor server(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        std::unique_ptr<TcpConnectClient> c;
        t.call([&] {
            c = std::make_unique<TcpConnectClient>(t.io, "127.0.0.1", server.local_endpoint().port());
            c->start();
        });
        tcp::socket s(sio);
        server.accept(s);
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return c->connected(); }); }));
        t.call([&] { checkKeepalive(findSocket(s.remote_endpoint(), s.local_endpoint())); });
        t.call([&] { c.reset(); });
    }
}

BOOST_AUTO_TEST_CASE(tcp_listen_rebinds_after_live_peers)
{
    IoThread t;
    const uint16_t port = freeTcpPort();
    std::unique_ptr<TcpListener> first, second;
    t.call([&] {
        first = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
        first->start();
    });
    asio::io_context cio;
    tcp::socket s(cio);
    BOOST_REQUIRE(eventually([&] {
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return first->peers() == 1; }); }));
    // The daemon closes first, so its end of the connection sits in TIME_WAIT.
    t.call([&] { first->stop(); });
    BOOST_CHECK(closedByPeer(s));
    s.close();
    t.call([&] {
        second = std::make_unique<TcpListener>(t.io, "127.0.0.1", port, "", kRebind);
        second->start();
    });
    tcp::socket s2(cio);
    BOOST_REQUIRE(eventually([&] {
        boost::system::error_code ec;
        s2.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return second->peers() == 1; }); }));
    t.call([&] {
        BOOST_CHECK_EQUAL(second->lastError(), "");
        first.reset();
        second.reset();
    });
}

BOOST_AUTO_TEST_CASE(tcp_listen_bind_failure_reported_and_retried)
{
    IoThread t;
    Rec rec;
    asio::io_context tio;
    auto blocker = std::make_unique<tcp::acceptor>(tio, tcp::endpoint(asio::ip::make_address("0.0.0.0"), 0));
    const uint16_t port = blocker->local_endpoint().port();
    std::unique_ptr<TcpListener> l;
    t.call([&] {
        l = std::make_unique<TcpListener>(t.io, "0.0.0.0", port, "", kRebind);
        rec.attach(*l);
        l->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return !l->lastError().empty(); }); }));
    t.call([&] {
        BOOST_CHECK(!l->connected());
        BOOST_CHECK_EQUAL(l->lastError(), "bind 0.0.0.0:" + std::to_string(port) + ": Address already in use");
    });

    blocker.reset();
    const auto freed = steady::now();
    asio::io_context cio;
    tcp::socket s(cio);
    BOOST_REQUIRE(eventually([&] {
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }));
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->connected(); }); }));
    BOOST_CHECK(steady::now() - freed < 1s);
    t.call([&] {
        BOOST_CHECK_EQUAL(l->connects(), 1u);
        l.reset();
    });
    rec.with([&] { BOOST_CHECK(rec.conn == std::vector<bool>{true}); });
}
