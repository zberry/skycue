// skycue-rawrec: records the raw receiver streams for later replay with
// skycue-rawplay.
//
//   --beast HOST:PORT  readsb's Beast output (30005): written verbatim to
//                      <dir>/beast-YYYYMMDD-HHMMSS.bin
//   --uat HOST:PORT    dump978-fa's raw port (30978): each line written as
//                      "<epoch_ms> <line>" to <dir>/uat-YYYYMMDD-HHMMSS.txt
//
// Files rotate like the Recorder's (--rotate-mb, --rotate-min); a Beast file
// is only ever cut at a frame start. Each input reconnects with a backoff
// from 0.5 s to 10 s. SIGINT/SIGTERM flush the files and exit 0.
// Single-threaded: everything runs on one io_context.

#include "log.hpp"
#include "raw_formats.hpp"
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
constexpr std::chrono::seconds kRetryWriteAfter{5};
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

// A RotatingFile that, after a failed write, drops data for a while instead
// of trying (and maybe creating) a new file for every chunk.
class Sink {
public:
    explicit Sink(RotatingFileConfig cfg) : name_(cfg.stream), file_(std::move(cfg)) {}

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
        if (n == 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (failing_ && now < retry_at_) {
            dropped_ += n;
            return;
        }
        const bool was_open = !file_.currentPath().empty();
        if (!file_.write(data, n)) {
            const int err = errno;
            dropped_ += n;
            retry_at_ = now + kRetryWriteAfter;
            if (!failing_)
                LOG_ERROR(name_ << ": cannot write under the output directory: " << std::strerror(err)
                                << "; dropping data, retrying every " << kRetryWriteAfter.count() << " s");
            failing_ = true;
            return;
        }
        if (failing_) {
            LOG_INFO(name_ << ": writing again; dropped " << dropped_ << " bytes meanwhile");
            failing_ = false;
            dropped_ = 0;
        }
        if (!was_open)
            LOG_INFO("writing " << file_.currentPath());
    }
    void flush()
    {
        if (!file_.flush() && !failing_) {
            const int err = errno;
            LOG_ERROR(name_ << ": flush failed: " << std::strerror(err));
            failing_ = true;
            retry_at_ = std::chrono::steady_clock::now() + kRetryWriteAfter;
        }
    }

private:
    std::string name_;
    RotatingFile file_;
    bool failing_ = false;
    std::chrono::steady_clock::time_point retry_at_{};
    uint64_t dropped_ = 0;
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
        resolver_.async_resolve(where_.host, where_.port, [this](const error_code& ec, tcp::resolver::results_type r) {
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

}  // namespace

int main(int argc, char** argv)
{
    std::string beast_arg, uat_arg, dir;
    double rotate_mb = 256, rotate_min = 60;
    po::options_description desc(
        "usage: skycue-rawrec [--beast HOST:PORT] [--uat HOST:PORT] --dir DIR [--rotate-mb N] [--rotate-min N]\n"
        "Records readsb's Beast output and dump978-fa's raw UAT lines");
    desc.add_options()("help,h", "show this help")(
        "beast", po::value(&beast_arg), "readsb Beast output to record (e.g. 127.0.0.1:30005)")(
        "uat", po::value(&uat_arg), "dump978-fa raw output to record (e.g. 127.0.0.1:30978)")(
        "dir", po::value(&dir), "output directory (created when missing)")(
        "rotate-mb", po::value(&rotate_mb)->default_value(256), "start a new file at this size (0: never)")(
        "rotate-min", po::value(&rotate_min)->default_value(60), "start a new file at this age (0: never)");
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
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
    if (!beast_arg.empty() && !(beast_at = parseHostPort(beast_arg))) {
        std::cerr << "skycue-rawrec: --beast must be HOST:PORT, got '" << beast_arg << "'\n";
        return 2;
    }
    if (!uat_arg.empty() && !(uat_at = parseHostPort(uat_arg))) {
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
    if (!std::isfinite(rotate_mb) || rotate_mb < 0 || !std::isfinite(rotate_min) || rotate_min < 0) {
        std::cerr << "skycue-rawrec: --rotate-mb and --rotate-min must be 0 or more\n";
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
        return c;
    };
    Sink beast_sink(fileConfig("beast", "bin")), uat_sink(fileConfig("uat", "txt"));

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
    io.run();
    return 0;
}
