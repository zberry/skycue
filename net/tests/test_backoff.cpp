#define BOOST_TEST_MODULE net_backoff
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/backoff.hpp"

#include <atomic>
#include <chrono>

using namespace skycue::net;
using std::chrono::milliseconds;

BOOST_AUTO_TEST_CASE(backoff_doubles_and_caps)
{
    Backoff b;
    for (int want : {500, 1000, 2000, 4000, 8000, 10000, 10000})
        BOOST_TEST(b.next().count() == want);
    b.reset();
    BOOST_TEST(b.next().count() == 500);
}

BOOST_AUTO_TEST_CASE(retry_timer_fires_once_and_cancel_prevents)
{
    test::IoThread t;
    RetryTimer timer(t.io);
    std::atomic<int> fired{0};

    t.call([&] { timer.after(milliseconds(20), [&] { ++fired; }); });
    BOOST_TEST(test::eventually([&] { return fired.load() == 1; }));
    std::this_thread::sleep_for(milliseconds(100));
    BOOST_TEST(fired.load() == 1);

    t.call([&] {
        timer.after(milliseconds(20), [&] { fired += 10; });
        timer.cancel();
    });
    std::this_thread::sleep_for(milliseconds(100));
    BOOST_TEST(fired.load() == 1);

    std::atomic<int> first{0}, second{0};
    t.call([&] {
        timer.after(milliseconds(20), [&] { ++first; });
        timer.after(milliseconds(20), [&] { ++second; });
    });
    BOOST_TEST(test::eventually([&] { return second.load() == 1; }));
    std::this_thread::sleep_for(milliseconds(100));
    BOOST_TEST(first.load() == 0);
    BOOST_TEST(second.load() == 1);
}
