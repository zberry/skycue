#define BOOST_TEST_MODULE skycued_ping_decode
#include <boost/test/unit_test.hpp>

#include "coding/bits.hpp"
#include "decode_test_support.hpp"
#include "engine_test_support.hpp"
#include "hub/raw_tap.hpp"
#include "inputs/beast_raw_input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/ping_json.hpp"
#include "inputs/ping_json_input.hpp"
#include "json_lite.hpp"
#include "uat_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace enginetest;
using skycued::InputType;
using skycued::PingJsonInput;
using skycued::RawDecodeOptions;
using skycued::testing::Bytes;
namespace testing = skycued::testing;

namespace {

using sys = std::chrono::system_clock;

const skycued::decode::cpr::Position kP0{52.2572, 3.9193};
constexpr uint32_t kAddr = 0x4840D6;

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

std::string hexOf(const Bytes& b) { return skycued::coding::toHex(b, false); }

Bytes air1090(bool odd, int32_t alt = 38000, uint32_t addr = kAddr)
{
    using namespace testing;
    return df17(addr, airbornePosition(11, encodeCpr(kP0, odd, false), alt));
}

Bytes uatLong(unsigned qualifier = 0, uint32_t address = 0xA66EF1)
{
    testing::UatSpec s;
    s.type = 1;
    s.qualifier = qualifier;
    s.address = address;
    s.lat = 52.1;
    s.lon = 4.6;
    s.alt_ft = 5000;
    s.aux_alt_ft = 5150;
    s.nic = 8;
    s.ns_kt = 100;
    s.ew_kt = 100;
    s.call = "KLM123";
    s.emitter = 3;
    return testing::encodeUat(s);
}

// A datagram with one aircraft: icao, a deliberately wrong decoded position and callsign (they must
// be ignored in decode mode) and the rawData.
std::string aircraft(const std::string& hex, const std::string& extra = "")
{
    return R"({"icaoAddress":"4840D6","latDD":10.5,"lonDD":20.5,"altitudeMM":3048000,"callsign":"DEVICE1",)" + extra +
           R"("detail":{"rawData":")" + hex + R"("}})";
}

std::string aircraftNoRaw()
{
    return R"({"icaoAddress":"4840D6","latDD":10.5,"lonDD":20.5,"callsign":"DEVICE1"})";
}

struct Rig {
    IoThread t;
    std::atomic<int64_t> now_ms{1'793'620'800'000};  // 2026-11-02 12:00:00 UTC
    std::mutex m;
    std::vector<skycued::TrackObservation> obs;
    std::unique_ptr<skycued::Input> in;
    asio::io_context cio;
    std::unique_ptr<asio::ip::tcp::socket> sock;
    uint64_t sent = 0;

    sys::time_point clock() const { return sys::time_point(std::chrono::milliseconds(now_ms.load())); }

    void run(const std::string& extra, bool with_clock = true, std::optional<std::pair<double, double>> ref = {})
    {
        const uint16_t port = freeTcpPort();
        auto def = defOf(R"({"id":"ps","type":"pingstation_json","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                         std::to_string(port) + extra + "}");
        RawDecodeOptions opt;
        if (with_clock)
            opt.clock = [this] { return clock(); };
        opt.reference = ref;
        t.call([&] {
            in = std::make_unique<PingJsonInput>(t.io, def, skycued::RecordOptions{}, opt);
            in->onObservation([this](const skycued::TrackObservation& o) {
                std::lock_guard<std::mutex> lock(m);
                obs.push_back(o);
            });
            in->start();
        });
        sock = std::make_unique<asio::ip::tcp::socket>(cio);
        const auto until = steady::now() + kWait;
        for (;;) {
            boost::system::error_code ec;
            sock->connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
            if (!ec)
                break;
            if (steady::now() > until)
                throw std::runtime_error("cannot connect");
            sock->close();
            std::this_thread::sleep_for(10ms);
        }
    }
    // One datagram (one JSON line) at the clock's `at_ms`; returns when the input counted it.
    void send(const std::string& json, int64_t at_ms)
    {
        now_ms = at_ms;
        asio::write(*sock, asio::buffer(json + "\n"));
        ++sent;
        BOOST_REQUIRE(eventually([&] { return state().counters.messages >= sent; }));
    }
    ~Rig()
    {
        try {
            t.call([&] { in.reset(); });
        } catch (const std::exception&) {
        }
    }
    skycued::InputState state() { return t.call([&] { return in->state(); }); }
    std::vector<skycued::TrackObservation> observations()
    {
        std::lock_guard<std::mutex> lock(m);
        return obs;
    }
};

constexpr int64_t kT0 = 1'793'620'800'000;

}  // namespace

// ---- device mode ------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(device_mode_observations_unchanged)
{
    const std::string text = aircraft(hexOf(air1090(false)), R"("squawk":1200,)");
    Rig rig;
    rig.run("");  // source defaults to device
    rig.send(text, kT0);
    const auto got = rig.observations();
    const auto want = skycued::parsePingDatagram(text, skycued::PingContext{rig.clock(), skycued::TimeMode::utc});
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_REQUIRE_EQUAL(want.observations.size(), 1u);
    const auto& g = got[0];
    const auto& w = want.observations[0];
    BOOST_TEST((g.id == w.id));
    BOOST_TEST(g.lat.value() == w.lat.value());
    BOOST_TEST(g.lon.value() == w.lon.value());
    BOOST_TEST(g.alt_baro_ft.value_or(-1) == w.alt_baro_ft.value_or(-1));
    BOOST_TEST(g.callsign.value_or("") == "DEVICE1");
    BOOST_TEST(g.squawk.value_or("") == w.squawk.value_or("?"));
    BOOST_TEST((g.received_time == w.received_time));
    BOOST_TEST(g.raw_line == w.raw_line);
    BOOST_TEST(g.input_id == "ps");
    const auto s = rig.state();
    BOOST_TEST(!s.decoding);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.no_raw == 0u);
}

// ---- decode mode ------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(decode_mode_1090_frame_emits_decoded_position)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraft(hexOf(air1090(false))), kT0);
    rig.send(aircraft(hexOf(air1090(true))), kT0 + 2000);
    const auto got = rig.observations();
    BOOST_REQUIRE(!got.empty());
    const auto& o = got.back();
    BOOST_REQUIRE(o.lat && o.lon);
    BOOST_TEST(std::abs(*o.lat - kP0.lat) < 0.01);
    BOOST_TEST(std::abs(*o.lon - kP0.lon) < 0.01);
    BOOST_TEST(o.alt_baro_ft.value_or(-1) == 38000);
    BOOST_TEST(o.input_id == "ps");
    const auto s = rig.state();
    BOOST_TEST(s.decoding);
    BOOST_TEST(s.counters.frames == 2u);
    BOOST_TEST(s.counters.decoded >= 1u);
    BOOST_TEST(s.counters.cpr_pending >= 1u);
    BOOST_TEST(s.counters.raw_duplicates == 0u);
}

BOOST_AUTO_TEST_CASE(decode_mode_uat_long_frame_emits_both_altitudes)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraft(hexOf(uatLong())), kT0);
    const auto got = rig.observations();
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_TEST(got[0].alt_baro_ft.value_or(-1) == 5000);
    BOOST_TEST(got[0].alt_geom_ft.value_or(-1) == 5150);
    BOOST_TEST(got[0].geom_minus_baro_ft.value_or(-1) == 150);
    BOOST_TEST((got[0].source == skycue::Source::uat_978));
    BOOST_TEST(got[0].callsign.value_or("") == "KLM123");
}

BOOST_AUTO_TEST_CASE(decode_mode_ignores_device_fields_except_time)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    // The GPS counter time of arrival + 1 s (the counter counts from the GPS epoch, 1980-01-06).
    const auto want = rig.clock() + 1s;
    const int64_t gps_s = std::chrono::duration_cast<std::chrono::seconds>(want.time_since_epoch()).count() - 315964800;
    const std::string stamp = R"("utcSync":1,"timeOfReceptionS":)" + std::to_string(gps_s) + R"(,"timeOfReceptionNS":0,)";
    rig.send(aircraft(hexOf(uatLong()), stamp), kT0);
    const auto got = rig.observations();
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_TEST(got[0].lat.value() != 10.5);
    BOOST_TEST(std::abs(*got[0].lat - 52.1) < 0.001);
    BOOST_TEST(got[0].callsign.value_or("") == "KLM123");
    BOOST_TEST(got[0].alt_baro_ft.value_or(-1) == 5000);  // not the device's 3048000 mm
    const auto dt = std::chrono::duration<double>(got[0].received_time - rig.clock()).count();
    BOOST_TEST(dt > 0.5);  // the counter time (arrival + 1 s), not arrival
    BOOST_TEST(dt < 1.5);
}

BOOST_AUTO_TEST_CASE(ps_duplicate_within_1s_skipped)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    const std::string d = aircraft(hexOf(uatLong()));
    rig.send(d, kT0);
    rig.send(d, kT0 + 400);
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 1u);
    BOOST_TEST(s.counters.raw_duplicates == 1u);
    BOOST_TEST(rig.observations().size() == 1u);
}

BOOST_AUTO_TEST_CASE(ps_same_frame_after_1s_decoded)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    const std::string d = aircraft(hexOf(uatLong()));
    rig.send(d, kT0);
    rig.send(d, kT0 + 1000);
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 2u);
    BOOST_TEST(s.counters.raw_duplicates == 0u);
}

BOOST_AUTO_TEST_CASE(ps_snapshot_repeating_frames_decoded_once_per_second)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    const std::string d = aircraft(hexOf(uatLong()));
    for (int i = 0; i < 11; ++i)  // every 0.4 s for 4 s
        rig.send(d, kT0 + i * 400);
    const auto s = rig.state();
    // Decoded at 0, 1.2, 2.4, 3.6 s: the memory is the last decoded one, not the last seen.
    BOOST_TEST(s.counters.frames == 4u);
    BOOST_TEST(s.counters.raw_duplicates == 7u);
}

BOOST_AUTO_TEST_CASE(ps_mixed_bands_in_one_datagram)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    const std::string text = R"({"aircraft":[)" + aircraft(hexOf(air1090(false))) + "," + aircraft(hexOf(uatLong())) + "]}";
    rig.send(text, kT0);
    const auto s = rig.state();
    BOOST_TEST(s.counters.frames == 2u);
    BOOST_TEST(s.counters.no_raw == 0u);
    BOOST_TEST(s.counters.cpr_pending == 1u);  // the 1090 frame waits for its partner
    BOOST_TEST(rig.observations().size() == 1u);  // the UAT frame
}

BOOST_AUTO_TEST_CASE(ps_bad_length_counts_raw_unknown_len)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraft("5d4840d6202c"), kT0);  // 6 bytes
    const auto s = rig.state();
    BOOST_TEST(s.counters.raw_unknown_len == 1u);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(s.counters.no_raw == 0u);
    BOOST_TEST(rig.observations().empty());
}

BOOST_AUTO_TEST_CASE(ps_no_raw_counts_no_raw)
{
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraftNoRaw(), kT0);
    const auto s = rig.state();
    BOOST_TEST(s.counters.no_raw == 1u);
    BOOST_TEST(s.counters.frames == 0u);
    BOOST_TEST(rig.observations().empty());  // the device's decoded fields are not used
}

BOOST_AUTO_TEST_CASE(ps_uat_18_bytes_type_1_unknown_type)
{
    testing::UatSpec s;
    s.type = 1;
    s.length = 18;  // a short frame with a long-frame type
    s.lat = 52.1;
    s.lon = 4.6;
    s.nic = 8;
    s.alt_ft = 5000;
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraft(hexOf(testing::encodeUat(s))), kT0);
    const auto st = rig.state();
    BOOST_TEST(st.counters.frames == 1u);
    BOOST_TEST(st.counters.unknown_type == 1u);
    BOOST_TEST(rig.observations().empty());
}

BOOST_AUTO_TEST_CASE(ps_reference_from_status_position)
{
    // One CPR frame alone decodes only against a reference. No reference at first: no position.
    Rig rig;
    rig.run(R"(,"source":"decode")");
    rig.send(aircraft(hexOf(air1090(false))), kT0);
    BOOST_TEST(rig.observations().empty());
    // The station reports its position near the aircraft: the next odd frame alone resolves.
    rig.send(R"({"status":{"pingStationLatDD":52.0,"pingStationLonDD":4.0}})", kT0 + 100);
    rig.send(aircraft(hexOf(air1090(true, 38000, 0x4840D7))), kT0 + 200);
    const auto got = rig.observations();
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_TEST(std::abs(*got[0].lat - kP0.lat) < 0.01);
    BOOST_TEST(std::abs(*got[0].lon - kP0.lon) < 0.01);
}

BOOST_AUTO_TEST_CASE(ps_reference_falls_back_to_input_reference)
{
    Rig rig;
    rig.run(R"(,"source":"decode")", true, std::make_pair(52.0, 4.0));
    rig.send(aircraft(hexOf(air1090(false))), kT0);
    BOOST_REQUIRE_EQUAL(rig.observations().size(), 1u);
}
