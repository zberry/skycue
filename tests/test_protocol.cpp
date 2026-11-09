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
    BOOST_TEST(b[4] == 2);
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

BOOST_AUTO_TEST_CASE(track_golden_bytes_v1_layout)
{
    // Computed independently from the layout in docs/protocol.md, not from this encoder.
    const uint8_t v1[] = {
        0x53, 0x4B, 0x59, 0x43, 0x01, 0x02, 0x43, 0x00, 0xC3, 0xB2, 0xA1, 0x00,
        0x00, 0x01, 0x07, 0x00, 0x7B, 0x44, 0x8E, 0x99, 0xA1, 0x01, 0x00, 0x00,
        0x40, 0xAB, 0x2F, 0x17, 0xA8, 0x02, 0x15, 0xD2, 0xFF, 0x03, 0xB8, 0x88,
        0x00, 0x00, 0x50, 0xFB, 0xFF, 0xFF, 0x00, 0xC0, 0xE1, 0x43, 0x00, 0xA0,
        0x87, 0x43, 0x00, 0x00, 0xC0, 0xC4, 0x06, 0x55, 0x41, 0x4C, 0x31, 0x32,
        0x33, 0x04, 0x31, 0x32, 0x30, 0x30, 0x06, 0x4E, 0x31, 0x32, 0x33, 0x34,
        0x35, 0x08, 0x0A,
    };
    // The version-2 frame is the same bytes with the version byte 2, the payload length 2 longer
    // and the presence word widened to u32 (two zero bytes after the u16 at offset 24).
    std::vector<uint8_t> want(std::begin(v1), std::end(v1));
    want[4] = 2;
    want[6] = 0x45;
    want.insert(want.begin() + kHeaderSize + 26, {0x00, 0x00});
    std::vector<uint8_t> got = encodeTrackUpdate(fullTrack());
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
    BOOST_TEST(f.payload.size() == 4u + 1 + 1 + 1 + 1 + 8 + 4 + 4 + 4);
    BOOST_TEST(f.payload.size() == 28u);

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
    b[4] = 3;
    Frame f;
    size_t consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed) == DecodeStatus::bad_version));
    b[4] = 0;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed) == DecodeStatus::bad_version));
    BOOST_TEST(consumed == 0u);
}

BOOST_AUTO_TEST_CASE(unknown_bit_26_stops_reading)
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
    BOOST_REQUIRE(f.payload.size() == 29u); // 28 fixed + nic
    // Set presence bit 12 (known since version 2, so use 26) and append its unknown data after nic.
    f.payload[27] = static_cast<uint8_t>(f.payload[27] | (1u << (26 - 24)));
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

BOOST_AUTO_TEST_CASE(uncertainty_bit_10_roundtrip)
{
    Track in = fullTrack();
    in.alt_geom_uncertainty_ft = static_cast<uint16_t>(325);
    const Frame f = decodeOne(encodeTrackUpdate(in));
    // presence is a u16 at payload offset 24; bit 10 is bit 2 of its high byte (little endian)
    BOOST_TEST((f.payload[25] & 0x04) == 0x04);
    // the u16 value is the last field, little endian
    BOOST_REQUIRE(f.payload.size() > 2u);
    BOOST_TEST(f.payload[f.payload.size() - 2] == 0x45);  // 325 = 0x0145
    BOOST_TEST(f.payload[f.payload.size() - 1] == 0x01);
    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    checkEqual(in, out);
    BOOST_TEST((out.alt_geom_uncertainty_ft == std::optional<uint16_t>(325)));
}

BOOST_AUTO_TEST_CASE(old_reader_stops_at_bit_10)
{
    // A reader that knows bits 0-9 stops at the first unknown set bit: bit 10 comes after
    // every field it knows, so it reads all of them and ignores the rest. Shown by hand:
    // the payload with bit 10 is the payload without it, plus the presence bit and the
    // two trailing bytes, nothing moved.
    const Track base = fullTrack();
    Track with = base;
    with.alt_geom_uncertainty_ft = static_cast<uint16_t>(125);
    const Frame fb = decodeOne(encodeTrackUpdate(base));
    const Frame fw = decodeOne(encodeTrackUpdate(with));
    BOOST_REQUIRE(fw.payload.size() == fb.payload.size() + 2);
    std::vector<uint8_t> expect = fb.payload;
    expect[25] = static_cast<uint8_t>(expect[25] | 0x04);
    expect.push_back(125);
    expect.push_back(0);
    BOOST_TEST(fw.payload == expect, boost::test_tools::per_element());
    // bits 0-9 decode identically from either; the new one is simply absent without it
    Track a, b;
    BOOST_TEST(decodeTrackUpdate(fb, a));
    BOOST_TEST(decodeTrackUpdate(fw, b));
    b.alt_geom_uncertainty_ft.reset();
    checkEqual(a, b);
}

BOOST_AUTO_TEST_CASE(no_bit_10_for_reported)
{
    Track t = fullTrack();  // a reported alt_geom_ft, no uncertainty
    const Frame f = decodeOne(encodeTrackUpdate(t));
    BOOST_TEST((f.payload[25] & 0x04) == 0);
    // and the golden bytes of track_golden_bytes are unchanged by the new field
    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    BOOST_TEST(!out.alt_geom_uncertainty_ft.has_value());
}

BOOST_AUTO_TEST_CASE(uncertainty_saturates_at_65535)
{
    Track t = fullTrack();
    t.alt_geom_uncertainty_ft = static_cast<uint16_t>(65535);
    Track out;
    BOOST_TEST(decodeTrackUpdate(decodeOne(encodeTrackUpdate(t)), out));
    BOOST_TEST((out.alt_geom_uncertainty_ft == std::optional<uint16_t>(65535)));
}

namespace {

Track v2Track()
{
    Track t = fullTrack();
    t.acas_ra = AcasRaWire{0x2A5B, 0x4F}; // active, rac 15, rat, mte
    t.heading_mag_deg = 271.5f;
    t.heading_true_deg = 270.25f;
    t.ias_kt = static_cast<uint16_t>(250);
    t.tas_kt = static_cast<uint16_t>(430);
    t.mach = 0.78f;
    t.roll_deg = -12.5f;
    t.sel_alt_ft = -100;
    t.sel_alt_fms_ft = 36000;
    t.qnh_mb = 1013.25f;
    t.sel_heading_deg = 90.0f;
    t.nav_modes = static_cast<uint8_t>(0x2B);
    t.track_rate_dps = -3.0f;
    t.emergency = static_cast<uint8_t>(6);
    t.category = std::string("A3");
    return t;
}

void checkV2(const Track& a, const Track& b)
{
    checkEqual(a, b);
    BOOST_TEST((a.heading_mag_deg == b.heading_mag_deg));
    BOOST_TEST((a.heading_true_deg == b.heading_true_deg));
    BOOST_TEST((a.ias_kt == b.ias_kt));
    BOOST_TEST((a.tas_kt == b.tas_kt));
    BOOST_TEST((a.mach == b.mach));
    BOOST_TEST((a.roll_deg == b.roll_deg));
    BOOST_TEST((a.sel_alt_ft == b.sel_alt_ft));
    BOOST_TEST((a.sel_alt_fms_ft == b.sel_alt_fms_ft));
    BOOST_TEST((a.qnh_mb == b.qnh_mb));
    BOOST_TEST((a.sel_heading_deg == b.sel_heading_deg));
    BOOST_TEST((a.nav_modes == b.nav_modes));
    BOOST_TEST((a.track_rate_dps == b.track_rate_dps));
    BOOST_TEST((a.emergency == b.emergency));
    BOOST_TEST((a.category == b.category));
    BOOST_REQUIRE(a.acas_ra.has_value() == b.acas_ra.has_value());
    if (a.acas_ra) {
        BOOST_TEST(a.acas_ra->ara == b.acas_ra->ara);
        BOOST_TEST(int(a.acas_ra->flags) == int(b.acas_ra->flags));
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(v2_round_trip_every_new_field)
{
    const Track in = v2Track();
    Track out;
    BOOST_TEST(decodeTrackUpdate(decodeOne(encodeTrackUpdate(in)), out));
    checkV2(in, out);
    // each field on its own, so a wrong field order shows up
    auto alone = [](auto set) {
        Track t = fullTrack();
        set(t);
        Track o;
        BOOST_TEST(decodeTrackUpdate(decodeOne(encodeTrackUpdate(t)), o));
        checkV2(t, o);
    };
    alone([](Track& t) { t.acas_ra = AcasRaWire{1, 0}; });
    alone([](Track& t) { t.heading_mag_deg = 1.5f; });
    alone([](Track& t) { t.ias_kt = static_cast<uint16_t>(65535); });
    alone([](Track& t) { t.emergency = static_cast<uint8_t>(0); });
    alone([](Track& t) { t.category = std::string("C1"); });
}

BOOST_AUTO_TEST_CASE(v2_presence_is_u32_at_offset_24)
{
    Track t = fullTrack();
    t.category = std::string("A3");   // bit 25
    t.alt_geom_uncertainty_ft = static_cast<uint16_t>(5); // bit 10
    const Frame f = decodeOne(encodeTrackUpdate(t));
    BOOST_TEST(f.version == 2);
    const uint32_t presence = uint32_t(f.payload[24]) | (uint32_t(f.payload[25]) << 8) |
                              (uint32_t(f.payload[26]) << 16) | (uint32_t(f.payload[27]) << 24);
    BOOST_TEST(presence == (0x3FFu | (1u << 10) | (1u << 25)));
    // category is the last field: length 2, "A3"
    BOOST_TEST(f.payload[f.payload.size() - 3] == 2);
    BOOST_TEST(f.payload[f.payload.size() - 2] == 'A');
    BOOST_TEST(f.payload[f.payload.size() - 1] == '3');
}

BOOST_AUTO_TEST_CASE(v1_reader_rejects_v2)
{
    std::vector<uint8_t> b = encodeTrackUpdate(v2Track());
    Frame f;
    size_t consumed = 99;
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed, 1) == DecodeStatus::bad_version));
    BOOST_TEST(consumed == 0u);
    BOOST_TEST((decodeFrame(b.data(), b.size(), f, consumed, 2) == DecodeStatus::ok));
}

BOOST_AUTO_TEST_CASE(v2_reader_reads_v1)
{
    // A frame laid out as version 1 (u16 presence) decodes with the old fields; a version 1
    // frame never has bits 11+, and what a u32 reading would take as high bits is payload.
    const uint8_t v1[] = {
        0x53, 0x4B, 0x59, 0x43, 0x01, 0x02, 0x43, 0x00, 0xC3, 0xB2, 0xA1, 0x00,
        0x00, 0x01, 0x07, 0x00, 0x7B, 0x44, 0x8E, 0x99, 0xA1, 0x01, 0x00, 0x00,
        0x40, 0xAB, 0x2F, 0x17, 0xA8, 0x02, 0x15, 0xD2, 0xFF, 0x03, 0xB8, 0x88,
        0x00, 0x00, 0x50, 0xFB, 0xFF, 0xFF, 0x00, 0xC0, 0xE1, 0x43, 0x00, 0xA0,
        0x87, 0x43, 0x00, 0x00, 0xC0, 0xC4, 0x06, 0x55, 0x41, 0x4C, 0x31, 0x32,
        0x33, 0x04, 0x31, 0x32, 0x30, 0x30, 0x06, 0x4E, 0x31, 0x32, 0x33, 0x34,
        0x35, 0x08, 0x0A,
    };
    Frame f;
    size_t consumed = 0;
    BOOST_REQUIRE(decodeFrame(v1, sizeof v1, f, consumed) == DecodeStatus::ok);
    BOOST_TEST(f.version == 1);
    Track out;
    BOOST_TEST(decodeTrackUpdate(f, out));
    checkEqual(fullTrack(), out);
    BOOST_TEST(!out.acas_ra.has_value());
    BOOST_TEST(!out.category.has_value());

    // a v1 frame whose u16 presence has bit 11 or above set: unknown to version 1, stop reading
    std::vector<uint8_t> bad(v1, v1 + sizeof v1);
    bad[8 + 25] = static_cast<uint8_t>(bad[8 + 25] | 0x08);
    BOOST_REQUIRE(decodeFrame(bad.data(), bad.size(), f, consumed) == DecodeStatus::ok);
    BOOST_TEST(decodeTrackUpdate(f, out));
    BOOST_TEST(!out.acas_ra.has_value());
}

BOOST_AUTO_TEST_CASE(emergency_names)
{
    BOOST_TEST(std::string(emergencyName(0)) == "none");
    BOOST_TEST(std::string(emergencyName(3)) == "minimum_fuel");
    BOOST_TEST(std::string(emergencyName(6)) == "downed_aircraft");
    BOOST_TEST(std::string(emergencyName(7)) == "unknown");
    BOOST_TEST((emergencyCode("no_comms") == std::optional<uint8_t>(4)));
    BOOST_TEST(!emergencyCode("bogus").has_value());
}

BOOST_AUTO_TEST_CASE(v2_full_size_matches_docs)
{
    Track t = v2Track();
    t.callsign = std::string(8, 'C');
    t.squawk = std::string(4, '7');
    t.registration = std::string(16, 'R');
    t.alt_geom_uncertainty_ft = static_cast<uint16_t>(9);
    BOOST_TEST(decodeOne(encodeTrackUpdate(t)).payload.size() == 131u); // docs/protocol.md
}
