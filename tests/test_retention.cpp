#define BOOST_TEST_MODULE skycued_retention
#include <boost/test/unit_test.hpp>

#include "retention.hpp"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using skycued::enforce;
using skycued::isRecordingFileName;

namespace {

int removeEntry(const char* path, const struct stat*, int, struct FTW*) { return std::remove(path); }

struct TempDir {
    std::string path;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-retention-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
        ::mkdir((path + "/raw").c_str(), 0755);
    }
    ~TempDir() { ::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS); }

    // A file of `size` bytes at path/rel; returns the full path.
    std::string file(const std::string& rel, std::size_t size) const
    {
        const std::string p = path + "/" + rel;
        std::ofstream(p, std::ios::binary) << std::string(size, 'x');
        return p;
    }
    bool has(const std::string& rel) const
    {
        struct stat st {};
        return ::lstat((path + "/" + rel).c_str(), &st) == 0;
    }
};

std::vector<std::string> names(const TempDir& t, const std::vector<std::string>& paths)
{
    std::vector<std::string> out;
    for (const auto& p : paths)
        out.push_back(p.substr(t.path.size() + 1));
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(file_name_rule)
{
    for (const char* ok : {"received-20261101-120000.jsonl", "sent-20261101-120000.csv", "fisb-20261101-120000.jsonl",
                           "beast-20261101-120000.bin", "uat-20261101-120000.txt", "received-20261101-120000-1.csv",
                           "uat-20261101-120000-12.txt"})
        BOOST_TEST(isRecordingFileName(ok), ok);
    for (const char* bad :
         {"notes.txt", "received.jsonl", "received-2026-120000.jsonl", "received-20261101-12000.jsonl",
          "received-20261101-120000.jsonl.bak", "received-20261101-120000.json", "other-20261101-120000.jsonl",
          "Received-20261101-120000.jsonl", "received-20261101-120000-.jsonl", "received-20261101-120000-x.jsonl",
          "xreceived-20261101-120000.jsonl", "received-20261101-120000.csv.gz", "skycue.runtime.json", ""})
        BOOST_TEST(!isRecordingFileName(bad), bad);
}

BOOST_AUTO_TEST_CASE(zero_cap_is_a_no_op)
{
    TempDir t;
    t.file("received-20261101-100000.jsonl", 1000);
    t.file("received-20261101-110000.jsonl", 1000);
    const auto r = enforce(t.path, 0, {});
    BOOST_TEST(r.deleted.empty());
    BOOST_TEST(t.has("received-20261101-100000.jsonl"));
    BOOST_TEST(t.has("received-20261101-110000.jsonl"));
}

BOOST_AUTO_TEST_CASE(under_the_cap_deletes_nothing)
{
    TempDir t;
    t.file("received-20261101-100000.jsonl", 100);
    t.file("raw/beast-20261101-100000.bin", 100);
    const auto r = enforce(t.path, 200, {});
    BOOST_TEST(r.deleted.empty());
    BOOST_TEST(r.bytes_before == 200u);
    BOOST_TEST(r.bytes_after == 200u);
}

BOOST_AUTO_TEST_CASE(deletes_oldest_first_across_streams_and_raw)
{
    TempDir t;
    // Oldest to newest by stamp; the newest of each stream+ext stays anyway.
    t.file("raw/beast-20261101-080000.bin", 300);
    t.file("received-20261101-090000.jsonl", 100);
    t.file("received-20261101-090000.csv", 100);
    t.file("sent-20261101-093000.jsonl", 50);
    t.file("received-20261101-100000.jsonl", 100);
    t.file("received-20261101-100000.csv", 100);
    t.file("sent-20261101-103000.jsonl", 50);
    t.file("raw/beast-20261101-110000.bin", 300);
    // Total 1100; a cap of 750 needs 350 gone: the 08:00 beast file (300),
    // then the first of the 09:00 pair in name order (csv before jsonl).
    const auto r = enforce(t.path, 750, {});
    BOOST_TEST(r.bytes_before == 1100u);
    BOOST_TEST(r.bytes_after == 700u);
    const std::vector<std::string> want{"raw/beast-20261101-080000.bin", "received-20261101-090000.csv"};
    BOOST_TEST(names(t, r.deleted) == want, boost::test_tools::per_element());
    BOOST_TEST(!t.has("raw/beast-20261101-080000.bin"));
    BOOST_TEST(t.has("received-20261101-090000.jsonl"));
    BOOST_TEST(t.has("raw/beast-20261101-110000.bin"));
}

BOOST_AUTO_TEST_CASE(suffix_orders_after_the_plain_name_numerically)
{
    TempDir t;
    t.file("uat-20261101-120000-10.txt", 10);
    t.file("uat-20261101-120000-2.txt", 10);
    t.file("uat-20261101-120000.txt", 10);
    t.file("uat-20261101-120000-1.txt", 10);
    t.file("uat-20261101-130000.txt", 10);  // newest: kept
    const auto r = enforce(t.path, 10, {});
    const std::vector<std::string> want{"uat-20261101-120000.txt", "uat-20261101-120000-1.txt",
                                        "uat-20261101-120000-2.txt", "uat-20261101-120000-10.txt"};
    BOOST_TEST(names(t, r.deleted) == want, boost::test_tools::per_element());
    BOOST_TEST(r.bytes_after == 10u);
}

BOOST_AUTO_TEST_CASE(other_files_are_neither_counted_nor_touched)
{
    TempDir t;
    t.file("notes.txt", 5000);
    t.file("received-20261101-090000.jsonl.bak", 5000);
    t.file("skycue.runtime.json", 5000);
    t.file("raw/beast-20261101-090000.bin.gz", 5000);
    ::mkdir((t.path + "/sub").c_str(), 0755);
    t.file("sub/received-20261101-080000.jsonl", 5000);         // not dir or dir/raw
    ::mkdir((t.path + "/received-20261101-070000.jsonl").c_str(), 0755);  // a directory with our name
    const std::string target = t.file("notes2.txt", 10);
    BOOST_REQUIRE(::symlink(target.c_str(), (t.path + "/sent-20261101-060000.jsonl").c_str()) == 0);
    t.file("received-20261101-090000.jsonl", 100);
    t.file("received-20261101-100000.jsonl", 100);
    const auto r = enforce(t.path, 100, {});
    BOOST_TEST(r.bytes_before == 200u);
    BOOST_TEST(names(t, r.deleted) == std::vector<std::string>{"received-20261101-090000.jsonl"},
               boost::test_tools::per_element());
    for (const char* keep : {"notes.txt", "received-20261101-090000.jsonl.bak", "skycue.runtime.json",
                             "raw/beast-20261101-090000.bin.gz", "sub/received-20261101-080000.jsonl",
                             "received-20261101-070000.jsonl", "sent-20261101-060000.jsonl", "notes2.txt"})
        BOOST_TEST(t.has(keep), keep);
}

BOOST_AUTO_TEST_CASE(open_files_are_never_deleted)
{
    TempDir t;
    const std::string open_old = t.file("received-20261101-090000.jsonl", 100);
    t.file("received-20261101-100000.jsonl", 100);
    t.file("received-20261101-110000.jsonl", 100);
    // Another spelling of the open file's path still protects it.
    const auto r = enforce(t.path, 1, {t.path + "/./received-20261101-090000.jsonl"});
    BOOST_TEST(names(t, r.deleted) == std::vector<std::string>{"received-20261101-100000.jsonl"},
               boost::test_tools::per_element());
    BOOST_TEST(t.has("received-20261101-090000.jsonl"));
    BOOST_TEST(r.bytes_after == 200u);  // above the cap: only protected files are left
}

BOOST_AUTO_TEST_CASE(newest_of_each_stream_and_extension_is_kept)
{
    TempDir t;
    t.file("received-20261101-090000.jsonl", 100);
    t.file("received-20261101-090000.csv", 100);
    t.file("sent-20261101-090000.jsonl", 100);
    t.file("raw/beast-20261101-090000.bin", 100);
    t.file("raw/uat-20261101-090000.txt", 100);
    const auto r = enforce(t.path, 1, {});
    BOOST_TEST(r.deleted.empty());
    BOOST_TEST(r.bytes_after == 500u);
}
