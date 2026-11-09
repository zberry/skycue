#define BOOST_TEST_MODULE file_util
#include <boost/test/unit_test.hpp>

#include "file_util.hpp"
#include "temp_dir.hpp"
#include "test_util.hpp"
#include "time_util.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <thread>

using skycue_test::fileExists;
using skycue_test::TempDir;
using skycued::ReadStatus;

namespace {

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

BOOST_AUTO_TEST_CASE(read_regular_ok)
{
    TempDir d;
    std::string body;
    body.assign("line one\nline two\0tail", 22);
    writeFile(d.path + "/a", body);
    std::string out = "old";
    BOOST_TEST((skycued::readRegularFile(d.path + "/a", 1024, out) == ReadStatus::ok));
    BOOST_TEST(out == body);
}

BOOST_AUTO_TEST_CASE(read_missing_is_missing_with_enoent)
{
    TempDir d;
    std::string out = "keep";
    errno = 0;
    BOOST_TEST((skycued::readRegularFile(d.path + "/none", 1024, out) == ReadStatus::missing));
    BOOST_TEST(errno == ENOENT);
    BOOST_TEST(out == "keep");
    writeFile(d.path + "/file", "x");
    errno = 0;
    BOOST_TEST((skycued::readRegularFile(d.path + "/file/below", 1024, out) == ReadStatus::missing));
    BOOST_TEST(errno == ENOTDIR);
}

BOOST_AUTO_TEST_CASE(read_directory_is_missing_with_eisdir)
{
    TempDir d;
    BOOST_REQUIRE(::mkdir((d.path + "/dir").c_str(), 0755) == 0);
    std::string out = "keep";
    errno = 0;
    BOOST_TEST((skycued::readRegularFile(d.path + "/dir", 1024, out) == ReadStatus::missing));
    BOOST_TEST(errno == EISDIR);
    BOOST_TEST(out == "keep");
}

BOOST_AUTO_TEST_CASE(read_fifo_is_missing_with_einval_and_does_not_block)
{
    TempDir d;
    const std::string fifo = d.path + "/pipe";
    BOOST_REQUIRE(::mkfifo(fifo.c_str(), 0644) == 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        std::string out;
        errno = 0;
        const bool ok = skycued::readRegularFile(fifo, 1024, out) == ReadStatus::missing && errno == EINVAL;
        ::_exit(ok ? 0 : 1);
    }
    int status = 0;
    bool exited = false;
    for (int i = 0; i < 100 && !exited; ++i) {  // 5 s
        exited = ::waitpid(pid, &status, WNOHANG) == pid;
        if (!exited)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    BOOST_REQUIRE_MESSAGE(exited, "readRegularFile blocked on a FIFO");
    BOOST_TEST((WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

BOOST_AUTO_TEST_CASE(read_empty_ok_and_empty_text)
{
    TempDir d;
    writeFile(d.path + "/empty", "");
    std::string out = "old";
    BOOST_TEST((skycued::readRegularFile(d.path + "/empty", 0, out) == ReadStatus::ok));
    BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_CASE(read_exactly_max_ok_and_one_more_too_large)
{
    TempDir d;
    writeFile(d.path + "/max", std::string(100, 'x'));
    writeFile(d.path + "/over", std::string(101, 'x'));
    std::string out;
    BOOST_TEST((skycued::readRegularFile(d.path + "/max", 100, out) == ReadStatus::ok));
    BOOST_TEST(out.size() == 100u);
    out = "keep";
    BOOST_TEST((skycued::readRegularFile(d.path + "/over", 100, out) == ReadStatus::too_large));
    BOOST_TEST(out == "keep");
}

BOOST_AUTO_TEST_CASE(read_unreadable_is_error_with_eacces)
{
    TempDir d;
    writeFile(d.path + "/secret", "s");
    BOOST_REQUIRE(::chmod((d.path + "/secret").c_str(), 0) == 0);
    if (::access((d.path + "/secret").c_str(), R_OK) == 0) {
        BOOST_TEST_MESSAGE("running with read access to mode 000 files: skipped");
        return;
    }
    std::string out = "keep";
    errno = 0;
    BOOST_TEST((skycued::readRegularFile(d.path + "/secret", 1024, out) == ReadStatus::error));
    BOOST_TEST(errno == EACCES);
    BOOST_TEST(out == "keep");
}

BOOST_AUTO_TEST_CASE(set_aside_renames_and_reports)
{
    TempDir d;
    writeFile(d.path + "/f", "data");
    std::string why;
    BOOST_TEST(skycued::setAside(d.path + "/f", ".bad", why));
    BOOST_TEST(!fileExists(d.path + "/f"));
    BOOST_TEST(fileExists(d.path + "/f.bad"));
    BOOST_TEST(why.empty());
    BOOST_TEST(!skycued::setAside(d.path + "/f", ".bad", why));
    BOOST_TEST(why == "No such file or directory");
}

BOOST_AUTO_TEST_CASE(format_utc_known_instants)
{
    using std::chrono::seconds;
    using std::chrono::system_clock;
    BOOST_TEST(skycued::formatUtc(system_clock::time_point{}, "%Y-%m-%dT%H:%M:%SZ") == "1970-01-01T00:00:00Z");
    BOOST_TEST(skycued::formatUtc(system_clock::time_point{seconds(1793620800)}, "%Y%m%dT%H%M%SZ") ==
               "20261102T120000Z");
    // Longer than the 64-byte buffer: empty.
    BOOST_TEST(skycued::formatUtc(system_clock::time_point{}, "%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y%Y").empty());
}

BOOST_AUTO_TEST_CASE(is_directory)
{
    TempDir d;
    writeFile(d.path + "/f", "x");
    BOOST_TEST(skycued::isDirectory(d.path));
    BOOST_TEST(!skycued::isDirectory(d.path + "/f"));
    BOOST_TEST(!skycued::isDirectory(d.path + "/none"));
}
