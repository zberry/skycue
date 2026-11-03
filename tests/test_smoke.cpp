#define BOOST_TEST_MODULE skycue_smoke
#include <boost/test/unit_test.hpp>

#include <boost/version.hpp>

#include <chrono>
#include <cstdlib>

#include "test_slow.hpp"

BOOST_AUTO_TEST_CASE(boost_version_floor) { BOOST_TEST(BOOST_VERSION >= 106600); }

BOOST_AUTO_TEST_CASE(slow_factor_reads_env)
{
    using namespace std::chrono_literals;
    ::unsetenv("SKYCUE_TEST_SLOW");
    BOOST_TEST(testSlowFactor() == 1);
    ::setenv("SKYCUE_TEST_SLOW", "4", 1);
    BOOST_TEST(testSlowFactor() == 4);
    BOOST_TEST(slow(5s).count() == 20000);
    BOOST_TEST(slow(250ms).count() == 1000);
    for (const char* bad : {"0", "-2", "abc", "4x", ""}) {
        ::setenv("SKYCUE_TEST_SLOW", bad, 1);
        BOOST_TEST(testSlowFactor() == 1);
    }
    ::setenv("SKYCUE_TEST_SLOW", "100", 1);
    BOOST_TEST(testSlowFactor() == 20);  // clamped
    ::unsetenv("SKYCUE_TEST_SLOW");
}
