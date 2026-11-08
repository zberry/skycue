#define BOOST_TEST_MODULE skycued_capture
#include <boost/test/unit_test.hpp>


#include "asterix/capture_line.hpp"
#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "engine_test_support.hpp"
#include "inputs/capture_writer.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/ping_cat021_input.hpp"
#include "inputs/ping_json.hpp"
#include "inputs/ping_json_input.hpp"
#include "json_lite.hpp"
#include "raw_formats.hpp"
#include "retention.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cmath>

using namespace enginetest;
using asio::ip::udp;
using skycued::CaptureLine;
using skycued::InputDef;
using skycued::InputManager;
using skycued::InputManagerConfig;
using skycued::PingCat021Input;
using skycued::PingJsonInput;
using skycued::RawFrame;
using skycued::RawFrameWriter;
using skycued::RecordOptions;
namespace ax = skycued::asterix;

namespace {

using Bytes = std::vector<uint8_t>;

template <class F> bool eventually(F pred)
{
    const auto until = steady::now() + kWait;
    while (steady::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string fixture(const std::string& name)
{
    const std::string text = readAll(std::string(SKYCUE_FIXTURE_DIR) + "/pingstation/" + name);
    BOOST_REQUIRE_MESSAGE(!text.empty(), "cannot open fixture " << name);
    return text;
}

InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

InputDef psJson(const std::string& id, uint16_t port, const std::string& extra = "")
{
    return defOf(R"({"id":")" + id + R"(","type":"pingstation_json","bind":"127.0.0.1","port":)" +
                 std::to_string(port) + extra + "}");
}

InputDef cat021(const std::string& id, const std::string& transport, uint16_t port, const std::string& extra = "")
{
    return defOf(R"({"id":")" + id + R"(","type":"pingstation_cat021","transport":")" + transport +
                 R"(","bind":"127.0.0.1","port":)" + std::to_string(port) + extra + "}");
}

struct UdpSender {
    asio::io_context io;
    udp::socket sock{io, udp::endpoint(udp::v4(), 0)};
    void send(uint16_t port, const void* data, std::size_t size)
    {
        boost::system::error_code ignored;
        sock.send_to(asio::buffer(data, size), udp::endpoint(asio::ip::make_address("127.0.0.1"), port), 0, ignored);
    }
    void send(uint16_t port, const std::string& s) { send(port, s.data(), s.size()); }
    void send(uint16_t port, const Bytes& b) { send(port, b.data(), b.size()); }
};

tcp::socket connectWhenListening(asio::io_context& io, uint16_t port)
{
    const auto until = steady::now() + kWait;
    for (;;) {
        tcp::socket s(io);
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec)
            return s;
        if (steady::now() > until)
            throw std::runtime_error("cannot connect to port " + std::to_string(port));
        std::this_thread::sleep_for(10ms);
    }
}

Bytes block(uint32_t address)
{
    ax::Cat021Record r;
    r.sac = 0;
    r.sic = 0;
    r.trd = ax::Descriptor040{};
    r.address = address;
    r.lat131 = static_cast<int32_t>(std::lround(42.5 / ax::kLsb131Deg));
    r.lon131 = static_cast<int32_t>(std::lround(-93.25 / ax::kLsb131Deg));
    return ax::encodeBlock({r});
}

// One Input started on the io thread; tick() flushes its writers.
struct Probe {
    IoThread& t;
    std::unique_ptr<skycued::Input> in;
    Probe(IoThread& thread, std::unique_ptr<skycued::Input> input) : t(thread)
    {
        t.call([&] {
            in = std::move(input);
            in->start();
        });
    }
    ~Probe()
    {
        t.call([&] { in.reset(); });
    }
    skycued::InputState state()
    {
        return t.call([&] { return in->state(); });
    }
    void tick()
    {
        t.call([&] { in->tick(std::chrono::system_clock::now()); });
    }
};

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        out.push_back(l);
    return out;
}

// The lines of the single file in `dir` ending in `suffix` whose name starts with `prefix`.
std::vector<std::string> fileLines(const std::string& dir, const std::string& prefix, const std::string& suffix)
{
    std::vector<std::string> files;
    for (const auto& f : filesWithSuffix(dir, suffix))
        if (f.compare(dir.size() + 1, prefix.size(), prefix) == 0)
            files.push_back(f);
    BOOST_REQUIRE_EQUAL(files.size(), 1u);
    const std::string name = files[0].substr(dir.size() + 1);
    BOOST_TEST(skycued::isRecordingFileName(name), name);
    return lines(readAll(files[0]));
}

std::vector<std::string> dirEntries(const std::string& dir)
{
    std::vector<std::string> out;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d))
            if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                out.push_back(e->d_name);
        ::closedir(d);
    }
    return out;
}

struct StderrCapture {
    std::string path;
    int saved = -1;
    explicit StderrCapture(std::string p) : path(std::move(p))
    {
        std::fflush(stderr);
        saved = ::dup(2);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::dup2(fd, 2);
        ::close(fd);
    }
    std::string text()
    {
        if (saved >= 0) {
            std::fflush(stderr);
            ::dup2(saved, 2);
            ::close(saved);
            saved = -1;
        }
        return readAll(path);
    }
    ~StderrCapture() { text(); }
};

int64_t epochNs(std::chrono::system_clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------- capture lines

BOOST_AUTO_TEST_CASE(capture_line_round_trip)
{
    CaptureLine l;
    l.t = std::chrono::system_clock::time_point(std::chrono::milliseconds(1793614500123));
    l.input = "ps-north";
    l.transport = "tcp_listen";
    l.bytes = {0x15, 0x00, 0x0A, 0xAB, 0xFF};
    const std::string text = skycued::captureLineJson(l);
    BOOST_TEST(text ==
               R"({"t":"2026-11-02T10:15:00.123Z","input":"ps-north","transport":"tcp_listen","hex":"15000aabff"})");
    const auto back = skycued::parseCaptureLine(text);
    BOOST_REQUIRE(back);
    BOOST_TEST((back->t == l.t));
    BOOST_TEST(back->input == l.input);
    BOOST_TEST(back->transport == l.transport);
    BOOST_TEST((back->bytes == l.bytes));

    // Uppercase hex and a trailing newline read too; an empty datagram is a line.
    const auto upper = skycued::parseCaptureLine(
        R"({"t":"2026-11-02T10:15:00.123Z","input":"ps","transport":"udp","hex":"0AFF"})"
        "\n");
    BOOST_REQUIRE(upper);
    BOOST_TEST((upper->bytes == Bytes{0x0A, 0xFF}));
    l.bytes.clear();
    l.transport = "udp";
    const auto empty = skycued::parseCaptureLine(skycued::captureLineJson(l));
    BOOST_REQUIRE(empty);
    BOOST_TEST(empty->bytes.empty());
}

BOOST_AUTO_TEST_CASE(capture_line_rejects_bad_hex_and_missing_keys)
{
    const std::string t = R"("t":"2026-11-02T10:15:00.123Z")", in = R"("input":"ps")",
                      tr = R"("transport":"udp")", hex = R"("hex":"15ab")";
    BOOST_REQUIRE(skycued::parseCaptureLine("{" + t + "," + in + "," + tr + "," + hex + "}"));
    const std::vector<std::string> bads{
             "{" + t + "," + in + "," + tr + R"(,"hex":"15a"})",           // odd length
             "{" + t + "," + in + "," + tr + R"(,"hex":"15zz"})",          // not hex
             "{" + t + "," + in + "," + tr + R"(,"hex":"15 a"})",          // a space
             "{" + in + "," + tr + "," + hex + "}",                        // no t
             "{" + t + "," + tr + "," + hex + "}",                         // no input
             "{" + t + "," + in + "," + hex + "}",                         // no transport
             "{" + t + "," + in + "," + tr + "}",                          // no hex
             R"({"t":"yesterday",)" + in + "," + tr + "," + hex + "}",     // bad time
             "{" + t + "," + in + R"(,"transport":"serial",)" + hex + "}",  // unknown transport
             "{" + t + "," + in + "," + tr + R"(,"hex":{"a":"15"}})",      // not a string
             "{" + t + R"(,"input":"",)" + tr + "," + hex + "}",           // empty input
             "[1,2]",
             "not json",
             "",
    };
    for (const auto& bad : bads)
        BOOST_TEST(!skycued::parseCaptureLine(bad), bad);
}

// Found by fuzz_recording (mode 1): a time outside system_clock's int64
// nanosecond range (1677 to 2262) overflowed in parseIso8601Ms (undefined
// behaviour); such a line is now malformed.
BOOST_AUTO_TEST_CASE(fuzz_recording_capture_time_out_of_range)
{
    BOOST_TEST(!skycued::parseCaptureLine(
        R"({"t":"1013-11-02T10:15:00.123Z","input":"ps","transport":"udp","hex":"15ab"})"));
    BOOST_TEST(!skycued::parseCaptureLine(
        R"({"t":"9999-11-02T10:15:00.123Z","input":"ps","transport":"udp","hex":"15ab"})"));
}

// Found by fuzz_recording (final rerun of the sweep): a capture line whose
// "input" held invalid UTF-8 (an overlong C0 A3, which PropertyTree lets
// through) parsed, but captureLineJson writes such bytes as U+FFFD, so the
// line did not come back from its own JSON. Such an input is refused.
BOOST_AUTO_TEST_CASE(fuzz_recording_capture_input_invalid_utf8)
{
    const std::string head = R"({"t":"2026-11-02T10:15:00.123Z","input":")";
    const std::string tail = R"(","transport":"udp","hex":"15ab"})";
    for (const std::string& bad : {std::string("ps\xC0\xA3"), std::string("ps\xC0\xAF"), std::string("\xED\xA0\x80"),
                                  std::string("ps\x80"), std::string("\xF4\x90\x80\x80")}) {
        const auto c = skycued::parseCaptureLine(head + bad + tail);
        BOOST_TEST(!c.has_value());
    }
    // Valid UTF-8 beyond ASCII still parses and round-trips.
    const auto ok = skycued::parseCaptureLine(head + "ps-\xC3\xA9" + tail);
    BOOST_REQUIRE(ok.has_value());
    const auto back = skycued::parseCaptureLine(skycued::captureLineJson(*ok));
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST(back->input == ok->input);
}

BOOST_AUTO_TEST_CASE(capture_line_deep_nesting_refused_without_parsing)
{
    // Deep enough to overflow the stack in read_json if it got there.
    const std::size_t depth = 200000;
    const std::string line = R"({"t":"2026-11-02T10:15:00.123Z","input":"ps","transport":"udp","hex":"15ab","x":)" +
                             std::string(depth, '[') + std::string(depth, ']') + "}";
    BOOST_TEST(!skycued::parseCaptureLine(line));
    // Brackets inside a string do not count.
    BOOST_TEST(!skycued::parseCaptureLine(R"({"t":"2026-11-02T10:15:00.123Z","input":"ps","transport":"udp","hex":")" +
                                          std::string(depth, '[') + R"("})"));  // refused for its hex, not a crash
}

// ---------------------------------------------------------------- capture from inputs

BOOST_AUTO_TEST_CASE(capture_input_writes_datagrams)
{
    TempDir tmp;
    IoThread t;
    const RecordOptions rec{tmp.path, false, 0};
    const uint16_t port = freeUdpPort();
    Probe p(t, std::make_unique<PingCat021Input>(t.io, cat021("cu", "udp", port, R"(,"capture":true)"), rec));
    BOOST_REQUIRE(eventually([&] { return p.state().connected; }));
    const std::vector<Bytes> sent{block(0xABC001), block(0xABC002), {0x30, 0x00, 0x04, 0x99}};
    UdpSender tx;
    const auto before = std::chrono::system_clock::now();
    for (const auto& d : sent)
        tx.send(port, d);
    BOOST_REQUIRE(eventually([&] {
        const auto c = p.state().counters;
        return c.messages == 2 && c.rejected == 1;
    }));
    p.tick();
    const auto after = std::chrono::system_clock::now();

    const auto got = fileLines(tmp.path, "capture-cu-", ".jsonl");
    BOOST_REQUIRE_EQUAL(got.size(), sent.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        const auto l = skycued::parseCaptureLine(got[i]);
        BOOST_REQUIRE_MESSAGE(l, got[i]);
        BOOST_TEST(l->input == "cu");
        BOOST_TEST(l->transport == "udp");
        BOOST_TEST((l->bytes == sent[i]));
        BOOST_TEST((l->t >= before - 1ms && l->t <= after + 1ms));
    }

    // A JSON datagram is captured the same way: the hex of its text.
    const uint16_t jport = freeUdpPort();
    Probe j(t, std::make_unique<PingJsonInput>(t.io, psJson("pj", jport, R"(,"capture":true)"), rec));
    BOOST_REQUIRE(eventually([&] { return j.state().connected; }));
    const std::string text = fixture("traffic-1090.json");
    tx.send(jport, text);
    BOOST_REQUIRE(eventually([&] { return j.state().counters.messages == 1; }));
    j.tick();
    const auto jl = fileLines(tmp.path, "capture-pj-", ".jsonl");
    BOOST_REQUIRE_EQUAL(jl.size(), 1u);
    const auto l = skycued::parseCaptureLine(jl[0]);
    BOOST_REQUIRE(l);
    BOOST_TEST(std::string(l->bytes.begin(), l->bytes.end()) == text);
    // Without record_raw no raw file is written.
    BOOST_TEST(filesWithSuffix(tmp.path, ".csv").empty());
}

BOOST_AUTO_TEST_CASE(capture_tcp_writes_framed_blocks_only)
{
    TempDir tmp;
    IoThread t;
    asio::io_context cio;
    const uint16_t port = freeTcpPort();
    Probe p(t, std::make_unique<PingCat021Input>(t.io, cat021("cl", "tcp_listen", port, R"(,"capture":true)"),
                                                 RecordOptions{tmp.path, false, 0}));
    tcp::socket c = connectWhenListening(cio, port);
    const Bytes b1 = block(0xABC001), b2 = block(0xABC002);
    Bytes stream{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // garbage before the first block
    stream.insert(stream.end(), b1.begin(), b1.end());
    stream.insert(stream.end(), b2.begin(), b2.end());
    asio::write(c, asio::buffer(stream));
    BOOST_REQUIRE(eventually([&] { return p.state().counters.messages == 2; }));
    p.tick();

    const auto got = fileLines(tmp.path, "capture-cl-", ".jsonl");
    BOOST_REQUIRE_EQUAL(got.size(), 2u);
    const auto l1 = skycued::parseCaptureLine(got[0]), l2 = skycued::parseCaptureLine(got[1]);
    BOOST_REQUIRE(l1);
    BOOST_REQUIRE(l2);
    BOOST_TEST(l1->transport == "tcp_listen");
    BOOST_TEST((l1->bytes == b1));
    BOOST_TEST((l2->bytes == b2));
}

// ---------------------------------------------------------------- RECORD_RAW

BOOST_AUTO_TEST_CASE(raw_frames_written_in_rawplay_formats)
{
    TempDir tmp;
    {
        // The generated traffic example with an arrival inside the clock check: the GPS counter time is used.
        const auto arrival = std::chrono::system_clock::time_point(std::chrono::seconds(1793620800));
        const auto r = skycued::parsePingDatagram(fixture("traffic-1090.json"),
                                                  skycued::PingContext{arrival, skycued::TimeMode::utc});
        BOOST_REQUIRE_EQUAL(r.raw_frames.size(), 1u);
        RawFrameWriter w(tmp.path, "ps-north", 0);
        w.write(r.raw_frames[0]);
        // A 7-byte short frame is 1090 too.
        w.write(RawFrame{RawFrame::Kind::adsb_1090, "5dac5e81a1b2c3", arrival + 5ms});
        w.flush();
    }
    const auto csv = fileLines(tmp.path, "raw-ps-north-", ".csv");
    BOOST_REQUIRE_EQUAL(csv.size(), 2u);
    BOOST_TEST(csv[0] == "1793620798.412907153,8DAC5E815821C2A65E8F1B30D47A");
    const auto m = skycued::parseCsvMessage(csv[0]);
    BOOST_REQUIRE(m);
    BOOST_TEST(m->hex == "8DAC5E815821C2A65E8F1B30D47A");
    BOOST_TEST(m->epoch_ns == int64_t{1793620798412907153});
    BOOST_TEST(csv[1] == "1793620800.005000000,5DAC5E81A1B2C3");
    BOOST_TEST(skycued::parseCsvMessage(csv[1]).has_value());

    // A UAT 18-byte frame through a JSON input with record_raw: no utcSync, so arrival.
    IoThread t;
    const uint16_t port = freeUdpPort();
    Probe p(t, std::make_unique<PingJsonInput>(t.io, psJson("ps-uat", port), RecordOptions{tmp.path, true, 0}));
    BOOST_REQUIRE(eventually([&] { return p.state().connected; }));
    UdpSender tx;
    const auto before = std::chrono::system_clock::now();
    tx.send(port, fixture("uat-detail.json"));
    // rawData of 31 bytes (a UAT length): counted, not written.
    tx.send(port, R"({"icaoAddress":"A1B2C4","detail":{"rawData":"00a1b2c31a2b3c4d5e6f708192a3b4c5d6e7a1b2c3d4e5f6a7b8c9d0e1f2a3"}})");
    BOOST_REQUIRE(eventually([&] { return p.state().counters.messages == 2; }));
    p.tick();
    const auto after = std::chrono::system_clock::now();
    BOOST_TEST(p.state().counters.raw_unknown_len == 1u);
    const auto txt = fileLines(tmp.path, "raw-ps-uat-", ".txt");
    BOOST_REQUIRE_EQUAL(txt.size(), 1u);
    const auto u = skycued::parseUatLine(txt[0]);
    BOOST_REQUIRE_MESSAGE(u, txt[0]);
    BOOST_REQUIRE(u->epoch_ms);
    BOOST_TEST(*u->epoch_ms >= epochNs(before) / 1000000);
    BOOST_TEST(*u->epoch_ms <= epochNs(after) / 1000000);
    BOOST_TEST(u->text == "-00a1b2c31a2b3c4d5e6f708192a3b4c5d6e7;");
    for (const auto& f : filesWithSuffix(tmp.path, ".csv"))
        BOOST_TEST(!contains(f, "raw-ps-uat-"), f);  // no 1090 frame from this input
}

BOOST_AUTO_TEST_CASE(record_raw_without_record_dir_warns_once)
{
    TempDir tmp, logdir;
    char cwd[4096];
    BOOST_REQUIRE(::getcwd(cwd, sizeof cwd));
    BOOST_REQUIRE(::chdir(tmp.path.c_str()) == 0);  // a relative write would land here
    std::string log;
    {
        StderrCapture cap(logdir.path + "/stderr.txt");
        IoThread t;
        InputManagerConfig cfg;
        cfg.record_raw = true;
        cfg.poll_interval = 50ms;
        std::unique_ptr<InputManager> m;
        std::atomic<int> obs{0};
        const uint16_t p1 = freeUdpPort(), p2 = freeUdpPort();
        t.call([&] {
            m = std::make_unique<InputManager>(t.io, cfg);
            m->onObservation([&](const TrackObservation&) { ++obs; });
            m->load();
            m->start();
            m->create(psJson("ps-a", p1));
            m->create(psJson("ps-b", p2));
        });
        BOOST_REQUIRE(eventually([&] {
            return t.call([&] { return m->find("ps-a")->state.connected && m->find("ps-b")->state.connected; });
        }));
        UdpSender tx;
        tx.send(p1, fixture("uat-detail.json"));
        tx.send(p2, fixture("uat-detail.json"));
        BOOST_REQUIRE(eventually([&] { return obs == 2; }));
        std::this_thread::sleep_for(150ms);  // a few ticks
        t.call([&] { m.reset(); });
        log = cap.text();
    }
    BOOST_REQUIRE(::chdir(cwd) == 0);
    std::size_t warns = 0;
    for (const auto& l : lines(log))
        if (contains(l, "RECORD_RAW"))
            ++warns;
    BOOST_TEST(warns == 1u, log);
    BOOST_TEST(dirEntries(tmp.path).empty());
}

BOOST_AUTO_TEST_CASE(capture_write_failures_counted)
{
    TempDir tmp, logdir;
    // A record_dir under a regular file: no capture or raw file can be created.
    const std::string blocker = tmp.path + "/not-a-dir";
    { std::ofstream(blocker) << "x"; }
    const RecordOptions rec{blocker + "/rec", true, 0};
    std::string log;
    {
        StderrCapture cap(logdir.path + "/stderr.txt");
        IoThread t;
        UdpSender tx;
        const uint16_t cport = freeUdpPort(), jport = freeUdpPort();
        Probe c(t, std::make_unique<PingCat021Input>(t.io, cat021("cf", "udp", cport, R"(,"capture":true)"), rec));
        Probe j(t, std::make_unique<PingJsonInput>(t.io, psJson("jf", jport, R"(,"capture":true)"), rec));
        BOOST_REQUIRE(eventually([&] { return c.state().connected && j.state().connected; }));
        tx.send(cport, block(0xABC001));
        tx.send(cport, block(0xABC002));
        tx.send(jport, fixture("uat-detail.json"));  // one capture line and one raw frame
        BOOST_REQUIRE(eventually([&] { return c.state().counters.messages == 2 && j.state().counters.messages == 1; }));
        BOOST_TEST(c.state().counters.capture_dropped == 2u);
        BOOST_TEST(j.state().counters.capture_dropped == 2u);
        log = cap.text();
    }
    BOOST_TEST(contains(log, "cannot create"), log);
}

BOOST_AUTO_TEST_CASE(capture_dropped_added)
{
    skycued::InputCounters a, b;
    a.capture_dropped = 1;
    b.capture_dropped = 2;
    a += b;
    BOOST_TEST(a.capture_dropped == 3u);
}

// captureLineJson writes through the shared escaper, so its text is valid
// UTF-8 as well (json_writer_output_is_valid_utf8 in test_readsb_json.cpp).
BOOST_AUTO_TEST_CASE(capture_line_json_is_valid_utf8)
{
    const auto w = [](const std::string& s) {
        CaptureLine l;
        l.t = std::chrono::system_clock::time_point(std::chrono::milliseconds(1793614500123));
        l.input = s;
        l.transport = "udp";
        const std::string text = skycued::captureLineJson(l);
        const auto a = text.find("\"input\":");
        const auto b = text.find(",\"transport\"");
        return "{\"k\":" + text.substr(a + 8, b - a - 8) + "}";
    };
    BOOST_TEST(w("a\xFF" "b") == "{\"k\":\"a\xEF\xBF\xBD" "b\"}");
    BOOST_TEST(w("\xC0\xAF") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\"}");                  // overlong
    BOOST_TEST(w("\xED\xA0\x80") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"}");  // surrogate
    BOOST_TEST(w("\xC3\xA9t\xC3\xA9") == "{\"k\":\"\xC3\xA9t\xC3\xA9\"}");                // valid kept
    BOOST_TEST(w("\xE2\x82") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\"}");                  // truncated
}
