// skycue-dump: connects to a skycue stream (TCP or UDP) and prints one line
// per frame, as text or as JSON. A pure libskycue consumer: everything it
// shows comes through skycue::Client, so it links nothing from skycued.

#include <skycue/client.hpp>
#include <skycue/protocol.hpp>
#include <skycue/types.hpp>

#include <boost/program_options.hpp>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace po = boost::program_options;
using namespace skycue;

namespace {

volatile std::sig_atomic_t g_signalled = 0;
extern "C" void onSignal(int) { g_signalled = 1; }

// 2026-11-01T12:00:00.123Z
std::string iso8601(std::chrono::system_clock::time_point tp)
{
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    long long secs = ms / 1000, frac = ms % 1000;
    if (frac < 0) { // floor for times before the epoch
        frac += 1000;
        --secs;
    }
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    if (!gmtime_r(&t, &tm))
        return "invalid-time";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(frac));
    return buf;
}

// A JSON string literal. Control characters and bytes >= 0x7f are written as
// \u00XX (the latter read as Latin-1), so the output is always plain ASCII.
std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

// Text form: non-printable bytes and spaces become '?' so a value stays one word.
std::string textWord(const std::string& s)
{
    std::string out = s;
    for (char& ch : out)
        if (static_cast<unsigned char>(ch) <= 0x20 || static_cast<unsigned char>(ch) >= 0x7f)
            ch = '?';
    return out.empty() ? "?" : out;
}

std::string num(double v, const char* fmt = "%g")
{
    if (!std::isfinite(v))
        return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}
std::string num(long long v) { return std::to_string(v); }

const char* name(AddressType t)
{
    switch (t) {
    case AddressType::icao: return "icao";
    case AddressType::non_icao: return "non_icao";
    case AddressType::tisb_trackfile: return "tisb_trackfile";
    case AddressType::other: return "other";
    }
    return "unknown";
}
const char* name(Source s)
{
    switch (s) {
    case Source::adsb_1090: return "adsb_1090";
    case Source::uat_978: return "uat_978";
    case Source::tisb: return "tisb";
    case Source::adsr: return "adsr";
    case Source::mlat: return "mlat";
    case Source::other: return "other";
    }
    return "unknown";
}
const char* name(DropReason r)
{
    switch (r) {
    case DropReason::timeout: return "timeout";
    case DropReason::filtered: return "filtered";
    case DropReason::source_lost: return "source_lost";
    case DropReason::shutdown: return "shutdown";
    case DropReason::stale: return "stale";
    }
    return "unknown";
}

std::string hex(uint32_t address)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06X", static_cast<unsigned>(address & 0xFFFFFFu));
    return buf;
}

// One output line. Text: "<time> TAG word word ..."; JSON: {"time":..,
// "frame":.., key: value, ...}. add() takes the JSON value already encoded and
// the text word ("" = not shown in text).
class Line {
public:
    Line(bool json, const char* frame, const char* tag) : json_(json)
    {
        const std::string now = iso8601(std::chrono::system_clock::now());
        if (json_)
            s_ = "{\"time\":" + jsonString(now) + ",\"frame\":" + jsonString(frame);
        else
            s_ = now + " " + tag;
    }
    Line& add(const char* key, const std::string& json_value, const std::string& text_word)
    {
        if (json_) {
            s_ += ",";
            s_ += jsonString(key);
            s_ += ":" + json_value;
        } else if (!text_word.empty()) {
            s_ += " " + text_word;
        }
        return *this;
    }
    std::string str() const { return s_ + (json_ ? "}\n" : "\n"); }

private:
    bool json_;
    std::string s_;
};

template <class T, class F> void addOpt(Line& l, const char* key, const char* tag, const std::optional<T>& v, F fmt)
{
    if (v) {
        const std::string s = fmt(*v);
        l.add(key, s, std::string(tag) + "=" + s);
    }
}

std::string updateLine(bool json, const Track& t)
{
    Line l(json, "update", "UPDATE");
    const std::string h = hex(t.id.address);
    l.add("hex", jsonString(h), h)
        .add("addr_type", jsonString(name(t.id.type)), name(t.id.type))
        .add("source", jsonString(name(t.source)), name(t.source))
        .add("lat", num(t.lat_deg, "%.7f"), num(t.lat_deg, "%.7f"))
        .add("lon", num(t.lon_deg, "%.7f"), num(t.lon_deg, "%.7f"))
        .add("position_time", jsonString(iso8601(t.position_time)), "");
    const auto i = [](int32_t v) { return num(static_cast<long long>(v)); };
    const auto f = [](float v) { return num(static_cast<double>(v)); };
    const auto u = [](uint8_t v) { return num(static_cast<long long>(v)); };
    const auto txt = [json](const std::string& v) { return json ? jsonString(v) : textWord(v); };
    addOpt(l, "alt_baro_ft", "alt_baro", t.alt_baro_ft, i);
    addOpt(l, "alt_geom_ft", "alt_geom", t.alt_geom_ft, i);
    addOpt(l, "ground_speed_kt", "gs", t.ground_speed_kt, f);
    addOpt(l, "track_deg", "trk", t.track_deg, f);
    addOpt(l, "vertical_rate_fpm", "vr", t.vertical_rate_fpm, f);
    addOpt(l, "callsign", "cs", t.callsign, txt);
    addOpt(l, "squawk", "sq", t.squawk, txt);
    addOpt(l, "registration", "reg", t.registration, txt);
    addOpt(l, "nic", "nic", t.nic, u);
    addOpt(l, "nac_p", "nacp", t.nac_p, u);
    l.add("on_ground", t.on_ground ? "true" : "false", t.on_ground ? "gnd" : "")
        .add("alt_is_geom", t.alt_is_geom ? "true" : "false", "")
        .add("keyframe", t.keyframe ? "true" : "false", t.keyframe ? "kf" : "");
    return l.str();
}

bool parseEndpoint(const std::string& s, std::string& host, uint16_t& port)
{
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == s.size())
        return false;
    const std::string p = s.substr(colon + 1);
    if (p.find_first_not_of("0123456789") != std::string::npos || p.size() > 5)
        return false;
    const unsigned long v = std::stoul(p);
    if (v == 0 || v > 65535)
        return false;
    host = s.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') // [::1]:30500
        host = host.substr(1, host.size() - 2);
    port = static_cast<uint16_t>(v);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string tcp_ep, udp_ep;
    unsigned long count = 0;
    double timeout_s = 0;
    po::options_description desc("usage: skycue-dump [--tcp HOST:PORT | --udp BIND:PORT] [--json] [--count N] "
                                 "[--timeout S]\n"
                                 "Prints a skycue stream one line per frame (HELLO, UPDATE, DROP, HB, CONN)");
    desc.add_options()("help,h", "show this help")(
        "tcp", po::value(&tcp_ep), "connect to a skycued TCP port (default 127.0.0.1:30500)")(
        "udp", po::value(&udp_ep), "bind a UDP port and print the datagrams sent to it")(
        "json", "one JSON object per line instead of text")(
        "count", po::value(&count), "exit 0 after N lines (every frame kind counts, heartbeats too)")(
        "timeout", po::value(&timeout_s), "exit after S seconds: 0 if anything was printed, else 3");
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "skycue-dump: " << e.what() << "\n" << desc << "\n";
        return 2;
    }
    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }
    if (vm.count("tcp") && vm.count("udp")) {
        std::cerr << "skycue-dump: --tcp and --udp are mutually exclusive\n";
        return 2;
    }
    if (vm.count("count") && count == 0) {
        std::cerr << "skycue-dump: --count must be at least 1\n";
        return 2;
    }
    if (vm.count("timeout") && !(std::isfinite(timeout_s) && timeout_s > 0)) {
        std::cerr << "skycue-dump: --timeout must be a positive number of seconds\n";
        return 2;
    }

    Config cfg;
    cfg.run_io_thread = false; // everything, printing included, on this thread
    cfg.reconnect_interval = std::chrono::milliseconds(500);
    const bool udp = vm.count("udp") > 0;
    cfg.transport = udp ? Config::Transport::udp : Config::Transport::tcp;
    const std::string ep = udp ? udp_ep : (vm.count("tcp") ? tcp_ep : std::string("127.0.0.1:30500"));
    if (!parseEndpoint(ep, cfg.host, cfg.port)) {
        std::cerr << "skycue-dump: expected HOST:PORT, got '" << ep << "'\n";
        return 2;
    }
    const bool json = vm.count("json") > 0;

    Client client(cfg);
    unsigned long printed = 0;
    bool done = false;
    const auto emit = [&](const std::string& line) {
        if (done)
            return;
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
        if (++printed == count) {
            done = true;
            client.stop();
        }
    };
    client.onFrame([&](const Frame& f) {
        Hello h;
        Heartbeat hb;
        if (decodeHello(f, h))
            emit(Line(json, "hello", "HELLO")
                     .add("sender", jsonString(h.sender), textWord(h.sender))
                     .add("keyframe_interval_ms", num(static_cast<long long>(h.keyframe_interval_ms)),
                          "kf=" + std::to_string(h.keyframe_interval_ms) + "ms")
                     .add("heartbeat_interval_ms", num(static_cast<long long>(h.heartbeat_interval_ms)),
                          "hb=" + std::to_string(h.heartbeat_interval_ms) + "ms")
                     .str());
        else if (decodeHeartbeat(f, hb))
            emit(Line(json, "heartbeat", "HB")
                     .add("sender_time_ms", std::to_string(hb.sender_time_ms), "")
                     .str());
        // Updates and drops are printed from the track callbacks below.
    });
    client.onTrackUpdate([&](const Track& t) { emit(updateLine(json, t)); });
    client.onTrackDropped([&](TrackId id, DropReason r) {
        const std::string h = hex(id.address);
        emit(Line(json, "drop", "DROP")
                 .add("hex", jsonString(h), h)
                 .add("addr_type", jsonString(name(id.type)), "")
                 .add("reason", jsonString(name(r)), name(r))
                 .str());
    });
    // The first connect is implied by the HELLO that follows it; later
    // transitions (loss, reconnect) are shown.
    bool ever_connected = false;
    client.onConnectionChange([&](bool up) {
        if (up && !ever_connected) {
            ever_connected = true;
            return;
        }
        emit(Line(json, "conn", "CONN").add("connected", up ? "true" : "false", up ? "up" : "down").str());
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(timeout_s));
    bool timed_out = false;
    client.start();
    try {
        while (!done && !g_signalled) {
            client.poll();
            if (vm.count("timeout") && std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (const std::exception& e) {
        std::cerr << "skycue-dump: " << e.what() << "\n";
        return 1;
    }
    client.stop();
    return timed_out && printed == 0 ? 3 : 0;
}
