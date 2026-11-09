// The synthetic UAT matrix (tests/uat_synth.cpp, about 110 frames built with encodeUat from stated
// values, one per matrix row) three ways: the values built into each frame (the expect sidecar),
// what dump978-fa made of the frame (the golden; tests/fixtures/gen_uat_synthetic.sh), and our
// decoder. Real captures never exercise emitter categories 24..39, emergency codes other than
// none, ground track types 2/3, frames without a position and most squawk/AUX variants; these do.
//
// Rules, per row and field: our decoder equals the built-in value; dump978-fa equals it too
// wherever it reports the field (a field built in but not printed fails, unless listed); the few
// rows where dump978-fa reads a value differently are listed in kDocumented, with what it prints,
// and are pinned (a changed reading by the tool fails the test).
#define BOOST_TEST_MODULE decode_uat_synthetic
#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fixture_io.hpp"
#include "test_util.hpp"
#include "decode/uat_frame.hpp"
#include "gz_line_reader.hpp"
#include "json_lite.hpp"

using skycue_test::fileReadable;
using skycue_test::forEachLine;

using skycued::JsonObject;
using namespace skycued::decode::uat;

namespace {

const std::string kGolden = std::string(SKYCUE_FIXTURE_DIR) + "/raw/uat-synthetic.golden.jsonl.gz";
const std::string kExpect = std::string(SKYCUE_FIXTURE_DIR) + "/raw/uat-synthetic.expect.jsonl.gz";


struct RequireGoldens {
    RequireGoldens()
    {
        if (!fileReadable(kGolden) || !fileReadable(kExpect)) {
            std::cout << "SKIP: raw/uat-synthetic.golden.jsonl.gz or raw/uat-synthetic.expect.jsonl.gz is missing "
                         "(gen_uat_synthetic.sh makes them)\n";
            std::exit(77);
        }
    }
};
BOOST_GLOBAL_FIXTURE(RequireGoldens);

// Where dump978-fa reads a value differently from the spec text the layout follows (the built-in
// value, which ours keeps). "theirs" is exactly what it prints for the row.
struct Documented {
    const char* row;
    const char* field;
    const char* theirs;
    const char* note;
};
const Documented kDocumented[] = {
    {"emergency3", "emergency", "invalid", "code 3 is minimum fuel in the spec text; dump978-fa prints \"invalid\""},
    {"emergency7", "emergency", "reserved", "code 7 is reserved: dump978-fa prints \"reserved\", ours has no value"},
};
// dump978-fa's names for emergency codes 4..6 against ours (a spelling, not a reading).
const std::map<std::string, std::string> kEmergencyNames = {
    {"nordo", "no_comms"}, {"unlawful", "unlawful_interference"}, {"downed", "downed_aircraft"}};
const std::map<std::string, int> kQualifierCodes = {
    {"adsb_icao", 0}, {"adsb_other", 1}, {"tisb_icao", 2}, {"tisb_trackfile", 3},
    {"vehicle", 4},   {"fixed_beacon", 5}, {"adsr_other", 6}, {"reserved", 7}};

struct Tally {
    int agree = 0, unreported = 0, documented = 0, failures = 0;
};

struct Report {
    std::map<std::string, Tally> groups;  // by matrix group
    std::map<std::string, int> compared;  // by field: built in and reported by dump978-fa
    int rows = 0, failures = 0;
    std::vector<std::string> messages;
};

struct Row {
    JsonObject expect;
    std::optional<JsonObject> golden;
};

std::vector<Row> loadRows()
{
    std::map<std::string, JsonObject> golden;
    {
        forEachLine(kGolden, [&](const std::string& line) {
            auto g = JsonObject::parse(line);
            BOOST_REQUIRE(g);
            const std::string hex = g->getString("hex").value_or("");
            golden.emplace(hex, std::move(*g));
        });
    }
    std::vector<Row> rows;
    forEachLine(kExpect, [&](const std::string& line) {
        auto e = JsonObject::parse(line);
        BOOST_REQUIRE(e);
        Row r{std::move(*e), std::nullopt};
        const auto it = golden.find(r.expect.getString("hex").value_or(""));
        if (it != golden.end()) r.golden = it->second;
        rows.push_back(std::move(r));
    });
    return rows;
}

// Our decoder's values for a frame, under the golden's key names.
struct Ours {
    std::map<std::string, double> num;
    std::map<std::string, std::string> str;
};

Ours decodeOurs(const UatFrame& f)
{
    Ours o;
    char ab[8];
    std::snprintf(ab, sizeof ab, "%06x", static_cast<unsigned>(f.address()));
    o.str["address"] = ab;
    const StateVector sv = decodeStateVector(f);
    o.str["ag"] = sv.ag == StateVector::Ag::ground ? "ground" : "airborne";
    if (sv.lat) o.num["lat"] = *sv.lat;
    if (sv.lon) o.num["lon"] = *sv.lon;
    if (sv.altitude_ft) o.num[sv.alt_type == AltType::geometric ? "alt_geom" : "alt_baro"] = *sv.altitude_ft;
    if (const auto aux = decodeAuxSv(f)) o.num[sv.alt_type == AltType::geometric ? "alt_baro" : "alt_geom"] = *aux;
    o.num["nic"] = sv.nic;
    if (sv.gs_kt) o.num["gs"] = *sv.gs_kt;
    if (sv.track_deg) o.num["track"] = *sv.track_deg;
    if (sv.vrate_fpm) o.num["vrate"] = *sv.vrate_fpm;
    if (sv.vrate_fpm && sv.vrate_geometric) o.str["vrate_src"] = *sv.vrate_geometric ? "geometric" : "barometric";
    if (const auto ms = decodeModeStatus(f)) {
        if (!ms->emitter_category.empty()) o.str["category"] = ms->emitter_category;
        if (ms->emergency) o.str["emergency"] = *ms->emergency;
        if (ms->nac_p) o.num["nac_p"] = *ms->nac_p;
        if (ms->callsign) o.str["callsign"] = *ms->callsign;
        if (ms->squawk) o.str["squawk"] = *ms->squawk;
    }
    return o;
}

struct FieldSpec {
    const char* key;
    bool numeric;
    double tol_theirs;  // built-in value against dump978-fa's (it rounds; positions have an LSB)
    double tol_ours;    // built-in value against ours
};
const FieldSpec kFields[] = {
    {"address", false, 0, 0},   {"lat", true, 3e-5, 3e-5},  {"lon", true, 3e-5, 3e-5},
    {"alt_baro", true, 0, 0},   {"alt_geom", true, 0, 0},   {"gs", true, 1.0, 1e-3},
    {"track", true, 0.6, 0.6},  {"vrate", true, 0, 0},      {"vrate_src", false, 0, 0},
    {"callsign", false, 0, 0},  {"squawk", false, 0, 0},    {"category", false, 0, 0},
    {"emergency", false, 0, 0}, {"nic", true, 0, 0},        {"nac_p", true, 0, 0},
    {"ag", false, 0, 0}};

const Documented* documented(const std::string& row, const char* field)
{
    for (const Documented& d : kDocumented)
        if (row == d.row && std::string(field) == d.field) return &d;
    return nullptr;
}

Report compare(const std::vector<Row>& rows)
{
    Report r;
    for (const Row& row : rows) {
        ++r.rows;
        const std::string name = row.expect.getString("row").value_or("?");
        Tally& tl = r.groups[row.expect.getString("group").value_or("?")];
        auto fail = [&](const std::string& what) {
            ++tl.failures;
            ++r.failures;
            r.messages.push_back(name + " " + what);
        };
        const auto f = parseDownlinkLine(row.expect.getString("hex").value_or(""));
        BOOST_REQUIRE_MESSAGE(f, "row " << name << ": hex does not parse");
        BOOST_REQUIRE_MESSAGE(row.golden, "row " << name << ": no dump978-fa object (a demodulation miss)");
        const JsonObject& g = *row.golden;
        const Ours ours = decodeOurs(*f);

        for (const FieldSpec& fs : kFields) {
            const std::string key = fs.key;
            std::optional<double> ed, td, od;
            std::optional<std::string> es, ts, os;
            if (fs.numeric) {
                ed = row.expect.getDouble(fs.key);
                td = g.getDouble(fs.key);
                if (auto it = ours.num.find(key); it != ours.num.end()) od = it->second;
            } else {
                es = row.expect.getString(fs.key);
                ts = g.getString(fs.key);
                if (auto it = ours.str.find(key); it != ours.str.end()) os = it->second;
                if (key == "ag" && ts && *ts == "supersonic") ts = "airborne";
                if (key == "emergency" && ts) {
                    if (auto it = kEmergencyNames.find(*ts); it != kEmergencyNames.end()) ts = it->second;
                }
                // The source of a vertical rate is only defined where there is a rate.
                if (key == "vrate_src" && !row.expect.has("vrate")) ts.reset();
            }
            const bool built = fs.numeric ? ed.has_value() : es.has_value();

            // Ours against the built-in value, always.
            if (fs.numeric) {
                if (ed.has_value() != od.has_value() || (ed && std::fabs(*ed - *od) > fs.tol_ours + 1e-9)) fail(key + " ours");
            } else if (es != os) {
                fail(key + " ours");
            }

            if (const Documented* d = documented(name, fs.key)) {
                ++tl.documented;
                if (ts != std::optional<std::string>(d->theirs)) fail(key + " dump978-fa changed its reading");
                continue;
            }
            // dump978-fa against the built-in value.
            const bool theirs = fs.numeric ? td.has_value() : ts.has_value();
            if (built && !theirs) {
                ++tl.unreported;
                fail(key + " built in but not reported by dump978-fa (list it in kDocumented)");
            } else if (!built && theirs) {
                fail(key + " reported by dump978-fa but not built in");
            } else if (built) {
                ++r.compared[key];
                const bool ok = fs.numeric ? std::fabs(*ed - *td) <= fs.tol_theirs + 1e-9 : *es == *ts;
                if (ok) ++tl.agree;
                else fail(key + " dump978-fa differs");
            }
        }

        // Qualifier: dump978-fa's name against the built-in code, and ours.
        {
            const auto q = g.getString("qualifier");
            const auto it = q ? kQualifierCodes.find(*q) : kQualifierCodes.end();
            const int built = static_cast<int>(row.expect.getInt("qualifier_code").value_or(-1));
            if (static_cast<int>(f->qualifier()) != built) fail("qualifier ours");
            if (it == kQualifierCodes.end() || it->second != built) fail("qualifier dump978-fa differs");
            else ++tl.agree;
        }
        // Ground track types 2/3: a heading, never a track; dump978-fa prints it under its own key.
        if (const auto tt = row.expect.getInt("track_type"); tt && *tt >= 2) {
            const char* key = *tt == 2 ? "mag_heading" : "true_heading";
            const auto h = g.getDouble(key), e = row.expect.getDouble("heading");
            if (!h || !e || std::fabs(*h - *e) > 0.6 || g.has("track") || ours.num.count("track")) fail("heading");
            else ++tl.agree;
        } else if (g.has("mag_heading") || g.has("true_heading")) {
            fail("heading reported on a row that is not a ground track-type 2/3 row");
        }
    }
    return r;
}

void print(const Report& r)
{
    std::cout << "uat synthetic: " << r.rows << " rows, " << r.failures << " disagreements\n";
    for (const auto& [name, t] : r.groups)
        std::cout << "uat synthetic group " << name << ": agree " << t.agree << ", unreported " << t.unreported
                  << ", documented " << t.documented << ", failures " << t.failures << "\n";
    int shown = 0;
    for (const auto& m : r.messages)
        if (++shown <= 20) std::cout << "  " << m << "\n";
}

}  // namespace

BOOST_AUTO_TEST_CASE(synthetic_rows_all_paired)
{
    const auto rows = loadRows();
    BOOST_TEST(rows.size() == 109u);
    std::set<std::string> hexes;
    std::string missed;
    for (const Row& r : rows) {
        hexes.insert(r.expect.getString("hex").value_or(""));
        if (!r.golden) missed += " " + r.expect.getString("row").value_or("?");
    }
    BOOST_TEST(hexes.size() == rows.size(), "two rows built the same frame");
    BOOST_TEST(missed.empty(), "rows dump978-fa did not demodulate:" << missed);
}

BOOST_AUTO_TEST_CASE(synthetic_golden_matches_builtin_values)
{
    const auto rows = loadRows();
    const Report r = compare(rows);
    print(r);
    BOOST_TEST(r.failures == 0);
    BOOST_TEST(r.rows == 109);
    for (const auto& kv : r.groups) {
        BOOST_TEST_CONTEXT(kv.first) { BOOST_TEST(kv.second.agree > 0); }
    }
    for (const char* name : {"lat", "lon", "alt_baro", "alt_geom", "gs", "track", "vrate", "vrate_src", "callsign", "squawk",
                             "category", "emergency", "nic", "nac_p", "ag", "address"}) {
        BOOST_TEST_CONTEXT(name) {
            const auto it = r.compared.find(name);
            BOOST_TEST((it != r.compared.end() && it->second > 0), "no row with a value on every side: the comparison is vacuous");
        }
    }
}

// An entry of the documented list names a real row.
BOOST_AUTO_TEST_CASE(documented_rows_exist)
{
    const auto rows = loadRows();
    for (const Documented& d : kDocumented) {
        bool found = false;
        for (const Row& r : rows) found = found || r.expect.getString("row").value_or("") == d.row;
        BOOST_TEST(found, d.row << " is not a matrix row");
    }
}
