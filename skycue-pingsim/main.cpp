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
// Output: --udp HOST:PORT (one datagram per record), --tcp HOST:PORT (connects,
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
#include "log.hpp"
#include "recording_reader.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace po = boost::program_options;
using asio::ip::tcp;
using asio::ip::udp;
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

// Where the records go. Blocking, on the one io_context.
class Output {
public:
    Output(asio::io_context& io, Mode mode, HostPort to, const asio::ip::address& bind, uint16_t listen_port)
        : io_(io), mode_(mode), to_(std::move(to)), tcp_(io), udp_(io)
    {
        if (mode_ == Mode::tcp_listen) {
            const tcp::endpoint ep(bind, listen_port);
            acceptor_.emplace(io_);
            acceptor_->open(ep.protocol());
            acceptor_->set_option(tcp::acceptor::reuse_address(true));
            acceptor_->bind(ep);  // throws when the port is taken
            acceptor_->listen();
        }
    }

    // UDP: resolves the target. TCP: blocks until connected (retrying with
    // backoff) or until a client connected.
    void open()
    {
        auto backoff = kMinBackoff;
        bool warned = false;
        for (;;) {
            error_code ec;
            if (mode_ == Mode::tcp_listen) {
                acceptor_->accept(tcp_, ec);
                if (!ec) {
                    LOG_INFO("client " << tcp_.remote_endpoint(ec) << " connected");
                    return;
                }
                LOG_WARN("accept failed: " << ec.message());
            } else if (mode_ == Mode::udp) {
                udp::resolver resolver(io_);
                auto results = resolver.resolve(to_.host, to_.port, ec);
                if (!ec && !results.empty()) {
                    target_ = results.begin()->endpoint();
                    if (udp_.is_open())
                        udp_.close(ec);
                    udp_.open(target_.protocol(), ec);
                    if (!ec)
                        return;
                }
                if (!ec)
                    ec = asio::error::host_not_found;
                LOG_WARN(to_.host << ":" << to_.port << ": " << ec.message() << "; retrying");
            } else {
                tcp::resolver resolver(io_);
                auto results = resolver.resolve(to_.host, to_.port, ec);
                if (!ec)
                    asio::connect(tcp_, results, ec);
                if (!ec) {
                    LOG_INFO("connected to " << to_.host << ":" << to_.port);
                    return;
                }
                if (!warned)
                    LOG_WARN(to_.host << ":" << to_.port << ": " << ec.message() << "; retrying until it accepts");
                warned = true;
                error_code ignored;
                tcp_.close(ignored);
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, kMaxBackoff);
        }
    }

    // A failed TCP write drops the record and connects again (a new client,
    // for --tcp-listen). A failed datagram is dropped and counted.
    void send(const std::vector<uint8_t>& bytes)
    {
        error_code ec;
        if (mode_ == Mode::udp) {
            udp_.send_to(asio::buffer(bytes), target_, 0, ec);
            if (ec) {
                if (lost_++ == 0)
                    LOG_WARN("send failed: " << ec.message() << " (logged once)");
            }
            return;
        }
        asio::write(tcp_, asio::buffer(bytes), ec);
        if (ec) {
            LOG_WARN("write failed (" << ec.message() << "); reconnecting");
            error_code ignored;
            tcp_.close(ignored);
            ++lost_;
            open();
        }
    }

    void close()
    {
        error_code ignored;
        if (mode_ == Mode::udp) {
            udp_.close(ignored);
            return;
        }
        tcp_.shutdown(tcp::socket::shutdown_send, ignored);
        tcp_.close(ignored);
        if (acceptor_)
            acceptor_->close(ignored);
    }

    unsigned lost() const { return lost_; }

private:
    asio::io_context& io_;
    Mode mode_;
    HostPort to_;
    tcp::socket tcp_;
    udp::socket udp_;
    udp::endpoint target_;
    std::optional<tcp::acceptor> acceptor_;
    unsigned lost_ = 0;
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
    Pacer(asio::io_context& io, double speed, Output& out, std::optional<StatusOptions> status)
        : timer_(io), speed_(speed), out_(out), status_(status)
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
    void sleepUntil(steady::time_point t)
    {
        if (steady::now() >= t)
            return;
        timer_.expires_at(t);
        error_code ignored;
        timer_.wait(ignored);
    }

    asio::steady_timer timer_;
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
    std::string udp_arg, tcp_arg, capture_file, as_form, bind_arg = "127.0.0.1";
    int listen_port = 0, sac = 0, sic = 0;
    double speed = 1.0, status_interval = 0, status_lat = 0, status_lon = 0;
    std::vector<std::string> files;
    po::options_description desc(
        "usage: skycue-pingsim (--udp HOST:PORT | --tcp HOST:PORT | --tcp-listen PORT [--bind ADDR])\n"
        "                      (--capture FILE | --as json|cat021 FILE) [--speed N] [--loop]\n"
        "                      [--sac N] [--sic N] [--status-interval S --status-lat D --status-lon D]\n"
        "Plays a pingStation 3 to a skycued input: a capture file as recorded, or a received-stream\n"
        "recording / readsb JSON capture synthesised as pingStation JSON or CAT021 ed. 2.5");
    desc.add_options()("help,h", "show this help")(
        "udp", po::value(&udp_arg), "send datagrams to HOST:PORT (pingstation_json, or a udp CAT021 input)")(
        "tcp", po::value(&tcp_arg), "connect to HOST:PORT (a tcp_listen CAT021 input)")(
        "tcp-listen", po::value(&listen_port), "serve CAT021 on PORT (a tcp_connect input)")(
        "bind", po::value(&bind_arg), "with --tcp-listen: the local address (default 127.0.0.1)")(
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
    asio::io_context io;
    std::optional<Output> out;
    try {
        out.emplace(io, mode, to.value_or(HostPort{}), bind_addr, static_cast<uint16_t>(listen_port));
    } catch (const std::exception& e) {
        std::cerr << "skycue-pingsim: --tcp-listen " << bind_arg << ":" << listen_port << ": " << e.what() << "\n";
        return 1;
    }
    const bool loop = vm.count("loop") > 0;
    try {
        out->open();
        Pacer pacer(io, speed, *out, status);
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
