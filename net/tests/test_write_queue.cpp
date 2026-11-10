#define BOOST_TEST_MODULE net_write_queue
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/buffer.hpp"
#include "skycue/net/write_queue.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

using namespace skycue::net;
using namespace skycue::net::test;
using std::chrono::milliseconds;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

// A connected pair: `server` lives on the io thread's context (the side a
// WriteQueue writes to), `client` is read by the test with a poll() deadline.
struct Pair {
    asio::io_context cio;
    tcp::socket client{cio};
    std::shared_ptr<tcp::socket> server;

    explicit Pair(asio::io_context& io) : server(std::make_shared<tcp::socket>(io))
    {
        tcp::acceptor a(io, {asio::ip::make_address("127.0.0.1"), 0});
        client.connect(a.local_endpoint());
        a.accept(*server);
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
            pollfd p{client.native_handle(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0)
                break;
            const ssize_t n = ::recv(client.native_handle(), tmp, sizeof tmp, 0);
            if (n <= 0)
                break;
            out.append(tmp, static_cast<std::size_t>(n));
        }
        return out;
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(enqueue_rejects_over_cap_without_partial)
{
    IoThread t;
    Pair p(t.io);
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 10); });
    t.call([&] {
        BOOST_CHECK(q->enqueue(makeBuf(std::string_view("abcdef"))));
        BOOST_CHECK_EQUAL(q->queuedBytes(), 6u);
        BOOST_CHECK(!q->enqueue(makeBuf(std::string_view("12345")))); // 11 > 10: nothing queued
        BOOST_CHECK_EQUAL(q->queuedBytes(), 6u);
        BOOST_CHECK(q->enqueue(makeBuf(std::string_view("1234")))); // exactly the cap
        BOOST_CHECK_EQUAL(q->queuedBytes(), 10u);
        BOOST_CHECK(!q->enqueue(makeBuf(std::string_view("x"))));
        BOOST_CHECK_EQUAL(q->queuedBytes(), 10u);
        q->start([](const boost::system::error_code&) {});
    });
    // Only what was accepted is ever written, in order.
    BOOST_CHECK_EQUAL(p.read(10, slow(std::chrono::seconds(2))), "abcdef1234");
    BOOST_CHECK(eventually([&] { return t.call([&] { return q->queuedBytes(); }) == 0u; }));
    BOOST_CHECK_EQUAL(p.read(1, milliseconds(100)), "");
    t.call([&] { q->clear(); });
}

BOOST_AUTO_TEST_CASE(gather_write_preserves_order_and_counts)
{
    IoThread t;
    Pair p(t.io);
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 1u << 20); });
    std::string want;
    t.call([&] {
        q->start([](const boost::system::error_code&) {});
        // The first enqueue starts a write; the rest queue behind it and go out gathered.
        for (int i = 0; i < 50; ++i) {
            std::string s = "msg" + std::to_string(i) + ";";
            want += s;
            BOOST_REQUIRE(q->enqueue(makeBuf(s)));
        }
    });
    BOOST_CHECK_EQUAL(p.read(want.size(), slow(std::chrono::seconds(2))), want);
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return q->messagesWritten(); }) == 50u; }));
    BOOST_CHECK_EQUAL(t.call([&] { return q->bytesWritten(); }), want.size());
    BOOST_CHECK_EQUAL(t.call([&] { return q->queuedBytes(); }), 0u);
    // Later enqueues after it went idle are written too.
    t.call([&] { BOOST_REQUIRE(q->enqueue(makeBuf(std::string_view("tail")))); });
    BOOST_CHECK_EQUAL(p.read(4, slow(std::chrono::seconds(2))), "tail");
    BOOST_CHECK(eventually([&] { return t.call([&] { return q->messagesWritten(); }) == 51u; }));
    t.call([&] { q->clear(); });
}

BOOST_AUTO_TEST_CASE(clear_drops_queue)
{
    IoThread t;
    Pair p(t.io);
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 16u << 20); });
    std::atomic<int> errors{0};
    t.call([&] {
        BOOST_CHECK(q->enqueue(makeBuf(std::string_view("one"))));
        BOOST_CHECK(q->enqueue(makeBuf(std::string_view("two"))));
        q->clear();
        BOOST_CHECK_EQUAL(q->queuedBytes(), 0u);
        q->start([&](const boost::system::error_code&) { ++errors; });
    });
    BOOST_CHECK_EQUAL(p.read(1, milliseconds(100)), ""); // nothing was left to write
    t.call([&] { BOOST_CHECK(q->enqueue(makeBuf(std::string_view("three")))); });
    BOOST_CHECK_EQUAL(p.read(5, slow(std::chrono::seconds(2))), "three");

    // clear() with a write in flight: the caller closes the socket, and no
    // error is reported for the voided write.
    t.call([&] {
        BOOST_CHECK(q->enqueue(makeBuf(std::string(8u << 20, 'z')))); // the peer does not read it
    });
    t.call([&] {
        q->clear();
        boost::system::error_code ec;
        p.server->close(ec);
        BOOST_CHECK_EQUAL(q->queuedBytes(), 0u);
    });
    BOOST_CHECK(!eventually([&] { return errors.load() != 0; }, milliseconds(200)));
}

BOOST_AUTO_TEST_CASE(in_flight_accounting_releases_completed_blocks_while_the_peer_stalls)
{
    IoThread t;
    Pair p(t.io);
    constexpr std::size_t count = 400000, each = 18;
    constexpr std::size_t total = count * each;
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 32u << 20); });
    std::string want;
    want.reserve(total);
    t.call([&] {
        for (std::size_t i = 0; i < count; ++i) {
            std::string s(each, static_cast<char>('a' + i % 26));
            want += s;
            BOOST_REQUIRE(q->enqueue(makeBuf(s)));
        }
        BOOST_REQUIRE_EQUAL(q->queuedBytes(), total);
        q->start([](const boost::system::error_code&) {});
    });
    // The peer reads nothing. Blocks the kernel has taken are released while the rest stays queued.
    BOOST_CHECK(eventually([&] { return t.call([&] { return q->bytesWritten(); }) >= 65536u; }, slow(std::chrono::seconds(2))));
    BOOST_CHECK_LE(t.call([&] { return q->queuedBytes(); }), total - 65536u);
    BOOST_CHECK_GE(t.call([&] { return q->bytesWritten(); }), 65536u);

    BOOST_CHECK(p.read(total, slow(std::chrono::seconds(20))) == want);
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return q->messagesWritten(); }) == count; }));
    BOOST_CHECK_EQUAL(t.call([&] { return q->bytesWritten(); }), total);
    BOOST_CHECK_EQUAL(t.call([&] { return q->queuedBytes(); }), 0u);
    t.call([&] { q->clear(); });
}

BOOST_AUTO_TEST_CASE(single_large_message_is_written_whole)
{
    IoThread t;
    Pair p(t.io);
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 1u << 20); });
    std::string big(200000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>((i * 7 + 3) & 0xff);
    t.call([&] {
        q->start([](const boost::system::error_code&) {});
        BOOST_REQUIRE(q->enqueue(makeBuf(big)));
    });
    BOOST_CHECK(p.read(big.size(), slow(std::chrono::seconds(5))) == big);
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return q->messagesWritten(); }) == 1u; }));
    BOOST_CHECK_EQUAL(t.call([&] { return q->bytesWritten(); }), big.size());
    BOOST_CHECK_EQUAL(t.call([&] { return q->queuedBytes(); }), 0u);
    t.call([&] { q->clear(); });
}

BOOST_AUTO_TEST_CASE(blocks_preserve_order_and_byte_content)
{
    IoThread t;
    Pair p(t.io);
    auto q = t.call([&] { return std::make_shared<WriteQueue>(p.server, 8u << 20); });
    std::string want;
    t.call([&] {
        for (std::size_t i = 0; i < 10000; ++i) {
            std::string s(1 + (i * 37 + i / 3) % 300, '\0');
            for (std::size_t k = 0; k < s.size(); ++k)
                s[k] = static_cast<char>((i * 31 + k * 11) & 0xff);
            want += s;
            BOOST_REQUIRE(q->enqueue(makeBuf(s)));
        }
        BOOST_REQUIRE_GT(want.size(), 3u * 65536u);
        q->start([](const boost::system::error_code&) {});
    });
    BOOST_CHECK(p.read(want.size(), slow(std::chrono::seconds(10))) == want);
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return q->messagesWritten(); }) == 10000u; }));
    BOOST_CHECK_EQUAL(t.call([&] { return q->bytesWritten(); }), want.size());
    BOOST_CHECK_EQUAL(t.call([&] { return q->queuedBytes(); }), 0u);
    t.call([&] { q->clear(); });
}
