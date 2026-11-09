#define BOOST_TEST_MODULE skycued_support_headers
#include <boost/test/unit_test.hpp>

#include "fixture_io.hpp"
#include "temp_dir.hpp"
#include "test_util.hpp"

#include <sys/stat.h>
#include <zlib.h>

#include <fstream>
#include <string>
#include <vector>

using skycue_test::fileExists;
using skycue_test::fileReadable;
using skycue_test::forEachLine;
using skycue_test::loadJsonl;
using skycue_test::TempDir;

namespace {

bool isDir(const std::string& p)
{
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void writeGz(const std::string& path, const std::string& text)
{
    gzFile f = gzopen(path.c_str(), "wb");
    BOOST_REQUIRE(f);
    gzwrite(f, text.data(), static_cast<unsigned>(text.size()));
    gzclose(f);
}

}  // namespace

BOOST_AUTO_TEST_CASE(temp_dir_lives_for_its_scope)
{
    std::string kept;
    {
        TempDir t;
        kept = t.path;
        BOOST_TEST(isDir(kept));
        std::ofstream(kept + "/f") << "x";
        BOOST_TEST(fileExists(kept + "/f"));
    }
    BOOST_TEST(!fileExists(kept));
}

BOOST_AUTO_TEST_CASE(temp_dir_removes_nested_trees)
{
    std::string kept;
    {
        TempDir t;
        kept = t.path;
        BOOST_REQUIRE(::mkdir((kept + "/a").c_str(), 0755) == 0);
        BOOST_REQUIRE(::mkdir((kept + "/a/b").c_str(), 0755) == 0);
        std::ofstream(kept + "/a/b/f") << "x";
        BOOST_TEST(fileExists(kept + "/a/b/f"));
    }
    BOOST_TEST(!fileExists(kept + "/a/b/f"));
    BOOST_TEST(!fileExists(kept + "/a/b"));
    BOOST_TEST(!fileExists(kept + "/a"));
    BOOST_TEST(!fileExists(kept));
}

BOOST_AUTO_TEST_CASE(temp_dir_name_has_the_tag)
{
    TempDir t("x");
    BOOST_TEST(t.path.find("skycue-x-") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(file_predicates)
{
    TempDir t;
    const std::string f = t.path + "/a.txt";
    BOOST_TEST(!fileExists(f));
    BOOST_TEST(!fileReadable(f));
    std::ofstream(f) << "x";
    BOOST_TEST(fileExists(f));
    BOOST_TEST(fileReadable(f));
    BOOST_TEST(fileExists(t.path));
    // An ifstream opens a directory on Linux (reading it fails later), so it counts as good.
    BOOST_TEST(fileReadable(t.path));
}

BOOST_AUTO_TEST_CASE(for_each_line_in_order_and_stops)
{
    TempDir t;
    const std::string gz = t.path + "/l.gz";
    writeGz(gz, "one\ntwo\nthree\n");
    std::vector<std::string> all;
    forEachLine(gz, [&](const std::string& l) { all.push_back(l); });
    BOOST_TEST((all == std::vector<std::string>{"one", "two", "three"}));
    std::vector<std::string> some;
    forEachLine(gz, [&](const std::string& l) {
        some.push_back(l);
        return l != "two";
    });
    BOOST_TEST((some == std::vector<std::string>{"one", "two"}));
}

BOOST_AUTO_TEST_CASE(load_jsonl_skips_a_bad_line)
{
    TempDir t;
    const std::string gz = t.path + "/j.gz";
    writeGz(gz, "{\"hex\":\"a\"}\nnot json\n{\"hex\":\"b\"}\n");
    const auto rows = loadJsonl(gz);
    BOOST_REQUIRE_EQUAL(rows.size(), 2u);
    BOOST_TEST(rows[0].getString("hex").value_or("") == "a");
    BOOST_TEST(rows[1].getString("hex").value_or("") == "b");
}
