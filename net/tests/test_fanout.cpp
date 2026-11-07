#define BOOST_TEST_MODULE net_fanout
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/buffer.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/fanout.hpp"
#include "skycue/net/listener.hpp"
#include "skycue/net/udp_receiver.hpp"
#include "skycue/net/udp_sender.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace skycue::net;
using namespace skycue::net::test;
using std::chrono::milliseconds;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

// A plain blocking client; reads with a poll() deadline so no test can hang.
struct Client {
    asio::io_context io;
    tcp::socket s{io};

    void connect(uint16_t port, int rcvbuf = 0)
    {
        const tcp::endpoint to(asio::ip::make_address("127.0.0.1"), port);
        s.open(to.protocol());
        if (rcvbuf > 0)
            s.set_option(asio::socket_base::receive_buffer_size(rcvbuf));
        s.connect(to);
    }
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
};

// Collects what a test TCP server read.
struct Sink {
    std::mutex m;
    std::string data;
    std::size_t size()
    {
        std::lock_guard<std::mutex> l(m);
        return data.size();
    }
    std::string get()
    {
        std::lock_guard<std::mutex> l(m);
        return data;
    }
};

// Accepts connections on a loopback port and appends all it reads to a Sink.
class Server : public std::enable_shared_from_this<Server> {
public:
    static std::shared_ptr<Server> create(asio::io_context& io, uint16_t port, std::shared_ptr<Sink> sink)
    {
        std::shared_ptr<Server> s(new Server(io, port, std::move(sink)));
        s->accept();
        return s;
    }
    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    void stop()
    {
        boost::system::error_code ec;
        acceptor_.close(ec);
        if (sock_)
            sock_->close(ec);
    }

private:
    Server(asio::io_context& io, uint16_t port, std::shared_ptr<Sink> sink)
        : io_(io), acceptor_(io, {asio::ip::make_address("127.0.0.1"), port}), sink_(std::move(sink))
    {
    }
    void accept()
    {
        auto s = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*s, [self = shared_from_this(), s](const boost::system::error_code& ec) {
            if (ec)
                return;
            self->sock_ = s;
            self->read(s);
            self->accept();
        });
    }
    void read(std::shared_ptr<tcp::socket> s)
    {
        auto buf = std::make_shared<std::vector<char>>(4096);
        s->async_read_some(asio::buffer(*buf), [self = shared_from_this(), s, buf](auto ec, std::size_t n) {
            if (ec)
                return;
            {
                std::lock_guard<std::mutex> l(self->sink_->m);
                self->sink_->data.append(buf->data(), n);
            }
            self->read(s);
        });
    }
    asio::io_context& io_;
    tcp::acceptor acceptor_;
    std::shared_ptr<Sink> sink_;
    std::shared_ptr<tcp::socket> sock_;
};

struct Datagrams {
    std::mutex m;
    std::vector<std::string> items;
    std::size_t count()
    {
        std::lock_guard<std::mutex> l(m);
        return items.size();
    }
    std::string at(std::size_t i)
    {
        std::lock_guard<std::mutex> l(m);
        return items.at(i);
    }
};

std::shared_ptr<UdpReceiver> startUdpReceiver(IoThread& t, const std::shared_ptr<Datagrams>& g)
{
    return t.call([&] {
        auto r = UdpReceiver::create(t.io, "127.0.0.1", 0, {});
        r->onDatagram([g](const uint8_t* p, std::size_t n, const asio::ip::udp::endpoint&) {
            std::lock_guard<std::mutex> l(g->m);
            g->items.emplace_back(reinterpret_cast<const char*>(p), n);
        });
        r->start();
        return r;
    });
}

std::shared_ptr<UdpSender> udpSenderTo(IoThread& t, uint16_t port)
{
    return t.call([&] {
        auto u = UdpSender::create(t.io);
        UdpTarget tg;
        tg.host = "127.0.0.1";
        tg.port = port;
        u->addTarget(tg);
        u->start();
        return u;
    });
}

std::shared_ptr<Listener> startListener(IoThread& t, ListenOptions o = {})
{
    return t.call([&] {
        auto l = Listener::create(t.io, "127.0.0.1", 0, o);
        l->start();
        return l;
    });
}

} // namespace

BOOST_AUTO_TEST_CASE(fanout_reaches_listener_peers_dialer_and_udp)
{
    IoThread t;
    auto l = startListener(t);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Client a, b;
    a.connect(l->localPort());
    b.connect(l->localPort());
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers(); }) == 2; }));

    auto sink = std::make_shared<Sink>();
    auto srv = t.call([&] { return Server::create(t.io, 0, sink); });
    auto d = t.call([&] {
        auto x = Dialer::create(t.io, "127.0.0.1", srv->port());
        x->start();
        return x;
    });
    BOOST_REQUIRE(eventually([&] { return d->connected(); }));

    auto dg = std::make_shared<Datagrams>();
    auto rx = startUdpReceiver(t, dg);
    BOOST_REQUIRE(eventually([&] { return rx->bound(); }));
    auto u = udpSenderTo(t, rx->localPort());

    Fanout f(t.io);
    t.call([&] {
        f.attach(l);
        f.attach(d);
        f.attach(u);
        f.attach(u); // twice: ignored
    });
    BOOST_CHECK_EQUAL(t.call([&] { return f.destinations(); }), 3u);
    t.call([&] {
        f.publish(makeBuf(std::string_view("one;")));
        f.publish(makeBuf(std::string_view("two;")));
        f.publish(makeBuf(std::string_view("three;")));
    });
    const std::string all = "one;two;three;";
    BOOST_CHECK_EQUAL(a.read(all.size(), slow(std::chrono::seconds(2))), all);
    BOOST_CHECK_EQUAL(b.read(all.size(), slow(std::chrono::seconds(2))), all);
    BOOST_REQUIRE(eventually([&] { return sink->size() == all.size(); }));
    BOOST_CHECK_EQUAL(sink->get(), all);
    BOOST_REQUIRE(eventually([&] { return dg->count() == 3; }));
    BOOST_CHECK_EQUAL(dg->at(0), "one;");
    BOOST_CHECK_EQUAL(dg->at(1), "two;");
    BOOST_CHECK_EQUAL(dg->at(2), "three;");

    // Two TCP peers, one dialer, one UDP target: four sinks, three messages each.
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return f.counters().messages_out; }) == 4u * 3u; }));
    const Counters c = t.call([&] { return f.counters(); });
    BOOST_CHECK_EQUAL(c.bytes_out, 4u * all.size());
    BOOST_CHECK_EQUAL(c.dropped, 0u);
    BOOST_CHECK_EQUAL(c.peers, 2u);

    t.call([&] {
        f.detach(l);
        f.detach(d);
        f.detach(u);
        l->stop();
        d->stop();
        u->stop();
        rx->stop();
        srv->stop();
    });
}

BOOST_AUTO_TEST_CASE(fanout_slow_peer_dropped_others_continue)
{
    IoThread t;
    ListenOptions o;
    o.tcp.send_cap = 4096;
    auto l = startListener(t, o);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Fanout f(t.io);
    t.call([&] { f.attach(l); });
    Client slowc, good;
    slowc.connect(l->localPort(), 4096); // never reads, small receive window
    good.connect(l->localPort());
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers(); }) == 2; }));

    // The kernel buffers swallow a lot on loopback, so publish small buffers
    // one at a time (the good client drains between them) until the slow
    // peer's queue passes the cap.
    const std::string chunk(1000, 'x');
    std::size_t published = 0;
    const auto end = std::chrono::steady_clock::now() + slow(std::chrono::seconds(20));
    auto dropped = [&] { return t.call([&] { return f.counters().dropped; }); };
    while (dropped() == 0 && std::chrono::steady_clock::now() < end) {
        t.call([&] { f.publish(makeBuf(chunk)); });
        ++published;
        std::this_thread::sleep_for(milliseconds(1));
    }
    BOOST_CHECK_GE(dropped(), 1u);
    BOOST_CHECK(eventually([&] { return t.call([&] { return l->peers(); }) == 1; }));
    BOOST_CHECK_EQUAL(good.read(published * chunk.size(), slow(std::chrono::seconds(5))).size(),
                      published * chunk.size());
    t.call([&] { f.publish(makeBuf(std::string_view("tail"))); });
    BOOST_CHECK_EQUAL(good.read(4, slow(std::chrono::seconds(2))), "tail");
    t.call([&] { f.detach(l); l->stop(); });
}

BOOST_AUTO_TEST_CASE(fanout_drops_while_dialer_down)
{
    IoThread t;
    TcpOptions to;
    to.min_backoff = milliseconds(20);
    to.max_backoff = milliseconds(50);
    const uint16_t port = freeTcpPort();
    auto d = t.call([&] {
        auto x = Dialer::create(t.io, "127.0.0.1", port, to);
        x->start();
        return x;
    });
    Fanout f(t.io);
    t.call([&] { f.attach(d); });
    t.call([&] {
        for (int i = 0; i < 5; ++i)
            f.publish(makeBuf(std::string_view("lost")));
    });
    BOOST_CHECK_EQUAL(t.call([&] { return f.counters().dropped; }), 5u);

    auto sink = std::make_shared<Sink>();
    auto srv = t.call([&] { return Server::create(t.io, port, sink); });
    BOOST_REQUIRE(eventually([&] { return d->connected(); }));
    t.call([&] { f.publish(makeBuf(std::string_view("found"))); });
    BOOST_REQUIRE(eventually([&] { return sink->size() == 5; }));
    BOOST_CHECK_EQUAL(sink->get(), "found");
    BOOST_CHECK_EQUAL(t.call([&] { return f.counters().dropped; }), 5u);
    t.call([&] { f.detach(d); d->stop(); srv->stop(); });
}

BOOST_AUTO_TEST_CASE(fanout_detach_stops_delivery)
{
    IoThread t;
    auto dg = std::make_shared<Datagrams>();
    auto rx = startUdpReceiver(t, dg);
    BOOST_REQUIRE(eventually([&] { return rx->bound(); }));
    auto u = udpSenderTo(t, rx->localPort());
    Fanout f(t.io);
    t.call([&] { f.attach(u); });
    t.call([&] { f.publish(makeBuf(std::string_view("before"))); });
    BOOST_REQUIRE(eventually([&] { return dg->count() == 1; }));
    t.call([&] { f.detach(u); });
    BOOST_CHECK_EQUAL(t.call([&] { return f.destinations(); }), 0u);
    t.call([&] { f.publish(makeBuf(std::string_view("after"))); });
    std::this_thread::sleep_for(milliseconds(200));
    BOOST_CHECK_EQUAL(dg->count(), 1u);
    BOOST_CHECK_EQUAL(t.call([&] { return u->counters().messages_out; }), 1u);
    t.call([&] { u->stop(); rx->stop(); });
}

BOOST_AUTO_TEST_CASE(fanout_peer_up_hook_can_greet)
{
    IoThread t;
    auto l = startListener(t);
    BOOST_REQUIRE(eventually([&] { return l->listening(); }));
    Fanout f(t.io);
    auto downs = std::make_shared<std::atomic<int>>(0);
    t.call([&] {
        f.attach(l);
        f.onPeerUp([](const std::function<void(Buf)>& to) { to(makeBuf(std::string_view("hello\n"))); });
        f.onPeerDown([downs](uint64_t) { ++*downs; });
    });
    {
        Client c;
        c.connect(l->localPort());
        BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers(); }) == 1; }));
        t.call([&] { f.publish(makeBuf(std::string_view("data\n"))); });
        BOOST_CHECK_EQUAL(c.read(11, slow(std::chrono::seconds(2))), "hello\ndata\n");
    }
    BOOST_CHECK(eventually([&] { return downs->load() == 1; }));
    // Detached: no hook call for new peers any more.
    t.call([&] { f.detach(l); });
    Client d;
    d.connect(l->localPort());
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return l->peers(); }) == 1; }));
    BOOST_CHECK_EQUAL(d.read(1, milliseconds(200)), "");
    t.call([&] { l->stop(); });
}
