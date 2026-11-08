// The UAT frame decoders against dump978-fa on the sample's 439 downlinks
// (tests/fixtures/gen_uat_golden.sh). The golden holds, per downlink, the hex
// frame and what dump978-fa printed for it; every field both sides can have
// must agree, and a field only we produce is a failure. A decoder with a
// deliberately wrong altitude or latitude scale must fail the same comparison.
#define BOOST_TEST_MODULE decode_uat_golden
#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "decode/uat_frame.hpp"
#include "gz_line_reader.hpp"
#include "json_lite.hpp"

using skycued::JsonObject;
using namespace skycued::decode::uat;

namespace {

const std::string kGolden = std::string(SKYCUE_FIXTURE_DIR) + "/raw/uat-sample.golden.jsonl.gz";

bool exists(const std::string& p) { return std::ifstream(p).good(); }

// A decoder variant: the faithful one, or one with a deliberate fault.
struct Variant {
    int alt_delta_ft = 0;
    double lat_scale = 1.0;
};

struct Tally {
    int both = 0, agree = 0, only_theirs = 0, only_ours = 0;
};

struct Report {
    std::map<std::string, Tally> fields;
    int entries = 0, failures = 0;
    std::vector<std::string> messages;
};

std::optional<AddressQualifier> qualifierOf(const std::string& name)
{
    if (name == "adsb_icao") return AddressQualifier::adsb_icao;
    if (name == "tisb_icao") return AddressQualifier::tisb_adsr_icao;
    if (name == "tisb_trackfile") return AddressQualifier::tisb_trackfile;
    return std::nullopt;
}

// Compares one field; theirs / ours are optional so absence on either side is a result of its own.
template <class T>
void cmp(Report& r, const std::string& hex, const char* what, const std::optional<T>& ours, const std::optional<T>& theirs,
         double tol = 0.0)
{
    Tally& t = r.fields[what];
    if (!ours && !theirs) return;
    bool bad = false;
    if (ours && !theirs) {
        ++t.only_ours;
        bad = true;
    } else if (!ours) {
        ++t.only_theirs;
        bad = true;
    } else {
        ++t.both;
        if constexpr (std::is_arithmetic_v<T>) {
            if (std::fabs(static_cast<double>(*ours) - static_cast<double>(*theirs)) <= tol + 1e-9) ++t.agree;
            else bad = true;
        } else {
            if (*ours == *theirs) ++t.agree;
            else bad = true;
        }
    }
    if (bad) {
        ++r.failures;
        r.messages.push_back(hex + " " + what);
    }
}

std::optional<std::string> optStr(const JsonObject& o, const char* k) { return o.getString(k); }
std::optional<double> optNum(const JsonObject& o, const char* k) { return o.getDouble(k); }

// Decodes every golden entry and compares. A wrong variant is expected to accumulate failures.
Report compare(const Variant& v)
{
    Report r;
    skycued::GzLineReader in(kGolden);
    std::string line;
    while (in.getline(line)) {
        const auto g = JsonObject::parse(line);
        BOOST_REQUIRE(g);
        const std::string hex = g->getString("hex").value_or("");
        const auto f = parseDownlinkLine(hex);
        BOOST_REQUIRE_MESSAGE(f, "golden hex does not parse: " << hex);
        ++r.entries;
        const StateVector sv = decodeStateVector(*f);
        const auto ms = decodeModeStatus(*f);
        const auto aux = decodeAuxSv(*f);

        const auto q = qualifierOf(g->getString("qualifier").value_or(""));
        BOOST_REQUIRE_MESSAGE(q, "golden qualifier not mapped: " << g->getString("qualifier").value_or("?"));
        cmp<int>(r, hex, "qualifier", static_cast<int>(f->qualifier()), static_cast<int>(*q));
        const std::string agName = g->getString("ag").value_or("");
        BOOST_REQUIRE_MESSAGE(agName == "airborne" || agName == "ground", "golden ag not mapped: " << agName);
        cmp<int>(r, hex, "ground", sv.ag == StateVector::Ag::ground ? 1 : 0, agName == "ground" ? 1 : 0);
        cmp<std::string>(r, hex, "address", [&] {
            char b[8];
            std::snprintf(b, sizeof b, "%06x", static_cast<unsigned>(f->address()));
            return std::optional<std::string>(b);
        }(), optStr(*g, "address"));

        std::optional<double> lat, lon;
        if (sv.lat) lat = *sv.lat * v.lat_scale;
        if (sv.lon) lon = *sv.lon;
        cmp(r, hex, "lat", lat, optNum(*g, "lat"), 1e-5);
        cmp(r, hex, "lon", lon, optNum(*g, "lon"), 1e-5);

        std::optional<double> altBaro, altGeom;
        if (sv.altitude_ft) {
            const double a = *sv.altitude_ft + v.alt_delta_ft;
            (sv.alt_type == AltType::geometric ? altGeom : altBaro) = a;
        }
        if (aux) altGeom = *aux + v.alt_delta_ft;  // a long frame's secondary altitude is the other kind
        cmp(r, hex, "alt_baro", altBaro, optNum(*g, "alt_baro"));
        cmp(r, hex, "alt_geom", altGeom, optNum(*g, "alt_geom"));

        std::optional<double> gs, trk, vr;
        if (sv.gs_kt) gs = *sv.gs_kt;
        if (sv.track_deg) trk = *sv.track_deg;
        if (sv.vrate_fpm) vr = *sv.vrate_fpm;
        cmp(r, hex, "gs", gs, optNum(*g, "gs"), 1.0);
        if (trk && g->getDouble("track")) {  // wraparound: 359.95 and 0.0
            double d = std::fabs(*trk - *g->getDouble("track"));
            d = std::min(d, 360.0 - d);
            cmp<double>(r, hex, "track", d, 0.0, 0.5);
        } else {
            cmp(r, hex, "track", trk, optNum(*g, "track"), 0.5);
        }
        cmp(r, hex, "vrate", vr, optNum(*g, "vrate"));
        cmp<double>(r, hex, "nic", static_cast<double>(sv.nic), optNum(*g, "nic"));

        std::optional<std::string> cs, sq, cat, em;
        std::optional<double> nacp;
        if (ms) {
            cs = ms->callsign;
            sq = ms->squawk;
            if (!ms->emitter_category.empty()) cat = ms->emitter_category;
            em = ms->emergency;
            if (ms->nac_p) nacp = *ms->nac_p;
        }
        cmp(r, hex, "callsign", cs, optStr(*g, "callsign"));
        cmp(r, hex, "squawk", sq, optStr(*g, "squawk"));
        cmp(r, hex, "category", cat, optStr(*g, "category"));
        cmp(r, hex, "emergency", em, optStr(*g, "emergency"));
        cmp(r, hex, "nac_p", nacp, optNum(*g, "nac_p"));
    }
    return r;
}

void print(const Report& r)
{
    std::cout << "uat golden: " << r.entries << " downlinks, " << r.failures << " field disagreements\n";
    for (const auto& [name, t] : r.fields)
        std::cout << "uat golden field " << name << ": both " << t.both << ", agree " << t.agree << ", only dump978-fa "
                  << t.only_theirs << ", only ours " << t.only_ours << "\n";
    int shown = 0;
    for (const auto& m : r.messages)
        if (++shown <= 20) std::cout << "  " << m << "\n";
}

}  // namespace

BOOST_AUTO_TEST_CASE(golden_matches_dump978)
{
    if (!exists(kGolden)) {
        std::cout << "SKIP: raw/uat-sample.golden.jsonl.gz is missing (gen_uat_golden.sh makes it)\n";
        return;
    }
    const Report r = compare(Variant{});
    print(r);
    BOOST_TEST(r.entries >= 400);
    BOOST_TEST(r.failures == 0);
    for (const char* name : {"lat", "lon", "alt_baro", "alt_geom", "gs", "track", "vrate", "nic", "callsign", "squawk",
                             "category", "emergency", "nac_p", "qualifier", "address", "ground"}) {
        BOOST_TEST_CONTEXT(name) {
            BOOST_TEST(r.fields.at(name).both > 0, "no entry with a value from both sides: the comparison is vacuous");
        }
    }
}

BOOST_AUTO_TEST_CASE(golden_catches_wrong_decoder)
{
    if (!exists(kGolden)) {
        std::cout << "SKIP: raw/uat-sample.golden.jsonl.gz is missing\n";
        return;
    }
    BOOST_TEST(compare(Variant{25, 1.0}).failures > 0);   // altitude formula off by 25 ft
    BOOST_TEST(compare(Variant{0, 1.001}).failures > 0);  // latitude scaling off
}
