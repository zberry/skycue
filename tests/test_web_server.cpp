#define BOOST_TEST_MODULE skycued_web_server
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"

#include "runtime_config.hpp"
#include "time_util.hpp"
#include "web_api.hpp"
#include "web_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <signal.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <map>
#include <regex>

extern char** environ;

using namespace enginetest;
using skycued::RuntimeConfig;
using skycued::WebConfig;
using skycued::WebServer;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace pt = boost::property_tree;

namespace {

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool exists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

pt::ptree parseJson(const std::string& text)
{
    pt::ptree tree;
    std::istringstream in(text);
    pt::read_json(in, tree);
    return tree;
}

template <class Pred> bool pollUntil(Pred p, std::chrono::milliseconds timeout = kWait)
{
    const auto deadline = steady::now() + timeout;
    while (steady::now() < deadline) {
        if (p())
            return true;
        std::this_thread::sleep_for(20ms);
    }
    return p();
}

const char* const kIndex = "<!doctype html><title>skycue test</title>\n";

// ---- a minimal synchronous HTTP client ----

struct Reply {
    unsigned status = 0;
    std::string body, content_type, cache_control, allow;
};

void setRecvTimeout(tcp::socket& s, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

Reply exchange(tcp::socket& s, http::verb verb, const std::string& target, const std::string& body = "",
               bool keep_alive = false)
{
    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1");
    if (verb == http::verb::put || verb == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
    }
    req.keep_alive(keep_alive);
    req.prepare_payload();
    http::write(s, req);
    beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    parser.body_limit(64u << 20);
    http::read(s, buf, parser);
    const auto& res = parser.get();
    Reply r;
    r.status = res.result_int();
    r.body = res.body();
    r.content_type = std::string(res[http::field::content_type]);
    r.cache_control = std::string(res[http::field::cache_control]);
    r.allow = std::string(res[http::field::allow]);
    return r;
}

Reply request(uint16_t port, http::verb verb, const std::string& target, const std::string& body = "")
{
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    setRecvTimeout(s, 10);
    return exchange(s, verb, target, body);
}

Reply get(uint16_t port, const std::string& target) { return request(port, http::verb::get, target); }

// A request with exactly these headers (besides Host, unless given, and
// Content-Length).
Reply requestWith(uint16_t port, http::verb verb, const std::string& target, const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>& headers)
{
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    setRecvTimeout(s, 10);
    http::request<http::string_body> req{verb, target, 11};
    bool host = false;
    for (const auto& h : headers) {
        req.set(h.first, h.second);
        host = host || h.first == "Host";
    }
    if (!host)
        req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    req.body() = body;
    req.keep_alive(false);
    req.prepare_payload();
    http::write(s, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(s, buf, res);
    Reply r;
    r.status = res.result_int();
    r.body = res.body();
    r.content_type = std::string(res[http::field::content_type]);
    return r;
}

// ---- a minimal WebSocket client: a thread collects every text message ----

struct WsClient {
    asio::io_context io;
    websocket::stream<tcp::socket> ws{io};
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> msgs;
    bool closed = false;

    // `read` false: never reads after the handshake (a stalled client), with
    // `rcvbuf` bytes of socket receive buffer when not 0.
    explicit WsClient(uint16_t port, bool read = true, int rcvbuf = 0)
    {
        auto& s = ws.next_layer();
        s.open(tcp::v4());
        if (rcvbuf)
            s.set_option(asio::socket_base::receive_buffer_size(rcvbuf));
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        ws.handshake("127.0.0.1", "/ws");
        if (read)
            reader = std::thread([this] { loop(); });
    }
    ~WsClient() { close(); }

    void close()
    {
        ::shutdown(ws.next_layer().native_handle(), SHUT_RDWR);
        if (reader.joinable())
            reader.join();
        boost::system::error_code ec;
        ws.next_layer().close(ec);
    }

    void loop()
    {
        for (;;) {
            beast::flat_buffer b;
            boost::system::error_code ec;
            ws.read(b, ec);
            std::lock_guard<std::mutex> lk(m);
            if (ec) {
                closed = true;
                cv.notify_all();
                return;
            }
            msgs.emplace_back(asio::buffers_begin(b.data()), asio::buffers_end(b.data()));
            cv.notify_all();
        }
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, p);
    }
    // Waits for a message containing every needle; returns it ("" on timeout).
    std::string waitForMessage(const std::vector<std::string>& needles, std::chrono::milliseconds timeout = kWait)
    {
        std::string found;
        waitFor(
            [&] {
                for (const auto& msg : msgs) {
                    if (std::all_of(needles.begin(), needles.end(), [&](const std::string& n) { return has(msg, n); })) {
                        found = msg;
                        return true;
                    }
                }
                return false;
            },
            timeout);
        return found;
    }
};

// ---- the fixture: conf + www in a temp dir, Engine, RuntimeConfig, WebServer ----

const char* const kConfHead = "RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\nRECEIVER_ALT_M=100\nANTENNA_HEIGHT_M=5\n"
                              "TRACK_TIMEOUT_S=5\n";

struct WebOptions {
    bool replay = false;  // the readsb fixture at 20x, else a FakeSource
    bool www = true;      // create www/ with index.html
    size_t max_ws_queue_bytes = 2u << 20;
    std::string extra_conf;
    std::chrono::milliseconds ping_interval{15000};
};

struct WebFx {
    TempDir tmp;
    std::string conf, rec, www;
    FakeSource* src = nullptr;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<RuntimeConfig> rc;
    std::unique_ptr<WebServer> web;
    uint16_t port = 0;

    explicit WebFx(const WebOptions& o = WebOptions())
    {
        conf = tmp.path + "/skycue.conf";
        rec = tmp.path + "/rec";
        www = tmp.path + "/www";
        ::mkdir(rec.c_str(), 0755);
        if (o.www) {
            ::mkdir(www.c_str(), 0755);
            writeFile(www + "/index.html", kIndex);
        }
        writeFile(conf, std::string(kConfHead) + o.extra_conf);
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), o.replay ? replay(20) : fakeSource(src), 10s,
                                        rec);
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
            WebConfig wc;
            wc.bind = "127.0.0.1";
            wc.port = 0;
            wc.www_dir = www;
            wc.max_ws_queue_bytes = o.max_ws_queue_bytes;
            wc.ping_interval = o.ping_interval;
            web = std::make_unique<WebServer>(fx->t.io, *fx->engine, *rc, wc);
            web->start();
            port = web->localPort();
        });
    }
    ~WebFx()
    {
        try {
            fx->t.call([&] {
                web.reset();
                rc.reset();
            });
        } catch (...) {
        }
    }
    void emit(const TrackObservation& o)
    {
        fx->t.call([&] { src->emit(o); });
    }
    uint32_t webClients() { return fx->stats().web_clients; }
    std::string sent()
    {
        fx->t.call([&] { fx->sent->flush(); });
        return streamText(rec, "sent");
    }
};

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

// ---- running the skycued binary ----

pid_t spawnSkycued(const std::vector<std::string>& args, const std::string& log)
{
    std::vector<std::string> all{SKYCUED_EXE};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all)
        argv.push_back(&a[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0)
        throw std::runtime_error("posix_spawn failed");
    return pid;
}

// The exit status, or -1 when it did not exit in time (it is then killed).
int waitExit(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = steady::now() + timeout;
    int status = 0;
    while (steady::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        std::this_thread::sleep_for(20ms);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    return -1;
}

uint16_t freePort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

}  // namespace

BOOST_AUTO_TEST_CASE(static_index_served)
{
    WebFx f;
    writeFile(f.www + "/app.js", "console.log(1);\n");
    writeFile(f.www + "/style.css", "body{}\n");
    ::mkdir((f.www + "/vendor").c_str(), 0755);
    writeFile(f.www + "/vendor/lib.js", "var lib;\n");

    // Two requests on one keep-alive connection.
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), f.port));
    setRecvTimeout(s, 10);
    Reply r = exchange(s, http::verb::get, "/", "", true);
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == kIndex);
    BOOST_TEST(has(r.content_type, "text/html"));
    BOOST_TEST(r.cache_control == "no-cache");
    r = exchange(s, http::verb::get, "/index.html?v=2", "", true);
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == kIndex);

    r = get(f.port, "/app.js");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/javascript");
    BOOST_TEST(r.cache_control == "no-cache");
    BOOST_TEST(get(f.port, "/style.css").content_type == "text/css");
    r = get(f.port, "/vendor/lib.js");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "var lib;\n");
    BOOST_TEST(r.cache_control == "max-age=86400");

    r = get(f.port, "/missing.js");
    BOOST_TEST(r.status == 404u);
    BOOST_TEST(has(r.content_type, "text/plain"));
    BOOST_TEST(get(f.port, "/vendor").status == 404u);  // a directory
    r = request(f.port, http::verb::delete_, "/index.html");
    BOOST_TEST(r.status == 405u);
    BOOST_TEST(r.allow == "GET, HEAD");

    // HEAD: the headers of GET, no body.
    {
        tcp::socket h(io);
        h.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), f.port));
        setRecvTimeout(h, 10);
        http::request<http::empty_body> req{http::verb::head, "/app.js", 11};
        req.set(http::field::host, "127.0.0.1");
        http::write(h, req);
        beast::flat_buffer buf;
        http::response_parser<http::empty_body> p;
        p.skip(true);
        http::read(h, buf, p);
        BOOST_TEST(p.get().result_int() == 200u);
        BOOST_TEST(p.get()[http::field::content_length] == "16");
    }

    // Tiles: none, then a meta.json, then zoom directories only.
    r = get(f.port, "/api/tiles-available");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"tiles\":false}");
    ::mkdir((f.www + "/tiles").c_str(), 0755);
    // As scripts/make-tiles.sh writes it: the bbox array and strings are ignored.
    writeFile(f.www + "/tiles/meta.json", "{\"minZoom\":5,\"maxZoom\":9,\"bbox\":[-10.5,35,30,60],"
                                          "\"source\":\"Natural Earth 1:10m\",\"generated\":\"2026-11-01T00:00:00Z\"}");
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":true,\"minZoom\":5,\"maxZoom\":9}");
    ::remove((f.www + "/tiles/meta.json").c_str());
    for (const char* z : {"/tiles/4", "/tiles/7", "/tiles/6"})
        ::mkdir((f.www + z).c_str(), 0755);
    ::mkdir((f.www + "/tiles/7/37").c_str(), 0755);
    writeFile(f.www + "/tiles/7/37/49.png", "PNG");
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":true,\"minZoom\":4,\"maxZoom\":7}");
    r = get(f.port, "/tiles/7/37/49.png");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "image/png");
    BOOST_TEST(r.cache_control == "max-age=86400");
}

BOOST_AUTO_TEST_CASE(static_file_over_8mib_500)
{
    WebFx f;
    writeFile(f.www + "/max.bin", std::string(8u << 20, 'x'));
    writeFile(f.www + "/big.bin", std::string((8u << 20) + 1, 'x'));
    Reply r = get(f.port, "/max.bin");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body.size() == (8u << 20));
    r = get(f.port, "/big.bin");
    BOOST_TEST(r.status == 500u);
    BOOST_TEST(r.body == "file too large to serve\n");
    BOOST_TEST(r.cache_control == "no-store");
    BOOST_TEST(get(f.port, "/index.html").status == 200u);  // still serving
}

BOOST_AUTO_TEST_CASE(missing_www_dir_api_still_works)
{
    WebOptions o;
    o.www = false;
    WebFx f(o);
    BOOST_TEST(get(f.port, "/").status == 404u);
    BOOST_TEST(get(f.port, "/api/state").status == 200u);
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":false}");
}

BOOST_AUTO_TEST_CASE(path_traversal_rejected)
{
    WebFx f;
    writeFile(f.tmp.path + "/secret.txt", "SECRET\n");
    for (const char* target : {"/../secret.txt", "/../etc/passwd", "/%2e%2e/secret.txt", "/%2E%2E/etc/passwd",
                               "/%2e%2e%2fsecret.txt", "//etc/passwd", "/a/../index.html", "/./index.html",
                               "/index.html%00.js", "/..", "/x/..", "/%", "/%zz", "index.html",
                               "/vendor/..%2f..%2fsecret.txt", "/a\\..\\secret.txt"}) {
        const Reply r = get(f.port, target);
        BOOST_TEST(r.status == 400u, target);
        BOOST_TEST(!has(r.body, "SECRET"), target);
    }
    BOOST_TEST(get(f.port, "/index.html").status == 200u);  // still serving
    BOOST_TEST(get(f.port, "/%69ndex.html").body == kIndex);  // percent-decoding of a harmless byte
}

BOOST_AUTO_TEST_CASE(api_state_shape)
{
    WebFx f;
    const TrackId a{0xa1b2c3, AddressType::icao}, c{0xc0ffee, AddressType::icao};
    f.fx->t.call([&] {
        f.src->connection(true);
        auto oa = nearbyObservation(a.address, 0);
        oa.callsign = std::string("TEST1");
        oa.squawk = std::string("1200");
        oa.rssi = -12.5;
        oa.category = std::string("A3");
        oa.nic = 8;
        f.src->emit(oa);
        auto oc = nearbyObservation(c.address, 0);
        oc.lat.reset();
        oc.lon.reset();
        oc.position_time.reset();
        f.src->emit(oc);
    });
    f.fx->t.call([&] {
        skycued::Overlay d;
        d.allow_add = {"N123AB"};
        f.rc->applyUi(d, "test");
    });

    const Reply r = get(f.port, "/api/state");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/json");
    BOOST_TEST(r.cache_control == "no-store");
    const pt::ptree s = parseJson(r.body);
    const std::regex iso("\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z");
    BOOST_TEST(std::regex_match(s.get<std::string>("now"), iso));
    BOOST_TEST(s.get<double>("receiver.lat") == 38.9);
    BOOST_TEST(s.get<double>("receiver.lon") == -77.0);
    BOOST_TEST(s.get<double>("receiver.alt_m") == 100.0);
    BOOST_TEST(s.get<double>("receiver.antenna_m") == 5.0);
    BOOST_TEST(s.get<double>("filters.max_range_nm") == 150.0);
    BOOST_TEST(s.get<double>("filters.min_elevation_deg") == 0.0);
    BOOST_TEST(s.get<bool>("filters.overlay_active") == true);
    BOOST_TEST(s.get_child("lists.allow").size() == 1u);
    BOOST_TEST(s.get_child("lists.allow").front().second.data() == "N123AB");
    BOOST_TEST(s.get_child("lists.block").empty());
    BOOST_TEST(s.get<int>("trail_seconds") == 120);
    BOOST_TEST(s.get<int>("stats.observations") == 2);
    BOOST_TEST(s.get<int>("stats.tracks_total") == 2);
    BOOST_TEST(s.get<int>("stats.tracks_forwarded") == 1);
    BOOST_TEST(s.get<bool>("stats.readsb_connected") == true);
    BOOST_TEST(s.count("stats") == 1u);
    for (const char* key : {"drops_blocked", "drops_out_of_range", "drops_below_elevation", "drops_no_position"})
        BOOST_TEST(s.get_child("stats").count(key) == 1u, key);

    const auto& tracks = s.get_child("tracks");
    BOOST_TEST(tracks.size() == 2u);
    bool saw_a = false, saw_c = false;
    for (const auto& kv : tracks) {
        const pt::ptree& t = kv.second;
        BOOST_TEST(t.get<std::string>("addr_type") == "icao");
        BOOST_TEST(t.get<std::string>("source") == "adsb_1090");
        BOOST_TEST(std::regex_match(t.get<std::string>("first_seen"), iso));
        BOOST_TEST(std::regex_match(t.get<std::string>("last_seen"), iso));
        BOOST_TEST(t.get<std::string>("input", "") == "readsb");  // the input that emitted them
        if (t.get<std::string>("hex") == "a1b2c3") {
            saw_a = true;
            BOOST_TEST(t.get<double>("lat") == 38.95);
            BOOST_TEST(t.get<double>("lon") == -77.0);
            BOOST_TEST(t.get<int>("alt_baro_ft") == 5000);
            BOOST_TEST(t.count("alt_geom_ft") == 0u);
            BOOST_TEST(t.get<double>("gs_kt") == 200.0);
            BOOST_TEST(t.get<double>("track_deg") == 90.0);
            BOOST_TEST(t.get<std::string>("callsign") == "TEST1");
            BOOST_TEST(t.get<std::string>("squawk") == "1200");
            BOOST_TEST(t.get<int>("nic") == 8);
            BOOST_TEST(t.get<bool>("on_ground") == false);
            BOOST_TEST(t.get<double>("range_nm") > 2.9);
            BOOST_TEST(t.get<double>("range_nm") < 3.1);
            BOOST_TEST(t.count("elevation_deg") == 1u);
            BOOST_TEST(t.get<std::string>("decision") == "send");
            BOOST_TEST(t.get<bool>("forwarded") == true);
            BOOST_TEST(t.get<std::string>("category") == "A3");
            BOOST_TEST(t.get<double>("rssi") == -12.5);
        } else {
            saw_c = true;
            BOOST_TEST(t.get<std::string>("hex") == "c0ffee");
            BOOST_TEST(t.count("lat") == 0u);
            BOOST_TEST(t.count("range_nm") == 0u);  // NaN: omitted
            BOOST_TEST(t.count("rssi") == 0u);
            BOOST_TEST(t.get<std::string>("decision") == "no_position");
            BOOST_TEST(t.get<bool>("forwarded") == false);
        }
    }
    BOOST_TEST(saw_a);
    BOOST_TEST(saw_c);
    // Numbers and booleans are never quoted.
    BOOST_TEST(has(r.body, "\"alt_baro_ft\":5000"));
    BOOST_TEST(has(r.body, "\"forwarded\":true"));
    BOOST_TEST(has(r.body, "\"lat\":38.9,"));
    BOOST_TEST(!has(r.body, "null"));

    // The other GET routes.
    Reply x = get(f.port, "/api/stats");
    BOOST_TEST(x.status == 200u);
    BOOST_TEST(parseJson(x.body).get<int>("tracks_total") == 2);
    x = get(f.port, "/api/filters");
    BOOST_TEST(x.body == "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":true}");
    x = get(f.port, "/api/lists");
    BOOST_TEST(x.body == "{\"allow\":[\"N123AB\"],\"block\":[],\"overlay_active\":true}");

    // Errors.
    x = get(f.port, "/api/nope");
    BOOST_TEST(x.status == 404u);
    BOOST_TEST(x.content_type == "application/json");
    BOOST_TEST(parseJson(x.body).count("error") == 1u);
    x = request(f.port, http::verb::delete_, "/api/state");
    BOOST_TEST(x.status == 405u);
    BOOST_TEST(x.allow == "GET");
    BOOST_TEST(parseJson(x.body).count("error") == 1u);
    BOOST_TEST(request(f.port, http::verb::post, "/api/filters", "{}").allow == "GET, PUT");
    BOOST_TEST(get(f.port, "/api/lists/allow").allow == "POST");
    BOOST_TEST(get(f.port, "/api/filters/save-baseline").status == 405u);
    BOOST_TEST(get(f.port, "/ws").status == 400u);  // not an upgrade
}

BOOST_AUTO_TEST_CASE(drop_json_fields)
{
    skycued::EngineEvent ev;
    ev.kind = skycued::EventKind::drop;
    ev.id = TrackId{0xa1b2c3, AddressType::icao};
    ev.reason = DropReason::filtered;
    ev.decision = skycued::Decision::out_of_range;
    ev.callsign = std::string("UAL1423");
    ev.time = skycued::parseIso8601Ms("2020-10-30T09:00:17.000Z").value();
    BOOST_TEST(skycued::webapi::eventJson(ev) == "{\"type\":\"drop\",\"hex\":\"a1b2c3\",\"addr_type\":\"icao\",\"reason\":\"filtered\","
                                                 "\"decision\":\"out_of_range\",\"callsign\":\"UAL1423\",\"time\":\"2020-10-30T09:00:17.000Z\"}");
    ev.reason = DropReason::timeout;
    ev.decision.reset();
    ev.callsign.reset();
    BOOST_TEST(skycued::webapi::eventJson(ev) == "{\"type\":\"drop\",\"hex\":\"a1b2c3\",\"addr_type\":\"icao\",\"reason\":\"timeout\",\"time\":\"2020-10-30T09:00:17.000Z\"}");
}

BOOST_AUTO_TEST_CASE(track_json_input_present_when_known)
{
    skycued::TrackSnapshot ts;
    ts.track.id = skycue::TrackId{0xa1b2c3, skycue::AddressType::icao};
    ts.input = "readsb";
    BOOST_TEST(parseJson(skycued::webapi::trackJson(ts)).get<std::string>("input") == "readsb");
    ts.input.clear();
    BOOST_TEST(parseJson(skycued::webapi::trackJson(ts)).count("input") == 0u);
}

// Review Focus 1, end to end: a PUT from the UI drops the tracks it excludes
// at once, after the filter_change record.
BOOST_AUTO_TEST_CASE(put_filters_applies_and_drops)
{
    WebOptions o;
    o.replay = true;
    WebFx f(o);
    Events ev;
    skycue::Client client(clientConfig(f.fx->port()));
    ev.attach(client);
    client.start();
    BOOST_TEST(ev.waitFor([&] { return ev.ids().size() >= 6; }, 8s));
    size_t filtered_before = 0;
    {
        std::lock_guard<std::mutex> lk(ev.m);
        filtered_before = ev.dropsWith(DropReason::filtered);
    }

    const Reply r = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"max_range_nm\":20,\"min_elevation_deg\":0,\"overlay_active\":true}");
    // Applied before the reply: nothing forwarded is now outside 20 nm.
    f.fx->t.call([&] {
        const auto snap = f.fx->engine->snapshot();
        BOOST_TEST(snap.filters.max_range_nm == 20.0);
        for (const auto& t : snap.tracks)
            if (t.forwarded)
                BOOST_TEST(t.range_nm <= 20.0);
    });
    BOOST_TEST(ev.waitFor([&] { return ev.dropsWith(DropReason::filtered) > filtered_before; }));
    BOOST_TEST(exists(f.tmp.path + "/skycue.runtime.json"));
    BOOST_TEST(has(readAll(f.tmp.path + "/skycue.runtime.json"), "\"max_range_nm\": 20"));
    BOOST_TEST(get(f.port, "/api/filters").body == r.body);

    // The sent stream: the "ui:" filter_change precedes the drops it caused.
    const auto sent = lines(f.sent());
    size_t change_at = sent.size(), filtered_after = 0;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (has(sent[i], "\"event\":\"filter_change\"") && has(sent[i], "ui: max_range_nm=20"))
            change_at = i;
        if (i > change_at && has(sent[i], "\"event\":\"drop\"") && has(sent[i], "filtered"))
            ++filtered_after;
    }
    BOOST_TEST(change_at < sent.size());
    BOOST_TEST(filtered_after >= 1u);

    // Save as baseline: into skycue.conf, overlay gone.
    const Reply s = request(f.port, http::verb::post, "/api/filters/save-baseline");
    BOOST_TEST(s.status == 200u);
    BOOST_TEST(s.body == "{\"ok\":true,\"unsaved\":\"\"}");
    BOOST_TEST(has(readAll(f.conf), "MAX_RANGE_NM=20"));
    BOOST_TEST(!exists(f.tmp.path + "/skycue.runtime.json"));
    BOOST_TEST(get(f.port, "/api/filters").body == "{\"max_range_nm\":20,\"min_elevation_deg\":0,\"overlay_active\":false}");
    client.stop();
}

BOOST_AUTO_TEST_CASE(post_list_add_drops_now)
{
    WebFx f(WebOptions{false, true, 2u << 20, "MIN_INTERVAL_S=5\nMAX_INTERVAL_S=60\n"});
    Events ev;
    skycue::Client client(clientConfig(f.fx->port()));
    ev.attach(client);
    client.start();
    std::this_thread::sleep_for(300ms);
    const TrackId a{0x3a3a3a, AddressType::icao}, b{0x3b3b3b, AddressType::icao};
    f.fx->t.call([&] {
        f.src->connection(true);
        f.src->emit(nearbyObservation(a.address, 0));
        f.src->emit(nearbyObservation(b.address, 0.1));
    });
    BOOST_TEST(ev.waitFor([&] { return ev.ids().size() == 2; }));

    Reply r = request(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"3A3A3A\"]}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"allow\":[],\"block\":[\"3A3A3A\"],\"overlay_active\":true}");
    f.fx->t.call([&] {
        for (const auto& t : f.fx->engine->snapshot().tracks) {
            if (t.track.id == a) {
                BOOST_TEST(!t.forwarded);
                BOOST_TEST((t.decision == skycued::Decision::blocked));
            } else {
                BOOST_TEST(t.forwarded);
            }
        }
    });
    BOOST_TEST(ev.waitFor([&] { return ev.dropped(a, DropReason::filtered); }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(!ev.dropped(b, DropReason::filtered));
    }
    BOOST_TEST(has(readAll(f.tmp.path + "/skycue.runtime.json"), "3A3A3A"));

    // Removing it sends the track again at once (MIN_INTERVAL_S is 5 s).
    size_t updates_a = 0;
    {
        std::lock_guard<std::mutex> lk(ev.m);
        updates_a = ev.updatesFor(a);
    }
    r = request(f.port, http::verb::post, "/api/lists/block", "{\"remove\":[\"3a3a3a\"]}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"allow\":[],\"block\":[],\"overlay_active\":false}");
    BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(a) > updates_a; }, 2s));

    // A bad entry names itself and changes nothing.
    r = request(f.port, http::verb::post, "/api/lists/allow", "{\"add\":[\"N123AB\",\"no good!\"]}");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(parseJson(r.body).get<std::string>("error"), "no good!"));
    BOOST_TEST(get(f.port, "/api/lists").body == "{\"allow\":[],\"block\":[],\"overlay_active\":false}");
    BOOST_TEST(request(f.port, http::verb::post, "/api/lists/other", "{\"add\":[]}").status == 404u);
    client.stop();
}

BOOST_AUTO_TEST_CASE(ws_snapshot_then_events)
{
    WebFx f;
    const TrackId a{0xabc001, AddressType::icao}, b{0xabc002, AddressType::icao};
    f.fx->t.call([&] {
        f.src->connection(true);
        f.src->emit(nearbyObservation(a.address, 0));
    });
    {
        WsClient c(f.port);
        BOOST_TEST(c.waitFor([&] { return !c.msgs.empty(); }));
        {
            std::lock_guard<std::mutex> lk(c.m);
            const std::string& first = c.msgs.front();
            BOOST_TEST(first.compare(0, 18, "{\"type\":\"snapshot\"") == 0);
            const pt::ptree s = parseJson(first);
            BOOST_TEST(s.get_child("tracks").size() == 1u);
            BOOST_TEST(s.get_child("tracks").front().second.get<std::string>("hex") == "abc001");
            BOOST_TEST(s.count("receiver") == 1u);
            BOOST_TEST(s.count("filters") == 1u);
            BOOST_TEST(s.count("lists") == 1u);
            BOOST_TEST(s.count("stats") == 1u);
        }
        BOOST_TEST(pollUntil([&] { return f.webClients() == 1u; }));

        f.emit(nearbyObservation(b.address, 0.05));
        const std::string m = c.waitForMessage({"\"type\":\"events\"", "\"type\":\"update\"", "\"hex\":\"abc002\""});
        BOOST_TEST(!m.empty());
        if (!m.empty()) {
            const pt::ptree e = parseJson(m);
            bool found = false;
            for (const auto& kv : e.get_child("items"))
                if (kv.second.get<std::string>("type") == "update" && kv.second.get<std::string>("hex") == "abc002") {
                    found = true;
                    BOOST_TEST(kv.second.get<std::string>("decision") == "send");
                    BOOST_TEST(kv.second.get<bool>("forwarded") == true);
                }
            BOOST_TEST(found);
        }
        BOOST_TEST(!c.waitForMessage({"\"type\":\"stats\"", "\"web_clients\":1"}, 3s).empty());

        // A filter change: the filters item, then the drop it causes.
        BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":4}").status == 200u);
        const std::string fm = c.waitForMessage({"\"type\":\"filters\"", "\"max_range_nm\":4"});
        BOOST_TEST(!fm.empty());
        const auto filters_at = fm.find("\"type\":\"filters\"");
        const auto drop_at = fm.find("\"type\":\"drop\",\"hex\":\"abc002\",\"addr_type\":\"icao\",\"reason\":\"filtered\"");
        BOOST_TEST(drop_at != std::string::npos);
        BOOST_TEST(filters_at < drop_at);
        BOOST_TEST(request(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"abc001\"]}").status == 200u);
        BOOST_TEST(!c.waitForMessage({"\"type\":\"lists\"", "\"block\":[\"abc001\"]"}).empty());

        // Text from the client is ignored.
        c.ws.write(asio::buffer(std::string("hello")));
        std::this_thread::sleep_for(100ms);
        BOOST_TEST(f.webClients() == 1u);
        std::lock_guard<std::mutex> lk(c.m);
        BOOST_TEST(!c.closed);
    }
    BOOST_TEST(pollUntil([&] { return f.webClients() == 0u; }));
}

BOOST_AUTO_TEST_CASE(ws_inputs_item_on_change)
{
    WebFx f;
    WsClient c(f.port);
    BOOST_TEST(c.waitFor([&] { return !c.msgs.empty(); }));
    {
        std::lock_guard<std::mutex> lk(c.m);
        const pt::ptree s = parseJson(c.msgs.front());
        BOOST_TEST(s.get_child("inputs").size() == 1u);
        BOOST_TEST(s.get_child("inputs").front().second.get<std::string>("id") == "readsb");
    }
    BOOST_TEST(pollUntil([&] { return f.webClients() == 1u; }));

    asio::io_context uio;
    asio::ip::udp::socket probe(uio, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const uint16_t uport = probe.local_endpoint().port();
    probe.close();
    const auto r = request(f.port, http::verb::post, "/api/inputs",
                           R"({"id":"ps-ws","type":"pingstation_json","bind":"127.0.0.1","port":)" +
                               std::to_string(uport) + "}");
    BOOST_TEST(r.status == 201u, r.body);
    const std::string m = c.waitForMessage({"\"type\":\"inputs\"", "\"id\":\"ps-ws\""});
    BOOST_REQUIRE(!m.empty());
    const pt::ptree e = parseJson(m);
    for (const auto& kv : e.get_child("items"))
        if (kv.second.get<std::string>("type") == "inputs") {
            const auto& list = kv.second.get_child("inputs");
            BOOST_TEST(list.size() == 2u);
            BOOST_TEST(list.back().second.get<std::string>("id") == "ps-ws");
            BOOST_TEST(list.back().second.count("state") == 1u);
        }

    // Several changes in one turn of the event loop: one inputs item.
    f.fx->t.call([&] {
        f.fx->engine->inputs().setEnabled("ps-ws", false);
        f.fx->engine->inputs().setEnabled("ps-ws", true);
        f.fx->engine->inputs().setEnabled("readsb", false);
    });
    const std::string one = c.waitForMessage({"\"type\":\"inputs\"", "\"id\":\"readsb\",\"name\":\"\",\"type\":\"readsb\",\"enabled\":false"});
    BOOST_REQUIRE(!one.empty());
    BOOST_TEST(countOf(one, "\"type\":\"inputs\"") == 1u, one);

    // DELETE over HTTP: 204 without a body, then an inputs item without it.
    size_t before = 0;
    {
        std::lock_guard<std::mutex> lk(c.m);
        before = c.msgs.size();
    }
    const auto d = requestWith(f.port, http::verb::delete_, "/api/inputs/ps-ws", "", {});
    BOOST_TEST(d.status == 204u);
    BOOST_TEST(d.body.empty());
    BOOST_TEST(c.waitFor([&] {
        for (size_t i = before; i < c.msgs.size(); ++i)
            if (has(c.msgs[i], "\"type\":\"inputs\"") && !has(c.msgs[i], "ps-ws"))
                return true;
        return false;
    }));
}

BOOST_AUTO_TEST_CASE(ws_observed_events_for_filtered_tracks)
{
    WebFx f;
    const TrackId blocked{0xb10c00, AddressType::icao}, sent{0x5e5e00, AddressType::icao};
    BOOST_TEST(request(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"b10c00\"]}").status == 200u);
    WsClient c(f.port);
    BOOST_TEST(c.waitFor([&] { return !c.msgs.empty(); }));
    f.fx->t.call([&] { f.src->connection(true); });
    for (int i = 0; i < 3; ++i) {
        f.fx->t.call([&] {
            f.src->emit(nearbyObservation(blocked.address, 0.01 * i));
            f.src->emit(nearbyObservation(sent.address, 0.01 * i));
            auto np = nearbyObservation(0xa0a0a0, 0);  // no position: no observed item
            np.lat.reset();
            np.lon.reset();
            np.position_time.reset();
            f.src->emit(np);
        });
        std::this_thread::sleep_for(50ms);
    }
    // Every positioned observation of the blocked track gives an observed item.
    auto count = [&](const std::string& needle) {
        std::lock_guard<std::mutex> lk(c.m);
        size_t n = 0;
        for (const auto& m : c.msgs)
            n += countOf(m, needle);
        return n;
    };
    BOOST_TEST(pollUntil([&] {
        return count("{\"type\":\"observed\",\"hex\":\"b10c00\"") == 3u &&
               count("{\"type\":\"observed\",\"hex\":\"5e5e00\"") == 3u;
    }));
    BOOST_TEST(count("{\"type\":\"update\",\"hex\":\"b10c00\"") == 0u);  // never sent
    BOOST_TEST(count("{\"type\":\"update\",\"hex\":\"5e5e00\"") >= 1u);
    BOOST_TEST(count("\"type\":\"observed\",\"hex\":\"a0a0a0\"") == 0u);
    const std::string m = c.waitForMessage({"{\"type\":\"observed\",\"hex\":\"b10c00\""});
    const pt::ptree e = parseJson(m);
    for (const auto& kv : e.get_child("items"))
        if (kv.second.get<std::string>("type") == "observed" && kv.second.get<std::string>("hex") == "b10c00") {
            BOOST_TEST(kv.second.get<std::string>("decision") == "blocked");
            BOOST_TEST(kv.second.get<bool>("forwarded") == false);
            BOOST_TEST(kv.second.count("lat") == 1u);
            BOOST_TEST(kv.second.count("range_nm") == 1u);
        }
}

BOOST_AUTO_TEST_CASE(put_filters_null_clears_overlay)
{
    WebFx f(WebOptions{false, true, 2u << 20, "MAX_RANGE_NM=120\nMIN_ELEVATION_DEG=1\n"});
    Reply r = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":30,\"min_elevation_deg\":3}");
    BOOST_TEST(r.body == "{\"max_range_nm\":30,\"min_elevation_deg\":3,\"overlay_active\":true}");
    r = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":null}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"max_range_nm\":120,\"min_elevation_deg\":3,\"overlay_active\":true}");
    BOOST_TEST(f.fx->t.call([&] { return f.fx->engine->snapshot().filters.max_range_nm; }) == 120.0);
    const std::string overlay = readAll(f.tmp.path + "/skycue.runtime.json");
    BOOST_TEST(!has(overlay, "max_range_nm"));
    BOOST_TEST(has(overlay, "\"min_elevation_deg\": 3"));
    r = request(f.port, http::verb::put, "/api/filters", "{\"min_elevation_deg\":null}");
    BOOST_TEST(r.body == "{\"max_range_nm\":120,\"min_elevation_deg\":1,\"overlay_active\":false}");
    BOOST_TEST(!exists(f.tmp.path + "/skycue.runtime.json"));  // empty overlay: deleted
    // Clearing what is not set is no change; the baseline conf is untouched.
    BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":null}").status == 200u);
    BOOST_TEST(has(readAll(f.conf), "MAX_RANGE_NM=120"));
    const std::string sent = f.sent();
    BOOST_TEST(has(sent, "ui: max_range_nm=120 min_elevation_deg=3"));
    BOOST_TEST(has(sent, "ui: max_range_nm=120 min_elevation_deg=1"));
}

BOOST_AUTO_TEST_CASE(ws_slow_client_closed)
{
    WebOptions o;
    o.max_ws_queue_bytes = 256 * 1024;
    WebFx f(o);
    f.fx->t.call([&] { f.src->connection(true); });
    WsClient good(f.port);
    WsClient slow(f.port, false, 4096);
    BOOST_TEST(pollUntil([&] { return f.webClients() == 2u; }));

    // Distinct new tracks, 25 per io turn (one events message of ~15 KB),
    // paced so that a client that reads keeps up, until the stalled client's
    // queue (behind its full socket buffers) passes the limit.
    uint32_t next = 0x200000;
    int rounds = 0;
    for (; rounds < 5000 && f.webClients() == 2u; ++rounds) {
        f.fx->t.call([&] {
            for (int i = 0; i < 25; ++i)
                f.src->emit(nearbyObservation(next++, 1e-6 * i));
        });
        std::this_thread::sleep_for(1ms);
    }
    BOOST_TEST(pollUntil([&] { return f.webClients() == 1u; }));
    BOOST_TEST_MESSAGE("stalled client closed after " << rounds << " batches");

    // The other client is unaffected and still gets events.
    f.emit(nearbyObservation(0x7abcde, 0.02));
    BOOST_TEST(!good.waitForMessage({"\"hex\":\"7abcde\""}, 10s).empty());
    {
        std::lock_guard<std::mutex> lk(good.m);
        BOOST_TEST(!good.closed);
    }
    // The stalled one finds its connection closed once it reads.
    boost::system::error_code ec;
    for (int i = 0; i < 1000000 && !ec; ++i) {
        beast::flat_buffer b;
        slow.ws.read(b, ec);
    }
    BOOST_TEST(static_cast<bool>(ec));  // not ec.failed(): Boost 1.66 lacks it
    good.close();
    BOOST_TEST(pollUntil([&] { return f.webClients() == 0u; }));
}

BOOST_AUTO_TEST_CASE(ws_ping_timeout_closes_silent_client)
{
    WebOptions o;
    o.ping_interval = 150ms;
    WebFx f(o);
    WsClient reading(f.port);            // its reads answer the pings
    WsClient silent(f.port, false);      // never reads: never answers
    BOOST_TEST(pollUntil([&] { return f.webClients() == 2u; }));
    BOOST_TEST(pollUntil([&] { return f.webClients() == 1u; }, 3s));
    std::this_thread::sleep_for(600ms);  // several more ping rounds
    BOOST_TEST(f.webClients() == 1u);
    std::lock_guard<std::mutex> lk(reading.m);
    BOOST_TEST(!reading.closed);
}

BOOST_AUTO_TEST_CASE(bad_json_400)
{
    WebFx f;
    for (const char* body : {"not json", "[1,2]", "{\"max_range_nm\":\"abc\"}", "{\"max_range_nm\":-5}",
                             "{\"max_range_nm\":0}", "{\"min_elevation_deg\":95}", "{\"max_range\":10}",
                             "{\"max_range_nm\":{\"x\":1}}", "{\"max_range_nm\":true}", ""}) {
        const Reply r = request(f.port, http::verb::put, "/api/filters", body);
        BOOST_TEST(r.status == 400u, body);
        BOOST_TEST(r.content_type == "application/json", body);
        BOOST_TEST(!parseJson(r.body).get<std::string>("error").empty(), body);
    }
    BOOST_TEST(get(f.port, "/api/filters").body == "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":false}");
    BOOST_TEST(!exists(f.tmp.path + "/skycue.runtime.json"));
    for (const char* body : {"{\"add\":\"3a3a3a\"}", "{\"add\":[[1]]}", "{\"del\":[\"3a3a3a\"]}", "nope"}) {
        const Reply r = request(f.port, http::verb::post, "/api/lists/block", body);
        BOOST_TEST(r.status == 400u, body);
        BOOST_TEST(parseJson(r.body).count("error") == 1u, body);
    }
    // A body over 64 KiB.
    const Reply big = request(f.port, http::verb::put, "/api/filters", std::string(70 * 1024, ' '));
    BOOST_TEST(big.status == 413u);
    // Still serving.
    BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"min_elevation_deg\":2.5}").body ==
               "{\"max_range_nm\":150,\"min_elevation_deg\":2.5,\"overlay_active\":true}");
}

BOOST_AUTO_TEST_CASE(save_baseline_broadcasts_filters_lists)
{
    WebFx f;
    WsClient c(f.port);
    BOOST_TEST(c.waitFor([&] { return !c.msgs.empty(); }));
    BOOST_TEST(pollUntil([&] { return f.webClients() == 1u; }));

    // An overlay value equal to the baseline's: no Engine change, but the
    // overlay is no longer empty, and every page must show that.
    BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":150}").status == 200u);
    BOOST_TEST(!c.waitForMessage({"\"type\":\"filters\",\"max_range_nm\":150,\"min_elevation_deg\":0,"
                                  "\"overlay_active\":true"})
                    .empty());
    BOOST_TEST(!c.waitForMessage({"\"type\":\"lists\",\"allow\":[],\"block\":[],\"overlay_active\":true"}).empty());

    // Save as baseline: nothing effective changes; overlay_active goes false.
    const Reply s = request(f.port, http::verb::post, "/api/filters/save-baseline", "{}");
    BOOST_TEST(s.status == 200u);
    const std::string m = c.waitForMessage({"\"type\":\"filters\",\"max_range_nm\":150,\"min_elevation_deg\":0,"
                                            "\"overlay_active\":false"});
    BOOST_TEST(!m.empty());
    BOOST_TEST(has(m, "\"type\":\"lists\",\"allow\":[],\"block\":[],\"overlay_active\":false"));
    BOOST_TEST(m.compare(0, 17, "{\"type\":\"events\",") == 0);
    // No filter_change was recorded for either: the Engine saw no change.
    BOOST_TEST(countOf(f.sent(), "\"event\":\"filter_change\"") == 1u);  // startup only

    // A change the Engine applies still gives one filters item, not two.
    BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":90}").status == 200u);
    const std::string fm = c.waitForMessage({"\"type\":\"filters\",\"max_range_nm\":90"});
    BOOST_TEST(countOf(fm, "\"type\":\"filters\"") == 1u);
}

using Headers = std::vector<std::pair<std::string, std::string>>;

BOOST_AUTO_TEST_CASE(put_without_json_content_type_415)
{
    WebFx f;
    const std::string body = "{\"max_range_nm\":20}";
    for (const Headers& h : {Headers{}, Headers{{"Content-Type", "text/plain;charset=UTF-8"}},
                             Headers{{"Content-Type", "application/x-www-form-urlencoded"}},
                             Headers{{"Content-Type", "multipart/form-data; boundary=x"}}}) {
        const Reply r = requestWith(f.port, http::verb::put, "/api/filters", body, h);
        BOOST_TEST(r.status == 415u);
        BOOST_TEST(r.body == "{\"error\":\"content-type must be application/json\"}");
    }
    for (const char* target : {"/api/lists/block", "/api/filters/save-baseline"})
        BOOST_TEST(requestWith(f.port, http::verb::post, target, "{\"add\":[\"abc001\"]}",
                               {{"Content-Type", "text/plain"}})
                       .status == 415u,
                   target);
    // Nothing changed.
    BOOST_TEST(get(f.port, "/api/filters").body == "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":false}");
    BOOST_TEST(get(f.port, "/api/lists").body == "{\"allow\":[],\"block\":[],\"overlay_active\":false}");
    BOOST_TEST(!exists(f.tmp.path + "/skycue.runtime.json"));
    // With a charset parameter (and in any case) it is accepted; GET needs none.
    BOOST_TEST(requestWith(f.port, http::verb::put, "/api/filters", body,
                           {{"Content-Type", "Application/JSON; charset=utf-8"}})
                   .status == 200u);
    BOOST_TEST(requestWith(f.port, http::verb::get, "/api/filters", "", {}).status == 200u);
    // A wrong method is still a 405, not a 415.
    BOOST_TEST(requestWith(f.port, http::verb::post, "/api/filters", body, {}).status == 405u);
}

BOOST_AUTO_TEST_CASE(post_with_foreign_origin_403)
{
    WebFx f;
    const std::string json = "application/json";
    const std::string host = "127.0.0.1:" + std::to_string(f.port);
    for (const std::string& origin : {std::string("http://evil.example"), std::string("http://evil.example:") +
                                      std::to_string(f.port), std::string("http://127.0.0.1:1"),
                                      std::string("null"), std::string("http://127.0.0.1:") + std::to_string(f.port) +
                                      ".evil.example"}) {
        Reply r = requestWith(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"abc001\"]}",
                              {{"Content-Type", json}, {"Origin", origin}});
        BOOST_TEST(r.status == 403u, origin);
        BOOST_TEST(r.body == "{\"error\":\"cross-origin request rejected\"}", origin);
        r = requestWith(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}",
                        {{"Content-Type", json}, {"Origin", origin}});
        BOOST_TEST(r.status == 403u, origin);
    }
    BOOST_TEST(requestWith(f.port, http::verb::post, "/api/filters/save-baseline", "{}",
                           {{"Content-Type", json}, {"Origin", "http://evil.example"}})
                   .status == 403u);
    // An Origin with no Host to compare with is rejected too.
    BOOST_TEST(requestWith(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"abc001\"]}",
                           {{"Content-Type", json}, {"Origin", "http://" + host}, {"Host", ""}})
                   .status == 403u);
    BOOST_TEST(get(f.port, "/api/lists").body == "{\"allow\":[],\"block\":[],\"overlay_active\":false}");
    BOOST_TEST(get(f.port, "/api/filters").body == "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":false}");
    // GET is never refused for its Origin.
    BOOST_TEST(requestWith(f.port, http::verb::get, "/api/state", "", {{"Origin", "http://evil.example"}}).status ==
               200u);
}

BOOST_AUTO_TEST_CASE(same_origin_ok)
{
    WebFx f;
    const std::string json = "application/json";
    const std::string port = std::to_string(f.port);
    // The page's own origin, as a browser sends it (case of the host ignored).
    Reply r = requestWith(f.port, http::verb::post, "/api/lists/block", "{\"add\":[\"abc001\"]}",
                          {{"Content-Type", json}, {"Origin", "http://127.0.0.1:" + port}});
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"allow\":[],\"block\":[\"abc001\"],\"overlay_active\":true}");
    r = requestWith(f.port, http::verb::post, "/api/lists/block", "{\"remove\":[\"abc001\"]}",
                    {{"Content-Type", json}, {"Origin", "http://LocalHost:" + port}, {"Host", "localhost:" + port}});
    BOOST_TEST(r.status == 200u);
    // A default port may be left out of either.
    r = requestWith(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":30}",
                    {{"Content-Type", json}, {"Origin", "http://skycue.example"}, {"Host", "skycue.example:80"}});
    BOOST_TEST(r.status == 200u);
    // No Origin (curl, the skycue tools): allowed.
    r = requestWith(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":null}", {{"Content-Type", json}});
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":false}");
    r = requestWith(f.port, http::verb::post, "/api/filters/save-baseline", "{}",
                    {{"Content-Type", json}, {"Origin", "http://[::1]:" + port}, {"Host", "[::1]:" + port}});
    BOOST_TEST(r.status == 200u);
}

BOOST_AUTO_TEST_CASE(query_params_parsed)
{
    using skycued::webapi::queryParams;
    using Map = std::map<std::string, std::string>;
    BOOST_TEST((queryParams("") == Map{}));
    BOOST_TEST((queryParams("a=1&b=two") == Map{{"a", "1"}, {"b", "two"}}));
    BOOST_TEST((queryParams("q=x%20y+z&%41=%2f&flag&&e=") == Map{{"q", "x y z"}, {"A", "/"}, {"flag", ""}, {"e", ""}}));
    BOOST_TEST((queryParams("a=1&a=2") == Map{{"a", "2"}}));
    BOOST_TEST((queryParams("bad=%zz%4") == Map{{"bad", "%zz%4"}}));
    BOOST_TEST((queryParams("eq=a=b") == Map{{"eq", "a=b"}}));
    // The server hands the query over and still routes by the path alone.
    WebFx f;
    BOOST_TEST(get(f.port, "/api/filters?x=1&y=%20").body ==
               "{\"max_range_nm\":150,\"min_elevation_deg\":0,\"overlay_active\":false}");
}

BOOST_AUTO_TEST_CASE(web_port_zero_disabled)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.conf", log = tmp.path + "/log";
    writeFile(conf, std::string(kConfHead) + "WEB_PORT=0\nCUE_TCP_PORT=0\n");
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture, "--replay-loop"}, log);
    BOOST_TEST(pollUntil([&] { return has(readAll(log), "web UI disabled"); }));
    ::kill(pid, SIGTERM);
    BOOST_TEST(waitExit(pid, 5s) == 0);
    BOOST_TEST(!has(readAll(log), "web: listening"));
}

BOOST_AUTO_TEST_CASE(web_bind_failure_exits_1)
{
    TempDir tmp;
    TestServer busy;  // holds the port
    const std::string conf = tmp.path + "/skycue.conf", log = tmp.path + "/log";
    writeFile(conf, std::string(kConfHead) + "WEB_PORT=" + std::to_string(busy.port()) + "\nCUE_TCP_PORT=0\n");
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture}, log);
    BOOST_TEST(waitExit(pid, 5s) == 1);
    BOOST_TEST(has(readAll(log), "web: cannot listen on 127.0.0.1:" + std::to_string(busy.port())));
}

BOOST_AUTO_TEST_CASE(main_serves_with_overrides)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.conf", log = tmp.path + "/log", www = tmp.path + "/www";
    ::mkdir(www.c_str(), 0755);
    writeFile(www + "/index.html", kIndex);
    writeFile(conf, std::string(kConfHead) + "WEB_PORT=0\nWEB_BIND=0.0.0.0\nCUE_TCP_PORT=0\n");
    const uint16_t port = freePort();
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture, "--replay-loop", "--www-dir", www,
                                    "--web-bind", "127.0.0.1", "--web-port", std::to_string(port)},
                                   log);
    BOOST_TEST(pollUntil([&] { return has(readAll(log), "web: listening on http://127.0.0.1:" + std::to_string(port)); }));
    BOOST_TEST(get(port, "/").body == kIndex);
    BOOST_TEST(get(port, "/api/state").status == 200u);
    ::kill(pid, SIGTERM);
    BOOST_TEST(waitExit(pid, 5s) == 0);
}

BOOST_AUTO_TEST_CASE(filters_history_route)
{
    WebFx f;
    BOOST_TEST(request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}").status == 200u);

    const Reply r = get(f.port, "/api/filters/history");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/json");
    const pt::ptree h = parseJson(r.body);
    const auto& changes = h.get_child("changes");
    BOOST_REQUIRE(changes.size() == 2u);
    auto it = changes.begin();
    const pt::ptree& first = it->second;
    const pt::ptree& second = (++it)->second;
    BOOST_TEST(first.get<std::string>("origin") == "startup");
    BOOST_TEST(first.get<double>("filters.max_range_nm") == 150.0);
    BOOST_TEST(second.get<std::string>("origin") == "ui");
    BOOST_TEST(second.get<double>("filters.max_range_nm") == 20.0);
    BOOST_TEST(second.get<double>("filters.min_elevation_deg") == 0.0);
    const std::regex iso("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$");
    BOOST_TEST(std::regex_match(first.get<std::string>("time"), iso));
    BOOST_TEST(std::regex_match(second.get<std::string>("time"), iso));

    const Reply p = request(f.port, http::verb::post, "/api/filters/history");
    BOOST_TEST(p.status == 405u);
    BOOST_TEST(p.allow == "GET");
}

BOOST_AUTO_TEST_CASE(cues_clients_route)
{
    WebFx f;
    std::atomic<bool> hello{false};
    skycue::Client client(clientConfig(f.fx->port()));
    client.onFrame([&](const skycue::Frame& fr) {
        if (fr.type == skycue::FrameType::hello)
            hello = true;
    });
    client.start();
    BOOST_REQUIRE(pollUntil([&] { return hello.load(); }));

    const Reply r = get(f.port, "/api/cues/clients");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/json");
    BOOST_TEST(has(r.body, "\"udp\":[]"));
    const pt::ptree j = parseJson(r.body);
    const auto& tcp = j.get_child("tcp");
    BOOST_REQUIRE(tcp.size() == 1u);
    const pt::ptree& c = tcp.begin()->second;
    BOOST_TEST(c.get<std::string>("direction") == "accepted");
    BOOST_TEST(has(c.get<std::string>("endpoint"), "127.0.0.1:"));
    BOOST_TEST(c.get<uint64_t>("bytes_sent") > 0u);
    const std::regex iso("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$");
    BOOST_TEST(std::regex_match(c.get<std::string>("connected_since"), iso));

    BOOST_TEST(request(f.port, http::verb::post, "/api/cues/clients").status == 405u);
    client.stop();
}
