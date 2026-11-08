// skycue-pingsim: plays a pingStation 3 to a skycued input, without hardware.
//
//   --capture FILE        a capture file (capture-<id>-*.jsonl, written by an
//                         input with "capture": true): every record's bytes
//                         sent as they were received, paced by "t"
//   --as json FILE        a received-stream recording or a capture of readsb's
//   --as cat021 FILE      JSON port: each observation synthesised as a bare
//                         pingStation JSON aircraft object or a one-record
//                         CAT021 (ed. 2.5) block, stamped with the send time
//                         and paced by the observation times
//
// Output: --udp HOST:PORT (one datagram per record; a multicast group from the
// interface --iface ADDR names), --tcp HOST:PORT (connects,
// for a tcp_listen input) or --tcp-listen PORT (serves one client at a time,
// for a tcp_connect input; on 127.0.0.1 unless --bind ADDR). JSON is UDP
// only, as the pingstation_json input.
// --status-interval S (JSON) also sends {"status":{...}} every S seconds.
// Times are scheduled from the stream's start (no drift), --speed N plays N
// times faster (0: as fast as possible), --loop starts again after the end.
// Without --loop it exits 0 once everything is sent. FILE may be
// gzip-compressed.

#include "asterix/capture_line.hpp"
#include "asterix/cat021_encode.hpp"
#include "gz_line_reader.hpp"
#include "inputs/ping_encode.hpp"
#include "inputs/transport.hpp"
#include "log.hpp"
#include "recording_reader.hpp"

#include "skycue/net/buffer.hpp"
#include "skycue/net/dialer.hpp"
#include "skycue/net/listener.hpp"
#include "skycue/net/tcp_options.hpp"
#include "skycue/net/udp_sender.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace po = boost::program_options;
using asio::ip::tcp;
namespace net = skycue::net;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;
using std::chrono::system_clock;
using namespace skycued;

namespace {

constexpr std::chrono::milliseconds kMinBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
constexpr int64_t kGapNs = 100'000'000;  // between passes

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

enum class Mode { udp, tcp, tcp_listen };

// Where the records go, on the network library. The library works on its
// io_context's thread; this tool has one thread, which runs the io_context
// itself, in short turns: whenever it waits (pacing, a connection, a full
// queue) and after each record.
class Output {
public:
    Output(asio::io_context& io, Mode mode, HostPort to, const asio::ip::address& bind, uint16_t listen_port,
           std::string iface)
        : io_(io), mode_(mode), to_(std::move(to)), iface_(std::move(iface)), guard_(asio::make_work_guard(io))
    {
        if (mode_ == Mode::tcp_listen) {
            net::ListenOptions o;
            o.max_peers = 1;  // one client at a time
            o.fail_fast = true;
            listener_ = net::Listener::create(io_, bind.to_string(), listen_port, o);
            listener_->onPeer([this](uint64_t stream, bool up, const tcp::endpoint& who) {
                if (up) {
                    streams_.insert(stream);
                    LOG_INFO("client " << who << " connected");
                } else {
                    streams_.erase(stream);
                    LOG_INFO("client " << who << " gone");
                }
            });
            listener_->start();  // throws when the port is taken
        }
    }
    ~Output()
    {
        stopAll();
        guard_.reset();
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // Runs the library until t.
    void pumpUntil(steady::time_point t)
    {
        while (steady::now() < t) {
            io_.restart();
            io_.run_until(t);
        }
    }
    void pumpFor(std::chrono::milliseconds d) { pumpUntil(steady::now() + d); }

    // UDP: resolves the target (retrying with backoff). TCP: blocks until
    // connected (retrying with backoff) or until a client connected.
    void open()
    {
        if (mode_ == Mode::tcp_listen) {
            while (listener_->peers() == 0)
                pumpFor(std::chrono::milliseconds(20));
            return;
        }
        if (mode_ == Mode::tcp) {
            net::TcpOptions t;
            dialer_ = net::Dialer::create(io_, to_.host, static_cast<uint16_t>(std::stoi(to_.port)), t);
            dialer_->onConnection([this](bool up) {
                if (up) {
                    LOG_INFO("connected to " << to_.host << ":" << to_.port);
                } else {
                    LOG_WARN("connection lost; reconnecting");
                    ++lost_;
                }
            });
            dialer_->start();
            while (!dialer_->connected())
                pumpFor(std::chrono::milliseconds(20));
            return;
        }
        auto backoff = kMinBackoff;
        for (;;) {
            sender_ = net::UdpSender::create(io_);
            net::UdpTarget target;
            target.host = to_.host;
            target.port = static_cast<uint16_t>(std::stoi(to_.port));
            target.iface = iface_;
            sender_->addTarget(target);
            if (sender_->start() > 0)
                return;
            LOG_WARN(to_.host << ":" << to_.port << ": " << sender_->counters().last_error << "; retrying");
            sender_->stop();
            pumpFor(backoff);
            backoff = std::min(backoff * 2, kMaxBackoff);
        }
    }

    // A record while the TCP connection is down waits for the next one (a new
    // client, for --tcp-listen). A datagram the kernel cannot take is dropped
    // and counted.
    void send(const std::vector<uint8_t>& bytes)
    {
        const net::Buf b = net::makeBuf(bytes.data(), bytes.size());
        if (mode_ == Mode::udp) {
            sender_->send(b);
            io_.restart();
            io_.poll();
            return;
        }
        while (!connected()) {
            if (mode_ == Mode::tcp_listen && !waiting_) {
                waiting_ = true;
                ++lost_;
                LOG_WARN("no client; waiting for one");
            }
            pumpFor(std::chrono::milliseconds(20));
        }
        waiting_ = false;
        if (listener_)
            listener_->sendAll(b);
        else
            dialer_->send(b);
        io_.restart();
        io_.poll();
        while (queued() > kBacklog && connected())  // do not outrun the reader
            pumpFor(std::chrono::milliseconds(2));
    }

    void close()
    {
        for (int i = 0; i < 2000 && queued() > 0 && connected(); ++i)
            pumpFor(std::chrono::milliseconds(2));
        stopAll();
    }

    // Reconnects (TCP) or datagrams the kernel would not take (UDP).
    unsigned lost() const { return mode_ == Mode::udp ? static_cast<unsigned>(udp_dropped_) : lost_; }

private:
    bool connected() const { return listener_ ? listener_->peers() > 0 : dialer_ && dialer_->connected(); }
    uint64_t queued()
    {
        if (dialer_)
            return dialer_->peerStats().queued_bytes;
        uint64_t most = 0;
        if (listener_)
            for (uint64_t s : streams_)
                if (auto st = listener_->peerStats(s))
                    most = std::max(most, st->queued_bytes);
        return most;
    }
    void stopAll()
    {
        if (sender_) {
            udp_dropped_ = sender_->counters().dropped;
            sender_->stop();
        }
        if (dialer_)
            dialer_->stop();
        if (listener_)
            listener_->stop();
        io_.restart();
        boost::system::error_code ignored;
        io_.poll(ignored);
    }

    static constexpr uint64_t kBacklog = 256 * 1024;
    asio::io_context& io_;
    Mode mode_;
    HostPort to_;
    std::string iface_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::shared_ptr<net::UdpSender> sender_;
    std::shared_ptr<net::Dialer> dialer_;
    std::shared_ptr<net::Listener> listener_;
    std::set<uint64_t> streams_;
    unsigned lost_ = 0;
    uint64_t udp_dropped_ = 0;
    bool waiting_ = false;
};

struct StatusOptions {
    std::chrono::duration<double> interval;
    double lat, lon;
};

// Absolute schedule: media time t (ns since the stream's start) is due at
// origin + t / speed. Status messages, when asked for, go out every interval
// of wall time from the start, between the records.
class Pacer {
public:
    Pacer(double speed, Output& out, std::optional<StatusOptions> status)
        : speed_(speed), out_(out), status_(status)
    {
    }

    void start()
    {
        origin_ = steady::now();
        next_status_ = origin_;
    }

    void waitFor(int64_t media_ns)
    {
        const auto due = speed_ == 0
                             ? steady::now()
                             : origin_ + std::chrono::duration_cast<steady::duration>(std::chrono::duration<double, std::nano>(
                                             static_cast<double>(media_ns) / speed_));
        while (status_ && next_status_ <= due) {
            sleepUntil(next_status_);
            const std::string s = pingStatusJson(status_->lat, status_->lon, system_clock::now());
            out_.send(std::vector<uint8_t>(s.begin(), s.end()));
            ++statuses_;
            next_status_ += std::chrono::duration_cast<steady::duration>(status_->interval);
        }
        sleepUntil(due);
    }

    uint64_t statuses() const { return statuses_; }

private:
    void sleepUntil(steady::time_point t) { out_.pumpUntil(t); }

    double speed_;
    Output& out_;
    std::optional<StatusOptions> status_;
    steady::time_point origin_{}, next_status_{};
    uint64_t statuses_ = 0;
};

enum class Form { capture, json, cat021 };

struct Stats {
    uint64_t sent = 0, skipped = 0;
    bool transport_warned = false;
};

int64_t nsBetween(system_clock::time_point a, system_clock::time_point b)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(a - b).count();
}

// Plays the file once starting at media time `base`; returns the media time
// of its last record.
int64_t playFile(const std::string& path, Form form, Mode mode, uint8_t sac, uint8_t sic, int64_t base,
                 Output& out, Pacer& pacer, Stats& st)
{
    GzLineReader in(path);
    std::optional<system_clock::time_point> first;
    int64_t rel = 0;
    for (std::string line; in.getline(line);) {
        system_clock::time_point t;
        std::optional<CaptureLine> captured;
        RecordingRecord rec;
        if (form == Form::capture) {
            captured = parseCaptureLine(line);
            if (!captured) {
                if (!line.empty())
                    ++st.skipped;
                continue;
            }
            t = captured->t;
            // A TCP block sent as a datagram keeps its bytes but not the stream; a
            // datagram (JSON above all) sent over TCP loses its boundaries.
            const bool from_udp = captured->transport == "udp";
            if (!st.transport_warned && from_udp != (mode == Mode::udp)) {
                st.transport_warned = true;
                LOG_WARN(path << ": a record captured over " << captured->transport << " is sent over "
                              << (mode == Mode::udp ? "UDP" : "TCP")
                              << (from_udp && !captured->bytes.empty() && captured->bytes[0] == '{'
                                      ? ": JSON has no framing on TCP"
                                      : "")
                              << " (warned once)");
            }
        } else {
            const LineKind kind = parseRecordingLine(line, rec);
            if (!(kind == LineKind::readsb_raw || (kind == LineKind::received && rec.kind == RecordKind::obs))) {
                if (kind != LineKind::ignore)
                    ++st.skipped;  // sent-stream records, uplinks, bad lines
                continue;
            }
            t = rec.time;
        }
        if (!first)
            first = t;
        rel = std::max(rel, nsBetween(t, *first));
        pacer.waitFor(base + rel);
        if (form == Form::capture) {
            out.send(captured->bytes);
        } else {
            const auto stamp = system_clock::now();
            if (form == Form::json) {
                const std::string s = pingJsonFromObservation(rec.obs, stamp);
                out.send(std::vector<uint8_t>(s.begin(), s.end()));
            } else {
                std::vector<uint8_t> block;
                try {
                    block = asterix::encodeBlock({cat021FromObservation(rec.obs, sac, sic, stamp)});
                } catch (const std::invalid_argument&) {  // not expected: the mapping keeps values in range
                    ++st.skipped;
                    continue;
                }
                out.send(block);
            }
        }
        ++st.sent;
    }
    return base + rel;
}

int run(int argc, char** argv)
{
    std::string udp_arg, tcp_arg, capture_file, as_form, bind_arg = "127.0.0.1", iface;
    int listen_port = 0, sac = 0, sic = 0;
    double speed = 1.0, status_interval = 0, status_lat = 0, status_lon = 0;
    std::vector<std::string> files;
    po::options_description desc(
        "usage: skycue-pingsim (--udp HOST:PORT [--iface ADDR] | --tcp HOST:PORT | --tcp-listen PORT [--bind ADDR])\n"
        "                      (--capture FILE | --as json|cat021 FILE) [--speed N] [--loop]\n"
        "                      [--sac N] [--sic N] [--status-interval S --status-lat D --status-lon D]\n"
        "Plays a pingStation 3 to a skycued input: a capture file as recorded, or a received-stream\n"
        "recording / readsb JSON capture synthesised as pingStation JSON or CAT021 ed. 2.5");
    desc.add_options()("help,h", "show this help")(
        "udp", po::value(&udp_arg), "send datagrams to HOST:PORT (pingstation_json, or a udp CAT021 input)")(
        "tcp", po::value(&tcp_arg), "connect to HOST:PORT (a tcp_listen CAT021 input)")(
        "tcp-listen", po::value(&listen_port), "serve CAT021 on PORT (a tcp_connect input)")(
        "bind", po::value(&bind_arg), "with --tcp-listen: the local address (default 127.0.0.1)")(
        "iface", po::value(&iface),
        "with --udp to a multicast group (--udp GROUP:PORT): the local interface address to send from")(
        "capture", po::value(&capture_file), "replay a capture file (capture-<id>-*.jsonl)")(
        "as", po::value(&as_form), "json or cat021: synthesise from the recording FILE")(
        "speed", po::value(&speed)->default_value(1.0), "playback speed; 0 = as fast as possible")(
        "loop", "start again after the end")(
        "sac", po::value(&sac)->default_value(0), "CAT021 I021/010 SAC (0-255)")(
        "sic", po::value(&sic)->default_value(0), "CAT021 I021/010 SIC (0-255)")(
        "status-interval", po::value(&status_interval), "JSON only: send a status message every S seconds")(
        "status-lat", po::value(&status_lat), "the status message's latitude")(
        "status-lon", po::value(&status_lon), "the status message's longitude");
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
        std::cerr << "skycue-pingsim: " << e.what() << "\n" << desc << "\n";
        return 2;
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    auto usage = [](const std::string& msg) {
        std::cerr << "skycue-pingsim: " << msg << "\n";
        return 2;
    };
    if (vm.count("udp") + vm.count("tcp") + vm.count("tcp-listen") != 1)
        return usage("give exactly one of --udp, --tcp and --tcp-listen");
    Mode mode = Mode::udp;
    std::optional<HostPort> to;
    if (vm.count("udp")) {
        to = parseHostPort(udp_arg);
        if (!to)
            return usage("--udp must be HOST:PORT, got '" + udp_arg + "'");
    }
    if (vm.count("tcp")) {
        mode = Mode::tcp;
        to = parseHostPort(tcp_arg);
        if (!to)
            return usage("--tcp must be HOST:PORT, got '" + tcp_arg + "'");
    }
    if (vm.count("tcp-listen")) {
        mode = Mode::tcp_listen;
        if (listen_port < 1 || listen_port > 65535)
            return usage("--tcp-listen must be a port number 1-65535");
    } else if (vm.count("bind")) {
        return usage("--bind needs --tcp-listen");
    }
    if (vm.count("iface") && !vm.count("udp"))
        return usage("--iface needs --udp");
    error_code bind_ec;
    const auto bind_addr = asio::ip::make_address(bind_arg, bind_ec);
    if (bind_ec)
        return usage("--bind must be an IP address, got '" + bind_arg + "'");

    if (vm.count("capture") == vm.count("as"))
        return usage("give exactly one of --capture FILE and --as json|cat021 FILE");
    Form form = Form::capture;
    std::string path = capture_file;
    if (vm.count("as")) {
        if (as_form == "json")
            form = Form::json;
        else if (as_form == "cat021")
            form = Form::cat021;
        else
            return usage("--as must be json or cat021, got '" + as_form + "'");
        if (files.size() != 1)
            return usage("--as needs one FILE");
        path = files[0];
    } else if (!files.empty()) {
        return usage("unexpected argument '" + files[0] + "'");
    }
    if (form == Form::json && mode != Mode::udp)
        return usage("--as json is UDP only (the pingstation_json input listens on UDP)");
    if (!std::isfinite(speed) || speed < 0)
        return usage("--speed must be 0 or a positive number");
    if (sac < 0 || sac > 255 || sic < 0 || sic > 255)
        return usage("--sac and --sic must be 0-255");

    std::optional<StatusOptions> status;
    const auto status_keys = vm.count("status-interval") + vm.count("status-lat") + vm.count("status-lon");
    if (status_keys != 0) {
        if (status_keys != 3)
            return usage("--status-interval, --status-lat and --status-lon go together");
        if (form != Form::json)
            return usage("--status-interval needs --as json");
        if (!std::isfinite(status_interval) || status_interval < 0.1 || status_interval > 86400)
            return usage("--status-interval must be 0.1-86400 seconds");
        if (!(std::fabs(status_lat) <= 90) || !(std::fabs(status_lon) <= 180))
            return usage("--status-lat must be -90..90 and --status-lon -180..180");
        status = StatusOptions{std::chrono::duration<double>(status_interval), status_lat, status_lon};
    }

    try {
        GzLineReader probe(path);  // fail now rather than once connected
    } catch (const std::exception& e) {
        std::cerr << "skycue-pingsim: " << e.what() << "\n";
        return 1;
    }

    // NOLINTNEXTLINE(cert-err33-c): constant, valid arguments: cannot fail
    std::signal(SIGPIPE, SIG_IGN);
    installNetLogSink();
    asio::io_context io;
    std::optional<Output> out;
    try {
        out.emplace(io, mode, to.value_or(HostPort{}), bind_addr, static_cast<uint16_t>(listen_port), iface);
    } catch (const std::exception& e) {
        std::cerr << "skycue-pingsim: --tcp-listen " << bind_arg << ":" << listen_port << ": " << e.what() << "\n";
        return 1;
    }
    const bool loop = vm.count("loop") > 0;
    try {
        out->open();
        Pacer pacer(speed, *out, status);
        pacer.start();
        Stats st;
        int64_t media = 0;
        for (unsigned pass = 0;; ++pass) {
            LOG_INFO("playing " << path << (pass ? " (pass " + std::to_string(pass + 1) + ")" : ""));
            const uint64_t before = st.sent;
            media = playFile(path, form, mode, static_cast<uint8_t>(sac), static_cast<uint8_t>(sic), media, *out,
                             pacer, st);
            if (!loop)
                break;
            if (st.sent == before) {  // nothing to play: do not spin
                std::cerr << "skycue-pingsim: " << path << ": --loop, but a pass sent nothing\n";
                return 1;
            }
            media += kGapNs;
        }
        out->close();
        LOG_INFO("done: " << st.sent << " records sent, " << st.skipped << " lines skipped, " << pacer.statuses()
                          << " status messages, " << out->lost() << (mode == Mode::udp ? " send failures" : " reconnects"));
    } catch (const std::exception& e) {
        std::cerr << "skycue-pingsim: " << e.what() << "\n";
        return 1;
    }
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
        std::cerr << "skycue-pingsim: " << e.what() << "\n";
        return 1;
    }
}
