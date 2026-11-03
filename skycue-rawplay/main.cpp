// skycue-rawplay: replays raw captures into readsb, paced like the original.
//
//   FILE.bin                Beast binary (skycue-rawrec): frames forwarded
//                           verbatim to --beast-in (readsb's Beast input,
//                           30004), paced by their 12 MHz timestamps
//   FILE.csv[.gz]           "epoch_seconds.fraction,HEX" lines: sent to
//                           --raw-in (readsb's raw input, 30001) as
//                           "@<12-hex-digit 12 MHz timestamp><HEX>;" lines,
//                           paced by the epochs
//   FILE.txt[.gz], .log     dump978 raw UAT lines ("-..." / "+...", optionally
//                           "<epoch_ms> "-prefixed as skycue-rawrec writes
//                           them, or "<ns>,"-prefixed as stratux's UAT logs
//                           do, <ns> a nanosecond offset, a "START," line or
//                           a backward offset beginning a session that plays
//                           1 s after the one before; the prefix is
//                           stripped and paces the lines, untimestamped lines
//                           go at 10 per second), served
//                           to whoever connects to --uat-listen PORT (readsb
//                           with --net-connector 127.0.0.1,PORT,uat_in)
//
// Any input may be gzip-compressed. Files of one kind play one after another
// on one connection; the kinds play side by side. Times are scheduled from
// each stream's start (no drift), --speed N plays N times faster (0: as fast
// as possible), --loop starts again after the last file with every timestamp
// shifted forward by the length of one pass. Without --loop it exits 0 once
// everything is sent.

#include "gz_line_reader.hpp"
#include "log.hpp"
#include "raw_formats.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/program_options.hpp>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace po = boost::program_options;
using asio::ip::tcp;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;
using namespace skycued;

namespace {

constexpr std::chrono::milliseconds kMinBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
constexpr int64_t kGapNs = 100'000'000;         // between files and passes
constexpr int64_t kUntimedStepNs = 100'000'000;  // untimestamped UAT: 10 lines/s
constexpr int64_t kMaxBeastStepNs = 60'000'000'000;  // larger Beast clock jumps are discontinuities
constexpr int64_t kMaxBeastBackNs = 1'000'000'000;   // smaller steps back are reordering
constexpr std::size_t kSendBuffer = 64 * 1024;

std::atomic<int> g_failed{0};

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

bool endsWith(const std::string& s, const char* suffix)
{
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

enum class Kind { beast, csv, uat };

std::optional<Kind> kindOf(const std::string& path)
{
    if (endsWith(path, ".bin") || endsWith(path, ".bin.gz"))
        return Kind::beast;
    if (endsWith(path, ".csv") || endsWith(path, ".csv.gz"))
        return Kind::csv;
    if (endsWith(path, ".txt") || endsWith(path, ".txt.gz") || endsWith(path, ".log") || endsWith(path, ".log.gz"))
        return Kind::uat;
    return std::nullopt;
}

const char* kindName(Kind k)
{
    switch (k) {
    case Kind::beast: return "beast";
    case Kind::csv: return "raw";
    case Kind::uat: return "uat";
    }
    return "?";
}

// A plain or gzip file (zlib reads both); a truncated gzip stream throws.
using Reader = GzLineReader;

// Where one stream goes: a connection to readsb (client), or the client that
// connected to our listening port (server). Blocking; used by one thread.
class Output {
public:
    Output(const char* name, HostPort to) : name_(name), to_(std::move(to)), sock_(io_) {}
    Output(const char* name, uint16_t port) : name_(name), sock_(io_), acceptor_(std::in_place, io_)
    {
        const tcp::endpoint ep(asio::ip::address_v4::loopback(), port);
        acceptor_->open(ep.protocol());
        acceptor_->set_option(tcp::acceptor::reuse_address(true));
        acceptor_->bind(ep);  // throws when the port is taken
        acceptor_->listen();
        // 127.0.0.1 only: readsb runs on this machine.
    }

    // Blocks until connected (client: retrying with backoff) or a client connected (server).
    void open()
    {
        auto backoff = kMinBackoff;
        bool warned = false;
        for (;;) {
            error_code ec;
            if (acceptor_) {
                acceptor_->accept(sock_, ec);
                if (!ec) {
                    LOG_INFO(name_ << ": client connected");
                    return;
                }
                LOG_WARN(name_ << ": accept failed: " << ec.message());
            } else {
                tcp::resolver resolver(io_);
                auto results = resolver.resolve(to_.host, to_.port, ec);
                if (!ec)
                    asio::connect(sock_, results, ec);
                if (!ec) {
                    LOG_INFO(name_ << ": connected to " << to_.host << ":" << to_.port);
                    return;
                }
                if (!warned)
                    LOG_WARN(name_ << ": " << to_.host << ":" << to_.port << ": " << ec.message()
                                   << "; retrying until it accepts");
                warned = true;
                error_code ignored;
                sock_.close(ignored);
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, kMaxBackoff);
        }
    }

    void send(const std::string& bytes, bool newline)
    {
        buf_ += bytes;
        if (newline)
            buf_ += '\n';
        if (buf_.size() >= kSendBuffer)
            flush();
    }

    // On a write error the buffered data is dropped and the connection is
    // opened again (a new client, for a server).
    void flush()
    {
        if (buf_.empty())
            return;
        error_code ec;
        asio::write(sock_, asio::buffer(buf_), ec);
        buf_.clear();
        if (ec) {
            LOG_WARN(name_ << ": write failed (" << ec.message() << "); reconnecting");
            error_code ignored;
            sock_.close(ignored);
            ++lost_;
            open();
        }
    }

    void close()
    {
        flush();
        error_code ignored;
        sock_.shutdown(tcp::socket::shutdown_send, ignored);
        sock_.close(ignored);
        if (acceptor_)
            acceptor_->close(ignored);
    }

    unsigned reconnects() const { return lost_; }

private:
    std::string name_;
    HostPort to_;
    asio::io_context io_;
    tcp::socket sock_;
    std::optional<tcp::acceptor> acceptor_;
    std::string buf_;
    unsigned lost_ = 0;
};

// Absolute schedule: media time t (ns since the stream's start) is due at
// origin + t / speed.
class Pacer {
public:
    Pacer(double speed, Output& out) : speed_(speed), out_(out) {}
    void start() { origin_ = steady::now(); }
    void waitFor(int64_t media_ns)
    {
        if (speed_ == 0)
            return;
        const auto due = origin_ + std::chrono::duration_cast<steady::duration>(
                                       std::chrono::duration<double, std::nano>(static_cast<double>(media_ns) / speed_));
        if (steady::now() >= due)
            return;
        out_.flush();
        std::this_thread::sleep_until(due);
    }

private:
    double speed_;
    Output& out_;
    steady::time_point origin_{};
};

struct Stats {
    uint64_t sent = 0, skipped = 0;
};

// Each play* function plays one file starting at media time `base`, with
// timestamps shifted by `shift_ns`, and returns the media time of its last record.

int64_t playBeast(const std::string& path, int64_t base, int64_t shift_ns, Output& out, Pacer& pacer, Stats& st)
{
    Reader in(path);
    beast::FrameSplitter splitter;
    std::vector<beast::Frame> frames;
    std::vector<uint8_t> buf(64 * 1024);
    const auto shift_ticks = static_cast<uint64_t>(shift_ns / 1000 * 12);  // 12 ticks per microsecond
    bool have_ts = false;
    uint64_t last_ts = 0;
    int64_t rel = 0;
    for (std::size_t n; (n = in.read(buf.data(), buf.size())) > 0;) {
        frames.clear();
        splitter.feed(buf.data(), n, frames);
        for (const auto& f : frames) {
            if (f.type != 0xe3 && f.timestamp != 0) {
                if (!have_ts) {
                    have_ts = true;
                    last_ts = f.timestamp;
                } else {
                    const int64_t d =
                        beast::timestampDelta(last_ts, f.timestamp) * 1000 / 12;  // ticks -> ns
                    if (d >= 0 && d <= kMaxBeastStepNs) {
                        rel += d;
                        last_ts = f.timestamp;
                    } else if (d < -kMaxBeastBackNs || d > kMaxBeastStepNs) {
                        last_ts = f.timestamp;  // the clock jumped: carry on from here
                    }                           // else slightly out of order: hold the clock
                }
            }
            pacer.waitFor(base + rel);
            if (shift_ticks == 0 || f.type == 0xe3)
                out.send(f.raw, false);
            else
                out.send(beast::withTimestamp(f, f.timestamp + shift_ticks), false);
            ++st.sent;
        }
    }
    st.skipped += splitter.skippedBytes();
    return base + rel;
}

int64_t playCsv(const std::string& path, int64_t base, int64_t shift_ns, Output& out, Pacer& pacer, Stats& st)
{
    Reader in(path);
    std::optional<int64_t> first;
    int64_t rel = 0;
    for (std::string line; in.getline(line);) {
        auto m = parseCsvMessage(line);
        if (!m) {
            if (!line.empty() && line[0] != '#')
                ++st.skipped;
            continue;
        }
        if (!first)
            first = m->epoch_ns;
        rel = std::max(rel, m->epoch_ns - *first);
        pacer.waitFor(base + rel);
        out.send(avrLine(m->epoch_ns + shift_ns, m->hex), true);
        ++st.sent;
    }
    return base + rel;
}

int64_t playUat(const std::string& path, int64_t base, int64_t, Output& out, Pacer& pacer, Stats& st)
{
    Reader in(path);
    std::optional<int64_t> first_ms, first_ns;
    StratuxClock stratux;
    bool any = false;
    int64_t rel = 0;
    for (std::string line; in.getline(line);) {
        auto u = parseUatLine(line);
        if (!u) {
            if (isStratuxStart(line))
                stratux.startSession();
            if (!line.empty() && line[0] != '#')
                ++st.skipped;
            continue;
        }
        if (u->epoch_ms) {
            if (!first_ms)
                first_ms = u->epoch_ms;
            rel = std::max(rel, (*u->epoch_ms - *first_ms) * 1'000'000);
        } else if (u->offset_ns) {
            // stratux: each session in file order, 1 s after the one before.
            const auto t = stratux.next(*u->offset_ns);
            if (!t) {
                ++st.skipped;
                continue;
            }
            if (!first_ns)
                first_ns = t;
            rel = std::max(rel, *t - *first_ns);
        } else if (any) {
            rel += kUntimedStepNs;
        }
        any = true;
        pacer.waitFor(base + rel);
        out.send(u->text, true);
        ++st.sent;
    }
    return base + rel;
}

void playStream(Kind kind, const std::vector<std::string>& files, Output& out, double speed, bool loop)
{
    const char* name = kindName(kind);
    try {
        out.open();
        Pacer pacer(speed, out);
        pacer.start();
        Stats st;
        int64_t media = 0, pass_len = 0;
        for (unsigned pass = 0;; ++pass) {
            const int64_t shift = static_cast<int64_t>(pass) * pass_len;
            for (const auto& f : files) {
                LOG_INFO(name << ": playing " << f << (pass ? " (pass " + std::to_string(pass + 1) + ")" : ""));
                switch (kind) {
                case Kind::beast: media = playBeast(f, media, shift, out, pacer, st); break;
                case Kind::csv: media = playCsv(f, media, shift, out, pacer, st); break;
                case Kind::uat: media = playUat(f, media, shift, out, pacer, st); break;
                }
                media += kGapNs;
            }
            if (pass == 0)
                pass_len = media;
            if (!loop)
                break;
        }
        out.close();
        LOG_INFO(name << ": done: " << st.sent << (kind == Kind::beast ? " frames" : " lines") << " sent, "
                      << st.skipped << (kind == Kind::beast ? " bytes" : " lines") << " skipped, "
                      << out.reconnects() << " reconnects");
    } catch (const std::exception& e) {
        LOG_ERROR(name << ": " << e.what());
        g_failed = 1;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::string beast_arg, raw_arg;
    int uat_port = 0;
    double speed = 1.0;
    std::vector<std::string> files;
    po::options_description desc(
        "usage: skycue-rawplay [--beast-in HOST:PORT] [--raw-in HOST:PORT] [--uat-listen PORT]\n"
        "                      [--speed N] [--loop] FILE...\n"
        "Replays .bin (Beast), .csv[.gz] (epoch,HEX) and .txt[.gz]/.log[.gz] (UAT: dump978 lines,\n"
        "optionally '<epoch_ms> ' or stratux '<ns>,' prefixed) captures into readsb");
    desc.add_options()("help,h", "show this help")(
        "beast-in", po::value(&beast_arg), "readsb Beast input for .bin files (e.g. 127.0.0.1:30004)")(
        "raw-in", po::value(&raw_arg), "readsb raw input for .csv files (e.g. 127.0.0.1:30001)")(
        "uat-listen", po::value(&uat_port),
        "port on 127.0.0.1 to serve UAT lines on (readsb: --net-connector 127.0.0.1,PORT,uat_in)")(
        "speed", po::value(&speed)->default_value(1.0), "playback speed; 0 = as fast as possible")(
        "loop", "start again after the last file, timestamps shifted forward");
    po::options_description hidden;
    hidden.add_options()("file", po::value(&files));
    po::options_description all;
    all.add(desc).add(hidden);
    po::positional_options_description pos;
    pos.add("file", -1);
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "skycue-rawplay: " << e.what() << "\n" << desc << "\n";
        return 2;
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    const bool loop = vm.count("loop") > 0;
    auto usage = [&](const std::string& msg) {
        std::cerr << "skycue-rawplay: " << msg << "\n";
        return 2;
    };
    if (!std::isfinite(speed) || speed < 0)
        return usage("--speed must be 0 or a positive number");
    if (files.empty())
        return usage("no input files");
    std::optional<HostPort> beast_to, raw_to;
    if (!beast_arg.empty()) {
        beast_to = parseHostPort(beast_arg);
        if (!beast_to)
            return usage("--beast-in must be HOST:PORT, got '" + beast_arg + "'");
    }
    if (!raw_arg.empty()) {
        raw_to = parseHostPort(raw_arg);
        if (!raw_to)
            return usage("--raw-in must be HOST:PORT, got '" + raw_arg + "'");
    }
    if (vm.count("uat-listen") && (uat_port < 1 || uat_port > 65535))
        return usage("--uat-listen must be a port number 1-65535");

    std::vector<std::string> by_kind[3];
    for (const auto& f : files) {
        auto k = kindOf(f);
        if (!k)
            return usage(f + ": unknown kind (want .bin, .csv[.gz], .txt[.gz] or .log)");
        try {
            Reader probe(f);  // fail now rather than mid-replay
        } catch (const std::exception& e) {
            std::cerr << "skycue-rawplay: " << e.what() << "\n";
            return 1;
        }
        by_kind[static_cast<int>(*k)].push_back(f);
    }
    if (!by_kind[static_cast<int>(Kind::beast)].empty() && !beast_to)
        return usage(".bin files need --beast-in HOST:PORT");
    if (!by_kind[static_cast<int>(Kind::csv)].empty() && !raw_to)
        return usage(".csv files need --raw-in HOST:PORT");
    if (!by_kind[static_cast<int>(Kind::uat)].empty() && !vm.count("uat-listen"))
        return usage("UAT files need --uat-listen PORT");

    // NOLINTNEXTLINE(cert-err33-c): constant, valid arguments: cannot fail
    std::signal(SIGPIPE, SIG_IGN);
    // Every output is set up (the UAT port bound) before anything plays.
    std::vector<std::pair<Kind, std::unique_ptr<Output>>> outs;
    try {
        for (Kind k : {Kind::beast, Kind::csv, Kind::uat}) {
            if (by_kind[static_cast<int>(k)].empty())
                continue;
            if (k == Kind::uat)
                outs.emplace_back(k, std::make_unique<Output>(kindName(k), static_cast<uint16_t>(uat_port)));
            else
                outs.emplace_back(k, std::make_unique<Output>(kindName(k), k == Kind::beast ? *beast_to : *raw_to));
        }
    } catch (const std::exception& e) {
        std::cerr << "skycue-rawplay: --uat-listen " << uat_port << ": " << e.what() << "\n";
        return 1;
    }
    std::vector<std::thread> threads;
    for (auto& o : outs) {
        const Kind k = o.first;
        Output& out = *o.second;
        const auto& list = by_kind[static_cast<int>(k)];
        threads.emplace_back([k, &list, &out, speed, loop] { playStream(k, list, out, speed, loop); });
    }
    for (auto& t : threads)
        t.join();
    return g_failed ? 1 : 0;
}
