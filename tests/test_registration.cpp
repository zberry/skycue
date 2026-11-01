#define BOOST_TEST_MODULE skycued_registration
#include <boost/test/unit_test.hpp>

#include "registration.hpp"

#include <cstdint>
#include <string>

namespace reg = skycued::registration;

namespace {

struct Vector {
    uint32_t icao;
    const char* n;
};

const Vector kVectors[] = {
    {0xA00001, "N1"},    {0xA00002, "N1A"},  {0xA00003, "N1AA"}, {0xA0001A, "N1AZ"},  {0xA0001B, "N1B"},
    {0xA00259, "N1ZZ"},  {0xA0025A, "N10"},  {0xA18D50, "N2"},   {0xADF7C7, "N99999"},
};

} // namespace

BOOST_AUTO_TEST_CASE(icao_to_n_number_vectors)
{
    for (const auto& v : kVectors) {
        BOOST_TEST_CONTEXT("icao " << std::hex << v.icao)
        {
            const auto n = reg::nNumberFromIcao(v.icao);
            BOOST_REQUIRE(n.has_value());
            BOOST_TEST(*n == v.n);
        }
    }
}

BOOST_AUTO_TEST_CASE(n_number_to_icao_vectors)
{
    for (const auto& v : kVectors) {
        BOOST_TEST_CONTEXT("n " << v.n)
        {
            const auto icao = reg::icaoFromNNumber(v.n);
            BOOST_REQUIRE(icao.has_value());
            BOOST_TEST(*icao == v.icao);
        }
    }
}

BOOST_AUTO_TEST_CASE(outside_us_range_is_nullopt)
{
    BOOST_TEST(!reg::nNumberFromIcao(0xA00000).has_value());
    BOOST_TEST(!reg::nNumberFromIcao(0xADF7C8).has_value());
    BOOST_TEST(!reg::nNumberFromIcao(0x000000).has_value());
    BOOST_TEST(!reg::nNumberFromIcao(0xFFFFFF).has_value());
}

BOOST_AUTO_TEST_CASE(more_vectors)
{
    // Four- and five-digit tails, and the boundary between N1's tree and N2's.
    BOOST_TEST(*reg::nNumberFromIcao(0xA18D4F) == "N19999");
    BOOST_TEST(*reg::icaoFromNNumber("N1000") + 1 == *reg::icaoFromNNumber("N1000A"));
    BOOST_TEST(*reg::icaoFromNNumber("N1000Z") + 1 == *reg::icaoFromNNumber("N10000"));
    BOOST_TEST(*reg::icaoFromNNumber("N10009") + 1 == *reg::icaoFromNNumber("N1001"));
    BOOST_TEST(*reg::icaoFromNNumber("N1ZZ") + 1 == *reg::icaoFromNNumber("N10"));
    BOOST_TEST(*reg::icaoFromNNumber("N1HZ") + 1 == *reg::icaoFromNNumber("N1J"));
}

BOOST_AUTO_TEST_CASE(round_trip_sweep_whole_us_range)
{
    uint32_t failures = 0;
    for (uint32_t icao = 0xA00001; icao <= 0xADF7C7; ++icao) {
        const auto n = reg::nNumberFromIcao(icao);
        if (!n) {
            ++failures;
            continue;
        }
        const auto back = reg::icaoFromNNumber(*n);
        if (!back || *back != icao)
            ++failures;
    }
    BOOST_TEST(failures == 0u);
}

BOOST_AUTO_TEST_CASE(lowercase_and_missing_n_accepted)
{
    BOOST_TEST(*reg::icaoFromNNumber("n1aa") == 0xA00003u);
    BOOST_TEST(*reg::icaoFromNNumber("1AA") == 0xA00003u);
    BOOST_TEST(*reg::icaoFromNNumber("1aZ") == 0xA0001Au);
    BOOST_TEST(*reg::icaoFromNNumber("99999") == 0xADF7C7u);
}

BOOST_AUTO_TEST_CASE(rejects_invalid)
{
    for (const char* bad : {"N1I", "N1O", "N1AI", "N0", "N01", "N100000", "N1ABC", "N12345A", "N1234AB", "", "N",
                            "NA", "N1A1", "N-1", "N1 ", "NN1"}) {
        BOOST_TEST_CONTEXT("n '" << bad << "'") { BOOST_TEST(!reg::icaoFromNNumber(bad).has_value()); }
    }
}
