// The 1090 decoder against OpenSky's own decoding of the same frames
// (tests/fixtures/opensky, fetched by scripts/fixtures/fetch-test-captures.sh).
//
// Each OpenSky "incident-data" tarball (CC BY 4.0) holds Trino tables for one
// aircraft. This test reads the position, velocity and identification tables. Every row carries the raw frame (`rawmsg`, hex) with its
// receive time (`mintime`, epoch seconds) next to the fields OpenSky decoded from it; per the
// tarball's README.txt the tables are extracts of OpenSky's `*_data4` tables for one
// icao24, time ordered, one row per reception (`msgcount` > 1 where the same frame was
// aggregated from several sensors: the frame is fed once, at `mintime`).
//
//   position_data4        lat, lon (degrees), alt (metres, baro or GNSS per `baroalt`),
//                         surface (true: a TC 5-8 frame, `alt` empty); lat and lon are
//                         empty while OpenSky itself had no pair or reference
//   velocity_data4        velocity (m/s ground speed), heading (degrees true)
//   identification_data4  identity (callsign, space padded)
//
// The frames of the three tables are merged in time order and fed through ModesDecoder with
// no reference position and no signal. The OpenSky columns are not aligned with the
// decoder's output one for one: OpenSky decodes position, altitude, velocity and identity
// frame by frame, but the decoder emits an observation only on a frame that gave a position
// (or on a change of identity, squawk or altitude when it has had none for 10 s) and
// fills it with the state it holds (design 7.2). So position and altitude are compared on
// the frame that produced the observation; ground speed, track and callsign on each observation,
// with the latest row of the velocity (no older than 60 s, the decoder's field age) or
// identification table.
//
// What is compared where both sides have a value: positions within 1e-4 degrees (OpenSky's
// float32 positions are good to about 4e-6; the maximum deviation is printed), altitude
// exact in feet (OpenSky's metres are the feet value times 0.3048), ground speed within
// 1 kt, track within 0.5 degrees, callsign exact. Rows OpenSky decoded and we did not are
// counted and printed, and bounded separately.
#define BOOST_TEST_MODULE decode_opensky
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coding/bits.hpp"
#include "decode/modes_decoder.hpp"
#include "test_slow.hpp"

using skycued::TrackObservation;
using skycued::decode::DecoderConfig;
using skycued::decode::ModesDecoder;

namespace {

namespace fs = std::filesystem;
const std::string kFixtures = SKYCUE_FIXTURE_DIR;

constexpr double kMsToKt = 1.0 / 0.514444;  // 1 kt = 0.514444 m/s
constexpr double kFtToM = 0.3048;

bool endsWith(const std::string& s, const std::string& suf)
{
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// A CSV line to fields; a double-quoted field may hold commas.
std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == ',' && !q) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r')
            cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

struct Row {
    double t = 0;
    int table = 0;  // 0 position, 1 velocity, 2 identification
    std::string raw;
    std::map<std::string, std::string> f;
};

std::vector<Row> readTable(const fs::path& p, int table)
{
    std::vector<Row> rows;
    std::ifstream in(p);
    std::string line;
    if (!std::getline(in, line)) return rows;
    const auto head = splitCsv(line);
    while (std::getline(in, line)) {
        const auto c = splitCsv(line);
        if (c.size() != head.size()) continue;
        Row r;
        r.table = table;
        for (size_t i = 0; i < head.size(); ++i) r.f[head[i]] = c[i];
        r.raw = r.f["rawmsg"];
        try {
            r.t = std::stod(r.f["mintime"]);
        } catch (...) {
            continue;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::optional<double> num(const Row& r, const char* k)
{
    const auto it = r.f.find(k);
    if (it == r.f.end() || it->second.empty()) return std::nullopt;
    try {
        return std::stod(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

struct Tally {
    int rows = 0;     // OpenSky has the field
    int both = 0;     // and we reported it from that frame
    int agree = 0;
    double maxdev = 0;
    void add(bool ours_has, bool ok, double dev = 0)
    {
        ++rows;
        if (!ours_has) return;
        ++both;
        if (ok) ++agree;
        maxdev = std::max(maxdev, dev);
    }
};

std::string hexOf(const TrackObservation& o)
{
    char b[16];
    std::snprintf(b, sizeof b, "%06x", static_cast<unsigned>(o.id.address));
    return b;
}

}  // namespace

BOOST_AUTO_TEST_CASE(decoded_fields_match_opensky)
{
    std::vector<fs::path> dirs;
    if (fs::is_directory(kFixtures + "/opensky"))
        for (const auto& e : fs::directory_iterator(kFixtures + "/opensky"))
            if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    if (dirs.empty()) {
        std::cout << "SKIP: opensky/ is missing (scripts/fixtures/fetch-test-captures.sh fetches and unpacks it)\n";
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    int files_checked = 0;
    for (const auto& dir : dirs) {
        std::vector<Row> rows;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string n = e.path().filename().string();
            if (endsWith(n, "_position_data4.csv")) {
                auto v = readTable(e.path(), 0);
                rows.insert(rows.end(), v.begin(), v.end());
            } else if (endsWith(n, "_velocity_data4.csv")) {
                auto v = readTable(e.path(), 1);
                rows.insert(rows.end(), v.begin(), v.end());
            } else if (endsWith(n, "_identification_data4.csv")) {
                auto v = readTable(e.path(), 2);
                rows.insert(rows.end(), v.begin(), v.end());
            }
        }
        if (rows.empty()) continue;
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
        ++files_checked;

        ModesDecoder dec(DecoderConfig{}, "opensky");
        std::vector<TrackObservation> got;
        dec.onObservation([&](const TrackObservation& o) { got.push_back(o); });

        Tally pos, surf, alt, gs, trk, cs;
        int frames = 0, bad = 0, obs_total = 0;
        // OpenSky's latest velocity and identity rows before the observation being checked: the
        // decoder emits an observation only on a frame that gave a position (or a change with
        // none), carrying the other fields as state, so those fields are compared with the last
        // row of their table, not with the frame that triggered the observation.
        std::optional<double> vel_kt, vel_trk, vel_t;
        std::string ident;
        for (const Row& r : rows) {
            std::vector<uint8_t> bytes;
            if (!skycued::coding::parseHex(r.raw, bytes)) {
                ++bad;
                continue;
            }
            ++frames;
            got.clear();
            dec.feed(bytes,
                     ModesDecoder::Clock::time_point(std::chrono::duration_cast<ModesDecoder::Clock::duration>(
                         std::chrono::duration<double>(r.t))),
                     std::nullopt);
            if (r.table == 1) {
                const auto v = num(r, "velocity"), h = num(r, "heading");
                vel_kt = v ? std::optional<double>(*v * kMsToKt) : std::nullopt;
                vel_trk = h;
                vel_t = r.t;
            } else if (r.table == 2) {
                ident = r.f.at("identity");
                while (!ident.empty() && ident.back() == ' ') ident.pop_back();
            }
            // The observation this frame produced (the last one, should there be several).
            const TrackObservation* o = got.empty() ? nullptr : &got.back();
            if (o) ++obs_total;

            if (r.table == 0) {
                const auto lat = num(r, "lat"), lon = num(r, "lon");
                const bool surface = r.f.at("surface") == "True";
                if (lat && lon) {
                    const bool have = o && o->lat && o->lon;
                    const double dev = have ? std::max(std::fabs(*o->lat - *lat), std::fabs(*o->lon - *lon)) : 0;
                    (surface ? surf : pos).add(have, dev <= 1e-4, dev);
                    if (have && dev > 1e-4 && (pos.both - pos.agree) + (surf.both - surf.agree) <= 5)
                        std::cout << "  position differs " << hexOf(*o) << " t=" << r.t << " ours " << *o->lat << ","
                                  << *o->lon << " opensky " << *lat << "," << *lon << "\n";
                }
                // Altitude of the frame, where an observation came out of it.
                if (const auto m = num(r, "alt"); m && !surface && o) {
                    const bool baro = r.f.at("baroalt") == "True";
                    const std::optional<int32_t> ours = baro ? o->alt_baro_ft : o->alt_geom_ft;
                    const long ft = std::lround(*m / kFtToM);
                    alt.add(ours.has_value(), ours && *ours == ft, ours ? std::fabs(double(*ours - ft)) : 0);
                    if (ours && *ours != ft && alt.both - alt.agree <= 5)
                        std::cout << "  altitude differs " << hexOf(*o) << " t=" << r.t << " ours " << *ours
                                  << " ft, opensky " << ft << " ft (" << (baro ? "baro" : "gnss") << ")\n";
                }
            }
            if (!o) continue;
            // State carried by the observation, against OpenSky's latest rows. Velocity counts
            // only while the row is no older than the decoder keeps a field (60 s).
            if (vel_t && r.t - *vel_t <= 60.0) {
                if (vel_kt) {
                    const bool have = o->gs_kt.has_value();
                    const double dev = have ? std::fabs(*o->gs_kt - *vel_kt) : 0;
                    gs.add(have, dev <= 1.0, dev);
                }
                if (vel_trk) {
                    const bool have = o->track_deg.has_value();
                    double dev = have ? std::fabs(*o->track_deg - *vel_trk) : 0;
                    dev = std::min(dev, 360.0 - dev);
                    trk.add(have, dev <= 0.5, dev);
                }
            }
            if (!ident.empty()) {
                const bool have = o->callsign.has_value();
                cs.add(have, have && *o->callsign == ident);
                if (have && *o->callsign != ident && cs.both - cs.agree <= 5)
                    std::cout << "  callsign ours '" << *o->callsign << "' opensky '" << ident << "'\n";
            }
        }

        const std::string name = dir.filename().string();
        std::cout << "opensky " << name << ": frames " << frames << " (unparsable " << bad << "), decoded "
                  << dec.counters().decoded << ", crc_fail " << dec.counters().crc_fail << "\n";
        const auto show = [&](const char* what, const Tally& t, const char* unit) {
            std::cout << "  " << what << ": opensky has " << t.rows << ", both " << t.both << ", agree " << t.agree
                      << ", ours missing " << (t.rows - t.both) << ", max deviation " << t.maxdev << " " << unit << "\n";
        };
        show("position", pos, "deg");
        show("position (surface)", surf, "deg");
        show("altitude", alt, "ft");
        show("ground speed", gs, "kt");
        show("track", trk, "deg");
        show("callsign", cs, "");

        // Where both have a value, they must agree.
        const auto check = [&](const char* what, const Tally& t) {
            if (t.both < 20) return;
            BOOST_TEST_CONTEXT(name << " " << what)
            BOOST_TEST(t.agree >= 0.95 * t.both, "agreement " << t.agree << " of " << t.both);
        };
        check("position", pos);
        check("position (surface)", surf);
        check("altitude", alt);
        check("ground speed", gs);
        check("track", trk);
        check("callsign", cs);
        // And a decoder that reports nothing must not pass: it has to produce the fields.
        const auto covers = [&](const char* what, const Tally& t, double frac) {
            if (t.rows < 20) return;
            BOOST_TEST_CONTEXT(name << " " << what)
            BOOST_TEST(t.both >= frac * t.rows, "ours reported " << t.both << " of " << t.rows);
        };
        covers("position", pos, 0.3);  // global CPR needs a pair within 10 s; OpenSky decodes more
        covers("altitude", alt, 0.8);
        covers("ground speed", gs, 0.8);
        covers("track", trk, 0.8);
        covers("callsign", cs, 0.8);
    }
    BOOST_REQUIRE(files_checked > 0);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "opensky: " << files_checked << " files, wall " << wall << " s\n";
    BOOST_TEST(wall < std::chrono::duration<double>(slow(std::chrono::seconds(60))).count());
}
