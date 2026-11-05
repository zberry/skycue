#pragma once
// Fixtures that the release source tarball leaves out (scripts/make-source-tarball.sh):
// the third-party captures in fixtures/raw/ and the vendor ICD examples
// fixtures/pingstation/icd-*.json (fixtures/README.md says why). A test case that
// reads one declares it:
//
//   BOOST_AUTO_TEST_CASE(name, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
//
// and Boost.Test skips that case, naming the file, when it is absent. In a git
// checkout the files are there and every case runs.
// The check uses stat(2), not std::filesystem: GCC 8 (EL8) keeps
// std::filesystem in a separate libstdc++fs that the tests do not link.
#include <boost/test/unit_test.hpp>

#include <sys/stat.h>

#include <string>
#include <utility>

namespace skycue_test {

struct FixturePresent {
    std::string rel;  // relative to SKYCUE_FIXTURE_DIR
    boost::test_tools::assertion_result operator()(boost::unit_test::test_unit_id) const
    {
        struct stat st {};
        const std::string path = std::string(SKYCUE_FIXTURE_DIR) + "/" + rel;
        const bool present = ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        boost::test_tools::assertion_result r(present);
        if (!present)
            r.message() << "fixture " << rel << " is not in this source tree (the release tarball leaves it out)";
        return r;
    }
};

inline boost::unit_test::decorator::precondition needs_fixture(std::string rel)
{
    return boost::unit_test::decorator::precondition(FixturePresent{std::move(rel)});
}

}  // namespace skycue_test
