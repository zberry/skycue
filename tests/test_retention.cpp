#define BOOST_TEST_MODULE skycued_retention
#include <boost/test/unit_test.hpp>

#include "retention.hpp"
#include "temp_dir.hpp"

#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
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

struct TempDir : skycue_test::TempDir {
    TempDir() : skycue_test::TempDir("retention") { ::mkdir((path + "/raw").c_str(), 0755); }

    // A file of `size` bytes at path/rel, last modified `age_s` seconds ago
    // (default: a day, well outside kRecentWindow); returns the full path.
    std::string file(const std::string& rel, std::size_t size, long age_s = 86400) const
    {
        const std::string p = path + "/" + rel;
        std::ofstream(p, std::ios::binary) << std::string(size, 'x');
        setAge(p, age_s);
        return p;
    }
    static void setAge(const std::string& p, long age_s)
    {
        struct timeval tv[2] {};
        ::gettimeofday(&tv[0], nullptr);
        tv[0].tv_sec -= age_s;
        tv[1] = tv[0];
        ::utimes(p.c_str(), tv);
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

BOOST_AUTO_TEST_CASE(capture_and_raw_names_recognised)
{
    for (const char* ok :
         {"capture-ps-north-20261102-101500.jsonl", "raw-ps-north-20261102-101500-2.csv",
          "raw-ps-north-20261102-101500.txt", "raw-bc-20261102-101500.bin", "raw-bc-20261102-101500-3.bin", "capture-a-20261102-101500-12.jsonl",
          "raw-0123456789abcdefghijklmnopqrstuv-20261102-101500.csv", "capture-20261101-20261102-101500.jsonl"})
        BOOST_TEST(isRecordingFileName(ok), ok);
    for (const char* bad :
         {"capture-PS-20261102-101500.jsonl", "raw--20261102-101500.csv", "capture-20261102-101500.jsonl",
          "capture-ps-20261102-101500.csv", "raw-ps-20261102-101500.jsonl", "capture-ps-20261102-101500.bin",
          "raw-ps_n-20261102-101500.csv", "raw-0123456789abcdefghijklmnopqrstuvw-20261102-101500.csv",
          "capture-ps-20261102-101500-.jsonl", "capture-ps-2026110-101500.jsonl", "rawx-ps-20261102-101500.csv",
          "capture-ps.jsonl", "beast-ps-20261102-101500.bin"})
        BOOST_TEST(!isRecordingFileName(bad), bad);
}

// The newest file of each input's stream is protected on its own: raw-a's
// newest does not shield raw-b's older files, nor the other way round.
BOOST_AUTO_TEST_CASE(newest_is_kept_per_input_prefix)
{
    TempDir t;
    t.file("raw-a-20261101-090000.csv", 100);
    t.file("raw-a-20261101-100000.csv", 100);
    t.file("raw-b-20261101-080000.csv", 100);
    t.file("raw-b-20261101-110000.csv", 100);
    t.file("capture-a-20261101-070000.jsonl", 100);
    t.file("capture-a-20261101-120000.jsonl", 100);
    const auto r = enforce(t.path, 1, {});
    const std::vector<std::string> want{"capture-a-20261101-070000.jsonl", "raw-b-20261101-080000.csv",
                                        "raw-a-20261101-090000.csv"};
    BOOST_TEST(names(t, r.deleted) == want, boost::test_tools::per_element());
    BOOST_TEST(t.has("raw-a-20261101-100000.csv"));
    BOOST_TEST(t.has("raw-b-20261101-110000.csv"));
    BOOST_TEST(t.has("capture-a-20261101-120000.jsonl"));
}

BOOST_AUTO_TEST_CASE(retention_never_deletes_recently_modified_file)
{
    TempDir t;
    // An older name stamp but modified a minute ago (a clock step back, or a
    // restart reusing an earlier stamp): another process may be writing it.
    t.file("raw/beast-20261101-080000.bin", 100, 60);
    t.file("raw/beast-20261101-090000.bin", 100);  // old in both name and mtime: deletable
    t.file("raw/beast-20261101-100000.bin", 100);  // newest by name: kept
    t.file("received-20261101-070000.jsonl", 100, 1200);  // 20 min old: outside the window
    t.file("received-20261101-110000.jsonl", 100, 900);  // newest by name and by mtime, not recent
    const auto r = enforce(t.path, 1, {});
    const std::vector<std::string> want{"received-20261101-070000.jsonl", "raw/beast-20261101-090000.bin"};
    BOOST_TEST(names(t, r.deleted) == want, boost::test_tools::per_element());
    BOOST_TEST(t.has("raw/beast-20261101-080000.bin"));
    BOOST_TEST(r.bytes_after == 300u);  // still over: the WARN case
}

BOOST_AUTO_TEST_CASE(newest_by_mtime_is_kept_even_with_an_old_name)
{
    TempDir t;
    // Both outside the window, but the older-named one was written last.
    t.file("uat-20261101-080000.txt", 100, 3600);
    t.file("uat-20261101-090000.txt", 100, 7200);
    t.file("uat-20261101-070000.txt", 100, 9000);
    const auto r = enforce(t.path, 1, {});
    BOOST_TEST(names(t, r.deleted) == std::vector<std::string>{"uat-20261101-070000.txt"},
               boost::test_tools::per_element());
    BOOST_TEST(t.has("uat-20261101-080000.txt"));  // newest by mtime
    BOOST_TEST(t.has("uat-20261101-090000.txt"));  // newest by name
}

BOOST_AUTO_TEST_CASE(raw_symlink_is_not_followed)
{
    TempDir t;
    TempDir elsewhere;
    elsewhere.file("raw/beast-20200101-000000.bin", 1000);
    elsewhere.file("raw/beast-20200102-000000.bin", 1000);
    ::rmdir((t.path + "/raw").c_str());
    BOOST_REQUIRE(::symlink((elsewhere.path + "/raw").c_str(), (t.path + "/raw").c_str()) == 0);
    t.file("received-20261101-090000.jsonl", 100);
    const auto r = enforce(t.path, 1, {});
    BOOST_TEST(r.bytes_before == 100u);
    BOOST_TEST(r.deleted.empty());
    BOOST_TEST(elsewhere.has("raw/beast-20200101-000000.bin"));
}

// The capture of a beast_raw input, raw-<id>-<stamp>.bin, is ordinary
// retention fare: the oldest goes first and the newest of the input stays.
BOOST_AUTO_TEST_CASE(raw_bin_files_are_retained_and_never_the_newest)
{
    TempDir t;
    t.file("raw-bc-20261101-090000.bin", 100);
    t.file("raw-bc-20261101-100000.bin", 100);
    t.file("raw-bc-20261101-110000.bin", 100);
    t.file("raw-bc-20261101-120000.txt", 100);  // another extension of the same input: its own newest
    t.file("raw-other-20261101-080000.bin", 100);  // the newest of its own input: kept
    const auto r = enforce(t.path, 1, {});
    const std::vector<std::string> want{"raw-bc-20261101-090000.bin", "raw-bc-20261101-100000.bin"};
    BOOST_TEST(names(t, r.deleted) == want, boost::test_tools::per_element());
    BOOST_TEST(t.has("raw-bc-20261101-110000.bin"));
    BOOST_TEST(t.has("raw-bc-20261101-120000.txt"));
    BOOST_TEST(t.has("raw-other-20261101-080000.bin"));
    // A file in open_files stays even when it is the oldest.
    TempDir u;
    const std::string open = u.file("raw-bc-20261101-090000.bin", 100);
    u.file("raw-bc-20261101-100000.bin", 100);
    u.file("raw-bc-20261101-110000.bin", 100);
    const auto r2 = enforce(u.path, 1, {open});
    BOOST_TEST(names(u, r2.deleted) == std::vector<std::string>{"raw-bc-20261101-100000.bin"}, boost::test_tools::per_element());
    BOOST_TEST(u.has("raw-bc-20261101-090000.bin"));
}
