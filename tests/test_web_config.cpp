#define BOOST_TEST_MODULE web_config
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"

#include "config.hpp"
#include "json_lite.hpp"
#include "runtime_config.hpp"
#include "web_api.hpp"
#include "web_config.hpp"
#include "web_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <sys/stat.h>

#include <map>

using namespace enginetest;
using skycued::DaemonInfo;
using skycued::JsonObject;
using skycued::RuntimeConfig;
using skycued::WebConfig;
using skycued::WebServer;
namespace beast = boost::beast;
namespace http = boost::beast::http;

namespace {

struct Reply {
    unsigned status = 0;
    std::string body;
};

Reply request(uint16_t port, http::verb verb, const std::string& target, const std::string& body = "")
{
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    if (verb == http::verb::put) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
    }
    req.keep_alive(false);
    req.prepare_payload();
    http::write(s, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(s, buf, res);
    return Reply{res.result_int(), res.body()};
}

JsonObject parsed(const std::string& text)
{
    auto o = JsonObject::parse(text);
    if (!o)
        throw std::runtime_error("not a JSON object: " + text);
    return *o;
}

// "keys" as key -> entry, plus the order they came in.
struct Keys {
    std::vector<std::string> order;
    std::map<std::string, JsonObject> by_key;
};

Keys keysOf(const JsonObject& config)
{
    Keys out;
    const auto list = config.getObjectArray("keys");
    BOOST_REQUIRE(list);
    for (const auto& k : *list) {
        const std::string name = k.getString("key").value_or("");
        out.order.push_back(name);
        out.by_key.emplace(name, k);
    }
    return out;
}

void expectKey(const Keys& keys, const std::string& key, const std::string& value, const std::string& source,
               bool restart)
{
    const auto it = keys.by_key.find(key);
    BOOST_REQUIRE_MESSAGE(it != keys.by_key.end(), key);
    BOOST_TEST(it->second.getString("value").value_or("?") == value, key);
    BOOST_TEST(it->second.getString("source").value_or("?") == source, key);
    BOOST_TEST(it->second.getBool("restart").value_or(!restart) == restart, key);
}

// skycue.conf in a temp dir, an Engine recording to <tmp>/rec, a
// RuntimeConfig and a WebServer on an ephemeral port with DaemonInfo.
struct ConfigFx {
    TempDir tmp;
    std::string conf, rec, inputs_file;
    FakeSource* src = nullptr;
    DaemonInfo info;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<RuntimeConfig> rc;
    std::unique_ptr<WebServer> web;
    uint16_t port = 0;

    ConfigFx()
    {
        conf = tmp.path + "/skycue.conf";
        rec = tmp.path + "/rec";
        inputs_file = tmp.path + "/skycue.inputs.json";
        ::mkdir(rec.c_str(), 0755);
        std::ofstream(conf) << "RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\nMAX_RANGE_NM=120\n";
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), fakeSource(src), 10s, rec);
        info.version = "skycued test";
        info.started = std::chrono::system_clock::now();
        info.config_path = conf;
        info.www_dir = tmp.path + "/www";
        info.log_level = "info";
        info.inputs_path = inputs_file;
        info.cli_overrides = {{"WEB_PORT", "0"}};
        info.recorders = {{"received", fx->received.get()}, {"sent", fx->sent.get()}, {"fisb", nullptr}};
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
            WebConfig wc;
            wc.port = 0;
            wc.www_dir = info.www_dir;
            wc.daemon = &info;
            web = std::make_unique<WebServer>(fx->t.io, *fx->engine, *rc, wc);
            web->start();
            port = web->localPort();
        });
    }
    ~ConfigFx()
    {
        try {
            fx->t.call([&] {
                web.reset();
                rc.reset();
            });
        } catch (...) {
        }
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(config_json_sources_and_values)
{
    ConfigFx f;
    const auto r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto config = parsed(r.body);
    const auto keys = keysOf(config);
    BOOST_TEST(keys.order == skycued::knownConfigKeys(), boost::test_tools::per_element());
    expectKey(keys, "RECEIVER_LAT", "38.9", "conf", true);
    expectKey(keys, "MAX_RANGE_NM", "120", "conf", false);
    expectKey(keys, "MIN_INTERVAL_S", "1", "default", true);
    expectKey(keys, "WEB_PORT", "0", "cli", true);
    expectKey(keys, "RECORD_FORMATS", "csv jsonl", "default", true);

    const auto daemon = config.getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getString("version").value_or("") == "skycued test");
    BOOST_TEST(daemon->getInt("uptime_s").value_or(-1) >= 0);
    BOOST_TEST(daemon->getString("started").has_value());
    BOOST_TEST(daemon->getBool("overlay_active").value_or(true) == false);
    BOOST_TEST(daemon->getString("inputs_path").value_or("") == f.inputs_file);
    BOOST_TEST(daemon->getString("config_path").value_or("") == f.conf);
    BOOST_TEST(daemon->getString("overlay_path").value_or("") == f.tmp.path + "/skycue.runtime.json");
    BOOST_TEST(daemon->getString("www_dir").value_or("") == f.tmp.path + "/www");
    BOOST_TEST(daemon->getString("log_level").value_or("") == "info");
    BOOST_TEST(daemon->getString("record_dir").has_value());
    const auto files = daemon->getStringArray("recording_files");
    BOOST_REQUIRE(files);
    for (const auto& name : *files)
        BOOST_TEST(name.find('/') == std::string::npos, name);
}

BOOST_AUTO_TEST_CASE(overlay_source_after_put)
{
    ConfigFx f;
    const auto put = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST_REQUIRE(put.status == 200u, put.body);
    const auto r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto config = parsed(r.body);
    expectKey(keysOf(config), "MAX_RANGE_NM", "20", "overlay", false);
    expectKey(keysOf(config), "MIN_ELEVATION_DEG", "0", "default", false);
    const auto daemon = config.getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getBool("overlay_active").value_or(false) == true);
}

BOOST_AUTO_TEST_CASE(config_without_daemon_info_500)
{
    ConfigFx f;
    skycued::webapi::ApiRequest req;
    req.method = "GET";
    req.path = "/api/config";
    const auto r = f.fx->t.call([&] {
        skycued::webapi::ApiContext ctx{*f.fx->engine, *f.rc, f.tmp.path};
        return skycued::webapi::handleApi(req, ctx);
    });
    BOOST_TEST(r.status == 500u);
    BOOST_TEST(r.body == "{\"error\":\"no daemon info\"}");
    req.method = "PUT";
    const auto put = f.fx->t.call([&] {
        skycued::webapi::ApiContext ctx{*f.fx->engine, *f.rc, f.tmp.path, &f.info};
        return skycued::webapi::handleApi(req, ctx);
    });
    BOOST_TEST(put.status == 405u);
}
