// ACAS resolution advisories and the rollcall replies against OpenSky's own decoding of the
// same frames (tests/fixtures/opensky, fetched by scripts/fixtures/fetch-test-captures.sh).
//
//   *_acas_data4.csv             DF0 / DF16 (rawmsg 14 / 28 hex digits); for a long frame
//                                 activeresolutionadvisories (ARA, 14-bit integer),
//                                 resolutionadvisorycomplement (RAC, 4 bits), hasterminated (RAT),
//                                 hasmultiplethreats (MTE), nopassbelow / nopassabove /
//                                 noturnleft / noturnright (the RAC bits), sensitivitylevel (SL),
//                                 replyinformation (RI), altitude (metres)
//   *_rollcall_replies_data4.csv DF4 / 5 / 20 / 21: altitude (metres), identity (Mode A code)
//
// Compared where both sides have a value: ACAS fields exactly (>= 99.5 % per field over the
// long DF16 frames), altitude within 1 ft of metres * 3.28084, the four RAC flags against the
// RAC bits; rollcall altitude and identity >= 99 %. Per-field counts and the first mismatches
// (frame, expected, got) are printed. OpenSky reads the MV field of every long DF16 as an RA,
// but few frames carry the RA marker 0x30 in its first byte: those are compared as received
// (no mismatch allowed), and for the field layout all the others are compared with byte 4
// forced to 0x30 in a copy of the frame.
#define BOOST_TEST_MODULE decode_acas_opensky
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coding/bits.hpp"
#include "decode/acas.hpp"
#include "decode/frame.hpp"
#include "opensky_csv.hpp"

using namespace skycued;
using namespace skycued::decode;

namespace {

namespace fs = std::filesystem;
const std::string kFixtures = SKYCUE_FIXTURE_DIR;

constexpr double kMToFt = 3.28084;
constexpr double kAcasMin = 0.995;
constexpr double kRollcallMin = 0.99;

using Row = std::map<std::string, std::string>;

bool endsWith(const std::string& s, const std::string& suf)
{
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

const std::string& col(const Row& r, const char* k)
{
    static const std::string none;
    const auto it = r.find(k);
    return it == r.end() ? none : it->second;
}

std::optional<long> integer(const Row& r, const char* k)
{
    const std::string& v = col(r, k);
    if (v.empty()) return std::nullopt;
    try {
        return std::stol(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> real(const Row& r, const char* k)
{
    const std::string& v = col(r, k);
    if (v.empty()) return std::nullopt;
    try {
        return std::stod(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> flag(const Row& r, const char* k)
{
    const std::string& v = col(r, k);
    if (v == "True") return true;
    if (v == "False") return false;
    return std::nullopt;
}

std::optional<Frame> frameOf(const std::string& hex)
{
    std::vector<uint8_t> b;
    if (!coding::parseHex(hex, b)) return std::nullopt;
    return classify(b);
}

struct Field {
    int n = 0, agree = 0;
    std::vector<std::string> misses;  // first 10
    void add(bool ok, const std::string& hex, const std::string& want, const std::string& got)
    {
        ++n;
        if (ok) ++agree;
        else if (misses.size() < 10) misses.push_back(hex + " expected " + want + " got " + got);
    }
    double rate() const { return n ? static_cast<double>(agree) / n : 1.0; }
};

struct Table {
    std::map<std::string, Field> f;
    std::vector<std::string> order;
    std::map<std::string, int> count;  // notes: rows skipped, frames forced, ...
    Field& at(const std::string& k)
    {
        if (!f.count(k)) order.push_back(k);
        return f[k];
    }
};

std::string num(long v) { return std::to_string(v); }
std::string yn(bool v) { return v ? "True" : "False"; }

std::vector<fs::path> dirsOrSkip()
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
    return dirs;
}

// The path of the table ending in `suffix` in `dir`; empty when absent.
std::string tableIn(const fs::path& dir, const char* suffix)
{
    for (const auto& e : fs::directory_iterator(dir))
        if (endsWith(e.path().filename().string(), suffix)) return e.path().string();
    return {};
}

void report(const char* title, const std::string& where, Table& t, double minRate)
{
    std::cout << title << " [" << where << "]\n";
    for (const auto& k : t.order) {
        const Field& f = t.f[k];
        std::printf("  %-22s %6d / %-6d  %7.3f %%\n", k.c_str(), f.agree, f.n, 100.0 * f.rate());
    }
    for (const auto& k : t.order)
        for (const auto& m : t.f[k].misses) std::cout << "  mismatch " << k << ": " << m << "\n";
    for (const auto& k : t.order) {
        const Field& f = t.f[k];
        BOOST_CHECK_MESSAGE(f.n > 0, where << " " << k << ": no row compared");
        BOOST_CHECK_MESSAGE(f.rate() >= minRate,
                            where << " " << k << ": " << f.agree << " of " << f.n << " agree");
    }
}

void merge(Table& all, Table& t)
{
    for (const auto& k : t.order) {
        Field& a = all.at(k);
        a.n += t.f[k].n;
        a.agree += t.f[k].agree;
        for (const auto& m : t.f[k].misses)
            if (a.misses.size() < 10) a.misses.push_back(m);
    }
    for (const auto& c : t.count) all.count[c.first] += c.second;
}

// Reads the `suffix` table of every tarball directory, hands its rows to
// perDir(rows, per-directory Table), reports each directory and returns the sum.
template <class F>
Table overTarballs(const char* title, const char* suffix, double minRate, F perDir)
{
    Table all;
    for (const auto& dir : dirsOrSkip()) {
        const std::string name = dir.filename().string();
        const std::string path = tableIn(dir, suffix);
        if (path.empty()) {
            BOOST_ERROR(name << ": no " << suffix << " table (re-run fetch-test-captures.sh)");
            continue;
        }
        const auto rows = opensky::readRows(path);
        BOOST_CHECK_MESSAGE(!rows.empty(), path << ": no rows");
        Table t;
        perDir(rows, t);
        std::cout << name << ": " << rows.size() << " rows\n";
        for (const auto& c : t.count) std::cout << "  " << c.first << ": " << c.second << "\n";
        report(title, name, t, minRate);
        merge(all, t);
    }
    report(title, "all five", all, minRate);
    return all;
}

// Altitude of a DF0/4/16/20 row against OpenSky's metres; a row whose AC field has no altitude is counted.
void compareAltitude(const Frame& f, const Row& r, const std::string& hex, Table& t)
{
    const auto m = real(r, "altitude");
    if (!m) return;
    const auto ft = altitudeAc13(f.bits(19, 13));
    if (!ft) {
        ++t.count["altitude rows skipped (AC13 gave no altitude)"];
        return;
    }
    const double want = *m * kMToFt;
    char w[32], g[32];
    std::snprintf(w, sizeof w, "%.1f ft", want);
    std::snprintf(g, sizeof g, "%d ft", static_cast<int>(*ft));
    t.at("altitude").add(std::fabs(want - *ft) <= 1.0, hex, w, g);
}

// The RA fields of a long DF16 row (decoded from `g`) against OpenSky's columns.
void compareRa(const Frame& g, const Row& r, const std::string& hex, Table& t)
{
    const auto ra = decodeAcasRaMv(g);
    if (!ra) return;
    const auto c = decodeAcasCommon(g);
    if (const auto v = integer(r, "activeresolutionadvisories"))
        t.at("ARA").add(*v == ra->ara, hex, num(*v), num(ra->ara));
    if (const auto v = integer(r, "resolutionadvisorycomplement"))
        t.at("RAC").add(*v == ra->rac, hex, num(*v), num(ra->rac));
    if (const auto v = flag(r, "hasterminated")) t.at("RAT").add(*v == ra->rat, hex, yn(*v), yn(ra->rat));
    if (const auto v = flag(r, "hasmultiplethreats")) t.at("MTE").add(*v == ra->mte, hex, yn(*v), yn(ra->mte));
    if (const auto v = integer(r, "sensitivitylevel")) t.at("SL").add(*v == c.sl, hex, num(*v), num(c.sl));
    if (const auto v = integer(r, "replyinformation")) t.at("RI").add(*v == c.ri, hex, num(*v), num(c.ri));
    // The four flags are the RAC bits, first bit on the wire most significant.
    static const struct { const char* col; const char* name; int bit; } kFlags[] = {
        {"nopassbelow", "RAC.nopassbelow", 3},
        {"nopassabove", "RAC.nopassabove", 2},
        {"noturnleft", "RAC.noturnleft", 1},
        {"noturnright", "RAC.noturnright", 0},
    };
    for (const auto& fl : kFlags)
        if (const auto v = flag(r, fl.col)) {
            const bool ours = ((ra->rac >> fl.bit) & 1) != 0;
            t.at(fl.name).add(*v == ours, hex, yn(*v), yn(ours));
        }
}

}  // namespace

BOOST_AUTO_TEST_CASE(acas_rows_match_opensky)
{
    Table native;  // rows whose received MV marker is 0x30: decoded as received, no override
    const Table all = overTarballs("ACAS (layout, byte 4 forced to 0x30)", "_acas_data4.csv", kAcasMin,
                                   [&](const std::vector<Row>& rows, Table& t) {
        for (const auto& r : rows) {
            const std::string& hex = col(r, "rawmsg");
            const auto f = frameOf(hex);
            if (!f) continue;
            if (f->df == 0 || f->df == 16) compareAltitude(*f, r, hex, t);
            if (f->df != 16 || flag(r, "islongformat") != true) continue;
            ++t.count["long DF16 frames"];
            if (decodeAcasRaMv(*f)) {
                ++t.count["native 0x30 frames compared unmodified"];
                compareRa(*f, r, hex, native);
            } else {
                // Without the override a frame whose marker is not 0x30 is not an RA.
                ++t.count["non-0x30 frames rejected by the decoder"];
            }
            Frame g = *f;
            g.bytes[4] = 0x30;
            ++t.count["frames compared with byte 4 forced to 0x30 in a copy"];
            compareRa(g, r, hex, t);
        }
    });
    std::cout << "SUMMARY: " << all.count.at("frames compared with byte 4 forced to 0x30 in a copy")
              << " long DF16 frames were compared with byte 4 forced to 0x30 in a copy (layout check); "
              << all.count.at("native 0x30 frames compared unmodified")
              << " native 0x30 frames were compared unmodified; "
              << all.count.at("non-0x30 frames rejected by the decoder") << " non-0x30 frames gave no RA\n";
    BOOST_CHECK_GE(all.count.at("native 0x30 frames compared unmodified"), 1);
    BOOST_CHECK_GE(all.count.at("non-0x30 frames rejected by the decoder"), 1);
    // Unmodified rows: any mismatch fails.
    report("ACAS (native 0x30, as received)", "all five", native, 1.0);
    BOOST_CHECK(native.f.count("ARA") && native.f["ARA"].n >= 1);
}

BOOST_AUTO_TEST_CASE(rollcall_rows_match_opensky)
{
    overTarballs("rollcall", "_rollcall_replies_data4.csv", kRollcallMin,
                                   [&](const std::vector<Row>& rows, Table& t) {
        for (const auto& r : rows) {
            const std::string& hex = col(r, "rawmsg");
            const auto f = frameOf(hex);
            if (!f) continue;
            if (f->df == 4 || f->df == 20) {
                compareAltitude(*f, r, hex, t);
            } else if (f->df == 5 || f->df == 21) {
                const std::string& id = col(r, "identity");
                if (id.empty()) continue;
                const auto sq = squawkId13(f->bits(19, 13));
                t.at("identity").add(sq && *sq == id, hex, id, sq ? *sq : "none");
            }
        }
    });
}
