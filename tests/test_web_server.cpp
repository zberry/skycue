#define BOOST_TEST_MODULE skycued_web_server
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
#include "decode_test_support.hpp"
#include "engine_test_support.hpp"
#include "inputs/beast_raw_input.hpp"

#include "icao_class.hpp"
#include "inputs/inputs_file.hpp"
#include "runtime_config.hpp"
#include "stats_store.hpp"
#include "time_util.hpp"
#include "web_api.hpp"
#include "web_server.hpp"
#include "web_stats.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
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

    // Sends one short masked text frame by writing it to the socket itself.
    // The websocket stream is not safe for a write on this thread while the
    // reader thread is inside ws.read() (TSan saw them race on its state);
    // the socket is.
    void sendText(const std::string& text)
    {
        BOOST_REQUIRE(text.size() < 126);
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        std::string frame;
        frame += static_cast<char>(0x81);  // FIN, text
        frame += static_cast<char>(0x80 | text.size());
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < text.size(); ++i)
            frame += static_cast<char>(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
        asio::write(ws.next_layer(), asio::buffer(frame));
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

using ConfMap = std::map<std::string, std::string>;  // dotted key -> TOML literal

// This suite's skycue.toml: the receiver and track timeout every test uses, plus `extra`
// (which overrides them where the keys coincide).
std::string headConf(const ConfMap& extra = {})
{
    ConfMap kv{{"receiver.lat", "38.9"},
               {"receiver.lon", "-77.0"},
               {"receiver.alt_m", "100"},
               {"receiver.antenna_height_m", "5"},
               {"cues.track_timeout_s", "5"}};
    for (const auto& [k, v] : extra)
        kv[k] = v;
    return skycue_test::tomlConf(kv);
}

// A TOML string literal of a path or text without quotes or backslashes.
std::string str(const std::string& text) { return "\"" + text + "\""; }

struct WebOptions {
    bool replay = false;  // the readsb fixture at 20x, else a FakeSource
    bool www = true;      // create www/ with index.html
    size_t max_ws_queue_bytes = 2u << 20;
    ConfMap extra_conf;
    std::chrono::milliseconds ping_interval{15000};
    bool map_dir = false;  // set wc.map_dir to maps/ (created unless map_dir_missing)
    bool map_dir_missing = false;
    bool decode_raw = false;  // the one input is a decoding beast_raw (UDP, decode_port) instead of the FakeSource
    uint16_t decode_port = 0;
};

struct WebFx {
    TempDir tmp;
    std::string conf, rec, www, maps;
    FakeSource* src = nullptr;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<RuntimeConfig> rc;
    std::unique_ptr<WebServer> web;
    uint16_t port = 0;

    explicit WebFx(const WebOptions& o = WebOptions())
    {
        conf = tmp.path + "/skycue.toml";
        rec = tmp.path + "/rec";
        www = tmp.path + "/www";
        maps = tmp.path + "/maps";
        ::mkdir(rec.c_str(), 0755);
        if (o.map_dir && !o.map_dir_missing)
            ::mkdir(maps.c_str(), 0755);
        if (o.www) {
            ::mkdir(www.c_str(), 0755);
            writeFile(www + "/index.html", kIndex);
        }
        writeFile(conf, headConf(o.extra_conf));
        if (o.decode_raw) {
            const uint16_t decode_port = o.decode_port;
            fx = std::make_unique<EngineFx>(
                skycued::parseConfigFile(conf),
                std::function<std::unique_ptr<skycued::InputManager>(asio::io_context&)>([decode_port](asio::io_context& io) {
                    skycued::InputManagerConfig mc;
                    mc.decode_raw = true;
                    mc.receiver_ref = std::make_pair(38.9, -77.0);
                    auto inputs = std::make_unique<skycued::InputManager>(io, mc);
                    skycued::InputDef def;
                    def.id = "readsb";
                    def.type = skycued::InputType::beast_raw;
                    def.transport = skycued::InputTransport::udp;
                    def.bind = "127.0.0.1";
                    def.port = decode_port;
                    def.format = "beast";
                    skycued::RawDecodeOptions decode;
                    decode.enabled = true;
                    decode.reference = mc.receiver_ref;
                    inputs->addBuiltin(std::make_unique<skycued::BeastRawInput>(io, def, skycued::RecordOptions{}, decode));
                    inputs->load();
                    return inputs;
                }),
                10s, rec);
        } else {
            fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), o.replay ? replay(20) : fakeSource(src), 10s,
                                            rec);
        }
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
            WebConfig wc;
            wc.bind = "127.0.0.1";
            wc.port = 0;
            wc.www_dir = www;
            if (o.map_dir)
                wc.map_dir = maps;
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

// The probe runs in a forked child (forked before any WebFx thread exists), so a server stuck in
// open() on its io thread can never hang this binary: the parent waits at most `limit`, then
// SIGKILLs the child. Returns the child's exit code, or -1 on timeout (H8). The parent never
// builds a WebFx itself, so its teardown can never block.
int inChild(const std::function<int()>& body, std::chrono::milliseconds limit)
{
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error("fork failed");
    if (pid == 0) {
        int rc = 3;
        try {
            rc = body();
        } catch (...) {
            rc = 3;
        }
        ::_exit(rc);
    }
    return waitExit(pid, limit);
}

// skycued --config `conf` `args` --web-bind 127.0.0.1 --web-port <free port>, logging to
// `log`.<attempt>. freeTcpPort() releases the port before skycued binds it, so a parallel test
// can take it in between: on "cannot listen", try another port. Returns the pid and the web
// port once "web: listening" is logged; fails the test otherwise.
std::pair<pid_t, uint16_t> spawnServing(const std::string& conf, std::vector<std::string> args, const std::string& log)
{
    args.insert(args.begin(), {"--config", conf});
    for (int attempt = 0; attempt < 5; ++attempt) {
        const uint16_t port = freeTcpPort();
        const std::string try_log = log + "." + std::to_string(attempt);
        std::vector<std::string> all = args;
        all.insert(all.end(), {"--web-bind", "127.0.0.1", "--web-port", std::to_string(port)});
        const pid_t p = spawnSkycued(all, try_log);
        const std::string listening = "web: listening on http://127.0.0.1:" + std::to_string(port);
        const std::string busy = "web: cannot listen on 127.0.0.1:" + std::to_string(port);
        if (!pollUntil([&] {
                const std::string text = readAll(try_log);
                return has(text, listening) || has(text, busy);
            })) {
            ::kill(p, SIGKILL);
            waitExit(p, 1s);
            BOOST_FAIL("skycued neither listened nor failed to listen on port " << port << ":\n" << readAll(try_log));
        }
        if (has(readAll(try_log), listening))
            return {p, port};
        BOOST_TEST(waitExit(p, 5s) == 1);
    }
    BOOST_FAIL("skycued found no free web port in 5 attempts");
    return {-1, 0};
}

// The "keys" entry of GET /api/config for `key`.
pt::ptree configKey(const pt::ptree& config, const std::string& key)
{
    for (const auto& k : config.get_child("keys"))
        if (k.second.get<std::string>("key") == key)
            return k.second;
    BOOST_FAIL("no key " << key << " in /api/config");
    return {};
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
    BOOST_TEST(r.body == "{\"tiles\":false,\"overlays\":false}");
    ::mkdir((f.www + "/tiles").c_str(), 0755);
    // As scripts/maps/make-tiles.sh writes it: the bbox array and strings are ignored.
    writeFile(f.www + "/tiles/meta.json", "{\"minZoom\":5,\"maxZoom\":9,\"bbox\":[-10.5,35,30,60],"
                                          "\"source\":\"Natural Earth 1:10m\",\"generated\":\"2026-11-01T00:00:00Z\"}");
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":true,\"minZoom\":5,\"maxZoom\":9,\"overlays\":false}");
    ::remove((f.www + "/tiles/meta.json").c_str());
    for (const char* z : {"/tiles/4", "/tiles/7", "/tiles/6"})
        ::mkdir((f.www + z).c_str(), 0755);
    ::mkdir((f.www + "/tiles/7/37").c_str(), 0755);
    writeFile(f.www + "/tiles/7/37/49.png", "PNG");
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":true,\"minZoom\":4,\"maxZoom\":7,\"overlays\":false}");
    r = get(f.port, "/tiles/7/37/49.png");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "image/png");
    BOOST_TEST(r.cache_control == "max-age=86400");
}

BOOST_AUTO_TEST_CASE(tiles_available_overlays_flag)
{
    WebFx f;
    const auto ends = [&](const char* tail) {
        const std::string b = get(f.port, "/api/tiles-available").body;
        const std::string t = tail;
        return b.size() >= t.size() && b.compare(b.size() - t.size(), t.size(), t) == 0;
    };
    BOOST_TEST(ends(",\"overlays\":false}"));
    ::mkdir((f.www + "/overlays").c_str(), 0755);
    writeFile(f.www + "/overlays/meta.json", "{\"files\":{}}");
    BOOST_TEST(ends(",\"overlays\":true}"));
    writeFile(f.www + "/overlays/meta.json", "{");
    BOOST_TEST(ends(",\"overlays\":false}"));
    ::remove((f.www + "/overlays/meta.json").c_str());
    BOOST_TEST(ends(",\"overlays\":false}"));
}

BOOST_AUTO_TEST_CASE(tiles_meta_fifo_is_ignored)
{
    const int rc = inChild([] {
        WebFx f;
        ::mkdir((f.www + "/tiles").c_str(), 0755);
        ::mkdir((f.www + "/overlays").c_str(), 0755);
        if (::mkfifo((f.www + "/tiles/meta.json").c_str(), 0644) != 0 ||
            ::mkfifo((f.www + "/overlays/meta.json").c_str(), 0644) != 0)
            return 2;
        const Reply r = get(f.port, "/api/tiles-available");  // get() sets a receive timeout
        return r.status == 200u && r.body == "{\"tiles\":false,\"overlays\":false}" ? 0 : 1;
    }, slow(3s));
    BOOST_TEST(rc == 0);  // -1: the server blocked on the FIFO
}

BOOST_AUTO_TEST_CASE(static_fifo_is_404)
{
    const int rc = inChild([] {
        WebFx f;
        if (::mkfifo((f.www + "/pipe.js").c_str(), 0644) != 0)
            return 2;
        return get(f.port, "/pipe.js").status == 404u ? 0 : 1;
    }, slow(3s));
    BOOST_TEST(rc == 0);  // -1: the server blocked on the FIFO
}

BOOST_AUTO_TEST_CASE(tiles_meta_is_not_cached)
{
    BOOST_TEST(std::string(skycued::webapi::cacheControl("/tiles/meta.json")) == "no-cache");
    BOOST_TEST(std::string(skycued::webapi::cacheControl("/overlays/meta.json")) == "no-cache");
    BOOST_TEST(std::string(skycued::webapi::cacheControl("/tiles/6/18/24.png")) == "max-age=86400");
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
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":false,\"overlays\":false}");
}

BOOST_AUTO_TEST_CASE(map_dir_serves_tiles_and_overlays)
{
    WebOptions o;
    o.map_dir = true;
    WebFx f(o);
    for (const char* d : {"/tiles", "/tiles/7", "/tiles/7/37", "/overlays"})
        ::mkdir((f.maps + d).c_str(), 0755);
    writeFile(f.maps + "/tiles/7/37/49.png", "PNG");
    writeFile(f.maps + "/tiles/meta.json", "{\"minZoom\":7,\"maxZoom\":7}");
    writeFile(f.maps + "/overlays/meta.json", "{}");
    Reply r = get(f.port, "/tiles/7/37/49.png");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "PNG");
    BOOST_TEST(r.content_type == "image/png");
    BOOST_TEST(r.cache_control == "max-age=86400");
    r = get(f.port, "/overlays/meta.json");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{}");
    BOOST_TEST(r.cache_control == "no-cache");
    BOOST_TEST(get(f.port, "/api/tiles-available").body ==
               "{\"tiles\":true,\"minZoom\":7,\"maxZoom\":7,\"overlays\":true}");
    // Confined to the map directory as the rest is to www: no way out.
    writeFile(f.tmp.path + "/secret.txt", "secret");
    for (const char* t : {"/tiles/../../secret.txt", "/tiles/%2e%2e/%2e%2e/secret.txt", "/overlays/..%2f..%2fsecret.txt",
                          "/tiles/7/..\\..\\secret.txt"}) {
        r = get(f.port, t);
        BOOST_TEST(r.status == 400u, t);
        BOOST_TEST(r.body != "secret", t);
    }
    BOOST_TEST(get(f.port, "/").status == 200u);  // the rest still comes from www
}

BOOST_AUTO_TEST_CASE(map_dir_ignores_www_tiles)
{
    WebOptions o;
    o.map_dir = true;
    WebFx f(o);
    for (const char* d : {"/tiles", "/tiles/7", "/tiles/7/37", "/overlays"})
        ::mkdir((f.www + d).c_str(), 0755);
    writeFile(f.www + "/tiles/7/37/49.png", "PNG");
    writeFile(f.www + "/tiles/meta.json", "{\"minZoom\":7,\"maxZoom\":7}");
    writeFile(f.www + "/overlays/meta.json", "{}");
    BOOST_TEST(get(f.port, "/tiles/7/37/49.png").status == 404u);
    BOOST_TEST(get(f.port, "/overlays/meta.json").status == 404u);
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":false,\"overlays\":false}");
    BOOST_TEST(get(f.port, "/").status == 200u);
}

// Redirects stderr (the log) to a file until text() is called.
struct StderrCapture {
    std::string path;
    int saved = -1;
    explicit StderrCapture(std::string p) : path(std::move(p))
    {
        std::fflush(stderr);
        saved = ::dup(2);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::dup2(fd, 2);
        ::close(fd);
    }
    std::string text()
    {
        if (saved >= 0) {
            std::fflush(stderr);
            ::dup2(saved, 2);
            ::close(saved);
            saved = -1;
        }
        return readAll(path);
    }
    ~StderrCapture() { text(); }
};

BOOST_AUTO_TEST_CASE(map_dir_missing_api_still_works)
{
    WebOptions o;
    o.map_dir = true;
    o.map_dir_missing = true;
    TempDir logdir;
    StderrCapture cap(logdir.path + "/stderr.txt");
    WebFx f(o);
    const std::string log = cap.text();
    BOOST_TEST(has(log, "WARN"));
    BOOST_TEST(has(log, "paths.map_dir"));
    BOOST_TEST(has(log, "map directory " + f.maps + " (paths.map_dir) does not exist"));
    BOOST_TEST(get(f.port, "/api/tiles-available").body == "{\"tiles\":false,\"overlays\":false}");
    BOOST_TEST(get(f.port, "/api/state").status == 200u);
    BOOST_TEST(get(f.port, "/tiles/7/37/49.png").status == 404u);
    BOOST_TEST(get(f.port, "/").status == 200u);
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

namespace {
skycued::TrackSnapshot geomSnapshot()
{
    skycued::TrackSnapshot ts;
    ts.track.id = skycue::TrackId{0xa1b2c3, skycue::AddressType::icao};
    ts.track.source = skycue::Source::adsb_1090;
    ts.track.alt_baro_ft = 35000;
    return ts;
}
}  // namespace

BOOST_AUTO_TEST_CASE(track_json_reported_keys)
{
    auto ts = geomSnapshot();
    ts.track.alt_geom_ft = 35250;
    ts.geom.source = skycued::GeomSource::reported;
    ts.geom.alt_geom_ft = 35250;
    ts.geom.age_s = 12.34;
    ts.geom.geom_minus_baro_ft = 250;
    const std::string body = skycued::webapi::trackJson(ts);
    BOOST_TEST(body.find("\"alt_geom_ft\":35250,\"alt_geom_source\":\"reported\",\"alt_geom_age_s\":12.3,"
                         "\"geom_minus_baro_ft\":250,\"on_ground\"") != std::string::npos);
    BOOST_TEST(body.find("alt_geom_uncertainty_ft") == std::string::npos);
    ts.geom.age_s = 5.0;
    BOOST_TEST(skycued::webapi::trackJson(ts).find("\"alt_geom_age_s\":5.0,") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(track_json_estimated_keys)
{
    auto ts = geomSnapshot();
    ts.track.alt_geom_ft = 35250;
    ts.track.alt_geom_uncertainty_ft = 150;
    ts.geom.source = skycued::GeomSource::estimated;
    ts.geom.alt_geom_ft = 35250;
    ts.geom.uncertainty_ft = 150;
    ts.geom.age_s = 42.0;
    ts.geom.geom_minus_baro_ft = 250;
    const std::string body = skycued::webapi::trackJson(ts);
    BOOST_TEST(body.find("\"alt_geom_ft\":35250,\"alt_geom_source\":\"estimated\",\"alt_geom_uncertainty_ft\":150,"
                         "\"alt_geom_age_s\":42.0,\"geom_minus_baro_ft\":250,\"on_ground\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(track_json_no_geom_has_no_new_keys)
{
    auto ts = geomSnapshot();  // a v1 consumer's fixture: no geometric altitude at all
    BOOST_TEST(skycued::webapi::trackJson(ts) ==
               "{\"hex\":\"a1b2c3\",\"addr_type\":\"icao\",\"source\":\"adsb_1090\",\"alt_baro_ft\":35000,"
               "\"on_ground\":false,\"alt_is_geom\":false,\"range_nm\":0,\"elevation_deg\":0,"
               "\"decision\":\"no_position\",\"forwarded\":false,\"first_seen\":\"1970-01-01T00:00:00.000Z\","
               "\"last_seen\":\"1970-01-01T00:00:00.000Z\",\"rssi\":0}");
    ts.track.alt_geom_ft = 35250;  // a value with no GeomResult (source none) adds no key either
    const std::string body = skycued::webapi::trackJson(ts);
    for (const char* k : {"alt_geom_source", "alt_geom_uncertainty_ft", "alt_geom_age_s", "geom_minus_baro_ft"})
        BOOST_TEST(body.find(k) == std::string::npos, k);
    // A difference known to the track, but no geometric altitude (source none): no key either.
    ts.geom.geom_minus_baro_ft = 250;
    BOOST_TEST(skycued::webapi::trackJson(ts).find("geom_minus_baro_ft") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(stats_altitude_fit_valid_and_absent)
{
    skycued::EngineStats st;
    auto j = parseJson(skycued::webapi::statsJson(st));
    BOOST_TEST(j.get<bool>("altitude_fit.valid") == false);
    BOOST_TEST(j.get_child("altitude_fit").size() == 1u);
    st.altitude_fit.valid = true;
    st.altitude_fit.linear = true;
    st.altitude_fit.a = 120.5;
    st.altitude_fit.b = 0.0125;
    st.altitude_fit.samples = 42;
    st.altitude_fit.aircraft = 7;
    st.altitude_fit.p90_ft = 88.5;
    st.altitude_fit.age_s = 3.5;
    const std::string body = skycued::webapi::statsJson(st);
    BOOST_TEST(body.find("\"altitude_fit\":{\"valid\":true,\"linear\":true,\"a\":120.5,\"b\":0.0125,"
                         "\"samples\":42,\"aircraft\":7,\"p90_ft\":88.5,\"age_s\":3.5}") != std::string::npos);
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

    // Save as baseline: into skycue.overrides.toml, overlay gone.
    const std::string conf_before = readAll(f.conf);
    const Reply s = request(f.port, http::verb::post, "/api/filters/save-baseline");
    BOOST_TEST(s.status == 200u);
    BOOST_TEST(s.body == "{\"ok\":true,\"unsaved\":\"\"}");
    BOOST_TEST(readAll(f.conf) == conf_before);  // the main file is never written
    BOOST_TEST(has(readAll(f.tmp.path + "/skycue.overrides.toml"), "[filters]\nmax_range_nm = 20.0\n"));
    BOOST_TEST(!exists(f.tmp.path + "/skycue.runtime.json"));
    BOOST_TEST(get(f.port, "/api/filters").body == "{\"max_range_nm\":20,\"min_elevation_deg\":0,\"overlay_active\":false}");
    client.stop();
}

BOOST_AUTO_TEST_CASE(post_list_add_drops_now)
{
    WebFx f(WebOptions{false, true, 2u << 20, {{"cues.min_interval_s", "5"}, {"cues.max_interval_s", "60"}}});
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

    // Removing it sends the track again at once (cues.min_interval_s is 5 s).
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
        c.sendText("hello");
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
    WebFx f(WebOptions{false, true, 2u << 20, {{"filters.max_range_nm", "120"}, {"filters.min_elevation_deg", "1"}}});
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
    BOOST_TEST(has(readAll(f.conf), "max_range_nm = 120"));
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
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    writeFile(conf, headConf({{"web.port", "0"}, {"cues.tcp_port", "0"}}));
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture, "--replay-loop"}, log);
    BOOST_TEST(pollUntil([&] { return has(readAll(log), "web UI disabled"); }));
    ::kill(pid, SIGTERM);
    BOOST_TEST(waitExit(pid, slow(5s)) == 0);
    BOOST_TEST(!has(readAll(log), "web: listening"));
}

// A SIGTERM that arrives while skycued is still starting up (here: on the first log line it writes
// after the config is read) must be held and end in a clean, prompt exit 0, not the default action
// (status 143). Regression: the handler used to be installed only after the engine had started, so
// under load the signal could win the race.
BOOST_AUTO_TEST_CASE(sigterm_during_startup_exits_cleanly_and_promptly)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"web.port", "0"}, {"cues.tcp_port", "0"}}));
    for (int i = 0; i < 20; ++i) {
        const std::string log = tmp.path + "/log" + std::to_string(i);
        const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture, "--replay-loop"}, log);
        BOOST_TEST(pollUntil([&] { return has(readAll(log), "web UI disabled"); }));
        const auto t0 = steady::now();
        ::kill(pid, SIGTERM);
        BOOST_TEST(waitExit(pid, slow(5s)) == 0);
        const bool prompt = steady::now() - t0 < slow(2s);
        BOOST_TEST(prompt);
    }
}

BOOST_AUTO_TEST_CASE(web_bind_failure_exits_1)
{
    TempDir tmp;
    TestServer busy;  // holds the port
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    writeFile(conf, headConf({{"web.port", std::to_string(busy.port())}, {"cues.tcp_port", "0"}}));
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture}, log);
    BOOST_TEST(waitExit(pid, 5s) == 1);
    BOOST_TEST(has(readAll(log), "web: cannot listen on 127.0.0.1:" + std::to_string(busy.port())));
}

BOOST_AUTO_TEST_CASE(skycued_version_is_cmake_version)
{
    TempDir tmp;
    const std::string log = tmp.path + "/log";
    const pid_t pid = spawnSkycued({"--version"}, log);
    BOOST_TEST(waitExit(pid, 5s) == 0);
    BOOST_TEST(readAll(log) == std::string("skycued ") + SKYCUE_CMAKE_VERSION + "\n");
}

// An explicitly empty --readsb-host or --dump978-host is a usage error (exit 2,
// one line), as an empty inputs.readsb.host or inputs.dump978.host key is; also with
// --check-inputs, which would otherwise probe an empty host.
BOOST_AUTO_TEST_CASE(empty_host_flag_exits_2)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"web.port", "0"}, {"cues.tcp_port", "0"}}));
    int n = 0;
    for (const std::string flag : {"--readsb-host", "--dump978-host"}) {
        for (const bool check : {false, true}) {
            const std::string log = tmp.path + "/log" + std::to_string(n++);
            std::vector<std::string> args{"--config", conf, flag, ""};
            if (check)
                args.push_back("--check-inputs");
            const pid_t pid = spawnSkycued(args, log);
            BOOST_TEST(waitExit(pid, 5s) == 2, flag << (check ? " --check-inputs" : ""));
            BOOST_TEST(readAll(log) == "skycued: " + flag + " must not be empty\n", flag);
        }
    }
}

BOOST_AUTO_TEST_CASE(main_serves_with_overrides)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log", www = tmp.path + "/www";
    ::mkdir(www.c_str(), 0755);
    writeFile(www + "/index.html", kIndex);
    writeFile(conf, headConf({{"web.port", "0"}, {"web.bind", str("0.0.0.0")}, {"cues.tcp_port", "0"}}));
    const auto [pid, port] = spawnServing(conf, {"--replay", kFixture, "--replay-loop", "--www-dir", www}, log);
    BOOST_REQUIRE(pid > 0);
    BOOST_TEST(get(port, "/").body == kIndex);
    BOOST_TEST(get(port, "/api/state").status == 200u);
    ::kill(pid, SIGTERM);
    BOOST_TEST(waitExit(pid, 5s) == 0);
}

// A spawned skycued: killed when the test ends early (a failed BOOST_REQUIRE).
struct Spawned {
    pid_t pid;
    uint16_t port;
    explicit Spawned(std::pair<pid_t, uint16_t> p) : pid(p.first), port(p.second) {}
    ~Spawned()
    {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            waitExit(pid, 1s);
        }
    }
    int terminate()  // SIGTERM; the exit status (-1: it did not exit in time)
    {
        ::kill(pid, SIGTERM);
        const int status = waitExit(pid, 5s);
        if (status != -1)  // a daemon that did not exit is still the destructor's to kill
            pid = -1;
        return status;
    }
    pt::ptree json(const std::string& target) const { return parseJson(get(port, target).body); }
};

std::vector<std::string> inputIds(const pt::ptree& state)
{
    std::vector<std::string> ids;
    for (const auto& in : state.get_child("inputs"))
        ids.push_back(in.second.get<std::string>("id"));
    return ids;
}

BOOST_AUTO_TEST_CASE(dump978_enable_no_creates_no_uat_input)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"}, {"inputs.dump978.enable", "false"}}));
    Spawned d(spawnServing(conf, {"--no-readsb"}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(config.get<std::string>("daemon.mode") == "live");
    BOOST_TEST(config.get<bool>("daemon.dump978") == false);
    BOOST_TEST(configKey(config, "inputs.dump978.enable").get<std::string>("value") == "false");
    BOOST_TEST(configKey(config, "inputs.dump978.enable").get<std::string>("source") == "conf");
    const auto ids = inputIds(d.json("/api/state"));
    BOOST_TEST(std::count(ids.begin(), ids.end(), "dump978") == 0);
    BOOST_TEST(d.terminate() == 0);
}

BOOST_AUTO_TEST_CASE(readsb_enable_false_in_the_file_creates_no_readsb_input)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"}, {"inputs.readsb.enable", "false"}, {"inputs.dump978.enable", "false"}}));
    Spawned d(spawnServing(conf, {}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(config.get<bool>("daemon.readsb") == false);
    BOOST_TEST(configKey(config, "inputs.readsb.enable").get<std::string>("source") == "conf");
    const auto ids = inputIds(d.json("/api/state"));
    BOOST_TEST(std::count(ids.begin(), ids.end(), "readsb") == 0);
    BOOST_TEST(d.terminate() == 0);
}

BOOST_AUTO_TEST_CASE(dump978_enabled_by_default)  // the counterpart: today's behaviour
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"}}));
    Spawned d(spawnServing(conf, {"--no-readsb"}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(config.get<bool>("daemon.dump978") == true);
    BOOST_TEST(configKey(config, "inputs.dump978.enable").get<std::string>("source") == "default");
    const auto ids = inputIds(d.json("/api/state"));
    BOOST_TEST(std::count(ids.begin(), ids.end(), "dump978") == 1);
    BOOST_TEST(d.terminate() == 0);
}

BOOST_AUTO_TEST_CASE(state_dir_holds_state_files)
{
    TempDir tmp;
    const std::string etc = tmp.path + "/etc", state = tmp.path + "/state";
    BOOST_REQUIRE(::mkdir(etc.c_str(), 0755) == 0);
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    const std::string conf = etc + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"}, {"paths.state_dir", str(state)}}));
    // A replay, so the statistics change and are saved at exit.
    Spawned d(spawnServing(conf, {"--replay", kFixture, "--replay-loop"}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(config.get<std::string>("daemon.inputs_path") == state + "/skycue.inputs.json");
    BOOST_TEST(config.get<std::string>("daemon.overlay_path") == state + "/skycue.runtime.json");
    BOOST_TEST(config.get<std::string>("daemon.locations.learned_path") == state + "/fisb-locations.json");
    BOOST_TEST(configKey(config, "paths.state_dir").get<std::string>("value") == state);
    BOOST_TEST(pollUntil([&] {
        const pt::ptree state_json = d.json("/api/state");
        for (const auto& in : state_json.get_child("inputs"))
            if (in.second.get<std::string>("id") == "replay" && in.second.get<uint64_t>("state.observations") > 0)
                return true;
        return false;
    }));
    BOOST_TEST(d.terminate() == 0);
    BOOST_TEST(exists(state + "/skycue.stats.json"));
    BOOST_TEST(!exists(etc + "/skycue.stats.json"));
}

BOOST_AUTO_TEST_CASE(state_dir_missing_exits_2)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    writeFile(conf, headConf({{"web.port", "0"}, {"cues.tcp_port", "0"}, {"paths.state_dir", str(tmp.path + "/nope")}}));
    const pid_t pid = spawnSkycued({"--config", conf, "--replay", kFixture}, log);
    BOOST_TEST(waitExit(pid, 5s) == 2);
    BOOST_TEST(has(readAll(log), "skycued: paths.state_dir " + tmp.path + "/nope is not a directory"));
    // A file is not a directory either.
    writeFile(tmp.path + "/file", "x\n");
    writeFile(conf, headConf({{"web.port", "0"}, {"cues.tcp_port", "0"}, {"paths.state_dir", str(tmp.path + "/file")}}));
    const pid_t pid2 = spawnSkycued({"--config", conf, "--replay", kFixture}, log);
    BOOST_TEST(waitExit(pid2, 5s) == 2);
    BOOST_TEST(has(readAll(log), "skycued: paths.state_dir " + tmp.path + "/file is not a directory"));
}

BOOST_AUTO_TEST_CASE(readsb_host_from_config_cli_wins)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"},
                                {"inputs.readsb.host", str("192.0.2.1")},
                                {"inputs.dump978.host", str("192.0.2.2")},
                                {"inputs.dump978.enable", "true"}}));
    Spawned d(spawnServing(conf, {"--readsb-host", "127.0.0.1", "--no-uat"}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(configKey(config, "inputs.readsb.host").get<std::string>("value") == "127.0.0.1");
    BOOST_TEST(configKey(config, "inputs.readsb.host").get<std::string>("source") == "flag");
    // No --dump978-host: the key applies.
    BOOST_TEST(configKey(config, "inputs.dump978.host").get<std::string>("value") == "192.0.2.2");
    BOOST_TEST(configKey(config, "inputs.dump978.host").get<std::string>("source") == "conf");
    // --no-uat overrides inputs.dump978.enable = true.
    BOOST_TEST(configKey(config, "inputs.dump978.enable").get<std::string>("value") == "false");
    BOOST_TEST(configKey(config, "inputs.dump978.enable").get<std::string>("source") == "flag");
    BOOST_TEST(config.get<bool>("daemon.dump978") == false);
    const auto ids = inputIds(d.json("/api/state"));
    BOOST_TEST(std::count(ids.begin(), ids.end(), "dump978") == 0);
    BOOST_TEST(std::count(ids.begin(), ids.end(), "readsb") == 1);
    BOOST_TEST(d.terminate() == 0);
}

BOOST_AUTO_TEST_CASE(readsb_host_from_config_without_flag)
{
    // No --readsb-host or --dump978-host: the keys are the hosts the built-in inputs use.
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    writeFile(conf, headConf({{"cues.tcp_port", "0"}, {"inputs.readsb.host", str("192.0.2.1")}, {"inputs.dump978.host", str("192.0.2.2")}}));
    Spawned d(spawnServing(conf, {}, tmp.path + "/log"));
    const pt::ptree config = d.json("/api/config");
    BOOST_TEST(configKey(config, "inputs.readsb.host").get<std::string>("value") == "192.0.2.1");
    BOOST_TEST(configKey(config, "inputs.readsb.host").get<std::string>("source") == "conf");
    std::map<std::string, std::string> hosts;  // input id -> host
    const pt::ptree inputs = d.json("/api/inputs");
    for (const auto& in : inputs.get_child("inputs"))
        hosts[in.second.get<std::string>("id")] = in.second.get<std::string>("host", "");
    BOOST_TEST(hosts["readsb"] == "192.0.2.1");
    BOOST_TEST(hosts["dump978"] == "192.0.2.2");
    BOOST_TEST(d.terminate() == 0);
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

// ---- /api/stats/* (docs/web-api.md "Statistics") ----

namespace {

const std::regex kIsoMs("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$");

// An observation of `address` near the receiver with an airline callsign and
// a signal level; a4xxxx addresses are the United States'.
TrackObservation statsObs(uint32_t address, double lat_offset = 0)
{
    auto o = nearbyObservation(address, lat_offset);
    o.callsign = "UAL" + std::to_string(address & 0xf) + "1";
    o.rssi = -20.0;
    return o;
}

std::vector<std::string> keysOf(const pt::ptree& rows)
{
    std::vector<std::string> out;
    for (const auto& kv : rows)
        out.push_back(kv.second.get<std::string>("key"));
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(stats_summary_shape)
{
    WebFx f;
    for (uint32_t a : {0xa40001u, 0xa40002u, 0xa40003u})
        f.emit(statsObs(a, 0.01 * double(a & 0xf)));
    const Reply r = get(f.port, "/api/stats/summary");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/json");
    const pt::ptree s = parseJson(r.body);
    BOOST_TEST(std::regex_match(s.get<std::string>("since"), kIsoMs));
    BOOST_TEST(std::regex_match(s.get<std::string>("series_since"), kIsoMs));
    BOOST_TEST(std::regex_match(s.get<std::string>("now"), kIsoMs));
    BOOST_TEST(s.get<double>("receiver.lat") == 38.9);
    BOOST_TEST(s.get<double>("receiver.lon") == -77.0);
    BOOST_TEST(s.get<int>("aircraft_total") == 3);
    BOOST_TEST(s.get<int>("evicted") == 0);
    BOOST_TEST(s.get<bool>("uat_seen") == false);
    const pt::ptree& range = s.get_child("range");
    BOOST_TEST(std::regex_match(range.get<std::string>("from"), kIsoMs));
    BOOST_TEST(std::regex_match(range.get<std::string>("to"), kIsoMs));
    BOOST_TEST(range.get<std::string>("res") == "minute");
    BOOST_TEST(range.get<int>("observations") == 3);
    BOOST_TEST(range.get<int>("positions") == 3);
    BOOST_TEST(range.get<int>("new_aircraft") == 3);
    BOOST_TEST(range.get<int>("emergencies") == 0);
    BOOST_TEST(range.get<int>("aircraft") == 3);
    BOOST_TEST(range.get<double>("max_range_nm") > 0.0);
    BOOST_TEST(has(range.get<std::string>("max_range_key"), "|icao"));
    int classes = 0;
    for (const auto& kv : range.get_child("by_class"))
        classes += kv.second.get_value<int>();
    BOOST_TEST(classes == 3);
    BOOST_TEST(range.get<int>("by_class.airline") == 3);
    BOOST_TEST(range.get_child("by_class").size() == 4u);
    const pt::ptree& countries = range.get_child("by_country");
    BOOST_REQUIRE(countries.size() == 1u);
    BOOST_TEST(countries.front().second.get<std::string>("name") == "United States");
    BOOST_TEST(countries.front().second.get<int>("n") == 3);
    for (const char* h : {"range", "altitude", "speed", "vertical_rate", "elevation", "rssi"})
        BOOST_TEST(s.get_child(std::string("hist.") + h).count("counts") == 1u, h);
    BOOST_TEST(s.get_child("hist.range.counts").size() == 50u);
    BOOST_TEST(s.get<double>("hist.range.bin") == 10.0);
    BOOST_TEST(s.get<double>("hist.rssi.start") == -50.0);
    BOOST_TEST(s.get<int>("counts.by_source.adsb_1090") >= 3);
    BOOST_TEST(s.get<int>("counts.by_addr_type.icao") == 3);
    BOOST_TEST(s.get<int>("counts.by_input.readsb") == 3);
    for (const char* c : {"by_category", "by_decision", "by_squawk_emergency"})
        BOOST_TEST(s.get_child("counts").count(c) == 1u, c);
}

BOOST_AUTO_TEST_CASE(stats_range_params)
{
    WebFx f;
    f.emit(statsObs(0xa40001));
    for (const char* bad : {"/api/stats/summary?from=bad", "/api/stats/summary?to=2026-13-01T00:00:00Z",
                            "/api/stats/series?from=yesterday"}) {
        const Reply r = get(f.port, bad);
        BOOST_TEST(r.status == 400u, bad);
        BOOST_TEST(has(r.body, "\"error\":"), bad);
    }
    // to before from: an empty range.
    Reply r = get(f.port, "/api/stats/summary?from=2030-01-01T00:00:00Z&to=2020-01-01T00:00:00Z");
    BOOST_TEST(r.status == 200u);
    pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<int>("range.observations") == 0);
    BOOST_TEST(s.get<int>("range.aircraft") == 0);
    BOOST_TEST(s.get<int>("aircraft_total") == 1);  // whole history is not ranged
    // from in the future (to defaults to the data clock).
    r = get(f.port, "/api/stats/summary?from=2100-01-01T00:00:00Z");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<int>("range.aircraft") == 0);
    BOOST_TEST(s.get<int>("range.observations") == 0);
    BOOST_TEST(s.get_child("range.by_country").empty());
    // No parameters: since .. now.
    r = get(f.port, "/api/stats/summary");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<int>("range.observations") == 1);
    BOOST_TEST(s.get<int>("range.aircraft") == 1);
    BOOST_TEST(s.get<std::string>("range.from") <= s.get<std::string>("since"));
    // More than 24 h: hourly.
    r = get(f.port, "/api/stats/summary?from=2020-01-01T00:00:00Z");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("range.res") == "hour");
    BOOST_TEST(s.get<int>("range.observations") == 1);
    BOOST_TEST(s.get<int>("range.aircraft") == 1);
    // The widest dates parseIso8601Ms accepts: to - from is ~585 years, past
    // int64 nanoseconds; choosing the resolution must not overflow.
    r = get(f.port, "/api/stats/summary?from=1678-01-01T00:00:00Z&to=2262-01-01T00:00:00Z");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("range.res") == "hour");
    BOOST_TEST(s.get<int>("range.observations") == 1);
}

BOOST_AUTO_TEST_CASE(stats_series_columns)
{
    WebFx f;
    f.emit(statsObs(0xa40001));
    f.emit(statsObs(0xa40002));
    Reply r = get(f.port, "/api/stats/series?res=minute");
    BOOST_TEST(r.status == 200u);
    pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("res") == "minute");
    BOOST_TEST(s.get<int>("bucket_s") == 60);
    const std::size_t n = s.get_child("t").size();
    BOOST_TEST(n >= 1u);
    BOOST_TEST(std::regex_match(s.get_child("t").front().second.data(), kIsoMs));
    BOOST_TEST(s.get_child("observations").size() == n);
    BOOST_TEST(s.get_child("by_input.readsb").size() == n);
    for (const char* c : {"positions", "aircraft", "new_aircraft", "sent_updates", "drops_timeout", "drops_filtered",
                          "drops_source_lost", "drops_blocked", "drops_out_of_range", "drops_below_elevation",
                          "drops_no_position", "uat_downlink", "uat_uplink", "fisb_products", "emergencies",
                          "tcp_clients", "web_clients", "max_range_nm", "max_range_key"})
        BOOST_TEST(s.get_child(c).size() == n, c);
    int obs = 0;
    for (const auto& kv : s.get_child("observations"))
        obs += kv.second.get_value<int>();
    BOOST_TEST(obs == 2);
    // The default is minute.
    BOOST_TEST(parseJson(get(f.port, "/api/stats/series").body).get<int>("bucket_s") == 60);
    r = get(f.port, "/api/stats/series?res=hour");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("res") == "hour");
    BOOST_TEST(s.get<int>("bucket_s") == 3600);
    BOOST_TEST(s.get_child("t").size() == 1u);
    BOOST_TEST(s.get_child("observations").front().second.get_value<int>() == 2);
    r = get(f.port, "/api/stats/series?res=week");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "\"error\":"));
    // An empty range: empty columns.
    s = parseJson(get(f.port, "/api/stats/series?from=2100-01-01T00:00:00Z").body);
    BOOST_TEST(s.get_child("t").empty());
    BOOST_TEST(s.get_child("observations").empty());
}

BOOST_AUTO_TEST_CASE(stats_coverage_and_rssi)
{
    WebFx f;
    for (uint32_t a : {0xa40001u, 0xa40002u, 0xa40003u})
        f.emit(statsObs(a, 0.01 * double(a & 0xf)));
    Reply r = get(f.port, "/api/stats/coverage");
    BOOST_TEST(r.status == 200u);
    pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<int>("sector_deg") == 5);
    BOOST_TEST(s.get_child("bands").size() == 8u);
    BOOST_TEST(s.get_child("bands").front().second.data() == "0-2.5k");
    BOOST_TEST(s.get<double>("receiver.lat") == 38.9);
    const pt::ptree& cells = s.get_child("cells");
    BOOST_REQUIRE(!cells.empty());
    int n = 0;
    const std::regex key("^a4000[1-3]\\|icao$");
    for (const auto& kv : cells) {
        const pt::ptree& c = kv.second;
        BOOST_TEST(c.get<int>("sector") >= 0);
        BOOST_TEST(c.get<int>("sector") <= 71);
        BOOST_TEST(c.get<int>("band") >= 0);
        BOOST_TEST(c.get<int>("band") <= 7);
        BOOST_TEST(std::regex_match(c.get<std::string>("key"), key));
        BOOST_TEST(std::regex_match(c.get<std::string>("time"), kIsoMs));
        BOOST_TEST(c.get<double>("max_range_nm") > 0.0);
        n += c.get<int>("n");
    }
    BOOST_TEST(n == 3);
    BOOST_TEST(s.get<double>("grid.cell_deg") == 0.05);
    BOOST_TEST(s.get<int>("grid.cap") == 40000);
    BOOST_TEST(s.get<bool>("grid.full") == false);
    const pt::ptree& grid = s.get_child("grid.cells");
    BOOST_REQUIRE(!grid.empty());
    BOOST_TEST(grid.front().second.size() == 3u);
    int gn = 0;
    for (const auto& kv : grid) {
        auto it = kv.second.begin();
        const double lat = it->second.get_value<double>();
        const double lon = (++it)->second.get_value<double>();
        gn += (++it)->second.get_value<int>();
        BOOST_TEST(lat > 38.9);
        BOOST_TEST(lat < 39.1);
        BOOST_TEST(lon == -76.975);  // the centre of the cell [-77.0, -76.95)
    }
    BOOST_TEST(gn == 3);

    r = get(f.port, "/api/stats/rssi");
    BOOST_TEST(r.status == 200u);
    s = parseJson(r.body);
    BOOST_TEST(s.get<int>("range_bin_nm") == 10);
    BOOST_TEST(s.get<int>("rssi_bin_db") == 2);
    BOOST_TEST(s.get<int>("rssi_start") == -50);
    const pt::ptree& counts = s.get_child("counts");
    BOOST_REQUIRE(counts.size() == 50u);
    int total = 0;
    for (const auto& row : counts) {
        BOOST_TEST(row.second.size() == 25u);
        for (const auto& v : row.second)
            total += v.second.get_value<int>();
    }
    BOOST_TEST(total == 3);
    // rssi -20 is bin 15 of the first range bin (< 10 nm).
    auto first = counts.front().second.begin();
    std::advance(first, 15);
    BOOST_TEST(first->second.get_value<int>() == 3);
}

BOOST_AUTO_TEST_CASE(stats_aircraft_sort_and_q)
{
    WebFx f;
    for (int i = 0; i < 3; ++i)
        f.emit(statsObs(0xa40003));
    for (int i = 0; i < 2; ++i) {
        auto o = statsObs(0xa40002);
        o.registration = std::string("N123AB");
        f.emit(o);
    }
    f.emit(statsObs(0xa40001));

    Reply r = get(f.port, "/api/stats/aircraft?sort=observations&dir=asc");
    BOOST_TEST(r.status == 200u);
    pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<int>("total") == 3);
    BOOST_TEST(s.get<int>("offset") == 0);
    BOOST_TEST(s.get<int>("limit") == 100);
    BOOST_TEST(s.get<int>("evicted") == 0);
    std::vector<int> obs;
    for (const auto& kv : s.get_child("rows"))
        obs.push_back(kv.second.get<int>("observations"));
    BOOST_TEST((obs == std::vector<int>{1, 2, 3}));
    s = parseJson(get(f.port, "/api/stats/aircraft?sort=observations").body);
    BOOST_TEST((keysOf(s.get_child("rows")) == std::vector<std::string>{"a40003|icao", "a40002|icao", "a40001|icao"}));
    // Default: last_seen desc (a40001 was heard last).
    s = parseJson(get(f.port, "/api/stats/aircraft").body);
    BOOST_TEST(keysOf(s.get_child("rows")).front() == "a40001|icao");

    // One row in full.
    const pt::ptree& row = s.get_child("rows").front().second;
    BOOST_TEST(row.get<std::string>("hex") == "a40001");
    BOOST_TEST(row.get<std::string>("addr_type") == "icao");
    BOOST_TEST(std::regex_match(row.get<std::string>("first_seen"), kIsoMs));
    BOOST_TEST(std::regex_match(row.get<std::string>("last_seen"), kIsoMs));
    BOOST_TEST(std::regex_match(row.get<std::string>("max_range_time"), kIsoMs));
    BOOST_TEST(row.get<int>("sessions") == 1);
    BOOST_TEST(row.get<int>("positions") == 1);
    BOOST_TEST(row.get<double>("max_range_nm") > 0.0);
    BOOST_TEST(row.get<int>("max_alt_ft") == 5000);
    BOOST_TEST(row.get<double>("max_gs_kt") == 200.0);
    BOOST_TEST(row.get<double>("max_rssi") == -20.0);
    BOOST_TEST(row.get_child("callsigns").front().second.data() == "UAL11");
    BOOST_TEST(row.get_child("inputs").front().second.data() == "readsb");
    BOOST_TEST(row.get<std::string>("source") == "adsb_1090");
    BOOST_TEST(row.get<std::string>("class") == "airline");
    BOOST_TEST(row.get<std::string>("country") == "United States");
    BOOST_TEST(row.count("registration") == 0u);  // unset: left out
    BOOST_TEST(row.count("category") == 0u);

    for (const char* bad : {"/api/stats/aircraft?sort=bogus", "/api/stats/aircraft?sort=callsigns",
                            "/api/stats/aircraft?dir=up", "/api/stats/aircraft?limit=x",
                            "/api/stats/aircraft?offset=-1", "/api/stats/aircraft?offset=1.5",
                            "/api/stats/aircraft?limit=%2B-5", "/api/stats/aircraft?offset=%2B%2B1",
                            "/api/stats/aircraft?limit="}) {
        r = get(f.port, bad);
        BOOST_TEST(r.status == 400u, bad);
        BOOST_TEST(has(r.body, "\"error\":"), bad);
    }

    s = parseJson(get(f.port, "/api/stats/aircraft?q=n123ab").body);
    BOOST_TEST(s.get<int>("total") == 1);
    BOOST_TEST(s.get<std::string>("rows..hex") == "a40002");
    BOOST_TEST(s.get<std::string>("rows..registration") == "N123AB");
    s = parseJson(get(f.port, "/api/stats/aircraft?q=ual31").body);  // a callsign
    BOOST_TEST(s.get<int>("total") == 1);
    BOOST_TEST(s.get<std::string>("rows..hex") == "a40003");
    s = parseJson(get(f.port, "/api/stats/aircraft?q=A4000").body);  // the address
    BOOST_TEST(s.get<int>("total") == 3);
    s = parseJson(get(f.port, "/api/stats/aircraft?q=nothing").body);
    BOOST_TEST(s.get<int>("total") == 0);
    BOOST_TEST(s.get_child("rows").empty());

    s = parseJson(get(f.port, "/api/stats/aircraft?limit=5000").body);
    BOOST_TEST(s.get<int>("limit") == 1000);
    BOOST_TEST(parseJson(get(f.port, "/api/stats/aircraft?limit=%2B2").body).get<int>("limit") == 2);
    s = parseJson(get(f.port, "/api/stats/aircraft?limit=0").body);
    BOOST_TEST(s.get<int>("limit") == 1);
    BOOST_TEST(s.get_child("rows").size() == 1u);
    s = parseJson(get(f.port, "/api/stats/aircraft?offset=7").body);
    BOOST_TEST(s.get<int>("total") == 3);
    BOOST_TEST(s.get_child("rows").empty());
}

BOOST_AUTO_TEST_CASE(stats_aircraft_paging_stable)
{
    WebFx f;
    // 25 aircraft with one observation each, heard in descending address order.
    for (int i = 24; i >= 0; --i)
        f.emit(statsObs(0xa40100u + uint32_t(i)));
    std::vector<std::string> keys;
    for (int offset : {0, 10, 20}) {
        const pt::ptree s = parseJson(
            get(f.port, "/api/stats/aircraft?sort=observations&limit=10&offset=" + std::to_string(offset)).body);
        BOOST_TEST(s.get<int>("total") == 25);
        BOOST_TEST(s.get<int>("offset") == offset);
        const auto page = keysOf(s.get_child("rows"));
        BOOST_TEST(page.size() == (offset == 20 ? 5u : 10u));
        keys.insert(keys.end(), page.begin(), page.end());
    }
    BOOST_REQUIRE(keys.size() == 25u);
    BOOST_TEST(std::is_sorted(keys.begin(), keys.end()));
    BOOST_TEST(std::set<std::string>(keys.begin(), keys.end()).size() == 25u);
    BOOST_TEST(keys.front() == "a40100|icao");
}

BOOST_AUTO_TEST_CASE(stats_aircraft_entry_and_404)
{
    WebFx f;
    f.emit(statsObs(0xa40001));
    Reply r = get(f.port, "/api/stats/aircraft/a40001");
    BOOST_TEST(r.status == 200u);
    pt::ptree s = parseJson(r.body);
    BOOST_TEST(s.get<std::string>("key") == "a40001|icao");
    BOOST_TEST(s.get<int>("observations") == 1);
    BOOST_TEST(s.get<std::string>("class") == "airline");
    BOOST_TEST(get(f.port, "/api/stats/aircraft/A40001").status == 200u);
    BOOST_TEST(get(f.port, "/api/stats/aircraft/a40001?addr_type=icao").status == 200u);
    BOOST_TEST(get(f.port, "/api/stats/aircraft/a40001?addr_type=non_icao").status == 404u);
    BOOST_TEST(get(f.port, "/api/stats/aircraft/a40001?addr_type=bogus").status == 400u);
    r = get(f.port, "/api/stats/aircraft/a4ffff");
    BOOST_TEST(r.status == 404u);
    BOOST_TEST(has(r.body, "\"error\":"));
    for (const char* bad : {"/api/stats/aircraft/zz", "/api/stats/aircraft/a4000g", "/api/stats/aircraft/a400011",
                            "/api/stats/aircraft/"})
        BOOST_TEST(get(f.port, bad).status == 400u, bad);
    r = request(f.port, http::verb::post, "/api/stats/aircraft/a40001", "{}");
    BOOST_TEST(r.status == 405u);
    BOOST_TEST(r.allow == "GET");
}

BOOST_AUTO_TEST_CASE(stats_reset_guarded)
{
    WebFx f;
    f.emit(statsObs(0xa40001));
    const std::string since = parseJson(get(f.port, "/api/stats/summary").body).get<std::string>("since");
    Reply r = requestWith(f.port, http::verb::post, "/api/stats/reset", "{}", {});
    BOOST_TEST(r.status == 415u);
    r = requestWith(f.port, http::verb::post, "/api/stats/reset", "{}",
                    {{"Content-Type", "application/json"}, {"Origin", "http://evil.example"}});
    BOOST_TEST(r.status == 403u);
    r = request(f.port, http::verb::post, "/api/stats/reset", "[]");
    BOOST_TEST(r.status == 400u);
    r = request(f.port, http::verb::post, "/api/stats/reset", "{\"all\":true}");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(parseJson(get(f.port, "/api/stats/summary").body).get<int>("aircraft_total") == 1);

    const int obs_before = parseJson(get(f.port, "/api/stats/summary").body).get<int>("range.observations");
    BOOST_TEST(obs_before == 1);
    r = request(f.port, http::verb::post, "/api/stats/reset", "{}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"ok\":true}");
    const pt::ptree s = parseJson(get(f.port, "/api/stats/summary").body);
    BOOST_TEST(s.get<int>("aircraft_total") == 0);
    BOOST_TEST(s.get<int>("range.observations") == 0);
    BOOST_TEST(s.get<std::string>("since") >= since);
    // At most one reset every 5 s.
    r = request(f.port, http::verb::post, "/api/stats/reset", "{}");
    BOOST_TEST(r.status == 429u);
    BOOST_TEST(has(r.body, "\"error\":"));

    for (http::verb v : {http::verb::delete_, http::verb::get, http::verb::put}) {
        r = request(f.port, v, "/api/stats/reset", "{}");
        BOOST_TEST(r.status == 405u);
        BOOST_TEST(r.allow == "POST");
    }
}

BOOST_AUTO_TEST_CASE(stats_reset_rate_limit)
{
    // handleStatsApi directly, with the last reset set by the test.
    WebFx f;
    const auto req = skycued::webapi::makeApiRequest("POST", "/api/stats/reset",
                                                     {{"Content-Type", "application/json"}}, "{}", "127.0.0.1:1");
    skycued::webapi::StatsApiState state;
    auto reset = [&] {
        return f.fx->t.call([&] { return skycued::webapi::handleStatsApi(req, *f.fx->engine, &state); });
    };
    state.last_reset = std::chrono::steady_clock::now() - 6s;
    auto r = reset();
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.body == "{\"ok\":true}");
    BOOST_REQUIRE(state.last_reset.has_value());
    BOOST_TEST((std::chrono::steady_clock::now() - *state.last_reset < 2s));  // moved to this reset
    state.last_reset = std::chrono::steady_clock::now() - 1s;
    const auto before = *state.last_reset;
    r = reset();
    BOOST_TEST(r.status == 429u);
    BOOST_TEST(r.body == "{\"error\":\"the statistics were reset less than 5 s ago\"}");
    BOOST_TEST((*state.last_reset == before));  // a refused reset does not move it
    // Without a state (no WebServer): never limited.
    r = f.fx->t.call([&] { return skycued::webapi::handleStatsApi(req, *f.fx->engine, nullptr); });
    BOOST_TEST(r.status == 200u);
}

BOOST_AUTO_TEST_CASE(stats_unknown_path_404)
{
    WebFx f;
    f.emit(statsObs(0xa40001));
    for (const char* p : {"/api/stats/nope", "/api/stats/", "/api/stats/summary/x", "/api/stats/aircraft/a40001/x"}) {
        const Reply r = get(f.port, p);
        BOOST_TEST(r.status == 404u, p);
        BOOST_TEST(has(r.body, "\"error\":"), p);
    }
    for (const char* p : {"/api/stats/summary", "/api/stats/series", "/api/stats/coverage", "/api/stats/rssi",
                          "/api/stats/aircraft"}) {
        const Reply r = request(f.port, http::verb::post, p, "{}");
        BOOST_TEST(r.status == 405u, p);
        BOOST_TEST(r.allow == "GET", p);
    }
    BOOST_TEST(request(f.port, http::verb::post, "/api/stats/reset", "{}").status == 200u);
    // The engine's lifetime counters are not reset.
    const pt::ptree s = parseJson(get(f.port, "/api/stats").body);
    BOOST_TEST(s.get<int>("observations") == 1);
    BOOST_TEST(s.get<int>("tracks_total") == 1);
}

BOOST_AUTO_TEST_CASE(stats_decoder_object_only_with_decoding_inputs)
{
    {  // inputs.decoder=readsb: no decoder object
        WebFx f;
        const pt::ptree s = parseJson(get(f.port, "/api/stats").body);
        BOOST_TEST(!s.get_child_optional("decoder"));
    }
    WebOptions o;
    o.decode_raw = true;
    o.decode_port = freeUdpPort();
    WebFx f(o);
    namespace st = skycued::testing;
    auto frame = [](bool odd) {
        const auto b = st::df17(0xABC123, st::airbornePosition(11, st::encodeCpr({38.95, -77.05}, odd, false), 10000));
        return std::string("\x1a\x33") + std::string(7, '\0') + std::string(b.begin(), b.end());
    };
    asio::io_context uio;
    asio::ip::udp::socket sock(uio, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const asio::ip::udp::endpoint to(asio::ip::make_address("127.0.0.1"), o.decode_port);
    BOOST_TEST(pollUntil([&] {
        sock.send_to(asio::buffer(frame(false) + frame(true)), to);
        return parseJson(get(f.port, "/api/stats").body).get_child_optional("decoder").has_value();
    }));
    BOOST_TEST(pollUntil([&] { return parseJson(get(f.port, "/api/stats").body).get<int>("decoder.observations", 0) > 0; }));
    const pt::ptree s = parseJson(get(f.port, "/api/stats").body);
    for (const char* k : {"frames", "crc_fail", "crc_fixed", "decoded", "observations", "evicted", "keys",
                          "rebroadcast_suppressed"})
        BOOST_TEST(s.get_child("decoder").count(k) == 1u, k);
    BOOST_TEST(s.get<int>("decoder.frames") >= 2);
    BOOST_TEST(s.get<int>("decoder.keys") == 1);
    BOOST_TEST(s.get<int>("decoder.crc_fail") == 0);
}

// ---- statsReadRoute on a hand-built store ----

namespace {

using skycued::StatsStore;
const int64_t kStatsT0 = 1793707200;  // 2026-11-03T12:00:00Z

std::chrono::system_clock::time_point statsAt(int64_t s)
{
    return std::chrono::system_clock::time_point(std::chrono::seconds(s));
}

// One positioned observation of `address` at data time `t_s`.
void observeAt(StatsStore& st, uint32_t address, int64_t t_s, const std::string& callsign = "",
               const std::string& category = "")
{
    TrackObservation o;
    o.id = TrackId{address, AddressType::icao};
    o.source = skycue::Source::adsb_1090;
    o.received_time = statsAt(t_s);
    o.position_time = o.received_time;
    o.lat = 39.0;
    o.lon = -77.0;
    o.alt_baro_ft = 12000;
    o.input_id = "readsb";
    if (!callsign.empty())
        o.callsign = callsign;
    if (!category.empty())
        o.category = category;
    skycued::TrackState s;
    s.id = o.id;
    s.source = o.source;
    s.latest = o;
    s.last_input = o.input_id;
    s.first_seen = s.last_seen = o.received_time;
    st.observe(o, s, skycued::FilterResult{skycued::Decision::send, 6.0, 1.0, false});
}

pt::ptree summaryOf(const StatsStore& st, const std::string& query, int64_t now_s)
{
    const auto r = skycued::webapi::statsReadRoute(
        skycued::webapi::makeApiRequest("GET", "/api/stats/summary" + query, {}, ""), st, statsAt(now_s));
    BOOST_REQUIRE(r.status == 200u);
    return parseJson(r.body);
}

}  // namespace

BOOST_AUTO_TEST_CASE(stats_summary_class_country_follow_range)
{
    StatsStore st(skycued::ReceiverPosition{38.9, -77.0, 100.0});
    const int64_t t1 = kStatsT0, t2 = kStatsT0 + 2 * 3600;
    observeAt(st, 0xa40001, t1, "UAL11");      // United States, airline
    observeAt(st, 0xc00001, t2, "", "A1");     // Canada, private
    const std::string range2 = "?from=" + skycued::formatIso8601Ms(statsAt(t2 - 60)) +
                               "&to=" + skycued::formatIso8601Ms(statsAt(t2 + 60));
    pt::ptree s = summaryOf(st, range2, t2 + 60);
    BOOST_TEST(s.get<int>("aircraft_total") == 2);
    BOOST_TEST(s.get<int>("range.aircraft") == 1);
    BOOST_TEST(s.get<int>("range.observations") == 1);
    BOOST_TEST(s.get<int>("range.by_class.private") == 1);
    BOOST_TEST(s.get<int>("range.by_class.airline") == 0);
    BOOST_REQUIRE(s.get_child("range.by_country").size() == 1u);
    BOOST_TEST(s.get<std::string>("range.by_country..name") == "Canada");
    BOOST_TEST(s.get<int>("range.by_country..n") == 1);
    // The whole history (default range): both.
    s = summaryOf(st, "", t2 + 60);
    BOOST_TEST(s.get<int>("range.aircraft") == 2);
    BOOST_TEST(s.get<int>("range.by_class.airline") == 1);
    BOOST_TEST(s.get<int>("range.by_class.private") == 1);
    BOOST_TEST(s.get_child("range.by_country").size() == 2u);
    // A range before both: none.
    s = summaryOf(st, "?to=" + skycued::formatIso8601Ms(statsAt(t1 - 1)), t2 + 60);
    BOOST_TEST(s.get<int>("range.aircraft") == 0);
    BOOST_TEST(s.get_child("range.by_country").empty());
}

BOOST_AUTO_TEST_CASE(stats_summary_by_country_top20_and_other)
{
    // 22 countries: the first address of each with 3 addresses of one State,
    // found by scanning the address space in 1024-address steps.
    std::vector<std::pair<std::string, uint32_t>> countries;
    std::set<std::string> seen;
    for (uint32_t a = 0; a < 0x1000000u && countries.size() < 22; a += 0x400) {
        const auto c = skycued::icaoCountry(skycued::hexAddress(a));
        if (!c || seen.count(*c) || skycued::icaoCountry(skycued::hexAddress(a + 2)) != c)
            continue;
        seen.insert(*c);
        countries.emplace_back(*c, a);
    }
    BOOST_REQUIRE(countries.size() == 22u);
    // 8 countries with 3 aircraft, 8 with 2, 6 with 1: ties at every count.
    StatsStore st(skycued::ReceiverPosition{38.9, -77.0, 100.0});
    std::map<std::string, int> expected;
    for (std::size_t i = 0; i < countries.size(); ++i) {
        const int n = 3 - int(i / 8);
        for (int k = 0; k < n; ++k)
            observeAt(st, countries[i].second + uint32_t(k), kStatsT0 + int64_t(i));
        expected[countries[i].first] = n;
    }
    std::vector<std::pair<std::string, int>> order(expected.begin(), expected.end());
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    int other = 0;
    for (std::size_t i = 20; i < order.size(); ++i)
        other += order[i].second;
    BOOST_REQUIRE(other == 2);

    const pt::ptree s = summaryOf(st, "", kStatsT0 + 60);
    BOOST_TEST(s.get<int>("range.aircraft") == 8 * 3 + 8 * 2 + 6);
    const pt::ptree& by = s.get_child("range.by_country");
    BOOST_REQUIRE(by.size() == 21u);
    std::size_t i = 0;
    for (const auto& kv : by) {
        const std::string name = kv.second.get<std::string>("name");
        const int n = kv.second.get<int>("n");
        if (i < 20) {
            BOOST_TEST(name == order[i].first, i);
            BOOST_TEST(n == order[i].second, i);
        } else {
            BOOST_TEST(name == "other");
            BOOST_TEST(n == other);
        }
        ++i;
    }
}

// ---- skycued --check-inputs ----

namespace {

uint16_t closedPort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();  // closed again on return
}

std::string checkConf(const ConfMap& extra)
{
    ConfMap kv{{"cues.tcp_port", "0"}, {"web.port", "0"}};
    for (const auto& [k, v] : extra)
        kv[k] = v;
    return headConf(kv);
}

}  // namespace

BOOST_AUTO_TEST_CASE(check_inputs_exit_status)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    {
        enginetest::TestServer s;
        const std::string p = std::to_string(s.port());
        writeFile(conf, checkConf({{"inputs.readsb.json_port", p}, {"inputs.dump978.enable", "false"}}));
        const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs"}, log);
        BOOST_TEST(waitExit(pid, 10s) == 0);
        BOOST_TEST(readAll(log) == "readsb 127.0.0.1:" + p + ": connected\n");
    }
    const std::string cp = std::to_string(closedPort());
    writeFile(conf, checkConf({{"inputs.readsb.json_port", cp}, {"inputs.dump978.enable", "false"}}));
    const std::string log2 = tmp.path + "/log2";
    const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs"}, log2);
    BOOST_TEST(waitExit(pid, 10s) == 1);
    const auto out = lines(readAll(log2));
    BOOST_REQUIRE(out.size() == 1u);
    BOOST_TEST(out[0].rfind("readsb 127.0.0.1:" + cp + ": ", 0) == 0u);
    BOOST_TEST(out[0] != "readsb 127.0.0.1:" + cp + ": connected");
}

BOOST_AUTO_TEST_CASE(check_inputs_uses_readsb_host_key)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml";
    enginetest::TestServer s;
    const std::string p = std::to_string(s.port());
    writeFile(conf, checkConf({{"inputs.readsb.host", str("127.0.0.2")}, {"inputs.readsb.json_port", p}, {"inputs.dump978.enable", "false"}}));
    const std::string log = tmp.path + "/log";
    const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs"}, log);
    BOOST_TEST(waitExit(pid, 10s) == 1);
    const auto out = lines(readAll(log));
    BOOST_REQUIRE(out.size() == 1u);
    BOOST_TEST(out[0].rfind("readsb 127.0.0.2:" + p + ": ", 0) == 0u);
    const std::string log2 = tmp.path + "/log2";
    const pid_t pid2 = spawnSkycued({"--config", conf, "--check-inputs", "--readsb-host", "127.0.0.1"}, log2);
    BOOST_TEST(waitExit(pid2, 10s) == 0);
    BOOST_TEST(readAll(log2) == "readsb 127.0.0.1:" + p + ": connected\n");
}

BOOST_AUTO_TEST_CASE(check_inputs_includes_dump978_and_user_inputs)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    enginetest::TestServer s1, s2, s3;
    const std::string p1 = std::to_string(s1.port()), p2 = std::to_string(s2.port()), p3 = std::to_string(s3.port());
    skycued::InputsFile file;
    skycued::InputDef ps;
    ps.id = "ps";
    ps.type = skycued::InputType::pingstation_cat021;
    ps.transport = skycued::InputTransport::tcp_connect;
    ps.host = "127.0.0.1";
    ps.port = s3.port();
    skycued::InputDef pu;
    pu.id = "pu";
    pu.type = skycued::InputType::pingstation_json;
    pu.transport = skycued::InputTransport::udp;
    pu.port = 30999;
    file.inputs = {ps, pu};
    const std::string inputs = tmp.path + "/skycue.inputs.json";
    writeFile(inputs, skycued::inputsFileJson(file));
    writeFile(conf, checkConf({{"inputs.readsb.json_port", p1}, {"inputs.dump978.raw_port", p2}}));
    const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs"}, log);
    BOOST_TEST(waitExit(pid, 10s) == 0);
    const auto out = lines(readAll(log));
    BOOST_REQUIRE(out.size() == 4u);
    BOOST_TEST(out[0] == "readsb 127.0.0.1:" + p1 + ": connected");
    BOOST_TEST(out[1] == "dump978 127.0.0.1:" + p2 + ": connected");
    BOOST_TEST(out[2] == "ps 127.0.0.1:" + p3 + ": connected");
    BOOST_TEST(out[3] == "pu: not checked (listens on 0.0.0.0:30999)");
}

BOOST_AUTO_TEST_CASE(check_inputs_creates_nothing)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    const std::string state = tmp.path + "/state";
    ::mkdir(state.c_str(), 0755);
    writeFile(conf, checkConf({{"recording.dir", str(tmp.path + "/rec-new")},
                              {"paths.state_dir", str(state)},
                              {"inputs.readsb.json_port", std::to_string(closedPort())},
                              {"inputs.dump978.enable", "false"}}));
    const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs"}, log);
    BOOST_TEST(waitExit(pid, 10s) == 1);
    struct stat st{};
    BOOST_TEST(::stat((tmp.path + "/rec-new").c_str(), &st) != 0);
    size_t entries = 0;
    if (DIR* d = ::opendir(state.c_str())) {
        while (const dirent* e = ::readdir(d))
            if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                ++entries;
        ::closedir(d);
    }
    BOOST_TEST(entries == 0u);
}

BOOST_AUTO_TEST_CASE(check_inputs_with_replay_is_usage_error)
{
    TempDir tmp;
    const std::string conf = tmp.path + "/skycue.toml", log = tmp.path + "/log";
    writeFile(conf, checkConf({}));
    const pid_t pid = spawnSkycued({"--config", conf, "--check-inputs", "--replay", kFixture}, log);
    BOOST_TEST(waitExit(pid, 10s) == 2);
}

BOOST_AUTO_TEST_CASE(api_track_phase6_fields)
{
    WebFx f;
    f.fx->t.call([&] {
        f.src->connection(true);
        auto o = nearbyObservation(0xa1b2c3, 0);
        o.ias_kt = 250;
        o.heading_mag_deg = 91.5f;
        o.nav_modes = 0x03;
        o.acas_sl = 4;
        skycued::AcasRaState ra;
        ra.active = true;
        ra.rac = 3;
        ra.rat = true;
        ra.mte = true;
        ra.tti = 2;
        ra.threat_alt_ft = 12300;
        ra.threat_range_nm = 3.5f;
        ra.threat_bearing_deg = 72.0f;
        ra.text = "climb";
        ra.at = o.received_time;
        o.acas_ra = ra;
        f.src->emit(o);
    });
    const pt::ptree s = parseJson(get(f.port, "/api/state").body);
    const auto& t = s.get_child("tracks").front().second;
    BOOST_TEST(t.get<int>("ias_kt") == 250);
    BOOST_TEST(t.get<double>("heading_mag_deg") == 91.5);
    BOOST_TEST(t.get<int>("acas_sl") == 4);
    BOOST_TEST(t.get_child("nav_modes").size() == 2u);
    BOOST_TEST(t.get_child("nav_modes").front().second.data() == "autopilot");
    BOOST_TEST(t.get<std::string>("acas_ra.text") == "climb");
    BOOST_TEST(t.get<bool>("acas_ra.active") == true);
    BOOST_TEST(t.get<std::string>("acas_ra.threat.type") == "position");
    BOOST_TEST(t.get<int>("acas_ra.threat.alt_ft") == 12300);
    BOOST_TEST(t.get<int>("acas_ra.rac") == 3);
    BOOST_TEST(t.get<bool>("acas_ra.rat") == true);
    BOOST_TEST(t.get<bool>("acas_ra.mte") == true);
    BOOST_TEST(t.get<double>("acas_ra.threat.range_nm") == 3.5);
    BOOST_TEST(t.get<double>("acas_ra.threat.bearing_deg") == 72.0);
}

BOOST_AUTO_TEST_CASE(api_track_acas_ra_address_threat)
{
    WebFx f;
    f.fx->t.call([&] {
        f.src->connection(true);
        auto o = nearbyObservation(0xa1b2c3, 0);
        skycued::AcasRaState ra;
        ra.active = true;
        ra.tti = 1;
        ra.threat_address = 0x4ca002;
        ra.text = "climb";
        ra.at = o.received_time;
        o.acas_ra = ra;
        f.src->emit(o);
    });
    const pt::ptree s = parseJson(get(f.port, "/api/state").body);
    const auto& t = s.get_child("tracks").front().second;
    BOOST_TEST(t.get<std::string>("acas_ra.threat.type") == "address");
    BOOST_TEST(t.get<std::string>("acas_ra.threat.address") == "4ca002");
}

BOOST_AUTO_TEST_CASE(api_track_acas_ra_expires_without_a_message)
{
    WebFx f;
    std::chrono::system_clock::time_point t0;
    f.fx->t.call([&] {
        f.src->connection(true);
        auto o = nearbyObservation(0xa1b2c3, 0);
        t0 = o.received_time;
        skycued::AcasRaState ra;
        ra.active = true;
        ra.text = "climb";
        ra.at = o.received_time;
        o.acas_ra = ra;
        f.src->emit(o);
    });
    {
        const pt::ptree s = parseJson(get(f.port, "/api/state").body);
        BOOST_TEST(s.get_child("tracks").front().second.get_child_optional("acas_ra").has_value());
    }
    // Another aircraft 40 s later moves the data clock; the silent one loses its advisory.
    f.fx->t.call([&] {
        auto o = nearbyObservation(0xd4e5f6, 0.01);
        o.received_time = t0 + 40s;
        o.position_time = o.received_time;
        f.src->emit(o);
    });
    const pt::ptree s = parseJson(get(f.port, "/api/state").body);
    bool found = false;
    for (const auto& kv : s.get_child("tracks")) {
        if (kv.second.get<std::string>("hex") != "a1b2c3")
            continue;
        found = true;
        BOOST_TEST(!kv.second.get_child_optional("acas_ra").has_value());
    }
    BOOST_TEST(found);
}

BOOST_AUTO_TEST_CASE(track_json_phase6_absent_has_no_keys)
{
    auto ts = geomSnapshot();
    const std::string body = skycued::webapi::trackJson(ts);
    for (const char* k : {"ias_kt", "nav_modes", "acas_ra", "mach", "qnh_mb"})
        BOOST_TEST(body.find(std::string("\"") + k + "\"") == std::string::npos, k);
}
