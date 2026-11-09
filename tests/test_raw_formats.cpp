#define BOOST_TEST_MODULE skycued_raw_formats
#include <boost/test/unit_test.hpp>

#include "raw_formats.hpp"
#include "rotating_file.hpp"
#include "temp_dir.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace skycued;
using beast::Frame;
using beast::FrameSplitter;

namespace {

std::string bytes(std::initializer_list<int> v)
{
    std::string s;
    for (int b : v)
        s += static_cast<char>(b);
    return s;
}

// A frame as the wire carries it: 0x1a, type, then the body with every
// 0x1a doubled.
std::string wire(uint8_t type, const std::string& body)
{
    std::string s = bytes({0x1a, type});
    for (char c : body) {
        s += c;
        if (static_cast<uint8_t>(c) == 0x1a)
            s += c;
    }
    return s;
}

std::string tsBytes(uint64_t ts)
{
    std::string s;
    for (int i = 5; i >= 0; --i)
        s += static_cast<char>((ts >> (8 * i)) & 0xff);
    return s;
}

std::vector<Frame> splitAll(const std::string& stream, std::size_t chunk)
{
    FrameSplitter sp;
    std::vector<Frame> out;
    for (std::size_t i = 0; i < stream.size(); i += chunk) {
        std::size_t n = std::min(chunk, stream.size() - i);
        sp.feed(reinterpret_cast<const uint8_t*>(stream.data() + i), n, out);
    }
    return out;
}

struct TempDir : skycue_test::TempDir {
    TempDir() : skycue_test::TempDir("rawfmt") {}
    std::vector<std::string> list() const
    {
        std::vector<std::string> names;
        if (DIR* d = ::opendir(path.c_str())) {
            while (dirent* e = ::readdir(d))
                if (e->d_name[0] != '.')
                    names.push_back(e->d_name);
            ::closedir(d);
        }
        std::sort(names.begin(), names.end());
        return names;
    }
};

std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

BOOST_AUTO_TEST_CASE(body_lengths)
{
    BOOST_TEST(beast::bodyLength('1') == 6u + 1 + 2);
    BOOST_TEST(beast::bodyLength('2') == 6u + 1 + 7);
    BOOST_TEST(beast::bodyLength('3') == 6u + 1 + 14);
    BOOST_TEST(beast::bodyLength('4') == 6u + 1 + 14);
    BOOST_TEST(beast::bodyLength('5') == 6u + 1 + 14);
    BOOST_TEST(beast::bodyLength(0xe3) == 8u);
    BOOST_TEST(beast::bodyLength('6') == 0u);
    BOOST_TEST(beast::bodyLength(0x1a) == 0u);
}

BOOST_AUTO_TEST_CASE(splits_frames_with_escapes_at_any_chunking)
{
    const std::string b1 = tsBytes(0x1a0000000001) + bytes({0x80, 0x12, 0x34});
    const std::string b2 = tsBytes(0x00000000ff1a) + bytes({0x1a, 0x5d, 0x4c, 0xa1, 0x1a, 0x1a, 0x00, 0x01});
    std::string b3 = tsBytes(0x123456789abc) + bytes({0x40});
    for (int i = 0; i < 14; ++i)
        b3 += static_cast<char>(i == 13 ? 0x1a : 0x8d + i);
    const std::string stream = wire('1', b1) + wire('2', b2) + wire('3', b3);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{5}, stream.size()}) {
        BOOST_TEST_CONTEXT("chunk " << chunk)
        {
            auto frames = splitAll(stream, chunk);
            BOOST_REQUIRE(frames.size() == 3u);
            BOOST_TEST(frames[0].type == '1');
            BOOST_TEST(frames[0].timestamp == 0x1a0000000001u);
            BOOST_TEST(frames[0].body == b1);
            BOOST_TEST(frames[0].raw == wire('1', b1));
            BOOST_TEST(frames[0].start == 0u);
            BOOST_TEST(frames[1].type == '2');
            BOOST_TEST(frames[1].timestamp == 0xff1au);
            BOOST_TEST(frames[1].body == b2);
            BOOST_TEST(frames[1].raw == wire('2', b2));
            BOOST_TEST(frames[1].start == wire('1', b1).size());
            BOOST_TEST(frames[2].type == '3');
            BOOST_TEST(frames[2].body == b3);
            BOOST_TEST(frames[0].raw + frames[1].raw + frames[2].raw == stream);
        }
    }
}

BOOST_AUTO_TEST_CASE(skips_garbage_unknown_types_and_cut_frames)
{
    const std::string good = wire('2', tsBytes(42) + bytes({0x10, 1, 2, 3, 4, 5, 6, 7}));
    FrameSplitter sp;
    std::vector<Frame> out;
    // garbage, an unknown type, a doubled escape outside a frame, a cut-short
    // long frame (a new frame starts inside it), then a good frame.
    const std::string cut = wire('3', tsBytes(7) + bytes({0x10, 1, 2}));
    const std::string stream = bytes({0x00, 0x41}) + bytes({0x1a, 'x'}) + bytes({0x1a, 0x1a}) + cut + good;
    sp.feed(reinterpret_cast<const uint8_t*>(stream.data()), stream.size(), out);
    BOOST_REQUIRE(out.size() == 1u);
    BOOST_TEST(out[0].raw == good);
    BOOST_TEST(out[0].timestamp == 42u);
    BOOST_TEST(out[0].start == stream.size() - good.size());
    BOOST_TEST(sp.skippedBytes() == stream.size() - good.size());
    BOOST_TEST(sp.fed() == stream.size());
}

// What a write failure in skycue-rawrec can leave in a .bin: N good frames,
// one frame cut short (the rest never written), then, after the gap, M good
// frames from the reopen. The splitter resyncs on the next frame's 0x1a.
BOOST_AUTO_TEST_CASE(resyncs_after_a_truncated_frame)
{
    auto frame = [](int i) {
        const uint8_t type = "123"[i % 3];
        std::string body = tsBytes(1000 + static_cast<uint64_t>(i)) + bytes({0x1a});  // an escaped 0x1a in each
        while (body.size() < beast::bodyLength(type))
            body += static_cast<char>(i * 7 + static_cast<int>(body.size()));
        return wire(type, body);
    };
    const std::size_t n = 5, m = 7;
    std::string head, tail;
    for (std::size_t i = 0; i < n; ++i)
        head += frame(static_cast<int>(i));
    for (std::size_t i = 0; i < m; ++i)
        tail += frame(static_cast<int>(100 + i));
    const std::string full = wire('3', tsBytes(77) + bytes({0x8d, 0x1a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
    // Cut anywhere except between the two bytes of a doubled 0x1a (below).
    for (std::size_t cut = 1; cut < full.size(); ++cut) {
        const std::string trunc = full.substr(0, cut);
        // Count trailing 0x1a bytes after the header: an odd count inside the
        // body means the cut split an escape pair.
        std::size_t trailing = 0;
        for (std::size_t k = trunc.size(); k > 2 && static_cast<uint8_t>(trunc[k - 1]) == 0x1a; --k)
            ++trailing;
        if (trailing % 2 == 1)
            continue;
        BOOST_TEST_CONTEXT("cut at " << cut)
        {
            const std::string stream = head + trunc + tail;
            for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, stream.size()}) {
                FrameSplitter sp;
                std::vector<Frame> out;
                for (std::size_t i = 0; i < stream.size(); i += chunk)
                    sp.feed(reinterpret_cast<const uint8_t*>(stream.data()) + i, std::min(chunk, stream.size() - i),
                            out);
                BOOST_REQUIRE(out.size() == n + m);
                std::string got;
                for (const auto& f : out)
                    got += f.raw;
                BOOST_TEST((got == head + tail));
                BOOST_TEST(sp.skippedBytes() == trunc.size());
                BOOST_TEST(out[n].start == head.size() + trunc.size());
            }
        }
    }
    // A cut between the two bytes of a doubled 0x1a inside the body is
    // ambiguous: that 0x1a plus the next frame's leading 0x1a read as one
    // escaped data byte, so the cut frame swallows the start of the next one
    // and comes out as a single malformed frame of the right length (readsb
    // drops it: its CRC fails). Every frame after that one is read normally.
    const std::string split_escape = full.substr(0, 10);  // 1a '3', 6 timestamp bytes, 8d, the first 1a of the pair
    BOOST_REQUIRE(static_cast<uint8_t>(split_escape.back()) == 0x1a);
    const std::string stream = head + split_escape + tail;
    FrameSplitter sp;
    std::vector<Frame> out;
    sp.feed(reinterpret_cast<const uint8_t*>(stream.data()), stream.size(), out);
    BOOST_REQUIRE(out.size() == n + m);
    std::string got_head, got_rest;
    for (std::size_t i = 0; i < n; ++i)
        got_head += out[i].raw;
    BOOST_TEST((got_head == head));
    BOOST_TEST((out[n].raw != full));  // the merged, malformed one
    for (std::size_t i = n + 1; i < out.size(); ++i)
        got_rest += out[i].raw;
    BOOST_TEST((got_rest == tail.substr(frame(100).size())));
}

BOOST_AUTO_TEST_CASE(receiver_id_prefix_is_its_own_frame)
{
    const std::string id = wire(0xe3, bytes({1, 2, 3, 0x1a, 5, 6, 7, 8}));
    const std::string f = wire('1', tsBytes(9) + bytes({0x20, 0x0a, 0x0b}));
    auto frames = splitAll(id + f, 3);
    BOOST_REQUIRE(frames.size() == 2u);
    BOOST_TEST(frames[0].type == 0xe3);
    BOOST_TEST(frames[0].timestamp == 0u);
    BOOST_TEST(frames[0].raw == id);
    BOOST_TEST(frames[1].timestamp == 9u);
}

BOOST_AUTO_TEST_CASE(timestamp_rewrite_and_delta)
{
    const std::string body = tsBytes(5) + bytes({0x1a, 0x5d, 1, 2, 3, 4, 5, 6});
    auto frames = splitAll(wire('2', body), 64);
    BOOST_REQUIRE(frames.size() == 1u);
    const uint64_t ts = 0x001a1a00001a;
    const std::string want = wire('2', tsBytes(ts) + body.substr(6));
    BOOST_TEST(beast::withTimestamp(frames[0], ts) == want);
    BOOST_TEST(beast::withTimestamp(frames[0], ts + (uint64_t{1} << 48)) == want);  // masked to 48 bits
    auto again = splitAll(want, 1);
    BOOST_REQUIRE(again.size() == 1u);
    BOOST_TEST(again[0].timestamp == ts);

    BOOST_TEST(beast::timestampDelta(100, 250) == 150);
    BOOST_TEST(beast::timestampDelta(250, 100) == -150);
    BOOST_TEST(beast::timestampDelta(beast::kTimestampMask - 9, 5) == 15);  // wrapped
    BOOST_TEST(beast::timestampDelta(5, beast::kTimestampMask - 9) == -15);
}

BOOST_AUTO_TEST_CASE(csv_lines)
{
    auto m = parseCsvMessage("1783963099.005369,5f406a936f50f0");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->epoch_ns == 1783963099005369000LL);
    BOOST_TEST(m->hex == "5F406A936F50F0");
    m = parseCsvMessage("1783963099.005369,A00007318DB436F0A80000A7959E\r");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->hex.size() == 28u);
    m = parseCsvMessage("12,5F406A936F50F0");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->epoch_ns == 12000000000LL);
    m = parseCsvMessage("1.1234567891,5F406A936F50F0");  // beyond ns: truncated
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->epoch_ns == 1123456789LL);
    for (const char* bad : {"", "# c", "timestamp,hex", "1.5,", ",5F406A936F50F0", "1.5,5F406A936F50F",
                            "1.5,5F406A936F50FG", "1.5;5F406A936F50F0", "-1.5,5F406A936F50F0", "1..5,5F406A936F50F0",
                            "1.5,5F406A936F50F0,x"})
        BOOST_TEST(!parseCsvMessage(bad).has_value(), "accepted: " << bad);
}

// Found by fuzz_raw_formats (mode 2): parseCsvMessage multiplied every
// integer digit into an int64_t before checking for more than 10 of them, so
// 21 digits overflowed (undefined behaviour); and 10 digits passed the check
// but secs * 1e9 overflowed past 9223372036 s (the year 2262), giving a
// garbage, even negative, epoch_ns.
BOOST_AUTO_TEST_CASE(fuzz_raw_formats_csv_seconds_overflow)
{
    const std::string payload("178396343000000005369,5F406A03\r\0\0\0\0\0\0\0r099.005369,5F006A936F50F0", 64);
    BOOST_TEST(payload.size() == 64u);
    BOOST_TEST(!parseCsvMessage(payload).has_value());
    BOOST_TEST(!parseCsvMessage("178396343000000005369,5F406A936F50F0").has_value());
    for (const char* bad : {"9999999999.5,5F406A936F50F0", "9223372036,5F406A936F50F0"})
        BOOST_TEST(!parseCsvMessage(bad).has_value(), "accepted: " << bad);
    // The largest accepted epoch is still exact.
    const auto m = parseCsvMessage("9223372035.999999999,5F406A936F50F0");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->epoch_ns == 9223372035999999999LL);
}

BOOST_AUTO_TEST_CASE(avr_lines)
{
    // 1 s = 12e6 ticks = 0xB71B00
    BOOST_TEST(avrLine(1000000000LL, "5F406A936F50F0") == "@000000B71B005F406A936F50F0;");
    // (1783963099.005369 * 12e6) mod 2^48
    const uint64_t ticks = (static_cast<uint64_t>(1783963099005369LL) * 12u) % (uint64_t{1} << 48);
    char want[16];
    std::snprintf(want, sizeof want, "%012llX", static_cast<unsigned long long>(ticks));
    BOOST_TEST(avrLine(1783963099005369000LL, "AB") == std::string("@") + want + "AB;");
}

// Found by fuzz_raw_formats (mode 2, the avrLine round trip of a parsed CSV
// line, on "9000000000,5F406A936F50F0"): avrLine computed epoch_ns * 3 in
// uint64_t, which wraps past 2^64 / 3 ns (the year 2165), well inside what
// parseCsvMessage accepts, so later epochs got wrong ticks.
BOOST_AUTO_TEST_CASE(fuzz_raw_formats_avr_ticks_wrap)
{
    const auto m = parseCsvMessage("9000000000,5F406A936F50F0");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(avrLine(m->epoch_ns, m->hex) == "@B16D83BE00005F406A936F50F0;");
    // The largest epoch parseCsvMessage accepts.
    BOOST_TEST(avrLine(9223372035999999999LL, "AB") == "@374BC60B6BFFAB;");
}

BOOST_AUTO_TEST_CASE(avr_input_lines)
{
    auto m = parseAvrLine("@000000B71B005F406A936F50F0;\r\n");
    BOOST_REQUIRE(m);
    BOOST_TEST(*m->ticks == 0xB71B00u);
    BOOST_TEST(m->hex == "5F406A936F50F0");
    m = parseAvrLine("*8d4840d6202cc371c32ce0576098;");
    BOOST_REQUIRE(m);
    BOOST_TEST(!m->ticks);
    BOOST_TEST(m->hex == "8D4840D6202CC371C32CE0576098");
    BOOST_TEST(parseAvrLine(avrLine(1783963099005369000LL, "5D486258DD7987"))->hex == "5D486258DD7987");
    for (const char* bad : {"", "@12;", "*ABC;", "@000000B71B005F406A936F50F0", "#x", "*5F406A936F50FZ;"})
        BOOST_TEST(!parseAvrLine(bad).has_value(), "accepted: " << bad);
    // Mode A/C (4 digits) is not a Mode S frame.
    BOOST_TEST(!parseAvrLine("@000000B71B001234;").has_value());
    BOOST_TEST(!parseAvrLine("*1234;").has_value());
}

BOOST_AUTO_TEST_CASE(uat_lines)
{
    auto u = parseUatLine("-00a66ef135445d525a0c0519119021204800;\n");
    BOOST_REQUIRE(u.has_value());
    BOOST_TEST(!u->epoch_ms.has_value());
    BOOST_TEST(u->text == "-00a66ef135445d525a0c0519119021204800;");
    u = parseUatLine("1793536496789 +3514c952;rs=3;\r\n");
    BOOST_REQUIRE(u.has_value());
    BOOST_TEST(*u->epoch_ms == 1793536496789LL);
    BOOST_TEST(u->text == "+3514c952;rs=3;");
    for (const char* bad : {"", "\n", "# comment", "123 x;", "123", "123 ", "abc -00;", " -00;", "12a -00;"})
        BOOST_TEST(!parseUatLine(bad).has_value(), "accepted: " << bad);
    BOOST_TEST(!parseUatLine("1793536496789 +3514c952;")->offset_ns.has_value());
}

BOOST_AUTO_TEST_CASE(stratux_uat_lines)
{
    auto u = parseUatLine("1441566520123456789,-00a66ef135445d525a0c0519119021204800;rs=2;\n");
    BOOST_REQUIRE(u);
    BOOST_TEST(!u->epoch_ms);
    BOOST_TEST(*u->offset_ns == 1441566520123456789LL);
    BOOST_TEST(u->text == "-00a66ef135445d525a0c0519119021204800;rs=2;");
    for (const char* bad : {"123,", "123,x;", ",+00;", "12a,+00;"})
        BOOST_TEST(!parseUatLine(bad).has_value(), "accepted: " << bad);
    // The stratux log's session header, and an <ns> past int64_t.
    for (const char* bad : {"START,Wed Sep 2 04:23:37 +0000 UTC 2015", "9223372036854775808,+00;",
                            "12345678901234567890,+00;"})
        BOOST_TEST(!parseUatLine(bad).has_value(), "accepted: " << bad);
    u = parseUatLine("9223372036854775807,+00;");
    BOOST_REQUIRE(u);
    BOOST_TEST(*u->offset_ns == INT64_MAX);
    // 19 digits are an <ns> only: an epoch_ms has at most 18.
    BOOST_TEST(!parseUatLine("1234567890123456789 +00;").has_value());
}

BOOST_AUTO_TEST_CASE(stratux_sessions_in_file_order)
{
    BOOST_TEST(isStratuxStart("START,Wed Sep 2 04:25:25 +0000 UTC 2015\r\n"));
    BOOST_TEST(!isStratuxStart("START"));
    BOOST_TEST(!isStratuxStart("123,+00;"));
    // Session 1 at 5.0/5.2/5.2 s; a START, then session 2 below it (1.0/1.3 s);
    // a backward jump without a START, session 3 (0.5/0.9 s); a START before a
    // forward offset still begins session 4 (9.0 s).
    StratuxClock c;
    c.startSession();  // a header before the first line changes nothing
    std::vector<int64_t> got;
    for (int64_t off : {5'000'000'000LL, 5'200'000'000LL, 5'200'000'000LL})
        got.push_back(*c.next(off));
    c.startSession();
    for (int64_t off : {1'000'000'000LL, 1'300'000'000LL})
        got.push_back(*c.next(off));
    for (int64_t off : {500'000'000LL, 900'000'000LL})
        got.push_back(*c.next(off));
    c.startSession();
    got.push_back(*c.next(9'000'000'000LL));
    const std::vector<int64_t> want = {5'000'000'000LL, 5'200'000'000LL, 5'200'000'000LL, 6'200'000'000LL,
                                       6'500'000'000LL, 7'500'000'000LL, 7'900'000'000LL, 8'900'000'000LL};
    BOOST_TEST(got == want, boost::test_tools::per_element());
    // Past int64_t: nullopt, and the clock carries on from where it was.
    StratuxClock big;
    BOOST_TEST(*big.next(INT64_MAX - 10) == INT64_MAX - 10);
    BOOST_TEST(!big.next(0).has_value());
    BOOST_TEST(*big.next(INT64_MAX) == INT64_MAX);
}

BOOST_AUTO_TEST_CASE(rotating_file_names_and_rotation)
{
    TempDir tmp;
    RotatingFileConfig cfg;
    cfg.dir = tmp.path + "/a/b";
    cfg.stream = "beast";
    cfg.ext = "bin";
    cfg.rotate_bytes = 10;
    RotatingFile f(cfg);
    BOOST_TEST(f.currentPath().empty());  // nothing written yet: no file
    BOOST_TEST(!f.rotationDue());
    BOOST_REQUIRE(f.write("0123456789", 10));
    const std::string first = f.currentPath();
    BOOST_TEST(first.find(cfg.dir + "/beast-") == 0u);
    BOOST_TEST(first.size() == (cfg.dir + "/beast-YYYYMMDD-HHMMSS.bin").size());
    BOOST_TEST(f.rotationDue());
    f.rotate();
    BOOST_REQUIRE(f.write("abc", 3));
    BOOST_TEST(f.currentPath() != first);
    BOOST_TEST(!f.rotationDue());
    BOOST_REQUIRE(f.flush());
    BOOST_TEST(slurp(first) == "0123456789");
    BOOST_TEST(slurp(f.currentPath()) == "abc");
    BOOST_TEST(f.bytes() == 3u);
}

BOOST_AUTO_TEST_CASE(unique_stem_numbers_within_a_second)
{
    TempDir tmp;
    const std::string base = tmp.path + "/x-20261101-000000";
    std::string last;
    int suffix = 0;
    BOOST_TEST(uniqueStem(base, {"bin"}, last, suffix) == base);
    std::ofstream(base + ".bin").put('x');
    BOOST_TEST(uniqueStem(base, {"bin"}, last, suffix) == base + "-1");
    BOOST_TEST(uniqueStem(base, {"bin", "txt"}, last, suffix) == base + "-2");  // continues after the last used
    std::string last2;
    int suffix2 = 0;
    BOOST_TEST(uniqueStem(base, {"jsonl", "txt"}, last2, suffix2) == base);  // only base.bin exists
    std::ofstream(base + ".txt").put('x');
    std::ofstream(base + "-1.txt").put('x');
    std::string last3;
    int suffix3 = 0;
    BOOST_TEST(uniqueStem(base, {"jsonl", "txt"}, last3, suffix3) == base + "-2");
}

// The Recorder's write_failure_backs_off_and_reuses_name, for RotatingFile
// (what skycue-rawrec writes through).
BOOST_AUTO_TEST_CASE(rotating_file_write_failure_backs_off_and_reuses_name)
{
    using steady = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    TempDir tmp;
    RotatingFileConfig cfg;
    cfg.dir = tmp.path;
    cfg.stream = "uat";
    cfg.ext = "txt";
    cfg.reopen_initial = 40ms;
    cfg.reopen_max = 200ms;
    RotatingFile f(cfg);
    BOOST_REQUIRE(f.write("a\n", 2));
    BOOST_REQUIRE(f.flush());
    const std::string first = f.currentPath();
    BOOST_TEST(f.reopenDelay().count() == 0);
    BOOST_TEST(!f.failing());

    f.setFailWritesForTest(true);
    std::vector<std::pair<steady::time_point, std::chrono::milliseconds>> changes;
    uint64_t attempted = 0;
    const auto until = steady::now() + 1200ms;
    while (steady::now() < until) {
        BOOST_TEST(!f.write("xx\n", 3));
        attempted += 3;
        if (changes.empty() || f.reopenDelay() != changes.back().second)
            changes.emplace_back(steady::now(), f.reopenDelay());
        std::this_thread::sleep_for(1ms);
    }
    BOOST_TEST(f.failing());
    BOOST_TEST(f.droppedBytes() == attempted);
    BOOST_REQUIRE(changes.size() == 4u);  // 40, 80, 160, then capped at 200
    BOOST_TEST(changes[0].second.count() == 40);
    BOOST_TEST(changes[1].second.count() == 80);
    BOOST_TEST(changes[2].second.count() == 160);
    BOOST_TEST(changes[3].second.count() == 200);
    for (std::size_t i = 1; i < changes.size(); ++i) {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(changes[i].first - changes[i - 1].first);
        BOOST_CHECK_MESSAGE(gap.count() >= changes[i - 1].second.count() - 5,
                            "attempt " << i << " after " << gap.count() << " ms");
    }
    // Several reopens, one extra file only.
    const auto files = tmp.list();
    BOOST_REQUIRE(files.size() == 2u);
    const std::string second = tmp.path + "/" + (tmp.path + "/" + files[0] == first ? files[1] : files[0]);
    BOOST_TEST(slurp(first) == "a\n");

    // Recovery appends to the streak's file and resets the backoff.
    f.setFailWritesForTest(false);
    std::this_thread::sleep_for(250ms);
    BOOST_REQUIRE(f.write("b\n", 2));
    BOOST_TEST(!f.failing());
    BOOST_TEST(f.reopenDelay().count() == 0);
    BOOST_TEST(f.currentPath() == second);
    BOOST_REQUIRE(f.write("c\n", 2));
    BOOST_REQUIRE(f.flush());
    BOOST_TEST(slurp(second) == "b\nc\n");
    BOOST_TEST(tmp.list().size() == 2u);

    // A new streak starts again from the initial delay; a flush failure counts too.
    f.setFailWritesForTest(true);
    BOOST_TEST(!f.flush());
    BOOST_TEST(f.reopenDelay().count() == 40);
}

BOOST_AUTO_TEST_CASE(rotating_file_open_failure_retries_the_same_name)
{
    using namespace std::chrono_literals;
    TempDir tmp;
    std::ofstream(tmp.path + "/blocker").put('x');  // a file where the directory should be
    RotatingFileConfig cfg;
    cfg.dir = tmp.path + "/blocker/raw";
    cfg.stream = "beast";
    cfg.ext = "bin";
    cfg.reopen_initial = 20ms;
    cfg.reopen_max = 20ms;
    RotatingFile f(cfg);
    BOOST_TEST(!f.write("abc", 3));
    BOOST_TEST(f.failing());
    BOOST_TEST(f.reopenDelay().count() == 20);
    BOOST_TEST(!f.write("abc", 3));  // not due yet: dropped without trying
    BOOST_TEST(f.droppedBytes() == 6u);

    // The directory becomes usable: the next due reopen succeeds.
    ::remove((tmp.path + "/blocker").c_str());
    std::this_thread::sleep_for(30ms);
    BOOST_REQUIRE(f.write("def", 3));
    BOOST_TEST(!f.failing());
    BOOST_REQUIRE(f.flush());
    BOOST_TEST(slurp(f.currentPath()) == "def");
}

// ---------------------------------------------------------------- encodeFrame

BOOST_AUTO_TEST_CASE(encode_frame_short_and_long_zero_timestamp)
{
    const std::string s = beast::encodeFrame("5D4840D6202CC3", 0);
    BOOST_TEST(s == bytes({0x1a, '2', 0, 0, 0, 0, 0, 0, 0, 0x5d, 0x48, 0x40, 0xd6, 0x20, 0x2c, 0xc3}));
    const std::string l = beast::encodeFrame("8d4840d6202cc371c32ce0576098", 0);  // lowercase digits too
    BOOST_TEST(l == bytes({0x1a, '3', 0, 0, 0, 0, 0, 0, 0, 0x8d, 0x48, 0x40, 0xd6, 0x20, 0x2c, 0xc3, 0x71, 0xc3, 0x2c,
                           0xe0, 0x57, 0x60, 0x98}));
    // A non-zero timestamp is big-endian in the six bytes after the type.
    const std::string t = beast::encodeFrame("5D4840D6202CC3", 0x0102030405FF);
    BOOST_TEST(t == bytes({0x1a, '2', 1, 2, 3, 4, 5, 0xff, 0, 0x5d, 0x48, 0x40, 0xd6, 0x20, 0x2c, 0xc3}));
    BOOST_TEST(beast::encodeFrame("5D4840D6202CC3", 0xFFFF000000000001ULL) ==
               bytes({0x1a, '2', 0, 0, 0, 0, 0, 1, 0, 0x5d, 0x48, 0x40, 0xd6, 0x20, 0x2c, 0xc3}));  // low 48 bits
}

BOOST_AUTO_TEST_CASE(encode_frame_escapes_1a_in_payload_and_timestamp)
{
    const std::string s = beast::encodeFrame("5D1A40D6202C1A", 0x00001a000000);
    BOOST_TEST(s == bytes({0x1a, '2', 0, 0, 0x1a, 0x1a, 0, 0, 0, 0, 0x5d, 0x1a, 0x1a, 0x40, 0xd6, 0x20, 0x2c, 0x1a, 0x1a}));
    // The type byte is never doubled, and a timestamp of all 0x1a doubles each.
    const std::string t = beast::encodeFrame("5D4840D6202CC3", 0x1a1a1a1a1a1a);
    BOOST_TEST(t.substr(0, 2) == bytes({0x1a, '2'}));
    BOOST_TEST(t.size() == 2u + 12u + 1u + 7u);
}

BOOST_AUTO_TEST_CASE(encode_frame_round_trips_through_splitter)
{
    std::string stream;
    const std::string hexes[] = {"8D4840D6202CC371C32CE0576098", "5D1A40D6202C1A", "1A1A1A1A1A1A1A1A1A1A1A1A1A1A"};
    const uint64_t ticks[] = {0, 0x1a1a1a1a1a1aULL, 0xffffffffffffULL};
    for (int i = 0; i < 3; ++i)
        stream += beast::encodeFrame(hexes[i], ticks[i]);
    FrameSplitter sp;
    std::vector<Frame> out;
    sp.feed(reinterpret_cast<const uint8_t*>(stream.data()), stream.size(), out);
    BOOST_REQUIRE_EQUAL(out.size(), 3u);
    BOOST_TEST(sp.skippedBytes() == 0u);
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(out[static_cast<std::size_t>(i)].type == (hexes[i].size() == 14 ? '2' : '3'));
        BOOST_TEST(out[static_cast<std::size_t>(i)].timestamp == ticks[i]);
        const std::string& body = out[static_cast<std::size_t>(i)].body;
        BOOST_REQUIRE_EQUAL(body.size(), 7 + hexes[i].size() / 2);
        BOOST_TEST(body[6] == '\0');  // signal
        std::string hex;
        for (std::size_t k = 7; k < body.size(); ++k) {
            char b[3];
            std::snprintf(b, sizeof b, "%02X", static_cast<unsigned>(static_cast<uint8_t>(body[k])));
            hex += b;
        }
        BOOST_TEST(hex == hexes[i]);
        BOOST_TEST(out[static_cast<std::size_t>(i)].raw == beast::encodeFrame(hexes[i], ticks[i]));
    }
}

BOOST_AUTO_TEST_CASE(encode_frame_refuses_other_lengths)
{
    for (const char* hex : {"", "5D", "0123", "5D4840D6202CC", "5D4840D6202CC3A", "8D4840D6202CC371C32CE057609", "8D4840D6202CC371C32CE0576098AB"})
        BOOST_TEST(beast::encodeFrame(hex, 0).empty(), hex);
    BOOST_TEST(beast::encodeFrame("5D4840D6202CCZ", 0).empty());  // not a hex digit
    // Mode A/C only through the typed form skycue-rawplay uses.
    BOOST_TEST(beast::encodeFrameOfType('1', "0123", 0) == bytes({0x1a, '1', 0, 0, 0, 0, 0, 0, 0, 0x01, 0x23}));
    BOOST_TEST(beast::encodeFrameOfType('2', "0123", 0).empty());
    BOOST_TEST(beast::encodeFrameOfType('9', "0123", 0).empty());
}
