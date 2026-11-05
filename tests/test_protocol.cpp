#define BOOST_TEST_MODULE skycue_protocol
#include <boost/test/unit_test.hpp>

#include <skycue/protocol.hpp>
#include <skycue/types.hpp>
#include <skycue/version.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace skycue;

namespace {

Track fullTrack()
{
    Track t;
    t.id = TrackId{0xA1B2C3, AddressType::icao};
    t.source = Source::uat_978;
    t.position_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(1793577600123LL));
    t.lat_deg = 38.9;
    t.lon_deg = -77.0375;
    t.on_ground = true;
    t.keyframe = true;
    t.alt_is_geom = true;
    t.alt_baro_ft = 35000;
    t.alt_geom_ft = -1200;
    t.ground_speed_kt = 451.5f;
    t.track_deg = 271.25f;
    t.vertical_rate_fpm = -1536.0f;
    t.callsign = std::string("UAL123");
    t.squawk = std::string("1200");
    t.registration = std::string("N12345");
    t.nic = static_cast<uint8_t>(8);
    t.nac_p = static_cast<uint8_t>(10);
    return t;
}

void checkEqual(const Track& a, const Track& b)
{
    BOOST_TEST(a.id.address == b.id.address);
    BOOST_TEST(static_cast<int>(a.id.type) == static_cast<int>(b.id.type));
    BOOST_TEST(static_cast<int>(a.source) == static_cast<int>(b.source));
    BOOST_TEST(a.position_time.time_since_epoch().count() == b.position_time.time_since_epoch().count());
    BOOST_TEST(a.lat_deg == b.lat_deg);
    BOOST_TEST(a.lon_deg == b.lon_deg);
    BOOST_TEST(a.on_ground == b.on_ground);
    BOOST_TEST(a.keyframe == b.keyframe);
    BOOST_TEST(a.alt_is_geom == b.alt_is_geom);
    BOOST_TEST((a.alt_baro_ft == b.alt_baro_ft));
    BOOST_TEST((a.alt_geom_ft == b.alt_geom_ft));
    BOOST_TEST((a.ground_speed_kt == b.ground_speed_kt));
    BOOST_TEST((a.track_deg == b.track_deg));
    BOOST_TEST((a.vertical_rate_fpm == b.vertical_rate_fpm));
    BOOST_TEST((a.callsign == b.callsign));
    BOOST_TEST((a.squawk == b.squawk));
    BOOST_TEST((a.registration == b.registration));
    BOOST_TEST((a.nic == b.nic));
    BOOST_TEST((a.nac_p == b.nac_p));
}

int32_t readI32(const std::vector<uint8_t>& v, size_t off)
{
    uint32_t u = uint32_t(v[off]) | (uint32_t(v[off + 1]) << 8) | (uint32_t(v[off + 2]) << 16) |
                 (uint32_t(v[off + 3]) << 24);
    return static_cast<int32_t>(u);
}

Frame decodeOne(const std::vector<uint8_t>& bytes)
{
    Frame f;
    size_t consumed = 0;
    BOOST_REQUIRE(decodeFrame(bytes.data(), bytes.size(), f, consumed) == DecodeStatus::ok);
    BOOST_TEST(consumed == bytes.size());
    return f;
}

} // namespace

BOOST_AUTO_TEST_CASE(header_layout)
{
    std::vector<uint8_t> b = encodeHeartbeat(Heartbeat{0x0102030405060708ULL});
    BOOST_REQUIRE(b.size() == kHeaderSize + 8);
    BOOST_TEST(b[0] == 'S');
    BOOST_TEST(b[1] == 'K');
    BOOST_TEST(b[2] == 'Y');
    BOOST_TEST(b[3] == 'C');
    BOOST_TEST(b[4] == 1);
    BOOST_TEST(b[5] == 4);
    BOOST_TEST(b[6] == 8);
    BOOST_TEST(b[7] == 0);
    // Payload is little-endian.
    BOOST_TEST(b[8] == 0x08);
    BOOST_TEST(b[15] == 0x01);

    Heartbeat hb{};
    BOOST_TEST(decodeHeartbeat(decodeOne(b), hb));
    BOOST_TEST(hb.sender_time_ms == 0x0102030405060708ULL);
}

BOOST_AUTO_TEST_CASE(track_roundtrip_all_fields)
{
    Track in = fullTrack();
    std::vector<uint8_t> b = encodeTrackUpdate(in);
    Frame f = decodeOne(b);
    BOOST_TEST(static_cast<int>(f.type) == static_cast<int>(FrameType::track_update));
    // lat i32 sits after address(4) type(1) source(1) flags(1) reserved(1) time(8).
    BOOST_TEST(readI32(f.payload, 16) == 389000000);
    BOOST_TEST(readI32(f.payload, 20) == -770375000);
    // flags: keyframe | on_ground | alt_is_geom
    BOOST_TEST(f.payload[6] == 0x07);
    BOOST_TEST(f.payload[7] == 0);

    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    checkEqual(in, out);
}

BOOST_AUTO_TEST_CASE(track_golden_bytes)
{
    // Computed independently from the layout in docs/protocol.md, not from this encoder.
    const uint8_t expected[] = {
        0x53, 0x4B, 0x59, 0x43, 0x01, 0x02, 0x43, 0x00, 0xC3, 0xB2, 0xA1, 0x00,
        0x00, 0x01, 0x07, 0x00, 0x7B, 0x44, 0x8E, 0x99, 0xA1, 0x01, 0x00, 0x00,
        0x40, 0xAB, 0x2F, 0x17, 0xA8, 0x02, 0x15, 0xD2, 0xFF, 0x03, 0xB8, 0x88,
        0x00, 0x00, 0x50, 0xFB, 0xFF, 0xFF, 0x00, 0xC0, 0xE1, 0x43, 0x00, 0xA0,
        0x87, 0x43, 0x00, 0x00, 0xC0, 0xC4, 0x06, 0x55, 0x41, 0x4C, 0x31, 0x32,
        0x33, 0x04, 0x31, 0x32, 0x30, 0x30, 0x06, 0x4E, 0x31, 0x32, 0x33, 0x34,
        0x35, 0x08, 0x0A,
    };
    std::vector<uint8_t> got = encodeTrackUpdate(fullTrack());
    std::vector<uint8_t> want(std::begin(expected), std::end(expected));
    BOOST_TEST(got == want, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(huge_time_clamped_no_ub)
{
    Frame f = decodeOne(encodeTrackUpdate(fullTrack()));
    for (size_t i = 8; i < 16; ++i)
        f.payload[i] = 0xFF; // position_time_ms = 0xFFFFFFFFFFFFFFFF
    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(out.position_time.time_since_epoch()).count();
    BOOST_TEST(ms == static_cast<int64_t>(kMaxPositionTimeMs));
    BOOST_TEST((out.nac_p == std::optional<uint8_t>(10)));
}

BOOST_AUTO_TEST_CASE(track_roundtrip_minimal)
{
    Track in;
    in.id = TrackId{0x00FFEE, AddressType::non_icao};
    in.source = Source::adsb_1090;
    in.position_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
    in.lat_deg = -33.5;
    in.lon_deg = 151.25;
    in.on_ground = false;
    in.keyframe = false;
    in.alt_is_geom = false;

    std::vector<uint8_t> b = encodeTrackUpdate(in);
    Frame f = decodeOne(b);
    BOOST_TEST(f.payload.size() == 4u + 1 + 1 + 1 + 1 + 8 + 4 + 4 + 2);
    BOOST_TEST(f.payload.size() == 26u);

    Track out = fullTrack(); // decode must clear optionals that are absent
    BOOST_TEST(decodeTrackUpdate(f, out));
    checkEqual(in, out);
}

BOOST_AUTO_TEST_CASE(truncated_stream_needs_more)
{
    std::vector<uint8_t> b = encodeTrackUpdate(fullTrack());
    Frame f;
    size_t consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), kHeaderSize, f, consumed) == DecodeStatus::need_more));
    BOOST_TEST(consumed == 0u);

    consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), 3, f, consumed) == DecodeStatus::need_more));
    BOOST_TEST(consumed == 0u);

    consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), b.size() - 1, f, consumed) == DecodeStatus::need_more));
    BOOST_TEST(consumed == 0u);

    consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), 0, f, consumed) == DecodeStatus::need_more));
    BOOST_TEST(consumed == 0u);
}

BOOST_AUTO_TEST_CASE(two_frames_in_one_buffer)
{
    std::vector<uint8_t> a = encodeHeartbeat(Heartbeat{42});
    std::vector<uint8_t> c = encodeTrackDrop(TrackDrop{TrackId{0x123456, AddressType::icao}, DropReason::timeout});
    std::vector<uint8_t> buf = a;
    buf.insert(buf.end(), c.begin(), c.end());

    Frame f;
    size_t consumed1 = 0;
    BOOST_REQUIRE(decodeFrame(buf.data(), buf.size(), f, consumed1) == DecodeStatus::ok);
    BOOST_TEST(consumed1 == a.size());
    BOOST_TEST(static_cast<int>(f.type) == static_cast<int>(FrameType::heartbeat));
    Heartbeat hb{};
    BOOST_TEST(decodeHeartbeat(f, hb));
    BOOST_TEST(hb.sender_time_ms == 42u);

    size_t consumed2 = 0;
    BOOST_REQUIRE(decodeFrame(buf.data() + consumed1, buf.size() - consumed1, f, consumed2) == DecodeStatus::ok);
    BOOST_TEST(consumed2 == c.size());
    BOOST_TEST(consumed1 + consumed2 == buf.size());
    TrackDrop d{};
    BOOST_TEST(decodeTrackDrop(f, d));
    BOOST_TEST(d.id.address == 0x123456u);
}

BOOST_AUTO_TEST_CASE(bad_magic)
{
    std::vector<uint8_t> b = encodeHeartbeat(Heartbeat{1});
    b[2] = 'X';
    Frame f;
    size_t consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed) == DecodeStatus::bad_magic));
    BOOST_TEST(consumed == 0u);
}

BOOST_AUTO_TEST_CASE(bad_version)
{
    std::vector<uint8_t> b = encodeHeartbeat(Heartbeat{1});
    b[4] = 2;
    Frame f;
    size_t consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed) == DecodeStatus::bad_version));
    BOOST_TEST(consumed == 0u);
}

BOOST_AUTO_TEST_CASE(unknown_presence_bit_keeps_known_fields)
{
    Track in;
    in.id = TrackId{0xABCDEF, AddressType::icao};
    in.source = Source::adsb_1090;
    in.position_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(5000));
    in.lat_deg = 1.0;
    in.lon_deg = 2.0;
    in.on_ground = false;
    in.keyframe = true;
    in.alt_is_geom = false;
    in.nic = static_cast<uint8_t>(7);

    Frame f = decodeOne(encodeTrackUpdate(in));
    BOOST_REQUIRE(f.payload.size() == 27u); // 26 fixed + nic
    // Set presence bit 12 (unknown to this decoder) and append its unknown data after nic.
    f.payload[25] = static_cast<uint8_t>(f.payload[25] | (1u << (12 - 8)));
    f.payload.push_back(0xDE);
    f.payload.push_back(0xAD);

    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    BOOST_TEST((out.nic == std::optional<uint8_t>(7)));
    BOOST_TEST(!out.nac_p.has_value());
    BOOST_TEST(out.id.address == 0xABCDEFu);
    BOOST_TEST(out.keyframe);
}

BOOST_AUTO_TEST_CASE(udp_size_bound)
{
    Track t = fullTrack();
    t.callsign = std::string(8, 'C');
    t.squawk = std::string(4, '7');
    t.registration = std::string(16, 'R');
    std::vector<uint8_t> b = encodeTrackUpdate(t);
    BOOST_TEST(b.size() <= kMaxUdpPayload);

    // Over-long strings are truncated to the wire limits on encode.
    Track longer = t;
    longer.callsign = std::string(40, 'C');
    longer.squawk = std::string(10, '7');
    longer.registration = std::string(40, 'R');
    BOOST_TEST(encodeTrackUpdate(longer).size() == b.size());
}

BOOST_AUTO_TEST_CASE(truncated_payload_rejected)
{
    std::vector<uint8_t> b = encodeTrackUpdate(fullTrack());
    Frame full = decodeOne(b);
    for (size_t n = 0; n < full.payload.size(); ++n) {
        Frame f{full.type, std::vector<uint8_t>(full.payload.begin(), full.payload.begin() + static_cast<long>(n))};
        Track out;
        BOOST_TEST(!decodeTrackUpdate(f, out), "prefix of " << n << " bytes accepted");
    }
    Frame hf = decodeOne(encodeHello(Hello{1000, 2000, "skycued"}));
    hf.payload.pop_back();
    Hello h;
    BOOST_TEST(!decodeHello(hf, h));
}

BOOST_AUTO_TEST_CASE(unknown_frame_type_is_skippable)
{
    std::vector<uint8_t> b = encodeHeartbeat(Heartbeat{1});
    b[5] = 200;
    Frame f;
    size_t consumed = 0;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed) == DecodeStatus::ok));
    BOOST_TEST(consumed == b.size());
    Heartbeat hb{};
    BOOST_TEST(!decodeHeartbeat(f, hb));
}

BOOST_AUTO_TEST_CASE(drop_roundtrip)
{
    TrackDrop in{TrackId{0x7C0001, AddressType::tisb_trackfile}, DropReason::stale};
    Frame f = decodeOne(encodeTrackDrop(in));
    BOOST_TEST(static_cast<int>(f.type) == static_cast<int>(FrameType::track_drop));
    BOOST_TEST(f.payload.size() == 6u);
    TrackDrop out{};
    BOOST_TEST(decodeTrackDrop(f, out));
    BOOST_TEST((out.id == in.id));
    BOOST_TEST(static_cast<int>(out.reason) == static_cast<int>(DropReason::stale));
}

BOOST_AUTO_TEST_CASE(hello_roundtrip)
{
    Hello in{1000, 2500, "skycued 0.1.0"};
    Frame f = decodeOne(encodeHello(in));
    BOOST_TEST(static_cast<int>(f.type) == static_cast<int>(FrameType::hello));
    Hello out;
    BOOST_TEST(decodeHello(f, out));
    BOOST_TEST(out.keyframe_interval_ms == 1000u);
    BOOST_TEST(out.heartbeat_interval_ms == 2500u);
    BOOST_TEST(out.sender == "skycued 0.1.0");
}

BOOST_AUTO_TEST_CASE(track_id_ordering_and_hash)
{
    TrackId a{0x10, AddressType::icao};
    TrackId b{0x10, AddressType::non_icao};
    TrackId c{0x11, AddressType::icao};
    BOOST_TEST((a == TrackId{0x10, AddressType::icao}));
    BOOST_TEST(!(a == b));
    BOOST_TEST((a < b));
    BOOST_TEST((b < c));
    BOOST_TEST(!(c < a));
    BOOST_TEST(std::hash<TrackId>{}(a) == std::hash<TrackId>{}(TrackId{0x10, AddressType::icao}));
}

BOOST_AUTO_TEST_CASE(version_header_matches_cmake)
{
    BOOST_TEST(std::string(skycue::kVersionString) == SKYCUE_CMAKE_VERSION);
    BOOST_TEST(std::string(SKYCUE_VERSION_STRING) == SKYCUE_CMAKE_VERSION);
}
