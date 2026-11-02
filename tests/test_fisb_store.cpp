// FisbStore: mosaics, text, TWGO pairing and expiry (design §10.1-§10.6),
// on a fake clock: every `now` is passed in explicitly.

#define BOOST_TEST_MODULE fisb_store
#include <boost/test/unit_test.hpp>

#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"
#include "fisb/global_block.hpp"
#include "json_lite.hpp"
#include "time_util.hpp"

#include <algorithm>
#include <map>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace skycued::fisb;
using namespace std::chrono_literals;

namespace {

TimePoint iso(const std::string& s)
{
    auto t = skycued::parseIso8601Ms(s);
    BOOST_REQUIRE_MESSAGE(t.has_value(), "bad time " << s);
    return *t;
}

const TimePoint kT0 = iso("2015-01-24T04:12:00Z");

FisbProduct base(uint16_t pid, TimePoint now)
{
    FisbProduct p;
    p.id = pid;
    p.received = now;
    p.station = 42;
    p.station_lat = 38.5;
    p.station_lon = -121.5;
    return p;
}

// ---- rasters ----

RadarBlock block(uint32_t bn, uint8_t scale, uint8_t value)
{
    auto g = blockGeometry(bn, scale, false);
    BOOST_REQUIRE(g.has_value());
    RadarBlock b;
    b.block = bn;
    b.lat0 = g->lat0;
    b.lon0 = g->lon0;
    b.dlat = g->dlat;
    b.dlon = g->dlon;
    b.bins.fill(value);
    return b;
}

FisbProduct blocks(uint16_t pid, TimePoint product_time, TimePoint now, std::vector<RadarBlock> rle,
                   std::vector<uint32_t> empty = {}, uint8_t scale = 0,
                   std::optional<uint8_t> altitude_code = std::nullopt)
{
    FisbProduct p = base(pid, now);
    p.product_time = product_time;
    DecodedBlocks d;
    d.product_id = pid;
    d.scale = scale;
    d.blocks = std::move(rle);
    d.empty_blocks = std::move(empty);
    d.conus = pid == 64;
    d.altitude_code = altitude_code;
    p.body = std::move(d);
    return p;
}

const Mosaic* findMosaic(const FisbSnapshot& s, uint16_t pid)
{
    for (const auto* v : {&s.radar, &s.rasters})
        for (const auto& m : *v)
            if (m.product == pid)
                return &m;
    return nullptr;
}

const RadarBlock* findBlock(const Mosaic& m, uint32_t bn)
{
    for (const auto& b : m.blocks)
        if (b.block == bn)
            return &b;
    return nullptr;
}

std::size_t countKind(const std::vector<FisbEvent>& ev, FisbEvent::Kind k)
{
    return static_cast<std::size_t>(
        std::count_if(ev.begin(), ev.end(), [&](const FisbEvent& e) { return e.kind == k; }));
}

// ---- text ----

TextRecord text(TextRecord::Kind kind, const std::string& station, TimePoint issued, const std::string& body,
                std::optional<uint8_t> period = std::nullopt)
{
    TextRecord r;
    r.kind = kind;
    r.station = station;
    r.issued = issued;
    r.text = body;
    r.period_h = period;
    return r;
}

FisbProduct textProduct(TimePoint now, std::vector<TextRecord> records)
{
    FisbProduct p = base(413, now);
    p.product_time = now;
    p.body = std::move(records);
    return p;
}

// ---- TWGO ----

TwgoKey key(uint16_t pid, uint32_t report)
{
    TwgoKey k;
    k.pid = pid;
    k.year = 15;
    k.report = report;
    k.month = 1;
    return k;
}

FisbProduct twgoText(uint16_t pid, uint32_t report, const std::string& body, TimePoint now, bool cancel = false)
{
    FisbProduct p = base(pid, now);
    TwgoParsed t;
    t.key = key(pid, report);
    t.text = body;
    t.cancel = cancel;
    t.record_format = 2;
    p.body = std::move(t);
    return p;
}

Shape square(double lat, double lon)
{
    Shape s;
    s.geom = Shape::polygon;
    s.points = {{lat, lon}, {lat, lon + 1}, {lat + 1, lon + 1}, {lat + 1, lon}, {lat, lon}};
    s.alt_low_ft = 0;
    s.alt_high_ft = 18000;
    return s;
}

FisbProduct twgoGraphics(uint16_t pid, uint32_t report, std::vector<Shape> shapes, TimePoint now,
                         uint8_t object_status = 15, std::optional<TimePoint> end = std::nullopt)
{
    FisbProduct p = base(pid, now);
    TwgoParsed t;
    t.key = key(pid, report);
    t.shapes = std::move(shapes);
    t.has_graphics = true;
    t.object_status = object_status;
    t.cancel = object_status == 13;
    t.end = end;
    t.record_format = 8;
    p.body = std::move(t);
    return p;
}

const TwgoRecord* findTwgo(const FisbSnapshot& s, uint32_t report)
{
    for (const auto& r : s.twgo)
        if (r.key.report == report)
            return &r;
    return nullptr;
}

}  // namespace

// §10.1: newest block wins; publish after 10 s without a block, or 30 s after
// the first unpublished block while blocks keep coming.
BOOST_AUTO_TEST_CASE(mosaic_merges_newest_wins_and_publishes_on_quiescence)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    BOOST_TEST(st.apply(blocks(63, pt, kT0, {block(277546, 0, 1)}), kT0).empty());
    BOOST_TEST(st.apply(blocks(63, pt, kT0 + 3s, {block(277547, 0, 1)}), kT0 + 3s).empty());
    BOOST_TEST(st.sweep(kT0 + 12s).empty());  // 9 s quiet
    auto ev = st.sweep(kT0 + 13s);            // 10 s quiet
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::radar));
    BOOST_TEST(ev[0].product == 63);
    BOOST_TEST(ev[0].key == "63");
    BOOST_REQUIRE(ev[0].mosaic.has_value());
    BOOST_TEST(ev[0].mosaic->blocks.size() == 2u);
    BOOST_TEST((ev[0].mosaic->product_time == pt));
    BOOST_TEST(st.sweep(kT0 + 20s).empty());  // published once, nothing new

    // A newer product time replaces the block; an older one does not.
    const TimePoint pt2 = pt + 2min;
    st.apply(blocks(63, pt2, kT0 + 30s, {block(277546, 0, 2)}), kT0 + 30s);
    st.apply(blocks(63, pt, kT0 + 31s, {block(277546, 0, 3)}), kT0 + 31s);
    BOOST_TEST(st.stats().blocks_superseded == 1u);
    ev = st.sweep(kT0 + 41s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    const RadarBlock* b = findBlock(*ev[0].mosaic, 277546);
    BOOST_REQUIRE(b);
    BOOST_TEST(b->bins[0] == 2);
    BOOST_TEST((ev[0].mosaic->product_time == pt2));
    // Equal product times: the later reception wins.
    st.apply(blocks(63, pt2, kT0 + 50s, {block(277546, 0, 4)}), kT0 + 50s);
    BOOST_TEST(findBlock(*findMosaic(st.snapshot(), 63), 277546)->bins[0] == 4);
    st.sweep(kT0 + 60s);

    // Streaming: a block every 5 s never goes quiet; publish at 30 s.
    const TimePoint s0 = kT0 + 100s;
    std::size_t published_at = 0;
    for (int i = 0; i <= 8; ++i) {
        const TimePoint now = s0 + std::chrono::seconds(5 * i);
        auto a = st.apply(blocks(63, pt2, now, {block(277600 + static_cast<uint32_t>(i), 0, 1)}), now);
        auto s = st.sweep(now + 1s);
        if ((countKind(a, FisbEvent::Kind::radar) || countKind(s, FisbEvent::Kind::radar)) && !published_at)
            published_at = static_cast<std::size_t>(i);
    }
    BOOST_TEST(published_at == 6u);  // the block at s0 + 30 s
    BOOST_TEST(st.stats().mosaics_published == 4u);  // 13 s, 41 s, 60 s, streaming
}

// Final review: every cycle re-sends the mosaic; a block identical to the
// one held (same time and bins) is no change, so it neither republishes nor
// holds back a publish that is due.
BOOST_AUTO_TEST_CASE(repeated_identical_blocks_do_not_republish)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    const auto product = [&](TimePoint now, uint8_t v) {
        return blocks(63, pt, now, {block(277546, 0, v), block(277547, 0, 1)}, {277548});
    };
    st.apply(product(kT0, 1), kT0);
    BOOST_REQUIRE_EQUAL(countKind(st.sweep(kT0 + 10s), FisbEvent::Kind::radar), 1u);
    const auto updated = findMosaic(st.snapshot(), 63)->updated;
    // The same blocks every 5 s for 2 min: never published again.
    for (int i = 1; i <= 24; ++i) {
        const TimePoint now = kT0 + 10s + std::chrono::seconds(5 * i);
        BOOST_TEST(st.apply(product(now, 1), now).empty());
        BOOST_TEST(st.sweep(now + 1s).empty());
    }
    BOOST_TEST(st.stats().mosaics_published == 1u);
    BOOST_TEST(st.stats().blocks_unchanged == 24u * 3u);
    BOOST_TEST((findMosaic(st.snapshot(), 63)->updated == updated));

    // A changed bin is a change: published 10 s after it, although the
    // identical repeats go on arriving every 5 s.
    const TimePoint tc = kT0 + 200s;
    st.apply(product(tc, 5), tc);
    std::size_t published = 0;
    TimePoint at{};
    for (int i = 1; i <= 4; ++i) {
        const TimePoint now = tc + std::chrono::seconds(5 * i);
        st.apply(product(now, 5), now);
        for (const TimePoint t : {now, now + 1s})
            if (countKind(st.sweep(t), FisbEvent::Kind::radar) && !published++)
                at = t;
    }
    BOOST_TEST(published == 1u);
    BOOST_TEST((at == tc + 10s));
    // A newer product time with the same bins is a change too.
    st.apply(blocks(63, pt + 2min, tc + 60s, {block(277546, 0, 5)}), tc + 60s);
    BOOST_TEST(countKind(st.sweep(tc + 70s), FisbEvent::Kind::radar) == 1u);
}

BOOST_AUTO_TEST_CASE(mosaic_rejects_blocks_older_than_10min)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:20:00Z");
    st.apply(blocks(63, pt, kT0, {block(277546, 0, 1)}), kT0);
    st.apply(blocks(63, pt - 11min, kT0, {block(277547, 0, 1)}), kT0);  // 11 min older: rejected
    st.apply(blocks(63, pt - 10min, kT0, {block(277548, 0, 1)}), kT0);  // exactly 10: merged
    BOOST_TEST(st.stats().blocks_too_old == 1u);
    auto s = st.snapshot();
    const Mosaic* m = findMosaic(s, 63);
    BOOST_REQUIRE(m);
    BOOST_TEST(m->blocks.size() == 2u);
    BOOST_TEST(!findBlock(*m, 277547));
    BOOST_TEST(findBlock(*m, 277548));

    // When the mosaic time advances, blocks now more than 10 min older go.
    st.apply(blocks(63, pt + 5min, kT0 + 1s, {block(277549, 0, 1)}), kT0 + 1s);
    s = st.snapshot();
    m = findMosaic(s, 63);
    BOOST_TEST(m->blocks.size() == 2u);  // 277546 (04:20) and 277549 (04:25)
    BOOST_TEST(!findBlock(*m, 277548));
    BOOST_TEST(st.stats().blocks_pruned == 1u);
}

BOOST_AUTO_TEST_CASE(mosaic_cap_4096_evicts_oldest_and_counts)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    // The very first block is the oldest by product time.
    st.apply(blocks(63, pt - 1min, kT0, {block(200000, 0, 1)}), kT0);
    for (uint32_t i = 1; i <= kMosaicMaxBlocks; ++i)
        st.apply(blocks(63, pt, kT0, {block(200000 + i, 0, 1)}), kT0);
    BOOST_TEST(st.mosaicBlocks() == kMosaicMaxBlocks);
    BOOST_TEST(st.stats().blocks_evicted == 1u);
    auto s = st.snapshot();
    const Mosaic* m = findMosaic(s, 63);
    BOOST_TEST(m->evicted == 1u);
    BOOST_TEST(!findBlock(*m, 200000));
    BOOST_TEST(findBlock(*m, 200000 + kMosaicMaxBlocks));
}

// §10.6: empty CONUS blocks show as level 1; empty regional ones as 0.
// Other rasters are separate layers per altitude code.
BOOST_AUTO_TEST_CASE(empty_conus_blocks_are_level_1_and_rasters_are_layers)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:00:00Z");
    st.apply(blocks(64, pt, kT0, {}, {139405, 139410}, 1), kT0);
    st.apply(blocks(63, pt, kT0, {}, {277546}, 0), kT0);
    st.apply(blocks(70, pt, kT0, {block(139405, 1, 9)}, {}, 1, uint8_t{2}), kT0);
    st.apply(blocks(70, pt, kT0, {block(139405, 1, 7)}, {}, 1, uint8_t{3}), kT0);
    auto s = st.snapshot();
    const Mosaic* conus = findMosaic(s, 64);
    BOOST_REQUIRE(conus);
    BOOST_TEST(conus->blocks.size() == 2u);
    for (const auto& b : conus->blocks)
        BOOST_TEST(std::all_of(b.bins.begin(), b.bins.end(), [](uint8_t v) { return v == 1; }));
    BOOST_TEST(findMosaic(s, 63)->blocks[0].bins[5] == 0);
    BOOST_TEST(s.rasters.size() == 2u);  // 70-2 and 70-3
    BOOST_TEST(s.radar.size() == 2u);
    BOOST_TEST(s.rasters[0].key() == "70-2");
    BOOST_TEST(s.rasters[1].key() == "70-3");
    BOOST_TEST(s.rasters[1].blocks[0].bins[0] == 7);
    auto ev = st.sweep(kT0 + 10s);
    BOOST_TEST(countKind(ev, FisbEvent::Kind::radar) == 2u);
    BOOST_TEST(countKind(ev, FisbEvent::Kind::raster) == 2u);
}

BOOST_AUTO_TEST_CASE(twgo_text_and_graphics_pair_across_apdus)
{
    FisbStore st;
    // Graphics first: held, not published.
    auto ev = st.apply(twgoGraphics(8, 6095, {square(39, -121)}, kT0), kT0);
    BOOST_TEST(ev.empty());
    BOOST_TEST(st.snapshot().twgo.empty());
    BOOST_TEST(st.stats().twgo_pending == 1u);
    // Then the text: one complete record with both halves.
    ev = st.apply(twgoText(8, 6095, "NOTAM-TFR 5/6095 240100Z ...", kT0 + 5s), kT0 + 5s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::twgo));
    BOOST_TEST(ev[0].key == "8-15-6095--1");
    BOOST_REQUIRE(ev[0].twgo.has_value());
    BOOST_TEST(ev[0].twgo->complete);
    BOOST_TEST(ev[0].twgo->has_graphics);
    BOOST_TEST(ev[0].twgo->shapes.size() == 1u);
    BOOST_TEST(*ev[0].twgo->text == "NOTAM-TFR 5/6095 240100Z ...");
    BOOST_TEST(st.stats().twgo_pending == 0u);
    // The same halves again: nothing changes.
    BOOST_TEST(st.apply(twgoGraphics(8, 6095, {square(39, -121)}, kT0 + 60s), kT0 + 60s).empty());
    BOOST_TEST(st.apply(twgoText(8, 6095, "NOTAM-TFR 5/6095 240100Z ...", kT0 + 61s), kT0 + 61s).empty());
    // New graphics: republished.
    ev = st.apply(twgoGraphics(8, 6095, {square(40, -121)}, kT0 + 70s), kT0 + 70s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].twgo->shapes[0].points[0].first == 40.0);
    // A changed text drops the old graphics (reference §6.5).
    ev = st.apply(twgoText(8, 6095, "NOTAM-TFR 5/6095 240200Z amended", kT0 + 80s), kT0 + 80s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(!ev[0].twgo->has_graphics);
    BOOST_TEST(ev[0].twgo->shapes.empty());

    // Text-only products are complete at once; G-AIRMET (14) has graphics only.
    ev = st.apply(twgoText(13, 9294, "SUA 241445 ...", kT0), kT0);
    BOOST_TEST(ev.size() == 1u);
    ev = st.apply(twgoGraphics(14, 300, {square(41, -90)}, kT0), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].twgo->complete);
    BOOST_TEST(!ev[0].twgo->text.has_value());
    BOOST_TEST(st.snapshot().twgo.size() == 3u);
}

BOOST_AUTO_TEST_CASE(twgo_keepalive_does_not_blank_text)
{
    FisbStore st;
    st.apply(twgoText(8, 4342, "NOTAM-TFR 5/4342 152352Z PART 1 OF 2", kT0), kT0);
    // An empty-text active record 10 min later: renew only.
    auto ev = st.apply(twgoText(8, 4342, "", kT0 + 10min), kT0 + 10min);
    BOOST_TEST(ev.empty());
    auto s = st.snapshot();
    const TwgoRecord* r = findTwgo(s, 4342);
    BOOST_REQUIRE(r);
    BOOST_REQUIRE(r->text.has_value());
    BOOST_TEST(*r->text == "NOTAM-TFR 5/4342 152352Z PART 1 OF 2");
    BOOST_TEST((r->last_received == kT0 + 10min));
    BOOST_TEST((r->expires == kT0 + 10min + expiry::kTwgo));
    // So it outlives the 70 min from its first reception.
    BOOST_TEST(st.sweep(kT0 + 75min).empty());
    BOOST_TEST(findTwgo(st.snapshot(), 4342));
    // A keep-alive for a report never seen creates nothing.
    BOOST_TEST(st.apply(twgoText(8, 4343, "", kT0), kT0).empty());
    BOOST_TEST(!findTwgo(st.snapshot(), 4343));
    BOOST_TEST(st.stats().twgo_pending == 0u);
}

BOOST_AUTO_TEST_CASE(twgo_cancel_kept_30min_then_expires)
{
    FisbStore st;
    st.apply(twgoText(8, 12045, "NOTAM-D KSCK.01/124 ...", kT0), kT0);
    st.apply(twgoGraphics(8, 12045, {square(37, -121)}, kT0 + 1s), kT0 + 1s);
    // object_status 13 cancels.
    const TimePoint tc = kT0 + 5min;
    auto ev = st.apply(twgoGraphics(8, 12045, {square(37, -121)}, tc, 13), tc);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::expired));
    BOOST_TEST(ev[0].reason == "cancelled");
    BOOST_TEST(ev[0].key == "8-15-12045--1");
    auto s = st.snapshot();
    const TwgoRecord* r = findTwgo(s, 12045);
    BOOST_REQUIRE(r);
    BOOST_TEST(r->cancelled);
    BOOST_TEST((r->expires == tc + expiry::kCancelled));
    // A repeated cancel (text status 0 this time) does not extend the 30 min.
    BOOST_TEST(st.apply(twgoText(8, 12045, "", tc + 10min, true), tc + 10min).empty());
    BOOST_TEST(st.sweep(tc + 30min - 1s).empty());
    ev = st.sweep(tc + 30min);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::expired));
    BOOST_TEST(ev[0].reason == "expired");
    BOOST_TEST(!findTwgo(st.snapshot(), 12045));
    BOOST_TEST(st.stats().cancelled == 1u);
}

// Reference §6.5: a cancelled report is removed; only the cancellation is
// kept for 30 min, whatever else is still on the air for that key.
BOOST_AUTO_TEST_CASE(cancelled_twgo_does_not_resurrect_on_keepalive_or_graphics)
{
    FisbStore st;
    const std::string tfr = "NOTAM-TFR 5/6095 240100Z ...";
    st.apply(twgoText(8, 6095, tfr, kT0), kT0);
    st.apply(twgoGraphics(8, 6095, {square(39, -121)}, kT0 + 1s), kT0 + 1s);
    const TimePoint tc = kT0 + 2min;
    auto ev = st.apply(twgoText(8, 6095, "", tc, true), tc);  // text status 0
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].reason == "cancelled");
    BOOST_REQUIRE(ev[0].twgo.has_value());
    BOOST_TEST(!ev[0].twgo->text.has_value());  // nothing stale left in it
    BOOST_TEST(ev[0].twgo->shapes.empty());
    BOOST_TEST(!ev[0].twgo->complete);

    // Within the 30 min: a keep-alive, a graphics half still broadcast active,
    // even the full text again: all ignored.
    BOOST_TEST(st.apply(twgoText(8, 6095, "", tc + 5min), tc + 5min).empty());
    BOOST_TEST(st.apply(twgoGraphics(8, 6095, {square(39, -121)}, tc + 6min), tc + 6min).empty());
    BOOST_TEST(st.apply(twgoText(8, 6095, tfr, tc + 7min), tc + 7min).empty());
    BOOST_TEST(st.stats().ignored_after_cancel == 3u);
    auto s = st.snapshot();
    const TwgoRecord* r = findTwgo(s, 6095);
    BOOST_REQUIRE(r);
    BOOST_TEST(r->cancelled);
    BOOST_TEST(!r->text.has_value());
    BOOST_TEST(r->shapes.empty());
    BOOST_TEST((r->expires == tc + expiry::kCancelled));
    BOOST_TEST(st.sweep(tc + 30min - 1s).empty());

    // After the 30 min the key is free: a full record publishes normally.
    ev = st.sweep(tc + 30min);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].reason == "expired");
    ev = st.apply(twgoText(8, 6095, tfr, tc + 31min), tc + 31min);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::twgo));
    BOOST_TEST(!ev[0].twgo->cancelled);
    BOOST_TEST(*ev[0].twgo->text == tfr);
    ev = st.apply(twgoGraphics(8, 6095, {square(39, -121)}, tc + 32min), tc + 32min);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].twgo->has_graphics);
}

// A mosaic that expires before it was ever published leaves silently.
BOOST_AUTO_TEST_CASE(unpublished_mosaic_expires_without_event)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    st.apply(blocks(63, pt, kT0, {block(277546, 0, 1)}), kT0);
    // No sweep in between (so no publish), then straight past its lifetime.
    BOOST_TEST(st.sweep(pt + 75min).empty());
    BOOST_TEST(!findMosaic(st.snapshot(), 63));
    // A product without a resolvable time is dropped and counted.
    FisbProduct p = blocks(63, pt, kT0, {block(277547, 0, 1)});
    p.product_time.reset();
    BOOST_TEST(st.apply(p, kT0).empty());
    BOOST_TEST(st.stats().no_product_time == 1u);
}

BOOST_AUTO_TEST_CASE(metar_replaced_by_newer_issued)
{
    FisbStore st;
    const TimePoint i1 = iso("2015-01-24T03:53:00Z"), i2 = iso("2015-01-24T04:53:00Z");
    auto ev = st.apply(textProduct(kT0, {text(TextRecord::metar, "KSFO", i1, "METAR KSFO 240353Z A")}), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].key == "metar:KSFO");
    BOOST_TEST((ev[0].text->expires == i1 + expiry::kMetar));
    // The same report again: nothing.
    BOOST_TEST(st.apply(textProduct(kT0, {text(TextRecord::metar, "KSFO", i1, "METAR KSFO 240353Z A")}), kT0)
                   .empty());
    ev = st.apply(textProduct(kT0 + 1h, {text(TextRecord::metar, "KSFO", i2, "METAR KSFO 240453Z B")}), kT0 + 1h);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    // An older one does not come back.
    BOOST_TEST(
        st.apply(textProduct(kT0 + 1h, {text(TextRecord::metar, "KSFO", i1, "METAR KSFO 240353Z A")}), kT0 + 1h)
            .empty());
    // A TAF for the same station is a separate record.
    st.apply(textProduct(kT0 + 1h, {text(TextRecord::taf, "KSFO", i2, "TAF KSFO 240440Z 2406/2512 ...")}),
             kT0 + 1h);
    auto s = st.snapshot();
    BOOST_REQUIRE_EQUAL(s.metar.size(), 1u);
    BOOST_TEST(s.metar[0].text == "METAR KSFO 240453Z B");
    BOOST_TEST(s.taf.size() == 1u);
}

BOOST_AUTO_TEST_CASE(winds_keyed_by_period)
{
    FisbStore st;
    const TimePoint v1 = iso("2015-01-24T06:00:00Z"), v2 = iso("2015-01-24T12:00:00Z");
    st.apply(textProduct(kT0, {text(TextRecord::winds, "SFO", v1, "WINDS SFO 240600Z a", uint8_t{6}),
                               text(TextRecord::winds, "SFO", v2, "WINDS SFO 241200Z b", uint8_t{12})}),
             kT0);
    auto s = st.snapshot();
    BOOST_TEST(s.winds.size() == 2u);
    // The next 6 h issue replaces only the 6 h forecast.
    auto ev = st.apply(textProduct(kT0 + 6h, {text(TextRecord::winds, "SFO", v2, "WINDS SFO 241200Z c",
                                                   uint8_t{6})}),
                       kT0 + 6h);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(ev[0].key == "winds:SFO:6");
    s = st.snapshot();
    BOOST_REQUIRE_EQUAL(s.winds.size(), 2u);
    std::vector<std::string> texts{s.winds[0].text, s.winds[1].text};
    std::sort(texts.begin(), texts.end());
    BOOST_TEST(texts[0] == "WINDS SFO 241200Z b");
    BOOST_TEST(texts[1] == "WINDS SFO 241200Z c");
}

BOOST_AUTO_TEST_CASE(pirep_set_expires)
{
    FisbStore st;
    const TimePoint i = iso("2015-01-24T04:00:00Z");
    auto ev = st.apply(textProduct(kT0, {text(TextRecord::pirep, "SFO", i, "PIREP SFO UA /OV SFO /TM 0400 a"),
                                         text(TextRecord::pirep, "SFO", i, "PIREP SFO UA /OV SFO /TM 0400 b")}),
                       kT0);
    BOOST_TEST(ev.size() == 2u);
    // A repeat adds nothing.
    BOOST_TEST(
        st.apply(textProduct(kT0, {text(TextRecord::pirep, "SFO", i, "PIREP SFO UA /OV SFO /TM 0400 a")}), kT0)
            .empty());
    BOOST_TEST(st.snapshot().pirep.size() == 2u);
    BOOST_TEST(st.sweep(i + 120min - 1s).empty());
    ev = st.sweep(i + 120min);
    BOOST_TEST(countKind(ev, FisbEvent::Kind::expired) == 2u);
    BOOST_TEST(st.snapshot().pirep.empty());
    // A PIREP already past its lifetime is not stored.
    BOOST_TEST(st.apply(textProduct(i + 3h, {text(TextRecord::pirep, "SFO", i, "late")}), i + 3h).empty());
    BOOST_TEST(st.stats().stale == 1u);
}

// Every row of §10.3.
BOOST_AUTO_TEST_CASE(expiry_table_values)
{
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    // Rasters: from product time.
    for (int id : {63, 64, 70, 71, 84, 90, 91, 103}) {
        const auto pid = static_cast<uint16_t>(id);
        FisbStore st;
        const uint8_t scale = (pid == 63 || pid == 84 || pid == 103) ? 0 : 1;
        const uint32_t bn = scale == 0 ? 277546u : 139405u;
        std::optional<uint8_t> alt;
        if (pid == 70 || pid == 71 || pid == 90 || pid == 91)
            alt = 1;
        st.apply(blocks(pid, pt, kT0, {block(bn, scale, 1)}, {}, scale, alt), kT0);
        // NEXRAD and lightning 75 min (reference §7); icing, turbulence, cloud tops 105.
        const auto life =
            (pid == 63 || pid == 64 || pid == 103) ? std::chrono::minutes(75) : std::chrono::minutes(105);
        BOOST_TEST_CONTEXT("product " << pid)
        {
            const auto snap = st.snapshot();
            const Mosaic* m = findMosaic(snap, pid);
            BOOST_REQUIRE(m);
            BOOST_TEST((m->expires == pt + life));
            auto before = st.sweep(pt + life - 1s);
            BOOST_TEST(countKind(before, FisbEvent::Kind::expired) == 0u);
            auto at = st.sweep(pt + life);
            BOOST_TEST(countKind(at, FisbEvent::Kind::expired) == 1u);
            BOOST_TEST(!findMosaic(st.snapshot(), pid));
        }
    }
    BOOST_TEST(expiry::kNexrad.count() == 75);
    BOOST_TEST(expiry::kOtherRaster.count() == 105);

    // Text.
    const TimePoint issued = iso("2015-01-24T04:00:00Z");
    BOOST_TEST((textExpiry(text(TextRecord::metar, "KSFO", issued, "METAR")) == issued + 120min));
    BOOST_TEST((textExpiry(text(TextRecord::pirep, "SFO", issued, "PIREP")) == issued + 120min));
    BOOST_TEST((textExpiry(text(TextRecord::winds, "SFO", issued, "WINDS", uint8_t{6})) == issued + 12h));
    {
        // Winds: 12 h from the forecast valid time, not from the APDU issue time.
        TextRecord w = text(TextRecord::winds, "SFO", issued, "WINDS SFO 241800Z", uint8_t{12});
        w.valid = iso("2015-01-24T18:00:00Z");
        BOOST_TEST((textExpiry(w) == iso("2015-01-25T06:00:00Z")));
    }
    BOOST_TEST((textExpiry(text(TextRecord::taf, "KSFO", issued, "TAF KSFO 240340Z 2406/2512 27010KT")) ==
                iso("2015-01-25T12:00:00Z")));
    BOOST_TEST((textExpiry(text(TextRecord::taf, "KSFO", issued, "TAF KSFO 240340Z 2406/2424 27010KT")) ==
                iso("2015-01-25T00:00:00Z")));  // hour 24
    BOOST_TEST((textExpiry(text(TextRecord::taf, "KSFO", iso("2015-01-31T23:40:00Z"),
                                "TAF KSFO 312340Z 0100/0206 27010KT")) == iso("2015-02-02T06:00:00Z")));
    BOOST_TEST((textExpiry(text(TextRecord::taf, "KSFO", issued, "TAF AMD KSFO NIL")) == issued + 120min));

    // TWGO: 70 min after the last reception; an end time wins; unmatched 720.
    {
        FisbStore st;
        st.apply(twgoText(8, 1, "NOTAM-D a", kT0), kT0);
        st.apply(twgoText(13, 2, "SUA b", kT0), kT0);
        st.apply(twgoGraphics(13, 2, {square(1, 1)}, kT0, 15, kT0 + 3h), kT0);
        st.apply(twgoGraphics(8, 3, {square(1, 1)}, kT0), kT0);  // never gets text
        auto s = st.snapshot();
        BOOST_TEST((findTwgo(s, 1)->expires == kT0 + 70min));
        BOOST_TEST((findTwgo(s, 2)->expires == kT0 + 3h));
        BOOST_TEST(!findTwgo(s, 3));
        BOOST_TEST(st.sweep(kT0 + 70min).size() == 1u);
        BOOST_TEST(st.stats().twgo_pending == 1u);
        st.sweep(kT0 + 720min - 1s);
        BOOST_TEST(st.stats().twgo_pending == 1u);
        BOOST_TEST(st.sweep(kT0 + 720min).empty());  // never published: removed silently
        BOOST_TEST(st.stats().twgo_pending == 0u);
    }
    BOOST_TEST(expiry::kCancelled.count() == 30);
    BOOST_TEST(expiry::kTwgoUnmatched.count() == 720);
}

BOOST_AUTO_TEST_CASE(twgo_expiry_latest_end_capped_by_reception)
{
    BOOST_TEST((notamTextEnd("!EYE 10/028 EYE RWY 03/21 CLSD 2011010100-2011011100") == iso("2020-11-01T11:00:00Z")));
    BOOST_TEST((notamTextEnd("!CVG 0/3733 ODP 2010162111-2107162111EST") == iso("2021-07-16T21:11:00Z")));
    BOOST_TEST(!notamTextEnd("!FDC 0/1 ZOB AIRSPACE 2010162111-PERM").has_value());
    BOOST_TEST(!notamTextEnd("NOTAM-D a").has_value());
    BOOST_TEST(!notamTextEnd("ASN 12345678901-2107162111").has_value());  // 11 digits: no group

    FisbStore st;
    const std::string body = "NOTAM-FDC KCVG.0/3733 Z !FDC 0/3733 CVG ODP 1501162111-1507162111";
    // A text end months ahead: 70 min after the last reception comes first.
    auto ev = st.apply(twgoText(8, 3733, body, kT0), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].twgo->text_end == iso("2015-07-16T21:11:00Z")));
    BOOST_TEST((ev[0].expires == kT0 + 70min));
    const TimePoint g_end = iso("2015-01-24T05:00:00Z");
    ev = st.apply(twgoGraphics(8, 3733, {square(39, -84)}, kT0 + 10s, 15, g_end), kT0 + 10s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::twgo));
    BOOST_TEST((ev[0].twgo->graphics_end == g_end));
    BOOST_TEST((ev[0].twgo->expires == kT0 + 10s + 70min));  // the text end is the latest end
    BOOST_TEST(st.sweep(g_end + 1min).empty());  // still held after the graphics end

    // An end before the 70 min: the end.
    ev = st.apply(twgoText(8, 1, "NOTAM-D !X 1/1 X RWY CLSD 1501240400-1501240500", kT0), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].expires == iso("2015-01-24T05:00:00Z")));
    // A later graphics end makes the end later; the 70 min still caps it.
    ev = st.apply(twgoGraphics(8, 1, {square(1, 1)}, kT0, 15, iso("2015-01-24T08:00:00Z")), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].twgo->expires == kT0 + 70min));

    // No end at all (PERM, non-NOTAM text): 70 min after the last reception.
    ev = st.apply(twgoText(8, 2, "NOTAM-FDC !FDC 0/2 AIRSPACE 1501240400-PERM", kT0), kT0);
    BOOST_TEST((ev.at(0).expires == kT0 + 70min));
    BOOST_TEST(!ev[0].twgo->text_end.has_value());
    // The text end is read for NOTAM products only.
    ev = st.apply(twgoText(11, 4, "AIRMET 1501240400-1501250400", kT0), kT0);
    BOOST_TEST((ev.at(0).expires == kT0 + 70min));
    // SUA keeps its end, however far.
    st.apply(twgoText(13, 5, "SUA b", kT0), kT0);
    ev = st.apply(twgoGraphics(13, 5, {square(1, 1)}, kT0, 15, kT0 + 6h), kT0);
    BOOST_TEST((ev.at(0).twgo->expires == kT0 + 6h));

    auto j = skycued::JsonObject::parse(twgoRecordJson(*findTwgo(st.snapshot(), 3733)));
    BOOST_REQUIRE(j.has_value());
    BOOST_TEST(*j->getString("text_end") == "2015-07-16T21:11:00.000Z");
    BOOST_TEST(*j->getString("graphics_end") == "2015-01-24T05:00:00.000Z");
    BOOST_TEST(!j->getString("end").has_value());
}

namespace {
FisbProduct withYear(FisbProduct p, uint8_t year)
{
    std::get<TwgoParsed>(p.body).key.year = year;
    return p;
}
}  // namespace

// Final review: a report of another year is not current (a stuck SIGMET, a
// foreign-year recording); across New Year the neighbouring year is allowed.
BOOST_AUTO_TEST_CASE(twgo_wrong_year_rejected)
{
    FisbStore st;  // kT0: 2015-01-24
    BOOST_TEST(st.apply(withYear(twgoText(12, 1, "SIGMET a", kT0), 15), kT0).size() == 1u);
    BOOST_TEST(st.apply(withYear(twgoText(12, 2, "SIGMET b", kT0), 14), kT0).size() == 1u);  // January
    BOOST_TEST(st.apply(withYear(twgoText(12, 3, "SIGMET c", kT0), 13), kT0).empty());
    BOOST_TEST(st.apply(withYear(twgoText(12, 4, "SIGMET d", kT0), 16), kT0).empty());
    BOOST_TEST(st.apply(withYear(twgoText(8, 5, "NOTAM-TFR e", kT0), 5), kT0).size() == 1u);  // one digit
    BOOST_TEST(st.apply(withYear(twgoText(12, 6, "SIGMET f", kT0), 5), kT0).empty());
    // Graphics, cancels and keep-alives of another year are dropped as well.
    BOOST_TEST(st.apply(withYear(twgoGraphics(14, 7, {square(1, 1)}, kT0), 13), kT0).empty());
    BOOST_TEST(st.apply(withYear(twgoText(12, 1, "", kT0, true), 13), kT0).empty());
    BOOST_TEST(st.stats().wrong_year == 5u);
    BOOST_TEST(st.snapshot().twgo.size() == 3u);
    BOOST_TEST(!findTwgo(st.snapshot(), 3));

    const TimePoint dec = iso("2015-12-31T23:00:00Z"), jun = iso("2015-06-15T12:00:00Z");
    FisbStore d;
    BOOST_TEST(d.apply(withYear(twgoText(11, 1, "AIRMET a", dec), 16), dec).size() == 1u);  // December
    BOOST_TEST(d.apply(withYear(twgoText(11, 2, "AIRMET b", dec), 14), dec).empty());
    BOOST_TEST(d.apply(withYear(twgoText(11, 3, "AIRMET c", jun), 14), jun).empty());
    BOOST_TEST(d.apply(withYear(twgoText(11, 4, "AIRMET d", jun), 16), jun).empty());
    BOOST_TEST(d.stats().wrong_year == 3u);
}

// Final review: a start still ahead is published pending, then again when it
// comes into force.
BOOST_AUTO_TEST_CASE(twgo_future_start_pending_then_active)
{
    FisbStore st;
    auto ev = st.apply(twgoText(12, 77, "SIGMET 77 VALID 240512/240912", kT0), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST(!ev[0].twgo->pending);
    FisbProduct g = twgoGraphics(12, 77, {square(39, -100)}, kT0 + 1s, 15, kT0 + 4h);
    std::get<TwgoParsed>(g.body).start = kT0 + 1h;
    ev = st.apply(g, kT0 + 1s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::twgo));
    BOOST_TEST(ev[0].twgo->pending);
    auto j = skycued::JsonObject::parse(twgoRecordJson(*ev[0].twgo));
    BOOST_REQUIRE(j.has_value());
    BOOST_TEST(*j->getString("pending") == "true");
    // A repeat while pending changes nothing.
    g.received = kT0 + 10min;
    BOOST_TEST(st.apply(g, kT0 + 10min).empty());
    BOOST_TEST(st.sweep(kT0 + 1h - 1s).empty());
    ev = st.sweep(kT0 + 1h);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::twgo));
    BOOST_TEST(!ev[0].twgo->pending);
    BOOST_TEST(ev[0].twgo->has_graphics);
    BOOST_TEST(!findTwgo(st.snapshot(), 77)->pending);
    BOOST_TEST(st.sweep(kT0 + 1h + 1s).empty());  // once
    // Received again after its start: no longer pending, no event.
    g.received = kT0 + 61min;
    BOOST_TEST(st.apply(g, kT0 + 61min).empty());
}

// Final review: SIGMET (and AIRMET, G-AIRMET, CWA) end 70 min after the last
// reception even when the graphics end is later.
BOOST_AUTO_TEST_CASE(sigmet_lifetime_capped_by_last_reception)
{
    for (uint16_t pid : {uint16_t{11}, uint16_t{12}, uint16_t{14}, uint16_t{15}}) {
        BOOST_TEST_CONTEXT("product " << pid)
        {
            FisbStore st;
            if (pid != 14)
                st.apply(twgoText(pid, 9, "WX a", kT0), kT0);
            auto ev = st.apply(twgoGraphics(pid, 9, {square(39, -100)}, kT0, 15, kT0 + 4h), kT0);
            BOOST_REQUIRE_EQUAL(ev.size(), 1u);
            BOOST_TEST((ev[0].twgo->expires == kT0 + 70min));
            // Heard again at +60 min: renewed to +130.
            st.apply(twgoGraphics(pid, 9, {square(39, -100)}, kT0 + 60min, 15, kT0 + 4h), kT0 + 60min);
            BOOST_TEST((findTwgo(st.snapshot(), 9)->expires == kT0 + 130min));
            BOOST_TEST(st.sweep(kT0 + 130min - 1s).empty());
            ev = st.sweep(kT0 + 130min);
            BOOST_REQUIRE_EQUAL(ev.size(), 1u);
            BOOST_TEST(ev[0].reason == "expired");
            BOOST_TEST(!findTwgo(st.snapshot(), 9));
        }
    }
    // A graphics end before the 70 min still ends it first.
    FisbStore st;
    st.apply(twgoText(12, 10, "SIGMET b", kT0), kT0);
    auto ev = st.apply(twgoGraphics(12, 10, {square(39, -100)}, kT0, 15, kT0 + 30min), kT0);
    BOOST_TEST((ev.at(0).twgo->expires == kT0 + 30min));
}

// Final review: a TFR's keep-alives are what keeps it; without them it goes
// 70 min after the last one, whatever end its text gives.
BOOST_AUTO_TEST_CASE(tfr_expires_70min_after_last_keepalive_even_with_far_text_end)
{
    FisbStore st;
    const std::string body = "NOTAM-TFR 5/5116 240100Z ... 1501240400-1509010000";
    auto ev = st.apply(twgoText(8, 5116, body, kT0), kT0);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].twgo->text_end == iso("2015-09-01T00:00:00Z")));
    BOOST_TEST((ev[0].expires == kT0 + 70min));
    // Keep-alives (empty text) at +30 and +60 min renew it, silently.
    BOOST_TEST(st.apply(twgoText(8, 5116, "", kT0 + 30min), kT0 + 30min).empty());
    BOOST_TEST(st.apply(twgoText(8, 5116, "", kT0 + 60min), kT0 + 60min).empty());
    const auto snap = st.snapshot();
    const TwgoRecord* r = findTwgo(snap, 5116);
    BOOST_REQUIRE(r);
    BOOST_TEST((r->last_received == kT0 + 60min));
    BOOST_TEST((r->expires == kT0 + 130min));
    BOOST_TEST(*r->text == body);
    // Then nothing more: gone 70 min after the last keep-alive.
    BOOST_TEST(st.sweep(kT0 + 130min - 1s).empty());
    ev = st.sweep(kT0 + 130min);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    BOOST_TEST((ev[0].kind == FisbEvent::Kind::expired));
    BOOST_TEST(ev[0].reason == "expired");
    BOOST_TEST(!findTwgo(st.snapshot(), 5116));
}

BOOST_AUTO_TEST_CASE(summary_and_json_fields)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    st.apply(blocks(63, pt, kT0, {block(277546, 0, 1)}, {277547}), kT0);
    st.apply(textProduct(kT0, {text(TextRecord::metar, "KSFO", pt, "METAR KSFO \"q\"")}), kT0);
    st.apply(twgoText(8, 1, "NOTAM-D a", kT0), kT0);
    auto sum = st.summary(kT0 + 10s);
    BOOST_TEST(sum.mosaics == 1u);
    BOOST_TEST(sum.mosaic_blocks == 2u);
    BOOST_TEST(sum.text_records == 1u);
    BOOST_TEST(sum.twgo_records == 1u);
    BOOST_TEST(sum.stations == 1u);
    BOOST_REQUIRE(sum.radar_age_s.has_value());
    BOOST_TEST(*sum.radar_age_s == 130.0);
    BOOST_TEST(!sum.clock_skew_s.has_value());  // nothing ahead of the clock
    {
        // A product time ahead of the data clock is reported as skew.
        FisbStore ahead;
        ahead.apply(blocks(63, kT0 + 15min, kT0, {block(277546, 0, 1)}), kT0);
        const auto s2 = ahead.summary(kT0);
        BOOST_REQUIRE(s2.clock_skew_s.has_value());
        BOOST_TEST(*s2.clock_skew_s == 900.0);
    }

    auto ev = st.sweep(kT0 + 10s);
    BOOST_REQUIRE_EQUAL(ev.size(), 1u);
    const std::string line = eventRecordJson(ev[0], kT0 + 10s);
    auto j = skycued::JsonObject::parse(line);
    BOOST_REQUIRE(j.has_value());
    BOOST_TEST(*j->getString("type") == "fisb_radar");
    BOOST_TEST(*j->getString("time") == "2015-01-24T04:12:10.000Z");
    BOOST_TEST(*j->getInt("product") == 63);
    BOOST_TEST(*j->getInt("block_count") == 2);
    BOOST_TEST(*j->getString("product_time") == "2015-01-24T04:10:00.000Z");
    BOOST_TEST(*j->getInt("scale") == 0);
    BOOST_TEST(line.find("\"rle\":[[1,128]]") != std::string::npos);
    BOOST_TEST(line.find("\"rle\":[[0,128]]") != std::string::npos);
    BOOST_TEST(mosaicHeadJson(*ev[0].mosaic).find("blocks\"") == std::string::npos);

    std::array<uint8_t, 128> bins{};
    bins[126] = 1;
    bins[127] = 1;
    BOOST_TEST(rleJson(bins) == "[[0,126],[1,2]]");

    TextRecord tr = text(TextRecord::metar, "KSFO", pt, "METAR KSFO \"q\"");
    tr.expires = pt + 2h;
    auto tj = skycued::JsonObject::parse(textRecordJson(tr));
    BOOST_REQUIRE(tj.has_value());
    BOOST_TEST(*tj->getString("kind") == "metar");
    BOOST_TEST(*tj->getString("text") == "METAR KSFO \"q\"");
    BOOST_TEST(*tj->getString("key") == "metar:KSFO");
    BOOST_TEST(!tj->getString("valid").has_value());

    TextRecord wr = text(TextRecord::winds, "SFO", iso("2015-01-24T02:06:00Z"), "WINDS SFO 250000Z", uint8_t{24});
    wr.valid = iso("2015-01-25T00:00:00Z");
    wr.expires = textExpiry(wr);
    auto wj = skycued::JsonObject::parse(textRecordJson(wr));
    BOOST_REQUIRE(wj.has_value());
    BOOST_TEST(*wj->getString("issued") == "2015-01-24T02:06:00.000Z");
    BOOST_TEST(*wj->getString("valid") == "2015-01-25T00:00:00.000Z");
    BOOST_TEST(*wj->getString("expires") == "2015-01-25T12:00:00.000Z");
}

// The summary (what /api/state, the WebSocket snapshot and /api/fisb/status
// are built from) carries mosaic heads and counts without copying a block;
// mosaic() copies only the mosaic asked for.
BOOST_AUTO_TEST_CASE(status_does_not_copy_blocks)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    for (uint32_t i = 0; i < kMosaicMaxBlocks; ++i)
        st.apply(blocks(63, pt, kT0, {block(200000 + i, 0, 1)}), kT0);
    for (uint8_t code : {uint8_t{5}, uint8_t{2}})
        st.apply(blocks(70, pt, kT0, {block(249640, 1, code)}, {}, 1, code), kT0);
    st.apply(textProduct(kT0, {text(TextRecord::metar, "KSFO", pt, "METAR KSFO 240410Z"),
                               text(TextRecord::taf, "KSFO", pt, "TAF KSFO 240410Z")}),
             kT0);
    BOOST_REQUIRE_EQUAL(st.blocksCopied(), 0u);

    const FisbSummary sum = st.summary(kT0 + 1s);
    BOOST_TEST(st.blocksCopied() == 0u);
    BOOST_REQUIRE_EQUAL(sum.radar.size(), 1u);
    BOOST_TEST(sum.radar[0].key == "63");
    BOOST_TEST(sum.radar[0].block_count == kMosaicMaxBlocks);
    BOOST_TEST((sum.radar[0].product_time == pt));
    BOOST_REQUIRE_EQUAL(sum.rasters.size(), 2u);
    BOOST_TEST(sum.metar == 1u);
    BOOST_TEST(sum.taf == 1u);
    BOOST_TEST(sum.heard.size() == 1u);

    // One mosaic: only its blocks.
    const auto m = st.mosaic(63);
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->blocks.size() == kMosaicMaxBlocks);
    BOOST_TEST(st.blocksCopied() == kMosaicMaxBlocks);
    // A layered raster: the lowest altitude code unless one is named.
    const auto low = st.mosaic(70);
    BOOST_REQUIRE(low.has_value());
    BOOST_TEST(*low->altitude_code == 2u);
    BOOST_TEST(*st.mosaic(70, uint8_t{5})->altitude_code == 5u);
    BOOST_TEST(!st.mosaic(70, uint8_t{3}).has_value());
    BOOST_TEST(!st.mosaic(64).has_value());
    BOOST_TEST(st.blocksCopied() == kMosaicMaxBlocks + 2);

    // The records (what /api/fisb/text and /shapes read): no block.
    const auto before = st.blocksCopied();
    const FisbSnapshot rec = st.records();
    BOOST_TEST(st.blocksCopied() == before);
    BOOST_TEST(rec.radar.empty());
    BOOST_TEST(rec.rasters.empty());
    BOOST_TEST(rec.metar.size() == 1u);
    BOOST_TEST(rec.taf.size() == 1u);
    BOOST_TEST(rec.stations.size() == 1u);
    // The full snapshot copies everything.
    (void)st.snapshot();
    BOOST_TEST(st.blocksCopied() == before + kMosaicMaxBlocks + 2);
}

// Expired events say what left, without the key having to be parsed.
BOOST_AUTO_TEST_CASE(expired_events_carry_their_kind)
{
    FisbStore st;
    const TimePoint pt = iso("2015-01-24T04:10:00Z");
    st.apply(blocks(63, pt, kT0, {block(200000, 0, 1)}), kT0);
    st.apply(blocks(70, pt, kT0, {block(249640, 1, 1)}, {}, 1, uint8_t{0}), kT0);
    st.apply(textProduct(kT0, {text(TextRecord::metar, "KSFO", pt, "METAR KSFO 240410Z")}), kT0);
    st.apply(twgoText(8, 1, "NOTAM-D a", kT0), kT0);
    st.sweep(kT0 + 30s);  // publish the mosaics
    std::map<std::string, FisbEvent> expired;
    for (const auto& e : st.sweep(kT0 + 24h))
        if (e.kind == FisbEvent::Kind::expired)
            expired[e.key] = e;
    BOOST_REQUIRE_EQUAL(expired.size(), 4u);
    BOOST_TEST((expired.at("63").expired_kind == FisbEvent::Kind::radar));
    BOOST_TEST((expired.at("70-0").expired_kind == FisbEvent::Kind::raster));
    BOOST_TEST((expired.at("metar:KSFO").expired_kind == FisbEvent::Kind::text));
    BOOST_REQUIRE(expired.at("metar:KSFO").text.has_value());
    BOOST_TEST(expired.at("metar:KSFO").text->station == "KSFO");
    BOOST_TEST((expired.at(key(8, 1).str()).expired_kind == FisbEvent::Kind::twgo));

    // A cancel is a twgo expiry too.
    st.apply(twgoText(8, 2, "NOTAM-D b", kT0 + 24h), kT0 + 24h);
    const auto ev = st.apply(twgoText(8, 2, "", kT0 + 24h + 1s, true), kT0 + 24h + 1s);
    BOOST_REQUIRE_EQUAL(countKind(ev, FisbEvent::Kind::expired), 1u);
    BOOST_TEST((ev.back().expired_kind == FisbEvent::Kind::twgo));
}
