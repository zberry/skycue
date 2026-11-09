// The 1090 decoder against readsb on the 120 s capture (frame decoder design,
// section 10, "Golden"). The golden file holds what readsb made of the same
// frames, one object per address (tests/fixtures/gen_raw_golden.sh). The
// capture is fed with arrival = the file's epoch, no reference position and no
// signal, so positions come from global CPR decoding only.
#define BOOST_TEST_MODULE decode_golden
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fixture_io.hpp"
#include "test_util.hpp"
#include "coding/bits.hpp"
#include "decode/modes_decoder.hpp"
#include "gz_line_reader.hpp"
#include "json_lite.hpp"
#include "test_slow.hpp"

using skycue_test::fileReadable;
using skycue_test::forEachLine;

using skycued::JsonObject;
using skycued::TrackObservation;
using skycued::decode::DecoderConfig;
using skycued::decode::ModesDecoder;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;

struct Snap {  // one position observation of ours with the fields it carried
    double lat = 0, lon = 0;
    std::optional<int32_t> alt;
    std::optional<float> gs, track, baro_rate, geom_rate;
    std::optional<std::string> callsign, category, squawk;
    std::optional<uint8_t> nic, nac_p;
};
struct Ours {
    std::vector<Snap> positions;
};

double deg(const JsonObject& o, const char* k) { return o.getDouble(k).value_or(1e9); }

bool near(double a, double b) { return std::fabs(a - b) <= 1e-5; }

// Per-field tally: readsb has it and we do not (reported), both have it and agree / differ.
struct Tally {
    int both = 0, agree = 0, ours_missing = 0;
};
std::map<std::string, Tally> g_tally;

void cmpNum(const std::string& hex, const char* what, const std::optional<double>& ours, double theirs, double tol,
            bool wrap360 = false)
{
    Tally& t = g_tally[what];
    if (!ours) {
        ++t.ours_missing;
        return;
    }
    ++t.both;
    double d = std::fabs(*ours - theirs);
    if (wrap360) d = std::min(d, 360.0 - d);  // 359.9 and 0.1 are 0.2 apart
    if (d <= tol + 1e-6) ++t.agree;
    else std::cout << hex << " " << what << ": ours " << *ours << ", readsb " << theirs << "\n";
}

void cmpStr(const std::string& hex, const char* what, const std::optional<std::string>& ours, const std::string& theirs)
{
    Tally& t = g_tally[what];
    if (!ours) {
        ++t.ours_missing;
        return;
    }
    ++t.both;
    if (*ours == theirs) ++t.agree;
    else std::cout << hex << " " << what << ": ours " << *ours << ", readsb " << theirs << "\n";
}

template <class T>
std::optional<double> asD(const std::optional<T>& v)
{
    if (!v) return std::nullopt;
    return static_cast<double>(*v);
}

}  // namespace

BOOST_AUTO_TEST_CASE(golden_matches_readsb)
{
    const std::string csv = kFixtures + "/raw/modes-120s.csv.gz", golden = kFixtures + "/raw/modes-120s.golden.jsonl.gz";
    if (!fileReadable(csv) || !fileReadable(golden)) {
        std::cout << "SKIP: raw/modes-120s.csv.gz or raw/modes-120s.golden.jsonl.gz is missing "
                     "(scripts/fixtures/fetch-test-captures.sh fetches the capture)\n";
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    ModesDecoder dec(DecoderConfig{}, "golden");
    std::map<std::string, Ours> ours;
    uint64_t our_positions = 0;
    dec.onObservation([&](const TrackObservation& o) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%06x", static_cast<unsigned>(o.id.address));
        if (!o.lat || !o.lon) return;
        Snap sn;
        sn.lat = *o.lat;
        sn.lon = *o.lon;
        sn.alt = o.alt_baro_ft;
        sn.gs = o.gs_kt;
        sn.track = o.track_deg;
        sn.baro_rate = o.baro_rate_fpm;
        sn.geom_rate = o.geom_rate_fpm;
        sn.callsign = o.callsign;
        sn.category = o.category;
        sn.squawk = o.squawk;
        sn.nic = o.nic;
        sn.nac_p = o.nac_p;
        ours[hex].positions.push_back(std::move(sn));
        ++our_positions;
    });

    {
        std::vector<uint8_t> bytes;
        forEachLine(csv, [&](const std::string& line) {
            const auto comma = line.find(',');
            if (comma == std::string::npos) return;
            if (!skycued::coding::parseHex(line.substr(comma + 1), bytes)) return;
            const double epoch = std::stod(line.substr(0, comma));
            dec.feed(bytes,
                     ModesDecoder::Clock::time_point(
                         std::chrono::duration_cast<ModesDecoder::Clock::duration>(std::chrono::duration<double>(epoch))),
                     std::nullopt);
        });
    }

    uint64_t golden_positions = 0;
    int eligible = 0, positioned = 0, second_ok = 0, last_ok = 0;
    {
        forEachLine(golden, [&](const std::string& line) {
            const auto g = JsonObject::parse(line);
            BOOST_REQUIRE(g);
            const std::string hex = g->getString("hex").value_or("");
            const int64_t n = g->getInt("positions").value_or(0);
            golden_positions += static_cast<uint64_t>(n);
            if (n < 2) return;
            ++eligible;
            const auto it = ours.find(hex);
            if (it == ours.end() || it->second.positions.empty()) {
                std::cout << "no position from us: " << hex << " (readsb " << n << ")\n";
                return;
            }
            ++positioned;
            const auto& ps = it->second.positions;
            const auto sec = g->getObject("second"), last = g->getObject("last");
            BOOST_REQUIRE(sec && last);
            bool found = false;
            for (const auto& p : ps)
                if (near(p.lat, deg(*sec, "lat")) && near(p.lon, deg(*sec, "lon"))) found = true;
            if (found) ++second_ok;
            else std::cout << "second position not found: " << hex << "\n";
            // Our observation at readsb's last position (our last one when none matches).
            const Snap* at = &ps.back();
            for (const auto& p : ps)
                if (near(p.lat, deg(*last, "lat")) && near(p.lon, deg(*last, "lon"))) at = &p;
            if (near(ps.back().lat, deg(*last, "lat")) && near(ps.back().lon, deg(*last, "lon"))) ++last_ok;
            else
                std::cout << "last position differs: " << hex << " ours " << ps.back().lat << "," << ps.back().lon
                          << " readsb " << deg(*last, "lat") << "," << deg(*last, "lon") << "\n";
            const Snap& l = *at;

            // The rest, as carried by that observation and only where readsb had it.
            if (const auto v = last->getInt("alt_baro")) cmpNum(hex, "alt_baro", asD(l.alt), static_cast<double>(*v), 0);
            if (const auto v = last->getDouble("gs")) cmpNum(hex, "gs", asD(l.gs), *v, 1.0);
            if (const auto v = last->getDouble("track")) cmpNum(hex, "track", asD(l.track), *v, 0.1, true);
            if (const auto v = last->getInt("baro_rate")) cmpNum(hex, "baro_rate", asD(l.baro_rate), static_cast<double>(*v), 0);
            if (const auto v = last->getInt("geom_rate")) cmpNum(hex, "geom_rate", asD(l.geom_rate), static_cast<double>(*v), 0);
            if (const auto s = g->getString("flight")) cmpStr(hex, "callsign", l.callsign, *s);
            if (const auto s = g->getString("category")) cmpStr(hex, "category", l.category, *s);
            if (const auto s = g->getString("squawk")) cmpStr(hex, "squawk", l.squawk, *s);
            if (const auto v = g->getInt("nic")) cmpNum(hex, "nic", asD(l.nic), static_cast<double>(*v), 0);
            if (const auto v = g->getInt("nac_p")) cmpNum(hex, "nac_p", asD(l.nac_p), static_cast<double>(*v), 0);
        });
    }

    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double ratio = golden_positions ? static_cast<double>(our_positions) / static_cast<double>(golden_positions) : 0;
    std::cout << "golden: readsb addresses with >= 2 positions " << eligible << ", positioned by us " << positioned
              << ", second matched " << second_ok << ", last matched " << last_ok << "; position observations ours "
              << our_positions << " vs readsb " << golden_positions << " (ratio " << ratio << "); wall " << wall
              << " s; frames " << dec.counters().frames << "\n";
    for (const auto& [name, t] : g_tally)
        std::cout << "golden field " << name << ": both " << t.both << ", agree " << t.agree
                  << ", readsb has it and we do not " << t.ours_missing << "\n";

    BOOST_REQUIRE(eligible > 0);
    BOOST_TEST(positioned >= 0.98 * eligible);
    BOOST_TEST(second_ok >= 0.98 * positioned);
    BOOST_TEST(last_ok >= 0.98 * positioned);
    for (const char* name : {"callsign", "category", "squawk"}) {
        const Tally& t = g_tally[name];
        BOOST_TEST_CONTEXT(name) {
            BOOST_TEST(t.both > 0, "no aircraft with both a value from us and from readsb: the comparison is vacuous");
            BOOST_TEST(t.agree == t.both);
        }
    }
    for (const char* name : {"alt_baro", "gs", "track", "baro_rate", "geom_rate", "nic", "nac_p"}) {
        const Tally& t = g_tally[name];
        BOOST_TEST_CONTEXT(name) {
            BOOST_TEST(t.both > 0, "no aircraft with both a value from us and from readsb: the comparison is vacuous");
            BOOST_TEST(t.agree >= 0.9 * t.both);
        }
    }
    BOOST_TEST(ratio >= 0.85);
    BOOST_TEST(ratio <= 1.15);
    BOOST_TEST(wall < std::chrono::duration<double>(slow(std::chrono::seconds(10))).count());
}
