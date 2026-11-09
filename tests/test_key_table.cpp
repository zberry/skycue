// KeyTable: pointer stability, LRU order and idle eviction.
#define BOOST_TEST_MODULE key_table
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>

#include "decode/key_table.hpp"

using namespace skycued::decode;
using std::chrono::seconds;

namespace {

struct S : KeyBase {
    int tag = 0;
};

const TimePoint t0 = TimePoint{} + std::chrono::hours(100000);
constexpr auto kSrc = skycue::Source::adsb_1090;

StateKey key(uint32_t a) {
    StateKey k;
    k.address = a;
    return k;
}

}  // namespace

BOOST_AUTO_TEST_CASE(pointer_stays_valid_across_obtain) {
    uint64_t evicted = 0;
    KeyTable<S> t(0, seconds(300), evicted);
    S* p = &t.obtain(key(1), kSrc, t0);
    p->tag = 7;
    for (uint32_t i = 2; i < 502; ++i) t.obtain(key(i), kSrc, t0);
    BOOST_CHECK_EQUAL(t.size(), 501u);
    BOOST_CHECK(&t.obtain(key(1), kSrc, t0) == p);
    BOOST_CHECK(t.find(key(1)) == p);
    BOOST_CHECK_EQUAL(p->tag, 7);
    BOOST_CHECK_EQUAL(evicted, 0u);
}

BOOST_AUTO_TEST_CASE(pointer_is_valid_until_the_key_is_evicted) {
    uint64_t evicted = 0;
    KeyTable<S> t(0, seconds(300), evicted);
    S* p = &t.obtain(key(1), kSrc, t0);
    p->tag = 7;
    t.obtain(key(2), kSrc, t0 + seconds(400));
    t.evictIdle(t0 + seconds(400));
    BOOST_CHECK(t.find(key(1)) == nullptr);
    BOOST_CHECK(t.find(key(2)) != nullptr);
    BOOST_CHECK_EQUAL(evicted, 1u);
}

BOOST_AUTO_TEST_CASE(lru_touch_moves_to_young_end) {
    uint64_t evicted = 0;
    KeyTable<S> t(2, seconds(300), evicted);
    t.obtain(key(1), kSrc, t0);
    t.obtain(key(2), kSrc, t0);
    t.obtain(key(1), kSrc, t0);
    t.obtain(key(3), kSrc, t0);
    BOOST_CHECK(t.find(key(2)) == nullptr);
    BOOST_CHECK(t.find(key(1)) != nullptr);
    BOOST_CHECK(t.find(key(3)) != nullptr);
    BOOST_CHECK_EQUAL(evicted, 1u);
    BOOST_CHECK_EQUAL(t.size(), 2u);
}

BOOST_AUTO_TEST_CASE(evict_idle_boundary) {
    uint64_t evicted = 0;
    KeyTable<S> t(0, seconds(300), evicted);
    t.obtain(key(1), kSrc, t0);
    t.evictIdle(t0 + seconds(300));
    BOOST_CHECK(t.find(key(1)) != nullptr);
    BOOST_CHECK_EQUAL(evicted, 0u);
    t.evictIdle(t0 + seconds(301));
    BOOST_CHECK(t.find(key(1)) == nullptr);
    BOOST_CHECK_EQUAL(evicted, 1u);
}

BOOST_AUTO_TEST_CASE(obtain_updates_source_and_last_seen) {
    uint64_t evicted = 0;
    KeyTable<S> t(0, seconds(300), evicted);
    S& a = t.obtain(key(1), skycue::Source::adsb_1090, t0);
    BOOST_CHECK(a.source == skycue::Source::adsb_1090);
    S& b = t.obtain(key(1), skycue::Source::uat_978, t0 + seconds(5));
    BOOST_CHECK(&a == &b);
    BOOST_CHECK(b.source == skycue::Source::uat_978);
    BOOST_CHECK(b.last_seen == t0 + seconds(5));
}

BOOST_AUTO_TEST_CASE(find_does_not_touch_lru) {
    uint64_t evicted = 0;
    KeyTable<S> t(2, seconds(300), evicted);
    t.obtain(key(1), kSrc, t0);
    t.obtain(key(2), kSrc, t0);
    BOOST_CHECK(t.find(key(1)) != nullptr);
    t.obtain(key(3), kSrc, t0);
    BOOST_CHECK(t.find(key(1)) == nullptr);
    BOOST_CHECK(t.find(key(2)) != nullptr);
}

BOOST_AUTO_TEST_CASE(pointer_to_a_surviving_key_stays_valid_across_an_eviction) {
    uint64_t evicted = 0;
    KeyTable<S> t(2, seconds(300), evicted);
    t.obtain(key(1), kSrc, t0);
    S* p = &t.obtain(key(2), kSrc, t0);
    p->tag = 9;
    t.obtain(key(3), kSrc, t0);  // full: key 1, the oldest, goes
    BOOST_CHECK_EQUAL(evicted, 1u);
    BOOST_CHECK(t.find(key(1)) == nullptr);
    BOOST_CHECK(t.find(key(2)) == p);
    BOOST_CHECK_EQUAL(p->tag, 9);
    BOOST_CHECK(&t.obtain(key(2), kSrc, t0) == p);
    BOOST_CHECK_EQUAL(t.size(), 2u);
}
