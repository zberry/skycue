#define BOOST_TEST_MODULE net_dialer
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/buffer.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/tcp_options.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace skycue::net;
using std::chrono::milliseconds;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

// Accepts connections on 127.0.0.1:0 on the io thread it is given, records
// what the latest client sends, can write to it and close it. Never blocks.
class TestServer : public std::enable_shared_from_this<TestServer> {
public:
    static std::shared_ptr<TestServer> create(asio::io_context& io, uint16_t port = 0, bool read = true)
    {
        std::shared_ptr<TestServer> s(new TestServer(io, port, read));
        s->accept();
        return s;
    }
    uint16_t port() const { return port_; }
    int accepted() const { return accepted_; }
    std::string received() const
    {
        std::lock_guard<std::mutex> l(m_);
        return received_;
    }
    // Call on the io thread.
    void write(std::string s)
    {
        if (sock_)
            asio::async_write(*sock_, asio::buffer(*std::make_shared<std::string>(std::move(s))),
                              [](auto, auto) {});
    }
    void writeKeep(std::shared_ptr<std::string> s)
    {
        if (sock_)
            asio::async_write(*sock_, asio::buffer(*s), [s](auto, auto) {});
    }
    void closeClient()
    {
        if (sock_) {
            boost::system::error_code ec;
            sock_->close(ec);
            sock_.reset();
        }
    }
    void stopAll()
    {
        closeClient();
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

private:
    TestServer(asio::io_context& io, uint16_t port, bool read)
        : io_(io), acceptor_(io, {asio::ip::make_address("127.0.0.1"), port}), read_(read)
    {
        port_ = acceptor_.local_endpoint().port();
    }
    void accept()
    {
        auto s = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*s, [self = shared_from_this(), s](const boost::system::error_code& ec) {
            if (ec)
                return;
            self->sock_ = s;
            ++self->accepted_;
            if (self->read_)
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
                std::lock_guard<std::mutex> l(self->m_);
                self->received_.append(buf->data(), n);
            }
            self->read(s);
        });
    }

    asio::io_context& io_;
    tcp::acceptor acceptor_;
    bool read_;
    uint16_t port_ = 0;
    std::shared_ptr<tcp::socket> sock_;
    std::atomic<int> accepted_{0};
    mutable std::mutex m_;
    std::string received_;
};

} // namespace

BOOST_AUTO_TEST_CASE(dialer_delivers_chunks_in_order)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    std::mutex m;
    std::string got;
    d->onData([&](const uint8_t* p, std::size_t n) {
        std::lock_guard<std::mutex> l(m);
        got.append(reinterpret_cast<const char*>(p), n);
    });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    BOOST_REQUIRE(test::eventually([&] { return srv->accepted() == 1; }));

    std::string all;
    for (std::size_t i = 0; i < 100 * 1024; ++i)
        all.push_back(static_cast<char>('a' + (i * 7 + i / 13) % 26));
    t.call([&] {
        const std::size_t part = all.size() / 7;
        for (int i = 0; i < 7; ++i) {
            const std::size_t from = i * part;
            const std::size_t len = i == 6 ? all.size() - from : part;
            srv->writeKeep(std::make_shared<std::string>(all.substr(from, len)));
        }
    });
    BOOST_TEST(test::eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return got.size() == all.size();
    }));
    {
        std::lock_guard<std::mutex> l(m);
        BOOST_TEST((got == all));
    }
    BOOST_TEST(t.call([&] { return d->counters().bytes_in; }) == all.size());
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_send_reaches_server)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    t.call([&] { d->send(makeBuf("lost\n")); });
    BOOST_TEST(t.call([&] { return d->counters().dropped; }) == 1u);
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    t.call([&] { d->send(makeBuf("ping\n")); });
    BOOST_TEST(test::eventually([&] { return srv->received() == "ping\n"; }));
    BOOST_TEST(t.call([&] { return d->counters().messages_out; }) == 1u);
    BOOST_TEST(t.call([&] { return d->counters().bytes_out; }) == 5u);
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_peer_stats_and_endpoint)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    BOOST_TEST(t.call([&] { return d->peerStats().bytes_written; }) == 0u);
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    BOOST_TEST(t.call([&] { return d->peerEndpoint().port(); }) == srv->port());
    t.call([&] { d->send(makeBuf("ping\n")); });
    BOOST_REQUIRE(test::eventually([&] {
        const auto st = t.call([&] { return d->peerStats(); });
        return st.bytes_written == 5 && st.messages_written == 1 && st.queued_bytes == 0;
    }));
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_reconnects_with_backoff)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    std::mutex m;
    std::vector<std::pair<bool, std::chrono::steady_clock::time_point>> events;
    d->onConnection([&](bool up) {
        std::lock_guard<std::mutex> l(m);
        events.emplace_back(up, std::chrono::steady_clock::now());
    });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return srv->accepted() == 1; }));
    t.call([&] { srv->closeClient(); });
    BOOST_REQUIRE(test::eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return events.size() == 3;
    }));
    std::lock_guard<std::mutex> l(m);
    BOOST_TEST(events[0].first);
    BOOST_TEST(!events[1].first);
    BOOST_TEST(events[2].first);
    const auto gap = std::chrono::duration_cast<milliseconds>(events[2].second - events[1].second);
    BOOST_TEST(gap.count() >= 500);
    BOOST_TEST(gap.count() < 2000);
    BOOST_TEST(t.call([&] { return d->counters().connects; }) == 2u);
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_backoff_grows_when_nobody_listens)
{
    test::IoThread t;
    const uint16_t port = test::freeTcpPort();
    auto d = Dialer::create(t.io, "127.0.0.1", port);
    t.call([&] { d->start(); });
    std::this_thread::sleep_for(milliseconds(1800));
    // attempts at 0, 500 and 1500 ms; the next one would be at 3500 ms
    const auto attempts = t.call([&] { return d->attempts(); });
    BOOST_TEST(attempts >= 2u);
    BOOST_TEST(attempts <= 3u);
    BOOST_TEST(!d->connected());
    BOOST_TEST(!t.call([&] { return d->counters().last_error; }).empty());

    auto srv = t.call([&] { return TestServer::create(t.io, port); });
    BOOST_TEST(test::eventually([&] { return d->connected(); }, slow(std::chrono::seconds(10))));
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_sockets_use_keepalive_and_nodelay)
{
    asio::io_context io;
    tcp::acceptor acc(io, {asio::ip::make_address("127.0.0.1"), 0});
    tcp::socket client(io), server(io);
    client.connect(acc.local_endpoint());
    acc.accept(server);

    applyTcpOptions(client, TcpOptions{});
    const int fd = client.native_handle();
    auto get = [&](int level, int opt) {
        int v = -1;
        socklen_t len = sizeof v;
        BOOST_REQUIRE_EQUAL(::getsockopt(fd, level, opt, &v, &len), 0);
        return v;
    };
    BOOST_TEST(get(SOL_SOCKET, SO_KEEPALIVE) != 0);
    BOOST_TEST(get(IPPROTO_TCP, TCP_NODELAY) != 0);
#ifdef __linux__
    BOOST_TEST(get(IPPROTO_TCP, TCP_KEEPIDLE) == 30);
    BOOST_TEST(get(IPPROTO_TCP, TCP_KEEPINTVL) == 10);
    BOOST_TEST(get(IPPROTO_TCP, TCP_KEEPCNT) == 3);
#endif

    TcpOptions off;
    off.keepalive = false;
    off.nodelay = false;
    applyTcpOptions(server, off);
    int v = -1;
    socklen_t len = sizeof v;
    ::getsockopt(server.native_handle(), SOL_SOCKET, SO_KEEPALIVE, &v, &len);
    BOOST_TEST(v == 0);
}

BOOST_AUTO_TEST_CASE(no_callback_after_stop)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    std::atomic<int> data_calls{0}, conn_calls{0};
    d->onData([&](const uint8_t*, std::size_t) { ++data_calls; });
    d->onConnection([&](bool) { ++conn_calls; });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    t.call([&] {
        for (int i = 0; i < 50; ++i)
            srv->writeKeep(std::make_shared<std::string>(std::string(8192, 'x')));
    });
    BOOST_REQUIRE(test::eventually([&] { return data_calls.load() > 0; }));
    t.call([&] { d->stop(); });
    const int data_at_stop = data_calls.load(), conn_at_stop = conn_calls.load();
    t.call([&] { srv->closeClient(); });
    std::this_thread::sleep_for(milliseconds(200));
    BOOST_TEST(data_calls.load() == data_at_stop);
    BOOST_TEST(conn_calls.load() == conn_at_stop);
    BOOST_TEST(!d->connected());
    t.call([&] { srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_send_cap_disconnects)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io, 0, false); });
    TcpOptions o;
    o.send_cap = 4096;
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port(), o);
    std::atomic<int> ups{0}, downs{0};
    d->onConnection([&](bool up) { ++(up ? ups : downs); });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    t.call([&] {
        for (int i = 0; i < 8; ++i)
            d->send(makeBuf(std::string(1024, 'z')));
    });
    BOOST_TEST(test::eventually([&] { return downs.load() == 1; }));
    BOOST_TEST(t.call([&] { return d->counters().dropped; }) >= 1u);
    BOOST_TEST(test::eventually([&] { return ups.load() == 2; }));
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_loss_notification_is_posted_and_suppressed_by_stop)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io, 0, false); });
    TcpOptions o;
    o.send_cap = 1024;
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port(), o);
    std::atomic<int> downs{0};
    bool inside_send = false, reentered = false;
    d->onConnection([&](bool up) {
        if (!up) {
            ++downs;
            if (inside_send)
                reentered = true;
        }
    });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    t.call([&] {
        inside_send = true;
        d->send(makeBuf(std::string(2048, 'a'))); // over the cap: the connection is lost
        inside_send = false;
        d->stop(); // before the posted notification runs
    });
    std::this_thread::sleep_for(milliseconds(200));
    BOOST_TEST(downs.load() == 0);
    BOOST_TEST(!reentered);
    t.call([&] { srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_cap_during_write_and_stop_cycling_is_safe)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io, 0, false); });
    TcpOptions o;
    o.send_cap = 4096;
    auto d = Dialer::create(t.io, "localhost", srv->port(), o); // may resolve to several addresses
    for (int i = 0; i < 50; ++i) {
        t.call([&] { d->start(); });
        BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
        t.call([&] {
            d->send(makeBuf(std::string(3000, 'b')));
            for (int k = 0; k < 3; ++k)
                d->send(makeBuf(std::string(1500, 'c'))); // trips the cap with a write in flight
            if (i % 2 == 0)
                d->stop();
        });
        if (i % 2 != 0)
            t.call([&] { d->stop(); });
    }
    t.call([&] { srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_connect_timeout_moves_to_next_address)
{
    // 10.255.255.1 is unroutable (a connect to it hangs), so the first address
    // can only be passed by the connect timeout; the second one works.
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    TcpOptions o;
    o.connect_timeout = milliseconds(300);
    const std::vector<tcp::endpoint> eps{
        {asio::ip::make_address("10.255.255.1"), 9},
        {asio::ip::make_address("127.0.0.1"), srv->port()},
    };
    auto d = Dialer::create(t.io, eps, o);
    const auto t0 = std::chrono::steady_clock::now();
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }, slow(std::chrono::seconds(2))));
    const auto took = std::chrono::steady_clock::now() - t0;
    BOOST_CHECK(took < slow(std::chrono::seconds(2)));
    // The first address was passed by the timeout, not by an immediate error.
    BOOST_CHECK(took >= milliseconds(250));
    BOOST_TEST(t.call([&] { return d->counters().connects; }) == 1u);
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_zero_length_buf_dropped_and_counted)
{
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    auto d = Dialer::create(t.io, "127.0.0.1", srv->port());
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] { return d->connected(); }));
    t.call([&] {
        d->send(makeBuf(std::string_view("")));
        d->send(makeBuf(std::string_view("x")));
    });
    BOOST_TEST(t.call([&] { return d->counters().dropped; }) == 1u);
    BOOST_TEST(test::eventually([&] { return srv->received() == "x"; }));
    BOOST_TEST(t.call([&] { return d->counters().messages_out; }) == 1u);
    t.call([&] { d->stop(); srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_stale_connect_timer_does_not_close_live_connection)
{
    // A 1 ms timeout against a server that accepts at once: the timer and the
    // connect handler race; a timer that lost must never close the socket.
    test::IoThread t;
    auto srv = t.call([&] { return TestServer::create(t.io); });
    TcpOptions o;
    o.connect_timeout = milliseconds(1);
    std::atomic<int> downs{0};
    for (int i = 0; i < 50; ++i) {
        auto d = Dialer::create(t.io, "127.0.0.1", srv->port(), o);
        d->onConnection([&](bool up) {
            if (!up)
                ++downs;
        });
        t.call([&] { d->start(); });
        test::eventually([&] { return d->connected(); }, slow(std::chrono::seconds(2)));
        std::this_thread::sleep_for(milliseconds(3)); // let a stale timer handler run
        BOOST_CHECK(d->connected());
        t.call([&] { d->stop(); });
    }
    BOOST_TEST(downs.load() == 0);
    t.call([&] { srv->stopAll(); });
}

BOOST_AUTO_TEST_CASE(dialer_on_failure_reports_each_failed_cycle)
{
    test::IoThread t;
    const uint16_t port = test::freeTcpPort();
    auto d = Dialer::create(t.io, "127.0.0.1", port);
    std::mutex m;
    std::vector<std::string> whys;
    d->onFailure([&](const std::string& why) {
        std::lock_guard<std::mutex> l(m);
        whys.push_back(why);
    });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return !whys.empty();
    }));
    t.call([&] { d->stop(); });
    std::lock_guard<std::mutex> l(m);
    BOOST_REQUIRE(whys.size() == 1u); // stopped before the 500 ms retry
    BOOST_TEST(whys[0].find("Connection refused") != std::string::npos);
    BOOST_TEST(whys[0].rfind("connect ", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(dialer_on_failure_reports_a_failed_lookup_and_is_silent_after_stop)
{
    test::IoThread t;
    auto d = Dialer::create(t.io, "bad_host!", 30047);
    std::mutex m;
    std::vector<std::string> whys;
    d->onFailure([&](const std::string& why) {
        std::lock_guard<std::mutex> l(m);
        whys.push_back(why);
    });
    t.call([&] { d->start(); });
    BOOST_REQUIRE(test::eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return !whys.empty();
    }));
    t.call([&] { d->stop(); });
    std::this_thread::sleep_for(milliseconds(1200));
    std::lock_guard<std::mutex> l(m);
    BOOST_TEST(whys.size() == 1u);
    BOOST_TEST(whys[0].rfind("resolve ", 0) == 0u);
}
