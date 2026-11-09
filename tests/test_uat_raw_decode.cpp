#define BOOST_TEST_MODULE skycued_uat_raw_decode
#include <boost/test/unit_test.hpp>

#include "decode/uat_decoder.hpp"
#include "decode/uat_frame.hpp"
#include "engine_test_support.hpp"
#include "fixture_io.hpp"
#include "gz_line_reader.hpp"
#include "hub/raw_tap.hpp"
#include "inputs/beast_raw_input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "inputs/inputs_file.hpp"
#include "inputs/readsb_stream_input.hpp"
#include "inputs/uat_raw_input.hpp"
#include "json_lite.hpp"
#include "raw_formats.hpp"
#include "restricted_fixtures.hpp"
#include "uat_test_support.hpp"
#include "web_inputs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace enginetest;
using asio::ip::udp;
using skycued::InputType;
using skycued::RawDecodeOptions;
using skycued::RawKind;
using skycued::RawMessage;
using skycued::RawTap;
using skycued::RecordOptions;
using skycued::UatRawInput;
namespace uat = skycued::decode::uat;

namespace {

const std::string kRaw = std::string(SKYCUE_FIXTURE_DIR) + "/raw/";

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

InputDef defOf(const std::string& json)
{
    const auto o = skycued::JsonObject::parse(json);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + json);
    return skycued::parseInputDef(*o);
}

std::string listenDef(const std::string& id, const char* type, uint16_t port, const std::string& extra = "")
{
    return R"({"id":")" + id + R"(","type":")" + type + R"(","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
           std::to_string(port) + extra + "}";
}

// The built-in dump978 is a tcp_connect def with a reserved id; listening here keeps the test self-contained.
InputDef builtinDump978(uint16_t port)
{
    InputDef d;
    d.id = "dump978";
    d.type = InputType::dump978;
    d.transport = skycued::InputTransport::tcp_listen;
    d.bind = "127.0.0.1";
    d.port = port;
    d.builtin = true;
    return d;
}

RawDecodeOptions decodeOn()
{
    RawDecodeOptions o;
    o.enabled = true;
    return o;
}

struct Fixture {
    std::vector<std::string> down, up;  // lines without the stratux/rawrec prefix, ';'-terminated
};

Fixture loadUatFixture(const std::string& file)
{
    Fixture f;
    skycue_test::forEachLine(kRaw + file, [&](const std::string& line) {
        const auto u = skycued::parseUatLine(line);  // the <ns>, prefix; START, lines are refused
        if (!u)
            return;
        (u->text[0] == '-' ? f.down : f.up).push_back(u->text);
    });
    return f;
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string s;
    for (const auto& l : lines)
        s += l + "\n";
    return s;
}

struct Rig {
    IoThread t;
    RawTap tap;
    std::mutex m;
    std::vector<std::string> published, uplinks;
    std::vector<skycued::TrackObservation> obs;
    std::unique_ptr<skycued::Input> in;

    Rig()
    {
        t.call([&] {
            tap.setSink(
                [this](const RawMessage& msg) {
                    std::lock_guard<std::mutex> lock(m);
                    if (msg.kind == RawKind::uat)
                        published.emplace_back(msg.bytes->begin(), msg.bytes->end());
                },
                [](RawKind) { return true; });
        });
    }
    void run(std::unique_ptr<skycued::Input> input)
    {
        t.call([&] {
            in = std::move(input);
            in->setRawTap(&tap);
            in->onUplink([this](std::chrono::system_clock::time_point, const std::string& hex) {
                std::lock_guard<std::mutex> lock(m);
                uplinks.push_back(hex);
            });
            in->onObservation([this](const skycued::TrackObservation& o) {
                std::lock_guard<std::mutex> lock(m);
                obs.push_back(o);
            });
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
    std::size_t published_count()
    {
        std::lock_guard<std::mutex> lock(m);
        return published.size();
    }
    std::size_t obsCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs.size();
    }
    std::size_t uplinkCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return uplinks.size();
    }
    std::vector<skycued::TrackObservation> observations()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs;
    }
};

// Sends `text` over one TCP connection to a listening input and waits until `lines` messages were counted.
void feed(Rig& rig, uint16_t port, const std::string& text, std::size_t messages)
{
    asio::io_context cio;
    const auto until = steady::now() + kWait;
    tcp::socket s(cio);
    for (;;) {
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec)
            break;
        if (steady::now() > until)
            throw std::runtime_error("cannot connect to port " + std::to_string(port));
        s.close();
        std::this_thread::sleep_for(10ms);
    }
    asio::write(s, asio::buffer(text));
    BOOST_REQUIRE(eventually([&] { return rig.state().counters.messages >= messages; }));
    std::this_thread::sleep_for(50ms);
}

uint16_t runUat(Rig& rig, const std::string& id, bool decode)
{
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<UatRawInput>(rig.t.io, defOf(listenDef(id, "uat_raw", port)), RecordOptions{},
                                          decode ? decodeOn() : RawDecodeOptions{}));
    return port;
}

std::string hexOf(const std::string& line)
{
    const auto end = line.find(';');
    return line.substr(1, end == std::string::npos ? std::string::npos : end - 1);
}

}  // namespace

BOOST_AUTO_TEST_CASE(decode_off_counts_and_publishes_only, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    BOOST_REQUIRE_EQUAL(f.down.size(), 439u);
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(std::make_unique<UatRawInput>(rig.t.io, builtinDump978(port)));
    feed(rig, port, joined(f.down), f.down.size());
    const auto s = rig.state();
    const auto* u = dynamic_cast<UatRawInput*>(rig.in.get());
    BOOST_TEST(rig.t.call([&] { return u->downlinkFrames(); }) == 439u);
    BOOST_TEST(rig.published_count() == 439u);
    BOOST_TEST(!s.decoding);
    BOOST_TEST(s.counters.messages == 439u);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.decoded == 0u);
    BOOST_TEST(s.counters.decoder_observations == 0u);
    BOOST_TEST(s.counters.observations == 0u);
    BOOST_TEST(rig.obsCount() == 0u);
}

BOOST_AUTO_TEST_CASE(builtin_dump978_does_not_decode_under_readsb, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    skycued::InputManagerConfig cfg;
    cfg.decode_raw = false;
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(rig.t.call([&] { return skycued::makeInput(rig.t.io, builtinDump978(port), cfg); }));
    feed(rig, port, joined(f.down), f.down.size());
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(!rig.state().decoding);
    BOOST_TEST(rig.state().counters.frames == 0u);
    BOOST_TEST(rig.published_count() == f.down.size());
}

BOOST_AUTO_TEST_CASE(builtin_dump978_decodes_under_skycued, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    skycued::InputManagerConfig cfg;
    cfg.decode_raw = true;
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(rig.t.call([&] { return skycued::makeInput(rig.t.io, builtinDump978(port), cfg); }));
    feed(rig, port, joined(f.down), f.down.size());
    const auto s = rig.state();
    BOOST_TEST(s.decoding);
    BOOST_TEST(s.counters.frames == f.down.size());
    BOOST_TEST(s.counters.decoder_observations > 0u);
    BOOST_TEST(s.counters.observations == s.counters.decoder_observations);
    BOOST_TEST(rig.obsCount() == s.counters.observations);
    BOOST_TEST(rig.published_count() == f.down.size());  // the tap still sees every line
    for (const auto& o : rig.observations())
        BOOST_REQUIRE_EQUAL(o.input_id, "dump978");
}

BOOST_AUTO_TEST_CASE(user_uat_raw_decodes_under_skycued, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    skycued::InputManagerConfig cfg;
    cfg.decode_raw = true;
    cfg.receiver_ref = std::make_pair(38.9, -77.0);
    Rig rig;
    const uint16_t port = freeTcpPort();
    rig.run(rig.t.call([&] { return skycued::makeInput(rig.t.io, defOf(listenDef("ux", "uat_raw", port)), cfg); }));
    feed(rig, port, joined(f.down), f.down.size());
    BOOST_TEST(rig.state().decoding);
    BOOST_TEST(rig.obsCount() > 0u);
    for (const auto& o : rig.observations())
        BOOST_REQUIRE_EQUAL(o.input_id, "ux");
}

BOOST_AUTO_TEST_CASE(uplinks_still_go_to_fisb_when_decoding, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    BOOST_REQUIRE(!f.up.empty());
    Rig rig;
    const uint16_t port = runUat(rig, "up", true);
    feed(rig, port, joined(f.up), f.up.size());
    const auto s = rig.state();
    BOOST_TEST(rig.uplinkCount() == f.up.size());
    BOOST_TEST(rig.published_count() == f.up.size());
    BOOST_TEST(s.counters.uplinks == f.up.size());
    BOOST_TEST(s.counters.frames == 0u);  // uplinks are not decoded
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(rig.uplinks.front() == hexOf(f.up.front()));
}

BOOST_AUTO_TEST_CASE(rssi_key_reaches_observation, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    // The first downlink that yields an observation on its own.
    std::string pick;
    for (const auto& l : f.down) {
        skycued::decode::UatDecoder d({}, "p");
        bool any = false;
        d.onObservation([&](const skycued::TrackObservation&) { any = true; });
        d.feed(*uat::parseDownlinkLine(l), std::chrono::system_clock::now());
        if (any) {
            pick = hexOf(l);
            break;
        }
    }
    BOOST_REQUIRE(!pick.empty());
    Rig rig;
    const uint16_t port = runUat(rig, "rs", true);
    feed(rig, port, "-" + pick + ";rssi=-12.5;rs=1\n", 1);
    BOOST_REQUIRE_EQUAL(rig.obsCount(), 1u);
    const auto o = rig.observations()[0];
    BOOST_REQUIRE(o.rssi.has_value());
    BOOST_TEST(*o.rssi == -12.5, boost::test_tools::tolerance(0.11));
}

BOOST_AUTO_TEST_CASE(bad_length_line_counts_rejected_and_is_still_published)
{
    Rig rig;
    const uint16_t port = runUat(rig, "bl", true);
    feed(rig, port, "-00112233;\n", 1);  // hex, but neither 18 nor 34 bytes
    const auto s = rig.state();
    BOOST_TEST(s.counters.messages == 1u);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.rejected == 1u);
    BOOST_TEST(rig.published_count() == 1u);
    BOOST_TEST(rig.obsCount() == 0u);
}

BOOST_AUTO_TEST_CASE(rs_errors_of_the_line_are_counted)
{
    skycued::testing::UatSpec spec;
    spec.type = 1;
    spec.lat = 38.95;
    spec.lon = -77.0;
    spec.alt_ft = 5000;
    spec.nic = 8;
    const std::string hex = skycued::testing::toHex(skycued::testing::encodeUat(spec));
    Rig rig;
    const uint16_t port = runUat(rig, "rs", true);
    feed(rig, port, "-" + hex + ";rs=2;\n-" + hex + ";rs=1;\n-" + hex + ";\n", 3);
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 3u);
    BOOST_TEST(s.counters.rs_errors == 3u);
    skycued::InputEntry entry;
    entry.def = defOf(listenDef("rs", "uat_raw", port));
    entry.state = s;
    BOOST_TEST(skycued::webapi::inputJson(entry, std::chrono::system_clock::now()).find("\"rs_errors\":3") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(fixture_counts_per_qualifier, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    std::map<int, int> per_qualifier, per_type;
    for (const auto& l : f.down) {
        const auto fr = uat::parseDownlinkLine(l);
        BOOST_REQUIRE(fr);
        ++per_qualifier[static_cast<int>(fr->qualifier())];
        ++per_type[fr->payload_type()];
    }
    Rig rig;
    const uint16_t port = runUat(rig, "fx", true);
    feed(rig, port, joined(f.down), f.down.size());
    const auto s = rig.state();
    const auto& k = s.counters;
    std::string seen;
    for (const auto& kv : per_qualifier)
        seen += " q" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
    for (const auto& kv : per_type)
        seen += " t" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
    BOOST_TEST_MESSAGE("fixture:" << seen << " | frames " << k.frames << " decoded " << k.decoded << " unmapped "
                                  << k.unmapped << " no_position " << k.no_position << " cpr_rejected "
                                  << k.cpr_rejected << " unknown_type " << k.unknown_type << " obs "
                                  << k.decoder_observations);
    BOOST_TEST(k.messages == 439u);
    BOOST_TEST(k.frames == 439u);
    BOOST_TEST(k.unknown_type == 0u);
    BOOST_TEST(k.frames == k.unknown_type + k.no_position + k.unmapped + k.decoded);
    // Pinned from the first run (the dump978 sample is all airborne, no TIS-B beyond these).
    BOOST_TEST(per_qualifier[0] == 318);  // ADS-B, ICAO address
    BOOST_TEST(per_qualifier[2] == 51);   // TIS-B/ADS-R, ICAO address
    BOOST_TEST(per_qualifier[3] == 70);   // TIS-B trackfile
    BOOST_TEST(per_qualifier.size() == 3u);
    BOOST_TEST(per_type[0] == 169);
    BOOST_TEST(per_type[1] == 192);
    BOOST_TEST(per_type[2] == 78);
    BOOST_TEST(k.decoded == 439u);
    BOOST_TEST(k.unmapped == 0u);
    BOOST_TEST(k.no_position == 0u);
    BOOST_TEST(k.cpr_rejected == 0u);
    BOOST_TEST(k.decoder_observations == 439u);
}

BOOST_AUTO_TEST_CASE(stratux_log_decodes, *skycue_test::needs_fixture("raw/stratux-uat-2015.log.gz"))
{
    const auto f = loadUatFixture("stratux-uat-2015.log.gz");
    BOOST_REQUIRE(!f.down.empty());
    Rig rig;
    const uint16_t port = runUat(rig, "st", true);
    feed(rig, port, joined(f.down) + joined(f.up), f.down.size() + f.up.size());
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 3760u);  // the golden pairs the same 3,760 frames
    BOOST_TEST(s.counters.unknown_type == 0u);
    BOOST_TEST(s.counters.decoded == 3760u);
    BOOST_TEST(s.counters.frames + s.counters.rejected >= f.down.size());
    BOOST_TEST(rig.uplinkCount() == f.up.size());
}

BOOST_AUTO_TEST_CASE(long_frames_carry_both_altitudes, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    int type1 = 0, with_both = 0, type0 = 0;
    for (const auto& l : f.down) {
        const auto fr = uat::parseDownlinkLine(l);
        BOOST_REQUIRE(fr);
        skycued::decode::UatDecoder d({}, "p");
        std::vector<skycued::TrackObservation> got;
        d.onObservation([&](const skycued::TrackObservation& o) { got.push_back(o); });
        d.feed(*fr, std::chrono::system_clock::now());
        const bool both = std::any_of(got.begin(), got.end(),
                                      [](const skycued::TrackObservation& o) { return o.geom_minus_baro_ft.has_value(); });
        if (fr->payload_type() == 0) {
            ++type0;
            BOOST_TEST_CONTEXT(hexOf(l)) { BOOST_TEST(!both); }
        } else if (fr->payload_type() == 1 && fr->qualifier() != uat::AddressQualifier::tisb_trackfile) {
            ++type1;
            with_both += both ? 1 : 0;
        }
    }
    BOOST_TEST_MESSAGE("type0 " << type0 << " type1 " << type1 << " with both " << with_both);
    BOOST_TEST(type1 > 0);
    BOOST_TEST(with_both == type1);
}

BOOST_AUTO_TEST_CASE(counters_in_state_json, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    Rig rig;
    const uint16_t port = runUat(rig, "cj", true);
    feed(rig, port, joined(f.down), f.down.size());
    skycued::InputEntry entry;
    entry.def = defOf(listenDef("cj", "uat_raw", port));
    entry.state = rig.state();
    const std::string text = skycued::webapi::inputJson(entry, std::chrono::system_clock::now());
    BOOST_REQUIRE(skycued::JsonObject::parse(text));
    for (const char* name : {"frames", "crc_fail", "crc_fixed", "ap_unconfirmed", "no_position", "cpr_pending",
                             "cpr_rejected", "decoded", "unmapped", "unknown_type", "evicted", "decoder_observations",
                             "mlat_skipped", "modeac_skipped", "other_skipped", "no_raw", "raw_duplicates", "uplinks", "rs_errors"})
        BOOST_TEST(text.find(std::string("\"") + name + "\":") != std::string::npos, name);
}

BOOST_AUTO_TEST_CASE(stop_mid_stream_no_callback_after_stop, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    Rig rig;
    rig.t.call([&] {
        rig.tap.setSink([&](const RawMessage&) { rig.in->stop(); }, [](RawKind) { return true; });
    });
    const uint16_t port = runUat(rig, "sm", true);
    asio::io_context cio;
    tcp::socket s(cio);
    const auto until = steady::now() + kWait;
    for (;;) {
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (!ec || steady::now() > until)
            break;
        s.close();
        std::this_thread::sleep_for(10ms);
    }
    asio::write(s, asio::buffer(joined({f.down.begin(), f.down.begin() + 5})));
    std::this_thread::sleep_for(200ms);
    BOOST_TEST(rig.obsCount() == 0u);
    BOOST_TEST(rig.state().counters.frames == 0u);
}

BOOST_AUTO_TEST_CASE(skycued_with_readsb_uat_json_warns)
{
    InputDef rs = defOf(R"({"id":"rs2","type":"readsb","host":"127.0.0.1","port":30047})");
    InputDef u = defOf(listenDef("u1", "uat_raw", 30978));
    // A decoding uat_raw beside a readsb JSON input: one WARN naming both.
    const auto w = skycued::decoderWarnings({rs, u}, {});
    const auto uat_warnings = std::count_if(w.begin(), w.end(), [](const std::string& s) {
        return s.find("'rs2'") != std::string::npos && s.find("'u1'") != std::string::npos;
    });
    BOOST_TEST(uat_warnings == 1);
    BOOST_TEST(w.size() == 1u);
    BOOST_TEST(w[0].find("uat_raw") != std::string::npos);
    // The built-in dump978 is named by its id too.
    InputDef d = builtinDump978(30978);
    const auto wd = skycued::decoderWarnings({rs, d}, {});
    BOOST_REQUIRE(wd.size() == 1u);
    BOOST_TEST(wd[0].find("'dump978'") != std::string::npos);
    // Without a uat input (no uat input configured), or with it disabled: nothing.
    BOOST_TEST(skycued::decoderWarnings({rs}, {}).empty());
    u.enabled = false;
    BOOST_TEST(skycued::decoderWarnings({rs, u}, {}).empty());
}

namespace {

// An Engine over a decoding uat_raw (UDP) and, optionally, a readsb JSON UDP input.
struct FeedsFx {
    uint16_t uat_port = 0, json_port = 0;
    std::unique_ptr<EngineFx> fx;
    FeedsFx()
    {
        uat_port = freeUdpPort();
        json_port = freeUdpPort();
        const uint16_t up = uat_port, jp = json_port;
        fx = std::make_unique<EngineFx>(
            baseConfig(), std::function<std::unique_ptr<skycued::InputManager>(asio::io_context&)>(
                              [up, jp](asio::io_context& io) {
                                  auto m = std::make_unique<skycued::InputManager>(io, skycued::InputManagerConfig{});
                                  const std::string uat_def = R"({"id":"ua","type":"uat_raw","transport":"udp","bind":"127.0.0.1","port":)" +
                                                              std::to_string(up) + "}";
                                  const std::string json_def = R"({"id":"rj","type":"readsb","transport":"udp","bind":"127.0.0.1","port":)" +
                                                               std::to_string(jp) + "}";
                                  m->addBuiltin(std::make_unique<UatRawInput>(io, defOf(uat_def), RecordOptions{},
                                                                              decodeOn()));
                                  m->addBuiltin(std::make_unique<skycued::ReadsbStreamInput>(io, defOf(json_def)));
                                  m->load();
                                  return m;
                              }));
    }
    template <class P> bool wait(P pred)
    {
        return eventually([&] { return pred(fx->stats()); });
    }
};

void sendUdp(uint16_t port, const std::string& s)
{
    asio::io_context io;
    udp::socket sock(io, udp::endpoint(udp::v4(), 0));
    sock.send_to(asio::buffer(s), udp::endpoint(asio::ip::make_address("127.0.0.1"), port));
}

}  // namespace

BOOST_AUTO_TEST_CASE(two_feeds_one_aircraft_tracks_once, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto f = loadUatFixture("uat-sample.txt.gz");
    // A downlink with a position, and its address.
    std::string line;
    uint32_t address = 0;
    for (const auto& l : f.down) {
        skycued::decode::UatDecoder d({}, "p");
        bool pos = false;
        d.onObservation([&](const skycued::TrackObservation& o) { pos = pos || o.lat.has_value(); });
        const auto fr = *uat::parseDownlinkLine(l);
        d.feed(fr, std::chrono::system_clock::now());
        if (pos) {
            line = l;
            address = fr.address();
            break;
        }
    }
    BOOST_REQUIRE(!line.empty());
    FeedsFx e;
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) {
        return s.inputs.size() == 2 && s.inputs[0].connected && s.inputs[1].connected;
    }));
    sendUdp(e.uat_port, line + "\n");
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) { return s.tracks_total == 1; }));
    char hex[8];
    std::snprintf(hex, sizeof hex, "%06x", static_cast<unsigned>(address));
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    char json[256];
    std::snprintf(json, sizeof json,
                  R"({"now":%.3f,"hex":"%s","type":"adsb_icao","alt_baro":18000,"gs":379.0,"track":329.67,"lat":37.887976,"lon":-76.261396,"seen_pos":0.1})",
                  now, hex);
    sendUdp(e.json_port, std::string(json) + "\n");
    BOOST_REQUIRE(e.wait([](const skycued::EngineStats& s) {
        return s.inputs.size() == 2 && s.inputs[0].observations >= 1 && s.inputs[1].observations >= 1;
    }));
    const auto s = e.fx->stats();
    BOOST_TEST(s.tracks_total == 1u);  // one aircraft, one track, whichever input spoke last
    BOOST_TEST(s.observations == s.inputs[0].observations + s.inputs[1].observations);
}
