// skycued: reads its inputs (readsb's JSON output, dump978's raw UAT, the
// receiver inputs of the inputs file), filters and rate-limits the tracks,
// and forwards them to skycue clients.
//
// Exit status: 0 after SIGINT/SIGTERM (or --help/--version), 1 on a runtime
// failure (a port that cannot be bound, a recording directory that cannot be
// created), 2 on a usage or configuration error (also an unreadable or
// invalid file given to --convert-conf), 75 restart requested (the supervisor
// starts skycued again) from the web UI under web.allow_restart.

#include "check_inputs.hpp"
#include "conf_convert.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "forwarder.hpp"
#include "hub/hub.hpp"
#include "inputs/beast_raw_input.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/inputs_file.hpp"
#include "inputs/source_input.hpp"
#include "inputs/uat_raw_input.hpp"
#include "log.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "replay_source.hpp"
#include "retention.hpp"
#include "runtime_config.hpp"
#include "web_config.hpp"
#include "web_server.hpp"

#include <skycue/version.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace po = boost::program_options;
using namespace skycued;

namespace {

constexpr const char* kVersion = "skycued " SKYCUE_VERSION_STRING;

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

// `value` as one shell word: single-quoted, a quote inside as '\''.
std::string shellQuote(const std::string& value)
{
    std::string out = "'";
    for (char c : value) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

// --dump-env: every key's effective value as a plain shell assignment
// (SKYCUE_INPUTS_READSB_HOST='127.0.0.1'), never exported: skycued reads
// SKYCUE_* from its environment, so exporting would pin each key to itself.
// An array is its entries in one string, separated by spaces.
std::string dumpEnv(const Config& config)
{
    std::string out;
    for (const auto& key : knownConfigKeys())
{
        // An unset position (possible here only: none given, nan or both 0.0) is an empty value.
        const bool unset = (key == "receiver.lat" || key == "receiver.lon") && !config.receiver_position_set;
        out += envNameOf(key) + "=" + shellQuote(unset ? std::string() : configValueText(config, key)) + "\n";
    }
    return out;
}

// --convert-conf FILE: the equivalent skycue.toml on stdout.
int runConvertConf(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "skycued: --convert-conf: cannot open " << path << "\n";
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        int dropped = 0;
        const std::string toml = convertOldConf(ss.str(), path, &dropped);
        std::cout << toml;
        std::cout.flush();
        if (dropped > 0)
            std::cerr << "skycued: --convert-conf: " << dropped << " comment line" << (dropped == 1 ? "" : "s")
                      << " of " << path << " not carried over (the example's comments are used)\n";
        return std::cout ? 0 : 1;
    } catch (const ConfigError& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
}

// --check-inputs: one connection attempt per input, one line each on stdout.
int runCheckInputs(const Config& config, const std::string& inputs_path, bool check_readsb,
                   const std::string& readsb_host, bool check_dump978, const std::string& dump978_host)
{
    bool readsb_on = check_readsb, dump978_on = check_dump978;
    std::vector<InputDef> user;
    std::vector<OutputDef> outputs;
    std::vector<std::string> warnings;
    std::string text;
    if (!inputs_path.empty()) {
        std::ifstream in(inputs_path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            if (text.size() > kMaxInputsFileBytes) {
                std::cerr << "skycued: " << inputs_path << ": file is too large\n";
                return 2;
            }
            try {
                const InputsFile file = parseInputsFile(text);
                std::vector<ReservedPort> reserved;
                if (config.cue_tcp_port != 0)
                    reserved.push_back({"tcp", config.cue_tcp_port, "the cue stream (cues.tcp_port)"});
                if (config.web_port != 0)
                    reserved.push_back({"tcp", config.web_port, "the web server (web.port)"});
                for (const auto& warning : validateInputSet(file.inputs, file.outputs, reserved))
                    warnings.push_back(warning);
                const auto r = file.builtin_enabled.find("readsb");
                if (r != file.builtin_enabled.end())
                    readsb_on = readsb_on && r->second;
                const auto d = file.builtin_enabled.find("dump978");
                if (d != file.builtin_enabled.end())
                    dump978_on = dump978_on && d->second;
                user = file.inputs;
                outputs = file.outputs;
            } catch (const std::exception& e) {
                std::cerr << "skycued: " << inputs_path << ": " << e.what() << "\n";
                return 2;
            }
        }
    }
    std::vector<InputCheck> checks;
    const InputDef readsb_def = builtinReadsbDef(config, readsb_host);
    if (readsb_on)
        checks.push_back({"readsb", readsb_def.host, readsb_def.port});
    if (dump978_on)
        checks.push_back({"dump978", dump978_host, config.dump978_raw_port});
    std::vector<std::string> unchecked;
    for (const auto& d : user) {
        if (!d.enabled)
            continue;
        if (d.transport == InputTransport::tcp_connect)
            checks.push_back({d.id, d.host, d.port});
        else
            unchecked.push_back(d.id + ": not checked (listens on " + d.bind + ":" + std::to_string(d.port) + ")");
    }
    if (config.track_decoder == "skycued") {
        std::vector<InputDef> all;
        if (readsb_on)
            all.push_back(readsb_def);
        if (dump978_on) {
            InputDef d978;
            d978.id = "dump978";
            d978.type = InputType::dump978;
            d978.builtin = true;
            all.push_back(d978);
        }
        all.insert(all.end(), user.begin(), user.end());
        for (auto& warning : decoderWarnings(all, outputs))
            warnings.push_back(std::move(warning));
    }
    for (const auto& warning : warnings)
        std::cout << "WARN: " << warning << "\n";
    if (checks.empty() && unchecked.empty()) {
        std::cout << "no inputs to check\n";
        return 0;
    }
    bool ok = true;
    for (const auto& r : checkInputs(checks, std::chrono::seconds(3))) {
        std::cout << inputCheckLine(r) << "\n";
        ok = ok && r.connected;
    }
    for (const auto& line : unchecked)
        std::cout << line << "\n";
    std::cout.flush();
    return ok ? 0 : 1;
}

// The command line.
struct Options {
    std::string config_path, convert_path, replay_path, readsb_host, dump978_host, log_level, www_dir, web_bind,
        inputs_file;
    std::vector<std::string> set_args;
    double replay_speed = 1.0;
    unsigned cue_tcp_port = 0, web_port = 0;
};

po::options_description makeOptionTable(Options& o)
{
    auto& config_path = o.config_path;
    auto& convert_path = o.convert_path;
    auto& replay_path = o.replay_path;
    auto& readsb_host = o.readsb_host;
    auto& dump978_host = o.dump978_host;
    auto& log_level = o.log_level;
    auto& www_dir = o.www_dir;
    auto& web_bind = o.web_bind;
    auto& inputs_file = o.inputs_file;
    auto& set_args = o.set_args;
    auto& replay_speed = o.replay_speed;
    auto& cue_tcp_port = o.cue_tcp_port;
    auto& web_port = o.web_port;
    po::options_description desc(
        "usage: skycued --config PATH [--replay FILE [--replay-speed N] [--replay-loop]] [options]\n"
        "Forwards readsb's aircraft tracks to skycue clients");
    desc.add_options()("help,h", "show this help")("version", "print the version")(
        "config,c", po::value(&config_path), "skycue.toml (required)")(
        "set", po::value(&set_args)->composing()->value_name("KEY=VALUE"),
        "override a config key for this run, e.g. --set inputs.readsb.host=10.0.0.2 (repeatable, a later one wins; "
        "above the SKYCUE_* environment)")(
        "dump-env", "print the effective configuration (all layers and flags applied) as shell assignments "
                    "SKYCUE_INPUTS_READSB_HOST='...', one per key, and exit; nothing is printed on an error (status 2)")(
        "convert-conf", po::value(&convert_path)->value_name("FILE"),
        "print the skycue.toml equivalent of an old skycue.conf (or skycue.overrides.conf) and exit")(
        "replay", po::value(&replay_path), "play back a received-stream JSONL recording (or a readsb JSON capture) "
                                           "instead of connecting to readsb; no UAT")(
        "replay-speed", po::value(&replay_speed)->default_value(1.0), "replay pacing: 1 = real time, 0 = unpaced")(
        "replay-loop", "restart the replay at the end of the file")(
        "readsb-host", po::value(&readsb_host), "host of readsb's JSON port (overrides inputs.readsb.host)")(
        "dump978-host", po::value(&dump978_host), "host of dump978's raw port (overrides inputs.dump978.host)")(
        "no-uat", "do not connect to dump978 (overrides inputs.dump978.enable)")(
        "no-readsb", "do not connect to readsb (overrides inputs.readsb.enable)")(
        "inputs-file", po::value(&inputs_file),
        "override inputs.file (default: skycue.inputs.json in paths.state_dir, else next to the config; "
        "empty: not persisted)")(
        "cue-tcp-port", po::value(&cue_tcp_port), "override cues.tcp_port (0: no listener)")(
        "www-dir", po::value(&www_dir), "the web UI's files (default: <bin>/../share/skycue/www)")(
        "web-bind", po::value(&web_bind), "override web.bind (numeric address)")(
        "web-port", po::value(&web_port), "override web.port (0: no web UI)")(
        "check-inputs",
        "connect once to each configured TCP input (readsb, dump978, tcp_connect inputs of the inputs file), "
        "print one line per input, exit 0 when all connected, 1 when one is not, 2 on a usage or configuration error; starts nothing else. Each check "
        "gives up after 3 s; a name lookup cannot be cancelled, so one that is still running then is "
        "abandoned and ends on the system resolver's own timeout")(
        "log-level", po::value(&log_level)->default_value("info"), "debug, info, warn or error");
    return desc;
}

// An exit status when the process should end here (help, version, --convert-conf, a usage error: 0 or 2),
// else nullopt.
std::optional<int> parseOptions(int argc, char** argv, Options& o, po::variables_map& vm)
{
    auto& config_path = o.config_path;
    auto& convert_path = o.convert_path;
    auto& readsb_host = o.readsb_host;
    auto& dump978_host = o.dump978_host;
    auto& log_level = o.log_level;
    auto& web_bind = o.web_bind;
    auto& replay_speed = o.replay_speed;
    auto& cue_tcp_port = o.cue_tcp_port;
    auto& web_port = o.web_port;
    const po::options_description desc = makeOptionTable(o);
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
    if (vm.count("convert-conf"))
        return runConvertConf(convert_path);
    const auto level = parseLogLevel(log_level);
    if (!level) {
        std::cerr << "skycued: --log-level must be debug, info, warn or error, got '" << log_level << "'\n";
        return 2;
    }
    setLogLevel(*level);
    // --check-inputs and --dump-env print their own lines; the library's log would add noise.
    if (vm.count("check-inputs") == 0 && vm.count("dump-env") == 0)
        installNetLogSink();
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
    // As for the inputs.readsb.host and inputs.dump978.host keys: an empty host is an error, not "use the key".
    for (const auto& [flag, value] : {std::pair<const char*, const std::string*>{"readsb-host", &readsb_host},
                                      {"dump978-host", &dump978_host}}) {
        if (vm.count(flag) && value->empty()) {
            std::cerr << "skycued: --" << flag << " must not be empty\n";
            return 2;
        }
    }
    const bool replaying = vm.count("replay") > 0;
    const bool check_only = vm.count("check-inputs") > 0;
    if (check_only && replaying) {
        std::cerr << "skycued: --check-inputs cannot be combined with --replay\n";
        return 2;
    }
    if (replaying && (!std::isfinite(replay_speed) || replay_speed < 0)) {
        std::cerr << "skycued: --replay-speed must be 0 (unpaced) or positive\n";
        return 2;
    }
    return std::nullopt;
}

// The layers (defaults, the file, its overrides file, the environment, --set) and the flags, validated; an exit
// status when the process should end here (--dump-env, an error), else nullopt.
std::optional<int> loadDaemonConfig(const Options& o, const po::variables_map& vm, Config& config,
                                    std::map<std::string, std::string>& env)
{
    const auto& config_path = o.config_path;
    const auto& set_args = o.set_args;
    const auto& readsb_host = o.readsb_host;
    const auto& dump978_host = o.dump978_host;
    const auto& web_bind = o.web_bind;
    const auto& inputs_file = o.inputs_file;
    const auto& cue_tcp_port = o.cue_tcp_port;
    const auto& web_port = o.web_port;
    // The layers: defaults, the file, its overrides file, the SKYCUE_* environment, --set.
    std::vector<std::string> unknown_env;
    env = environmentOverrides(&unknown_env);
    try {
        // --dump-env also serves tools that need no receiver position (the map scripts).
        config = loadConfig(config_path, set_args, env, vm.count("dump-env") == 0);
    } catch (const ConfigError& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    if (!unknown_env.empty()) {  // once, after the load; a typo must show at the default log level
        std::string names;
        for (const auto& n : unknown_env)
            names += (names.empty() ? "" : ", ") + n;
        LOG_WARN("config: ignoring the environment variable(s) " << names << " (no such key)");
    }
    // The command-line flags apply last. A flag given overrides its key; an absent one defers to it.
    if (vm.count("readsb-host"))
        config.readsb_host = readsb_host;
    if (vm.count("dump978-host"))
        config.dump978_host = dump978_host;
    if (vm.count("no-uat"))
        config.dump978_enable = false;
    if (vm.count("no-readsb"))
        config.inputs_readsb_enable = false;
    if (vm.count("cue-tcp-port"))
        config.cue_tcp_port = static_cast<uint16_t>(cue_tcp_port);
    if (vm.count("web-port"))
        config.web_port = static_cast<uint16_t>(web_port);
    if (vm.count("web-bind"))
        config.web_bind = web_bind;
    if (vm.count("dump-env")) {
        if (vm.count("inputs-file"))
            config.inputs_file = inputs_file;
        std::cout << dumpEnv(config);
        std::cout.flush();
        return std::cout ? 0 : 1;
    }
    if (!config.state_dir.empty()) {
        struct stat st{};
        if (::stat(config.state_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::cerr << "skycued: paths.state_dir " << config.state_dir << " is not a directory\n";
            return 2;
        }
    }
    return std::nullopt;
}

ForwarderConfig forwarderConfig(const Config& config)
{
    ForwarderConfig fc;
    fc.tcp_port = config.cue_tcp_port;
    fc.tcp_connect_targets = config.cue_tcp_connect_targets;
    fc.udp_targets = config.cue_udp_targets;
    fc.keyframe_interval =
        std::chrono::milliseconds(std::max<long long>(1, std::llround(config.keyframe_interval_s * 1000)));
    return fc;
}

// What run() builds and the loops use. The members are declared in the order the locals of run() were, so they
// are destroyed in the same order (the Hub before the Forwarder it republishes through, the io_context last).
struct Stack {
    explicit Stack(const Config& config)
        : signals(io, SIGINT, SIGTERM), forwarder(io, forwarderConfig(config)), hub(io, &forwarder)
    {
    }

    boost::asio::io_context io;
    // Registered before anything is logged or started: a SIGINT/SIGTERM that arrives during startup (a
    // supervisor stopping a unit that is still coming up, a test killing on the first log line) is held
    // by asio until async_wait below and then shuts down cleanly, instead of the default action ending
    // the process with status 143/130.
    boost::asio::signal_set signals;
    Forwarder forwarder;
    // The hub republishes raw messages to the operator's outputs; the cue
    // stream's destinations are its built-in outputs. After the Forwarder, so
    // it is destroyed first.
    Hub hub;
    std::unique_ptr<Recorder> received, sent, fisb;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<RuntimeConfig> runtime;
    DaemonInfo daemon;
    std::unique_ptr<WebServer> web;
};

// The hub, recorders, inputs, engine and runtime configuration; an exit status when startup fails, else nullopt.
std::optional<int> buildEngine(Stack& st, const Options& o, const po::variables_map& vm, const Config& config,
                               const std::map<std::string, std::string>& env,
                               const std::chrono::system_clock::time_point& started)
{
    auto& io = st.io;
    auto& forwarder = st.forwarder;
    auto& hub = st.hub;
    auto& received = st.received;
    auto& sent = st.sent;
    auto& fisb = st.fisb;
    auto& engine = st.engine;
    auto& runtime = st.runtime;
    auto& daemon = st.daemon;
    const auto& config_path = o.config_path;
    const auto& replay_path = o.replay_path;
    const auto& readsb_host = o.readsb_host;
    const auto& dump978_host = o.dump978_host;
    const auto& log_level = o.log_level;
    const auto& www_dir = o.www_dir;
    const auto& web_bind = o.web_bind;
    const auto& inputs_file = o.inputs_file;
    const auto& set_args = o.set_args;
    const auto& replay_speed = o.replay_speed;
    const auto& cue_tcp_port = o.cue_tcp_port;
    const auto& web_port = o.web_port;
    const bool replaying = vm.count("replay") > 0;
    const bool use_dump978 = config.dump978_enable;
    const bool use_readsb = config.inputs_readsb_enable;

    if (!config.record_dir.empty()) {
        RecorderConfig rc;
        rc.dir = config.record_dir;
        rc.jsonl = config.record_formats.count("jsonl") > 0;
        rc.csv = config.record_formats.count("csv") > 0;
        rc.max_bytes = gigabytesToBytes(config.record_max_gb);  // recording.dir and recording.dir/raw together
        if (rc.max_bytes > 0)
            LOG_INFO("recordings under " << config.record_dir << " are kept within " << config.record_max_gb
                                         << " GB (recording.max_gb), oldest deleted first");
        try {
            received = std::make_unique<Recorder>(rc, "received");
            sent = std::make_unique<Recorder>(rc, "sent");
            if (config.fisb_enable && rc.jsonl)  // FIS-B is recorded as JSONL only
                fisb = std::make_unique<Recorder>(rc, "fisb");
        } catch (const std::exception& e) {
            std::cerr << "skycued: recording: " << e.what() << "\n";
            return 1;
        }
    }

    // The inputs: the built-ins (readsb and dump978, or the replay alone) and,
    // unless replaying, the user inputs of the inputs file.
    InputManagerConfig mc;
    mc.inputs_file = vm.count("inputs-file") ? inputs_file : resolveInputsFile(config, config_path);
    const std::string inputs_path = mc.inputs_file;
    mc.record_dir = config.record_dir;
    mc.record_raw = config.record_raw;
    mc.record_max_bytes = gigabytesToBytes(config.record_max_gb);
    mc.replaying = replaying;
    mc.decode_raw = config.track_decoder == "skycued";
    if (mc.decode_raw)
        mc.receiver_ref = std::make_pair(config.receiver_lat, config.receiver_lon);
    // An output may not take a port the daemon serves itself.
    if (config.cue_tcp_port != 0)
        mc.reserved_ports.push_back({"tcp", config.cue_tcp_port, "the cue stream (cues.tcp_port)"});
    if (config.web_port != 0)
        mc.reserved_ports.push_back({"tcp", config.web_port, "the web server (web.port)"});
    auto inputs = std::make_unique<InputManager>(io, mc);
    {
        std::vector<std::string> cue_ids;
        for (const auto& d : forwarder.destinations())
            if (d.builtin)
                cue_ids.push_back(d.id);
        inputs->setBuiltinOutputIds(std::move(cue_ids));
    }
    inputs->onOutputsChanged([&hub](const std::vector<OutputDef>& defs, const std::map<std::string, bool>& builtin) {
        hub.apply(defs, builtin);
    });
    inputs->rawTap().setSink([&hub](const RawMessage& message) { hub.publish(message); },
                             [&hub](RawKind kind) { return hub.wants(kind); });
    auto builtin = [](const char* id, InputType type, const std::string& host, uint16_t port) {
        InputDef d;
        d.id = id;
        d.type = type;
        d.host = host;
        d.port = port;
        return d;
    };
    try {
        if (replaying) {
            inputs->addBuiltin(std::make_unique<SourceInput>(
                builtin("replay", InputType::replay, "", 0),
                std::make_unique<ReplayObservationSource>(io, replay_path, replay_speed, vm.count("replay-loop") > 0)));
        } else {
            if (use_readsb) {
                if (mc.decode_raw) {
                    RawDecodeOptions decode;
                    decode.enabled = true;
                    decode.reference = mc.receiver_ref;
                    inputs->addBuiltin(std::make_unique<BeastRawInput>(
                        io, builtinReadsbDef(config, readsb_host),
                        RecordOptions{mc.record_dir, mc.record_raw, mc.record_max_bytes},
                        std::move(decode)));
                } else {
                    const uint16_t port = config.readsb_json_port;
                    inputs->addBuiltin(std::make_unique<SourceInput>(
                        builtin("readsb", InputType::readsb, readsb_host, port),
                        SourceInput::SourceFactory([&io, readsb_host, port]() -> std::unique_ptr<ObservationSource> {
                            return std::make_unique<ReadsbJsonSource>(io, readsb_host, port);
                        })));
                }
            }
            if (use_dump978) {
                // Decoding only under inputs.decoder=skycued; under readsb the line is counted and published alone.
                RawDecodeOptions decode;
                decode.enabled = mc.decode_raw;
                decode.reference = mc.receiver_ref;
                inputs->addBuiltin(std::make_unique<UatRawInput>(
                    io, builtin("dump978", InputType::dump978, dump978_host, config.dump978_raw_port),
                    RecordOptions{}, std::move(decode)));
            }
        }
        inputs->load();
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    // The outputs of the inputs file (none while replaying) and the cue
    // stream's enabled states; later changes arrive through onOutputsChanged.
    hub.apply(inputs->outputs(), inputs->outputBuiltinStates());

    try {
        engine = std::make_unique<Engine>(io, config, std::move(inputs), forwarder, received.get(), sent.get(),
                                          fisb.get());
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << config_path << ": " << e.what() << "\n";
        return 2;
    }
    engine->setHub(&hub);
    hub.onChanged([&engine] { engine->outputsChanged(); });
    // The learned FIS-B report locations (fisb-locations.json in paths.state_dir,
    // else next to the config) and paths.locations_file under them.
    try {
        engine->loadLocations(resolveStatePath(config, config_path, "fisb-locations.json"),
                              resolveStatePath(config, config_path, config.locations_file));
        // The statistics (stats.file, skycue.stats.json in paths.state_dir or next
        // to the config unless set; empty: not saved).
        engine->loadStats(resolveStatePath(config, config_path, config.stats_file),
                          std::chrono::seconds(config.stats_save_s));
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    // The runtime overlay (skycue.runtime.json in paths.state_dir, else next to the
    // config) on top of skycue.toml, and hot reload of both and of the list files.
    try {
        runtime = std::make_unique<RuntimeConfig>(io, config_path, *engine);
        runtime->setLayers(set_args, env);
        runtime->load();
    } catch (const std::exception& e) {
        std::cerr << "skycued: " << e.what() << "\n";
        return 2;
    }
    // What GET /api/config reports besides the configuration.
    daemon.version = kVersion;
    daemon.started = started;
    daemon.config_path = config_path;
    daemon.www_dir = www_dir;
    daemon.map_dir = config.map_dir.empty() ? www_dir : config.map_dir;
    daemon.log_level = log_level;
    daemon.inputs_path = inputs_path;
    daemon.supervisor = std::getenv("INVOCATION_ID") ? "systemd" : std::getenv("SKYCUE_LAUNCHER") ? "launcher" : "none";
    if (vm.count("cue-tcp-port"))
        daemon.cli_overrides["cues.tcp_port"] = std::to_string(cue_tcp_port);
    if (vm.count("web-port"))
        daemon.cli_overrides["web.port"] = std::to_string(web_port);
    if (vm.count("web-bind"))
        daemon.cli_overrides["web.bind"] = web_bind;
    if (vm.count("inputs-file"))
        daemon.cli_overrides["inputs.file"] = inputs_file;
    if (vm.count("readsb-host"))
        daemon.cli_overrides["inputs.readsb.host"] = readsb_host;
    if (vm.count("dump978-host"))
        daemon.cli_overrides["inputs.dump978.host"] = dump978_host;
    if (vm.count("no-uat"))
        daemon.cli_overrides["inputs.dump978.enable"] = "false";
    if (vm.count("no-readsb"))
        daemon.cli_overrides["inputs.readsb.enable"] = "false";
    // Keys a flag sets are pinned like the environment and --set: the Config tab cannot change them.
    {
        std::set<std::string> flag_keys;
        for (const auto& kv : daemon.cli_overrides)
            flag_keys.insert(kv.first);
        runtime->setFlagKeys(std::move(flag_keys));
    }
    // One line per key whose value is not the main file's or the default's, with where it came from.
    for (const auto& src : config.sources)
        if (!daemon.cli_overrides.count(src.key))
            LOG_INFO("config: " << src.key << " = " << configValueText(config, src.key) << " (" << src.source << ")");
    for (const auto& [key, value] : daemon.cli_overrides)
        LOG_INFO("config: " << key << " = " << value << " (flag)");
    daemon.recorders = {{"received", received.get()}, {"sent", sent.get()}, {"fisb", fisb.get()}};
    if (replaying) {
        daemon.replay_file = replay_path;
        daemon.replay_speed = replay_speed;
        daemon.replay_loop = vm.count("replay-loop") > 0;
    }
    daemon.readsb = !replaying && use_readsb;
    daemon.dump978 = !replaying && use_dump978;
    return std::nullopt;
}

int run(int argc, char** argv)
{
    const auto started = std::chrono::system_clock::now();
    Options o;
    po::variables_map vm;
    if (const auto rc = parseOptions(argc, argv, o, vm))
        return *rc;
    auto& config_path = o.config_path;
    auto& replay_path = o.replay_path;
    auto& readsb_host = o.readsb_host;
    auto& dump978_host = o.dump978_host;
    auto& www_dir = o.www_dir;
    auto& inputs_file = o.inputs_file;
    const bool replaying = vm.count("replay") > 0;
    const bool check_only = vm.count("check-inputs") > 0;

    std::map<std::string, std::string> env;
    Config config;
    if (const auto rc = loadDaemonConfig(o, vm, config, env))
        return *rc;
    readsb_host = config.readsb_host;
    dump978_host = config.dump978_host;
    const bool use_dump978 = config.dump978_enable;
    const bool use_readsb = config.inputs_readsb_enable;
    if (!vm.count("www-dir"))
        www_dir = defaultWwwDir();

    if (check_only)
        return runCheckInputs(config, vm.count("inputs-file") ? inputs_file : resolveInputsFile(config, config_path),
                              use_readsb, readsb_host, use_dump978, dump978_host);

    Stack st(config);
    if (const auto rc = buildEngine(st, o, vm, config, env, started))
        return *rc;
    auto& io = st.io;
    auto& signals = st.signals;
    auto& forwarder = st.forwarder;
    auto& hub = st.hub;
    auto& received = st.received;
    auto& sent = st.sent;
    auto& fisb = st.fisb;
    auto& engine = st.engine;
    auto& runtime = st.runtime;
    auto& daemon = st.daemon;
    // The web UI (web.bind:web.port); web.port=0: none.
    auto& web = st.web;
    int exit_code = 0;
    bool shutting_down = false;
    boost::asio::steady_timer restart_timer(io);
    std::function<void(const std::string&)> shutdown;  // set below, once the exit machinery exists
    if (config.web_port == 0) {
        LOG_INFO("web UI disabled (web.port=0)");
    } else {
        WebConfig wc;
        wc.bind = config.web_bind;
        wc.port = config.web_port;
        wc.www_dir = www_dir;
        wc.map_dir = config.map_dir;
        wc.daemon = &daemon;
        wc.on_restart = [&] {
            // Deferred so the 202 reply is written before the web server stops
            // (250 ms is a heuristic: a local reply is written within a few ms,
            // and the web server drops its connections as it stops). A signal
            // that arrives first makes this a no-op (exit 0, shutting_down is
            // set); once the timer has fired the restart wins (exit 75), and
            // a later signal finds shutting_down set and does nothing.
            restart_timer.expires_after(std::chrono::milliseconds(250));
            restart_timer.async_wait([&](const boost::system::error_code& ec) {
                if (ec || shutting_down)
                    return;
                exit_code = 75;
                shutdown("restart requested from the web UI");
            });
        };
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
        LOG_WARN("nowhere to forward to: cues.tcp_port is 0 and cues.tcp_connect and cues.udp_targets are empty");

    runtime->startWatching();

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
    shutdown = [&](const std::string& why) {
        if (shutting_down)
            return;
        shutting_down = true;
        LOG_INFO(why << ": shutting down");
        if (web)
            web->stop();
        engine->stop();  // shutdown drops; the Forwarder closes its sockets within ~2 s
        hub.stop();      // the user outputs; the cue destinations are the Forwarder's to drain
        exit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        pollExit();
    };
    signals.async_wait([&](const boost::system::error_code& ec, int sig) {
        if (!ec)
            shutdown("signal " + std::to_string(sig));
    });
    io.run();
    web.reset();
    runtime.reset();
    engine.reset();
    if (received)
        received->flush();
    if (sent)
        sent->flush();
    if (fisb)
        fisb->flush();
    return exit_code;
}

}  // namespace

// What escapes run() (io.run() rethrows a handler's exception; a constructor
// can throw) ends the program with a message and status 1, not terminate().
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: " << e.what());
        return 1;
    }
}
