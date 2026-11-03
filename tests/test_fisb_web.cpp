#define BOOST_TEST_MODULE skycued_fisb_web
#include <boost/test/unit_test.hpp>

// The FIS-B web API (/api/fisb/*) and the fisb_* WebSocket items
// (docs/web-api.md "FIS-B"), over an Engine replaying
// fixtures/fisb/received-with-uplinks.jsonl (2015 uplinks) unpaced.

#include "engine_test_support.hpp"

#include "fisb/location_index.hpp"
#include "runtime_config.hpp"
#include "time_util.hpp"
#include "web_api.hpp"
#include "web_fisb.hpp"
#include "web_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <map>
#include <set>

using namespace enginetest;
using skycued::RuntimeConfig;
using skycued::WebConfig;
using skycued::WebServer;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace pt = boost::property_tree;

namespace {

const std::string kReceivedWithUplinks = std::string(SKYCUE_FIXTURE_DIR) + "/fisb/received-with-uplinks.jsonl";
constexpr uint64_t kFixtureUplinks = 704, kFixtureObservations = 361;

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// Redirects fd 2 to `path` until restore() or destruction.
struct StderrCapture {
    std::string path;
    int saved = -1;
    explicit StderrCapture(const std::string& p) : path(p)
    {
        std::fflush(stderr);
        saved = ::dup(2);
        std::FILE* f = std::fopen(path.c_str(), "w");
        ::dup2(::fileno(f), 2);
        std::fclose(f);
    }
    ~StderrCapture() { restore(); }
    std::string text()
    {
        std::fflush(stderr);
        return readAll(path);
    }
    void restore()
    {
        if (saved < 0)
            return;
        std::fflush(stderr);
        ::dup2(saved, 2);
        ::close(saved);
        saved = -1;
    }
};

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

pt::ptree parseJson(const std::string& text)
{
    pt::ptree tree;
    std::istringstream in(text);
    pt::read_json(in, tree);
    return tree;
}

struct Reply {
    unsigned status = 0;
    std::string body, allow;
};

Reply request(uint16_t port, http::verb verb, const std::string& target)
{
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    timeval tv{};
    tv.tv_sec = 10;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    req.prepare_payload();
    http::write(s, req);
    beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    parser.body_limit(64u << 20);
    http::read(s, buf, parser);
    Reply r;
    r.status = parser.get().result_int();
    r.body = parser.get().body();
    r.allow = std::string(parser.get()[http::field::allow]);
    return r;
}

Reply get(uint16_t port, const std::string& target) { return request(port, http::verb::get, target); }

// A WebSocket client whose thread collects every text message.
struct WsClient {
    asio::io_context io;
    websocket::stream<tcp::socket> ws{io};
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> msgs;

    explicit WsClient(uint16_t port)
    {
        ws.next_layer().connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        ws.handshake("127.0.0.1", "/ws");
        reader = std::thread([this] {
            for (;;) {
                beast::flat_buffer b;
                boost::system::error_code ec;
                ws.read(b, ec);
                std::lock_guard<std::mutex> lk(m);
                if (ec)
                    return;
                msgs.emplace_back(asio::buffers_begin(b.data()), asio::buffers_end(b.data()));
                cv.notify_all();
            }
        });
    }
    ~WsClient()
    {
        ::shutdown(ws.next_layer().native_handle(), SHUT_RDWR);
        if (reader.joinable())
            reader.join();
        boost::system::error_code ec;
        ws.next_layer().close(ec);
    }
    // Waits until some message contains `needle`; returns it ("" on timeout).
    std::string waitFor(const std::string& needle, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        std::string found;
        cv.wait_for(lk, timeout, [&] {
            for (const auto& msg : msgs)
                if (has(msg, needle)) {
                    found = msg;
                    return true;
                }
            return false;
        });
        return found;
    }
    std::vector<std::string> all()
    {
        std::lock_guard<std::mutex> lk(m);
        return msgs;
    }
};

struct FisbWebOptions {
    bool fisb = true;         // FISB_ENABLE
    bool replay = true;       // replay the fixture unpaced, else a FakeSource
    bool start_engine = true; // else startEngine() later (to connect a client first)
    std::string recording = kReceivedWithUplinks;  // what replay replays
    // Non-empty: engine->loadLocations(locations_dir + "/fisb-locations.json", locations_csv) before start().
    std::string locations_dir{};
    std::string locations_csv{};
};

// Engine (+ Forwarder, RuntimeConfig, WebServer) on an IoThread.
struct FisbWebFx {
    TempDir tmp;
    IoThread t;
    FakeSource* src = nullptr;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<RuntimeConfig> rc;
    std::unique_ptr<WebServer> web;
    uint16_t port = 0;

    explicit FisbWebFx(const FisbWebOptions& o = FisbWebOptions())
    {
        const std::string conf = tmp.path + "/skycue.conf";
        const std::string www = tmp.path + "/www";
        ::mkdir(www.c_str(), 0755);
        writeFile(www + "/index.html", "<!doctype html><title>t</title>\n");
        writeFile(conf, std::string("RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\nRECEIVER_ALT_M=100\n"
                                    "ANTENNA_HEIGHT_M=5\nTRACK_TIMEOUT_S=5\nFISB_ENABLE=") +
                            (o.fisb ? "yes" : "no") + "\n");
        const Config cfg = skycued::parseConfigFile(conf);
        t.call([&] {
            ForwarderConfig fc;
            fc.tcp_listen_ephemeral = true;
            fwd = std::make_unique<Forwarder>(t.io, fc);
            std::unique_ptr<ObservationSource> source;
            if (o.replay)
                source = std::make_unique<skycued::ReplayObservationSource>(t.io, o.recording, 0, false);
            else
                source = fakeSource(src)(t.io);
            engine = std::make_unique<Engine>(t.io, cfg, sourceInputs(t.io, std::move(source)), *fwd, nullptr, nullptr);
            if (!o.locations_dir.empty())
                engine->loadLocations(o.locations_dir + "/fisb-locations.json", o.locations_csv);
            rc = std::make_unique<RuntimeConfig>(t.io, conf, *engine);
            rc->load();
            WebConfig wc;
            wc.bind = "127.0.0.1";
            wc.port = 0;
            wc.www_dir = www;
            web = std::make_unique<WebServer>(t.io, *engine, *rc, wc);
            web->start();
            port = web->localPort();
            if (o.start_engine)
                engine->start();
        });
    }
    ~FisbWebFx()
    {
        try {
            t.call([this] {
                web.reset();
                rc.reset();
                engine.reset();
                fwd.reset();
            });
        } catch (...) {
        }
        t.join();
    }
    void startEngine()
    {
        t.call([this] { engine->start(); });
    }
    skycued::EngineStats stats()
    {
        return t.call([this] { return engine->stats(); });
    }
    // Until every record of the fixture is in.
    bool waitReplayed(uint64_t uplinks = kFixtureUplinks, uint64_t observations = kFixtureObservations,
                      std::chrono::seconds timeout = 20s)
    {
        const auto end = steady::now() + timeout;
        while (steady::now() < end) {
            const auto s = stats();
            if (s.fisb_uplinks == uplinks && s.observations == observations)
                return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }
    skycued::fisb::FisbSnapshot fisbSnapshot()
    {
        return t.call([this] { return engine->fisbSnapshot(); });
    }
};

// tg30.978's dump978 lines ("+hex;...;t=<epoch s>;") as a received
// recording of uat_uplink records, in `path`; returns how many.
std::size_t tg30AsRecording(const std::string& path)
{
    std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/fisb/tg30.978");
    std::ofstream out(path, std::ios::trunc);
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '+')
            continue;
        const std::size_t t = line.find(";t=");
        BOOST_REQUIRE(t != std::string::npos);
        const double epoch = std::stod(line.substr(t + 3));
        const auto when = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(static_cast<int64_t>(std::llround(epoch * 1000))));
        out << "{\"time\":\"" << skycued::formatIso8601Ms(when) << "\",\"type\":\"uat_uplink\",\"uplink_hex\":\""
            << line.substr(1, line.find(';') - 1) << "\"}\n";
        ++n;
    }
    return n;
}

std::size_t count(const pt::ptree& array) { return array.size(); }

double num(const pt::ptree& v) { return v.get_value<double>(); }

// The [lon, lat] pairs of a GeoJSON geometry, flattened.
std::vector<std::pair<double, double>> positions(const pt::ptree& geometry)
{
    std::vector<std::pair<double, double>> out;
    const std::string type = geometry.get<std::string>("type");
    const pt::ptree& c = geometry.get_child("coordinates");
    auto pos = [&](const pt::ptree& p) {
        BOOST_REQUIRE_EQUAL(p.size(), 2u);
        auto it = p.begin();
        const double lon = num(it->second);
        ++it;
        out.emplace_back(lon, num(it->second));
    };
    if (type == "Point") {
        pos(c);
    } else if (type == "LineString") {
        for (const auto& p : c)
            pos(p.second);
    } else if (type == "Polygon") {
        for (const auto& ring : c) {
            BOOST_TEST(ring.second.size() >= 4u);
            for (const auto& p : ring.second)
                pos(p.second);
            // Closed ring.
            BOOST_TEST(ring.second.front().second.front().second.data() ==
                       ring.second.back().second.front().second.data());
        }
    } else {
        BOOST_ERROR("unexpected geometry type " << type);
    }
    return out;
}

}  // namespace

// /api/fisb/status after the replay: counts, stations, the regional mosaic,
// and the same object in the WebSocket snapshot's "fisb".
BOOST_AUTO_TEST_CASE(fisb_status_shape)
{
    FisbWebFx f;
    BOOST_REQUIRE(f.waitReplayed());
    const Reply r = get(f.port, "/api/fisb/status");
    BOOST_REQUIRE_EQUAL(r.status, 200u);
    BOOST_TEST_MESSAGE("status: " << r.body);
    const pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("enabled") == "true");
    BOOST_TEST(s.get<uint64_t>("uplinks") == kFixtureUplinks);
    BOOST_TEST(s.get<uint64_t>("products") > 0u);
    BOOST_TEST(s.count("bad_frames") == 1u);
    BOOST_TEST(s.count("segments_open") == 1u);
    BOOST_TEST(s.count("last_uplink_time") == 1u);
    BOOST_TEST(s.get<std::string>("last_uplink_time").compare(0, 4, "2015") == 0);

    const pt::ptree& stations = s.get_child("stations");
    BOOST_REQUIRE(count(stations) >= 1u);
    for (const auto& st : stations) {
        for (const char* k : {"site_id", "lat", "lon", "count", "last_seen"})
            BOOST_TEST(st.second.count(k) == 1u, "station has " << k);
        BOOST_TEST(std::abs(st.second.get<double>("lat")) <= 90.0);
        BOOST_TEST(std::abs(st.second.get<double>("lon")) <= 180.0);
    }

    const pt::ptree& regional = s.get_child("radar.regional");
    BOOST_TEST(regional.get<int>("blocks") == 671);
    BOOST_TEST(regional.count("product_time") == 1u);
    BOOST_TEST(regional.count("age_s") == 1u);
    BOOST_TEST(s.get_child("radar").count("conus") == 0u);  // absent, not null
    BOOST_TEST(s.count("clock_skew_s") == 0u);  // no product time ahead of the data clock
    BOOST_TEST(!has(r.body, "null"));
    BOOST_TEST(s.count("rasters") == 1u);

    const pt::ptree& counts = s.get_child("counts");
    BOOST_TEST(counts.get<int>("metar") == 127);
    BOOST_TEST(counts.get<int>("taf") == 33);
    for (const char* k : {"pirep", "winds", "twgo", "cancelled"})
        BOOST_TEST(counts.count(k) == 1u, "counts has " << k);
    BOOST_TEST(counts.get<int>("twgo") >= 20);

    // The same object in the snapshot, and never a mosaic's blocks.
    WsClient c(f.port);
    const std::string snap = c.waitFor("\"type\":\"snapshot\"");
    BOOST_REQUIRE(!snap.empty());
    const pt::ptree sp = parseJson(snap);
    const pt::ptree& fisb = sp.get_child("fisb");
    BOOST_TEST(fisb.get<uint64_t>("uplinks") == kFixtureUplinks);
    BOOST_TEST(fisb.get<int>("radar.regional.blocks") == 671);
    BOOST_TEST(fisb.get<int>("counts.metar") == 127);
    BOOST_TEST(!has(snap, "\"rle\""));
    // /api/state carries it too.
    BOOST_TEST(has(get(f.port, "/api/state").body, "\"fisb\":{\"enabled\":true"));

    // None of status, state or the snapshot copied a block out of the store;
    // the radar route copies its one mosaic.
    // Nor do the text and shapes routes (records only).
    BOOST_TEST(get(f.port, "/api/fisb/text").status == 200u);
    BOOST_TEST(get(f.port, "/api/fisb/text?type=twgo").status == 200u);
    BOOST_TEST(get(f.port, "/api/fisb/shapes").status == 200u);
    const uint64_t copied = f.t.call([&] { return f.engine->fisbBlocksCopied(); });
    BOOST_TEST(copied == 0u);
    get(f.port, "/api/fisb/radar?product=regional");
    BOOST_TEST(f.t.call([&] { return f.engine->fisbBlocksCopied(); }) == 671u);

    // GET only.
    const Reply post = request(f.port, http::verb::post, "/api/fisb/status");
    BOOST_TEST(post.status == 405u);
    BOOST_TEST(post.allow == "GET");
    BOOST_TEST(get(f.port, "/api/fisb/nope").status == 404u);
}

BOOST_AUTO_TEST_CASE(text_filter_by_station)
{
    FisbWebFx f;
    BOOST_REQUIRE(f.waitReplayed());
    const pt::ptree metars = parseJson(get(f.port, "/api/fisb/text?type=metar").body);
    const pt::ptree& recs = metars.get_child("records");
    BOOST_REQUIRE_EQUAL(count(recs), 127u);
    std::string prev_station, prev_issued, a_station;
    for (const auto& rec : recs) {
        const auto& r = rec.second;
        BOOST_TEST(r.get<std::string>("kind") == "metar");
        for (const char* k : {"key", "station", "issued", "expires", "text"})
            BOOST_TEST(r.count(k) == 1u, "record has " << k);
        const std::string st = r.get<std::string>("station"), issued = r.get<std::string>("issued");
        BOOST_TEST((prev_station < st || (prev_station == st && prev_issued <= issued)),
                   "sorted by station then issued: " << prev_station << " then " << st);
        prev_station = st;
        prev_issued = issued;
        if (a_station.empty() && st.size() == 4)
            a_station = st;
    }
    BOOST_REQUIRE(!a_station.empty());

    // A whole station: exactly its METAR.
    const pt::ptree one = parseJson(get(f.port, "/api/fisb/text?type=metar&station=" + a_station).body);
    BOOST_REQUIRE_EQUAL(count(one.get_child("records")), 1u);
    BOOST_TEST(one.get_child("records").front().second.get<std::string>("station") == a_station);

    // A prefix, case-insensitive: every record matches, METAR and TAF alike.
    const std::string prefix = a_station.substr(0, 2);
    std::string lower = prefix;
    for (auto& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const pt::ptree pre = parseJson(get(f.port, "/api/fisb/text?station=" + lower).body);
    std::set<std::string> kinds;
    BOOST_TEST(count(pre.get_child("records")) >= 1u);
    for (const auto& rec : pre.get_child("records")) {
        BOOST_TEST(rec.second.get<std::string>("station").compare(0, prefix.size(), prefix) == 0);
        kinds.insert(rec.second.get<std::string>("kind"));
    }
    BOOST_TEST(kinds.count("metar") == 1u);

    // No parameters: every 413 kind.
    const pt::ptree everything = parseJson(get(f.port, "/api/fisb/text").body);
    BOOST_TEST(count(everything.get_child("records")) >= 127u + 33u);

    // The TAF of a station.
    const pt::ptree tafs = parseJson(get(f.port, "/api/fisb/text?type=taf").body);
    BOOST_TEST(count(tafs.get_child("records")) == 33u);

    // TWGO records without graphics, on request.
    const Reply twgo = get(f.port, "/api/fisb/text?type=twgo");
    BOOST_TEST(twgo.status == 200u);
    const pt::ptree twgo_recs = parseJson(twgo.body);
    BOOST_TEST(count(twgo_recs.get_child("records")) >= 1u);
    for (const auto& rec : twgo_recs.get_child("records")) {
        BOOST_TEST(rec.second.get<std::string>("has_graphics") == "false");
        // kind "twgo" tells the page (fisb_text items) it is a TWGO record.
        BOOST_TEST(rec.second.get<std::string>("kind") == "twgo");
        BOOST_TEST(rec.second.count("pending") == 1u);
    }

    BOOST_TEST(get(f.port, "/api/fisb/text?type=sigmet").status == 400u);
    BOOST_TEST(has(get(f.port, "/api/fisb/text?type=sigmet").body, "\"error\""));
    BOOST_TEST(count(parseJson(get(f.port, "/api/fisb/text?station=QQQQ").body).get_child("records")) == 0u);
}

BOOST_AUTO_TEST_CASE(shapes_geojson_valid)
{
    FisbWebFx f;
    BOOST_REQUIRE(f.waitReplayed());
    const Reply r = get(f.port, "/api/fisb/shapes");
    BOOST_REQUIRE_EQUAL(r.status, 200u);
    const pt::ptree fc = parseJson(r.body);
    BOOST_TEST(fc.get<std::string>("type") == "FeatureCollection");
    const pt::ptree& features = fc.get_child("features");
    BOOST_REQUIRE(count(features) >= 1u);
    std::set<std::string> geoms, products;
    for (const auto& ft : features) {
        const auto& feature = ft.second;
        BOOST_TEST(feature.get<std::string>("type") == "Feature");
        BOOST_REQUIRE(feature.count("geometry") == 1u);
        BOOST_REQUIRE(feature.count("properties") == 1u);
        const auto& props = feature.get_child("properties");
        for (const char* k : {"product", "key", "geom", "cancelled", "agl"})
            BOOST_TEST(props.count(k) == 1u, "properties have " << k);
        const std::string geom = props.get<std::string>("geom");
        geoms.insert(geom);
        products.insert(props.get<std::string>("product"));
        const std::string type = feature.get<std::string>("geometry.type");
        if (geom == "circle") {
            BOOST_TEST(type == "Point");
            BOOST_TEST(props.count("radius_nm") == 1u);
        } else if (geom == "prism" || geom == "polygon") {
            BOOST_TEST(type == "Polygon");
        } else if (geom == "polyline") {
            BOOST_TEST(type == "LineString");
        } else {
            BOOST_TEST(type == "Point");
        }
        for (const auto& p : positions(feature.get_child("geometry"))) {
            BOOST_TEST(p.first >= -180.0);
            BOOST_TEST(p.first <= 180.0);
            BOOST_TEST(p.second >= -90.0);
            BOOST_TEST(p.second <= 90.0);
        }
    }
    BOOST_TEST_MESSAGE("geoms: " << geoms.size() << " products: " << products.size());

    // [lon, lat]: the first feature of a polygon record is that record's
    // first point swapped. The fixture's NOTAMs are around the US east
    // (lon < 0, lat > 0).
    const auto snap = f.fisbSnapshot();
    const skycued::fisb::TwgoRecord* rec = nullptr;
    for (const auto& t : snap.twgo)
        if (!t.cancelled && !t.shapes.empty() && t.shapes.front().geom == skycued::fisb::Shape::polygon) {
            rec = &t;
            break;
        }
    BOOST_REQUIRE(rec);
    bool matched = false;
    for (const auto& ft : features) {
        if (ft.second.get<std::string>("properties.key") != rec->key.str())
            continue;
        const auto pts = positions(ft.second.get_child("geometry"));
        BOOST_REQUIRE(!pts.empty());
        BOOST_TEST(std::abs(pts.front().first - rec->shapes.front().points.front().second) < 1e-5);
        BOOST_TEST(std::abs(pts.front().second - rec->shapes.front().points.front().first) < 1e-5);
        BOOST_TEST(pts.front().first < 0.0);
        matched = true;
        break;
    }
    BOOST_TEST(matched);

    // ?product= filters.
    const std::string only = *products.begin();
    const pt::ptree filtered = parseJson(get(f.port, "/api/fisb/shapes?product=" + only).body);
    BOOST_TEST(count(filtered.get_child("features")) >= 1u);
    for (const auto& ft : filtered.get_child("features"))
        BOOST_TEST(ft.second.get<std::string>("properties.product") == only);
    const pt::ptree none = parseJson(get(f.port, "/api/fisb/shapes?product=17,16").body);  // a list
    for (const auto& ft : none.get_child("features"))
        BOOST_TEST((ft.second.get<int>("properties.product") == 16 || ft.second.get<int>("properties.product") == 17));
    BOOST_TEST(get(f.port, "/api/fisb/shapes?product=8,x").status == 400u);
}

BOOST_AUTO_TEST_CASE(radar_rle_expands_to_128_per_block)
{
    FisbWebFx f;
    BOOST_REQUIRE(f.waitReplayed());
    const Reply r = get(f.port, "/api/fisb/radar?product=regional");
    BOOST_REQUIRE_EQUAL(r.status, 200u);
    const pt::ptree m = parseJson(r.body);
    BOOST_TEST(m.get<int>("product") == 63);
    BOOST_TEST(m.get<int>("bins_per_block") == 128);
    BOOST_TEST(m.count("scale") == 1u);
    BOOST_TEST(m.count("product_time") == 1u);
    const pt::ptree& blocks = m.get_child("blocks");
    BOOST_TEST(count(blocks) == 671u);
    BOOST_TEST(m.get<std::size_t>("block_count") == count(blocks));
    // Expanded, every block is the store's 128 bins.
    const auto snap = f.fisbSnapshot();
    std::map<uint32_t, const skycued::fisb::RadarBlock*> held;
    for (const auto& mo : snap.radar)
        if (mo.product == 63)
            for (const auto& b : mo.blocks)
                held[b.block] = &b;
    BOOST_REQUIRE_EQUAL(held.size(), 671u);
    for (const auto& b : blocks) {
        int total = 0;
        std::vector<int> bins;
        for (const auto& run : b.second.get_child("rle")) {
            BOOST_REQUIRE_EQUAL(run.second.size(), 2u);
            const int value = run.second.front().second.get_value<int>();
            const int n = run.second.back().second.get_value<int>();
            BOOST_TEST(value >= 0);
            BOOST_TEST(value <= 7);
            BOOST_TEST(n >= 1);
            total += n;
            bins.insert(bins.end(), static_cast<std::size_t>(std::max(n, 0)), value);
        }
        BOOST_TEST(total == 128);
        const auto it = held.find(b.second.get<uint32_t>("block"));
        BOOST_REQUIRE(it != held.end());
        BOOST_TEST(std::equal(bins.begin(), bins.end(), it->second->bins.begin(), it->second->bins.end()));
        BOOST_TEST(std::abs(b.second.get<double>("lat0")) <= 90.0);
        BOOST_TEST(b.second.get<double>("dlat") > 0.0);
    }

    // The palette: one entry per level 0-7, a colour for each.
    const pt::ptree& palette = m.get_child("palette");
    BOOST_REQUIRE_EQUAL(count(palette), 8u);
    int level = 0;
    for (const auto& p : palette) {
        BOOST_TEST(p.second.get<int>("level") == level++);
        BOOST_TEST(p.second.count("color") == 1u);
    }
    BOOST_TEST(palette.front().second.get<std::string>("color") == "transparent");
    BOOST_TEST(m.count("legend") == 1u);
    BOOST_TEST(count(m.get_child("legend.levels")) == 8u);
    BOOST_TEST(get(f.port, "/api/fisb/radar?product=63").status == 200u);
}

BOOST_AUTO_TEST_CASE(radar_404_when_absent)
{
    {
        // No uplinks at all.
        FisbWebFx f(FisbWebOptions{true, false, true});
        BOOST_TEST(get(f.port, "/api/fisb/radar?product=regional").status == 404u);
        BOOST_TEST(get(f.port, "/api/fisb/raster?product=70").status == 404u);
        BOOST_TEST(get(f.port, "/api/fisb/radar").status == 400u);
        BOOST_TEST(get(f.port, "/api/fisb/radar?product=nexrad").status == 400u);
        BOOST_TEST(get(f.port, "/api/fisb/raster?product=63").status == 400u);
        BOOST_TEST(get(f.port, "/api/fisb/raster?product=70&altitude=x").status == 400u);
        BOOST_TEST(request(f.port, http::verb::put, "/api/fisb/radar?product=regional").status == 405u);
    }
    // The fixture has a regional mosaic but no CONUS one.
    FisbWebFx f;
    BOOST_REQUIRE(f.waitReplayed());
    const Reply r = get(f.port, "/api/fisb/radar?product=conus");
    BOOST_TEST(r.status == 404u);
    BOOST_TEST(has(r.body, "\"error\""));
}

BOOST_AUTO_TEST_CASE(fisb_disabled_status_enabled_false)
{
    FisbWebFx f(FisbWebOptions{false, true, true});
    BOOST_REQUIRE(f.waitReplayed(0));  // FIS-B off: uplinks are not counted
    const Reply r = get(f.port, "/api/fisb/status");
    BOOST_REQUIRE_EQUAL(r.status, 200u);
    const pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("enabled") == "false");
    BOOST_TEST(s.get<int>("uplinks") == 0);
    BOOST_TEST(count(s.get_child("stations")) == 0u);
    BOOST_TEST(has(r.body, "\"radar\":{}"));
    BOOST_TEST(!has(r.body, "null"));
    BOOST_TEST(s.get<int>("counts.metar") == 0);
    BOOST_TEST(count(parseJson(get(f.port, "/api/fisb/text").body).get_child("records")) == 0u);
    BOOST_TEST(count(parseJson(get(f.port, "/api/fisb/shapes").body).get_child("features")) == 0u);
    BOOST_TEST(get(f.port, "/api/fisb/radar?product=regional").status == 404u);
}

// A client connected before the replay starts sees fisb_text and fisb_shape
// items, then (once the store's 10 s quiescence passes on the data clock) a
// fisb_radar notification without blocks.
BOOST_AUTO_TEST_CASE(ws_fisb_events_on_replay)
{
    FisbWebFx f(FisbWebOptions{true, true, false});
    WsClient c(f.port);
    const std::string snap = c.waitFor("\"type\":\"snapshot\"");
    BOOST_REQUIRE(!snap.empty());
    BOOST_TEST(has(snap, "\"fisb\":{\"enabled\":true"));
    f.startEngine();

    BOOST_TEST(!c.waitFor("\"type\":\"fisb_text\"").empty());
    BOOST_TEST(!c.waitFor("\"type\":\"fisb_shape\"").empty());
    const std::string radar = c.waitFor("\"type\":\"fisb_radar\"", 30s);
    BOOST_REQUIRE(!radar.empty());

    std::size_t texts = 0, shapes = 0, radars = 0;
    for (const auto& msg : c.all()) {
        const pt::ptree m = parseJson(msg);
        if (m.get<std::string>("type") != "events")
            continue;
        for (const auto& item : m.get_child("items")) {
            const auto& it = item.second;
            const std::string type = it.get<std::string>("type");
            if (type == "fisb_text") {
                ++texts;
                BOOST_TEST(it.count("record") == 1u);
                BOOST_TEST(it.get_child("record").count("key") == 1u);
            } else if (type == "fisb_shape") {
                ++shapes;
                BOOST_TEST(it.count("key") == 1u);
                BOOST_TEST(it.count("product") == 1u);
                BOOST_REQUIRE(it.count("features") == 1u);
                BOOST_TEST(count(it.get_child("features")) >= 1u);
                for (const auto& ft : it.get_child("features")) {
                    BOOST_TEST(ft.second.get<std::string>("type") == "Feature");
                    BOOST_TEST(ft.second.get<std::string>("properties.key") == it.get<std::string>("key"));
                    positions(ft.second.get_child("geometry"));
                }
            } else if (type == "fisb_radar") {
                ++radars;
                BOOST_TEST(it.get<int>("product") == 63);
                BOOST_TEST(it.get<int>("blocks") == 671);
                BOOST_TEST(it.count("product_time") == 1u);
                BOOST_TEST(it.count("key") == 1u);
            }
        }
    }
    BOOST_TEST(texts >= 127u + 33u);
    BOOST_TEST(shapes >= 1u);
    BOOST_TEST(radars == 1u);
    BOOST_TEST(!has(radar, "\"rle\""));
}

// The fisb_* items from engine events directly: expired and raster.
BOOST_AUTO_TEST_CASE(event_json_items)
{
    using skycued::EngineEvent;
    using skycued::EventKind;
    using skycued::FisbNotice;
    EngineEvent ev;
    ev.kind = EventKind::fisb_expired;
    FisbNotice n;
    n.product = 413;
    n.key = "metar:KSFO";
    n.reason = "expired";
    n.expired_kind = skycued::fisb::FisbEvent::Kind::text;  // as the store sets it
    n.text = skycued::fisb::TextRecord();
    n.text->kind = skycued::fisb::TextRecord::metar;
    n.text->station = "KSFO";
    n.expires = std::chrono::system_clock::time_point(std::chrono::seconds(1446000000));
    ev.fisb = n;
    const pt::ptree e = parseJson(skycued::webapi::eventJson(ev));
    BOOST_TEST(e.get<std::string>("type") == "fisb_expired");
    BOOST_TEST(e.get<std::string>("kind") == "text");
    BOOST_TEST(e.get<std::string>("key") == "metar:KSFO");
    BOOST_TEST(e.get<std::string>("station") == "KSFO");
    BOOST_TEST(e.get<int>("product") == 413);
    BOOST_TEST(e.get<std::string>("reason") == "expired");

    n.product = 8;
    n.key = "8-15-12045-KSCK-1";
    n.reason = "cancelled";
    n.expired_kind = skycued::fisb::FisbEvent::Kind::twgo;
    n.text.reset();
    ev.fisb = n;
    const pt::ptree c = parseJson(skycued::webapi::eventJson(ev));
    BOOST_TEST(c.get<std::string>("kind") == "twgo");
    BOOST_TEST(c.get<std::string>("reason") == "cancelled");
    BOOST_TEST(c.count("station") == 0u);

    n.product = 70;
    n.key = "70-3";
    n.reason = "expired";
    n.expired_kind = skycued::fisb::FisbEvent::Kind::raster;
    ev.fisb = n;
    BOOST_TEST(parseJson(skycued::webapi::eventJson(ev)).get<std::string>("kind") == "raster");

    ev.kind = EventKind::fisb_raster;
    n.product_time = n.expires;
    n.block_count = 12;
    ev.fisb = n;
    const std::string raster = skycued::webapi::eventJson(ev);
    const pt::ptree rs = parseJson(raster);
    BOOST_TEST(rs.get<std::string>("type") == "fisb_raster");
    BOOST_TEST(rs.get<int>("product") == 70);
    BOOST_TEST(rs.get<int>("altitude_code") == 3);
    BOOST_TEST(rs.get<int>("blocks") == 12);
    BOOST_TEST(rs.get<std::string>("key") == "70-3");

    EngineEvent loc;
    loc.kind = EventKind::fisb_location;
    FisbNotice ln;
    ln.key = "KPRG";
    ln.position = std::make_pair(39.699097, -87.670212);
    loc.fisb = ln;
    BOOST_TEST(skycued::webapi::eventJson(loc) == "{\"type\":\"fisb_location\",\"id\":\"KPRG\",\"lat\":39.699097,\"lon\":-87.670212}");
}

// tg30.978 carries the rasters and a CONUS mosaic that the 2015 sample has
// not: /api/fisb/raster with its legends, ?altitude=, and the CONUS palette.
BOOST_AUTO_TEST_CASE(raster_routes_on_tg30)
{
    TempDir tmp;
    const std::string rec = tmp.path + "/tg30-received.jsonl";
    const std::size_t uplinks = tg30AsRecording(rec);
    BOOST_REQUIRE_EQUAL(uplinks, 963u);
    FisbWebOptions o;
    o.recording = rec;
    FisbWebFx f(o);
    BOOST_REQUIRE(f.waitReplayed(uplinks, 0));

    const pt::ptree s = parseJson(get(f.port, "/api/fisb/status").body);
    BOOST_TEST(s.get<int>("radar.conus.blocks") > 0);
    std::set<std::string> layers;
    for (const auto& r : s.get_child("rasters"))
        layers.insert(r.second.get<std::string>("key"));
    BOOST_TEST(layers.count("70-0") == 1u);
    BOOST_TEST(layers.count("103") == 1u);

    // CONUS: level 0 no data and level 1 (empty blocks) are not drawn.
    const pt::ptree conus = parseJson(get(f.port, "/api/fisb/radar?product=conus").body);
    BOOST_TEST(conus.get<int>("product") == 64);
    auto pal = conus.get_child("palette").begin();
    BOOST_TEST(pal->second.get<std::string>("color") == "transparent");
    ++pal;
    BOOST_TEST(pal->second.get<std::string>("color") == "transparent");
    BOOST_TEST(pal->second.get<int>("dbz_max") == 20);
    ++pal;
    BOOST_TEST(pal->second.get<std::string>("color") != "transparent");

    // Icing: the lowest layer by default, a chosen one with ?altitude=.
    const pt::ptree icing = parseJson(get(f.port, "/api/fisb/raster?product=70").body);
    BOOST_TEST(icing.get<int>("altitude_code") == 0);
    BOOST_TEST(icing.get<std::string>("legend.approximate") == "true");
    BOOST_TEST(icing.get<std::string>("legend.palette_field") == "severity");
    BOOST_TEST(icing.get<int>("legend.altitude_ft") == 2000);
    BOOST_TEST(count(icing.get_child("legend.fields")) == 3u);
    BOOST_TEST(count(icing.get_child("palette")) == 8u);
    for (const auto& b : icing.get_child("blocks")) {
        int total = 0;
        for (const auto& run : b.second.get_child("rle"))
            total += run.second.back().second.get_value<int>();
        BOOST_TEST(total == 128);
    }
    const pt::ptree icing3 = parseJson(get(f.port, "/api/fisb/raster?product=70&altitude=3").body);
    BOOST_TEST(icing3.get<int>("altitude_code") == 3);
    BOOST_TEST(icing3.get<int>("legend.altitude_ft") == 8000);

    const pt::ptree lightning = parseJson(get(f.port, "/api/fisb/raster?product=103").body);
    BOOST_TEST(count(lightning.get_child("legend.levels")) == 16u);
    BOOST_TEST(count(lightning.get_child("palette")) == 16u);
    BOOST_TEST(lightning.get<std::string>("legend.approximate") == "true");
    for (const char* p : {"71", "84", "90", "91"}) {
        const Reply r = get(f.port, std::string("/api/fisb/raster?product=") + p);
        BOOST_TEST(r.status == 200u, "raster " << p);
        if (r.status == 200u)
            BOOST_TEST(parseJson(r.body).count("legend") == 1u);
    }
}

namespace {

using Locations = std::map<std::string, std::pair<double, double>>;

// GET /api/fisb/locations's body: {"<id>":[lat,lon],...}.
Locations locationsOf(const std::string& body)
{
    Locations out;
    for (const auto& [id, pos] : parseJson(body)) {
        BOOST_REQUIRE_EQUAL(pos.size(), 2u);
        out[id] = {num(pos.front().second), num(pos.back().second)};
    }
    return out;
}

bool near(std::pair<double, double> a, double lat, double lon)
{
    return std::abs(a.first - lat) <= 1e-6 && std::abs(a.second - lon) <= 1e-6;
}

// The ids of fisb-locations.json at `path`.
std::set<std::string> savedIds(const std::string& path)
{
    std::set<std::string> ids;
    const pt::ptree doc = parseJson(readAll(path));
    for (const auto& kv : doc.get_child("locations"))
        ids.insert(kv.first);
    return ids;
}

// FisbWebOptions replaying tg30 (written into `dir`), locations kept in `dir`.
FisbWebOptions tg30Options(const TempDir& dir, std::size_t& uplinks)
{
    FisbWebOptions o;
    o.recording = dir.path + "/tg30-received.jsonl";
    uplinks = tg30AsRecording(o.recording);
    o.locations_dir = dir.path;
    return o;
}

}  // namespace

// Every NOTAM point location in tg30 is learned, served and announced.
BOOST_AUTO_TEST_CASE(locations_learned_on_tg30)
{
    TempDir dir;
    std::size_t uplinks = 0;
    FisbWebOptions o = tg30Options(dir, uplinks);
    o.start_engine = false;
    FisbWebFx f(o);
    WsClient c(f.port);
    BOOST_REQUIRE(!c.waitFor("\"type\":\"snapshot\"").empty());
    f.startEngine();
    BOOST_REQUIRE(f.waitReplayed(uplinks, 0));

    const Reply r = get(f.port, "/api/fisb/locations");
    BOOST_TEST(r.status == 200u);
    const Locations locs = locationsOf(r.body);
    BOOST_TEST(locs.size() == 42u);
    BOOST_REQUIRE(locs.count("KPRG") == 1u);
    BOOST_TEST(near(locs.at("KPRG"), 39.699097, -87.670212));
    BOOST_TEST(locs.count("KCVG") == 1u);
    for (const auto& kv : locs)
        BOOST_TEST(skycued::fisb::airportStyleId(kv.first), kv.first);

    BOOST_REQUIRE(!c.waitFor("\"type\":\"fisb_location\",\"id\":\"KPRG\"").empty());
    std::size_t items = 0;
    for (const auto& msg : c.all()) {
        const pt::ptree m = parseJson(msg);
        if (m.get<std::string>("type") != "events")
            continue;
        for (const auto& item : m.get_child("items")) {
            if (item.second.get<std::string>("type") != "fisb_location")
                continue;
            ++items;
            BOOST_TEST(item.second.size() == 4u);
            BOOST_TEST(locs.count(item.second.get<std::string>("id")) == 1u);
            if (item.second.get<std::string>("id") == "KPRG")
                BOOST_TEST(std::abs(item.second.get<double>("lat") - 39.699097) < 1e-3);
        }
    }
    BOOST_TEST(items >= 42u);
}

// stop() saves fisb-locations.json; a fresh Engine on the same file serves it before any uplink.
BOOST_AUTO_TEST_CASE(locations_persisted_and_reloaded)
{
    TempDir dir;
    const std::string file = dir.path + "/fisb-locations.json";
    std::size_t uplinks = 0;
    const FisbWebOptions o = tg30Options(dir, uplinks);
    Locations first;
    {
        FisbWebFx f(o);
        BOOST_REQUIRE(f.waitReplayed(uplinks, 0));
        first = locationsOf(get(f.port, "/api/fisb/locations").body);
        f.t.call([&] { f.engine->stop(); });
    }
    BOOST_REQUIRE(exists(file));
    BOOST_TEST(savedIds(file).size() == 42u);
    BOOST_TEST(first.size() == 42u);

    FisbWebOptions o2;
    o2.replay = false;
    o2.locations_dir = dir.path;
    FisbWebFx g(o2);
    const Locations again = locationsOf(get(g.port, "/api/fisb/locations").body);
    BOOST_REQUIRE_EQUAL(again.size(), first.size());
    for (const auto& kv : first) {
        BOOST_REQUIRE(again.count(kv.first) == 1u);
        BOOST_TEST(near(again.at(kv.first), kv.second.first, kv.second.second), kv.first);
    }
}

// LOCATIONS_FILE rows fill in what was not learned; learned positions win; the operator rows are not saved.
BOOST_AUTO_TEST_CASE(operator_file_merged_under_learned)
{
    TempDir dir;
    std::size_t uplinks = 0;
    FisbWebOptions o = tg30Options(dir, uplinks);
    o.locations_csv = dir.path + "/operator.csv";
    writeFile(o.locations_csv, "KPRG,1,1\nZZZ9,2,2\n");
    FisbWebFx f(o);
    BOOST_REQUIRE(f.waitReplayed(uplinks, 0));
    const Locations locs = locationsOf(get(f.port, "/api/fisb/locations").body);
    BOOST_REQUIRE(locs.count("KPRG") == 1u);
    BOOST_TEST(near(locs.at("KPRG"), 39.699097, -87.670212));
    BOOST_REQUIRE(locs.count("ZZZ9") == 1u);
    BOOST_TEST(near(locs.at("ZZZ9"), 2, 2));
    f.t.call([&] { f.engine->stop(); });
    const std::set<std::string> saved = savedIds(dir.path + "/fisb-locations.json");
    BOOST_TEST(saved.count("KPRG") == 1u);
    BOOST_TEST(saved.count("ZZZ9") == 0u);
}

// A learned file that does not parse is set aside, not fatal.
BOOST_AUTO_TEST_CASE(malformed_locations_file_renamed_rejected)
{
    TempDir dir;
    const std::string file = dir.path + "/fisb-locations.json";
    writeFile(file, "{");
    FisbWebOptions o;
    o.replay = false;
    o.locations_dir = dir.path;
    StderrCapture err(dir.path + "/stderr.txt");
    std::unique_ptr<FisbWebFx> f;
    BOOST_REQUIRE_NO_THROW(f = std::make_unique<FisbWebFx>(o));
    err.restore();
    BOOST_TEST(readAll(file + ".rejected") == "{");
    BOOST_TEST(!exists(file));
    std::size_t errors = 0;
    std::istringstream lines(err.text());
    for (std::string line; std::getline(lines, line);)
        if (has(line, " ERROR ")) {
            ++errors;
            BOOST_TEST(has(line, file), line);
        }
    BOOST_TEST(errors == 1u, err.text());
    BOOST_TEST(get(f->port, "/api/fisb/locations").body == "{}");
}

// LOCATIONS_FILE that cannot be read stops the start.
BOOST_AUTO_TEST_CASE(unreadable_operator_file_throws)
{
    TempDir dir;
    FisbWebOptions o;
    o.replay = false;
    o.start_engine = false;
    FisbWebFx f(o);
    std::string what;
    try {
        f.t.call([&] { f.engine->loadLocations(dir.path + "/fisb-locations.json", dir.path + "/missing.csv"); });
    } catch (const std::runtime_error& e) {
        what = e.what();
    }
    BOOST_TEST(has(what, "missing.csv"), what);
}

// Text with quotes, backslashes and control characters comes back intact
// through the text route's JSON.
BOOST_AUTO_TEST_CASE(text_route_escapes)
{
    skycued::fisb::FisbSnapshot snap;
    skycued::fisb::TextRecord r;
    r.kind = skycued::fisb::TextRecord::metar;
    r.station = "KXYZ";
    r.text = "METAR KXYZ 241200Z \"quoted\" back\\slash\ttab\nline \x01 end";
    r.issued = std::chrono::system_clock::time_point(std::chrono::seconds(1422100800));
    r.expires = r.issued + std::chrono::hours(2);
    snap.metar.push_back(r);
    const auto res = skycued::webapi::fisbTextRoute({{"station", "kx"}}, snap);
    BOOST_REQUIRE_EQUAL(res.status, 200u);
    const pt::ptree t = parseJson(res.body);
    BOOST_REQUIRE_EQUAL(count(t.get_child("records")), 1u);
    BOOST_TEST(t.get_child("records").front().second.get<std::string>("text") == r.text);
}
