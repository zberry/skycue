#define BOOST_TEST_MODULE skycued_fisb_twgo
#include <boost/test/unit_test.hpp>

#include "restricted_fixtures.hpp"

#include "gz_line_reader.hpp"
#include "fisb/apdu.hpp"
#include "fisb/fisb_time.hpp"
#include "fisb/products.hpp"
#include "fisb/segment_assembler.hpp"
#include "fisb/text_products.hpp"
#include "fisb/twgo.hpp"
#include "fisb/uplink_frame.hpp"
#include "time_util.hpp"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace skycued::fisb;
using std::chrono::minutes;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;
const std::string kSample = kFixtures + "/raw/uat-sample.txt.gz";
const std::string kTg30 = kFixtures + "/fisb/tg30.978";

TimePoint iso(const std::string& s)
{
    auto t = skycued::parseIso8601Ms(s);
    BOOST_REQUIRE_MESSAGE(t, "bad time " << s);
    return *t;
}

// The sample has no receive times (reference §7); its dates are January 2015.
TimePoint sampleRef() { return iso("2015-01-24T12:00:00Z"); }
// tg30's t= fields run 2020-10-30 08:52-09:00.
TimePoint tg30Ref() { return iso("2020-10-30T08:55:00Z"); }

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

struct Apdu {
    ApduHeader h;
    std::vector<uint8_t> payload;
};

// Every FIS-B APDU in a fixture, in order.
std::vector<Apdu> fixtureApdus(const std::string& path)
{
    std::vector<Apdu> out;
    for (const auto& l : readLines(path)) {
        if (!isUplinkLine(l))
            continue;
        std::vector<uint8_t> b;
        if (!hexToBytes(l, b) || b.size() != kUplinkBytes)
            continue;
        auto uh = parseUplinkHeader(b.data(), b.size());
        if (!uh || !uh->app_data_valid)
            continue;
        forEachInfoFrame(b.data(), b.size(), [&](const InfoFrame& f) {
            if (f.type != 0)
                return;
            auto a = parseApdu(f.data, f.len, false);
            if (!a)
                return;
            out.push_back({*a, std::vector<uint8_t>(f.data + a->header_bytes, f.data + f.len)});
        });
    }
    return out;
}

struct TwgoItem {
    ApduHeader h;
    std::vector<uint8_t> payload;  // reassembled
    std::optional<TwgoParsed> parsed;
};

// Every TWGO product in a fixture after reassembly, parsed.
std::vector<TwgoItem> fixtureTwgo(const std::string& path, TimePoint ref, SegmentAssembler* sa_out = nullptr)
{
    SegmentAssembler sa;
    std::vector<TwgoItem> out;
    for (const auto& a : fixtureApdus(path)) {
        if (productFamily(a.h.product_id) != ProductFamily::twgo)
            continue;
        auto done = sa.add(a.h, a.payload.data(), a.payload.size(), ref);
        if (!done)
            continue;
        TwgoItem it{done->hdr, done->payload, std::nullopt};
        it.parsed = parseTwgo(done->hdr, done->payload.data(), done->payload.size(), ref);
        out.push_back(std::move(it));
    }
    if (sa_out)
        *sa_out = sa;
    return out;
}

// MSB-first bit writer.
struct BitWriter {
    std::vector<uint8_t> bytes;
    std::size_t bits = 0;
    void put(uint32_t v, unsigned n)
    {
        for (unsigned i = n; i-- > 0;) {
            if (bits % 8 == 0)
                bytes.push_back(0);
            if ((v >> i) & 1u)
                bytes.back() = static_cast<uint8_t>(bytes.back() | (0x80u >> (bits % 8)));
            ++bits;
        }
    }
};

// The inverse of reference §6.3: a negative longitude is stored + 360, a
// negative latitude + 180 (decoders fold values above 90 by subtracting 180).
uint32_t rawAngle(double deg, unsigned bits, bool lat = false)
{
    if (deg < 0)
        deg += lat ? 180.0 : 360.0;
    return static_cast<uint32_t>(std::lround(deg * static_cast<double>(1u << bits) / 360.0)) & ((1u << bits) - 1u);
}

struct GraphicSpec {
    uint32_t report = 100;
    uint8_t year = 26;
    uint8_t status = 15;
    uint8_t geometry = 3;
    bool param = false;
    uint8_t op = 0;
    uint8_t fmt = 1;  // date/time format; times are 11/02 10:00 to 11/02 12:30
    std::vector<std::tuple<double, double, int>> verts;  // lat, lon, alt ft (6-byte vertices)
    // 14-byte: lat, lon, z_bot ft, z_top ft, radius nm
    std::vector<std::tuple<double, double, int, int, double>> prisms;
};

// One format-8 record: numeric label, no qualifier, start and end times.
std::vector<uint8_t> graphicRecord(const GraphicSpec& s)
{
    BitWriter w;
    w.put(0, 10);  // length, patched below
    w.put(s.report, 14);
    w.put(s.year, 7);
    w.put(0, 2);
    w.put(0, 2);
    w.put(0, 4);
    w.put(0, 1);   // numeric label
    w.put(0, 16);  // label
    w.put(0, 1);
    w.put(0, 1);
    w.put(s.param ? 1u : 0u, 1);
    w.put(0, 5);
    w.put(14, 4);  // airspace
    w.put(s.status, 4);
    if (s.param)
        w.put(0, 16);
    w.put(3, 2);  // start and end
    w.put(s.fmt, 2);
    w.put(s.geometry, 4);
    const std::size_t nv = s.prisms.empty() ? s.verts.size() : s.prisms.size();
    w.put(s.op, 2);
    w.put(static_cast<uint32_t>(nv - 1), 6);
    const std::vector<uint32_t> start = {11, 2, 10, 0}, end = {11, 2, 12, 30};
    const std::size_t skip = s.fmt == 1 ? 0 : s.fmt == 2 ? 1 : s.fmt == 3 ? 2 : 4;
    for (const auto* t : {&start, &end})
        for (std::size_t k = skip; k < 4; ++k)
            w.put((*t)[k], 8);
    for (const auto& v : s.verts) {
        w.put(rawAngle(std::get<1>(v), 19), 19);
        w.put(rawAngle(std::get<0>(v), 19, true), 19);
        w.put(static_cast<uint32_t>(std::get<2>(v) / 100), 10);
    }
    for (const auto& p : s.prisms) {
        for (int k = 0; k < 2; ++k) {
            w.put(rawAngle(std::get<1>(p), 18), 18);
            w.put(rawAngle(std::get<0>(p), 18, true), 18);
        }
        w.put(static_cast<uint32_t>(std::get<2>(p) / 500), 7);
        w.put(static_cast<uint32_t>(std::get<3>(p) / 500), 7);
        const auto r = static_cast<uint32_t>(std::lround(std::get<4>(p) / 0.2));
        w.put(r, 9);
        w.put(r, 9);
        w.put(0, 8);
    }
    const auto len = static_cast<uint32_t>(w.bytes.size());
    w.bytes[0] = static_cast<uint8_t>(len >> 2);
    w.bytes[1] = static_cast<uint8_t>((w.bytes[1] & 0x3fu) | ((len & 3u) << 6));
    return w.bytes;
}

std::vector<uint8_t> textRecord(uint32_t report, uint8_t year, bool active, const std::vector<uint8_t>& dlac)
{
    BitWriter w;
    const auto len = static_cast<uint32_t>(5 + dlac.size());
    w.put(len, 16);
    w.put(report, 14);
    w.put(year, 7);
    w.put(active ? 1u : 0u, 1);
    w.put(0, 2);
    w.bytes.insert(w.bytes.end(), dlac.begin(), dlac.end());
    return w.bytes;
}

// TWGO payload: 6-byte header (location "ABC" or none), then the records.
std::vector<uint8_t> twgoPayload(uint8_t format, const std::vector<std::vector<uint8_t>>& recs, bool loc = false)
{
    std::vector<uint8_t> p = {static_cast<uint8_t>(format << 4), static_cast<uint8_t>(recs.size() << 4), 0, 0, 0, 0};
    if (loc) {  // DLAC A=1 B=2 C=3 ETX: 000001 000010 000011 000000
        p[2] = 0x04;
        p[3] = 0x20;
        p[4] = 0xc0;
    }
    for (const auto& r : recs)
        p.insert(p.end(), r.begin(), r.end());
    return p;
}

// DLAC for upper-case letters, digits, space and LF/RS.
std::vector<uint8_t> dlacOf(const std::string& s)
{
    BitWriter w;
    const std::string rest = "| !\"#$%&'()*+,-./0123456789:;<=>?";
    for (char c : s) {
        unsigned code;
        if (c >= 'A' && c <= 'Z')
            code = static_cast<unsigned>(c - 'A' + 1);
        else if (c == '\n')
            code = 30;
        else if (c == '\x1e')
            code = 29;
        else
            code = static_cast<unsigned>(31 + rest.find(c));
        w.put(code, 6);
    }
    w.put(0, 6);
    return w.bytes;
}

ApduHeader hdr(uint16_t pid, uint8_t hours = 10, uint8_t minutes = 0)
{
    ApduHeader h;
    h.product_id = pid;
    h.hours = hours;
    h.minutes = minutes;
    h.t_opt = 2;
    h.month = 11;
    h.day = 2;
    return h;
}

ApduHeader segHdr(uint16_t pid, uint16_t file, uint16_t num, uint16_t count)
{
    ApduHeader h = hdr(pid);
    h.segmented = h.s_flag = true;
    h.apdu_number = file;
    h.segment_number = num;
    h.segment_count = count;
    return h;
}

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

std::vector<Apdu> sampleNotamSegments()
{
    std::vector<Apdu> segs;
    for (auto& a : fixtureApdus(kSample))
        if (a.h.segmented)
            segs.push_back(std::move(a));
    return segs;
}

}  // namespace

BOOST_AUTO_TEST_CASE(utc_resolution_picks_the_closest_candidate)
{
    const auto ref = iso("2015-01-01T00:10:00Z");
    BOOST_TEST(skycued::formatIso8601Ms(*resolveUtc(ref, std::nullopt, std::nullopt, 23, 50)) ==
               "2014-12-31T23:50:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*resolveUtc(ref, 12u, 31u, 22, 0)) == "2014-12-31T22:00:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*resolveUtc(ref, std::nullopt, 31u, 8, 0)) == "2014-12-31T08:00:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*resolveUtc(ref, std::nullopt, 1u, 24, 0)) == "2015-01-02T00:00:00.000Z");
    BOOST_TEST(!resolveUtc(ref, 2u, 30u, 0, 0).has_value());
    BOOST_TEST(!resolveUtc(ref, 13u, 1u, 0, 0).has_value());
    BOOST_TEST(!resolveUtc(ref, std::nullopt, std::nullopt, 25, 0).has_value());
    BOOST_TEST(!resolveUtc(ref, std::nullopt, std::nullopt, 10, 60).has_value());
    // 12 h either side: the earlier candidate wins.
    BOOST_TEST(skycued::formatIso8601Ms(*resolveUtc(iso("2015-01-10T12:00:00Z"), std::nullopt, std::nullopt, 0, 0)) ==
               "2015-01-10T00:00:00.000Z");
}

BOOST_AUTO_TEST_CASE(segmented_notam_from_sample_reassembles, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    auto segs = sampleNotamSegments();
    BOOST_REQUIRE_EQUAL(segs.size(), 3u);
    std::sort(segs.begin(), segs.end(),
              [](const Apdu& a, const Apdu& b) { return a.h.segment_number < b.h.segment_number; });
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST(segs[i].h.product_id == 8u);
        BOOST_TEST(segs[i].h.apdu_number == 739u);
        BOOST_TEST(segs[i].h.segment_count == 3u);
        BOOST_TEST(segs[i].h.segment_number == i + 1);
    }
    const auto t0 = sampleRef();
    const std::size_t want = segs[0].payload.size() + segs[1].payload.size() + segs[2].payload.size() - 12;

    std::vector<std::vector<std::size_t>> orders = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}, {0, 0, 2, 2, 1}};
    std::vector<uint8_t> first_result;
    for (const auto& order : orders) {
        SegmentAssembler sa;
        std::optional<SegmentAssembler::Done> done;
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto& s = segs[order[k]];
            auto d = sa.add(s.h, s.payload.data(), s.payload.size(), t0);
            if (k + 1 < order.size())
                BOOST_TEST(!d.has_value());
            else
                done = d;
        }
        BOOST_REQUIRE(done);
        BOOST_TEST(sa.open() == 0u);
        BOOST_TEST(done->hdr.segment_number == 1u);
        BOOST_TEST(done->payload.size() == want);
        // Segment 1 whole, then 2 and 3 without their repeated TWGO header.
        BOOST_TEST(std::equal(segs[0].payload.begin(), segs[0].payload.end(), done->payload.begin()));
        BOOST_TEST(std::equal(segs[1].payload.begin() + 6, segs[1].payload.end(),
                              done->payload.begin() + static_cast<std::ptrdiff_t>(segs[0].payload.size())));
        if (first_result.empty())
            first_result = done->payload;
        BOOST_TEST((done->payload == first_result));
        if (order.size() == 5)
            BOOST_TEST(sa.duplicates() == 2u);
    }

    auto t = parseTwgo(segs[0].h, first_result.data(), first_result.size(), t0);
    BOOST_REQUIRE(t);
    BOOST_REQUIRE(t->text);
    BOOST_TEST(t->text->rfind("NOTAM-TFR 5/4342 152352Z PART 1 OF 2", 0) == 0u);
    BOOST_TEST(t->text->find("BEALE AFB") != std::string::npos);
    BOOST_REQUIRE_EQUAL(t->records.size(), 1u);
    BOOST_TEST(t->records[0].declared == first_result.size() - 6);
    BOOST_TEST(t->leftover == 0u);
    BOOST_TEST(t->key.report == 4342u);
    BOOST_TEST(t->key.year == 5u);  // one digit for NOTAM-TFR
    BOOST_TEST(!t->has_graphics);
    BOOST_TEST(!t->cancel);

    // Walking the whole sample completes exactly this one set.
    SegmentAssembler sa;
    auto items = fixtureTwgo(kSample, t0, &sa);
    BOOST_TEST(sa.open() == 0u);
    int tfr = 0;
    for (const auto& it : items)
        if (it.h.segmented) {
            ++tfr;
            BOOST_TEST((it.payload == first_result));
        }
    BOOST_TEST(tfr == 1);
}

BOOST_AUTO_TEST_CASE(segments_complete_on_later_cycle, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    auto segs = sampleNotamSegments();
    BOOST_REQUIRE_EQUAL(segs.size(), 3u);
    std::sort(segs.begin(), segs.end(),
              [](const Apdu& a, const Apdu& b) { return a.h.segment_number < b.h.segment_number; });
    const auto t0 = sampleRef();
    SegmentAssembler sa;
    BOOST_TEST(!sa.add(segs[0].h, segs[0].payload.data(), segs[0].payload.size(), t0));
    BOOST_TEST(!sa.add(segs[2].h, segs[2].payload.data(), segs[2].payload.size(), t0));
    BOOST_TEST(sa.open() == 1u);
    // The next cycle repeats segment 1 (ignored) and finally brings segment 2.
    BOOST_TEST(sa.sweep(t0 + minutes(15)) == 0u);
    BOOST_TEST(!sa.add(segs[0].h, segs[0].payload.data(), segs[0].payload.size(), t0 + minutes(15)));
    auto d = sa.add(segs[1].h, segs[1].payload.data(), segs[1].payload.size(), t0 + minutes(15));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->payload.size() == segs[0].payload.size() + segs[1].payload.size() + segs[2].payload.size() - 12);
    BOOST_TEST(sa.open() == 0u);
    BOOST_TEST(sa.timedOut() == 0u);
}

BOOST_AUTO_TEST_CASE(segment_timeout_60min)
{
    const auto t0 = sampleRef();
    const std::vector<uint8_t> pl = {0x22, 0x10, 0, 0, 0, 0xff, 1, 2, 3};
    SegmentAssembler sa;
    BOOST_TEST(!sa.add(segHdr(8, 5, 1, 3), pl.data(), pl.size(), t0));
    BOOST_TEST(!sa.add(segHdr(8, 6, 1, 2), pl.data(), pl.size(), t0 + minutes(30)));
    BOOST_TEST(sa.open() == 2u);
    BOOST_TEST(sa.sweep(t0 + minutes(60)) == 0u);  // exactly 60 min: still open
    BOOST_TEST(sa.sweep(t0 + minutes(61)) == 1u);
    BOOST_TEST(sa.open() == 1u);
    BOOST_TEST(sa.timedOut() == 1u);
    // The dropped set does not complete from its remaining segments alone.
    BOOST_TEST(!sa.add(segHdr(8, 5, 2, 3), pl.data(), pl.size(), t0 + minutes(62)));
    BOOST_TEST(!sa.add(segHdr(8, 5, 3, 3), pl.data(), pl.size(), t0 + minutes(62)));
    BOOST_TEST(sa.open() == 2u);
    // The second set (file 6) is still there and completes.
    auto d = sa.add(segHdr(8, 6, 2, 2), pl.data(), pl.size(), t0 + minutes(62));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->payload.size() == pl.size() * 2 - 6);
    // A custom timeout.
    BOOST_TEST(sa.sweep(t0 + minutes(62) + minutes(11), minutes(10)) == 1u);
    BOOST_TEST(sa.open() == 0u);
    BOOST_TEST(sa.timedOut() == 2u);
}

BOOST_AUTO_TEST_CASE(segment_cap_512_and_rejects)
{
    const auto t0 = sampleRef();
    const std::vector<uint8_t> pl = {0x22, 0x10, 0, 0, 0, 0xff, 9};
    SegmentAssembler sa;
    for (uint16_t f = 0; f < 513; ++f)
        BOOST_TEST(!sa.add(segHdr(14, f, 1, 2), pl.data(), pl.size(), t0 + std::chrono::seconds(f)));
    BOOST_TEST(sa.open() == 512u);
    BOOST_TEST(sa.evicted() == 1u);
    // File 0 (the oldest) was evicted; file 1 still completes.
    BOOST_TEST(sa.add(segHdr(14, 1, 2, 2), pl.data(), pl.size(), t0).has_value());
    BOOST_TEST(sa.open() == 511u);
    BOOST_TEST(!sa.add(segHdr(14, 0, 2, 2), pl.data(), pl.size(), t0));  // a new set for file 0
    BOOST_TEST(sa.open() == 512u);
    BOOST_TEST(sa.evicted() == 1u);
    BOOST_TEST(!sa.add(segHdr(14, 600, 1, 2), pl.data(), pl.size(), t0));  // full again: evicts file 2
    BOOST_TEST(sa.evicted() == 2u);
    BOOST_TEST(!sa.add(segHdr(14, 2, 2, 2), pl.data(), pl.size(), t0));

    SegmentAssembler r;
    BOOST_TEST(!r.add(segHdr(8, 1, 0, 3), pl.data(), pl.size(), t0));
    BOOST_TEST(!r.add(segHdr(8, 1, 4, 3), pl.data(), pl.size(), t0));
    BOOST_TEST(!r.add(segHdr(8, 1, 1, 0), pl.data(), pl.size(), t0));
    BOOST_TEST(!r.add(segHdr(8, 1, 2, 3), pl.data(), 5, t0));  // shorter than the TWGO header
    BOOST_TEST(r.rejected() == 4u);
    BOOST_TEST(r.open() == 0u);
    // A changed segment count restarts the set.
    BOOST_TEST(!r.add(segHdr(8, 1, 1, 3), pl.data(), pl.size(), t0));
    BOOST_TEST(!r.add(segHdr(8, 1, 2, 2), pl.data(), pl.size(), t0));
    BOOST_TEST(r.open() == 1u);
    BOOST_TEST(r.add(segHdr(8, 1, 1, 2), pl.data(), pl.size(), t0).has_value());
    // Unsegmented passes through.
    auto u = r.add(hdr(413), pl.data(), pl.size(), t0);
    BOOST_REQUIRE(u);
    BOOST_TEST((u->payload == pl));
}

BOOST_AUTO_TEST_CASE(twgo_every_record_in_both_fixtures_consumes_exact_length,
                     *skycue_test::needs_fixture("raw/uat-sample.txt.gz")
                     *skycue_test::needs_fixture("fisb/tg30.978"))
{
    struct Want {
        std::string path;
        TimePoint ref;
        std::size_t apdus;
        std::map<std::pair<uint16_t, int>, int> by_product;  // (pid, format) -> records
        std::map<int, int> by_geometry;
    };
    const std::vector<Want> wants = {
        {kSample, sampleRef(), 137,
         {{{8, 2}, 33}, {{8, 8}, 29}, {{11, 2}, 1}, {{11, 8}, 1}, {{12, 2}, 1}, {{12, 8}, 1}, {{13, 2}, 71}},
         {{3, 2}, {7, 1}, {9, 28}}},
        {kTg30, tg30Ref(), 951,
         {{{8, 2}, 217}, {{8, 8}, 191}, {{11, 2}, 122}, {{11, 8}, 110}, {{12, 2}, 11}, {{12, 8}, 9}, {{13, 2}, 35},
          {{14, 8}, 262}},
         {{3, 253}, {4, 38}, {9, 190}, {11, 77}, {12, 14}}},
    };
    for (const auto& w : wants) {
        auto items = fixtureTwgo(w.path, w.ref);
        std::map<std::pair<uint16_t, int>, int> by_product;
        std::map<int, int> by_geometry;
        std::size_t records = 0;
        for (const auto& it : items) {
            BOOST_REQUIRE_MESSAGE(it.parsed, w.path << ": product " << it.h.product_id << " did not parse");
            const auto& t = *it.parsed;
            BOOST_TEST(t.leftover == 0u);
            BOOST_TEST(t.records.size() == static_cast<std::size_t>(it.payload[1] >> 4));
            std::size_t sum = 6;
            for (const auto& r : t.records) {
                BOOST_TEST(r.consumed == r.declared);
                BOOST_TEST(!r.ignored);
                sum += r.declared;
                ++by_product[{it.h.product_id, r.format}];
                if (r.format == 8)
                    ++by_geometry[r.geometry];
                ++records;
            }
            BOOST_TEST(sum == it.payload.size());
            BOOST_TEST(t.has_graphics == (t.record_format == 8));
            BOOST_TEST(t.text.has_value() == (t.record_format == 2));
            if (t.has_graphics)
                BOOST_TEST(!t.shapes.empty());
        }
        std::ostringstream os;
        for (const auto& kv : by_product)
            os << " " << kv.first.first << "/fmt" << kv.first.second << "=" << kv.second;
        for (const auto& kv : by_geometry)
            os << " geom" << kv.first << "=" << kv.second;
        BOOST_TEST_MESSAGE(w.path << ": " << items.size() << " TWGO products, " << records << " records:" << os.str());
        BOOST_TEST(items.size() == w.apdus);
        BOOST_TEST((by_product == w.by_product));
        BOOST_TEST((by_geometry == w.by_geometry));
    }
}

BOOST_AUTO_TEST_CASE(twgo_polygon_circle_prism_geometry,
                     *skycue_test::needs_fixture("raw/uat-sample.txt.gz")
                     *skycue_test::needs_fixture("fisb/tg30.978"))
{
    const auto ref = sampleRef();
    auto items = fixtureTwgo(kSample, ref);
    const TwgoParsed *sigmet = nullptr, *airmet = nullptr, *ksck = nullptr, *tfr = nullptr;
    for (const auto& it : items) {
        const auto& t = *it.parsed;
        if (!t.has_graphics)
            continue;
        if (t.key.pid == 12)
            sigmet = &t;
        if (t.key.pid == 11)
            airmet = &t;
        if (t.key.pid == 8 && t.label == "KSCK" && t.key.report == 12045)  // reference §6.3's example
            ksck = &t;
        if (t.key.pid == 8 && t.records[0].geometry == 7)
            tfr = &t;
    }
    BOOST_REQUIRE(sigmet);
    BOOST_REQUIRE(airmet);
    BOOST_REQUIRE(ksck);
    BOOST_REQUIRE(tfr);

    // SIGMET: one closed 6-vertex polygon at 10000 ft MSL, 1/24 02:23-06:23.
    BOOST_REQUIRE_EQUAL(sigmet->shapes.size(), 1u);
    const auto& sp = sigmet->shapes[0];
    BOOST_TEST(sp.geom == Shape::polygon);
    BOOST_TEST(!sp.agl);
    BOOST_REQUIRE_EQUAL(sp.points.size(), 6u);
    BOOST_TEST(near(sp.points[0].first, 34.817734));
    BOOST_TEST(near(sp.points[0].second, -117.869568));
    BOOST_TEST(near(sp.points[2].first, 32.427521));
    BOOST_TEST(near(sp.points[2].second, -116.805960));
    BOOST_TEST((sp.points.front() == sp.points.back()));
    BOOST_TEST(*sp.alt_low_ft == 10000);
    BOOST_TEST(*sp.alt_high_ft == 10000);
    BOOST_TEST(skycued::formatIso8601Ms(*sigmet->start) == "2015-01-24T02:23:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*sigmet->end) == "2015-01-24T06:23:00.000Z");
    BOOST_TEST(sigmet->object_status == 15u);
    BOOST_TEST(!sigmet->cancel);

    // AIRMET: a 7-point ring at 43000 ft and again at 18000 ft -> one polygon 18000-43000.
    BOOST_TEST(airmet->records[0].vertices == 14u);
    BOOST_REQUIRE_EQUAL(airmet->shapes.size(), 1u);
    const auto& ap = airmet->shapes[0];
    BOOST_TEST(ap.geom == Shape::polygon);
    BOOST_REQUIRE_EQUAL(ap.points.size(), 7u);
    BOOST_TEST(near(ap.points[1].first, 33.453369));
    BOOST_TEST(near(ap.points[1].second, -122.721405));
    BOOST_TEST(*ap.alt_low_ft == 18000);
    BOOST_TEST(*ap.alt_high_ft == 43000);

    // NOTAM-D KSCK: a point AGL at 0 ft, 1/23 16:18 to 1/24 23:59.
    BOOST_REQUIRE_EQUAL(ksck->shapes.size(), 1u);
    const auto& kp = ksck->shapes[0];
    BOOST_TEST(kp.geom == Shape::point);
    BOOST_TEST(kp.agl);
    BOOST_TEST(near(kp.points[0].first, 37.893906));
    BOOST_TEST(near(kp.points[0].second, -121.237564));
    BOOST_TEST(*kp.alt_high_ft == 0);
    BOOST_TEST(skycued::formatIso8601Ms(*ksck->start) == "2015-01-23T16:18:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*ksck->end) == "2015-01-24T23:59:00.000Z");

    // TFR: a circular prism MSL, 10 NM, surface to 18000 ft.
    BOOST_REQUIRE_EQUAL(tfr->shapes.size(), 1u);
    const auto& tp = tfr->shapes[0];
    BOOST_TEST(tp.geom == Shape::prism);
    BOOST_TEST(!tp.agl);
    BOOST_REQUIRE_EQUAL(tp.points.size(), 1u);
    BOOST_TEST(near(tp.points[0].first, 39.133301));
    BOOST_TEST(near(tp.points[0].second, -121.437378));
    BOOST_TEST(near(tp.radius_nm, 10.0));
    BOOST_TEST(*tp.alt_low_ft == 0);
    BOOST_TEST(*tp.alt_high_ft == 18000);
    BOOST_TEST(tfr->key.report == 6095u);
    BOOST_TEST(skycued::formatIso8601Ms(*tfr->start) == "2015-01-27T22:00:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*tfr->end) == "2015-01-28T20:00:00.000Z");

    // Hand-built: a flat circle (bottom == top), two prisms in one record ->
    // two shapes, and a southern / eastern 19-bit vertex.
    const auto ref2 = iso("2026-11-02T09:00:00Z");
    GraphicSpec c;
    c.geometry = 8;  // prism AGL
    c.prisms = {std::make_tuple(40.5, -100.25, 0, 0, 5.0), std::make_tuple(41.0, -99.0, 1000, 3000, 2.4)};
    auto pc = twgoPayload(8, {graphicRecord(c)});
    auto tc = parseTwgo(hdr(8), pc.data(), pc.size(), ref2);
    BOOST_REQUIRE(tc);
    BOOST_TEST(tc->records[0].consumed == tc->records[0].declared);
    BOOST_REQUIRE_EQUAL(tc->shapes.size(), 2u);
    BOOST_TEST(tc->shapes[0].geom == Shape::circle);
    BOOST_TEST(tc->shapes[0].agl);
    BOOST_TEST(near(tc->shapes[0].points[0].first, 40.5, 1e-3));
    BOOST_TEST(near(tc->shapes[0].points[0].second, -100.25, 1e-3));
    BOOST_TEST(near(tc->shapes[0].radius_nm, 5.0));
    BOOST_TEST(tc->shapes[1].geom == Shape::prism);
    BOOST_TEST(*tc->shapes[1].alt_low_ft == 1000);
    BOOST_TEST(*tc->shapes[1].alt_high_ft == 3000);
    BOOST_TEST(near(tc->shapes[1].radius_nm, 2.4));
    BOOST_TEST(skycued::formatIso8601Ms(*tc->start) == "2026-11-02T10:00:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*tc->end) == "2026-11-02T12:30:00.000Z");

    GraphicSpec e;
    e.geometry = 10;  // point MSL
    e.verts = {std::make_tuple(-33.9, 151.2, 500)};
    auto pe = twgoPayload(8, {graphicRecord(e)});
    auto te = parseTwgo(hdr(8), pe.data(), pe.size(), ref2);
    BOOST_REQUIRE(te);
    BOOST_REQUIRE_EQUAL(te->shapes.size(), 1u);
    BOOST_TEST(near(te->shapes[0].points[0].first, -33.9, 1e-3));
    BOOST_TEST(near(te->shapes[0].points[0].second, 151.2, 1e-3));
    BOOST_TEST(*te->shapes[0].alt_high_ft == 500);

    // tg30: G-AIRMET polylines longer than 64 vertices span two records and merge.
    int merged = 0;
    for (const auto& it : fixtureTwgo(kTg30, tg30Ref())) {
        const auto& t = *it.parsed;
        if (t.records.size() != 2)
            continue;
        ++merged;
        BOOST_TEST(t.records[0].vertices == 64u);
        BOOST_REQUIRE_EQUAL(t.shapes.size(), 1u);
        BOOST_TEST(t.shapes[0].geom == Shape::polyline);
        BOOST_TEST(t.shapes[0].points.size() == 64u + t.records[1].vertices - 1u);
    }
    BOOST_TEST(merged == 6);
}

BOOST_AUTO_TEST_CASE(twgo_key_fields, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    auto items = fixtureTwgo(kSample, sampleRef());
    std::map<TwgoKey, int> text_keys;
    const TwgoParsed* ksck = nullptr;
    const TwgoParsed* sua = nullptr;
    for (const auto& it : items) {
        const auto& t = *it.parsed;
        if (t.text)
            ++text_keys[t.key];
        if (t.has_graphics && t.label == "KSCK" && t.key.report == 12045)
            ksck = &t;
        if (!sua && t.key.pid == 13)
            sua = &t;
    }
    BOOST_REQUIRE(ksck);
    BOOST_TEST(ksck->key.pid == 8u);
    BOOST_TEST(ksck->key.year == 15u);
    BOOST_TEST(ksck->key.report == 12045u);
    BOOST_TEST(ksck->key.month == 1u);
    BOOST_TEST(ksck->location == "KSCK");
    BOOST_TEST(ksck->key.str() == "8-15-12045-KSCK-1");
    // Its text half arrives in another APDU under the same key.
    BOOST_TEST(text_keys.count(ksck->key) == 1u);

    // Reference §6.2: the first SUA text record is length 68, report 9294, year 15, active.
    BOOST_REQUIRE(sua);
    BOOST_TEST(sua->records[0].declared == 68u);
    BOOST_TEST(sua->key.report == 9294u);
    BOOST_TEST(sua->key.year == 15u);
    BOOST_TEST(!sua->cancel);
    BOOST_TEST(sua->text->rfind("SUA ", 0) == 0u);

    TwgoKey a{8, 15, 100, 0, 1}, b{8, 15, 101, 0, 1}, c{8, 15, 100, 1, 1};
    BOOST_TEST((a < b));
    BOOST_TEST(!(b < a));
    BOOST_TEST((a < c));
    BOOST_TEST((a == TwgoKey{8, 15, 100, 0, 1}));
    BOOST_TEST(a.str() == "8-15-100--1");

    // A hand-built text record with a location.
    auto p = twgoPayload(2, {textRecord(77, 26, true, dlacOf("HELLO\n"))}, true);
    auto t = parseTwgo(hdr(11), p.data(), p.size(), sampleRef());
    BOOST_REQUIRE(t);
    BOOST_TEST(t->location == "ABC");
    BOOST_TEST(t->key.str() == "11-26-77-ABC-11");
    BOOST_TEST(*t->text == "HELLO");

    // Malformed: short header, bad format, zero records, a record past the end.
    BOOST_TEST(!parseTwgo(hdr(8), p.data(), 5, sampleRef()));
    auto bad = p;
    bad[0] = 0x30;
    BOOST_TEST(!parseTwgo(hdr(8), bad.data(), bad.size(), sampleRef()));
    bad = p;
    bad[1] = 0;
    BOOST_TEST(!parseTwgo(hdr(8), bad.data(), bad.size(), sampleRef()));
    BOOST_TEST(!parseTwgo(hdr(8), p.data(), p.size() - 1, sampleRef()));
}

BOOST_AUTO_TEST_CASE(twgo_cancel_and_status13)
{
    const auto ref = iso("2026-11-02T09:00:00Z");
    // Text status 0.
    auto pt = twgoPayload(2, {textRecord(5, 26, false, dlacOf("CANCELLED\n"))});
    auto tt = parseTwgo(hdr(12), pt.data(), pt.size(), ref);
    BOOST_REQUIRE(tt);
    BOOST_TEST(tt->cancel);
    auto pa = twgoPayload(2, {textRecord(5, 26, true, dlacOf("ACTIVE\n"))});
    BOOST_TEST(!parseTwgo(hdr(12), pa.data(), pa.size(), ref)->cancel);

    // Graphic object_status 13.
    GraphicSpec g;
    g.geometry = 3;
    g.verts = {std::make_tuple(40.0, -100.0, 1000), std::make_tuple(41.0, -100.0, 1000),
               std::make_tuple(41.0, -99.0, 1000), std::make_tuple(40.0, -100.0, 1000)};
    g.status = 13;
    auto pg = twgoPayload(8, {graphicRecord(g)});
    auto tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
    BOOST_REQUIRE(tg);
    BOOST_TEST(tg->cancel);
    BOOST_TEST(tg->object_status == 13u);
    BOOST_TEST(tg->has_graphics);
    BOOST_TEST(!tg->text.has_value());
    g.status = 15;
    pg = twgoPayload(8, {graphicRecord(g)});
    tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
    BOOST_REQUIRE(tg);
    BOOST_TEST(!tg->cancel);
    BOOST_TEST(tg->object_status == 15u);
    BOOST_REQUIRE_EQUAL(tg->shapes.size(), 1u);
    BOOST_TEST(tg->shapes[0].points.size() == 4u);

    // A parameter or overlay operator 2 makes the record ignored, but the key and status stay.
    g.param = true;
    pg = twgoPayload(8, {graphicRecord(g)});
    tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
    BOOST_REQUIRE(tg);
    BOOST_TEST(tg->records[0].ignored);
    BOOST_TEST(tg->records[0].consumed == tg->records[0].declared);
    BOOST_TEST(tg->shapes.empty());
    BOOST_TEST(tg->key.report == 100u);
    g.param = false;
    g.op = 2;
    pg = twgoPayload(8, {graphicRecord(g)});
    tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
    BOOST_REQUIRE(tg);
    BOOST_TEST(tg->records[0].ignored);
    g.op = 0;
    // Times present in format 0 or 2: ignored (DO-358B), still exactly consumed.
    for (int fmt : {0, 2}) {
        g.fmt = static_cast<uint8_t>(fmt);
        pg = twgoPayload(8, {graphicRecord(g)});
        tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
        BOOST_REQUIRE(tg);
        BOOST_TEST(tg->records[0].ignored);
        BOOST_TEST(tg->records[0].consumed == tg->records[0].declared);
        BOOST_TEST(!tg->start.has_value());
    }
    // Format 3 (hh:mm only) takes the day from the reference time.
    g.fmt = 3;
    pg = twgoPayload(8, {graphicRecord(g)});
    tg = parseTwgo(hdr(12), pg.data(), pg.size(), ref);
    BOOST_REQUIRE(tg);
    BOOST_TEST(!tg->records[0].ignored);
    BOOST_TEST(skycued::formatIso8601Ms(*tg->start) == "2026-11-02T10:00:00.000Z");
    BOOST_TEST(skycued::formatIso8601Ms(*tg->end) == "2026-11-02T12:30:00.000Z");
}

BOOST_AUTO_TEST_CASE(text_413_splits_metar_taf_pirep_winds,
                     *skycue_test::needs_fixture("raw/uat-sample.txt.gz")
                     *skycue_test::needs_fixture("fisb/tg30.978"))
{
    struct Want {
        std::string path;
        TimePoint ref;
        std::size_t apdus;
        std::map<TextRecord::Kind, int> kinds;
    };
    const std::vector<Want> wants = {
        // Task 1 tallies: METAR 147 + SPECI 3; TAF 29 + TAF.AMD 4; PIREP 6; WINDS 35.
        {kSample, sampleRef(), 224, {{TextRecord::metar, 150}, {TextRecord::taf, 33}, {TextRecord::pirep, 6},
                                     {TextRecord::winds, 35}}},
        // METAR 873 + SPECI 40; TAF 90 (incl. 2 TAF COR) + TAF.AMD 31; PIREP 4; WINDS 216.
        {kTg30, tg30Ref(), 1254, {{TextRecord::metar, 913}, {TextRecord::taf, 121}, {TextRecord::pirep, 4},
                                  {TextRecord::winds, 216}}},
    };
    for (const auto& w : wants) {
        std::map<TextRecord::Kind, int> kinds;
        std::size_t apdus = 0, no_station = 0;
        bool kcey = false;
        for (const auto& a : fixtureApdus(w.path)) {
            if (a.h.product_id != 413)
                continue;
            ++apdus;
            auto recs = parseTextProduct(a.h, a.payload.data(), a.payload.size(), w.ref);
            BOOST_TEST(recs.size() == 1u);  // one report per APDU in both fixtures
            for (const auto& r : recs) {
                ++kinds[r.kind];
                if (r.station.empty())
                    ++no_station;
                BOOST_TEST(r.text.find('\x1e') == std::string::npos);
                BOOST_TEST((r.text.empty() || r.text.back() != '\n'));
                BOOST_TEST((std::chrono::abs(r.issued - w.ref) < std::chrono::hours(48)));
                if (r.kind == TextRecord::metar && r.station == "KCEY") {
                    kcey = true;
                    BOOST_TEST(r.text.rfind("METAR KCEY 300835Z AUTO", 0) == 0u);
                    BOOST_TEST(skycued::formatIso8601Ms(r.issued) == "2020-10-30T08:35:00.000Z");
                }
            }
        }
        BOOST_TEST(apdus == w.apdus);
        BOOST_TEST((kinds == w.kinds));
        BOOST_TEST(no_station == 0u);
        if (w.path == kTg30)
            BOOST_TEST(kcey);
    }

    // Several reports in one APDU, split on RS; the LF before each RS is dropped.
    const auto ref = iso("2015-01-24T12:00:00Z");
    auto p = dlacOf("METAR KSFO 241156Z 28010KT 10SM\n\x1eTAF COR KOAK 241120Z 2412/2512 VRB03KT\n\x1e"
                    "UA /OV SFO/TM 1150\n\x1eHELLO WORLD\n\x1e");
    ApduHeader h;
    h.product_id = 413;
    h.hours = 11;
    h.minutes = 58;
    auto recs = parseTextProduct(h, p.data(), p.size(), ref);
    BOOST_REQUIRE_EQUAL(recs.size(), 4u);
    BOOST_TEST(recs[0].kind == TextRecord::metar);
    BOOST_TEST(recs[0].station == "KSFO");
    BOOST_TEST(recs[0].text == "METAR KSFO 241156Z 28010KT 10SM");
    BOOST_TEST(skycued::formatIso8601Ms(recs[0].issued) == "2015-01-24T11:56:00.000Z");
    BOOST_TEST(recs[1].kind == TextRecord::taf);
    BOOST_TEST(recs[1].station == "KOAK");
    BOOST_TEST(skycued::formatIso8601Ms(recs[1].issued) == "2015-01-24T11:20:00.000Z");
    BOOST_TEST(recs[2].kind == TextRecord::pirep);
    BOOST_TEST(recs[2].station.empty());
    // No time group: the APDU time.
    BOOST_TEST(skycued::formatIso8601Ms(recs[2].issued) == "2015-01-24T11:58:00.000Z");
    BOOST_TEST(recs[3].kind == TextRecord::other);
    BOOST_TEST(recs[3].station.empty());
    BOOST_TEST(parseTextProduct(h, nullptr, 0, ref).empty());
}

BOOST_AUTO_TEST_CASE(winds_period_parsed,
                     *skycue_test::needs_fixture("raw/uat-sample.txt.gz")
                     *skycue_test::needs_fixture("fisb/tg30.978"))
{
    BOOST_TEST(*windsPeriod(2, 600) == 6u);
    BOOST_TEST(*windsPeriod(1, 1200) == 12u);
    BOOST_TEST(*windsPeriod(2, 0) == 24u);
    BOOST_TEST(!windsPeriod(2, 1800).has_value());
    BOOST_TEST(*windsPeriod(8, 600) == 24u);
    BOOST_TEST(*windsPeriod(14, 1800) == 6u);
    BOOST_TEST(*windsPeriod(20, 600) == 12u);
    BOOST_TEST(!windsPeriod(20, 1200).has_value());
    BOOST_TEST(!windsPeriod(5, 600).has_value());
    BOOST_TEST(!windsPeriod(8, 630).has_value());

    // Sample: APDU 02:06, "WINDS BCE 250000Z" -> the 24 h forecast.
    // tg30: APDU 07:54, valid 301200Z -> 6 h, 301800Z -> 12 h.
    struct Want {
        std::string path;
        TimePoint ref;
        std::map<int, int> periods;
    };
    std::map<std::string, int> seen;
    for (const auto& w : {Want{kSample, sampleRef(), {}}, Want{kTg30, tg30Ref(), {}}}) {
        std::map<int, int> periods;
        for (const auto& a : fixtureApdus(w.path)) {
            if (a.h.product_id != 413)
                continue;
            for (const auto& r : parseTextProduct(a.h, a.payload.data(), a.payload.size(), w.ref)) {
                if (r.kind != TextRecord::winds) {
                    BOOST_TEST(!r.period_h.has_value());
                    BOOST_TEST(!r.valid.has_value());
                    continue;
                }
                BOOST_REQUIRE_MESSAGE(r.period_h, "no period: " << r.text.substr(0, 40));
                ++periods[*r.period_h];
                BOOST_TEST(r.station.size() == 3u);
                if (r.text.rfind("WINDS BCE 250000Z", 0) == 0) {
                    BOOST_TEST(*r.period_h == 24u);
                    // The group is the valid time; issued is the APDU time (02:06).
                    BOOST_REQUIRE(r.valid.has_value());
                    BOOST_TEST(skycued::formatIso8601Ms(*r.valid) == "2015-01-25T00:00:00.000Z");
                    BOOST_TEST(skycued::formatIso8601Ms(r.issued) == "2015-01-24T02:06:00.000Z");
                    ++seen["BCE"];
                }
                if (r.text.rfind("WINDS TUL 301800Z", 0) == 0) {
                    BOOST_TEST(*r.period_h == 12u);
                    ++seen["TUL"];
                }
                if (r.text.rfind("WINDS CSG 301200Z", 0) == 0) {
                    BOOST_TEST(*r.period_h == 6u);
                    ++seen["CSG"];
                }
            }
        }
        std::ostringstream os;
        for (const auto& kv : periods)
            os << " " << kv.first << "h=" << kv.second;
        BOOST_TEST_MESSAGE(w.path << ": winds periods" << os.str());
    }
    BOOST_TEST(seen["BCE"] >= 1);
    BOOST_TEST(seen["TUL"] >= 1);
    BOOST_TEST(seen["CSG"] >= 1);
}

BOOST_AUTO_TEST_CASE(fuzz_twgo_no_throw, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    std::mt19937 rng(20261102);
    std::uniform_int_distribution<int> byte(0, 255);
    const auto ref = tg30Ref();
    const uint16_t pids[] = {8, 11, 12, 13, 14, 15, 16, 17, 413, 63};
    std::size_t parsed = 0, rejected = 0, text_recs = 0, shapes = 0;

    auto check = [&](const ApduHeader& h, const std::vector<uint8_t>& b) {
        BOOST_CHECK_NO_THROW({
            auto t = parseTwgo(h, b.data(), b.size(), ref);
            if (t) {
                ++parsed;
                BOOST_TEST(t->leftover <= b.size());
                std::size_t sum = 6 + t->leftover;
                for (const auto& r : t->records) {
                    BOOST_TEST(r.consumed <= r.declared);
                    sum += r.declared;
                }
                BOOST_TEST(sum == b.size());
                shapes += t->shapes.size();
                for (const auto& s : t->shapes)
                    for (const auto& pt : s.points) {
                        BOOST_TEST(std::fabs(pt.first) <= 180.0);
                        BOOST_TEST(std::fabs(pt.second) <= 180.0);
                    }
            } else
                ++rejected;
            text_recs += parseTextProduct(h, b.data(), b.size(), ref).size();
        });
    };

    // Random payloads, with a plausible TWGO header half the time.
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> b(static_cast<std::size_t>(byte(rng)) + static_cast<std::size_t>(byte(rng) & 1) * 256);
        for (auto& x : b)
            x = static_cast<uint8_t>(byte(rng));
        if (i & 1 && b.size() >= 2) {
            b[0] = static_cast<uint8_t>((byte(rng) & 1 ? 0x80 : 0x20) | (b[0] & 0x0f));
            b[1] = static_cast<uint8_t>((1 + (byte(rng) % 3)) << 4);
        }
        check(hdr(pids[i % 10]), b);
    }
    // Real TWGO payloads with random bit flips and truncations.
    auto items = fixtureTwgo(kSample, sampleRef());
    for (int i = 0; i < 20000; ++i) {
        const auto& it = items[static_cast<std::size_t>(i) % items.size()];
        auto b = it.payload;
        const int flips = 1 + byte(rng) % 4;
        for (int k = 0; k < flips; ++k)
            { const auto flipMask_ = static_cast<uint8_t>(1u << (byte(rng) % 8)); auto& flipCell_ = b[static_cast<std::size_t>(byte(rng) * 256 + byte(rng)) % b.size()]; flipCell_ = static_cast<uint8_t>(flipCell_ ^ flipMask_); }
        if (i % 5 == 0)
            b.resize(static_cast<std::size_t>(byte(rng) * 256 + byte(rng)) % (b.size() + 1));
        check(it.h, b);
    }
    check(hdr(8), {});

    // Random segment headers through the assembler.
    SegmentAssembler sa(64);
    std::size_t done = 0;
    for (int i = 0; i < 20000; ++i) {
        ApduHeader h = segHdr(pids[i % 10], static_cast<uint16_t>(byte(rng) % 80), static_cast<uint16_t>(byte(rng) % 5),
                              static_cast<uint16_t>(byte(rng) % 4));
        std::vector<uint8_t> b(static_cast<std::size_t>(byte(rng) % 40));
        for (auto& x : b)
            x = static_cast<uint8_t>(byte(rng));
        BOOST_CHECK_NO_THROW({
            auto d = sa.add(h, b.data(), b.size(), ref + std::chrono::seconds(i));
            if (d)
                ++done;
        });
        BOOST_TEST(sa.open() <= 64u);
        if (i % 1000 == 0)
            sa.sweep(ref + std::chrono::seconds(i), minutes(5));
    }
    BOOST_TEST_MESSAGE("fuzz: twgo parsed=" << parsed << " rejected=" << rejected << " shapes=" << shapes
                       << " text records=" << text_recs << "; segments done=" << done << " rejected="
                       << sa.rejected() << " dup=" << sa.duplicates() << " evicted=" << sa.evicted()
                       << " timed out=" << sa.timedOut());
    BOOST_TEST(parsed > 0u);
    BOOST_TEST(rejected > 0u);
    BOOST_TEST(done > 0u);
    BOOST_TEST(sa.rejected() > 0u);
    BOOST_TEST(sa.evicted() > 0u);
}
