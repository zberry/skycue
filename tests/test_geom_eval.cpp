// Evaluation of the geometric-altitude estimate on real data (design: geometric
// altitude estimation, part C).
//
// 1. geom_eval_leave_one_out_120s: the 120 s Mode S capture is decoded; every
//    aircraft that reported a GNSS-minus-baro difference is predicted from the
//    AltitudeFit of all the other aircraft (window ending at its last sample)
//    at its own median barometric altitude, and compared with its own median
//    difference. The plain median of the others is printed for contrast.
// 2. geom_eval_own_diff_hold_opensky: for each OpenSky fixture the difference
//    reported `lag` seconds earlier is held and corrected for the barometric
//    altitude change with the default slope of resolveGeomAltitude (0.027).
//
// Both print one line per window / lag: n, median absolute error, p90, max in
// ft and m. Capture and fixtures are fetched by scripts/fixtures/fetch-test-captures.sh;
// the test skips (exit 77) when they are absent.
#define BOOST_TEST_MODULE geom_eval
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "altitude_fit.hpp"
#include "coding/bits.hpp"
#include "decode/modes_decoder.hpp"
#include "gz_line_reader.hpp"
#include "test_slow.hpp"

using skycued::AltitudeFit;
using skycued::AltitudeFitConfig;
using skycued::TrackObservation;
using skycued::decode::DecoderConfig;
using skycued::decode::ModesDecoder;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
const std::string kFixtures = SKYCUE_FIXTURE_DIR;
constexpr double kFtToM = 0.3048;
constexpr double kSlope = 0.027;  // the default of resolveGeomAltitude

struct Sample {
    Clock::time_point t;  // when the difference was measured
    int32_t baro_ft, diff_ft;
};

Clock::time_point at(double epoch)
{
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(epoch)));
}

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(a - b).count(); }

// Collects the (difference time, baro, difference) of each reported difference once: the
// decoder repeats a held difference on every observation, so a sample is kept only when the
// difference's measurement time is new and a baro within 2 s of it is present.
struct Collector {
    std::map<uint32_t, std::vector<Sample>> by_aircraft;
    void operator()(const TrackObservation& o)
    {
        // Lone-difference observations count: the difference carries its own time.
        if (!o.geom_minus_baro_ft || !o.alt_baro_ft || !o.geom_minus_baro_time || !o.alt_baro_time) return;
        if (o.on_ground.value_or(false)) return;
        if (std::fabs(secs(*o.geom_minus_baro_time, *o.alt_baro_time)) > 2.0) return;
        auto& v = by_aircraft[o.id.address];
        if (!v.empty() && v.back().t >= *o.geom_minus_baro_time) return;
        v.push_back({*o.geom_minus_baro_time, *o.alt_baro_ft, *o.geom_minus_baro_ft});
    }
};

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct Stats {
    size_t n = 0;
    double med = 0, p90 = 0, max = 0;
};

Stats absStats(std::vector<double> err)
{
    Stats s;
    for (double& e : err) e = std::fabs(e);
    s.n = err.size();
    if (err.empty()) return s;
    std::sort(err.begin(), err.end());
    s.med = median(err);
    s.p90 = err[std::min(err.size() - 1, static_cast<size_t>(std::ceil(0.9 * static_cast<double>(err.size()))) - 1)];
    s.max = err.back();
    return s;
}

void print(const std::string& label, const Stats& s)
{
    std::cout << std::fixed << std::setprecision(0) << label << ": n=" << s.n << " medAE=" << s.med << " ft ("
              << s.med * kFtToM << " m) p90=" << s.p90 << " ft (" << s.p90 * kFtToM << " m) max=" << s.max << " ft ("
              << s.max * kFtToM << " m)\n";
}

}  // namespace

BOOST_AUTO_TEST_CASE(geom_eval_leave_one_out_120s)
{
    const std::string csv = kFixtures + "/raw/modes-120s.csv.gz";
    if (!std::ifstream(csv).good()) {
        std::cout << "SKIP: raw/modes-120s.csv.gz is missing (scripts/fixtures/fetch-test-captures.sh fetches the capture)\n";
        std::exit(77);  // ctest SKIP_RETURN_CODE
    }
    ModesDecoder dec(DecoderConfig{}, "geom-eval");
    Collector col;
    dec.onObservation([&](const TrackObservation& o) { col(o); });
    {
        skycued::GzLineReader in(csv);
        std::string line;
        std::vector<uint8_t> bytes;
        while (in.getline(line)) {
            const auto comma = line.find(',');
            if (comma == std::string::npos) continue;
            if (!skycued::coding::parseHex(line.substr(comma + 1), bytes)) continue;
            dec.feed(bytes, at(std::stod(line.substr(0, comma))), std::nullopt);
        }
    }
    size_t total = 0;
    for (const auto& kv : col.by_aircraft) total += kv.second.size();
    std::cout << "geom_eval: " << col.by_aircraft.size() << " aircraft with a difference, " << total << " samples\n";
    BOOST_REQUIRE_GT(col.by_aircraft.size(), 5u);

    for (const int win : {30, 60, 120}) {
        std::vector<double> err, err_median;
        for (const auto& target : col.by_aircraft) {
            const auto last = target.second.back().t;
            const auto from = last - std::chrono::seconds(win);
            std::vector<double> baros, diffs;
            for (const Sample& s : target.second)
                if (s.t > from) {
                    baros.push_back(s.baro_ft);
                    diffs.push_back(s.diff_ft);
                }
            if (diffs.empty()) continue;
            AltitudeFitConfig cfg;
            cfg.window = std::chrono::seconds(win);
            AltitudeFit fit(cfg);
            std::vector<double> others;
            for (const auto& other : col.by_aircraft) {
                if (other.first == target.first) continue;
                for (const Sample& s : other.second)
                    if (s.t > from && s.t <= last) {
                        fit.add(other.first, s.t, s.baro_ft, s.diff_ft);
                        others.push_back(s.diff_ft);
                    }
            }
            const auto r = fit.fit(last);
            if (!r) continue;
            err.push_back(r->predict(median(baros)) - median(diffs));
            err_median.push_back(median(others) - median(diffs));
        }
        const Stats s = absStats(err);
        print("leave-one-out window " + std::to_string(win) + " s", s);
        print("  plain median, window " + std::to_string(win) + " s", absStats(err_median));
        BOOST_CHECK_GE(s.n, 5u);
        BOOST_CHECK_LE(s.p90, 250.0);
    }
}

BOOST_AUTO_TEST_CASE(geom_eval_own_diff_hold_opensky)
{
    std::vector<fs::path> dirs;
    if (fs::is_directory(kFixtures + "/opensky"))
        for (const auto& e : fs::directory_iterator(kFixtures + "/opensky"))
            if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    if (dirs.empty()) {
        std::cout << "SKIP: opensky/ is missing (scripts/fixtures/fetch-test-captures.sh fetches and unpacks it)\n";
        std::exit(77);  // ctest SKIP_RETURN_CODE
    }

    struct Frame {
        double t;
        std::string raw;
    };
    std::vector<std::vector<Sample>> files;
    for (const auto& dir : dirs) {
        std::vector<Frame> frames;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string n = e.path().filename().string();
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".csv") != 0) continue;
            std::ifstream in(e.path());
            std::string line;
            if (!std::getline(in, line)) continue;
            // Header: find mintime and rawmsg columns (no quoted commas in these two).
            std::vector<std::string> head;
            for (size_t p = 0, q; p <= line.size(); p = q + 1) {
                q = line.find(',', p);
                if (q == std::string::npos) q = line.size();
                std::string f = line.substr(p, q - p);
                if (!f.empty() && f.back() == '\r') f.pop_back();
                head.push_back(f);
            }
            const auto col = [&](const char* k) {
                const auto it = std::find(head.begin(), head.end(), k);
                return it == head.end() ? -1 : static_cast<int>(it - head.begin());
            };
            const int ct = col("mintime"), cr = col("rawmsg");
            if (ct < 0 || cr < 0) continue;
            while (std::getline(in, line)) {
                // Quote-aware split.
                std::vector<std::string> f;
                std::string cur;
                bool q = false;
                for (char c : line) {
                    if (c == '"') q = !q;
                    else if (c == ',' && !q) {
                        f.push_back(cur);
                        cur.clear();
                    } else if (c != '\r')
                        cur.push_back(c);
                }
                f.push_back(cur);
                if (f.size() != head.size()) continue;
                try {
                    frames.push_back({std::stod(f[static_cast<size_t>(ct)]), f[static_cast<size_t>(cr)]});
                } catch (...) {
                }
            }
        }
        std::stable_sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.t < b.t; });
        ModesDecoder dec(DecoderConfig{}, "geom-eval-opensky");
        Collector col;
        dec.onObservation([&](const TrackObservation& o) { col(o); });
        std::vector<uint8_t> bytes;
        for (const Frame& f : frames)
            if (skycued::coding::parseHex(f.raw, bytes)) dec.feed(bytes, at(f.t), std::nullopt);
        for (auto& kv : col.by_aircraft) files.push_back(std::move(kv.second));
    }

    for (const int lag : {60, 300}) {
        std::vector<double> err;
        size_t samples = 0;
        for (const auto& v : files) {
            samples += lag == 60 ? v.size() : 0;
            for (const Sample& cur : v) {
                // The latest earlier difference that is at least `lag` s old, and not older than lag + 10 s.
                const Sample* held = nullptr;
                for (const Sample& s : v) {
                    if (secs(cur.t, s.t) < lag) break;
                    held = &s;
                }
                if (!held || secs(cur.t, held->t) > lag + 10.0) continue;
                err.push_back(held->diff_ft + kSlope * (cur.baro_ft - held->baro_ft) - cur.diff_ft);
            }
        }
        if (lag == 60) std::cout << "geom_eval: " << files.size() << " fixture aircraft, " << samples << " samples\n";
        const Stats s = absStats(err);
        print("own-difference hold, lag " + std::to_string(lag) + " s", s);
        BOOST_CHECK_GE(s.n, 20u);
        BOOST_CHECK_LE(s.p90, lag == 60 ? 200.0 : 400.0);
    }
}
