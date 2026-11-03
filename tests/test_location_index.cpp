// LocationIndex: positions learned from TWGO shapes, persisted, merged over
// an operator CSV (design §3.2).

#define BOOST_TEST_MODULE location_index
#include <boost/test/unit_test.hpp>

#include "fisb/location_index.hpp"

#include <initializer_list>
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
