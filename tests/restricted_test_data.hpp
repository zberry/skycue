#pragma once
// Test data that are not in the repository: the third-party captures
// (data/raw/*, fisb/tg30.978 and the fisb/received-*.jsonl derived from
// them), which scripts/test-data/fetch-test-captures.sh installs (data/README.md
// says why they are not committed). A test case that reads one declares it:
//
//   BOOST_AUTO_TEST_CASE(name, *skycue_test::needs_test_data("raw/uat-sample.txt.gz"))
//
// and Boost.Test skips that case, naming the file, when it is absent (on
// Boost before 1.74 the case is disabled instead; see below). With the files
// in place every case runs.
// The check uses stat(2), not std::filesystem: GCC 8 (EL8) keeps
// std::filesystem in a separate libstdc++fs that the tests do not link.
#include <boost/test/unit_test.hpp>
#include <boost/version.hpp>

#include <sys/stat.h>

#include <iostream>
#include <string>
#include <utility>

namespace skycue_test {

inline bool test_data_present(const std::string& rel)  // rel: relative to SKYCUE_TEST_DATA_DIR
{
    struct stat st {};
    const std::string path = std::string(SKYCUE_TEST_DATA_DIR) + "/" + rel;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline std::string test_data_absent_message(const std::string& rel)
{
    return "sample " + rel + " not fetched: run scripts/test-data/fetch-test-captures.sh";
}

#if BOOST_VERSION >= 107400
// A failed precondition skips the case and Boost.Test reports it as skipped.
struct TestDataPresent {
    std::string rel;
    boost::test_tools::assertion_result operator()(boost::unit_test::test_unit_id) const
    {
        boost::test_tools::assertion_result r(test_data_present(rel));
        if (!r)
            r.message() << test_data_absent_message(rel);
        return r;
    }
};

inline boost::unit_test::decorator::precondition needs_test_data(std::string rel)
{
    return boost::unit_test::decorator::precondition(TestDataPresent{std::move(rel)});
}
#else
// Older Boost.Test (1.66 on EL8) fails the whole module when a precondition
// skips a case (exit 200), so there the case is disabled when it is
// registered, and the reason printed. The status is set directly (not with
// enable_if's apply_impl) so a case can name several test data.
class needs_test_data : public boost::unit_test::decorator::base {
public:
    explicit needs_test_data(std::string rel) : rel_(std::move(rel)) {}
    void apply(boost::unit_test::test_unit& tu) override
    {
        if (test_data_present(rel_))
            return;
        std::cout << "Test case \"" << tu.p_name.get() << "\" is skipped: " << test_data_absent_message(rel_) << "\n";
        tu.p_default_status.value = boost::unit_test::test_unit::RS_DISABLED;
    }
    boost::unit_test::decorator::base_ptr clone() const override
    {
        return boost::unit_test::decorator::base_ptr(new needs_test_data(*this));
    }

private:
    std::string rel_;
};
#endif

}  // namespace skycue_test
