#define BOOST_TEST_MODULE skycued_beast_raw
#include <boost/test/unit_test.hpp>

#include "decode_test_support.hpp"
#include "engine_test_support.hpp"
#include "hub/raw_tap.hpp"
#include "inputs/beast_raw_input.hpp"
#include "inputs/capture_writer.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/uat_raw_input.hpp"
#include "json_lite.hpp"
#include "raw_formats.hpp"
#include "restricted_test_data.hpp"
#include "retention.hpp"
#include "test_util.hpp"
#include "web_inputs.hpp"

using namespace enginetest;
using asio::ip::udp;
using skycued::BeastRawInput;
using skycued::RawKind;
using skycued::RawMessage;
using skycued::RawTap;
using skycued::RecordOptions;
using skycued::UatRawInput;
using skycued::RawDecodeOptions;
namespace st = skycued::testing;

namespace {

InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

std::string beastDef(const std::string& id, const std::string& transport, uint16_t port, const std::string& extra = "")
{
    return R"({"id":")" + id + R"(","type":"beast_raw","transport":")" + transport +
           R"(","bind":"127.0.0.1","port":)" + std::to_string(port) + extra + "}";
}

// Hand-built frames: 0x1a, type, 6 timestamp bytes, signal 0, payload, with
// every 0x1a after the type doubled.
std::string wire(char type, const std::string& ts6, const std::string& payload)
{
    std::string body = ts6 + std::string(1, '\0') + payload, out = std::string("\x1a") + type;
    for (const char c : body) {
        out += c;
        if (c == '\x1a')
            out += c;
    }
    return out;
}

const std::string kTs1("\x00\x00\x00\x00\x00\x01", 6);
const std::string kTs2("\x00\x00\x00\x00\x00\x02", 6);
const std::string kTsEsc("\x00\x00\x00\x00\x1a\x03", 6);
const std::string kShortPayload("\x5d\x48\x40\xd6\x20\x2c\xc3", 7);
const std::string kLongPayload("\x8d\x48\x40\xd6\x20\x2c\xc3\x71\xc3\x2c\xe0\x57\x60\x98", 14);
const std::string kEscPayload("\x8d\x1a\x40\xd6\x20\x2c\xc3\x71\xc3\x2c\xe0\x1a\x60\x98", 14);
const std::string kShortFrame = wire('2', kTs1, kShortPayload);
const std::string kLongFrame = wire('3', kTs2, kLongPayload);
const std::string kEscFrame = wire('3', kTsEsc, kEscPayload);
const std::string kReceiverId = std::string("\x1a\xe3") + "\x11\x22\x33\x44\x55\x66\x77\x88";

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

struct Published {
    RawKind kind;
    std::string bytes, input;
};

// One input on its own io thread with a recording tap.
struct Rig {
    IoThread t;
    RawTap tap;
    std::mutex m;
    std::vector<Published> got;
    std::unique_ptr<skycued::Input> in;

    Rig()
    {
        t.call([&] {
            tap.setSink(
                [this](const RawMessage& msg) {
                    std::lock_guard<std::mutex> lock(m);
                    got.push_back({msg.kind, std::string(msg.bytes->begin(), msg.bytes->end()), std::string(msg.input)});
                },
                [](RawKind) { return true; });
        });
    }
    void run(std::unique_ptr<skycued::Input> input)
    {
        t.call([&] {
            in = std::move(input);
            in->setRawTap(&tap);
            in->start();
        });
    }
    ~Rig()
    {
        try {
            t.call([&] { in.reset(); });
        } catch (const std::exception&) {
        }
    }
    skycued::InputState state() { return t.call([&] { return in->state(); }); }
    void tick() { t.call([&] { in->tick(std::chrono::system_clock::now()); }); }
    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(m);
        return got.size();
    }
    Published at(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(m);
        return got.at(i);
    }
    std::string all()
    {
        std::lock_guard<std::mutex> lock(m);
        std::string s;
        for (const auto& p : got)
            s += p.bytes;
        return s;
    }
    bool connected() { return state().connected; }
};

void sendUdp(uint16_t port, const std::string& s, const char* from = "127.0.0.1")
{
    // One socket per source address for the whole run, so a sender keeps its port between calls.
    static asio::io_context io;
    static std::map<std::string, udp::socket> socks;
    auto it = socks.find(from);
    if (it == socks.end())
        it = socks.emplace(from, udp::socket(io, udp::endpoint(asio::ip::make_address(from), 0))).first;
    it->second.send_to(asio::buffer(s), udp::endpoint(asio::ip::make_address("127.0.0.1"), port));
}

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

void writeAll(tcp::socket& s, const std::string& text) { asio::write(s, asio::buffer(text)); }

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        out.push_back(l);
    return out;
}

// The single file in `dir` named raw-<id>-*.<ext>.
std::string captureFile(const std::string& dir, const std::string& id, const std::string& ext)
{
    std::vector<std::string> files;
    for (const auto& f : filesWithSuffix(dir, "." + ext))
        if (f.compare(dir.size() + 1, id.size() + 5, "raw-" + id + "-") == 0)
            files.push_back(f);
    BOOST_REQUIRE_EQUAL(files.size(), 1u);
    BOOST_TEST(skycued::isRecordingFileName(files[0].substr(dir.size() + 1)), files[0]);
    return files[0];
}

}  // namespace

BOOST_AUTO_TEST_CASE(beast_tcp_stream_frames_published_exactly_as_received)
{
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bt", "tcp_listen", port))));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    const std::string stream = kShortFrame + kReceiverId + kEscFrame + kLongFrame;
    writeAll(c, stream.substr(0, 11));  // frames split across writes, one inside an escape pair
    std::this_thread::sleep_for(30ms);
    writeAll(c, stream.substr(11, 20));
    std::this_thread::sleep_for(30ms);
    writeAll(c, stream.substr(31));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 4; }));
    BOOST_TEST(rig.at(0).bytes == kShortFrame);
    BOOST_TEST(rig.at(1).bytes == kReceiverId);  // 0xe3 passes as received
    BOOST_TEST(rig.at(2).bytes == kEscFrame);    // escaped bytes stay escaped
    BOOST_TEST(rig.at(3).bytes == kLongFrame);
    BOOST_TEST((rig.at(2).kind == RawKind::beast));
    BOOST_TEST(rig.at(2).input == "bt");
    const auto s = rig.state();
    BOOST_TEST(s.counters.messages == 4u);
    BOOST_TEST(s.counters.bytes == stream.size());
    BOOST_TEST(s.counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(beast_udp_two_frames_in_one_datagram_published_once_each)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bu", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, kShortFrame + kLongFrame);
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    BOOST_TEST(rig.at(0).bytes == kShortFrame);
    BOOST_TEST(rig.at(1).bytes == kLongFrame);
    BOOST_TEST(rig.state().counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(beast_udp_partial_frame_completes_with_next_datagram)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bu", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, kShortFrame + kLongFrame.substr(0, 9));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    sendUdp(port, kLongFrame.substr(9));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    BOOST_TEST(rig.at(1).bytes == kLongFrame);
    BOOST_TEST(rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(beast_udp_truncated_tail_resyncs)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bu", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    // A cut frame, then a datagram that starts a new one with 1a 33.
    sendUdp(port, kShortFrame + kLongFrame.substr(0, 12));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    BOOST_TEST(rig.state().counters.rejected == 0u);  // nothing skipped yet
    const std::string next = wire('3', kTs1, kLongPayload);
    BOOST_REQUIRE(next.compare(0, 2, "\x1a" "3") == 0);
    sendUdp(port, next);
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    BOOST_TEST(rig.at(1).bytes == next);  // whole, nothing of the cut frame in it
    BOOST_TEST(rig.state().counters.rejected == 1u);
    BOOST_TEST(rig.state().counters.messages == 2u);
}

BOOST_AUTO_TEST_CASE(beast_udp_two_senders_do_not_splice)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bu", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    try {  // 127.0.0.2 is a second source address on a Linux loopback
        asio::io_context io;
        udp::socket probe(io, udp::endpoint(asio::ip::make_address("127.0.0.2"), 0));
    } catch (const std::exception&) {
        BOOST_TEST_MESSAGE("SKIP: cannot bind 127.0.0.2");
        return;
    }
    const auto half = [](const std::string& f) { return f.size() / 2; };
    sendUdp(port, kLongFrame.substr(0, half(kLongFrame)), "127.0.0.1");
    sendUdp(port, kEscFrame.substr(0, half(kEscFrame)), "127.0.0.2");
    std::this_thread::sleep_for(30ms);
    sendUdp(port, kLongFrame.substr(half(kLongFrame)), "127.0.0.1");
    sendUdp(port, kEscFrame.substr(half(kEscFrame)), "127.0.0.2");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    const std::set<std::string> got{rig.at(0).bytes, rig.at(1).bytes};
    BOOST_TEST((got == std::set<std::string>{kLongFrame, kEscFrame}));
    BOOST_TEST(rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(beast_udp_two_senders_same_host_do_not_splice)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bu", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    asio::io_context io;
    udp::socket a(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    udp::socket b(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const udp::endpoint to(asio::ip::make_address("127.0.0.1"), port);
    const auto half = [](const std::string& f) { return f.size() / 2; };
    a.send_to(asio::buffer(kLongFrame.substr(0, half(kLongFrame))), to);
    std::this_thread::sleep_for(30ms);
    b.send_to(asio::buffer(kEscFrame.substr(0, half(kEscFrame))), to);
    std::this_thread::sleep_for(30ms);
    a.send_to(asio::buffer(kLongFrame.substr(half(kLongFrame))), to);
    std::this_thread::sleep_for(30ms);
    b.send_to(asio::buffer(kEscFrame.substr(half(kEscFrame))), to);
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.count() == 2u);
    const std::set<std::string> got{rig.at(0).bytes, rig.at(1).bytes};
    BOOST_TEST((got == std::set<std::string>{kLongFrame, kEscFrame}));
    BOOST_TEST(rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(avr_lines_reencoded_as_beast_type_2_and_3)
{
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("av", "tcp_listen", port, R"(,"format":"avr")"))));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    writeAll(c, "*8D4840D6202CC371C32CE0576098;\r\n@000000000002" "5D4840D6202CC3;\n*8d");
    writeAll(c, "4840d6202cc371c32ce0576098;\n");  // lowercase, split across writes
    BOOST_REQUIRE(eventually([&] { return rig.count() == 3; }));
    const std::string long_frame = wire('3', std::string(6, '\0'), kLongPayload);
    const std::string short_frame = wire('2', std::string(6, '\0'), kShortPayload);
    BOOST_TEST(rig.at(0).bytes == long_frame);
    BOOST_TEST(rig.at(1).bytes == short_frame);  // the "@" ticks are not kept: timestamp 0
    BOOST_TEST(rig.at(2).bytes == long_frame);
    BOOST_TEST((rig.at(0).kind == RawKind::beast));
    BOOST_TEST(rig.state().counters.messages == 3u);
    BOOST_TEST(rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(avr_modeac_and_junk_counted_rejected)
{
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("av", "udp", port, R"(,"format":"avr")"))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, "*0123;\njunk\n\n*8D4840D6202CC371C32CE0576098;\n*ZZ4840D6202CC3;\n");
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.messages == 1; }));
    std::this_thread::sleep_for(50ms);
    const auto s = rig.state();
    BOOST_TEST(rig.count() == 1u);
    BOOST_TEST(s.counters.rejected == 3u);  // Mode A/C, junk, a bad digit; the blank line is nothing
    BOOST_TEST(rig.at(0).bytes == wire('3', std::string(6, '\0'), kLongPayload));
}

BOOST_AUTO_TEST_CASE(beast_capture_bin_equals_published_frames)
{
    TempDir tmp;
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("bc", "tcp_listen", port, R"(,"capture":true)")),
                                            RecordOptions{tmp.path, false, 0}));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    writeAll(c, kShortFrame + kEscFrame + kReceiverId + kLongFrame);
    BOOST_REQUIRE(eventually([&] { return rig.count() == 4; }));
    rig.tick();
    const std::string file = captureFile(tmp.path, "bc", "bin");
    BOOST_TEST(readAll(file) == rig.all());
    BOOST_TEST(readAll(file) == kShortFrame + kEscFrame + kReceiverId + kLongFrame);
    BOOST_TEST(rig.state().counters.capture_dropped == 0u);
}

BOOST_AUTO_TEST_CASE(avr_capture_writes_beast_bin)
{
    TempDir tmp;
    Rig rig;
    const uint16_t port = freeUdpPort();
    rig.run(std::make_unique<BeastRawInput>(
        rig.t.io, defOf(beastDef("ac", "udp", port, R"(,"format":"avr","capture":true)")), RecordOptions{tmp.path, false, 0}));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, "*8D4840D6202CC371C32CE0576098;\n*0123;\n@0000000000025D4840D6202CC3;\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    rig.tick();
    const std::string bin = readAll(captureFile(tmp.path, "ac", "bin"));
    BOOST_TEST(bin == wire('3', std::string(6, '\0'), kLongPayload) + wire('2', std::string(6, '\0'), kShortPayload));
    // The file is a Beast stream: the splitter reads both frames back.
    skycued::beast::FrameSplitter splitter;
    std::vector<skycued::beast::Frame> frames;
    splitter.feed(reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), frames);
    BOOST_TEST(frames.size() == 2u);
    BOOST_TEST(splitter.skippedBytes() == 0u);
}

BOOST_AUTO_TEST_CASE(uat_capture_txt_lines_parse_with_parse_uat_line)
{
    TempDir tmp;
    asio::io_context sio;
    tcp::acceptor acc(sio, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    Rig rig;
    const auto before = std::chrono::system_clock::now();
    rig.run(std::make_unique<UatRawInput>(
        rig.t.io,
        defOf(R"({"id":"uc","type":"uat_raw","host":"127.0.0.1","port":)" + std::to_string(acc.local_endpoint().port()) +
              R"(,"capture":true})"),
        RecordOptions{tmp.path, false, 0}));
    tcp::socket server = acc.accept();
    writeAll(server, "-00a66ef135445d525a0c0519119021204800;rs=1;rssi=-10.0;\n!meta;\n+3c5e8a;rs=2;\n");
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    rig.tick();
    const auto after = std::chrono::system_clock::now();
    const auto txt = lines(readAll(captureFile(tmp.path, "uc", "txt")));
    BOOST_REQUIRE_EQUAL(txt.size(), 2u);
    const auto ms = [](std::chrono::system_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    };
    for (std::size_t i = 0; i < txt.size(); ++i) {
        const auto u = skycued::parseUatLine(txt[i]);
        BOOST_REQUIRE_MESSAGE(u, txt[i]);
        BOOST_REQUIRE(u->epoch_ms);
        BOOST_TEST(*u->epoch_ms >= ms(before));
        BOOST_TEST(*u->epoch_ms <= ms(after));
        BOOST_TEST(u->text == rig.at(i).bytes);  // the line as published
    }
    BOOST_TEST(skycued::parseUatLine(txt[1])->text == "+3c5e8a;rs=2;");
    BOOST_TEST(rig.state().counters.capture_dropped == 0u);
}

BOOST_AUTO_TEST_CASE(capture_write_failure_counts_capture_dropped)
{
    TempDir tmp;
    const std::string blocker = tmp.path + "/not-a-dir";
    { std::ofstream(blocker) << "x"; }  // a record_dir under a regular file: nothing can be created
    const RecordOptions rec{blocker + "/rec", false, 0};
    IoThread t;
    RawTap tap;
    const uint16_t bport = freeUdpPort(), uport = freeUdpPort();
    std::unique_ptr<skycued::Input> b, u;
    t.call([&] {
        b = std::make_unique<BeastRawInput>(t.io, defOf(beastDef("bf", "udp", bport, R"(,"capture":true)")), rec);
        u = std::make_unique<UatRawInput>(
            t.io, defOf(R"({"id":"uf","type":"uat_raw","transport":"udp","bind":"127.0.0.1","port":)" + std::to_string(uport) +
                        R"(,"capture":true})"),
            rec);
        b->start();
        u->start();
    });
    BOOST_REQUIRE(eventually([&] { return t.call([&] { return b->state().connected && u->state().connected; }); }));
    sendUdp(bport, kShortFrame + kLongFrame);
    sendUdp(uport, "-00a66ef1;\n+3c5e8a;\n-00a66ef2;\n");
    BOOST_REQUIRE(eventually([&] {
        return t.call([&] { return b->state().counters.messages == 2 && u->state().counters.messages == 3; });
    }));
    t.call([&] {
        BOOST_TEST(b->state().counters.capture_dropped == 2u);
        BOOST_TEST(u->state().counters.capture_dropped == 3u);
        b.reset();
        u.reset();
    });
}

BOOST_AUTO_TEST_CASE(capture_needs_record_dir_for_raw_types)
{
    IoThread t;
    skycued::InputManagerConfig cfg;
    cfg.poll_interval = 50ms;
    std::unique_ptr<skycued::InputManager> m;
    t.call([&] {
        m = std::make_unique<skycued::InputManager>(t.io, cfg);
        m->load();
        m->start();
    });
    for (const std::string& def :
         {beastDef("bcap", "udp", freeUdpPort(), R"(,"capture":true)"),
          R"({"id":"ucap","type":"uat_raw","transport":"udp","bind":"127.0.0.1","port":)" + std::to_string(freeUdpPort()) +
              R"(,"capture":true})"}) {
        try {
            t.call([&] { return m->create(defOf(def)); });
            BOOST_FAIL("capture without a record dir accepted: " << def);
        } catch (const skycued::InputDefError& e) {
            BOOST_TEST(std::string(e.what()) == "capture needs recording.dir");
        }
    }
    t.call([&] { m.reset(); });
}

BOOST_AUTO_TEST_CASE(beast_tcp_listen_peer_filter)
{
    {  // a peer filter that does not match: the connection is refused, nothing is published
        Rig rig;
        const uint16_t port = freeTcpPort();
        rig.run(std::make_unique<BeastRawInput>(rig.t.io,
                                                defOf(beastDef("bp", "tcp_listen", port, R"(,"peer":"192.0.2.1")"))));
        asio::io_context cio;
        tcp::socket c = connectWhenListening(cio, port);
        boost::system::error_code ignored;
        asio::write(c, asio::buffer(kShortFrame), ignored);
        BOOST_REQUIRE(eventually([&] { return rig.state().counters.rejected_peer == 1; }));
        std::this_thread::sleep_for(50ms);
        BOOST_TEST(rig.count() == 0u);
        BOOST_TEST(rig.state().counters.messages == 0u);
    }
    {  // the matching peer is served
        Rig rig;
        const uint16_t port = freeTcpPort();
        rig.run(std::make_unique<BeastRawInput>(rig.t.io,
                                                defOf(beastDef("bp", "tcp_listen", port, R"(,"peer":"127.0.0.1")"))));
        asio::io_context cio;
        tcp::socket c = connectWhenListening(cio, port);
        writeAll(c, kShortFrame);
        BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
        BOOST_TEST(rig.at(0).bytes == kShortFrame);
        BOOST_TEST(rig.state().counters.rejected_peer == 0u);
    }
}

// ---- the decoder on beast_raw (inputs.decoder=skycued) ----

namespace {

std::string beastFrame(const st::Bytes& b, const std::string& ts = kTs1)
{
    return wire(b.size() == 7 ? '2' : '3', ts, std::string(b.begin(), b.end()));
}

const st::cpr_pos kNear{38.95, -77.05};

st::Bytes posFrame(uint32_t addr, const st::cpr_pos& p, bool odd, int alt = 10000)
{
    return st::df17(addr, st::airbornePosition(11, st::encodeCpr(p, odd, false), alt));
}

st::Bytes tisbFrame(uint32_t addr, const st::cpr_pos& p, bool odd = false)
{
    return st::df18(2, addr, st::airbornePosition(11, st::encodeCpr(p, odd, false), 9000));
}

st::Bytes identFrame(uint32_t addr, const std::string& cs = "ABC123")
{
    return st::df17(addr, st::identification(4, 3, cs));
}

RawDecodeOptions decodeOn(std::optional<std::pair<double, double>> ref = std::nullopt)
{
    RawDecodeOptions o;
    o.enabled = true;
    o.reference = ref;
    return o;
}

// A Rig that also keeps the input's observations.
struct DecRig : Rig {
    std::mutex om;
    std::vector<skycued::TrackObservation> obs;
    void runInput(std::unique_ptr<BeastRawInput> input)
    {
        input->onObservation([this](const skycued::TrackObservation& o) {
            std::lock_guard<std::mutex> lock(om);
            obs.push_back(o);
        });
        run(std::move(input));
    }
    std::size_t obsCount()
    {
        std::lock_guard<std::mutex> lock(om);
        return obs.size();
    }
    skycued::TrackObservation obsAt(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(om);
        return obs.at(i);
    }
};

// Beast type-3 frames for the ADS-B rows of the committed pyModeS-style CSV (hex in the second column);
// raw/dump1090-modes1.bin holds IQ samples, not frames.
std::string sampleBytes()
{
    std::istringstream in(readAll(std::string(SKYCUE_TEST_DATA_DIR) + "/raw/pymodes-adsb.csv"));
    std::string out, line;
    std::size_t n = 0;
    while (n < 400 && std::getline(in, line)) {
        const auto q = line.find('"');
        if (q == std::string::npos || line.compare(q + 1, 2, "8D") != 0 || line.find('"', q + 1) != q + 29)
            continue;
        st::Bytes b;
        for (std::size_t i = 0; i < 28; i += 2)
            b.push_back(static_cast<uint8_t>(std::stoul(line.substr(q + 1 + i, 2), nullptr, 16)));
        out += beastFrame(b);
        ++n;
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(decoder_off_publishes_only)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("off", "udp", port))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, beastFrame(posFrame(0xABC123, kNear, false)) + beastFrame(posFrame(0xABC123, kNear, true)));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 2; }));
    std::this_thread::sleep_for(50ms);
    const auto s = rig.state();
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(!s.decoding);
    BOOST_TEST(s.counters.messages == 2u);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.decoded == 0u);
    BOOST_TEST(s.counters.decoder_observations == 0u);
    BOOST_TEST(s.counters.observations == 0u);
}

BOOST_AUTO_TEST_CASE(decoder_on_emits_observations_and_still_publishes, *skycue_test::needs_test_data("raw/pymodes-adsb.csv"))
{
    const std::string bin = sampleBytes();
    BOOST_REQUIRE(!bin.empty());
    std::vector<skycued::beast::Frame> all;
    skycued::beast::FrameSplitter().feed(reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), all);
    BOOST_REQUIRE(!all.empty());

    DecRig rig;
    const uint16_t port = freeTcpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("dec", "tcp_listen", port)), RecordOptions{},
                                                 decodeOn()));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    writeAll(c, bin);
    BOOST_REQUIRE(eventually([&] { return rig.count() == all.size(); }));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.frames > 0; }));
    std::this_thread::sleep_for(100ms);
    const auto s = rig.state();
    BOOST_TEST(rig.count() == all.size());
    BOOST_TEST(s.counters.messages == all.size());
    BOOST_TEST(s.decoding);
    BOOST_TEST(s.counters.decoder_observations > 0u,
               "frames " << s.counters.frames << " crc_fail " << s.counters.crc_fail << " decoded " << s.counters.decoded
                         << " pending " << s.counters.cpr_pending << " nopos " << s.counters.no_position << " rej "
                         << s.counters.cpr_rejected << " skipped " << s.counters.modeac_skipped << "/"
                         << s.counters.other_skipped << "/" << s.counters.mlat_skipped);
    BOOST_TEST(s.counters.observations == s.counters.decoder_observations);
    BOOST_TEST(rig.obsCount() == s.counters.observations);
    for (std::size_t i = 0; i < rig.obsCount(); ++i)
        BOOST_REQUIRE_EQUAL(rig.obsAt(i).input_id, "dec");
}

BOOST_AUTO_TEST_CASE(avr_lines_with_decoder_emit_observations)
{
    DecRig rig;
    const uint16_t port = freeTcpPort();
    rig.runInput(std::make_unique<BeastRawInput>(
        rig.t.io, defOf(beastDef("avdec", "tcp_listen", port, R"(,"format":"avr")")), RecordOptions{}, decodeOn()));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    const auto avr = [](const st::Bytes& b) {
        static const char* hex = "0123456789ABCDEF";
        std::string line = "*";
        for (uint8_t v : b) {
            line += hex[v >> 4];
            line += hex[v & 15];
        }
        return line + ";\n";
    };
    writeAll(c, avr(posFrame(0xABC123, kNear, false)) + avr(posFrame(0xABC123, kNear, true)));
    BOOST_REQUIRE(eventually([&] { return rig.obsCount() >= 1; }));
    const auto o = rig.obsAt(0);
    BOOST_TEST(o.id.address == 0xABC123u);
    BOOST_TEST(o.lat.has_value());
    BOOST_TEST(o.input_id == "avdec");
    BOOST_TEST(rig.state().counters.decoder_observations >= 1u);
    BOOST_TEST(rig.state().counters.rejected == 0u);
}

BOOST_AUTO_TEST_CASE(counters_in_state_json, *skycue_test::needs_test_data("raw/pymodes-adsb.csv"))
{
    const std::string bin = sampleBytes();
    std::vector<skycued::beast::Frame> all;
    skycued::beast::FrameSplitter().feed(reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), all);
    DecRig rig;
    const uint16_t port = freeTcpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("cj", "tcp_listen", port)), RecordOptions{},
                                                 decodeOn()));
    asio::io_context cio;
    tcp::socket c = connectWhenListening(cio, port);
    writeAll(c, bin + beastFrame(identFrame(0x111111)));
    BOOST_REQUIRE(eventually([&] { return rig.count() == all.size() + 1; }));
    std::this_thread::sleep_for(100ms);
    const auto s = rig.state();
    const auto& k = s.counters;
    BOOST_TEST(k.frames + k.mlat_skipped + k.modeac_skipped + k.other_skipped == k.messages);
    BOOST_TEST(k.frames > 0u);

    skycued::InputEntry entry;
    entry.def = defOf(beastDef("cj", "tcp_listen", port));
    entry.state = s;
    const std::string text = skycued::webapi::inputJson(entry, std::chrono::system_clock::now());
    BOOST_REQUIRE(skycued::JsonObject::parse(text));
    for (const char* name : {"frames", "crc_fail", "crc_fixed", "ap_unconfirmed", "no_position", "cpr_pending",
                             "cpr_rejected", "decoded", "unmapped", "unknown_type", "evicted", "decoder_observations",
                             "mlat_skipped", "modeac_skipped", "other_skipped", "commb_bds40", "commb_bds50",
                             "commb_bds60", "commb_ambiguous", "commb_other", "commb_empty", "commb_unknown",
                             "acas_ra", "acas_ra_tc28"})
        BOOST_TEST(text.find(std::string("\"") + name + "\":") != std::string::npos, name);
}

BOOST_AUTO_TEST_CASE(mlat_marker_frame_counts_mlat_skipped)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("ml", "udp", port)), RecordOptions{},
                                                 decodeOn()));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    const std::string marker("\xff\x00\x4d\x4c\x41\x54", 6);
    sendUdp(port, beastFrame(identFrame(0x222222), marker));
    BOOST_REQUIRE(eventually([&] { return rig.count() == 1; }));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.mlat_skipped == 1; }));
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.decoder_observations == 0u);
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(s.counters.messages == 1u);  // still published
}

BOOST_AUTO_TEST_CASE(crc_bad_frame_counts_crc_fail)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("cf", "udp", port)), RecordOptions{},
                                                 decodeOn()));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    st::Bytes bad = identFrame(0x333333);
    for (std::size_t i = 4; i < 12; ++i)
        bad[i] = static_cast<uint8_t>(~bad[i]);  // far beyond a single-bit repair
    sendUdp(port, beastFrame(bad));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.crc_fail == 1; }));
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 1u);
    BOOST_TEST(s.counters.decoded == 0u);
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(rig.count() == 1u);  // published all the same
}

BOOST_AUTO_TEST_CASE(no_reference_global_decode_only)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("nr", "udp", port)), RecordOptions{},
                                                 decodeOn()));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, beastFrame(posFrame(0xABC123, kNear, false)));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.frames == 1; }));
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(rig.state().counters.cpr_pending + rig.state().counters.no_position >= 1u);
    for (std::size_t i = 0; i < rig.obsCount(); ++i)
        BOOST_TEST(!rig.obsAt(i).lat.has_value());
    sendUdp(port, beastFrame(posFrame(0xABC123, kNear, true)));
    BOOST_REQUIRE(eventually([&] {
        for (std::size_t i = 0; i < rig.obsCount(); ++i)
            if (rig.obsAt(i).lat)
                return true;
        return false;
    }));
    for (std::size_t i = 0; i < rig.obsCount(); ++i)
        if (rig.obsAt(i).lat) {
            BOOST_TEST(*rig.obsAt(i).lat == kNear.lat, boost::test_tools::tolerance(0.001));
            BOOST_TEST(*rig.obsAt(i).lon == kNear.lon, boost::test_tools::tolerance(0.001));
        }
}

BOOST_AUTO_TEST_CASE(reference_from_def_overrides_receiver)
{
    // One frame decodes locally against the input's reference: the def's ref_lat/ref_lon when set, else the
    // receiver's. The right place comes out only for a reference near the aircraft.
    auto run = [&](const std::string& extra, std::optional<std::pair<double, double>> receiver) {
        DecRig rig;
        const uint16_t port = freeUdpPort();
        skycued::InputManagerConfig cfg;
        cfg.decode_raw = true;
        cfg.receiver_ref = receiver;
        rig.t.call([&] {
            auto in = skycued::makeInput(rig.t.io, defOf(beastDef("rf", "udp", port, extra)), cfg);
            in->onObservation([&](const skycued::TrackObservation& o) {
                std::lock_guard<std::mutex> lock(rig.om);
                rig.obs.push_back(o);
            });
            rig.in = std::move(in);
            rig.in->setRawTap(&rig.tap);
            rig.in->start();
        });
        BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
        sendUdp(port, beastFrame(posFrame(0xABC123, kNear, false)));
        BOOST_REQUIRE(eventually([&] { return rig.state().counters.frames == 1; }));
        std::this_thread::sleep_for(100ms);
        for (std::size_t i = 0; i < rig.obsCount(); ++i)
            if (rig.obsAt(i).lat)
                return *rig.obsAt(i).lat;
        return 999.0;  // no position
    };
    const std::pair<double, double> far{-33.0, 151.0}, near{38.9, -77.0};
    BOOST_TEST(std::fabs(run("", far) - kNear.lat) > 1.0);                                   // a far receiver is no help
    BOOST_TEST(run(R"(,"ref_lat":38.9,"ref_lon":-77.0)", far) == kNear.lat, boost::test_tools::tolerance(0.001));
    BOOST_TEST(run("", near) == kNear.lat, boost::test_tools::tolerance(0.001));
}

BOOST_AUTO_TEST_CASE(udp_datagram_three_frames_three_decodes)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("u3", "udp", port)), RecordOptions{},
                                                 decodeOn(std::make_pair(38.9, -77.0))));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, beastFrame(posFrame(0x100001, kNear, false)) + beastFrame(posFrame(0x100002, kNear, false)) +
                      beastFrame(posFrame(0x100003, kNear, false)));  // one local decode each
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.decoded == 3; }));
    std::this_thread::sleep_for(50ms);
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 3u);
    BOOST_TEST(s.counters.decoded == 3u);
    BOOST_TEST(rig.obsCount() == 3u);
    BOOST_TEST(rig.count() == 3u);
}

BOOST_AUTO_TEST_CASE(stop_mid_frames_no_callback_after_stop)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    rig.t.call([&] {
        rig.tap.setSink([&](const RawMessage&) { rig.in->stop(); }, [](RawKind) { return true; });
    });
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("sm", "udp", port)), RecordOptions{},
                                                 decodeOn()));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, beastFrame(identFrame(0x100001)) + beastFrame(identFrame(0x100002)) + beastFrame(identFrame(0x100003)));
    BOOST_REQUIRE(eventually([&] { return !rig.connected(); }));
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(rig.state().counters.frames == 0u);
}

BOOST_AUTO_TEST_CASE(idle_keys_evicted_after_300s)
{
    DecRig rig;
    const uint16_t port = freeUdpPort();
    auto now = std::make_shared<std::atomic<int64_t>>(1'700'000'000);
    RawDecodeOptions o = decodeOn();
    o.clock = [now] { return std::chrono::system_clock::time_point(std::chrono::seconds(now->load())); };
    rig.runInput(std::make_unique<BeastRawInput>(rig.t.io, defOf(beastDef("ev", "udp", port)), RecordOptions{}, o));
    BOOST_REQUIRE(eventually([&] { return rig.connected(); }));
    sendUdp(port, beastFrame(identFrame(0x100001)));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.decoded == 1; }));
    BOOST_TEST(rig.state().counters.evicted == 0u);
    now->store(1'700'000'000 + 301);
    sendUdp(port, beastFrame(identFrame(0x100002)));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.decoded == 2; }));
    BOOST_TEST(rig.state().counters.evicted == 1u);
    BOOST_TEST(rig.state().decoder_keys == 1u);
}

namespace {

// An Engine over beast_raw UDP inputs with the decoder on.
struct RawEngineFx {
    std::map<std::string, uint16_t> ports;
    std::unique_ptr<EngineFx> fx;
    RawEngineFx(const std::vector<std::string>& ids, std::optional<std::pair<double, double>> ref)
    {
        for (const auto& id : ids)
            ports[id] = freeUdpPort();
        auto ports_copy = ports;
        fx = std::make_unique<EngineFx>(
            baseConfig(), std::function<std::unique_ptr<skycued::InputManager>(asio::io_context&)>(
                              [ports_copy, ref](asio::io_context& io) {
                                  auto m = std::make_unique<skycued::InputManager>(io, skycued::InputManagerConfig{});
                                  for (const auto& kv : ports_copy)
                                      m->addBuiltin(std::make_unique<BeastRawInput>(
                                          io, defOf(beastDef(kv.first, "udp", kv.second)), RecordOptions{}, decodeOn(ref)));
                                  m->load();
                                  return m;
                              }));
    }
    template <class P> bool wait(P pred)
    {
        return eventually([&] { return pred(fx->stats()); });
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(rebroadcast_flag_reaches_engine)
{
    const std::pair<double, double> ref{38.9, -77.0};
    RawEngineFx e({"rb"}, ref);
    const uint16_t port = e.ports["rb"];
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) { return !s.inputs.empty() && s.inputs[0].connected; }));
    sendUdp(port, beastFrame(posFrame(0xABC123, kNear, false)));
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) { return s.tracks_total == 1; }));
    BOOST_TEST(e.fx->stats().rebroadcast_suppressed == 0u);
    sendUdp(port, beastFrame(tisbFrame(0xABC123, {38.97, -77.03})));  // a TIS-B copy inside 10 s
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) { return s.rebroadcast_suppressed == 1; }));
    BOOST_TEST(e.fx->stats().tracks_total == 1u);
}

BOOST_AUTO_TEST_CASE(two_feeds_one_aircraft_tracks_once)
{
    RawEngineFx e({"fa", "fb"}, std::nullopt);
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) {
        return s.inputs.size() == 2 && s.inputs[0].connected && s.inputs[1].connected;
    }));
    const std::string frames =
        beastFrame(posFrame(0xABC123, kNear, false)) + beastFrame(posFrame(0xABC123, kNear, true));
    sendUdp(e.ports["fa"], frames);
    sendUdp(e.ports["fb"], frames);
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) {
        return s.inputs.size() == 2 && s.inputs[0].decoder.decoder_observations >= 1 &&
               s.inputs[1].decoder.decoder_observations >= 1;
    }));
    const auto s = e.fx->stats();
    BOOST_TEST(s.tracks_total == 1u);  // one aircraft, one track
    BOOST_TEST(s.inputs[0].decoder.decoder_observations == s.inputs[0].observations);  // counted per input
    BOOST_TEST(s.inputs[1].decoder.decoder_observations == s.inputs[1].observations);
    BOOST_TEST(s.observations == s.inputs[0].observations + s.inputs[1].observations);
}
