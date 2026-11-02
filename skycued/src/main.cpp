// skycued: reads readsb's JSON output (and dump978's raw UAT), filters and
// rate-limits the tracks, and forwards them to skycue clients.
//
// Exit status: 0 after SIGINT/SIGTERM (or --help/--version), 1 on a runtime
// failure (a port that cannot be bound, a recording directory that cannot be
// created), 2 on a usage or configuration error.

#include "config.hpp"
#include "dump978_raw_source.hpp"
#include "engine.hpp"
#include "forwarder.hpp"
#include "log.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "replay_source.hpp"
#include "runtime_config.hpp"
#include "web_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace po = boost::program_options;
using namespace skycued;

namespace {

constexpr const char* kVersion = "skycued 0.1.0";

std::optional<LogLevel> parseLogLevel(const std::string& s)
{
    if (s == "debug")
        return LogLevel::debug;
    if (s == "info")
        return LogLevel::info;
    if (s == "warn")
        return LogLevel::warn;
    if (s == "error")
        return LogLevel::error;
    return std::nullopt;
}

// <directory of this executable>/../share/skycue/www, normalised when it exists.
std::string defaultWwwDir()
{
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string dir = ".";
    if (n > 0) {
        const std::string exe(buf, static_cast<size_t>(n));
        const auto slash = exe.rfind('/');
        if (slash != std::string::npos)
            dir = slash == 0 ? "/" : exe.substr(0, slash);
    }
    const std::string www = dir + "/../share/skycue/www";
    char real[PATH_MAX];
    return ::realpath(www.c_str(), real) ? std::string(real) : www;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string config_path, replay_path, readsb_host, dump978_host, log_level, www_dir, web_bind;
    double replay_speed = 1.0;
    unsigned cue_tcp_port = 0, web_port = 0;
    po::options_description desc(
        "usage: skycued --config PATH [--replay FILE [--replay-speed N] [--replay-loop]] [options]\n"
        "Forwards readsb's aircraft tracks to skycue clients");
    desc.add_options()("help,h", "show this help")("version", "print the version")(
        "config,c", po::value(&config_path), "skycue.conf (required)")(
        "replay", po::value(&replay_path), "play back a received-stream JSONL recording (or a readsb JSON capture) "
                                           "instead of connecting to readsb; no UAT")(
        "replay-speed", po::value(&replay_speed)->default_value(1.0), "replay pacing: 1 = real time, 0 = unpaced")(
        "replay-loop", "restart the replay at the end of the file")(
        "readsb-host", po::value(&readsb_host)->default_value("127.0.0.1"), "host of readsb's JSON port")(
        "dump978-host", po::value(&dump978_host)->default_value("127.0.0.1"), "host of dump978's raw port")(
        "no-uat", "do not connect to dump978")(
        "cue-tcp-port", po::value(&cue_tcp_port), "override CUE_TCP_PORT (0: no listener)")(
        "www-dir", po::value(&www_dir), "the web UI's files (default: <bin>/../share/skycue/www)")(
        "web-bind", po::value(&web_bind), "override WEB_BIND (numeric address)")(
        "web-port", po::value(&web_port), "override WEB_PORT (0: no web UI)")(
        "log-level", po::value(&log_level)->default_value("info"), "debug, info, warn or error");
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n" << desc << "\n";
        return 2;
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    if (vm.count("version")) {
        std::cout << kVersion << "\n";
        return 0;
    }
    const auto level = parseLogLevel(log_level);
    if (!level) {
        std::cerr << "skycued: --log-level must be debug, info, warn or error, got '" << log_level << "'\n";
        return 2;
    }
    setLogLevel(*level);
    if (config_path.empty()) {
        std::cerr << "skycued: --config is required\n";
        return 2;
    }
    if (vm.count("cue-tcp-port") && cue_tcp_port > 65535) {
        std::cerr << "skycued: --cue-tcp-port must be 0..65535\n";
        return 2;
    }
    if (vm.count("web-port") && web_port > 65535) {
        std::cerr << "skycued: --web-port must be 0..65535\n";
        return 2;
    }
    if (vm.count("web-bind") && web_bind.empty()) {
        std::cerr << "skycued: --web-bind must not be empty\n";
        return 2;
    }
    const bool replaying = vm.count("replay") > 0;
    if (replaying && (!std::isfinite(replay_speed) || replay_speed < 0)) {
        std::cerr << "skycued: --replay-speed must be 0 (unpaced) or positive\n";
        return 2;
    }

    Config config;
    try {
        config = parseConfigFile(config_path);
    } catch (const ConfigError& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    if (vm.count("cue-tcp-port"))
        config.cue_tcp_port = static_cast<uint16_t>(cue_tcp_port);
    if (vm.count("web-port"))
        config.web_port = static_cast<uint16_t>(web_port);
    if (vm.count("web-bind"))
        config.web_bind = web_bind;
    if (!vm.count("www-dir"))
        www_dir = defaultWwwDir();

    boost::asio::io_context io;

    ForwarderConfig fc;
    fc.tcp_port = config.cue_tcp_port;
    fc.tcp_connect_targets = config.cue_tcp_connect_targets;
    fc.udp_targets = config.cue_udp_targets;
    fc.keyframe_interval =
        std::chrono::milliseconds(std::max<long long>(1, std::llround(config.keyframe_interval_s * 1000)));
    Forwarder forwarder(io, fc);

    std::unique_ptr<Recorder> received, sent;
    if (!config.record_dir.empty()) {
        RecorderConfig rc;
        rc.dir = config.record_dir;
        rc.jsonl = config.record_formats.count("jsonl") > 0;
        rc.csv = config.record_formats.count("csv") > 0;
        try {
            received = std::make_unique<Recorder>(rc, "received");
            sent = std::make_unique<Recorder>(rc, "sent");
        } catch (const std::exception& e) {
            std::cerr << "skycued: recording: " << e.what() << "\n";
            return 1;
        }
    }

    std::unique_ptr<ObservationSource> source;
    std::unique_ptr<Dump978RawSource> uat;
    try {
        if (replaying)
            source = std::make_unique<ReplayObservationSource>(io, replay_path, replay_speed, vm.count("replay-loop") > 0);
        else
            source = std::make_unique<ReadsbJsonSource>(io, readsb_host, config.readsb_json_port);
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    if (!replaying && !vm.count("no-uat"))
        uat = std::make_unique<Dump978RawSource>(io, dump978_host, config.dump978_raw_port);

    std::unique_ptr<Engine> engine;
    try {
        engine = std::make_unique<Engine>(io, config, std::move(source), std::move(uat), forwarder, received.get(),
                                          sent.get());
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << config_path << ": " << e.what() << "\n";
        return 2;
    }
    // The runtime overlay (skycue.runtime.json) on top of skycue.conf, and
    // hot reload of both and of the list files.
    std::unique_ptr<RuntimeConfig> runtime;
    try {
        runtime = std::make_unique<RuntimeConfig>(io, config_path, *engine);
        runtime->load();
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    // The web UI (WEB_BIND:WEB_PORT); WEB_PORT=0: none.
    std::unique_ptr<WebServer> web;
    if (config.web_port == 0) {
        LOG_INFO("web UI disabled (WEB_PORT=0)");
    } else {
        WebConfig wc;
        wc.bind = config.web_bind;
        wc.port = config.web_port;
        wc.www_dir = www_dir;
        web = std::make_unique<WebServer>(io, *engine, *runtime, wc);
        try {
            web->start();
        } catch (const std::exception& e) {
            LOG_ERROR("cannot start: " << e.what());
            return 1;
        }
    }
    try {
        engine->start();
    } catch (const std::exception& e) {
        LOG_ERROR("cannot start: " << e.what());
        return 1;
    }
    LOG_INFO(kVersion << " forwarding"
                      << (forwarder.tcpPort() ? " on TCP port " + std::to_string(forwarder.tcpPort())
                                              : std::string(" with no TCP listener"))
                      << ", " << config.cue_tcp_connect_targets.size() << " TCP target(s), "
                      << config.cue_udp_targets.size() << " UDP target(s)"
                      << (replaying ? " (replaying " + replay_path + ")" : std::string()));
    if (config.cue_tcp_port == 0 && config.cue_tcp_connect_targets.empty() && config.cue_udp_targets.empty())
        LOG_WARN("nowhere to forward to: CUE_TCP_PORT is 0 and CUE_TCP_CONNECT and CUE_UDP_TARGETS are empty");

    runtime->startWatching();

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    boost::asio::steady_timer exit_timer(io);
    std::chrono::steady_clock::time_point exit_deadline{};
    // After stop(): exit once the clients have their data, after at most 3 s.
    std::function<void()> pollExit = [&] {
        if (forwarder.tcpClientCount() == 0 || std::chrono::steady_clock::now() >= exit_deadline) {
            io.stop();
            return;
        }
        exit_timer.expires_after(std::chrono::milliseconds(50));
        exit_timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec)
                pollExit();
        });
    };
    signals.async_wait([&](const boost::system::error_code& ec, int sig) {
        if (ec)
            return;
        LOG_INFO("signal " << sig << ": shutting down");
        if (web)
            web->stop();
        engine->stop();  // shutdown drops; the Forwarder closes its sockets within ~2 s
        exit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        pollExit();
    });
    io.run();
    web.reset();
    runtime.reset();
    engine.reset();
    if (received)
        received->flush();
    if (sent)
        sent->flush();
    return 0;
}
