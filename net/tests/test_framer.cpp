#define BOOST_TEST_MODULE net_framer
#include <boost/test/unit_test.hpp>

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/framer.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace skycue::net;

namespace {
struct Collector {
    std::vector<std::string> msgs;
    Framer::Out out()
    {
        return [this](const uint8_t* d, std::size_t n) {
            msgs.emplace_back(reinterpret_cast<const char*>(d), n);
        };
    }
};
void feed(Framer& f, const std::string& s, const Framer::Out& out)
{
    f.feed(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
}
} // namespace

BOOST_AUTO_TEST_CASE(line_framer_splits_and_strips_cr)
{
    LineFramer f;
    Collector c;
    feed(f, "a\r\nb\n\n c\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 3u);
    BOOST_TEST(c.msgs[0] == "a");
    BOOST_TEST(c.msgs[1] == "b");
    BOOST_TEST(c.msgs[2] == " c");
}

BOOST_AUTO_TEST_CASE(line_framer_strips_midline_cr)
{
    // Unlike line_client.cpp, every CR is dropped, not only a trailing one.
    LineFramer f;
    Collector c;
    feed(f, "a\rb\n\r\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 1u);
    BOOST_TEST(c.msgs[0] == "ab");
}

BOOST_AUTO_TEST_CASE(line_framer_joins_across_chunks)
{
    LineFramer f;
    Collector c;
    feed(f, "ab", c.out());
    feed(f, "c\nd", c.out());
    feed(f, "\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 2u);
    BOOST_TEST(c.msgs[0] == "abc");
    BOOST_TEST(c.msgs[1] == "d");
}

BOOST_AUTO_TEST_CASE(line_framer_drops_oversize_keeps_rest)
{
    LineFramer f(8);
    Collector c;
    feed(f, std::string(20, 'x'), c.out());
    feed(f, "\nok\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 1u);
    BOOST_TEST(c.msgs[0] == "ok");
    BOOST_TEST(f.rejected() == 1u);
}

BOOST_AUTO_TEST_CASE(line_framer_reset_drops_partial)
{
    LineFramer f;
    Collector c;
    feed(f, "abc", c.out());
    f.reset();
    feed(f, "d\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 1u);
    BOOST_TEST(c.msgs[0] == "d");
}

BOOST_AUTO_TEST_CASE(datagram_framer_passes_through)
{
    DatagramFramer f;
    Collector c;
    feed(f, "a\nb\r\n", c.out());
    BOOST_REQUIRE_EQUAL(c.msgs.size(), 1u);
    BOOST_TEST(c.msgs[0] == "a\nb\r\n");
}

BOOST_AUTO_TEST_CASE(make_buf_copies_bytes)
{
    const uint8_t raw[] = {1, 2, 3};
    Buf a = makeBuf(raw, 3);
    Buf b = makeBuf(std::string_view("xyz"));
    BOOST_REQUIRE(a);
    BOOST_REQUIRE(b);
    BOOST_TEST(a->size() == 3u);
    BOOST_TEST((*a)[2] == 3);
    BOOST_TEST(b->size() == 3u);
    BOOST_TEST((*b)[0] == 'x');
    Counters k;
    BOOST_TEST(k.bytes_in == 0u);
    BOOST_TEST(k.peers == 0u);
}
