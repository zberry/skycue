#define BOOST_TEST_MODULE skycued_log
#include <boost/test/unit_test.hpp>

#include "log.hpp"

#include <chrono>

BOOST_AUTO_TEST_CASE(log_line_format)
{
    using namespace std::chrono;
    // 2026-11-01T12:34:56.789Z
    system_clock::time_point t{milliseconds(1793536496789LL)};
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::info, "hello 42", t) ==
               "2026-11-01T12:34:56.789Z INFO  hello 42\n");
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::error, "x", t) ==
               "2026-11-01T12:34:56.789Z ERROR x\n");
    skycued::setLogLevel(skycued::LogLevel::warn);
    BOOST_TEST(!skycued::logEnabled(skycued::LogLevel::info));
    BOOST_TEST(skycued::logEnabled(skycued::LogLevel::error));
    int evaluated = 0;
    LOG_INFO("not built " << ++evaluated);
    BOOST_TEST(evaluated == 0);
    LOG_WARN("test_log log line " << ++evaluated);
    BOOST_TEST(evaluated == 1);
    skycued::setLogLevel(skycued::LogLevel::info);
}
