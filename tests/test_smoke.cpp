#define BOOST_TEST_MODULE skycue_smoke
#include <boost/test/unit_test.hpp>

#include <boost/version.hpp>

BOOST_AUTO_TEST_CASE(boost_version_floor) { BOOST_TEST(BOOST_VERSION >= 106600); }
