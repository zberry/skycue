#define BOOST_TEST_MODULE skycued_fisb_blocks
#include <boost/test/unit_test.hpp>

#include "fisb/apdu.hpp"
#include "fisb/global_block.hpp"
#include "fisb/products.hpp"
#include "fisb/uplink_frame.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace skycued::fisb;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;
const std::string kSample = kFixtures + "/raw/uat-sample.txt.gz";
const std::string kTg30 = kFixtures + "/fisb/tg30.978";

std::vector<std::string> readLines(const std::string& path)
{
    std::vector<std::string> lines;
    gzFile f = gzopen(path.c_str(), "rb");
    BOOST_REQUIRE_MESSAGE(f != nullptr, "cannot open " << path);
    char buf[4096];
    std::string cur;
    while (gzgets(f, buf, sizeof buf) != nullptr) {
        cur += buf;
        if (!cur.empty() && cur.back() == '\n') {
            cur.pop_back();
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    gzclose(f);
    return lines;
}

struct Apdu {
    ApduHeader h;
    std::vector<uint8_t> payload;
};

// Every global-block APDU in a fixture, in order.
std::vector<Apdu> blockApdus(const std::string& path)
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
            if (!a || productFamily(a->product_id) != ProductFamily::global_block)
                return;
            out.push_back({*a, std::vector<uint8_t>(f.data + a->header_bytes, f.data + f.len)});
        });
    }
    return out;
}

ApduHeader hdr(uint16_t pid)
{
    ApduHeader h;
    h.product_id = pid;
    h.header_bytes = 4;
    return h;
}

std::vector<uint8_t> hex(const std::string& s)
{
    std::vector<uint8_t> b;
    BOOST_REQUIRE(hexToBytes(s, b));
    return b;
}

std::optional<DecodedBlocks> parse(uint16_t pid, const std::vector<uint8_t>& b, bool experimental = false,
                                   GlobalBlockStats* st = nullptr)
{
    return parseGlobalBlockProduct(hdr(pid), b.data(), b.size(), experimental, st);
}

// Block reference indicator bytes for an E=1/0 block.
std::vector<uint8_t> ref(bool rle, uint8_t bits654, uint32_t bn)
{
    return {static_cast<uint8_t>((rle ? 0x80 : 0) | (bits654 << 4) | ((bn >> 16) & 0x0F)),
            static_cast<uint8_t>(bn >> 8), static_cast<uint8_t>(bn)};
}

std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

struct Tally {
    uint64_t apdus = 0, rle = 0, lists = 0, l0 = 0, empty = 0, rejected = 0;
    int max_level = -1;
    std::set<uint8_t> scales, alt_codes;
};

std::map<uint16_t, Tally> tallyFixture(const std::string& path)
{
    std::map<uint16_t, Tally> t;
    for (const auto& a : blockApdus(path)) {
        auto& x = t[a.h.product_id];
        ++x.apdus;
        GlobalBlockStats st;
        auto d = parseGlobalBlockProduct(a.h, a.payload.data(), a.payload.size(), false, &st);
        x.rle += st.rle_blocks;
        x.lists += st.empty_lists;
        x.l0 += st.empty_lists_l0;
        x.empty += st.empty_blocks;
        x.rejected += st.rejected + st.high_lat_skipped;
        if (!d)
            continue;
        x.scales.insert(d->scale);
        if (d->altitude_code)
            x.alt_codes.insert(*d->altitude_code);
        for (const auto& b : d->blocks)
            for (auto v : b.bins)
                x.max_level = std::max<int>(x.max_level, v);
    }
    return t;
}

}  // namespace

BOOST_AUTO_TEST_CASE(every_rle_block_in_tg30_totals_128)
{
    uint64_t rle = 0, lists = 0, empty = 0, raw_checked = 0;
    for (const auto& a : blockApdus(kTg30)) {
        const uint16_t pid = a.h.product_id;
        if (pid != 63 && pid != 64)
            continue;
        const auto& p = a.payload;
        BOOST_REQUIRE(p.size() >= 4);
        GlobalBlockStats st;
        auto d = parseGlobalBlockProduct(a.h, p.data(), p.size(), false, &st);
        BOOST_REQUIRE_MESSAGE(d, "pid " << pid << " block not decoded");
        BOOST_TEST(st.rejected == 0u);
        BOOST_TEST(d->conus == (pid == 64));
        if (p[0] & 0x80) {
            // Independent sum of the RRRRRIII runs over the whole payload.
            unsigned sum = 0;
            for (std::size_t i = 3; i < p.size(); ++i)
                sum += (p[i] >> 3) + 1u;
            BOOST_TEST(sum == 128u);
            ++raw_checked;
            BOOST_REQUIRE(d->blocks.size() == 1u);
            BOOST_TEST(d->empty_blocks.empty());
            for (auto v : d->blocks[0].bins)
                BOOST_TEST(v <= 7);
        } else {
            BOOST_TEST(d->blocks.empty());
            BOOST_TEST(!d->empty_blocks.empty());
        }
        rle += st.rle_blocks;
        lists += st.empty_lists;
        empty += st.empty_blocks;
    }
    // Reference §5.3 [V]: 503 regional + 1032 CONUS run-length blocks.
    BOOST_TEST(raw_checked == 1535u);
    BOOST_TEST(rle == 1535u);
    BOOST_TEST(lists == 395u + 55u);
    BOOST_TEST(empty == 2934u + 381u);
}

BOOST_AUTO_TEST_CASE(empty_block_lists_decode_including_last_byte_case)
{
    // Hand-built: L = 0, the high nibble alone lists b+1..b+4 (MUT/STX emit
    // nothing for this; §5.4).
    {
        auto d = parse(63, cat(ref(false, 0, 276640), {0xF0}));
        BOOST_REQUIRE(d);
        BOOST_TEST(d->empty_blocks == (std::vector<uint32_t>{276640, 276641, 276642, 276643, 276644}),
                   boost::test_tools::per_element());
    }
    {
        auto d = parse(63, cat(ref(false, 0, 276640), {0x00}));
        BOOST_REQUIRE(d);
        BOOST_TEST(d->empty_blocks == std::vector<uint32_t>{276640}, boost::test_tools::per_element());
    }
    // L = 2: bit4 = b+1, byte4 bit0 = b+5, the last byte's bit7 = b+20.
    {
        auto d = parse(63, cat(ref(false, 0, 276640), {0x12, 0x01, 0x80}));
        BOOST_REQUIRE(d);
        BOOST_TEST(d->empty_blocks == (std::vector<uint32_t>{276640, 276641, 276645, 276660}),
                   boost::test_tools::per_element());
    }
    // Medium scale steps 5 per bit (FD msgBlock.py:214-251).
    {
        auto d = parse(64, cat(ref(false, 1, 139405), {0x30}));
        BOOST_REQUIRE(d);
        BOOST_TEST(d->empty_blocks == (std::vector<uint32_t>{139405, 139410, 139415}),
                   boost::test_tools::per_element());
    }
    // Offsets wrap within the latitude row (§5.4): column 449 + 1 -> column 0.
    {
        auto d = parse(63, cat(ref(false, 0, 614 * 450 + 449), {0x30}));
        BOOST_REQUIRE(d);
        BOOST_TEST(d->empty_blocks == (std::vector<uint32_t>{614 * 450 + 449, 614 * 450, 614 * 450 + 1}),
                   boost::test_tools::per_element());
        auto m = parse(64, cat(ref(false, 1, 1800 + 2250 * 61 + 5 * 89), {0x10}));
        BOOST_REQUIRE(m);
        BOOST_TEST(m->empty_blocks == (std::vector<uint32_t>{1800 + 2250 * 61 + 5 * 89, 1800 + 2250 * 61}),
                   boost::test_tools::per_element());
    }
    // Length must be exactly 4 + L.
    {
        GlobalBlockStats st;
        BOOST_TEST(!parse(63, cat(ref(false, 0, 276640), {0x02, 0x01}), false, &st));
        BOOST_TEST(!parse(63, cat(ref(false, 0, 276640), {0x00, 0x01}), false, &st));
        BOOST_TEST(st.rejected == 2u);
    }

    // The 2015 sample: 200 regional APDUs, all empty-block lists.
    auto s = tallyFixture(kSample);
    BOOST_TEST(s.size() == 1u);
    const auto& r = s[63];
    BOOST_TEST(r.apdus == 200u);
    BOOST_TEST(r.lists == 200u);
    BOOST_TEST(r.rle == 0u);
    BOOST_TEST(r.l0 == 56u);
    BOOST_TEST(r.empty == 1342u);
    BOOST_TEST(r.rejected == 0u);

    // Reference §5.4 [V]: 1025 empty-block lists across the sample and tg30,
    // 415 of them with L = 0.
    auto t = tallyFixture(kTg30);
    uint64_t lists = r.lists, l0 = r.l0, empty = r.empty;
    for (const auto& kv : t) {
        lists += kv.second.lists;
        l0 += kv.second.l0;
        empty += kv.second.empty;
    }
    BOOST_TEST(lists == 1025u);
    BOOST_TEST(l0 == 415u);
    BOOST_TEST(empty == 1342u + 2934u + 381u + 190u + 176u + 230u + 861u);
}

BOOST_AUTO_TEST_CASE(block_geometry_known_numbers)
{
    // Block 0: SW corner at (0°N, 0°E) (§5.2), so NW corner at 4'.
    auto g = blockGeometry(0, 0, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 4.0 / 60));
    BOOST_TEST(near(g->lon0, 0.0));
    BOOST_TEST(near(g->dlat, 1.0 / 60));
    BOOST_TEST(near(g->dlon, 1.5 / 60));
    // FD alternateBlockNumber docstring: 276640 = row 614, column 340. North
    // edge 615 * 4' = 41°N; west edge 340 * 48' = 272°E = 88°W.
    g = blockGeometry(276640, 0, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 41.0));
    BOOST_TEST(near(g->lon0, -88.0));
    // tg30 regional block 277546 (row 616, column 346): 41°08'N, 83.2°W.
    g = blockGeometry(277546, 0, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 617 * 4.0 / 60));
    BOOST_TEST(near(g->lon0, 346 * 0.8 - 360));
    // tg30 CONUS block 139405 = 1800 + 2250*61 + 5*71: latitude
    // [20'*61, 20'*62), longitude [4°*71, 4°*72) (§5.2).
    g = blockGeometry(139405, 1, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 62 * 20.0 / 60));
    BOOST_TEST(near(g->lon0, 4.0 * 71 - 360));
    BOOST_TEST(near(g->dlat, 5.0 / 60));
    BOOST_TEST(near(g->dlon, 7.5 / 60));
    // Low scale: 3600 + 4050*k + 9*c covers [36'k, 36'(k+1)) x [7.2°c, ...).
    g = blockGeometry(3600 + 4050 * 10 + 9 * 3, 2, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 11 * 36.0 / 60));
    BOOST_TEST(near(g->lon0, 3 * 7.2));
    BOOST_TEST(near(g->dlat, 9.0 / 60));
    BOOST_TEST(near(g->dlon, 13.5 / 60));
    // Longitude: 180° exactly stays east; just past it goes negative.
    g = blockGeometry(225, 0, false);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lon0, 180.0));
    // Reserved scale, off-lattice medium/low numbers, beyond the pole.
    BOOST_TEST(!blockGeometry(276640, 3, true));
    BOOST_TEST(!blockGeometry(139406, 1, true));
    BOOST_TEST(!blockGeometry(1800 + 2250 * 61 + 450, 1, true));
    BOOST_TEST(!blockGeometry(100, 1, true));
    BOOST_TEST(!blockGeometry(3600 + 4050 * 10 + 4, 2, true));
    BOOST_TEST(!blockGeometry(1350 * 450, 0, true));
    BOOST_TEST(bool(blockGeometry(1349 * 450, 0, true)));

    // Every decoded tg30 block's bins fall inside its own geometry.
    for (const auto& a : blockApdus(kTg30)) {
        auto d = parseGlobalBlockProduct(a.h, a.payload.data(), a.payload.size(), false);
        if (!d)
            continue;
        for (const auto& b : d->blocks) {
            auto bg = blockGeometry(b.block, d->scale, false);
            BOOST_REQUIRE(bg);
            BOOST_TEST(near(bg->lat0, b.lat0));
            BOOST_TEST(near(bg->lon0, b.lon0));
            // CONUS and regional coverage in tg30 is around the US.
            BOOST_TEST((b.lat0 > 15 && b.lat0 < 60));
            BOOST_TEST((b.lon0 > -135 && b.lon0 < -55));
        }
    }
}

BOOST_AUTO_TEST_CASE(conus_vs_regional_scale)
{
    std::set<uint8_t> s63, s64;
    uint64_t n64 = 0;
    for (const auto& a : blockApdus(kTg30)) {
        const uint16_t pid = a.h.product_id;
        if (pid != 63 && pid != 64)
            continue;
        auto d = parseGlobalBlockProduct(a.h, a.payload.data(), a.payload.size(), false);
        BOOST_REQUIRE(d);
        (pid == 63 ? s63 : s64).insert(d->scale);
        BOOST_TEST(d->conus == (pid == 64));
        for (const auto& b : d->blocks) {
            if (pid == 63) {
                BOOST_TEST(near(b.dlat, 1.0 / 60));
                BOOST_TEST(near(b.dlon, 1.5 / 60));
            } else {
                ++n64;
                BOOST_TEST(near(b.dlat, 5.0 / 60));
                BOOST_TEST(near(b.dlon, 7.5 / 60));
                // §5.2 [V]: (row - 4) % 5 == 0 and col % 5 == 0.
                BOOST_TEST((b.block / 450 - 4) % 5 == 0u);
                BOOST_TEST(b.block % 450 % 5 == 0u);
            }
        }
    }
    // §5.6 [V]: 63 always scale 0, 64 always scale 1.
    BOOST_TEST((s63 == std::set<uint8_t>{0}));
    BOOST_TEST((s64 == std::set<uint8_t>{1}));
    BOOST_TEST(n64 == 1032u);

    // The same block number decodes to different areas by scale.
    auto r = parse(63, cat(ref(true, 0, 139405), {0xF8, 0xF8, 0xF8, 0xF8}));
    auto c = parse(64, cat(ref(true, 1, 139405), {0xF8, 0xF8, 0xF8, 0xF8}));
    BOOST_REQUIRE(r);
    BOOST_REQUIRE(c);
    BOOST_TEST(!r->conus);
    BOOST_TEST(c->conus);
    BOOST_TEST(near(r->blocks[0].lat0, c->blocks[0].lat0));  // same high-res row top
    BOOST_TEST(near(r->blocks[0].lon0, c->blocks[0].lon0));
    BOOST_TEST(near(c->blocks[0].dlat, 5 * r->blocks[0].dlat));
    BOOST_TEST(near(c->blocks[0].dlon, 5 * r->blocks[0].dlon));
    // Scale 3 is reserved: rejected.
    GlobalBlockStats st;
    BOOST_TEST(!parse(64, cat(ref(true, 3, 139405), {0xF8, 0xF8, 0xF8, 0xF8}), false, &st));
    BOOST_TEST(st.rejected == 1u);
    // A southern-hemisphere block (NS = 1) is untested and rejected.
    BOOST_TEST(!parse(63, cat(ref(true, 4, 139405), {0xF8, 0xF8, 0xF8, 0xF8}), false, &st));
    BOOST_TEST(st.rejected == 2u);
}

BOOST_AUTO_TEST_CASE(high_latitude_skipped_unless_experimental)
{
    BOOST_TEST(!blockGeometry(405000, 0, false));
    BOOST_TEST(bool(blockGeometry(404999, 0, false)));
    BOOST_TEST(!blockGeometry(406800, 1, false));
    BOOST_TEST(bool(blockGeometry(1800 + 2250 * 179 + 5 * 89, 1, false)));  // last medium row below 60°N

    // Row 900, column 3 (odd): experimental rounds it to the even block,
    // double-width bins (§5.2 [U]).
    auto g = blockGeometry(405003, 0, true);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lat0, 901 * 4.0 / 60));
    BOOST_TEST(near(g->lon0, 2 * 0.8));
    BOOST_TEST(near(g->dlon, 3.0 / 60));
    g = blockGeometry(406800 + 5, 1, true);
    BOOST_REQUIRE(g);
    BOOST_TEST(near(g->lon0, 0.0));
    BOOST_TEST(near(g->dlon, 15.0 / 60));

    const auto block = cat(ref(true, 0, 405002), {0xF8, 0xF8, 0xF8, 0xF0, 0x01});
    GlobalBlockStats st;
    BOOST_TEST(!parse(63, block, false, &st));
    BOOST_TEST(st.high_lat_skipped == 1u);
    BOOST_TEST(st.rejected == 0u);
    auto d = parse(63, block, true, &st);
    BOOST_REQUIRE(d);
    BOOST_REQUIRE(d->blocks.size() == 1u);
    BOOST_TEST(d->blocks[0].bins[127] == 1);
    BOOST_TEST(near(d->blocks[0].dlon, 3.0 / 60));

    // An empty list at >= 60°N: skipped, or walked on the even lattice.
    const auto list = cat(ref(false, 0, 405000 + 448), {0x30});
    BOOST_TEST(!parse(63, list, false, &st));
    BOOST_TEST(st.high_lat_skipped == 2u);
    d = parse(63, list, true, &st);
    BOOST_REQUIRE(d);
    BOOST_TEST(d->empty_blocks == (std::vector<uint32_t>{405448, 405000, 405002}), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(other_rasters_70_71_84_90_91_103_decode_as_levels)
{
    auto t = tallyFixture(kTg30);
    struct Want {
        uint16_t pid;
        uint64_t apdus, rle, lists, empty;
        uint8_t scale;
        int max_level;  // observed maximum
        int range;      // the encoding's maximum
    };
    const Want want[] = {
        {63, 898, 503, 395, 2934, 0, 5, 7},    {64, 1087, 1032, 55, 381, 1, 7, 7},
        {70, 369, 250, 119, 190, 1, 255, 255}, {71, 117, 44, 73, 176, 1, 26, 255},
        {84, 714, 631, 83, 230, 0, 12, 15},    {90, 440, 440, 0, 0, 1, 15, 15},
        {91, 220, 220, 0, 0, 1, 3, 15},        {103, 100, 0, 100, 861, 0, -1, 15},
    };
    BOOST_TEST(t.size() == 8u);
    for (const auto& w : want) {
        BOOST_TEST_CONTEXT("product " << w.pid)
        {
            const auto& x = t[w.pid];
            BOOST_TEST(x.apdus == w.apdus);
            BOOST_TEST(x.rle == w.rle);
            BOOST_TEST(x.lists == w.lists);
            BOOST_TEST(x.empty == w.empty);
            BOOST_TEST(x.rejected == 0u);
            BOOST_TEST(x.rle + x.lists == x.apdus);
            BOOST_TEST(x.rle + x.empty >= 1u);
            BOOST_TEST((x.scales == std::set<uint8_t>{w.scale}));
            BOOST_TEST(x.max_level == w.max_level);
            BOOST_TEST(x.max_level <= w.range);
            const bool alt = w.pid == 70 || w.pid == 71 || w.pid == 90 || w.pid == 91;
            BOOST_TEST(x.alt_codes.empty() == !alt);
        }
    }

    // Hand-built runs (§5.5). Cloud tops / turbulence: E4 7F = 128 x 4.
    auto d = parse(84, cat(ref(true, 0, 286994), {0xE4, 0x7F}));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->blocks[0].bins[0] == 4);
    BOOST_TEST(d->blocks[0].bins[127] == 4);
    // Turbulence: 0x32 = 4 bins of 2, then E1 7B = 124 bins of 1.
    d = parse(90, cat(ref(true, 2, 287885), {0x32, 0xE1, 0x7B}));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->scale == 1);
    BOOST_TEST(d->altitude_code.value_or(99) == 2);
    BOOST_TEST(d->blocks[0].bins[3] == 2);
    BOOST_TEST(d->blocks[0].bins[4] == 1);
    // Icing: (count-1, value) pairs; 7E 00 + 01 12 = 127 x 0, 2 x 0x12 = 129: rejected.
    GlobalBlockStats st;
    BOOST_TEST(!parse(71, cat(ref(true, 0, 276645), {0x7E, 0x00, 0x01, 0x12}), false, &st));
    BOOST_TEST(st.rejected == 1u);
    d = parse(71, cat(ref(true, 0, 276645), {0x7E, 0x00, 0x00, 0x12}));  // tg30 bytes
    BOOST_REQUIRE(d);
    BOOST_TEST(d->blocks[0].bins[126] == 0);
    BOOST_TEST(d->blocks[0].bins[127] == 0x12);
    // Lightning: F8 = 32 bins (FD special case), 0x8 -> 0, low nibble kept.
    d = parse(103, cat(ref(true, 0, 276640), {0xF8, 0xF8, 0xF8, 0xE8, 0xF0, 0x0B}));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->blocks[0].bins[0] == 0);
    BOOST_TEST(d->blocks[0].bins[126] == 0);
    BOOST_TEST(d->blocks[0].bins[127] == 0x0B);
    BOOST_TEST(!parse(103, cat(ref(true, 0, 276640), {0xF8, 0xF8, 0xF8, 0xF0}), false, &st));  // 112 bins

    // NEXRAD runs straight from tg30 (cross-checked against fisb-decode's
    // nextradRL, see the task report): 843c2af8f8f8e809.
    d = parse(63, hex("843c2af8f8f8e809"));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->blocks[0].block == 277546u);
    for (int i = 0; i < 126; ++i)
        BOOST_TEST(d->blocks[0].bins[i] == 0);
    BOOST_TEST(d->blocks[0].bins[126] == 1);
    BOOST_TEST(d->blocks[0].bins[127] == 1);
    // 843decf001f8f8f8: 31 x 0, 1 x 1, 96 x 0.
    d = parse(63, hex("843decf001f8f8f8"));
    BOOST_REQUIRE(d);
    BOOST_TEST(d->blocks[0].block == 277996u);
    BOOST_TEST(d->blocks[0].bins[30] == 0);
    BOOST_TEST(d->blocks[0].bins[31] == 1);
    BOOST_TEST(d->blocks[0].bins[32] == 0);
    // RLE rules: 127 bins, 129 bins, unused trailing byte.
    BOOST_TEST(!parse(63, hex("843c2af8f8f8e801"), false, &st));
    BOOST_TEST(!parse(63, hex("843c2af8f8f8e810"), false, &st));
    BOOST_TEST(!parse(63, hex("843c2af8f8f8e80900"), false, &st));
    BOOST_TEST(!parse(63, hex("843c2a"), false, &st));
    BOOST_TEST(st.rejected == 6u);
    // Not a global-block product.
    BOOST_TEST(!parse(413, hex("843c2af8f8f8e809")));
}

BOOST_AUTO_TEST_CASE(fuzz_blocks_no_throw)
{
    std::mt19937 rng(20261102);
    std::uniform_int_distribution<int> byte(0, 255);
    const uint16_t pids[] = {63, 64, 70, 71, 84, 90, 91, 103};
    GlobalBlockStats st;
    uint64_t ok = 0, none = 0;

    auto check = [&](uint16_t pid, const std::vector<uint8_t>& b, bool exp) {
        BOOST_CHECK_NO_THROW({
            // Copy into an exact-size heap buffer so an overrun is visible to
            // sanitizers.
            std::vector<uint8_t> buf(b);
            auto d = parseGlobalBlockProduct(hdr(pid), buf.empty() ? nullptr : buf.data(), buf.size(), exp, &st);
            if (d) {
                ++ok;
                BOOST_TEST(d->blocks.size() + d->empty_blocks.size() >= 1u);
                for (const auto& blk : d->blocks) {
                    BOOST_TEST(std::fabs(blk.lat0) <= 90.0);
                    BOOST_TEST(std::fabs(blk.lon0) <= 180.0);
                }
                for (auto e : d->empty_blocks)
                    BOOST_TEST(bool(blockGeometry(e, d->scale, exp)));
            } else
                ++none;
        });
    };

    for (int i = 0; i < 50000; ++i) {
        std::vector<uint8_t> b(static_cast<std::size_t>(byte(rng) % 40));
        for (auto& x : b)
            x = static_cast<uint8_t>(byte(rng));
        check(pids[i % 8], b, (i & 1) != 0);
    }
    // Real payloads with bit flips and truncations.
    auto real = blockApdus(kTg30);
    for (int i = 0; i < 50000; ++i) {
        const auto& a = real[static_cast<std::size_t>(byte(rng) * 256 + byte(rng)) % real.size()];
        auto b = a.payload;
        if (!b.empty())
            b[static_cast<std::size_t>(byte(rng)) % b.size()] ^= static_cast<uint8_t>(1u << (byte(rng) % 8));
        if (i % 3 == 0)
            b.resize(static_cast<std::size_t>(byte(rng)) % (b.size() + 1));
        check(a.h.product_id, b, (i & 1) != 0);
    }
    check(63, {}, false);
    BOOST_TEST_MESSAGE("fuzz: ok=" << ok << " none=" << none << " rejected=" << st.rejected
                       << " high-lat skipped=" << st.high_lat_skipped);
    BOOST_TEST(ok > 0u);
    BOOST_TEST(st.rejected > 0u);
    BOOST_TEST(st.high_lat_skipped > 0u);
}
