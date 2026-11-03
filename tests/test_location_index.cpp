// LocationIndex: positions learned from TWGO shapes, persisted, merged over
// an operator CSV (design §3.2).

#define BOOST_TEST_MODULE location_index
#include <boost/test/unit_test.hpp>

#include "fisb/location_index.hpp"
#include "geo.hpp"
#include "log.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace skycued::fisb;
namespace tt = boost::test_tools;

namespace {

using LatLon = std::pair<double, double>;

TwgoRecord rec(const std::string& location, std::vector<Shape> shapes)
{
    TwgoRecord r;
    r.location = location;
    r.shapes = std::move(shapes);
    r.has_graphics = true;
    return r;
}

Shape shape(Shape::Geom geom, std::vector<LatLon> points)
{
    Shape s;
    s.geom = geom;
    s.points = std::move(points);
    return s;
}

Shape point(double lat, double lon) { return shape(Shape::point, {{lat, lon}}); }
Shape polygon(std::vector<LatLon> points) { return shape(Shape::polygon, std::move(points)); }
Shape polyline(std::vector<LatLon> points) { return shape(Shape::polyline, std::move(points)); }

}  // namespace

BOOST_AUTO_TEST_CASE(airport_style_ids) {
    for (const char* ok : {"KBOS", "K2G4", "CYXU", "PANC", "PHOG"}) BOOST_TEST(airportStyleId(ok), ok);
    for (const char* no : {"", "BOS", "ZAU", "KBOSX", "PPGX", "EGLL", "kbos", "K-OS"}) BOOST_TEST(!airportStyleId(no), no);
}
BOOST_AUTO_TEST_CASE(learns_point_and_reports_new_once) {
    LocationIndex s;
    BOOST_TEST(s.learn(rec("KPRG", {point(39.699097, -87.670212)})));
    BOOST_TEST(!s.learn(rec("KPRG", {point(39.699097, -87.670212)})));
    const auto p = s.find("KPRG");
    BOOST_REQUIRE(p); BOOST_TEST(p->first == 39.699097, tt::tolerance(1e-9)); BOOST_TEST(s.dirty());
}
BOOST_AUTO_TEST_CASE(point_samples_averaged) {
    LocationIndex s;
    s.learn(rec("KGSH", {point(41.50, -85.80)})); s.learn(rec("KGSH", {point(41.60, -85.90)}));
    BOOST_TEST(s.find("KGSH")->first == 41.55, tt::tolerance(1e-9));
    BOOST_TEST(s.find("KGSH")->second == -85.85, tt::tolerance(1e-9));
}
BOOST_AUTO_TEST_CASE(non_airport_location_ignored) {
    LocationIndex s;
    BOOST_TEST(!s.learn(rec("ZAU", {point(41.8, -88.3)}))); BOOST_TEST(!s.learn(rec("", {point(41.8, -88.3)})));
    BOOST_TEST(s.learnedCount() == 0u);
}
BOOST_AUTO_TEST_CASE(polygon_centroid_only_without_points_and_small) {
    LocationIndex s;
    BOOST_TEST(s.learn(rec("KSMA", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}, {40.1, -86.0}})})));
    BOOST_TEST(s.find("KSMA")->first == 40.05, tt::tolerance(1e-9));
    s.learn(rec("KSMA", {point(40.20, -86.10)}));                 // a point replaces the centroid
    BOOST_TEST(s.find("KSMA")->first == 40.20, tt::tolerance(1e-9));
    s.learn(rec("KSMA", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}})}));   // ignored now
    BOOST_TEST(s.find("KSMA")->first == 40.20, tt::tolerance(1e-9));
    BOOST_TEST(!s.learn(rec("KBIG", {polygon({{40.0, -86.0}, {40.0, -85.0}, {41.0, -85.0}, {41.0, -86.0}})})));  // >10 nm
    BOOST_TEST(!s.learn(rec("KLIN", {polyline({{40.0, -86.0}, {40.0, -85.99}})})));
}
BOOST_AUTO_TEST_CASE(closed_polygon_centroid_drops_closing_vertex) {
    // as TWGO sends them (first vertex repeated last): the centroid equals the open polygon's
    LocationIndex s;
    s.learn(rec("KCLO", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}, {40.1, -86.0}, {40.0, -86.0}})}));
    BOOST_TEST(s.find("KCLO")->first == 40.05, tt::tolerance(1e-9));
    BOOST_TEST(s.find("KCLO")->second == -85.95, tt::tolerance(1e-9));
}
BOOST_AUTO_TEST_CASE(persistence_round_trip) {
    LocationIndex a;
    a.learn(rec("KPRG", {point(39.699097, -87.670212)})); a.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    const std::string j = a.learnedJson();
    BOOST_TEST(j.find("\"KPRG\":[39.699097,-87.670212,2]") != std::string::npos);
    LocationIndex b; b.loadLearned(j);
    BOOST_TEST(b.learnedCount() == 1u); BOOST_TEST(!b.dirty()); BOOST_TEST(b.learnedJson() == j);
    b.learn(rec("KPRG", {point(39.699097, -87.670212)}));        // the count continues from 2
    BOOST_TEST(b.learnedJson().find(",3]") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(malformed_learned_file_throws) {
    LocationIndex s;
    for (const char* bad : {"", "{", "[]", "{\"version\":2,\"locations\":{}}", "{\"version\":1,\"locations\":{\"KPRG\":[91,0,1]}}",
                            "{\"version\":1,\"locations\":{\"KPRG\":[\"x\",0,1]}}", "{\"version\":1,\"locations\":{\"KPRG\":[1,2]}}"})
        BOOST_CHECK_THROW(s.loadLearned(bad), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(operator_csv_and_merge_precedence) {
    LocationIndex s;
    const std::size_t n = s.loadOperatorCsv("id,lat,lon\n# comment\n\nkprg,39.0,-87.0\nIND,39.8,-86.3\nBAD LINE\nKXYZ,95,0\n");
    BOOST_TEST(n == 2u); BOOST_TEST(s.operatorRejected() == 2u); BOOST_TEST(s.operatorRejectedFirstLine() == 6u);
    BOOST_TEST(s.find("KPRG")->first == 39.0, tt::tolerance(1e-9));
    s.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    BOOST_TEST(s.find("KPRG")->first == 39.699097, tt::tolerance(1e-9));   // learned wins
    BOOST_TEST(s.merged().size() == 2u); BOOST_TEST(s.merged().at("IND").first == 39.8, tt::tolerance(1e-9));
    BOOST_TEST(s.learnedJson().find("IND") == std::string::npos);           // operator entries are never persisted
}

// Fix round 1 (review of 21978ea).

namespace {

// A polygon of two vertices dl degrees north and south of (40, -86): its
// centroid is (40, -86) and both vertices are `nm` from it.
Shape northSouthPair(double nm)
{
    const double dl = nm * skycued::geo::kMetresPerNm / skycued::geo::kEarthRadiusM * 180.0 / M_PI;
    return polygon({{40.0 + dl, -86.0}, {40.0 - dl, -86.0}});
}

// Distinct [A-Z0-9] ids: `first`, then `width` base-36 digits of i.
std::string idFor(char first, std::size_t i, int width = 3)
{
    const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string id(1, first);
    for (int k = 0; k < width; ++k, i /= 36)
        id += digits[i % 36];
    return id;
}

struct QuietLog {  // the cap tests WARN once; keep the test output clean
    QuietLog() { skycued::setLogLevel(skycued::LogLevel::error); }
    ~QuietLog() { skycued::setLogLevel(skycued::LogLevel::info); }
};

}  // namespace

BOOST_AUTO_TEST_CASE(centroid_radius_boundary) {
    LocationIndex s;
    BOOST_TEST(s.learn(rec("KINS", {northSouthPair(kCentroidMaxRadiusNm * 0.999)})));
    BOOST_TEST(s.find("KINS")->first == 40.0, tt::tolerance(1e-9));
    BOOST_TEST(!s.learn(rec("KOUT", {northSouthPair(kCentroidMaxRadiusNm * 1.001)})));
    BOOST_TEST(!s.find("KOUT").has_value());
}
BOOST_AUTO_TEST_CASE(invalid_samples_ignored) {
    LocationIndex s;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    BOOST_TEST(!s.learn(rec("KNAN", {point(nan, -86.0)})));
    BOOST_TEST(!s.learn(rec("KNAN", {point(40.0, inf)})));
    BOOST_TEST(!s.learn(rec("KNAN", {point(91.0, -86.0)})));
    BOOST_TEST(!s.learn(rec("KNAN", {point(40.0, -181.0)})));
    BOOST_TEST(!s.learn(rec("KNAN", {polygon({{40.0, -86.0}, {nan, -85.9}, {40.1, -85.9}})})));
    BOOST_TEST(!s.learn(rec("KNAN", {})));
    BOOST_TEST(s.learnedCount() == 0u); BOOST_TEST(!s.dirty());
    // a valid sample in the same record still counts
    BOOST_TEST(s.learn(rec("KNAN", {point(nan, 0.0), point(40.0, -86.0)})));
    BOOST_TEST(s.find("KNAN")->first == 40.0, tt::tolerance(1e-9));
}
BOOST_AUTO_TEST_CASE(sample_weight_capped) {
    LocationIndex s;
    for (uint32_t i = 0; i < kMaxSamples + 5; ++i) s.learn(rec("KCAP", {point(40.0, -86.0)}));
    BOOST_TEST(s.learnedJson().find("\"KCAP\":[40.000000,-86.000000,1000]") != std::string::npos);
    s.learn(rec("KCAP", {point(41.0, -86.0)}));                 // weight 1/1000, not 1/1006
    BOOST_TEST(s.find("KCAP")->first == 40.001, tt::tolerance(1e-9));
    BOOST_TEST(s.learnedJson().find(",1000]") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(dirty_only_when_rendering_changes) {
    LocationIndex s;
    BOOST_TEST(!s.dirty());
    s.learn(rec("KSMA", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}, {40.1, -86.0}})}));
    BOOST_TEST(s.dirty()); s.markSaved(); BOOST_TEST(!s.dirty());
    s.learn(rec("KSMA", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}, {40.1, -86.0}})}));
    BOOST_TEST(!s.dirty());                                       // identical centroid
    s.learn(rec("KPRG", {point(39.699097, -87.670212)})); s.markSaved();
    s.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    BOOST_TEST(s.dirty());                                        // the count moved 1 -> 2
    for (uint32_t i = 0; i < kMaxSamples; ++i) s.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    s.markSaved();
    const std::string saved = s.learnedJson();
    for (int i = 0; i < 10; ++i) s.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    BOOST_TEST(!s.dirty());                                       // at the cap, identical points: same file
    BOOST_TEST(s.learnedJson() == saved);
    s.learn(rec("KPRG", {point(39.709097, -87.670212)}));        // moves the mean by 1e-5
    BOOST_TEST(s.dirty());
}
BOOST_AUTO_TEST_CASE(antimeridian_samples_wrap) {
    LocationIndex s;
    s.learn(rec("PADK", {point(51.0, 179.99)})); s.learn(rec("PADK", {point(51.0, -179.99)}));
    BOOST_TEST(std::fabs(s.find("PADK")->second) == 180.0, tt::tolerance(1e-9));
    s.learn(rec("PASY", {point(52.0, -179.98)})); s.learn(rec("PASY", {point(52.0, 179.96)}));
    BOOST_TEST(s.find("PASY")->second == 179.99, tt::tolerance(1e-9));
    // a small polygon across the antimeridian has its centroid there
    BOOST_TEST(s.learn(rec("PAMC", {polygon({{52.0, 179.95}, {52.0, -179.95}, {52.05, -179.95}, {52.05, 179.95}})})));
    BOOST_TEST(std::fabs(s.find("PAMC")->second) == 180.0, tt::tolerance(1e-9));
    for (const auto& [id, p] : s.merged()) BOOST_TEST((p.second >= -180.0 && p.second <= 180.0), id);
    LocationIndex b; BOOST_CHECK_NO_THROW(b.loadLearned(s.learnedJson()));
}
BOOST_AUTO_TEST_CASE(failed_load_leaves_index_unchanged) {
    LocationIndex s;
    s.learn(rec("KPRG", {point(39.699097, -87.670212)}));
    const std::string before = s.learnedJson();
    BOOST_CHECK_THROW(s.loadLearned("{\"version\":1,\"locations\":{\"KGSH\":[41.5,-85.8,1],\"KBAD\":[91,0,1]}}"),
                      std::runtime_error);
    BOOST_TEST(s.learnedJson() == before); BOOST_TEST(s.dirty()); BOOST_TEST(!s.find("KGSH").has_value());
}
BOOST_AUTO_TEST_CASE(more_malformed_learned_files_throw) {
    LocationIndex s;
    const std::string deep = std::string(50, '[') + std::string(50, ']');
    for (const std::string& bad : std::vector<std::string>{"{\"version\":1,\"locations\":{\"kprg\":[1,2,1]}}", "{\"version\":1,\"locations\":{\"ZAU\":[1,2,1]}}",
                                  "{\"version\":1,\"locations\":{\"KPRG\":[1,2,1001]}}", "{\"version\":1,\"locations\":{\"KPRG\":[1,2,-1]}}",
                                  "{\"version\":1,\"locations\":{\"KPRG\":[1,2,1.5]}}", "{\"version\":1,\"locations\":{\"KPRG\":[1,2,1,4]}}",
                                  "{\"version\":1,\"locations\":{\"KPRG\":[1,181,1]}}", "{\"version\":1,\"locations\":{\"KPRG\":[1,[2],1]}}",
                                  "{\"version\":1,\"locations\":[[1,2,1]]}", "{\"version\":1,\"locations\":5}", "{\"version\":1}",
                                  "{\"locations\":{}}", "5", deep})
        BOOST_CHECK_THROW(s.loadLearned(bad), std::runtime_error);
    // PropertyTree keeps every scalar as text, so quoted numerals cannot be told apart and are accepted
    BOOST_CHECK_NO_THROW(s.loadLearned("{\"version\":1,\"locations\":{\"KPRG\":[\"39.7\",\"-87.6\",\"1\"]}}"));
    BOOST_TEST(s.find("KPRG")->first == 39.7, tt::tolerance(1e-9));
}
BOOST_AUTO_TEST_CASE(centroid_count_zero_round_trip) {
    LocationIndex a;
    a.learn(rec("KSMA", {polygon({{40.0, -86.0}, {40.0, -85.9}, {40.1, -85.9}, {40.1, -86.0}})}));
    const std::string j = a.learnedJson();
    BOOST_TEST(j.find("\"KSMA\":[40.050000,-85.950000,0]") != std::string::npos);
    LocationIndex b; b.loadLearned(j);
    BOOST_TEST(b.learnedJson() == j);
    b.learn(rec("KSMA", {point(40.20, -86.10)}));                 // still a centroid: a point replaces it
    BOOST_TEST(b.find("KSMA")->first == 40.20, tt::tolerance(1e-9));
    BOOST_TEST(b.learnedJson().find(",1]") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(operator_csv_edge_cases) {
    LocationIndex s;
    const std::size_t n = s.loadOperatorCsv("\xEF\xBB\xBFid,lat,lon\r\nKPRG,39.0,-87.0\r\nKGSH,41.5,-85.8,\nKAAA,1,2,3\n"
                                            "KBBB,,2\nKNAN,nan,2\nKINF,inf,2\nKBIG,1e999,2\nID,LAT,LON\n");
    BOOST_TEST(n == 1u); BOOST_TEST(s.operatorRejected() == 7u); BOOST_TEST(s.operatorRejectedFirstLine() == 3u);
    BOOST_TEST(s.find("KPRG")->second == -87.0, tt::tolerance(1e-9));
    BOOST_TEST(s.loadOperatorCsv("IND,39.8,-86.3\n") == 1u);     // a reload replaces the operator set
    BOOST_TEST(!s.find("KPRG").has_value()); BOOST_TEST(s.find("IND").has_value());
    BOOST_TEST(s.operatorRejected() == 0u); BOOST_TEST(s.operatorRejectedFirstLine() == 0u);
}
BOOST_AUTO_TEST_CASE(learned_ids_capped) {
    QuietLog quiet;
    LocationIndex s;
    for (std::size_t i = 0; i < kMaxLearnedIds; ++i) BOOST_REQUIRE(s.learn(rec(idFor('K', i), {point(40.0, -86.0)})));
    BOOST_TEST(!s.learn(rec("CXXX", {point(40.0, -86.0)}))); BOOST_TEST(!s.learn(rec("CYYY", {point(40.0, -86.0)})));
    BOOST_TEST(s.learnedCount() == kMaxLearnedIds); BOOST_TEST(s.learnedOverflow() == 2u);
    BOOST_TEST(!s.learn(rec(idFor('K', 0), {point(40.0, -86.0)})));   // known ids still learn
    BOOST_TEST(s.learnedJson().find("\"" + idFor('K', 0) + "\":[40.000000,-86.000000,2]") != std::string::npos);
    LocationIndex b; BOOST_CHECK_NO_THROW(b.loadLearned(s.learnedJson()));
    std::string over = s.learnedJson();
    over.insert(over.size() - 2, ",\"CXXX\":[40,-86,1]");
    BOOST_CHECK_THROW(b.loadLearned(over), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(operator_rows_capped) {
    QuietLog quiet;
    std::string csv;
    for (std::size_t i = 0; i < kMaxOperatorRows + 3; ++i) csv += idFor('Q', i, 4) + ",40,-86\n";
    LocationIndex s;
    BOOST_TEST(s.loadOperatorCsv(csv) == kMaxOperatorRows); BOOST_TEST(s.operatorOverflow() == 3u);
    BOOST_TEST(s.operatorRejected() == 0u);
    BOOST_TEST(s.loadOperatorCsv("IND,39.8,-86.3\n") == 1u); BOOST_TEST(s.operatorOverflow() == 0u);
}
