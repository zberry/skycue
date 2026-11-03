// skycue-dump: connects to a skycue stream (TCP or UDP) and prints one line
// per frame, as text or as JSON. For the stream it is a pure libskycue
// consumer: everything it shows comes through skycue::Client. --capture
// instead prints a pingStation capture file, decoding CAT021 with
// skycued_asterix (the only part of skycued it links).
//
// Frame lines (HELLO, UPDATE, DROP, HB, BAD) show exactly what arrived on the
// wire: they are decoded here from Client::onFrame, never from the track
// table. Two kinds of event lines are not frames and do not count towards
// --count: CONN (connection up/down) and LOCAL-DROP (a drop the client made up
// itself: stale, or source_lost after the connection went down).

#include <skycue/client.hpp>
#include <skycue/protocol.hpp>
#include <skycue/types.hpp>

#include "asterix/capture_line.hpp"
#include "asterix/cat021.hpp"
#include "time_util.hpp"

#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
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

const char* frameTypeName(FrameType t)
{
    switch (t) {
    case FrameType::hello: return "hello";
    case FrameType::track_update: return "update";
    case FrameType::track_drop: return "drop";
    case FrameType::heartbeat: return "heartbeat";
    }
    return nullptr;
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
    // Event lines (CONN, LOCAL-DROP) say so in JSON; frame lines carry no flag.
    Line& uncounted() { return add("counted", "false", ""); }
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

// "  CAT021 sac=0 sic=0 addr=A6452D lat=39.93328 lon=-88.38120 fl=390.25
// squawk=2001 ident=ATN3597 gnd=-": the fields present, in this order; gnd
// (Y or -) only when 040 has its first extension.
std::string cat021Line(const skycued::asterix::Cat021Record& r)
{
    using namespace skycued::asterix;
    std::string s = "  CAT021";
    if (r.sac)
        s += " sac=" + std::to_string(*r.sac);
    if (r.sic)
        s += " sic=" + std::to_string(*r.sic);
    if (r.address)
        s += " addr=" + hex(*r.address);
    if (r.lat131 && r.lon131)
        s += " lat=" + num(*r.lat131 * kLsb131Deg, "%.5f") + " lon=" + num(*r.lon131 * kLsb131Deg, "%.5f");
    else if (r.lat130 && r.lon130)
        s += " lat=" + num(*r.lat130 * kLsb130Deg, "%.5f") + " lon=" + num(*r.lon130 * kLsb130Deg, "%.5f");
    if (r.fl145)
        s += " fl=" + num(*r.fl145 * kFt145 / 100.0, "%.2f");
    if (r.mode3a) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04o", static_cast<unsigned>(*r.mode3a & 07777));
        s += " squawk=" + std::string(buf);
    }
    if (r.ident) {
        const auto first = r.ident->find_first_not_of(' ');
        if (first != std::string::npos)
            s += " ident=" + textWord(r.ident->substr(first, r.ident->find_last_not_of(' ') - first + 1));
    }
    if (r.trd && r.trd->has_ext1)
        s += r.trd->gbs ? " gnd=Y" : " gnd=-";
    return s + "\n";
}

// --capture: one "<t> <input> <transport> <n> bytes" line per record, then
// its text (a JSON datagram) or one line per CAT021 record. Lines that are
// not capture records are reported on stderr and skipped.
int dumpCapture(const std::string& path, skycued::asterix::Edition edition)
{
    using namespace skycued::asterix;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "skycue-dump: cannot open " << path << "\n";
        return 1;
    }
    unsigned long n = 0;
    std::string out;
    for (std::string text; std::getline(in, text);) {
        ++n;
        if (text.empty() || text == "\r")
            continue;
        const auto line = skycued::parseCaptureLine(text);
        if (!line) {
            std::cerr << "skycue-dump: " << path << ":" << n << ": not a capture record\n";
            continue;
        }
        const auto& b = line->bytes;
        out = skycued::formatIso8601Ms(line->t) + " " + textWord(line->input) + " " + line->transport + " " +
              std::to_string(b.size()) + " bytes\n";
        if (!b.empty() && b[0] == '{') {
            std::string body(b.begin(), b.end());
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
                body.pop_back();
            out += "  " + body + "\n";
        } else {
            const BlockSplit split = splitBlocks(b.data(), b.size());
            for (const auto& blk : split.blocks) {
                if (blk.cat != 21) {
                    out += "  BAD category " + std::to_string(blk.cat) + " block\n";
                    continue;
                }
                const DecodedBlock d = decodeBlock(b.data() + blk.offset, blk.size, edition);
                for (const auto& r : d.records)
                    out += cat021Line(r);
                if (d.error)
                    out += "  BAD " + d.error_text + "\n";
            }
            if (split.truncated)
                out += "  BAD truncated block\n";
        }
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string tcp_ep, udp_ep, listen_ep, capture_path, edition_text = "2.5";
    unsigned long count = 0;
    double timeout_s = 0;
    po::options_description desc("usage: skycue-dump [--tcp HOST:PORT | --tcp-listen BIND:PORT | --udp BIND:PORT]\n"
                                 "                   [--json] [--count N] [--timeout S]\n"
                                 "       skycue-dump --capture FILE [--edition 2.5]\n"
                                 "Prints a skycue stream one line per wire frame: HELLO, UPDATE, DROP, HB, or\n"
                                 "BAD <type> <len> for a frame that does not decode. Event lines CONN up|down and\n"
                                 "LOCAL-DROP <HEX> <reason> (a drop the client made up: stale, source_lost) are\n"
                                 "printed too but are not frames and do not count towards --count. --capture\n"
                                 "prints a pingStation capture file one record at a time, CAT021 decoded");
    desc.add_options()("help,h", "show this help")(
        "tcp", po::value(&tcp_ep), "connect to a skycued TCP port (default 127.0.0.1:30500)")(
        "tcp-listen", po::value(&listen_ep), "listen on a TCP port for skycued to connect (CUE_TCP_CONNECT); "
                                             "any number of connections")(
        "udp", po::value(&udp_ep), "bind a UDP port and print the datagrams sent to it")(
        "json", "one JSON object per line instead of text")(
        "count", po::value(&count), "exit 0 after N frame lines (heartbeats and BAD count; CONN and LOCAL-DROP do not)")(
        "timeout", po::value(&timeout_s), "exit after S seconds (at most 1e6): 0 if any frame arrived, else 3")(
        "capture", po::value(&capture_path), "print the capture file FILE (capture-<id>-*.jsonl) and exit")(
        "edition", po::value(&edition_text), "with --capture: the CAT021 edition, 2.2-2.7 (default 2.5)");
    // Numbers are printed with snprintf, whose decimal point follows
    // LC_NUMERIC; pin it so the JSON stays valid whatever setlocale callers do.
    std::setlocale(LC_NUMERIC, "C");
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
    if (vm.count("capture")) {
        if (vm.count("tcp") + vm.count("tcp-listen") + vm.count("udp") + vm.count("json") + vm.count("count") +
                vm.count("timeout") >
            0) {
            std::cerr << "skycue-dump: --capture excludes --tcp, --tcp-listen, --udp, --json, --count and --timeout\n";
            return 2;
        }
        const auto edition = skycued::asterix::parseEdition(edition_text);
        if (!edition) {
            std::cerr << "skycue-dump: --edition must be 2.2-2.7, got '" << edition_text << "'\n";
            return 2;
        }
        return dumpCapture(capture_path, *edition);
    }
    if (vm.count("edition")) {
        std::cerr << "skycue-dump: --edition needs --capture\n";
        return 2;
    }
    if (vm.count("tcp") + vm.count("tcp-listen") + vm.count("udp") > 1) {
        std::cerr << "skycue-dump: --tcp, --tcp-listen and --udp are mutually exclusive\n";
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
    timeout_s = std::min(timeout_s, 1e6); // keeps the duration_cast below in range

    Config cfg;
    cfg.run_io_thread = false; // everything, printing included, on this thread
    cfg.reconnect_interval = std::chrono::milliseconds(500);
    std::string ep = vm.count("tcp") ? tcp_ep : std::string("127.0.0.1:30500");
    cfg.transport = Config::Transport::tcp;
    if (vm.count("udp")) {
        cfg.transport = Config::Transport::udp;
        ep = udp_ep;
    } else if (vm.count("tcp-listen")) {
        cfg.transport = Config::Transport::tcp_listen;
        ep = listen_ep;
    }
    if (!parseEndpoint(ep, cfg.host, cfg.port)) {
        std::cerr << "skycue-dump: expected HOST:PORT, got '" << ep << "'\n";
        return 2;
    }
    const bool json = vm.count("json") > 0;

    Client client(cfg);
    unsigned long frames = 0; // frame lines printed: what --count and exit 3 look at
    bool done = false;
    const auto print = [&](const std::string& line) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    };
    const auto frameLine = [&](const std::string& line) {
        if (done)
            return;
        print(line);
        if (++frames == count) {
            done = true;
            client.stop();
        }
    };
    const auto eventLine = [&](const std::string& line) {
        if (!done)
            print(line);
    };
    // A TrackDrop frame for a track in the client's table makes the client
    // call onTrackDropped right after onFrame; that call is the wire drop
    // already printed, not a LOCAL-DROP. Any other frame clears it.
    std::optional<TrackId> wire_drop;
    client.onFrame([&](const Frame& f) {
        wire_drop.reset();
        const char* type = frameTypeName(f.type);
        Hello h;
        Heartbeat hb;
        Track t;
        TrackDrop d;
        if (f.type == FrameType::hello && decodeHello(f, h)) {
            frameLine(Line(json, "hello", "HELLO")
                          .add("sender", jsonString(h.sender), textWord(h.sender))
                          .add("keyframe_interval_ms", num(static_cast<long long>(h.keyframe_interval_ms)),
                               "kf=" + std::to_string(h.keyframe_interval_ms) + "ms")
                          .add("heartbeat_interval_ms", num(static_cast<long long>(h.heartbeat_interval_ms)),
                               "hb=" + std::to_string(h.heartbeat_interval_ms) + "ms")
                          .str());
        } else if (f.type == FrameType::track_update && decodeTrackUpdate(f, t)) {
            frameLine(updateLine(json, t));
        } else if (f.type == FrameType::track_drop && decodeTrackDrop(f, d)) {
            wire_drop = d.id;
            const std::string x = hex(d.id.address);
            frameLine(Line(json, "drop", "DROP")
                          .add("hex", jsonString(x), x)
                          .add("addr_type", jsonString(name(d.id.type)), "")
                          .add("reason", jsonString(name(d.reason)), name(d.reason))
                          .str());
        } else if (f.type == FrameType::heartbeat && decodeHeartbeat(f, hb)) {
            frameLine(Line(json, "heartbeat", "HB").add("sender_time_ms", std::to_string(hb.sender_time_ms), "").str());
        } else { // malformed payload, or a frame type this version does not know
            const std::string tn = type ? type : std::to_string(static_cast<unsigned>(f.type));
            const std::string len = std::to_string(f.payload.size());
            frameLine(Line(json, "bad", "BAD").add("type", jsonString(tn), tn).add("length", len, len).str());
        }
    });
    client.onTrackDropped([&](TrackId id, DropReason r) {
        if (wire_drop && *wire_drop == id) {
            wire_drop.reset();
            return;
        }
        const std::string x = hex(id.address);
        eventLine(Line(json, "local_drop", "LOCAL-DROP")
                      .add("hex", jsonString(x), x)
                      .add("addr_type", jsonString(name(id.type)), "")
                      .add("reason", jsonString(name(r)), name(r))
                      .uncounted()
                      .str());
    });
    client.onConnectionChange([&](bool up) {
        wire_drop.reset();
        eventLine(Line(json, "conn", "CONN").add("connected", up ? "true" : "false", up ? "up" : "down").uncounted().str());
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(timeout_s));
    bool timed_out = false;
    client.start();
    if (cfg.transport == Config::Transport::tcp_listen && client.localPort() == 0) {
        std::cerr << "skycue-dump: cannot listen on " << ep << "\n";
        return 1;
    }
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
    return timed_out && frames == 0 ? 3 : 0;
}
