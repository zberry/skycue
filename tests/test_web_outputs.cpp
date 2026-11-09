#define BOOST_TEST_MODULE skycued_web_outputs
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
#include "engine_test_support.hpp"

#include "hub/hub.hpp"
#include "json_lite.hpp"
#include "runtime_config.hpp"
#include "web_api.hpp"
#include "web_outputs.hpp"
#include "web_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

using namespace enginetest;
using skycue::net::test::eventually;
using skycued::Hub;
using skycued::JsonObject;
using skycued::OutputDef;
using skycued::RuntimeConfig;
using skycued::WebConfig;
using skycued::WebServer;
using skycued::webapi::ApiRequest;
using skycued::webapi::ApiResponse;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using Headers = std::map<std::string, std::string>;

namespace {

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

JsonObject parsed(const std::string& text)
{
    auto o = JsonObject::parse(text);
    if (!o)
        throw std::runtime_error("not a JSON object: " + text);
    return *o;
}

// The "outputs" array's entry with this id.
std::optional<JsonObject> listed(const std::string& body, const std::string& id)
{
    const auto list = parsed(body).getObjectArray("outputs");
    if (!list)
        throw std::runtime_error("no outputs array: " + body);
    for (const auto& o : *list)
        if (o.getString("id") == id)
            return o;
    return std::nullopt;
}

std::vector<std::string> idsOf(const std::string& body)
{
    std::vector<std::string> ids;
    for (const auto& o : parsed(body).getObjectArray("outputs").value_or(std::vector<JsonObject>()))
        ids.push_back(o.getString("id").value_or(""));
    return ids;
}

std::string stateText(const std::string& body, const std::string& id, const std::string& key)
{
    const auto o = listed(body, id);
    if (!o)
        return "<no such output>";
    const auto st = o->getObject("state");
    if (!st)
        return "<no state>";
    if (const auto n = st->getInt(key.c_str()))
        return std::to_string(*n);
    if (const auto b = st->getBool(key.c_str()))
        return *b ? "true" : "false";
    return st->getString(key.c_str()).value_or("<missing>");
}

std::string beastOut(const std::string& id, uint16_t port, const std::string& extra = "")
{
    return R"({"id":")" + id + R"(","kind":"beast","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
           std::to_string(port) + extra + "}";
}

// A client socket on 127.0.0.1:port; reads what has arrived without blocking.
struct Consumer {
    asio::io_context io;
    tcp::socket sock{io};
    std::string got;
    bool closed = false;

    explicit Consumer(uint16_t port, int rcvbuf = 0)
    {
        sock.open(tcp::v4());
        if (rcvbuf > 0)
            sock.set_option(asio::socket_base::receive_buffer_size(rcvbuf));
        sock.connect({asio::ip::make_address("127.0.0.1"), port});
        sock.non_blocking(true);
    }
    void pump()
    {
        char buf[16384];
        for (;;) {
            boost::system::error_code ec;
            const std::size_t n = sock.read_some(asio::buffer(buf), ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again)
                return;
            if (ec) {
                closed = true;
                return;
            }
            got.append(buf, n);
        }
    }
    bool readUntil(std::size_t size)
    {
        return eventually([&] {
            pump();
            return got.size() >= size;
        });
    }
    bool seesEnd()
    {
        return eventually([&] {
            pump();
            return closed;
        });
    }
};

// ---- the fixture: an Engine over an InputManager with an inputs file, the
// cue Forwarder, a Hub fed the way main.cpp feeds it, a RuntimeConfig and a WebServer ----

struct Fx {
    TempDir tmp;
    std::string conf, inputs_file;
    FakeSource* src = nullptr;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<RuntimeConfig> rc;
    std::unique_ptr<Hub> hub;
    std::unique_ptr<WebServer> web;
    uint16_t web_port = 0;

    explicit Fx(bool replaying = false, bool with_hub = true)
    {
        conf = tmp.path + "/skycue.toml";
        inputs_file = tmp.path + "/skycue.inputs.json";
        std::ofstream(conf) << skycue_test::tomlConf({{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"cues.track_timeout_s", "5"}});
        std::function<std::unique_ptr<InputManager>(asio::io_context&)> make = [this, replaying](asio::io_context& io) {
            auto s = std::make_unique<FakeSource>();
            src = s.get();
            if (!replaying)
                return sourceInputs(io, std::move(s), "readsb", inputs_file);
            skycued::InputManagerConfig mc;
            mc.inputs_file = inputs_file;
            mc.replaying = true;
            auto m = std::make_unique<InputManager>(io, mc);
            InputDef def;
            def.id = "replay";
            def.type = InputType::replay;
            m->addBuiltin(std::make_unique<skycued::SourceInput>(def, std::move(s)));
            m->load();
            return m;
        };
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), make);
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
            if (with_hub)
                wire();
            WebConfig wc;
            wc.bind = "127.0.0.1";
            wc.port = 0;
            wc.www_dir = tmp.path;
            web = std::make_unique<WebServer>(fx->t.io, *fx->engine, *rc, wc);
            web->start();
            web_port = web->localPort();
        });
    }
    ~Fx()
    {
        try {
            fx->t.call([&] {
                web.reset();
                if (hub)
                    hub->stop();
                rc.reset();
                hub.reset();
            });
        } catch (...) {
        }
    }

    void wire()
    {
        Engine& engine = *fx->engine;
        hub = std::make_unique<Hub>(fx->t.io, fx->fwd.get());
        std::vector<std::string> ids;
        for (const auto& d : fx->fwd->destinations())
            if (d.builtin)
                ids.push_back(d.id);
        InputManager& inputs = engine.inputs();
        inputs.setBuiltinOutputIds(ids);
        Hub* h = hub.get();
        inputs.onOutputsChanged(
            [h](const std::vector<OutputDef>& defs, const std::map<std::string, bool>& builtin) {
                h->apply(defs, builtin);
            });
        inputs.rawTap().setSink([h](const skycued::RawMessage& m) { h->publish(m); },
                                [h](skycued::RawKind k) { return h->wants(k); });
        hub->apply(inputs.outputs(), inputs.outputBuiltinStates());
        hub->onChanged([&engine] { engine.outputsChanged(); });
        engine.setHub(h);
    }

    ApiResponse call(const std::string& method, const std::string& path, const std::string& body, Headers headers)
    {
        ApiRequest req;
        req.method = method;
        req.path = path;
        req.headers = std::move(headers);
        req.body = body;
        return fx->t.call([&] {
            skycued::webapi::ApiContext ctx{*fx->engine, *rc, tmp.path};
            return skycued::webapi::handleApi(req, ctx);
        });
    }
    ApiResponse call(const std::string& method, const std::string& path, const std::string& body = "")
    {
        Headers h{{"host", "127.0.0.1:8080"}};
        if (method == "PUT" || method == "POST")
            h["content-type"] = "application/json";
        return call(method, path, body, h);
    }
    ApiResponse get(const std::string& path) { return call("GET", path); }

    void publish(skycued::RawKind kind, const std::string& bytes)
    {
        fx->t.call([&] {
            skycued::RawMessage m;
            m.kind = kind;
            m.bytes = skycue::net::makeBuf(bytes);
            m.input = "test";
            hub->publish(m);
        });
    }
};

// ---- a minimal WebSocket client ----

struct WsClient {
    asio::io_context io;
    websocket::stream<tcp::socket> ws{io};
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> msgs;
    bool closed = false;

    explicit WsClient(uint16_t port)
    {
        auto& s = ws.next_layer();
        s.open(tcp::v4());
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        ws.handshake("127.0.0.1", "/ws");
        reader = std::thread([this] { loop(); });
    }
    ~WsClient()
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

}  // namespace

BOOST_AUTO_TEST_CASE(outputs_api_crud_status_codes)
{
    Fx f;
    // The list: the cue stream's built-in first.
    auto r = f.get("/api/outputs");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(r.content_type == "application/json");
    BOOST_REQUIRE((idsOf(r.body) == std::vector<std::string>{"cue-tcp"}));
    const auto cue = listed(r.body, "cue-tcp");
    BOOST_TEST(cue->getBool("builtin").value_or(false));
    BOOST_TEST(cue->getString("kind").value_or("") == "cue");
    BOOST_TEST(stateText(r.body, "cue-tcp", "up") == "true");

    // POST: 201 with the definition and its state.
    const uint16_t port = freeTcpPort();
    r = f.call("POST", "/api/outputs", beastOut("bo", port, R"(,"name":"Beast out")"));
    BOOST_TEST(r.status == 201u, r.body);
    BOOST_TEST(has(r.body, R"("id":"bo","name":"Beast out","kind":"beast","enabled":true,"transport":"tcp_listen")"), r.body);
    BOOST_TEST(has(r.body, R"("builtin":false,"state":{"up":)"), r.body);
    BOOST_TEST(has(r.body, R"("last_error":""}})"), r.body);
    BOOST_TEST((idsOf(f.get("/api/outputs").body) == std::vector<std::string>{"cue-tcp", "bo"}));

    // 415 first, then cross-origin 403, then the body.
    r = f.call("POST", "/api/outputs", beastOut("x", freeTcpPort()), Headers{{"host", "127.0.0.1:8080"}});
    BOOST_TEST(r.status == 415u);
    r = f.call("POST", "/api/outputs", "not json",
               Headers{{"host", "127.0.0.1:8080"}, {"content-type", "text/plain"}});
    BOOST_TEST(r.status == 415u);
    r = f.call("POST", "/api/outputs", beastOut("x", freeTcpPort()),
               Headers{{"host", "127.0.0.1:8080"}, {"content-type", "application/json"},
                       {"origin", "http://evil.example"}});
    BOOST_TEST(r.status == 403u);
    BOOST_TEST(f.call("DELETE", "/api/outputs/bo", "", Headers{{"host", "127.0.0.1:8080"}, {"origin", "http://evil.example"}})
                   .status == 403u);
    BOOST_TEST(idsOf(f.get("/api/outputs").body).size() == 2u);

    // 400: not an object, a rule broken (the message verbatim), an unknown key, a reserved id.
    BOOST_TEST(f.call("POST", "/api/outputs", "not json").status == 400u);
    r = f.call("POST", "/api/outputs", R"({"id":"y","kind":"beast","transport":"tcp_listen","port":0})");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "port must be an integer from 1 to 65535"), r.body);
    r = f.call("POST", "/api/outputs", beastOut("y", freeTcpPort(), R"(,"colour":"red")"));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "colour"), r.body);
    r = f.call("POST", "/api/outputs", beastOut("cue-tcp", freeTcpPort()));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "reserved"), r.body);
    // 400 for a port another output serves, naming it.
    r = f.call("POST", "/api/outputs", beastOut("z", port));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "is already used by output bo"), r.body);
    // 409: the id is taken.
    r = f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort()));
    BOOST_TEST(r.status == 409u);

    // PUT: a changed definition, the id from the path, a built-in's enabled.
    const uint16_t moved = freeTcpPort();
    r = f.call("PUT", "/api/outputs/bo", beastOut("bo", moved, R"(,"name":"Moved")"));
    BOOST_TEST(r.status == 200u, r.body);
    BOOST_TEST(has(r.body, "\"port\":" + std::to_string(moved)), r.body);
    r = f.call("PUT", "/api/outputs/bo", R"({"kind":"beast","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                                              std::to_string(moved) + "}");
    BOOST_TEST(r.status == 200u, r.body);
    BOOST_TEST(has(r.body, R"("name":"")"), r.body);
    BOOST_TEST(f.call("PUT", "/api/outputs/bo", beastOut("other", moved)).status == 400u);
    BOOST_TEST(f.call("PUT", "/api/outputs/nope", beastOut("nope", moved)).status == 404u);
    BOOST_TEST(f.call("PUT", "/api/outputs/bo", "[]").status == 400u);
    r = f.call("PUT", "/api/outputs/cue-tcp", R"({"enabled":false})");
    BOOST_TEST(r.status == 200u, r.body);
    BOOST_TEST(has(r.body, R"("enabled":false)"), r.body);
    BOOST_TEST(stateText(f.get("/api/outputs").body, "cue-tcp", "up") == "false");
    BOOST_TEST(f.call("PUT", "/api/outputs/cue-tcp", R"({"enabled":true,"port":1})").status == 403u);
    BOOST_TEST(f.call("PUT", "/api/outputs/cue-tcp", beastOut("cue-tcp", moved)).status == 403u);
    BOOST_TEST(f.call("PUT", "/api/outputs/cue-tcp", R"({"enabled":"yes"})").status == 400u);
    BOOST_TEST(f.call("POST", "/api/outputs/cue-tcp/enable", "{}").status == 200u);
    BOOST_TEST(eventually([&] { return stateText(f.get("/api/outputs").body, "cue-tcp", "up") == "true"; }));

    // enable / disable.
    r = f.call("POST", "/api/outputs/bo/disable", "{}");
    BOOST_TEST(r.status == 200u);
    BOOST_TEST(has(r.body, R"("enabled":false)"), r.body);
    BOOST_TEST(stateText(f.get("/api/outputs").body, "bo", "up") == "false");
    BOOST_TEST(f.call("POST", "/api/outputs/bo/enable", R"({"x":1})").status == 400u);
    BOOST_TEST(f.call("POST", "/api/outputs/bo/enable", "{}").status == 200u);
    BOOST_TEST(f.call("POST", "/api/outputs/nope/enable", "{}").status == 404u);

    // DELETE.
    BOOST_TEST(f.call("DELETE", "/api/outputs/cue-tcp").status == 403u);
    BOOST_TEST(f.call("DELETE", "/api/outputs/nope").status == 404u);
    r = f.call("DELETE", "/api/outputs/bo");
    BOOST_TEST(r.status == 204u);
    BOOST_TEST(r.body.empty());
    BOOST_TEST((idsOf(f.get("/api/outputs").body) == std::vector<std::string>{"cue-tcp"}));

    // Paths and methods.
    r = f.get("/api/outputs/Bad_ID");  // the id is checked before the method
    BOOST_TEST(r.status == 400u);
    r = f.get("/api/outputs/bo");
    BOOST_TEST(r.status == 405u);
    BOOST_TEST(r.allow == "PUT, DELETE");
    r = f.call("PUT", "/api/outputs/Bad_ID", "{}");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "invalid output id"), r.body);
    BOOST_TEST(f.get("/api/outputs/x/y/z").status == 404u);
    BOOST_TEST(f.call("POST", "/api/outputs/bo/status", "{}").status == 404u);
    BOOST_TEST(f.get("/api/outputs/").status == 404u);
    r = f.call("PATCH", "/api/outputs", "{}");
    BOOST_TEST(r.status == 405u);
    BOOST_TEST(r.allow == "GET, POST");
    r = f.get("/api/outputs/bo/enable");
    BOOST_TEST(r.status == 405u);
    BOOST_TEST(r.allow == "POST");

    // The file: version 2 held only while there is v2 content.
    const std::string file = readAll(f.inputs_file);
    BOOST_TEST(!has(file, "\"cue-tcp\""), file);  // enabled is the default: not written
}

BOOST_AUTO_TEST_CASE(outputs_api_writes_409_while_replaying)
{
    Fx f(true);
    BOOST_TEST(f.get("/api/outputs").status == 200u);
    BOOST_TEST(f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort())).status == 409u);
    BOOST_TEST(f.call("PUT", "/api/outputs/cue-tcp", R"({"enabled":false})").status == 409u);
    BOOST_TEST(f.call("DELETE", "/api/outputs/bo").status == 409u);
    BOOST_TEST(f.call("POST", "/api/outputs/bo/enable", "{}").status == 409u);
    // The safety checks still come first.
    BOOST_TEST(f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort()), Headers{{"host", "127.0.0.1:8080"}}).status ==
               415u);
    BOOST_TEST(f.call("POST", "/api/outputs", "{}", Headers{{"host", "127.0.0.1:8080"},
                                                           {"content-type", "application/json"},
                                                           {"origin", "http://evil.example"}})
                   .status == 403u);
}

BOOST_AUTO_TEST_CASE(outputs_api_state_counts_a_connected_consumer)
{
    Fx f;
    const uint16_t port = freeTcpPort();
    BOOST_REQUIRE(f.call("POST", "/api/outputs", beastOut("bo", port)).status == 201u);
    BOOST_TEST(stateText(f.get("/api/outputs").body, "bo", "up") == "true");
    BOOST_TEST(stateText(f.get("/api/outputs").body, "bo", "peers") == "0");
    Consumer c(port);
    BOOST_REQUIRE(eventually([&] { return stateText(f.get("/api/outputs").body, "bo", "peers") == "1"; }));
    f.publish(skycued::RawKind::beast, "\x1a\x32hello");
    BOOST_REQUIRE(c.readUntil(7));
    BOOST_TEST(c.got == "\x1a\x32hello");
    BOOST_REQUIRE(eventually([&] { return stateText(f.get("/api/outputs").body, "bo", "messages") == "1"; }));
    BOOST_TEST(stateText(f.get("/api/outputs").body, "bo", "bytes") == "7");
    BOOST_TEST(stateText(f.get("/api/outputs").body, "bo", "dropped") == "0");
}

BOOST_AUTO_TEST_CASE(outputs_api_multicast_bad_interface_201_with_last_error)
{
    Fx f;
    const uint16_t port = freeUdpPort();
    const std::string body = R"({"id":"mc","kind":"json","transport":"udp","group":"239.255.42.8","port":)" +
                             std::to_string(port) + R"(,"interface":"192.0.2.77"})";
    const auto r = f.call("POST", "/api/outputs", body);
    BOOST_TEST(r.status == 201u, r.body);
    const std::string list = f.get("/api/outputs").body;
    if (stateText(list, "mc", "up") == "true") {
        BOOST_TEST_MESSAGE("a bad multicast interface is usable on this host: nothing to check");
        return;
    }
    BOOST_TEST(stateText(list, "mc", "last_error") != "");
    BOOST_TEST(stateText(list, "mc", "last_error") != "<missing>");
    // Everything else is unaffected, and the output can be fixed.
    BOOST_TEST(stateText(list, "cue-tcp", "up") == "true");
    const auto fixed = f.call("PUT", "/api/outputs/mc",
                              R"({"kind":"json","transport":"udp","host":"127.0.0.1","port":)" + std::to_string(port) + "}");
    BOOST_TEST(fixed.status == 200u, fixed.body);
    BOOST_TEST(stateText(f.get("/api/outputs").body, "mc", "up") == "true");
    BOOST_TEST(stateText(f.get("/api/outputs").body, "mc", "last_error") == "");
}

BOOST_AUTO_TEST_CASE(delete_output_with_backlogged_consumer_204_others_flow)
{
    Fx f;
    const uint16_t slow_port = freeTcpPort(), other_port = freeTcpPort();
    BOOST_REQUIRE(f.call("POST", "/api/outputs", beastOut("slow", slow_port)).status == 201u);
    BOOST_REQUIRE(f.call("POST", "/api/outputs", beastOut("other", other_port)).status == 201u);
    Consumer stalled(slow_port, 4096);  // never reads until the output is gone
    Consumer good(other_port);
    Consumer cue(f.fx->port());
    BOOST_REQUIRE(eventually([&] {
        const std::string list = f.get("/api/outputs").body;
        return stateText(list, "slow", "peers") == "1" && stateText(list, "other", "peers") == "1";
    }));
    // A backlog behind the stalled consumer: far more than its buffers hold.
    const std::string chunk(64 * 1024, 'x');
    std::size_t sent = 0;
    for (int i = 0; i < 64; ++i) {
        f.publish(skycued::RawKind::beast, chunk);
        sent += chunk.size();
        good.pump();
    }
    BOOST_REQUIRE(good.readUntil(sent / 2));

    const auto r = f.call("DELETE", "/api/outputs/slow");
    BOOST_TEST(r.status == 204u);
    // The stalled peer reaches EOF once it reads what was already in its buffers.
    BOOST_TEST(stalled.seesEnd());
    // The other output and the cue stream keep delivering.
    const std::size_t before = good.got.size();
    f.publish(skycued::RawKind::beast, "after-delete");
    BOOST_TEST(eventually([&] {
        good.pump();
        return good.got.size() >= before + 12;
    }));
    BOOST_REQUIRE(cue.readUntil(1));
    f.fx->t.call([&] {
        skycue::Track t;
        t.id = skycue::TrackId{0xabc123, skycue::AddressType::icao};
        t.source = skycue::Source::adsb_1090;
        t.position_time = std::chrono::system_clock::now();
        t.lat_deg = 38.9;
        t.lon_deg = -77.0;
        f.fx->fwd->sendUpdate(t);
    });
    const std::size_t cue_before = cue.got.size();
    BOOST_TEST(eventually([&] {
        cue.pump();
        return cue.got.size() > cue_before;
    }));
    // The freed port can be bound again at once.
    BOOST_TEST(f.call("POST", "/api/outputs", beastOut("slow", slow_port)).status == 201u);
    BOOST_TEST(eventually([&] { return stateText(f.get("/api/outputs").body, "slow", "up") == "true"; }));
}

BOOST_AUTO_TEST_CASE(outputs_error_ladder_status_and_exact_body)
{
    Fx f;
    auto r = f.call("POST", "/api/outputs", "not json");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(r.body == R"({"error":"the body must be a JSON object"})");
    r = f.call("DELETE", "/api/outputs/cue-tcp");
    BOOST_TEST(r.status == 403u);
    BOOST_TEST(r.body == R"({"error":"output 'cue-tcp' is built in: it can only be enabled or disabled"})");
    r = f.call("DELETE", "/api/outputs/nope");
    BOOST_TEST(r.status == 404u);
    BOOST_TEST(r.body == R"({"error":"no output 'nope'"})");
    BOOST_TEST(f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort())).status == 201u);
    r = f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort()));
    BOOST_TEST(r.status == 409u);
    BOOST_TEST(r.body == R"({"error":"output 'bo' already exists"})");

    if (::geteuid() == 0)
        return;  // root writes through a read-only directory
    ::chmod(f.tmp.path.c_str(), 0500);
    const auto w = f.call("POST", "/api/outputs", beastOut("bp", freeTcpPort()));
    ::chmod(f.tmp.path.c_str(), 0700);
    BOOST_TEST(w.status == 500u, w.body);
    BOOST_TEST(w.body.rfind("{\"error\":\"", 0) == 0u, w.body);
    BOOST_TEST(w.body.find(f.inputs_file) != std::string::npos, w.body);
    BOOST_TEST(w.body.find("Permission denied") != std::string::npos, w.body);
}

BOOST_AUTO_TEST_CASE(outputs_empty_array_body_today)
{
    Fx f;
    BOOST_TEST(f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort())).status == 201u);
    BOOST_TEST(f.call("POST", "/api/outputs/bo/disable", "[]").status == 200u);
    const auto r = f.call("PUT", "/api/outputs/bo", "[]");
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(r.body == R"({"error":"the body must be a JSON object"})");
}

BOOST_AUTO_TEST_CASE(cues_clients_json_has_output_field)
{
    Fx f;
    // The built-in listener.
    Consumer builtin(f.fx->port());
    BOOST_REQUIRE(eventually([&] { return has(f.get("/api/cues/clients").body, "\"output\":\"cue-tcp\""); }));
    auto tcp_entry = parsed(f.get("/api/cues/clients").body).getObjectArray("tcp").value();
    BOOST_REQUIRE(tcp_entry.size() == 1u);
    for (const char* key : {"endpoint", "direction", "connected_since", "bytes_sent", "output"})
        BOOST_TEST(tcp_entry[0].has(key), key);

    // A user cue output (listener and a UDP target) reports its own id.
    const uint16_t lport = freeTcpPort();
    BOOST_REQUIRE(f.call("POST", "/api/outputs",
                         R"({"id":"cue-extra","kind":"cue","transport":"tcp_listen","bind":"127.0.0.1","port":)" +
                             std::to_string(lport) + "}")
                      .status == 201u);
    asio::io_context uio;
    asio::ip::udp::socket sink(uio, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    BOOST_REQUIRE(f.call("POST", "/api/outputs",
                         R"({"id":"cue-udp-x","kind":"cue","transport":"udp","host":"127.0.0.1","port":)" +
                             std::to_string(sink.local_endpoint().port()) + "}")
                      .status == 201u);
    Consumer extra(lport);
    BOOST_REQUIRE(eventually([&] { return has(f.get("/api/cues/clients").body, "\"output\":\"cue-extra\""); }));
    const std::string body = f.get("/api/cues/clients").body;
    BOOST_TEST(has(body, "\"output\":\"cue-udp-x\""), body);
    const auto udp_entry = parsed(body).getObjectArray("udp").value();
    BOOST_REQUIRE(udp_entry.size() == 1u);
    BOOST_TEST(udp_entry[0].has("datagrams_sent"));
    BOOST_TEST(udp_entry[0].has("output"));
    // The cue output's own state counts the peer.
    BOOST_TEST(eventually([&] { return stateText(f.get("/api/outputs").body, "cue-extra", "peers") == "1"; }));
}

BOOST_AUTO_TEST_CASE(ws_outputs_message_on_change)
{
    Fx f;
    WsClient c(f.web_port);
    BOOST_REQUIRE(c.waitFor([&] { return !c.msgs.empty(); }));
    {
        std::lock_guard<std::mutex> lk(c.m);
        const auto snap = parsed(c.msgs.front());
        const auto outputs = snap.getObjectArray("outputs");
        BOOST_REQUIRE(outputs.has_value());
        BOOST_REQUIRE(outputs->size() == 1u);
        BOOST_TEST(outputs->front().getString("id").value_or("") == "cue-tcp");
    }
    const auto r = f.call("POST", "/api/outputs", beastOut("bo", freeTcpPort()));
    BOOST_REQUIRE(r.status == 201u);
    const std::string m = c.waitForMessage({"\"type\":\"outputs\"", "\"id\":\"bo\""});
    BOOST_REQUIRE(!m.empty());
    const auto events = parsed(m);
    bool seen = false;
    for (const auto& item : events.getObjectArray("items").value_or(std::vector<JsonObject>()))
        if (item.getString("type") == std::string("outputs")) {
            seen = true;
            std::vector<std::string> ids;
            for (const auto& o : item.getObjectArray("outputs").value_or(std::vector<JsonObject>()))
                ids.push_back(o.getString("id").value_or(""));
            BOOST_TEST((ids == std::vector<std::string>{"cue-tcp", "bo"}));
        }
    BOOST_TEST(seen);

    // Several changes in one turn of the event loop: one outputs item.
    f.fx->t.call([&] {
        InputManager& in = f.fx->engine->inputs();
        in.setOutputEnabled("bo", false);
        in.setOutputEnabled("bo", true);
        in.setOutputEnabled("cue-tcp", false);
    });
    const std::string one = c.waitForMessage({"\"type\":\"outputs\"", "\"id\":\"cue-tcp\",\"name\":\"Cue stream (TCP listener)\",\"kind\":\"cue\",\"enabled\":false"});
    BOOST_REQUIRE(!one.empty());
    BOOST_TEST(countOf(one, "\"type\":\"outputs\"") == 1u, one);

    // A removal over the API goes out too.
    std::size_t before = 0;
    {
        std::lock_guard<std::mutex> lk(c.m);
        before = c.msgs.size();
    }
    BOOST_TEST(f.call("DELETE", "/api/outputs/bo").status == 204u);
    BOOST_TEST(c.waitFor([&] {
        for (std::size_t i = before; i < c.msgs.size(); ++i)
            if (has(c.msgs[i], "\"type\":\"outputs\"") && !has(c.msgs[i], "\"id\":\"bo\""))
                return true;
        return false;
    }));
}

BOOST_AUTO_TEST_CASE(inputs_routes_unchanged_for_v1_clients)
{
    // The same inputs session with and without a hub: every status and body,
    // and the inputs file, are byte-identical, and the file stays version 1.
    auto session = [](bool with_hub, const uint16_t port, std::vector<std::string>& log, std::string& file) {
        Fx f(false, with_hub);
        auto note = [&](const ApiResponse& r) { log.push_back(std::to_string(r.status) + " " + r.body); };
        note(f.get("/api/inputs"));
        const std::string def = R"({"id":"ps","type":"pingstation_json","bind":"127.0.0.1","port":)" + std::to_string(port) + "}";
        auto strip = [](ApiResponse r) {  // a state that depends on timing is not what is compared
            const auto at = r.body.find("\"state\"");
            if (at != std::string::npos)
                r.body.erase(at);
            return r;
        };
        note(strip(f.call("POST", "/api/inputs", def)));
        note(f.call("POST", "/api/inputs", def));
        note(f.call("POST", "/api/inputs", "nonsense"));
        note(strip(f.call("PUT", "/api/inputs/ps", def)));
        note(strip(f.call("POST", "/api/inputs/ps/disable", "{}")));
        note(strip(f.call("POST", "/api/inputs/ps/enable", "{}")));
        note(f.call("PUT", "/api/inputs/readsb", R"({"enabled":true,"port":5})"));
        note(f.call("DELETE", "/api/inputs/readsb"));
        note(f.call("DELETE", "/api/inputs/nope"));
        note(f.call("POST", "/api/inputs/ps/explode", "{}"));
        f.fx->t.call([] {});
        file = readAll(f.inputs_file);
        note(f.call("DELETE", "/api/inputs/ps"));
        note(f.get("/api/inputs"));
    };
    const uint16_t port = freeUdpPort();
    std::vector<std::string> with, without;
    std::string file_with, file_without;
    session(true, port, with, file_with);
    session(false, port, without, file_without);
    BOOST_REQUIRE(with.size() == without.size());
    for (std::size_t i = 0; i < with.size(); ++i)
        BOOST_TEST(with[i] == without[i]);
    BOOST_TEST(file_with == file_without);
    BOOST_TEST(has(file_with, "\"version\": 1"), file_with);
    BOOST_TEST(!has(file_with, "outputs"), file_with);
    BOOST_TEST(!has(file_with, "cue-"), file_with);
    BOOST_TEST(has(with.front(), "{\"inputs\":[{\"id\":\"readsb\",\"name\":\"\",\"type\":\"readsb\",\"enabled\":true"), with.front());
}
