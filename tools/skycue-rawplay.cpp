// skycue-rawplay: replays raw captures into readsb, paced like the original.
//
//   FILE.bin                Beast binary (skycue-rawrec): frames forwarded
//                           verbatim to --beast-in (readsb's Beast input,
//                           30004), paced by their 12 MHz timestamps
//   FILE.csv[.gz]           "epoch_seconds.fraction,HEX" lines: sent to
//                           --raw-in (readsb's raw input, 30001) as
//                           "@<12-hex-digit 12 MHz timestamp><HEX>;" lines,
//                           paced by the epochs; to --1090-listen instead,
//                           as Beast type 2/3 frames (the "@" timestamp's
//                           12 MHz ticks, signal byte 0)
//   FILE.txt[.gz], .log     dump978 raw UAT lines ("-..." / "+...", optionally
//                           "<epoch_ms> "-prefixed as skycue-rawrec writes
//                           them, or "<ns>,"-prefixed as stratux's UAT logs
//                           do, <ns> a nanosecond offset, a "START," line or
//                           a backward offset beginning a session that plays
//                           1 s after the one before; the prefix is
//                           stripped and paces the lines, untimestamped lines
//                           go at 10 per second), served
//                           to whoever connects to --uat-listen PORT (readsb
//                           with --net-connector 127.0.0.1,PORT,uat_in), and
//                           to any number of clients at once
//
// --1090-listen PORT serves Beast frames the same way (readsb with
// --net-connector 127.0.0.1,PORT,beast_in, or skycued's beast_raw input).
// --wait-clients N (default 1) holds each listening stream until N clients are
// connected to its port. A listening port serves its clients independently:
// a client that stops reading has its backlog capped at 1 MiB, the rest is
// dropped (counted, printed at exit) and the others are not held up. With
// --speed 0 nothing is dropped: the stream waits for the slowest client.
//
// Any input may be gzip-compressed. Files of one kind play one after another
// on one connection; the kinds play side by side. Times are scheduled from
// each stream's start (no drift), --speed N plays N times faster (0: as fast
// as possible), --loop starts again after the last file with every timestamp
// shifted forward by the length of one pass. Without --loop it exits 0 once
// everything is sent.

#include "gz_line_reader.hpp"
#include "inputs/transport.hpp"
#include "log.hpp"
#include "raw_formats.hpp"

#include "skycue/net/buffer.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/listener.hpp"
#include "skycue/net/tcp_options.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/program_options.hpp>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace po = boost::program_options;
namespace net = skycue::net;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;
using namespace skycued;

namespace {

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

// The network library wants all its calls on one io thread; the playback
// threads (one per kind, they block while pacing) reach it through call().
class Io {
public:
    Io() : guard_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}
    ~Io()
    {
        guard_.reset();
        io_.stop();
        thread_.join();
    }
    Io(const Io&) = delete;
    Io& operator=(const Io&) = delete;

    asio::io_context& context() { return io_; }

    // Runs f on the io thread and returns its result (or rethrows what it threw).
    template <class F>
    auto call(F f) -> decltype(f())
    {
        std::packaged_task<decltype(f())()> task(std::move(f));
        auto result = task.get_future();
        asio::post(io_, [&task] { task(); });
        return result.get();
    }

private:
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread thread_;
};

void pauseMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Where one stream goes: a connection to readsb (dialed), or the clients that
// connect to our listening port. Used by one playback thread.
class Output {
public:
    virtual ~Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // Blocks until connected (dialed: retrying with backoff) or until
    // --wait-clients clients are connected (listening).
    virtual void open() = 0;
    // Waits for what is queued to go out, then closes.
    virtual void close() = 0;
    // What the done line adds to "N reconnects".
    virtual std::string extra() const { return {}; }
    virtual unsigned reconnects() const = 0;

    void send(const std::string& bytes, bool newline)
    {
        buf_ += bytes;
        if (newline)
            buf_ += '\n';
        if (buf_.size() >= kSendBuffer)
            flush();
    }

    void flush()
    {
        if (buf_.empty())
            return;
        deliver(net::makeBuf(buf_));
        buf_.clear();
    }

protected:
    Output() = default;
    // Hands the buffer to the library; waits as the output's policy says.
    virtual void deliver(const net::Buf& b) = 0;

private:
    std::string buf_;
};

// A connection to readsb's input, kept up by the library's Dialer. While it
// is down the stream waits; the queue is held to a few hundred KiB so a fast
// replay does not outrun the reader (and trip the library's queue cap).
class DialOut final : public Output {
public:
    DialOut(Io& io, const char* name, HostPort to) : io_(io), name_(name), to_(std::move(to))
    {
        io_.call([this] {
            net::TcpOptions t;
            t.name = name_;
            t.keepalive = false;  // plain sockets, as this tool always used
            t.nodelay = false;
            dialer_ = net::Dialer::create(io_.context(), to_.host, static_cast<uint16_t>(std::stoi(to_.port)), t);
            dialer_->onConnection([this](bool up) {
                if (up) {
                    LOG_INFO(name_ << ": connected to " << to_.host << ":" << to_.port);
                } else {
                    LOG_WARN(name_ << ": connection lost (" << dialer_->downInfo().why << "); reconnecting");
                    ++lost_;
                }
            });
            dialer_->onFailure([this](const std::string& why) { LOG_WARN(name_ << ": " << why << "; retrying"); });
        });
    }
    ~DialOut() override { shutdown(); }

    void open() override
    {
        io_.call([this] { dialer_->start(); });
        while (!dialer_->connected())
            pauseMs(20);
    }

    void close() override
    {
        flush();
        for (int i = 0; i < 1000 && queued() > 0; ++i)
            pauseMs(5);
        shutdown();
    }

    unsigned reconnects() const override { return lost_; }

protected:
    void deliver(const net::Buf& b) override
    {
        for (;;) {
            if (!dialer_->connected()) {
                pauseMs(50);
                continue;
            }
            if (io_.call([&] {
                    if (!dialer_->connected())
                        return false;
                    dialer_->send(b);
                    return true;
                }))
                break;
        }
        while (queued() > kDialBacklog && dialer_->connected())
            pauseMs(2);
    }

private:
    uint64_t queued() { return io_.call([this] { return dialer_ ? dialer_->peerStats().queued_bytes : uint64_t{0}; }); }
    void shutdown()
    {
        io_.call([this] {
            if (dialer_)
                dialer_->stop();
            dialer_.reset();
        });
    }

    static constexpr uint64_t kDialBacklog = 256 * 1024;
    Io& io_;
    const char* name_;
    HostPort to_;
    std::shared_ptr<net::Dialer> dialer_;
    std::atomic<unsigned> lost_{0};
};

// A listening port on 127.0.0.1 (readsb runs on this machine) serving every
// client that connects. Each client has its own queue, so a client that stops
// reading costs only itself: once its backlog is kBacklog the chunks it cannot
// take are dropped and counted, and the others carry on. With --speed 0 there
// is no pace to keep, so the stream waits for the backlogs to drain instead
// and nothing is dropped.
class ListenOut final : public Output {
public:
    static constexpr uint64_t kBacklog = 1024 * 1024;  // per client

    ListenOut(Io& io, const char* name, uint16_t port, unsigned wait_clients, bool lossless)
        : io_(io), name_(name), wait_clients_(wait_clients), lossless_(lossless)
    {
        io_.call([&] {  // throws when the port is taken
            net::ListenOptions o;
            o.tcp.name = name_;
            o.tcp.send_cap = 4 * kBacklog;  // above the backlog plus a chunk: our own limit comes first
            o.tcp.keepalive = false;
            o.tcp.nodelay = false;
            o.max_peers = 64;
            o.fail_fast = true;
            listener_ = net::Listener::create(io_.context(), "127.0.0.1", port, o);
            listener_->onPeer([this](uint64_t stream, bool up, const asio::ip::tcp::endpoint& who) {
                if (up) {
                    streams_.insert(stream);
                    LOG_INFO(name_ << ": client " << who << " connected");
                } else {
                    streams_.erase(stream);
                    LOG_INFO(name_ << ": client " << who << " gone");
                }
            });
            listener_->start();
        });
    }
    ~ListenOut() override { shutdown(); }

    void open() override
    {
        while (clients() < wait_clients_)
            pauseMs(20);
    }

    void close() override
    {
        flush();
        // Lossless: until everything is out, as a blocking write was. Otherwise as
        // long as the clients are still taking data: one that does not read must
        // not hold the exit.
        uint64_t last = backlog();
        for (int still = 0; last > 0 && (lossless_ || still < 100);) {
            pauseMs(5);
            const uint64_t now = backlog();
            still = now < last ? 0 : still + 1;
            last = now;
        }
        shutdown();
    }

    unsigned reconnects() const override { return waits_; }
    std::string extra() const override
    {
        return ", " + std::to_string(dropped_bytes_.load()) + " bytes dropped (slow clients)";
    }

protected:
    void deliver(const net::Buf& b) override
    {
        const uint64_t size = b->size();
        enum class Result { sent, no_clients, full };
        bool waiting = false;
        for (;;) {
            const Result r = io_.call([&] {
                if (streams_.empty())
                    return Result::no_clients;
                if (lossless_)  // nothing is sent until every client has room
                    for (uint64_t s : streams_)
                        if (auto st = listener_->peerStats(s); st && st->queued_bytes + size > kBacklog)
                            return Result::full;
                for (uint64_t s : streams_) {
                    auto st = listener_->peerStats(s);
                    if (!st)
                        continue;
                    if (st->queued_bytes + size > kBacklog)
                        dropped_bytes_ += size;
                    else
                        listener_->send(s, b);
                }
                return Result::sent;
            });
            if (r == Result::sent)
                return;
            if (r == Result::no_clients && !waiting) {  // everyone left: hold the stream until a client is back
                waiting = true;
                ++waits_;
            }
            pauseMs(r == Result::full ? 2 : 20);
        }
    }

private:
    unsigned clients() { return io_.call([this] { return static_cast<unsigned>(streams_.size()); }); }
    uint64_t backlog()
    {
        return io_.call([this] {
            uint64_t most = 0;
            for (uint64_t s : streams_)
                if (auto st = listener_->peerStats(s))
                    most = std::max(most, st->queued_bytes);
            return most;
        });
    }
    void shutdown()
    {
        io_.call([this] {
            if (listener_)
                listener_->stop();
            listener_.reset();
        });
    }

    Io& io_;
    const char* name_;
    unsigned wait_clients_;
    bool lossless_;
    std::shared_ptr<net::Listener> listener_;
    std::set<uint64_t> streams_;  // io thread
    std::atomic<uint64_t> dropped_bytes_{0};
    std::atomic<unsigned> waits_{0};
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

// The capture line as a Beast frame: type 1 (Mode A/C), 2 (short) or 3 (long),
// the 12 MHz timestamp avrLine() computes, signal byte 0, 0x1a doubled.
std::string beastFrame(int64_t epoch_ns, const std::string& hex)
{
    const auto ns = static_cast<uint64_t>(epoch_ns);
    const uint64_t ticks = (ns / 250u * 3u + ns % 250u * 3u / 250u) & beast::kTimestampMask;
    std::string f;
    auto put = [&f](uint8_t b) {
        f += static_cast<char>(b);
        if (b == beast::kEscape)
            f += static_cast<char>(b);
    };
    f += static_cast<char>(beast::kEscape);
    f += hex.size() == 4 ? '1' : hex.size() == 14 ? '2' : '3';
    for (int shift = 40; shift >= 0; shift -= 8)
        put(static_cast<uint8_t>(ticks >> shift));
    put(0);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        put(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return f;
}

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

int64_t playCsv(const std::string& path, bool as_beast, int64_t base, int64_t shift_ns, Output& out, Pacer& pacer, Stats& st)
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
        if (as_beast)
            out.send(beastFrame(m->epoch_ns + shift_ns, m->hex), false);
        else
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

void playStream(Kind kind, bool csv_as_beast, const std::vector<std::string>& files, Output& out, double speed,
                bool loop)
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
                case Kind::csv: media = playCsv(f, csv_as_beast, media, shift, out, pacer, st); break;
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
        const bool frames = kind == Kind::beast || csv_as_beast;
        LOG_INFO(name << ": done: " << st.sent << (frames ? " frames" : " lines") << " sent, " << st.skipped
                      << (kind == Kind::beast ? " bytes" : " lines") << " skipped, " << out.reconnects()
                      << " reconnects" << out.extra());
    } catch (const std::exception& e) {
        LOG_ERROR(name << ": " << e.what());
        g_failed = 1;
    }
}

int run(int argc, char** argv)
{
    std::string beast_arg, raw_arg;
    int uat_port = 0, beast_port = 0, wait_clients = 1;
    double speed = 1.0;
    std::vector<std::string> files;
    po::options_description desc(
        "usage: skycue-rawplay [--beast-in HOST:PORT] [--raw-in HOST:PORT] [--1090-listen PORT]\n"
        "                      [--uat-listen PORT] [--wait-clients N] [--speed N] [--loop] FILE...\n"
        "Replays .bin (Beast), .csv[.gz] (epoch,HEX) and .txt[.gz]/.log[.gz] (UAT: dump978 lines,\n"
        "optionally '<epoch_ms> ' or stratux '<ns>,' prefixed) captures into readsb or skycued");
    desc.add_options()("help,h", "show this help")(
        "beast-in", po::value(&beast_arg), "readsb Beast input for .bin files (e.g. 127.0.0.1:30004)")(
        "raw-in", po::value(&raw_arg), "readsb raw input for .csv files (e.g. 127.0.0.1:30001)")(
        "1090-listen", po::value(&beast_port),
        "port on 127.0.0.1 to serve Beast frames on, to any number of clients: .bin files as recorded,\n"
        ".csv files as type 2/3 frames (readsb: --net-connector 127.0.0.1,PORT,beast_in)")(
        "uat-listen", po::value(&uat_port),
        "port on 127.0.0.1 to serve UAT lines on, to any number of clients (readsb: --net-connector "
        "127.0.0.1,PORT,uat_in)")(
        "wait-clients", po::value(&wait_clients)->default_value(1),
        "hold playback until this many clients are connected to each listening port in use")(
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
    if (vm.count("1090-listen") && (beast_port < 1 || beast_port > 65535))
        return usage("--1090-listen must be a port number 1-65535");
    if (wait_clients < 1 || wait_clients > 64)
        return usage("--wait-clients must be 1-64");

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
    const bool have_beast = !by_kind[static_cast<int>(Kind::beast)].empty();
    const bool have_csv = !by_kind[static_cast<int>(Kind::csv)].empty();
    const bool have_uat = !by_kind[static_cast<int>(Kind::uat)].empty();
    // .bin goes to --beast-in or --1090-listen, .csv to --raw-in (AVR lines)
    // or --1090-listen (Beast frames); one listening port takes one kind.
    if (have_beast && beast_to && vm.count("1090-listen"))
        return usage(".bin files go to --beast-in or --1090-listen, not both");
    if (have_csv && raw_to && vm.count("1090-listen"))
        return usage(".csv files go to --raw-in or --1090-listen, not both");
    const bool beast_listens = have_beast && !beast_to && vm.count("1090-listen");
    const bool csv_listens = have_csv && !raw_to && vm.count("1090-listen");
    if (beast_listens && csv_listens)
        return usage(".bin and .csv files cannot share --1090-listen: send one of them with --beast-in or --raw-in");
    if (have_beast && !beast_to && !beast_listens)
        return usage(".bin files need --beast-in HOST:PORT or --1090-listen PORT");
    if (have_csv && !raw_to && !csv_listens)
        return usage(".csv files need --raw-in HOST:PORT or --1090-listen PORT");
    if (have_uat && !vm.count("uat-listen"))
        return usage("UAT files need --uat-listen PORT");

    // NOLINTNEXTLINE(cert-err33-c): constant, valid arguments: cannot fail
    std::signal(SIGPIPE, SIG_IGN);
    installNetLogSink();
    Io io;
    // Every output is set up (the listening ports bound) before anything plays.
    std::vector<std::pair<Kind, std::unique_ptr<Output>>> outs;
    const char* failing = "";
    try {
        const bool lossless = speed == 0;
        const auto waits = static_cast<unsigned>(wait_clients);
        for (Kind k : {Kind::beast, Kind::csv, Kind::uat}) {
            if (by_kind[static_cast<int>(k)].empty())
                continue;
            if (k == Kind::uat) {
                failing = "--uat-listen";
                outs.emplace_back(k, std::make_unique<ListenOut>(io, kindName(k), static_cast<uint16_t>(uat_port),
                                                                 waits, lossless));
            } else if ((k == Kind::beast && beast_listens) || (k == Kind::csv && csv_listens)) {
                failing = "--1090-listen";
                outs.emplace_back(k, std::make_unique<ListenOut>(io, kindName(k), static_cast<uint16_t>(beast_port),
                                                                 waits, lossless));
            } else {
                failing = "";
                outs.emplace_back(k, std::make_unique<DialOut>(io, kindName(k), k == Kind::beast ? *beast_to : *raw_to));
            }
        }
    } catch (const std::exception& e) {
        const int port = std::string(failing) == "--uat-listen" ? uat_port : beast_port;
        std::cerr << "skycue-rawplay: " << failing << " " << port << ": " << e.what() << "\n";
        return 1;
    }
    std::vector<std::thread> threads;
    for (auto& o : outs) {
        const Kind k = o.first;
        Output& out = *o.second;
        const auto& list = by_kind[static_cast<int>(k)];
        const bool as_beast = csv_listens;
        threads.emplace_back([k, as_beast, &list, &out, speed, loop] { playStream(k, as_beast, list, out, speed, loop); });
    }
    for (auto& t : threads)
        t.join();
    return g_failed ? 1 : 0;
}

}  // namespace

// What escapes run() ends the program with a message and status 1, not terminate().
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "skycue-rawplay: " << e.what() << "\n";
        return 1;
    }
}
