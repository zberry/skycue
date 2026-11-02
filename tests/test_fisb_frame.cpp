#define BOOST_TEST_MODULE skycued_fisb_frame
#include <boost/test/unit_test.hpp>

#include "gz_line_reader.hpp"
#include "fisb/apdu.hpp"
#include "fisb/bit_reader.hpp"
#include "fisb/dlac.hpp"
#include "fisb/products.hpp"
#include "fisb/uplink_frame.hpp"


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace skycued::fisb;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;

// Every line of a plain or gzip text file (gzopen reads both).
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

std::string toHex(const std::vector<uint8_t>& b)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) {
        s += d[x >> 4];
        s += d[x & 15];
    }
    return s;
}

// MSB-first bit writer for hand-built headers and DLAC strings.
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

std::vector<uint8_t> dlac(const std::vector<unsigned>& codes)
{
    BitWriter w;
    for (unsigned c : codes)
        w.put(c, 6);
    return w.bytes;
}

// The reference's DLAC alphabet, codes 0..63 (0, 27, 28, 29 are special).
unsigned code(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<unsigned>(c - 'A' + 1);
    const std::string rest = "| !\"#$%&'()*+,-./0123456789:;<=>?";  // codes 31..63
    auto i = rest.find(c);
    BOOST_REQUIRE(i != std::string::npos);
    return static_cast<unsigned>(31 + i);
}

std::vector<unsigned> codes(const std::string& s)
{
    std::vector<unsigned> v;
    for (char c : s)
        v.push_back(code(c));
    return v;
}

// A 432-byte uplink: the sample's first header, then these information frames.
std::vector<uint8_t> uplinkWith(const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& frames)
{
    std::vector<uint8_t> u = {0x35, 0x14, 0xc9, 0x52, 0xd6, 0x5c, 0xa7, 0xb0};
    for (const auto& f : frames) {
        const auto len = static_cast<unsigned>(f.second.size());
        u.push_back(static_cast<uint8_t>(len >> 1));
        u.push_back(static_cast<uint8_t>(((len & 1u) << 7) | (f.first & 0x0fu)));
        u.insert(u.end(), f.second.begin(), f.second.end());
    }
    u.resize(kUplinkBytes, 0);
    return u;
}

// The per-line pipeline the decoder will run, reduced to counting.
struct Probe {
    FisbCounters c;
    bool experimental = false;
    std::map<uint16_t, uint64_t> by_product;
    std::map<uint8_t, uint64_t> by_frame_type;
    std::map<uint8_t, uint64_t> by_topt;
    std::set<uint16_t> segmented_products;
    std::vector<ApduHeader> segments;
    std::vector<std::pair<ApduHeader, std::string>> texts;  // product 413 DLAC

    // false when the line was counted as bad.
    bool line(const std::string& l)
    {
        if (!isUplinkLine(l))
            return true;
        ++c.uplinks;
        std::vector<uint8_t> b;
        if (!hexToBytes(l, b)) {
            ++c.bad_hex;
            return false;
        }
        return uplink(b);
    }

    bool uplink(const std::vector<uint8_t>& b)
    {
        auto h = b.size() == kUplinkBytes ? parseUplinkHeader(b.data(), b.size()) : std::nullopt;
        if (!h) {
            ++c.bad_frames;
            return false;
        }
        if (!h->app_data_valid)
            return true;
        bool ok = true;
        bool walked = forEachInfoFrame(b.data(), b.size(), [&](const InfoFrame& f) {
            ++by_frame_type[f.type];
            if (f.type != 0)
                return;
            ApduStatus st = ApduStatus::ok;
            auto a = parseApdu(f.data, f.len, experimental, &st);
            if (!a) {
                if (st == ApduStatus::experimental_time_option)
                    ++c.skipped_experimental;
                else {
                    ++c.bad_apdu;
                    ok = false;
                }
                return;
            }
            ++c.apdus;
            ++by_product[a->product_id];
            ++by_topt[a->t_opt];
            if (productFamily(a->product_id) == ProductFamily::other)
                ++c.unsupported_products;
            if (a->segmented) {
                segmented_products.insert(a->product_id);
                segments.push_back(*a);
            }
            if (a->product_id == 413 && !a->segmented)
                texts.emplace_back(*a, decodeDlac(f.data + a->header_bytes, f.len - a->header_bytes));
        });
        if (!walked) {
            ++c.bad_frames;
            ok = false;
        }
        return ok;
    }
};

std::string table(const std::map<uint16_t, uint64_t>& m)
{
    std::ostringstream os;
    for (const auto& kv : m)
        os << "\n  " << kv.first << " (" << productName(kv.first) << "): " << kv.second;
    return os.str();
}

}  // namespace

BOOST_AUTO_TEST_CASE(bit_reader_reads_msb_first_and_flags_overrun)
{
    const uint8_t b[] = {0xa5, 0x0f};
    BitReader r(b, sizeof b);
    BOOST_TEST(*r.u(1) == 1u);
    BOOST_TEST(*r.u(3) == 2u);
    BOOST_TEST(*r.u(8) == 0x50u);
    BOOST_TEST(r.remaining() == 4u);
    BOOST_TEST(!r.u(5).has_value());
    BOOST_TEST(r.overrun());
    BOOST_TEST(!r.u(1).has_value());  // stays overrun
    BitReader z(nullptr, 0);
    BOOST_TEST(*z.u(0) == 0u);
    BOOST_TEST(!z.u(1).has_value());
}

BOOST_AUTO_TEST_CASE(uplink_header_from_sample)
{
    auto lines = readLines(kFixtures + "/raw/uat-sample.txt.gz");
    std::string first;
    for (const auto& l : lines)
        if (isUplinkLine(l)) {
            first = l;
            break;
        }
    std::vector<uint8_t> b;
    BOOST_REQUIRE(hexToBytes(first, b));
    BOOST_REQUIRE_EQUAL(b.size(), kUplinkBytes);
    auto h = parseUplinkHeader(b.data(), b.size());
    BOOST_REQUIRE(h);
    // 3514c952d65ca7b0: the sample's one ground station, south of San Jose, CA.
    BOOST_TEST(std::fabs(h->lat_deg - 37.322702) < 1e-5);
    BOOST_TEST(std::fabs(h->lon_deg - -121.754994) < 1e-5);
    BOOST_TEST(!h->position_valid);  // always 0 in practice
    BOOST_TEST(h->utc_coupled);
    BOOST_TEST(h->app_data_valid);
    BOOST_TEST(h->slot_id == 7u);
    BOOST_TEST(h->tisb_site_id == 11u);
    BOOST_TEST(!parseUplinkHeader(b.data(), 7).has_value());

    // Southern wrap: raw lat 0x7fffff is 179.99998 deg, which is > 90 so becomes just below 0.
    const uint8_t neg[8] = {0xff, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto n = parseUplinkHeader(neg, 8);
    BOOST_REQUIRE(n);
    BOOST_TEST(n->lat_deg < 0.0);
    BOOST_TEST(n->lat_deg > -0.0001);
}

BOOST_AUTO_TEST_CASE(info_frames_stop_at_zero_length)
{
    auto u = uplinkWith({{0, {1, 2, 3}}, {15, {9}}, {0, {}}, {0, {7, 7}}});
    std::vector<std::tuple<uint8_t, std::vector<uint8_t>>> got;
    BOOST_TEST(forEachInfoFrame(u.data(), u.size(), [&](const InfoFrame& f) {
        got.emplace_back(f.type, std::vector<uint8_t>(f.data, f.data + f.len));
    }));
    BOOST_REQUIRE_EQUAL(got.size(), 2u);
    BOOST_TEST(std::get<0>(got[0]) == 0u);
    BOOST_TEST((std::get<1>(got[0]) == std::vector<uint8_t>{1, 2, 3}));
    BOOST_TEST(std::get<0>(got[1]) == 15u);
    BOOST_TEST((std::get<1>(got[1]) == std::vector<uint8_t>{9}));

    // A 9-bit length (> 255) decodes, and a frame filling the app data exactly is fine.
    auto full = uplinkWith({{0, std::vector<uint8_t>(422, 0x55)}});
    int n = 0;
    BOOST_TEST(forEachInfoFrame(full.data(), full.size(), [&](const InfoFrame& f) {
        ++n;
        BOOST_TEST(f.len == 422u);
    }));
    BOOST_TEST(n == 1);

    // All-zero application data: nothing, and not an error.
    std::vector<uint8_t> zero(kUplinkBytes, 0);
    n = 0;
    BOOST_TEST(forEachInfoFrame(zero.data(), zero.size(), [&](const InfoFrame&) { ++n; }));
    BOOST_TEST(n == 0);
}

BOOST_AUTO_TEST_CASE(info_frame_overrun_rejected)
{
    auto u = uplinkWith({{0, {1, 2}}});
    // Second frame header at app offset 4 claims 511 bytes.
    u[8 + 4] = 0xff;
    u[8 + 5] = 0x80;
    int n = 0;
    BOOST_TEST(!forEachInfoFrame(u.data(), u.size(), [&](const InfoFrame&) { ++n; }));
    BOOST_TEST(n == 1);  // the good frame before it was delivered

    // One byte too long for the 424-byte application data.
    auto v = uplinkWith({{0, std::vector<uint8_t>(422, 1)}});
    v[8] = 211;  // 423 = 211<<1 | 1
    v[9] = 0x80;
    n = 0;
    BOOST_TEST(!forEachInfoFrame(v.data(), v.size(), [&](const InfoFrame&) { ++n; }));
    BOOST_TEST(n == 0);

    // Too short to hold a header.
    BOOST_TEST(!forEachInfoFrame(u.data(), 4, [&](const InfoFrame&) { ++n; }));
    // A truncated buffer bounds the walk too.
    auto w = uplinkWith({{0, std::vector<uint8_t>(100, 1)}});
    BOOST_TEST(!forEachInfoFrame(w.data(), 50, [&](const InfoFrame&) { ++n; }));
    BOOST_TEST(n == 0);
}

BOOST_AUTO_TEST_CASE(apdu_header_bits)
{
    // Product 8, T-opt 2 (mm/dd hh:mm), unsegmented: 37 bits -> 5 bytes.
    {
        BitWriter w;
        w.put(0, 1); w.put(1, 1); w.put(0, 1);  // A G P
        w.put(8, 11);
        w.put(0, 1);   // S
        w.put(2, 2);   // T-opt
        w.put(1, 4);   // month
        w.put(17, 5);  // day
        w.put(23, 5);  // hours
        w.put(59, 6);  // minutes
        w.put(0x3, 3);  // padding bits (ignored)
        w.put(0xab, 8);  // first payload byte
        auto a = parseApdu(w.bytes.data(), w.bytes.size(), false);
        BOOST_REQUIRE(a);
        BOOST_TEST(!a->a_flag);
        BOOST_TEST(a->g_flag);
        BOOST_TEST(!a->p_flag);
        BOOST_TEST(!a->s_flag);
        BOOST_TEST(!a->segmented);
        BOOST_TEST(a->product_id == 8u);
        BOOST_TEST(a->t_opt == 2u);
        BOOST_TEST(*a->month == 1u);
        BOOST_TEST(*a->day == 17u);
        BOOST_TEST(a->hours == 23u);
        BOOST_TEST(a->minutes == 59u);
        BOOST_TEST(!a->seconds.has_value());
        BOOST_TEST(a->header_bytes == 5u);
        BOOST_TEST(w.bytes[a->header_bytes] == 0xabu);
    }
    // Product 8, T-opt 2, segmented: 37 + 28 = 65 bits -> 9 bytes (sample: file 739, 3 segments).
    {
        BitWriter w;
        w.put(0, 3);
        w.put(8, 11);
        w.put(1, 1);
        w.put(2, 2);
        w.put(1, 4); w.put(2, 5); w.put(4, 5); w.put(30, 6);
        w.put(739, 10);  // product file id
        w.put(3, 9);     // product file length = segments
        w.put(2, 9);     // APDU number (1-based)
        w.put(0, 7);
        w.put(0x22, 8);
        auto a = parseApdu(w.bytes.data(), w.bytes.size(), false);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->segmented);
        BOOST_TEST(a->s_flag);
        BOOST_TEST(a->apdu_number == 739u);
        BOOST_TEST(a->segment_count == 3u);
        BOOST_TEST(a->segment_number == 2u);
        BOOST_TEST(a->header_bytes == 9u);
        BOOST_TEST(w.bytes[9] == 0x22u);
        // One byte short of the header.
        BOOST_TEST(!parseApdu(w.bytes.data(), 8, false).has_value());
    }
    // Product 63, T-opt 0: 28 bits -> 4 bytes; all flags set.
    {
        BitWriter w;
        w.put(7, 3);
        w.put(63, 11);
        w.put(0, 1);
        w.put(0, 2);
        w.put(12, 5);
        w.put(34, 6);
        w.put(0xf, 4);
        auto a = parseApdu(w.bytes.data(), w.bytes.size(), false);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->a_flag);
        BOOST_TEST(a->g_flag);
        BOOST_TEST(a->p_flag);
        BOOST_TEST(a->product_id == 63u);
        BOOST_TEST(a->t_opt == 0u);
        BOOST_TEST(a->hours == 12u);
        BOOST_TEST(a->minutes == 34u);
        BOOST_TEST(!a->month.has_value());
        BOOST_TEST(!a->day.has_value());
        BOOST_TEST(a->header_bytes == 4u);
        BOOST_TEST(a->segment_count == 0u);
        BOOST_TEST(!parseApdu(w.bytes.data(), 3, false).has_value());
    }
    // Product 413 (needs all 11 bits), T-opt 0, segmented: 56 bits -> 7 bytes.
    {
        BitWriter w;
        w.put(0, 3);
        w.put(413, 11);
        w.put(1, 1);
        w.put(0, 2);
        w.put(5, 5);
        w.put(0, 6);
        w.put(1023, 10);
        w.put(511, 9);
        w.put(1, 9);
        auto a = parseApdu(w.bytes.data(), w.bytes.size(), false);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->product_id == 413u);
        BOOST_TEST(a->apdu_number == 1023u);
        BOOST_TEST(a->segment_count == 511u);
        BOOST_TEST(a->segment_number == 1u);
        BOOST_TEST(a->header_bytes == 7u);
    }
    // The sample's first APDU (first uplink, frame header 15 80 = 43 bytes, type 0):
    // product 8 (NOTAM), T-opt 2, 01/23 16:18.
    {
        const uint8_t real[] = {0x00, 0x21, 0x0d, 0xe0, 0x90, 0x82};
        auto a = parseApdu(real, sizeof real, false);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->product_id == 8u);
        BOOST_TEST(!a->segmented);
        BOOST_TEST(a->t_opt == 2u);
        BOOST_TEST(*a->month == 1u);
        BOOST_TEST(*a->day == 23u);
        BOOST_TEST(a->hours == 16u);
        BOOST_TEST(a->minutes == 18u);
        BOOST_TEST(a->header_bytes == 5u);
    }
    BOOST_TEST(!parseApdu(nullptr, 0, false).has_value());
}

BOOST_AUTO_TEST_CASE(apdu_time_opt_1_skipped_unless_experimental)
{
    for (unsigned topt : {1u, 3u}) {
        BitWriter w;
        w.put(0, 3);
        w.put(413, 11);
        w.put(0, 1);
        w.put(topt, 2);
        if (topt == 3) {
            w.put(6, 4);
            w.put(30, 5);
        }
        w.put(10, 5);
        w.put(11, 6);
        w.put(42, 6);  // seconds
        w.put(0, 8);
        ApduStatus st = ApduStatus::ok;
        BOOST_TEST(!parseApdu(w.bytes.data(), w.bytes.size(), false, &st).has_value());
        BOOST_TEST((st == ApduStatus::experimental_time_option));

        auto a = parseApdu(w.bytes.data(), w.bytes.size(), true, &st);
        BOOST_REQUIRE(a);
        BOOST_TEST((st == ApduStatus::ok));
        BOOST_TEST(a->t_opt == topt);
        BOOST_TEST(a->hours == 10u);
        BOOST_TEST(a->minutes == 11u);
        BOOST_TEST(*a->seconds == 42u);
        BOOST_TEST(a->header_bytes == (topt == 1 ? 5u : 6u));
        if (topt == 3) {
            BOOST_TEST(*a->month == 6u);
            BOOST_TEST(*a->day == 30u);
        } else {
            BOOST_TEST(!a->month.has_value());
        }
    }
    // Truncation is reported as such, not as an experimental option.
    const uint8_t two[] = {0x00, 0x00};
    ApduStatus st = ApduStatus::ok;
    BOOST_TEST(!parseApdu(two, sizeof two, false, &st).has_value());
    BOOST_TEST((st == ApduStatus::truncated));
}

BOOST_AUTO_TEST_CASE(dlac_known_strings)
{
    auto txt = [](const std::vector<unsigned>& c) {
        auto b = dlac(c);
        return decodeDlac(b.data(), b.size());
    };
    auto cat = [](std::vector<unsigned> a, const std::vector<unsigned>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };

    BOOST_TEST(txt(cat(codes("METAR KSJC"), {0})) == "METAR KSJC");
    // The full printable alphabet, codes 1..26 and 31..63.
    const std::string printable = "ABCDEFGHIJKLMNOPQRSTUVWXYZ| !\"#$%&'()*+,-./0123456789:;<=>?";
    BOOST_TEST(txt(codes(printable)) == printable);
    // LF, RS (kept as 0x1e for record splitting), NC dropped.
    BOOST_TEST(txt(cat(cat(codes("A"), {30, 29, 27}), codes("B"))) == "A\n\x1e" "B");
    // ETX ends the text; padding after it (ETX/NC or anything) is ignored.
    BOOST_TEST(txt(cat(cat(codes("AB"), {30, 0, 0}), codes("ZZ"))) == "AB\n");
    // TAB: the next code is a space count, not a character.
    BOOST_TEST(txt(cat(cat(codes("A"), {28, 3}), codes("B"))) == "A   B");
    BOOST_TEST(txt(cat(cat(codes("A"), {28, 0}), codes("B"))) == "AB");
    BOOST_TEST(txt(cat(codes("A"), {28, 13})) == "A" + std::string(13, ' '));  // 13 = 'M' as a char
    // TAB straddling the 3-byte group: TAB is code 4, its count is code 5.
    BOOST_TEST(txt(cat(cat(codes("ABC"), {28, 5}), codes("D"))) == "ABC     D");
    // A trailing TAB with no count emits nothing.
    BOOST_TEST(txt(cat(codes("AB"), {28})) == "AB");
    // 5 codes = 30 bits = 4 bytes: no garbage 6th character from the 2 padding bits.
    {
        auto b = dlac(codes("HELLO"));
        BOOST_REQUIRE_EQUAL(b.size(), 4u);
        b[3] = static_cast<uint8_t>(b[3] | 0x03);  // dirty padding
        BOOST_TEST(decodeDlac(b.data(), b.size()) == "HELLO");
    }
    // Byte lengths 1 and 2: 1 and 2 characters.
    {
        auto b = dlac(codes("XY"));
        BOOST_TEST(decodeDlac(b.data(), 1) == "X");
        BOOST_TEST(decodeDlac(b.data(), 2) == "XY");
        BOOST_TEST(decodeDlac(nullptr, 0).empty());
    }
}

BOOST_AUTO_TEST_CASE(hex_wrappers_tolerated)
{
    const std::vector<uint8_t> want = {0x01, 0xab, 0xff};
    std::vector<uint8_t> b;
    for (const char* l : {"+01abff;", "-01abff;", "01abff", "01ABFF", "+01abff;rs=3;rssi=-12.5;t=1604047960.094;",
                          "1604047960094 +01abff;", "1604047960094 +01abff;rs=1;", "+01abff;\r", "  +01abff;  ",
                          "+01abff"}) {
        b.assign(5, 9);
        BOOST_TEST_CONTEXT(l)
        {
            BOOST_TEST(hexToBytes(l, b));
            BOOST_TEST((b == want));
        }
    }
    for (const char* l : {"", "+", "+;", "+01abf;", "+01abzz;", "# comment", "+01 ab;", "1604047960094", "x01ab;",
                          "1604047960094 ", "+01ab;;", "++01ab;"}) {
        BOOST_TEST_CONTEXT(l) { BOOST_TEST(!hexToBytes(l, b)); }
    }
    BOOST_TEST(isUplinkLine("+00;"));
    BOOST_TEST(isUplinkLine("1604047960094 +00;"));
    BOOST_TEST(isUplinkLine(" +00;"));
    BOOST_TEST(!isUplinkLine("-00;"));
    BOOST_TEST(!isUplinkLine("1604047960094 -00;"));
    BOOST_TEST(!isUplinkLine("#  RECENTER 15 -> 1767 i:1167"));
    BOOST_TEST(!isUplinkLine("00ab"));
    BOOST_TEST(!isUplinkLine(""));
}

BOOST_AUTO_TEST_CASE(fuzz_random_payloads_no_throw)
{
    std::mt19937 rng(20261102u);
    std::uniform_int_distribution<int> byte(0, 255);
    Probe p;
    p.experimental = true;
    std::vector<uint8_t> b(kUplinkBytes);
    for (int i = 0; i < 10000; ++i) {
        for (auto& x : b)
            x = static_cast<uint8_t>(byte(rng));
        // Half the time make the first frame plausibly small so APDUs get parsed.
        if (i % 2 == 0) {
            b[6] = static_cast<uint8_t>(b[6] | 0x20);
            b[8] = static_cast<uint8_t>(b[8] & 0x1f);
            b[9] = static_cast<uint8_t>(b[9] & 0xf0);
        }
        const std::string line = (i % 3 == 0 ? "1604047960094 +" : "+") + toHex(b) + ";";
        BOOST_CHECK_NO_THROW(p.line(line));
        // Raw parsers on random lengths straight into the buffer.
        const auto n = static_cast<std::size_t>(byte(rng)) * 2;
        BOOST_CHECK_NO_THROW(parseApdu(b.data(), std::min(n, b.size()) % 12, i % 2 == 0));
        BOOST_CHECK_NO_THROW(decodeDlac(b.data(), std::min(n, b.size())));
        BOOST_CHECK_NO_THROW(forEachInfoFrame(b.data(), std::min(n, b.size()), [](const InfoFrame& f) {
            (void)decodeDlac(f.data, f.len);
        }));
        // Truncated and garbage lines.
        BOOST_CHECK_NO_THROW(p.line(line.substr(0, static_cast<std::size_t>(byte(rng)) * 3)));
    }
    BOOST_TEST_MESSAGE("fuzz counters: uplinks=" << p.c.uplinks << " bad_hex=" << p.c.bad_hex
                                                 << " bad_frames=" << p.c.bad_frames << " apdus=" << p.c.apdus
                                                 << " bad_apdu=" << p.c.bad_apdu
                                                 << " unsupported=" << p.c.unsupported_products);
    BOOST_TEST(p.c.uplinks >= 10000u);
    BOOST_TEST(p.c.bad_hex > 0u);
    BOOST_TEST(p.c.bad_frames > 0u);
    BOOST_TEST(p.c.apdus > 0u);
    BOOST_TEST(p.c.unsupported_products > 0u);
}

BOOST_AUTO_TEST_CASE(fixture_sample_parses)
{
    Probe p;
    std::size_t downlinks = 0;
    for (const auto& l : readLines(kFixtures + "/raw/uat-sample.txt.gz")) {
        if (!l.empty() && l[0] == '-')
            ++downlinks;
        BOOST_TEST(p.line(l), l.substr(0, 40));
    }
    BOOST_TEST_MESSAGE("uat-sample APDUs by product ID:" << table(p.by_product));
    BOOST_TEST(p.c.uplinks == 704u);
    BOOST_TEST(downlinks == 439u);
    BOOST_TEST(p.c.bad_hex == 0u);
    BOOST_TEST(p.c.bad_frames == 0u);
    BOOST_TEST(p.c.bad_apdu == 0u);
    BOOST_TEST(p.c.skipped_experimental == 0u);
    const std::map<uint16_t, uint64_t> want = {{8, 64}, {11, 2}, {12, 2}, {13, 71}, {63, 200}, {413, 224}};
    BOOST_TEST((p.by_product == want));
    BOOST_TEST((p.by_frame_type == std::map<uint8_t, uint64_t>{{0, 563}, {15, 2}}));
    BOOST_TEST((p.by_topt == std::map<uint8_t, uint64_t>{{0, 424}, {2, 139}}));
    BOOST_TEST((p.segmented_products == std::set<uint16_t>{8}));
    for (const auto& s : p.segments) {
        BOOST_TEST(s.apdu_number == 739u);
        BOOST_TEST(s.segment_count == 3u);
        BOOST_TEST(s.header_bytes == 9u);
        BOOST_TEST(s.segment_number >= 1u);
        BOOST_TEST(s.segment_number <= 3u);
    }
    BOOST_TEST(!p.segments.empty());

    // DLAC on real 413 text: report types per the reference §3.
    // Each report ends LF, RS (codes 30, 29), then ETX padding; one report per APDU.
    std::map<std::string, int> kinds;
    std::map<std::string, int> endings;
    for (const auto& t : p.texts) {
        const auto& s = t.second;
        kinds[s.substr(0, s.find(' '))]++;
        endings[s.size() >= 2 ? s.substr(s.size() - 2) : s]++;
        BOOST_TEST(std::count(s.begin(), s.end(), '\x1e') == 1, s);
    }
    BOOST_TEST((endings == std::map<std::string, int>{{"\n\x1e", 224}}));
    const std::map<std::string, int> want_kinds = {{"METAR", 147}, {"WINDS", 35}, {"TAF", 29},
                                                   {"PIREP", 6},   {"TAF.AMD", 4}, {"SPECI", 3}};
    BOOST_TEST((kinds == want_kinds));
}

BOOST_AUTO_TEST_CASE(fixture_tg30_parses)
{
    Probe p;
    std::size_t comments = 0;
    for (const auto& l : readLines(kFixtures + "/fisb/tg30.978")) {
        if (!l.empty() && l[0] == '#') {
            ++comments;
            continue;
        }
        BOOST_TEST(isUplinkLine(l), l.substr(0, 40));
        std::vector<uint8_t> b;
        BOOST_TEST(hexToBytes(l, b));
        BOOST_TEST(b.size() == kUplinkBytes);
        BOOST_TEST(p.line(l), l.substr(0, 40));
    }
    BOOST_TEST_MESSAGE("tg30 APDUs by product ID:" << table(p.by_product));
    BOOST_TEST(comments == 85u);
    BOOST_TEST(p.c.uplinks == 963u);
    BOOST_TEST(p.c.bad_hex == 0u);
    BOOST_TEST(p.c.bad_frames == 0u);
    BOOST_TEST(p.c.bad_apdu == 0u);
    BOOST_TEST(p.c.skipped_experimental == 0u);
    BOOST_TEST(p.c.unsupported_products == 0u);
    // Reference §3: tg30 has every product it lists except 15-17 (CWA, NOTAM-TRA/TMOA).
    for (uint16_t id : std::initializer_list<uint16_t>{8, 11, 12, 13, 14, 63, 64, 70, 71, 84, 90, 91, 103, 413})
        BOOST_TEST(p.by_product[id] >= 1u, "product " << id);
    const std::map<uint16_t, uint64_t> want = {{8, 421},  {11, 232}, {12, 20},  {13, 35},  {14, 262},
                                               {63, 898}, {64, 1087}, {70, 369}, {71, 117}, {84, 714},
                                               {90, 440}, {91, 220}, {103, 100}, {413, 1254}};
    BOOST_TEST((p.by_product == want));
    BOOST_TEST((p.by_frame_type == std::map<uint8_t, uint64_t>{{0, 6169}, {14, 17}, {15, 46}}));
    BOOST_TEST((p.by_topt == std::map<uint8_t, uint64_t>{{0, 5199}, {2, 970}}));
    BOOST_TEST((p.segmented_products == std::set<uint16_t>{8, 14}));
    for (const auto& s : p.segments) {
        BOOST_TEST(s.segment_number >= 1u);
        BOOST_TEST(s.segment_number <= s.segment_count);
    }
}

BOOST_AUTO_TEST_CASE(product_families)
{
    BOOST_TEST((productFamily(8) == ProductFamily::twgo));
    BOOST_TEST((productFamily(17) == ProductFamily::twgo));
    BOOST_TEST((productFamily(63) == ProductFamily::global_block));
    BOOST_TEST((productFamily(103) == ProductFamily::global_block));
    BOOST_TEST((productFamily(413) == ProductFamily::generic_text));
    BOOST_TEST((productFamily(9) == ProductFamily::other));
    BOOST_TEST((productFamily(2047) == ProductFamily::other));
    BOOST_TEST(std::string(productName(64)) == "NEXRAD CONUS");
    BOOST_TEST(std::string(productName(1234)) == "other");
    FisbCounters c;
    BOOST_TEST(c.uplinks == 0u);
    BOOST_TEST(c.blocks_rejected == 0u);
}
