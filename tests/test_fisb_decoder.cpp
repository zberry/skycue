// FisbDecoder end to end, and its integration in the Engine, Recorder and
// replay (design §5, §10; phase-4 Task 4).

#define BOOST_TEST_MODULE fisb_decoder
#include <boost/test/unit_test.hpp>

#include "restricted_fixtures.hpp"

#include "gz_line_reader.hpp"
#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"
#include "fisb/segment_assembler.hpp"
#include "fisb/uplink_frame.hpp"
#include "json_lite.hpp"
#include "recording_reader.hpp"
#include "time_util.hpp"

#include "engine_test_support.hpp"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using namespace skycued::fisb;
using namespace enginetest;
using skycued::EngineEvent;
using skycued::EventKind;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;
const std::string kSample = kFixtures + "/raw/uat-sample.txt.gz";
const std::string kTg30 = kFixtures + "/fisb/tg30.978";
const std::string kReceivedWithUplinks = kFixtures + "/fisb/received-with-uplinks.jsonl";

TimePoint iso(const std::string& s)
{
    auto t = skycued::parseIso8601Ms(s);
    BOOST_REQUIRE_MESSAGE(t, "bad time " << s);
    return *t;
}

std::vector<std::string> readLines(const std::string& path)
{
    std::vector<std::string> lines;
    skycued::GzLineReader in(path);  // throws on a missing file or a truncated gzip stream
    for (std::string cur; in.getline(cur);) {
        if (!cur.empty() && cur.back() == '\r')
            cur.pop_back();
        lines.push_back(cur);
    }
    return lines;
}

// tg30's ";t=<unix seconds>.<ms>;" receive time.
TimePoint tg30Time(const std::string& line)
{
    const auto p = line.find(";t=");
    BOOST_REQUIRE(p != std::string::npos);
    const double s = std::strtod(line.c_str() + p + 3, nullptr);
    return TimePoint(std::chrono::duration_cast<TimePoint::duration>(
        std::chrono::milliseconds(static_cast<int64_t>(std::llround(s * 1000)))));
}

struct Pipeline {
    FisbDecoder dec;
    FisbStore store;
    std::vector<FisbEvent> events;
    TimePoint now{};
    explicit Pipeline(bool experimental = false) : dec(experimental)
    {
        dec.onProduct([this](const FisbProduct& p) {
            for (auto& e : store.apply(p, now))
                events.push_back(std::move(e));
        });
    }
    void feed(TimePoint t, const std::string& line)
    {
        now = t;
        dec.feed(t, line);
    }
    void sweep(TimePoint t)
    {
        dec.sweep(t);
        for (auto& e : store.sweep(t))
            events.push_back(std::move(e));
    }
};

bool inRange(const Shape& s)
{
    return std::all_of(s.points.begin(), s.points.end(), [](const std::pair<double, double>& p) {
        return std::isfinite(p.first) && std::isfinite(p.second) && std::fabs(p.first) <= 90 &&
               std::fabs(p.second) <= 180;
    });
}

std::size_t count(const std::vector<FisbEvent>& ev, FisbEvent::Kind k)
{
    return static_cast<std::size_t>(
        std::count_if(ev.begin(), ev.end(), [&](const FisbEvent& e) { return e.kind == k; }));
}

}  // namespace

BOOST_AUTO_TEST_CASE(decoder_rejects_wrong_size_and_bad_hex)
{
    FisbDecoder dec;
    std::size_t products = 0;
    dec.onProduct([&](const FisbProduct&) { ++products; });
    const auto t = iso("2015-01-24T04:12:00Z");
    dec.feed(t, "+" + std::string(431 * 2, '0') + ";");  // 431 bytes
    dec.feed(t, "+" + std::string(433 * 2, '0') + ";");  // 433 bytes
    dec.feed(t, "+zz;");
    dec.feed(t, "");
    BOOST_TEST(dec.counters().uplinks == 4u);
    BOOST_TEST(dec.counters().bad_frames == 2u);
    BOOST_TEST(dec.counters().bad_hex == 2u);
    BOOST_TEST(products == 0u);
    // 432 zero bytes: a well-formed uplink whose application data is not valid.
    dec.feed(t, std::string(432 * 2, '0'));
    BOOST_TEST(dec.counters().bad_frames == 2u);
    BOOST_TEST(dec.counters().not_app_data == 1u);
}

// File IDs are per ground station: two stations' segments never join.
BOOST_AUTO_TEST_CASE(segment_key_includes_ground_station)
{
    SegmentAssembler sa;
    ApduHeader h;
    h.product_id = 8;
    h.segmented = h.s_flag = true;
    h.apdu_number = 77;
    h.segment_count = 2;
    const std::vector<uint8_t> a(20, 1), b(20, 2);
    const auto t = iso("2015-01-24T04:12:00Z");
    h.segment_number = 1;
    BOOST_TEST(!sa.add(h, a.data(), a.size(), t, 100).has_value());
    h.segment_number = 2;
    BOOST_TEST(!sa.add(h, b.data(), b.size(), t, 200).has_value());  // another station's
    BOOST_TEST(sa.open() == 2u);
    auto done = sa.add(h, b.data(), b.size(), t, 100);
    BOOST_REQUIRE(done.has_value());
    BOOST_TEST(done->payload.size() == 20u + 14u);

    UplinkHeader u1, u2;
    u1.lat_deg = 38.5;
    u1.lon_deg = -121.25;
    u2 = u1;
    u2.lon_deg = -121.0;
    BOOST_TEST(stationKey(u1) != stationKey(u2));
    BOOST_TEST(stationKey(u1) == stationKey(u1));
}

// Every uplink of the 2015 sample on a 1 Hz clock from 04:12 (its products
// are from 03:53-04:10 that day).
BOOST_AUTO_TEST_CASE(sample_products_stored, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    Pipeline p;
    const TimePoint t0 = iso("2015-01-24T04:12:00Z");
    std::size_t i = 0;
    for (const auto& l : readLines(kSample)) {
        if (!isUplinkLine(l))
            continue;
        const TimePoint t = t0 + std::chrono::seconds(i++);
        p.feed(t, l);
        p.sweep(t);
    }
    p.sweep(t0 + std::chrono::seconds(i + 15));
    const auto& c = p.dec.counters();
    BOOST_TEST(c.uplinks == 704u);
    BOOST_TEST(c.bad_frames == 0u);
    BOOST_TEST(c.bad_products == 0u);
    BOOST_TEST(c.rejected_shapes == 0u);
    const auto s = p.store.snapshot();
    BOOST_TEST_MESSAGE("sample: products=" << c.products << " metar=" << s.metar.size() << " taf=" << s.taf.size()
                                           << " pirep=" << s.pirep.size() << " winds=" << s.winds.size()
                                           << " other=" << s.other.size() << " twgo=" << s.twgo.size()
                                           << " radar=" << s.radar.size() << " radar blocks="
                                           << (s.radar.empty() ? 0 : s.radar[0].blocks.size())
                                           << " stations=" << s.stations.size()
                                           << " stale=" << p.store.stats().stale);
    BOOST_TEST(s.stations.size() == 1u);
    BOOST_REQUIRE_EQUAL(s.radar.size(), 1u);
    BOOST_TEST(s.radar[0].product == 63);
    BOOST_TEST((s.radar[0].product_time == iso("2015-01-24T04:10:00Z")));
    BOOST_TEST(s.radar[0].blocks.size() > 100u);
    BOOST_TEST(s.metar.size() > 10u);
    BOOST_TEST(s.twgo.size() > 50u);
    BOOST_TEST(count(p.events, FisbEvent::Kind::radar) >= 1u);
    for (const auto& r : s.twgo)
        for (const auto& sh : r.shapes)
            BOOST_TEST(inRange(sh));
}

// Design §5.5: decode tg30.978 end to end in under 2 s; spec §4.12: one
// uplink decodes in <= 1 ms typically.
BOOST_AUTO_TEST_CASE(tg30_end_to_end_under_2s)
{
    const auto lines = readLines(kTg30);
    Pipeline p;
    std::vector<double> per_uplink_us;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& l : lines) {
        if (!isUplinkLine(l))
            continue;
        const TimePoint t = tg30Time(l);
        const auto a = std::chrono::steady_clock::now();
        p.feed(t, l);
        per_uplink_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
        p.sweep(t);
    }
    p.sweep(tg30Time(lines.back()) + std::chrono::seconds(15));
    const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(per_uplink_us.begin(), per_uplink_us.end());
    const double median = per_uplink_us[per_uplink_us.size() / 2];
    const double p99 = per_uplink_us[per_uplink_us.size() * 99 / 100];
    const auto s = p.store.snapshot();
    const auto& c = p.dec.counters();
    std::size_t blocks = 0;
    for (const auto* v : {&s.radar, &s.rasters})
        for (const auto& m : *v)
            blocks += m.blocks.size();
    BOOST_TEST_MESSAGE("tg30: " << per_uplink_us.size() << " uplinks in " << total_s << " s (decode+store); feed median "
                                << median << " us, p99 " << p99 << " us, max " << per_uplink_us.back()
                                << " us; products=" << c.products << " metar=" << s.metar.size()
                                << " taf=" << s.taf.size() << " pirep=" << s.pirep.size() << " winds="
                                << s.winds.size() << " other=" << s.other.size() << " twgo=" << s.twgo.size()
                                << " radar=" << s.radar.size() << " rasters=" << s.rasters.size()
                                << " blocks=" << blocks << " published=" << p.store.stats().mosaics_published
                                << " stale=" << p.store.stats().stale);
    {
        std::map<std::string, std::size_t> twgo_events;
        for (const auto& e : p.events)
            if (e.kind == FisbEvent::Kind::twgo)
                ++twgo_events[e.key];
        std::size_t most = 0;
        for (const auto& k : twgo_events)
            most = std::max(most, k.second);
        BOOST_TEST_MESSAGE("tg30 twgo: " << count(p.events, FisbEvent::Kind::twgo) << " events over "
                                         << twgo_events.size() << " keys, at most " << most << " per key; "
                                         << count(p.events, FisbEvent::Kind::text) << " text events, "
                                         << count(p.events, FisbEvent::Kind::expired) << " expired");
    }
    BOOST_TEST(per_uplink_us.size() == 963u);
    BOOST_TEST(total_s < 2.0);
    BOOST_TEST(median < 1000.0);
    BOOST_TEST(c.bad_frames == 0u);
    BOOST_TEST(c.bad_products == 0u);
    BOOST_TEST(c.blocks_rejected == 0u);
    BOOST_TEST(c.segments_timed_out == 0u);
    BOOST_TEST(s.radar.size() == 2u);  // regional and CONUS
    BOOST_TEST(s.rasters.size() >= 6u);
    BOOST_TEST(!s.metar.empty());
    BOOST_TEST(!s.twgo.empty());
    for (const auto& r : s.twgo)
        for (const auto& sh : r.shapes)
            BOOST_TEST(inRange(sh));
}

// Bit-flipped tg30 uplinks: never a throw, never a shape out of range.
BOOST_AUTO_TEST_CASE(fuzz_products_stay_in_range)
{
    std::vector<std::vector<uint8_t>> payloads;
    for (const auto& l : readLines(kTg30)) {
        std::vector<uint8_t> b;
        if (isUplinkLine(l) && hexToBytes(l, b) && b.size() == kUplinkBytes)
            payloads.push_back(std::move(b));
    }
    BOOST_REQUIRE(!payloads.empty());
    std::mt19937 rng(20261102);
    FisbDecoder dec;
    std::size_t shapes = 0, out_of_range = 0, bad_blocks = 0;
    dec.onProduct([&](const FisbProduct& p) {
        if (const auto* t = std::get_if<TwgoParsed>(&p.body))
            for (const auto& s : t->shapes) {
                ++shapes;
                out_of_range += inRange(s) ? 0 : 1;
            }
        if (const auto* b = std::get_if<DecodedBlocks>(&p.body))
            for (const auto& blk : b->blocks)
                bad_blocks += std::fabs(blk.lat0) <= 90 ? 0 : 1;
    });
    static const char* hex = "0123456789abcdef";
    const TimePoint t = iso("2020-10-30T08:55:00Z");
    for (int i = 0; i < 20000; ++i) {
        auto b = payloads[rng() % payloads.size()];
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k)
            { const auto flipMask_ = static_cast<uint8_t>(1u << (rng() % 8)); auto& flipCell_ = b[8 + rng() % (b.size() - 8)]; flipCell_ = static_cast<uint8_t>(flipCell_ ^ flipMask_); }
        std::string line = "+";
        for (uint8_t v : b) {
            line += hex[v >> 4];
            line += hex[v & 15];
        }
        line += ";";
        BOOST_CHECK_NO_THROW(dec.feed(t, line));
    }
    BOOST_TEST_MESSAGE("fuzz: products=" << dec.counters().products << " shapes=" << shapes << " rejected_shapes="
                                         << dec.counters().rejected_shapes << " graphics_rejected="
                                         << dec.counters().graphics_rejected << " bad_products="
                                         << dec.counters().bad_products);
    BOOST_TEST(out_of_range == 0u);
    BOOST_TEST(dec.counters().graphics_rejected <= dec.counters().rejected_shapes);
    BOOST_TEST(bad_blocks == 0u);
    BOOST_TEST(dec.counters().products > 1000u);
}

namespace {

// An Engine replaying received-with-uplinks.jsonl unpaced, optionally
// recording all three streams; collects every fisb_* event.
struct ReplayFx {
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<skycued::Recorder> received, sent, fisb;
    std::unique_ptr<Engine> engine;
    std::mutex m;
    std::vector<EngineEvent> events;

    explicit ReplayFx(const std::string& record_dir = "")
    {
        Config cfg = baseConfig();
        cfg.receiver_lat = 38.9;
        cfg.receiver_lon = -77.0;
        t.call([&] {
            ForwarderConfig fc;
            fc.tcp_listen_ephemeral = true;
            fwd = std::make_unique<Forwarder>(t.io, fc);
            if (!record_dir.empty()) {
                skycued::RecorderConfig rc;
                rc.dir = record_dir;
                rc.csv = false;
                received = std::make_unique<skycued::Recorder>(rc, "received");
                sent = std::make_unique<skycued::Recorder>(rc, "sent");
                fisb = std::make_unique<skycued::Recorder>(rc, "fisb");
            }
            auto src = std::make_unique<skycued::ReplayObservationSource>(t.io, kReceivedWithUplinks, 0, false);
            engine = std::make_unique<Engine>(t.io, cfg, sourceInputs(t.io, std::move(src)), *fwd, received.get(),
                                              sent.get(), fisb.get());
            engine->onEvent([this](const EngineEvent& e) {
                if (!e.fisb)
                    return;
                std::lock_guard<std::mutex> lk(m);
                events.push_back(e);
            });
            engine->start();
        });
    }
    ~ReplayFx()
    {
        try {
            t.call([this] {
                engine.reset();
                fwd.reset();
                received.reset();
                sent.reset();
                fisb.reset();
            });
        } catch (...) {
        }
        t.join();
    }
    skycued::EngineStats stats()
    {
        return t.call([this] { return engine->stats(); });
    }
    std::size_t countEvents(EventKind k)
    {
        std::lock_guard<std::mutex> lk(m);
        return static_cast<std::size_t>(
            std::count_if(events.begin(), events.end(), [&](const EngineEvent& e) { return e.kind == k; }));
    }
    // Until every uplink is in and a radar mosaic has been published.
    bool waitDone(std::chrono::seconds timeout = 20s)
    {
        const auto end = steady::now() + timeout;
        while (steady::now() < end) {
            const auto s = stats();
            if (s.fisb_uplinks == 704u && s.observations == 361u && countEvents(EventKind::fisb_radar) >= 1)
                return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }
};

}  // namespace

// The recording's 2015 times are the FIS-B reference and expiry clock: the
// products are decoded and kept while the data clock reads 2015, and nothing
// expires before its time on that clock.
BOOST_AUTO_TEST_CASE(replay_decodes_uplinks_on_recording_clock)
{
    // What the store holds when every record is applied at its own time.
    Pipeline ref;
    TimePoint last{};
    {
        std::ifstream in(kReceivedWithUplinks);
        std::string line;
        while (std::getline(in, line)) {
            skycued::RecordingRecord r;
            if (skycued::parseRecordingLine(line, r) != skycued::LineKind::received ||
                r.kind != skycued::RecordKind::uat_uplink)
                continue;
            ref.feed(r.time, r.uplink_hex);
            ref.sweep(r.time);
            last = r.time;
        }
    }
    const auto expected = ref.store.snapshot();

    ReplayFx fx;
    BOOST_REQUIRE(fx.waitDone());
    const auto st = fx.stats();
    BOOST_TEST(st.fisb_bad_frames == 0u);
    BOOST_TEST(st.fisb_products == ref.dec.counters().products);
    BOOST_TEST(st.fisb_mosaic_blocks > 100u);
    const auto snap = fx.t.call([&] { return fx.engine->snapshot(); });
    const auto fs = fx.t.call([&] { return fx.engine->fisbSnapshot(); });
    // The data clock is the recording's.
    BOOST_TEST((snap.now >= last));
    BOOST_TEST((snap.now < iso("2015-01-25T00:00:00Z")));
    BOOST_TEST(snap.fisb.enabled);
    BOOST_TEST(snap.fisb.stations == 1u);
    BOOST_REQUIRE(snap.fisb.radar_age_s.has_value());
    BOOST_TEST(*snap.fisb.radar_age_s < 3600.0);
    // Everything decoded is still there.
    BOOST_TEST(fs.metar.size() == expected.metar.size());
    BOOST_TEST(fs.taf.size() == expected.taf.size());
    BOOST_TEST(fs.twgo.size() == expected.twgo.size());
    BOOST_TEST(!fs.metar.empty());
    BOOST_TEST(!fs.twgo.empty());
    BOOST_REQUIRE_EQUAL(fs.radar.size(), 1u);
    BOOST_TEST(fs.radar[0].blocks.size() == expected.radar[0].blocks.size());
    BOOST_TEST(snap.fisb.text_records == fs.metar.size() + fs.taf.size() + fs.pirep.size() + fs.winds.size() +
                                             fs.other.size());
    // Nothing expired early: every expiry seen was due on the 2015 clock.
    {
        std::lock_guard<std::mutex> lk(fx.m);
        for (const auto& e : fx.events) {
            BOOST_TEST(e.fisb.has_value());
            if (e.kind == EventKind::fisb_expired) {
                BOOST_REQUIRE(e.fisb->expires.has_value());
                BOOST_TEST((*e.fisb->expires <= snap.now));
                BOOST_TEST((*e.fisb->expires < iso("2015-01-25T00:00:00Z")));
            }
            if (e.kind == EventKind::fisb_radar) {
                BOOST_TEST(e.fisb->product == 63);
                BOOST_TEST(e.fisb->block_count > 0u);
                BOOST_TEST((e.fisb->product_time == iso("2015-01-24T04:10:00Z")));
            }
        }
    }
    BOOST_TEST(fx.countEvents(EventKind::fisb_text) > 10u);
    BOOST_TEST(fx.countEvents(EventKind::fisb_shape) >= 1u);
}

BOOST_AUTO_TEST_CASE(recording_fisb_stream_written)
{
    TempDir tmp;
    {
        ReplayFx fx(tmp.path);
        BOOST_REQUIRE(fx.waitDone());
        fx.t.call([&] { fx.engine->stop(); });
    }
    const std::string text = streamText(tmp.path, "fisb");
    BOOST_REQUIRE(!text.empty());
    std::map<std::string, std::size_t> by_type;
    std::istringstream in(text);
    std::string line;
    std::size_t radar_blocks = 0;
    while (std::getline(in, line)) {
        auto j = skycued::JsonObject::parse(line, 16u << 20);
        BOOST_REQUIRE_MESSAGE(j.has_value(), line.substr(0, 120));
        const auto type = j->getString("type");
        BOOST_REQUIRE(type.has_value());
        ++by_type[*type];
        BOOST_TEST(skycued::parseIso8601Ms(*j->getString("time")).has_value());
        BOOST_TEST(j->getString("time")->compare(0, 4, "2015") == 0);  // the data clock
        if (*type == "fisb_radar") {
            radar_blocks = static_cast<std::size_t>(*j->getInt("block_count"));
            BOOST_TEST(line.find("\"rle\":[[") != std::string::npos);
            BOOST_TEST(*j->getInt("product") == 63);
        }
    }
    BOOST_TEST_MESSAGE("fisb stream: " << by_type["fisb_text"] << " text, " << by_type["fisb_twgo"] << " twgo, "
                                       << by_type["fisb_radar"] << " radar, " << by_type["fisb_expired"]
                                       << " expired; " << text.size() << " bytes");
    BOOST_TEST(by_type["fisb_text"] > 10u);
    BOOST_TEST(by_type["fisb_twgo"] > 10u);
    BOOST_TEST(by_type["fisb_radar"] >= 1u);
    BOOST_TEST(radar_blocks > 100u);
    // The uplinks are recorded in the received stream as before.
    BOOST_TEST(countOf(streamText(tmp.path, "received"), "\"type\":\"uat_uplink\"") == 704u);
}

BOOST_AUTO_TEST_CASE(fisb_disabled_decodes_nothing)
{
    Config cfg = baseConfig();
    cfg.fisb_enable = false;
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<Engine> engine;
    t.call([&] {
        ForwarderConfig fc;
        fc.tcp_listen_ephemeral = true;
        fwd = std::make_unique<Forwarder>(t.io, fc);
        auto src = std::make_unique<skycued::ReplayObservationSource>(t.io, kReceivedWithUplinks, 0, false);
        engine = std::make_unique<Engine>(t.io, cfg, sourceInputs(t.io, std::move(src)), *fwd, nullptr, nullptr);
        engine->start();
    });
    const auto end = steady::now() + 10s;
    while (steady::now() < end && t.call([&] { return engine->stats().observations; }) < 361u)
        std::this_thread::sleep_for(20ms);
    const auto st = t.call([&] { return engine->stats(); });
    const auto snap = t.call([&] { return engine->snapshot(); });
    BOOST_TEST(st.observations == 361u);
    BOOST_TEST(st.fisb_uplinks == 0u);
    BOOST_TEST(!snap.fisb.enabled);
    t.call([&] {
        engine.reset();
        fwd.reset();
    });
}
