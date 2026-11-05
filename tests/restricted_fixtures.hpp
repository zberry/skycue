#pragma once
// Fixtures that the release source tarball leaves out (scripts/make-source-tarball.sh):
// the third-party captures in fixtures/raw/ and the vendor ICD examples
// fixtures/pingstation/icd-*.json (fixtures/README.md says why). A test case that
// reads one declares it:
//
//   BOOST_AUTO_TEST_CASE(name, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
//
// and Boost.Test skips that case, naming the file, when it is absent (on
// Boost before 1.74 the case is disabled instead; see below). In a git
// checkout the files are there and every case runs.
// The check uses stat(2), not std::filesystem: GCC 8 (EL8) keeps
// std::filesystem in a separate libstdc++fs that the tests do not link.
#include <boost/test/unit_test.hpp>
#include <boost/version.hpp>

#include <sys/stat.h>

#include <iostream>
#include <string>
#include <utility>

namespace skycue_test {

inline bool fixture_present(const std::string& rel)  // rel: relative to SKYCUE_FIXTURE_DIR
{
    struct stat st {};
    const std::string path = std::string(SKYCUE_FIXTURE_DIR) + "/" + rel;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline std::string fixture_absent_message(const std::string& rel)
{
    return "fixture " + rel + " is not in this source tree (the release tarball leaves it out)";
}

#if BOOST_VERSION >= 107400
// A failed precondition skips the case and Boost.Test reports it as skipped.
struct FixturePresent {
    std::string rel;
    boost::test_tools::assertion_result operator()(boost::unit_test::test_unit_id) const
    {
        boost::test_tools::assertion_result r(fixture_present(rel));
        if (!r)
            r.message() << fixture_absent_message(rel);
        return r;
    }
};

inline boost::unit_test::decorator::precondition needs_fixture(std::string rel)
{
    return boost::unit_test::decorator::precondition(FixturePresent{std::move(rel)});
}
#else
// Older Boost.Test (1.66 on EL8) fails the whole module when a precondition
// skips a case (exit 200), so there the case is disabled when it is
// registered, and the reason printed.
class needs_fixture : public boost::unit_test::decorator::enable_if_impl {
public:
    explicit needs_fixture(std::string rel) : rel_(std::move(rel)) {}
    void apply(boost::unit_test::test_unit& tu) override
    {
        const bool present = fixture_present(rel_);
        if (!present)
            std::cout << "Test case \"" << tu.p_name.get() << "\" is skipped: " << fixture_absent_message(rel_) << "\n";
        apply_impl(tu, present);
    }
    boost::unit_test::decorator::base_ptr clone() const override
    {
        return boost::unit_test::decorator::base_ptr(new needs_fixture(*this));
    }

private:
    std::string rel_;
};
#endif

}  // namespace skycue_test
