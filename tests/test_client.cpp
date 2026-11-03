#define BOOST_TEST_MODULE skycue_client
#include <boost/test/unit_test.hpp>

#include <skycue/client.hpp>
#include <skycue/protocol.hpp>
#include <skycue/types.hpp>

#include "test_slow.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace skycue;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;

namespace {

const std::chrono::milliseconds kWait = slow(5s);

Track mkTrack(uint32_t address, bool keyframe)
{
    Track t;
    t.id = TrackId{address, AddressType::icao};
    t.source = Source::adsb_1090;
    t.position_time = std::chrono::system_clock::now();
    t.lat_deg = 38.9;
    t.lon_deg = -77.0;
    t.keyframe = keyframe;
    t.alt_baro_ft = 10000;
    return t;
}

std::vector<uint8_t> cat(std::vector<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Records every callback; tests wait on it with a timeout.
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Track> updates;
    std::vector<std::pair<TrackId, DropReason>> drops;
    std::vector<bool> conns;
    std::vector<std::thread::id> threads;

    void attach(Client& c)
    {
        c.onTrackUpdate([this](const Track& t) {
            std::lock_guard<std::mutex> lk(m);
            updates.push_back(t);
            threads.push_back(std::this_thread::get_id());
            cv.notify_all();
        });
        c.onTrackDropped([this](TrackId id, DropReason r) {
            std::lock_guard<std::mutex> lk(m);
            drops.emplace_back(id, r);
            threads.push_back(std::this_thread::get_id());
            cv.notify_all();
        });
        c.onConnectionChange([this](bool up) {
            std::lock_guard<std::mutex> lk(m);
            conns.push_back(up);
            threads.push_back(std::this_thread::get_id());
            cv.notify_all();
        });
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWait)
    {
        // system_clock rather than wait_for: libstdc++ maps a steady-clock wait
        // to pthread_cond_clockwait, which older ThreadSanitizers do not
        // intercept, so they report false races on everything below.
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_until(lk, std::chrono::system_clock::now() + timeout, p);
    }

    bool conn(size_t i)
    {
        std::lock_guard<std::mutex> lk(m);
        return i < conns.size() && conns[i];
    }

    size_t total()
    {
        std::lock_guard<std::mutex> lk(m);
        return updates.size() + drops.size() + conns.size();
    }

    bool sawUpdate(uint32_t address) // caller holds m
    {
        return std::any_of(updates.begin(), updates.end(), [&](const Track& t) { return t.id.address == address; });
    }

    size_t countDrops(DropReason r) // caller holds m
    {
        return static_cast<size_t>(
            std::count_if(drops.begin(), drops.end(), [&](const std::pair<TrackId, DropReason>& d) { return d.second == r; }));
    }
};

// A one-client TCP server on its own io thread.
class TestTcpServer {
public:
    explicit TestTcpServer(uint16_t port = 0)
    {
        tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        port_ = acceptor_.local_endpoint().port();
        acceptor_.async_accept(sock_, [this](const boost::system::error_code& ec) {
            if (!ec)
                accepted_.set_value();
        });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~TestTcpServer()
    {
        runSync([this] {
            boost::system::error_code ec;
            acceptor_.close(ec);
            sock_.close(ec);
        });
        guard_.reset();
        thread_.join();
    }

    uint16_t port() const { return port_; }

    bool waitAccepted() { return waitAcceptedFor(kWait); }
    bool waitAcceptedFor(std::chrono::milliseconds timeout)
    {
        return acceptedFuture_.wait_for(timeout) == std::future_status::ready;
    }

    void send(const std::vector<uint8_t>& bytes)
    {
        runSync([&] {
            boost::system::error_code ec;
            asio::write(sock_, asio::buffer(bytes), ec);
        });
    }

    void closeClient()
    {
        runSync([this] {
            boost::system::error_code ec;
            sock_.close(ec);
        });
    }

private:
    void runSync(std::function<void()> f)
    {
        std::promise<void> done;
        asio::post(io_, [&] {
            f();
            done.set_value();
        });
        done.get_future().wait();
    }

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_{asio::make_work_guard(io_)};
    tcp::acceptor acceptor_{io_};
    tcp::socket sock_{io_};
    uint16_t port_{};
    std::promise<void> accepted_;
    std::future<void> acceptedFuture_{accepted_.get_future()};
    std::thread thread_;
};

uint16_t freeUdpPort()
{
    asio::io_context io;
    udp::socket s(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return s.local_endpoint().port();
}

class UdpSender {
public:
    explicit UdpSender(uint16_t port) : dest_(asio::ip::make_address("127.0.0.1"), port) { sock_.open(udp::v4()); }
    void send(const std::vector<uint8_t>& datagram) { sock_.send_to(asio::buffer(datagram), dest_); }

private:
    asio::io_context io_;
    udp::socket sock_{io_};
    udp::endpoint dest_;
};

Config tcpConfig(uint16_t port)
{
    Config c;
    c.transport = Config::Transport::tcp;
    c.host = "127.0.0.1";
    c.port = port;
    c.reconnect_interval = 100ms;
    return c;
}

Config udpConfig(uint16_t port)
{
    Config c;
    c.transport = Config::Transport::udp;
    c.host = "127.0.0.1";
    c.port = port;
    return c;
}

std::vector<uint8_t> hello(uint32_t keyframe_ms) { return encodeHello(Hello{keyframe_ms, 1000, "test"}); }

Config listenConfig()
{
    Config c;
    c.transport = Config::Transport::tcp_listen;
    c.host = "127.0.0.1";
    c.port = 0; // ephemeral; see Client::localPort
    return c;
}

// A "skycued" that connects to a listening Client: blocking connect and
// writes, and a non-blocking wait for the Client to hang up.
class TcpPeer {
public:
    explicit TcpPeer(uint16_t port)
    {
        sock_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    }

    void send(const std::vector<uint8_t>& bytes) { asio::write(sock_, asio::buffer(bytes)); }

    void close()
    {
        boost::system::error_code ec;
        sock_.close(ec);
    }

    // True once the other end has closed the connection (EOF or reset).
    bool waitClosedFor(std::chrono::milliseconds timeout)
    {
        sock_.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            uint8_t buf[256];
            boost::system::error_code ec;
            sock_.read_some(asio::buffer(buf), ec);
            if (ec && ec != asio::error::would_block)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(5ms);
        }
    }

private:
    asio::io_context io_;
    tcp::socket sock_{io_};
};

} // namespace

BOOST_AUTO_TEST_CASE(tcp_receives_updates_and_drops)
{
    TestTcpServer server;
    Events ev;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server.waitAccepted());
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 1; }));
    BOOST_TEST(ev.conn(0) == true);

    // Everything in one write: the client must split concatenated frames.
    server.send(cat({hello(10000), encodeTrackUpdate(mkTrack(1, true)), encodeTrackUpdate(mkTrack(2, true)),
                     encodeTrackUpdate(mkTrack(3, false)),
                     encodeTrackDrop(TrackDrop{TrackId{2, AddressType::icao}, DropReason::filtered})}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3 && ev.drops.size() == 1; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.updates[0].id.address == 1u);
        BOOST_TEST(ev.updates[1].id.address == 2u);
        BOOST_TEST(ev.updates[2].id.address == 3u);
        BOOST_TEST(ev.drops[0].first.address == 2u);
        BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::filtered));
    }
    BOOST_TEST(client.trackCount() == 2u);
}

BOOST_AUTO_TEST_CASE(tcp_partial_frame_is_reassembled)
{
    TestTcpServer server;
    Events ev;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server.waitAccepted());

    auto frame = encodeTrackUpdate(mkTrack(7, true));
    const auto half = static_cast<std::ptrdiff_t>(frame.size() / 2);
    server.send(std::vector<uint8_t>(frame.begin(), frame.begin() + half));
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(ev.waitFor([&] { return !ev.updates.empty(); }, 0ms) == false);
    server.send(std::vector<uint8_t>(frame.begin() + half, frame.end()));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
    BOOST_TEST(client.trackCount() == 1u);
}

BOOST_AUTO_TEST_CASE(tcp_bad_magic_closes_connection)
{
    TestTcpServer server;
    Events ev;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server.waitAccepted());
    server.send(encodeTrackUpdate(mkTrack(1, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
    server.send(std::vector<uint8_t>{'X', 'X', 'X', 'X', 1, 2, 0, 0});
    // The connection callback fires before the source_lost drops (and the
    // count reset), so wait for the drop as well.
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2 && ev.drops.size() == 1; }));
    BOOST_TEST(ev.conn(1) == false);
    BOOST_TEST(client.trackCount() == 0u);
}

BOOST_AUTO_TEST_CASE(tcp_disconnect_drops_all_source_lost)
{
    TestTcpServer server;
    Events ev;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server.waitAccepted());
    server.send(cat({encodeTrackUpdate(mkTrack(1, true)), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    server.closeClient();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2 && ev.drops.size() == 2; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns[1] == false);
        BOOST_TEST(ev.countDrops(DropReason::source_lost) == 2u);
    }
    BOOST_TEST(client.trackCount() == 0u);
}

BOOST_AUTO_TEST_CASE(tcp_reconnects)
{
    Events ev;
    auto server = std::make_unique<TestTcpServer>();
    const uint16_t port = server->port();
    Client client(tcpConfig(port));
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server->waitAccepted());
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 1; }));

    server.reset(); // closes the connection and the listener
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2; }));
    BOOST_TEST(ev.conn(1) == false);

    server = std::make_unique<TestTcpServer>(port);
    BOOST_REQUIRE(server->waitAccepted());
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 3; }));
    BOOST_TEST(ev.conn(2) == true);
}

BOOST_AUTO_TEST_CASE(udp_stale_after_missing_two_keyframes)
{
    const uint16_t port = freeUdpPort();
    Events ev;
    Config cfg = udpConfig(port);
    cfg.heartbeat_timeout = 2000ms; // A keeps the source alive; only staleness may drop B
    Client client(cfg);              // default keyframe interval 10 s: the Hello must lower it
    ev.attach(client);
    client.start();
    UdpSender tx(port);

    // Keyframe bursts of A and B until the client (bound asynchronously) has both.
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool haveBoth = false;
    while (!haveBoth && std::chrono::steady_clock::now() < deadline) {
        tx.send(hello(200));
        tx.send(encodeTrackUpdate(mkTrack(0xA, true)));
        tx.send(encodeTrackUpdate(mkTrack(0xB, true)));
        haveBoth = ev.waitFor([&] { return ev.sawUpdate(0xA) && ev.sawUpdate(0xB); }, 200ms);
    }
    BOOST_REQUIRE(haveBoth);

    // Bursts with A only (plus a delta for B, which must not keep it alive).
    bool bDropped = false;
    const auto start = std::chrono::steady_clock::now();
    while (!bDropped && std::chrono::steady_clock::now() < start + 3s) {
        tx.send(hello(200));
        tx.send(encodeTrackUpdate(mkTrack(0xA, true)));
        tx.send(encodeTrackUpdate(mkTrack(0xB, false)));
        bDropped = ev.waitFor([&] { return !ev.drops.empty(); }, 200ms);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_REQUIRE(bDropped);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_REQUIRE(ev.drops.size() == 1u);
        BOOST_TEST(ev.drops[0].first.address == 0xBu);
        BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::stale));
    }
    // Not before two keyframe intervals have been missed.
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 300);
    BOOST_TEST(client.trackCount() == 1u);
}

BOOST_AUTO_TEST_CASE(udp_heartbeat_timeout_source_lost)
{
    const uint16_t port = freeUdpPort();
    Events ev;
    Config cfg = udpConfig(port);
    cfg.heartbeat_timeout = 300ms;
    Client client(cfg);
    ev.attach(client);
    client.start();
    UdpSender tx(port);

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool up = false;
    while (!up && std::chrono::steady_clock::now() < deadline) {
        tx.send(encodeHeartbeat(Heartbeat{1}));
        up = ev.waitFor([&] { return !ev.conns.empty(); }, 50ms);
    }
    BOOST_REQUIRE(up);
    tx.send(encodeTrackUpdate(mkTrack(1, true)));
    tx.send(encodeTrackUpdate(mkTrack(2, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    // Silence.
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2 && ev.drops.size() == 2; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns[0] == true);
        BOOST_TEST(ev.conns[1] == false);
        BOOST_TEST(ev.countDrops(DropReason::source_lost) == 2u);
    }
    BOOST_TEST(client.trackCount() == 0u);
}

BOOST_AUTO_TEST_CASE(udp_drops_datagram_with_wrong_size)
{
    const uint16_t port = freeUdpPort();
    Events ev;
    Client client(udpConfig(port));
    ev.attach(client);
    client.start();
    UdpSender tx(port);

    // Wait for the bind with heartbeats.
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool up = false;
    while (!up && std::chrono::steady_clock::now() < deadline) {
        tx.send(encodeHeartbeat(Heartbeat{1}));
        up = ev.waitFor([&] { return !ev.conns.empty(); }, 50ms);
    }
    BOOST_REQUIRE(up);

    auto longer = encodeTrackUpdate(mkTrack(1, true));
    longer.push_back(0); // datagram larger than 8 + length
    auto shorter = encodeTrackUpdate(mkTrack(2, true));
    shorter.pop_back(); // datagram smaller than 8 + length
    auto two = cat({encodeTrackUpdate(mkTrack(3, true)), encodeTrackUpdate(mkTrack(4, true))}); // two frames
    tx.send(longer);
    tx.send(shorter);
    tx.send(two);
    tx.send(encodeTrackUpdate(mkTrack(5, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.sawUpdate(5); }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.updates.size() == 1u);
    }
    BOOST_TEST(client.trackCount() == 1u);
}

BOOST_AUTO_TEST_CASE(udp_drops_oversized_datagram)
{
    const uint16_t port = freeUdpPort();
    Events ev;
    Client client(udpConfig(port));
    ev.attach(client);
    client.start();
    UdpSender tx(port);

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool up = false;
    while (!up && std::chrono::steady_clock::now() < deadline) {
        tx.send(encodeHeartbeat(Heartbeat{1}));
        up = ev.waitFor([&] { return !ev.conns.empty(); }, 50ms);
    }
    BOOST_REQUIRE(up);

    // 1300 bytes whose header claims 1201 payload bytes: truncated to the
    // receive buffer (8 + 1200 + 1) it would look like a well-sized frame
    // with a valid TrackUpdate followed by ignorable trailing bytes.
    auto big = encodeTrackUpdate(mkTrack(9, true));
    big[6] = static_cast<uint8_t>(1201 & 0xFF);
    big[7] = static_cast<uint8_t>(1201 >> 8);
    big.resize(1300, 0);
    tx.send(big);
    tx.send(encodeTrackUpdate(mkTrack(10, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.sawUpdate(10); }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.updates.size() == 1u);
        BOOST_TEST(!ev.sawUpdate(9));
    }
}

BOOST_AUTO_TEST_CASE(throwing_callback_does_not_stop_reading)
{
    TestTcpServer server;
    Config cfg = tcpConfig(server.port());
    cfg.run_io_thread = false;
    Client client(cfg);
    int calls = 0;
    client.onTrackUpdate([&](const Track&) {
        if (++calls == 1)
            throw std::runtime_error("callback failure");
    });
    client.start();

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    int thrown = 0;
    auto pollUntil = [&](std::function<bool()> done) {
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            try {
                client.poll();
            } catch (const std::runtime_error& e) {
                BOOST_TEST(std::string(e.what()) == "callback failure");
                ++thrown;
            }
            std::this_thread::sleep_for(5ms);
        }
    };
    pollUntil([&] { return server.waitAcceptedFor(0ms); });
    BOOST_REQUIRE(server.waitAccepted());

    server.send(encodeTrackUpdate(mkTrack(1, true)));
    pollUntil([&] { return calls == 1 && thrown == 1; });
    BOOST_REQUIRE(calls == 1);
    BOOST_TEST(thrown == 1); // rethrown from poll()
    BOOST_TEST(client.trackCount() == 1u);

    server.send(encodeTrackUpdate(mkTrack(2, true)));
    pollUntil([&] { return calls == 2; });
    BOOST_TEST(calls == 2); // reading continued after the throw
    BOOST_TEST(thrown == 1);
    BOOST_TEST(client.trackCount() == 2u);
}

BOOST_AUTO_TEST_CASE(throwing_callback_in_thread_mode_keeps_connection)
{
    TestTcpServer server;
    Events ev;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    std::atomic<int> calls{0};
    client.onTrackUpdate([&](const Track& t) {
        {
            std::lock_guard<std::mutex> lk(ev.m);
            ev.updates.push_back(t);
            ev.cv.notify_all();
        }
        if (++calls == 1)
            throw std::runtime_error("callback failure");
    });
    client.start();
    BOOST_REQUIRE(server.waitAccepted());

    server.send(encodeTrackUpdate(mkTrack(1, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
    server.send(encodeTrackUpdate(mkTrack(2, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns.size() == 1u); // still the first connection
        BOOST_TEST(ev.drops.empty());
    }
    BOOST_TEST(client.trackCount() == 2u);
}

BOOST_AUTO_TEST_CASE(stop_from_inside_callback_is_safe)
{
    TestTcpServer server;
    Events ev;
    std::atomic<int> after{0};
    {
        Client client(tcpConfig(server.port()));
        ev.attach(client);
        client.onTrackUpdate([&](const Track& t) {
            {
                std::lock_guard<std::mutex> lk(ev.m);
                ev.updates.push_back(t);
            }
            client.stop(); // on the I/O thread: no self-join
            ev.cv.notify_all();
        });
        client.onTrackDropped([&](TrackId, DropReason) { ++after; });
        client.start();
        BOOST_REQUIRE(server.waitAccepted());
        // Two updates in one read: the second must not surface after stop().
        server.send(cat({encodeTrackUpdate(mkTrack(1, true)), encodeTrackUpdate(mkTrack(2, true))}));
        BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() >= 1; }));
        server.closeClient();
        std::this_thread::sleep_for(50ms);
    } // destructor joins the I/O thread
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updates.size() == 1u);
    BOOST_TEST(ev.conns.size() == 1u);
    BOOST_TEST(after.load() == 0);
}

BOOST_AUTO_TEST_CASE(poll_mode_runs_callbacks_only_in_poll)
{
    TestTcpServer server;
    Events ev;
    Config cfg = tcpConfig(server.port());
    cfg.run_io_thread = false;
    Client client(cfg);
    ev.attach(client);
    client.start();

    // Without poll() the client does not even connect.
    BOOST_TEST(server.waitAcceptedFor(200ms) == false);
    BOOST_TEST(ev.total() == 0u);

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (ev.total() == 0 && std::chrono::steady_clock::now() < deadline) {
        client.poll();
        std::this_thread::sleep_for(5ms);
    }
    BOOST_REQUIRE(ev.total() == 1u);
    BOOST_REQUIRE(server.waitAccepted());

    server.send(encodeTrackUpdate(mkTrack(1, true)));
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(ev.total() == 1u); // data has arrived but nobody polled
    while (client.trackCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        client.poll();
        std::this_thread::sleep_for(5ms);
    }
    BOOST_TEST(client.trackCount() == 1u);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.updates.size() == 1u);
        for (auto id : ev.threads)
            BOOST_TEST((id == std::this_thread::get_id()));
    }
    client.stop();
}

BOOST_AUTO_TEST_CASE(stop_is_idempotent_and_no_callbacks_after_stop)
{
    {
        Client never(tcpConfig(1)); // destroyed without start()
    }
    {
        Client stoppedOnly(tcpConfig(1));
        stoppedOnly.stop();
        stoppedOnly.stop();
    }

    for (bool threaded : {true, false}) {
        TestTcpServer server;
        Events ev;
        Config cfg = tcpConfig(server.port());
        cfg.run_io_thread = threaded;
        Client client(cfg);
        ev.attach(client);
        client.start();
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        if (!threaded) {
            while (ev.total() == 0 && std::chrono::steady_clock::now() < deadline) {
                client.poll();
                std::this_thread::sleep_for(5ms);
            }
        }
        BOOST_REQUIRE(server.waitAccepted());
        server.send(encodeTrackUpdate(mkTrack(1, true)));
        if (threaded) {
            BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
        } else {
            while (client.trackCount() == 0 && std::chrono::steady_clock::now() < deadline) {
                client.poll();
                std::this_thread::sleep_for(5ms);
            }
        }
        BOOST_REQUIRE(client.trackCount() == 1u);

        // Data already in flight when stop() is called must not surface.
        server.send(encodeTrackUpdate(mkTrack(2, true)));
        client.stop();
        const size_t seen = ev.total();
        client.stop();
        client.poll(); // a no-op after stop()

        server.send(encodeTrackUpdate(mkTrack(3, true)));
        server.closeClient();
        std::this_thread::sleep_for(150ms);
        client.poll();
        BOOST_TEST(ev.total() == seen);
    }
}

BOOST_AUTO_TEST_CASE(on_frame_sees_hello_and_heartbeat)
{
    TestTcpServer server;
    Events ev;
    std::vector<FrameType> types;
    std::vector<Hello> hellos;
    Client client(tcpConfig(server.port()));
    ev.attach(client);
    client.onFrame([&](const Frame& f) {
        std::lock_guard<std::mutex> lk(ev.m);
        types.push_back(f.type);
        Hello h;
        if (decodeHello(f, h))
            hellos.push_back(h);
        ev.cv.notify_all();
    });
    client.start();
    BOOST_REQUIRE(server.waitAccepted());

    server.send(cat({hello(10000), encodeHeartbeat(Heartbeat{42}), encodeTrackUpdate(mkTrack(1, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_REQUIRE(types.size() == 3u);
        BOOST_TEST(static_cast<int>(types[0]) == static_cast<int>(FrameType::hello));
        BOOST_TEST(static_cast<int>(types[1]) == static_cast<int>(FrameType::heartbeat));
        BOOST_TEST(static_cast<int>(types[2]) == static_cast<int>(FrameType::track_update));
        BOOST_REQUIRE(hellos.size() == 1u);
        BOOST_TEST(hellos[0].sender == "test");
        BOOST_TEST(hellos[0].keyframe_interval_ms == 10000u);
    }
    client.stop();
}

BOOST_AUTO_TEST_CASE(tcp_listen_accepts_server_connection)
{
    Events ev;
    Client client(listenConfig());
    ev.attach(client);
    client.start();
    const uint16_t port = client.localPort();
    BOOST_REQUIRE(port != 0);

    TcpPeer peer(port);
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 1; }));
    BOOST_TEST(ev.conn(0) == true);
    peer.send(cat({hello(10000), encodeTrackUpdate(mkTrack(1, true)), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    BOOST_TEST(client.trackCount() == 2u);
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updates[0].id.address == 1u);
    BOOST_TEST(ev.updates[1].id.address == 2u);
    BOOST_TEST(ev.drops.empty());
}

BOOST_AUTO_TEST_CASE(tcp_listen_two_connections_merge_into_one_table)
{
    Events ev;
    Client client(listenConfig());
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    TcpPeer a(client.localPort()), b(client.localPort());
    a.send(cat({hello(10000), encodeTrackUpdate(mkTrack(1, true))}));
    b.send(cat({hello(10000), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    BOOST_TEST(client.trackCount() == 2u);

    // One source going away takes only its own tracks; the client stays connected.
    a.close();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.drops.size() == 1; }));
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns.size() == 1u);
        BOOST_TEST(ev.drops.size() == 1u);
        BOOST_TEST(ev.drops[0].first.address == 1u);
    }
    BOOST_TEST(client.trackCount() == 1u);

    // The last one going away is a lost source.
    b.close();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2 && ev.drops.size() == 2; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns[0] == true);
        BOOST_TEST(ev.conns[1] == false);
        BOOST_TEST(ev.drops[1].first.address == 2u);
        BOOST_TEST(ev.countDrops(DropReason::source_lost) == 2u);
    }
    BOOST_TEST(client.trackCount() == 0u);

    // The listener keeps accepting.
    TcpPeer c(client.localPort());
    c.send(encodeTrackUpdate(mkTrack(3, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3 && ev.conns.size() == 3; }));
    BOOST_TEST(ev.conn(2) == true);
}

BOOST_AUTO_TEST_CASE(tcp_listen_rejects_bad_magic_closes_only_that_peer)
{
    Events ev;
    Client client(listenConfig());
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    TcpPeer bad(client.localPort()), good(client.localPort());
    bad.send(encodeTrackUpdate(mkTrack(1, true)));
    good.send(encodeTrackUpdate(mkTrack(2, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    bad.send(std::vector<uint8_t>{'X', 'X', 'X', 'X', 1, 2, 0, 0});
    BOOST_REQUIRE(bad.waitClosedFor(kWait));

    good.send(encodeTrackUpdate(mkTrack(3, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3 && ev.drops.size() == 1; }));
    BOOST_TEST(client.trackCount() == 2u); // the bad peer's track went with it
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.conns.size() == 1u);
    BOOST_TEST(ev.drops[0].first.address == 1u);
    BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::source_lost));
}

BOOST_AUTO_TEST_CASE(tcp_dead_peer_detected_by_heartbeat_timeout)
{
    TestTcpServer server;
    Events ev;
    Config cfg = tcpConfig(server.port());
    cfg.heartbeat_timeout = 200ms; // less than 3 x the Hello's 150 ms heartbeat, which wins
    Client client(cfg);
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(server.waitAccepted());
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 1; }));
    server.send(cat({encodeHello(Hello{10000, 150, "test"}), encodeTrackUpdate(mkTrack(1, true)),
                     encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    // Heartbeats keep the connection alive well past the timeout.
    for (int i = 0; i < 8; ++i) {
        server.send(encodeHeartbeat(Heartbeat{static_cast<uint64_t>(i)}));
        std::this_thread::sleep_for(100ms);
    }
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns.size() == 1u);
        BOOST_TEST(ev.drops.empty());
    }

    // The server stops sending without closing: dead after 3 x 150 ms.
    const auto silent = std::chrono::steady_clock::now();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() >= 2 && ev.drops.size() == 2; }, 450ms + 1s));
    const auto elapsed = std::chrono::steady_clock::now() - silent;
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns[1] == false);
        BOOST_TEST(ev.countDrops(DropReason::source_lost) == 2u);
    }
    // steady_timer may fire a few ms early after rounding; the bound proves detection is not immediate.
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 315);
    BOOST_TEST(client.trackCount() == 0u);

    // And it reconnects (the listener's backlog completes the connect).
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() >= 3; }, 1s));
    BOOST_TEST(ev.conn(2) == true);
}

BOOST_AUTO_TEST_CASE(tcp_listen_silent_peer_closed)
{
    Events ev;
    Config cfg = listenConfig();
    cfg.heartbeat_timeout = 300ms;
    Client client(cfg);
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    TcpPeer a(client.localPort()), b(client.localPort());
    a.send(cat({encodeHello(Hello{10000, 100, "a"}), encodeTrackUpdate(mkTrack(1, true))}));
    b.send(cat({encodeHello(Hello{10000, 100, "b"}), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    // a goes silent while b keeps sending heartbeats: a is closed.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool aClosed = false;
    uint64_t seq = 0;
    while (!aClosed && std::chrono::steady_clock::now() < deadline) {
        b.send(encodeHeartbeat(Heartbeat{++seq}));
        aClosed = a.waitClosedFor(50ms);
    }
    BOOST_REQUIRE(aClosed);
    b.send(encodeTrackUpdate(mkTrack(3, true)));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3 && ev.drops.size() == 1; }));
    BOOST_TEST(client.trackCount() == 2u);
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.conns.size() == 1u); // b is still connected
    BOOST_TEST(ev.conns[0] == true);
    BOOST_TEST(ev.drops[0].first.address == 1u); // a's track went with a
    BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::source_lost));
}

BOOST_AUTO_TEST_CASE(tcp_listen_peer_close_drops_only_its_tracks_source_lost)
{
    Events ev;
    Client client(listenConfig());
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    TcpPeer a(client.localPort()), b(client.localPort());
    a.send(cat({hello(10000), encodeTrackUpdate(mkTrack(1, true)), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    // b sends 3, and an update for 2, which takes 2 over from a.
    b.send(cat({hello(10000), encodeTrackUpdate(mkTrack(3, true)), encodeTrackUpdate(mkTrack(2, false))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 4; }));
    BOOST_TEST(client.trackCount() == 3u);

    a.close();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.drops.size() == 1; }));
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.drops.size() == 1u);
        BOOST_TEST(ev.drops[0].first.address == 1u);
        BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::source_lost));
        BOOST_TEST(ev.conns.size() == 1u); // still connected through b
    }
    BOOST_TEST(client.trackCount() == 2u);

    b.close();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2 && ev.drops.size() == 3; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.conns[1] == false);
        BOOST_TEST(ev.countDrops(DropReason::source_lost) == 3u);
    }
    BOOST_TEST(client.trackCount() == 0u);
}

BOOST_AUTO_TEST_CASE(tcp_listen_drop_from_non_owner_ignored)
{
    Events ev;
    Client client(listenConfig());
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    TcpPeer a(client.localPort()), b(client.localPort());
    a.send(cat({hello(10000), encodeTrackUpdate(mkTrack(1, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));

    // b drops a's track (that skycued filtered it), then sends 5: by the time
    // 5 arrives the drop has been handled, and ignored.
    b.send(cat({hello(10000), encodeTrackDrop(TrackDrop{TrackId{1, AddressType::icao}, DropReason::filtered}),
                encodeTrackUpdate(mkTrack(5, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.sawUpdate(5); }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.drops.empty());
    }
    BOOST_TEST(client.trackCount() == 2u);

    // The owner's drop is honoured.
    a.send(encodeTrackDrop(TrackDrop{TrackId{1, AddressType::icao}, DropReason::filtered}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.drops.size() == 1; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.drops[0].first.address == 1u);
        BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::filtered));
    }
    BOOST_TEST(client.trackCount() == 1u);
}

BOOST_AUTO_TEST_CASE(tcp_listen_per_peer_keyframe_interval)
{
    Events ev;
    Client client(listenConfig()); // heartbeat_timeout 3 s: the test ends before it matters
    ev.attach(client);
    client.start();
    BOOST_REQUIRE(client.localPort() != 0);

    // a keyframes every 200 ms, b every 10 s; b's Hello comes last and must
    // not lengthen a's interval.
    TcpPeer a(client.localPort()), b(client.localPort());
    a.send(cat({hello(200), encodeTrackUpdate(mkTrack(1, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 1; }));
    b.send(cat({hello(10000), encodeTrackUpdate(mkTrack(2, true))}));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));

    // Only deltas from now on: a's track goes stale after two of its
    // intervals, b's (two of b's intervals = 20 s) does not.
    const auto start = std::chrono::steady_clock::now();
    bool dropped = false;
    while (!dropped && std::chrono::steady_clock::now() < start + 3s) {
        a.send(encodeTrackUpdate(mkTrack(1, false)));
        b.send(encodeTrackUpdate(mkTrack(2, false)));
        dropped = ev.waitFor([&] { return !ev.drops.empty(); }, 100ms);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_REQUIRE(dropped);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 300);
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_REQUIRE(ev.drops.size() == 1u);
        BOOST_TEST(ev.drops[0].first.address == 1u);
        BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::stale));
    }
    BOOST_TEST(client.trackCount() == 1u);
}

BOOST_AUTO_TEST_CASE(udp_no_stale_drop_before_hello)
{
    const uint16_t port = freeUdpPort();
    Events ev;
    Config cfg = udpConfig(port);
    cfg.default_keyframe_interval = 100ms; // would make A stale within ~350 ms
    cfg.heartbeat_timeout = 2000ms;
    Client client(cfg);
    ev.attach(client);
    client.start();
    UdpSender tx(port);

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool have = false;
    while (!have && std::chrono::steady_clock::now() < deadline) {
        tx.send(encodeTrackUpdate(mkTrack(0xA, true)));
        have = ev.waitFor([&] { return ev.sawUpdate(0xA); }, 100ms);
    }
    BOOST_REQUIRE(have);

    // No Hello yet: deltas and heartbeats only, for well over 2 x 100 ms.
    for (int i = 0; i < 8; ++i) {
        tx.send(encodeTrackUpdate(mkTrack(0xA, false)));
        tx.send(encodeHeartbeat(Heartbeat{static_cast<uint64_t>(i)}));
        std::this_thread::sleep_for(100ms);
    }
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.drops.empty());
    }
    BOOST_TEST(client.trackCount() == 1u);

    // Once a Hello has announced the interval, staleness applies.
    bool dropped = false;
    const auto start = std::chrono::steady_clock::now();
    while (!dropped && std::chrono::steady_clock::now() < start + 3s) {
        tx.send(hello(100));
        dropped = ev.waitFor([&] { return !ev.drops.empty(); }, 100ms);
    }
    BOOST_REQUIRE(dropped);
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.drops[0].first.address == 0xAu);
    BOOST_TEST(static_cast<int>(ev.drops[0].second) == static_cast<int>(DropReason::stale));
}

namespace {

// onError messages with when and on which thread they arrived.
struct Errors {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> messages;
    std::vector<std::chrono::steady_clock::time_point> at;
    std::vector<std::thread::id> threads;

    void attach(Client& c, bool throw_first = false)
    {
        c.onError([this, throw_first](const std::string& msg) {
            bool first = false;
            {
                std::lock_guard<std::mutex> lk(m);
                first = messages.empty();
                messages.push_back(msg);
                at.push_back(std::chrono::steady_clock::now());
                threads.push_back(std::this_thread::get_id());
                cv.notify_all();
            }
            if (throw_first && first)
                throw std::runtime_error("onError itself threw"); // must be ignored
        });
    }
    bool waitCount(size_t n, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_until(lk, std::chrono::system_clock::now() + timeout, [&] { return messages.size() >= n; });
    }
    std::vector<std::string> copy()
    {
        std::lock_guard<std::mutex> lk(m);
        return messages;
    }
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

} // namespace

BOOST_AUTO_TEST_CASE(on_error_reports_bind_failure)
{
    // tcp_listen on a port someone else listens on.
    asio::io_context io;
    tcp::acceptor taken(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t tport = taken.local_endpoint().port();
    Config cfg = listenConfig();
    cfg.port = tport;
    cfg.reconnect_interval = 100ms;
    Client client(cfg);
    Errors errs;
    errs.attach(client, true);
    client.start();
    BOOST_TEST(client.localPort() == 0);
    BOOST_REQUIRE(errs.waitCount(2)); // retried, and reported again, despite onError throwing
    for (const auto& m : errs.copy()) {
        BOOST_TEST(contains(m, "127.0.0.1:" + std::to_string(tport)), m);
        BOOST_TEST(contains(m, "cannot listen"), m);
        BOOST_TEST(contains(m, "retrying in 100 ms"), m);
    }
    {
        std::lock_guard<std::mutex> lk(errs.m);
        for (const auto& id : errs.threads)
            BOOST_TEST((id != std::this_thread::get_id())); // on the I/O thread
    }
    client.stop();

    // udp on a port that is already bound.
    udp::socket utaken(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t uport = utaken.local_endpoint().port();
    Config ucfg = udpConfig(uport);
    ucfg.reconnect_interval = 100ms;
    Client uclient(ucfg);
    Errors uerrs;
    uerrs.attach(uclient);
    uclient.start();
    BOOST_REQUIRE(uerrs.waitCount(1));
    const auto um = uerrs.copy().at(0);
    BOOST_TEST(contains(um, "udp"), um);
    BOOST_TEST(contains(um, "cannot bind 127.0.0.1:" + std::to_string(uport)), um);
    BOOST_TEST(contains(um, "retrying in 100 ms"), um);
}

BOOST_AUTO_TEST_CASE(on_error_reports_connect_failure_once_per_attempt)
{
    uint16_t port = 0;
    {
        asio::io_context io;
        tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        port = a.local_endpoint().port();
    } // closed: connections are refused
    Config cfg = tcpConfig(port);
    cfg.reconnect_interval = 100ms;
    Client client(cfg);
    Errors errs;
    errs.attach(client);
    Events ev;
    ev.attach(client);
    const auto t0 = std::chrono::steady_clock::now();
    client.start();
    std::this_thread::sleep_for(1050ms);
    client.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto msgs = errs.copy();
    // One attempt at once, then one every 100 ms: about 11, one message each.
    const auto attempts = static_cast<size_t>(elapsed / 100ms) + 1;
    BOOST_TEST(msgs.size() >= 5u);
    BOOST_TEST(msgs.size() <= attempts);
    for (const auto& m : msgs) {
        BOOST_TEST(contains(m, "tcp: cannot connect to 127.0.0.1:" + std::to_string(port)), m);
        BOOST_TEST(contains(m, "retrying in 100 ms"), m);
    }
    std::lock_guard<std::mutex> lk(errs.m);
    for (size_t i = 1; i < errs.at.size(); ++i)
        BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(errs.at[i] - errs.at[i - 1]).count() >= 90);
    std::lock_guard<std::mutex> lk2(ev.m);
    BOOST_TEST(ev.conns.empty()); // a failed attempt is not a connection change
}

BOOST_AUTO_TEST_CASE(on_error_reports_swallowed_callback_exception)
{
    // Thread mode: the exception is discarded, and reported.
    {
        TestTcpServer server;
        Client client(tcpConfig(server.port()));
        Errors errs;
        errs.attach(client);
        std::atomic<int> calls{0};
        client.onTrackUpdate([&](const Track&) {
            if (++calls == 1)
                throw std::runtime_error("boom in onTrackUpdate");
        });
        client.onConnectionChange([](bool) { throw 42; }); // not a std::exception
        client.start();
        BOOST_REQUIRE(server.waitAccepted());
        server.send(encodeTrackUpdate(mkTrack(1, true)));
        BOOST_REQUIRE(errs.waitCount(2));
        const auto msgs = errs.copy();
        BOOST_TEST(std::any_of(msgs.begin(), msgs.end(), [](const std::string& m) {
            return contains(m, "callback threw") && contains(m, "boom in onTrackUpdate");
        }));
        BOOST_TEST(std::any_of(msgs.begin(), msgs.end(), [](const std::string& m) {
            return contains(m, "callback threw") && contains(m, "unknown exception");
        }));
        server.send(encodeTrackUpdate(mkTrack(2, true)));
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (calls < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        BOOST_TEST(calls == 2); // carried on
        BOOST_TEST(errs.copy().size() == 2u);
    }
    // Poll mode: rethrown from poll() instead, not reported.
    {
        TestTcpServer server;
        Config cfg = tcpConfig(server.port());
        cfg.run_io_thread = false;
        Client client(cfg);
        Errors errs;
        errs.attach(client);
        client.onTrackUpdate([](const Track&) { throw std::runtime_error("poll boom"); });
        client.start();
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        bool thrown = false;
        bool sent = false;
        while (!thrown && std::chrono::steady_clock::now() < deadline) {
            try {
                client.poll();
            } catch (const std::runtime_error& e) {
                thrown = std::string(e.what()) == "poll boom";
            }
            if (!sent && server.waitAcceptedFor(0ms)) {
                server.send(encodeTrackUpdate(mkTrack(1, true)));
                sent = true;
            }
            std::this_thread::sleep_for(5ms);
        }
        BOOST_TEST(thrown);
        BOOST_TEST(errs.copy().empty());
    }
}
