// skycue-rawrec: records the raw receiver streams for later replay with
// skycue-rawplay.
//
//   --beast HOST:PORT  readsb's Beast output (30005): written verbatim to
//                      <dir>/beast-YYYYMMDD-HHMMSS.bin
//   --uat HOST:PORT    dump978-fa's raw port (30978): each line written as
//                      "<epoch_ms> <line>" to <dir>/uat-YYYYMMDD-HHMMSS.txt
//
// Files rotate like the Recorder's (--rotate-mb, --rotate-min); a Beast file
// is only ever cut at a frame start. A write failure (a full disk) drops data
// with the Recorder's backoff (5 s doubling to 300 s, one file name reused per
// failure streak). With --max-gb, each new file runs retention over
// --retention-dir (default --dir) and its raw/ (see retention.hpp). Each input
// reconnects with a backoff from 0.5 s to 10 s. SIGINT/SIGTERM flush the files
// and exit 0.
// Single-threaded: everything runs on one io_context.

#include "log.hpp"
#include "raw_formats.hpp"
#include "retention.hpp"
#include "rotating_file.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace po = boost::program_options;
using asio::ip::tcp;
using error_code = boost::system::error_code;
using namespace skycued;

namespace {

constexpr std::chrono::milliseconds kMinBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
constexpr std::chrono::seconds kFlushInterval{1};
constexpr std::size_t kMaxUatLine = 64 * 1024;

struct HostPort {
    std::string host;
    std::string port;
};

std::optional<HostPort> parseHostPort(const std::string& s)
{
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == s.size())
        return std::nullopt;
    std::string host = s.substr(0, colon), port = s.substr(colon + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    const long p = std::stol(port);
    if (p < 1 || p > 65535)
        return std::nullopt;
    return HostPort{host, port};
}

// One output stream. RotatingFile handles write failures (drops data with a
// 5 s .. 300 s backoff and reuses one file name per failure streak, as the
// Recorder does); this logs file changes and runs retention for new files.
class Sink {
public:
    Sink(RotatingFileConfig cfg, std::function<void()> on_new_file)
        : file_(std::move(cfg)), on_new_file_(std::move(on_new_file))
    {
    }

    bool rotationDue() const { return file_.rotationDue(); }
    void rotate()
    {
        const std::string old = file_.currentPath();
        file_.rotate();
        if (!old.empty())
            LOG_INFO("closed " << old);
    }
    void write(const char* data, std::size_t n)
    {
        if (!file_.write(data, n))
            return;
        const std::string now = file_.currentPath();
        if (now != last_path_) {  // a new file (not a streak's reopen of the same one)
            last_path_ = now;
            LOG_INFO("writing " << now);
            if (on_new_file_)
                on_new_file_();
        }
    }
    void flush() { file_.flush(); }
    std::string currentPath() const { return file_.currentPath(); }
    void setFailWritesForTest(bool fail) { file_.setFailWritesForTest(fail); }

private:
    RotatingFile file_;
    std::function<void()> on_new_file_;
    std::string last_path_;
};

// One TCP input, reconnecting with backoff.
class Input {
public:
    enum class Kind { beast, uat };

    Input(asio::io_context& io, Kind kind, HostPort where, Sink& sink)
        : kind_(kind), where_(std::move(where)), sink_(sink), resolver_(io), sock_(io), timer_(io)
    {
    }

    void start() { connect(); }
    void stop()
    {
        stopping_ = true;
        error_code ignored;
        timer_.cancel(ignored);
        resolver_.cancel();
        sock_.close(ignored);
    }

private:
    const char* name() const { return kind_ == Kind::beast ? "beast" : "uat"; }

    void connect()
    {
        if (stopping_)
            return;
        resolver_.async_resolve(where_.host, where_.port, [this](const error_code& ec, const tcp::resolver::results_type& r) {
            if (stopping_)
                return;
            if (ec) {
                retry("resolve: " + ec.message());
                return;
            }
            asio::async_connect(sock_, r, [this](const error_code& cec, const tcp::endpoint&) {
                if (stopping_)
                    return;
                if (cec) {
                    retry(cec.message());
                    return;
                }
                LOG_INFO(name() << ": connected to " << where_.host << ":" << where_.port);
                backoff_ = kMinBackoff;
                splitter_.reset();
                line_.clear();
                read();
            });
        });
    }

    void retry(const std::string& why)
    {
        error_code ignored;
        sock_.close(ignored);
        if (stopping_)
            return;
        if (!quiet_)
            LOG_WARN(name() << ": " << where_.host << ":" << where_.port << ": " << why << "; retrying in "
                            << backoff_.count() << " ms (further failures logged at debug level)");
        else
            LOG_DEBUG(name() << ": " << why << "; retrying in " << backoff_.count() << " ms");
        quiet_ = true;
        timer_.expires_after(backoff_);
        backoff_ = std::min(backoff_ * 2, kMaxBackoff);
        timer_.async_wait([this](const error_code& ec) {
            if (!ec)
                connect();
        });
    }

    void read()
    {
        sock_.async_read_some(asio::buffer(buf_), [this](const error_code& ec, std::size_t n) {
            if (stopping_)
                return;
            if (n > 0)
                onData(buf_.data(), n);
            if (ec) {
                LOG_INFO(name() << ": connection closed (" << ec.message() << ")");
                quiet_ = false;
                retry("disconnected");
                return;
            }
            read();
        });
    }

    void onData(const uint8_t* data, std::size_t n)
    {
        if (kind_ == Kind::beast)
            onBeast(data, n);
        else
            onUat(data, n);
    }

    // Verbatim, but a rotation that is due waits for the next frame start.
    void onBeast(const uint8_t* data, std::size_t n)
    {
        const uint64_t begin = splitter_.fed();
        frames_.clear();
        splitter_.feed(data, n, frames_);
        std::size_t done = 0;
        for (const auto& f : frames_) {
            if (f.start < begin)
                continue;  // started in an earlier chunk
            const auto cut = static_cast<std::size_t>(f.start - begin);
            sink_.write(reinterpret_cast<const char*>(data) + done, cut - done);
            done = cut;
            if (sink_.rotationDue())
                sink_.rotate();
        }
        sink_.write(reinterpret_cast<const char*>(data) + done, n - done);
    }

    void onUat(const uint8_t* data, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(data[i]);
            if (c != '\n') {
                if (line_.size() < kMaxUatLine)
                    line_ += c;
                continue;
            }
            if (!line_.empty() && line_.back() == '\r')
                line_.pop_back();
            if (!line_.empty()) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
                std::string out = std::to_string(ms);
                out += ' ';
                out += line_;
                out += '\n';
                if (sink_.rotationDue())
                    sink_.rotate();
                sink_.write(out.data(), out.size());
            }
            line_.clear();
        }
    }

    Kind kind_;
    HostPort where_;
    Sink& sink_;
    tcp::resolver resolver_;
    tcp::socket sock_;
    asio::steady_timer timer_;
    std::array<uint8_t, 65536> buf_{};
    std::chrono::milliseconds backoff_ = kMinBackoff;
    bool stopping_ = false;
    bool quiet_ = false;  // a retry was already logged at WARN
    beast::FrameSplitter splitter_;
    std::vector<beast::Frame> frames_;
    std::string line_;  // partial UAT line
};

int run(int argc, char** argv)
{
    std::string beast_arg, uat_arg, dir, retention_dir;
    double rotate_mb = 256, rotate_min = 60, max_gb = 0;
    long reopen_ms_for_test = 0;
    po::options_description desc(
        "usage: skycue-rawrec [--beast HOST:PORT] [--uat HOST:PORT] --dir DIR [--rotate-mb N] [--rotate-min N]\n"
        "                     [--max-gb N [--retention-dir DIR]]\n"
        "Records readsb's Beast output and dump978-fa's raw UAT lines");
    desc.add_options()("help,h", "show this help")(
        "beast", po::value(&beast_arg), "readsb Beast output to record (e.g. 127.0.0.1:30005)")(
        "uat", po::value(&uat_arg), "dump978-fa raw output to record (e.g. 127.0.0.1:30978)")(
        "dir", po::value(&dir), "output directory (created when missing)")(
        "rotate-mb", po::value(&rotate_mb)->default_value(256), "start a new file at this size (0: never)")(
        "rotate-min", po::value(&rotate_min)->default_value(60), "start a new file at this age (0: never)")(
        "max-gb", po::value(&max_gb)->default_value(0),
        "keep the recordings within this many GB, deleting the oldest (0: no limit; RECORD_MAX_GB)")(
        "retention-dir", po::value(&retention_dir),
        "the directory --max-gb covers, with its raw/ (default: --dir)");
    // Test seams, not in --help.
    po::options_description hidden;
    hidden.add_options()("fail-writes-for-test", "every write fails as on a full disk")(
        "reopen-ms-for-test", po::value(&reopen_ms_for_test), "write-failure backoff: N ms doubling to 4N ms");
    po::options_description all;
    all.add(desc).add(hidden);
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, all), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "skycue-rawrec: " << e.what() << "\n" << desc << "\n";
        return 2;
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    std::optional<HostPort> beast_at, uat_at;
    if (!beast_arg.empty())
        beast_at = parseHostPort(beast_arg);
    if (!beast_arg.empty() && !beast_at) {
        std::cerr << "skycue-rawrec: --beast must be HOST:PORT, got '" << beast_arg << "'\n";
        return 2;
    }
    if (!uat_arg.empty())
        uat_at = parseHostPort(uat_arg);
    if (!uat_arg.empty() && !uat_at) {
        std::cerr << "skycue-rawrec: --uat must be HOST:PORT, got '" << uat_arg << "'\n";
        return 2;
    }
    if (!beast_at && !uat_at) {
        std::cerr << "skycue-rawrec: give --beast, --uat or both\n";
        return 2;
    }
    if (dir.empty()) {
        std::cerr << "skycue-rawrec: --dir is required\n";
        return 2;
    }
    if (!std::isfinite(rotate_mb) || rotate_mb < 0 || !std::isfinite(rotate_min) || rotate_min < 0 ||
        !std::isfinite(max_gb) || max_gb < 0) {
        std::cerr << "skycue-rawrec: --rotate-mb, --rotate-min and --max-gb must be 0 or more\n";
        return 2;
    }
    if (reopen_ms_for_test < 0) {
        std::cerr << "skycue-rawrec: --reopen-ms-for-test must be 0 or more\n";
        return 2;
    }
    if (!makeDirs(dir)) {
        std::cerr << "skycue-rawrec: cannot create " << dir << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    auto fileConfig = [&](const char* stream, const char* ext) {
        RotatingFileConfig c;
        c.dir = dir;
        c.stream = stream;
        c.ext = ext;
        c.rotate_bytes = static_cast<uint64_t>(std::llround(rotate_mb * 1024 * 1024));
        c.rotate_after = std::chrono::seconds(std::llround(rotate_min * 60));
        if (reopen_ms_for_test > 0) {
            c.reopen_initial = std::chrono::milliseconds(reopen_ms_for_test);
            c.reopen_max = 4 * c.reopen_initial;
        }
        return c;
    };
    const uint64_t max_bytes = gigabytesToBytes(max_gb);
    if (retention_dir.empty())
        retention_dir = dir;
    Sink* sinks[2] = {nullptr, nullptr};
    auto retention = [&] {
        if (max_bytes == 0)
            return;
        std::set<std::string> open_files;
        for (const Sink* k : sinks)
            if (k && !k->currentPath().empty())
                open_files.insert(k->currentPath());
        enforce(retention_dir, max_bytes, open_files);
    };
    Sink beast_sink(fileConfig("beast", "bin"), retention), uat_sink(fileConfig("uat", "txt"), retention);
    sinks[0] = &beast_sink;
    sinks[1] = &uat_sink;
    if (vm.count("fail-writes-for-test")) {
        beast_sink.setFailWritesForTest(true);
        uat_sink.setFailWritesForTest(true);
    }

    asio::io_context io;
    std::vector<std::unique_ptr<Input>> inputs;
    if (beast_at)
        inputs.push_back(std::make_unique<Input>(io, Input::Kind::beast, *beast_at, beast_sink));
    if (uat_at)
        inputs.push_back(std::make_unique<Input>(io, Input::Kind::uat, *uat_at, uat_sink));

    asio::steady_timer flush_timer(io);
    std::function<void()> armFlush = [&] {
        flush_timer.expires_after(kFlushInterval);
        flush_timer.async_wait([&](const error_code& ec) {
            if (ec)
                return;
            beast_sink.flush();
            uat_sink.flush();
            armFlush();
        });
    };
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const error_code& ec, int sig) {
        if (ec)
            return;
        LOG_INFO("signal " << sig << ": flushing and exiting");
        for (auto& in : inputs)
            in->stop();
        error_code ignored;
        flush_timer.cancel(ignored);
        beast_sink.flush();
        uat_sink.flush();
        io.stop();
    });

    for (auto& in : inputs)
        in->start();
    armFlush();
    LOG_INFO("recording to " << dir);
    if (max_bytes > 0)
        LOG_INFO("keeping " << retention_dir << " within " << max_gb << " GB, oldest deleted first");
    io.run();
    return 0;
}

}  // namespace

// What escapes run() (io.run() rethrows a handler's exception; a constructor
// can throw) ends the program with a message and status 1, not terminate().
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "skycue-rawrec: " << e.what() << "\n";
        return 1;
    }
}
