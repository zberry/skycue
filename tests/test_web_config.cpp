#define BOOST_TEST_MODULE web_config
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
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
#include <unistd.h>

#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <thread>

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

// skycue.toml in a temp dir, an Engine recording to <tmp>/rec, a
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

    // `setup`, when given, runs first with the temp dir: it may write files
    // there and returns more skycue.toml entries (dotted key -> TOML literal).
    using ConfMap = std::map<std::string, std::string>;
    // `set_args` and `env` are the --set list and the SKYCUE_* variables the
    // daemon runs with.
    explicit ConfigFx(const std::function<ConfMap(const std::string& dir)>& setup = nullptr,
                      const std::vector<std::string>& set_args = {},
                      const std::map<std::string, std::string>& env = {})
    {
        conf = tmp.path + "/skycue.toml";
        rec = tmp.path + "/rec";
        inputs_file = tmp.path + "/skycue.inputs.json";
        ::mkdir(rec.c_str(), 0755);
        ConfMap kv{{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"filters.max_range_nm", "120"}};
        if (setup)
            for (const auto& [k, v] : setup(tmp.path))
                kv[k] = v;
        std::ofstream(conf) << skycue_test::tomlConf(kv);
        const std::string overrides = skycued::overridesPathOf(conf, set_args, env);
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf, overrides, set_args, env), fakeSource(src), 10s,
                                        rec);
        info.version = "skycued test";
        info.supervisor = "none";
        info.started = std::chrono::system_clock::now();
        info.config_path = conf;
        info.www_dir = tmp.path + "/www";
        info.map_dir = tmp.path + "/maps";
        info.log_level = "info";
        info.inputs_path = inputs_file;
        info.cli_overrides = {{"web.port", "0"}};
        info.recorders = {{"received", fx->received.get()}, {"sent", fx->sent.get()}, {"fisb", nullptr}};
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->setLayers(set_args, env);
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
    expectKey(keys, "receiver.lat", "38.9", "conf", true);
    expectKey(keys, "filters.max_range_nm", "120", "conf", false);
    expectKey(keys, "cues.min_interval_s", "1", "default", true);
    expectKey(keys, "web.port", "0", "flag", true);
    expectKey(keys, "recording.formats", "csv jsonl", "default", true);

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
    BOOST_TEST(daemon->getString("map_dir").value_or("") == f.tmp.path + "/maps");
    BOOST_TEST(daemon->getString("log_level").value_or("") == "info");
    BOOST_TEST(daemon->getString("record_dir").has_value());
    const auto files = daemon->getStringArray("recording_files");
    BOOST_REQUIRE(files);
    for (const auto& name : *files)
        BOOST_TEST(name.find('/') == std::string::npos, name);
}

BOOST_AUTO_TEST_CASE(overrides_source_reported)
{
    ConfigFx f([](const std::string& dir) {
        std::ofstream(dir + "/skycue.overrides.toml") << "[filters]\nmax_range_nm = 90\n";
        return ConfigFx::ConfMap{{"paths.state_dir", "\"" + dir + "\""}};
    });
    auto r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    auto config = parsed(r.body);
    expectKey(keysOf(config), "filters.max_range_nm", "90", "overrides", false);
    expectKey(keysOf(config), "filters.min_elevation_deg", "0", "default", false);
    expectKey(keysOf(config), "paths.state_dir", f.tmp.path, "conf", true);
    auto daemon = config.getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getString("overrides_path").value_or("?") == f.tmp.path + "/skycue.overrides.toml");
    // The overlay still ranks first.
    const auto put = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST_REQUIRE(put.status == 200u, put.body);
    r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    config = parsed(r.body);
    expectKey(keysOf(config), "filters.max_range_nm", "20", "runtime", false);
}

BOOST_AUTO_TEST_CASE(overrides_path_beside_the_conf_without_state_dir)
{
    ConfigFx f;
    const auto r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto daemon = parsed(r.body).getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getString("overrides_path").value_or("?") == f.tmp.path + "/skycue.overrides.toml");
}

BOOST_AUTO_TEST_CASE(overlay_source_after_put)
{
    ConfigFx f;
    const auto put = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST_REQUIRE(put.status == 200u, put.body);
    const auto r = request(f.port, http::verb::get, "/api/config");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto config = parsed(r.body);
    expectKey(keysOf(config), "filters.max_range_nm", "20", "runtime", false);
    expectKey(keysOf(config), "filters.min_elevation_deg", "0", "default", false);
    const auto daemon = config.getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getBool("overlay_active").value_or(false) == true);
}

BOOST_AUTO_TEST_CASE(api_filters_pinned_key_409)
{
    ConfigFx f(nullptr, {"filters.max_range_nm=88"}, {{"SKYCUE_FILTERS_MIN_ELEVATION_DEG", "2"}});
    auto r = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST(r.status == 409u, r.body);
    auto body = parsed(r.body);
    BOOST_TEST(body.getString("key").value_or("") == "filters.max_range_nm");
    BOOST_TEST(body.getString("source").value_or("") == "set");
    BOOST_TEST(body.getString("error").value_or("").find("filters.max_range_nm") != std::string::npos);
    r = request(f.port, http::verb::put, "/api/filters", "{\"min_elevation_deg\":5}");
    BOOST_TEST(r.status == 409u, r.body);
    BOOST_TEST(parsed(r.body).getString("source").value_or("") == "env");
    r = request(f.port, http::verb::put, "/api/filters", "{\"min_elevation_deg\":null}");
    BOOST_TEST(r.status == 409u, r.body);
    // Nothing changed, and the running values are the pins'.
    r = request(f.port, http::verb::get, "/api/filters");
    BOOST_TEST(r.body == "{\"max_range_nm\":88,\"min_elevation_deg\":2,\"overlay_active\":false}", r.body);
}

BOOST_AUTO_TEST_CASE(api_config_dotted_keys_and_sources)
{
    // One key from each source: conf, overrides, env, --set, flag, runtime, and a default.
    ConfigFx f(
        [](const std::string& dir) {
            std::ofstream(dir + "/skycue.overrides.toml") << "[filters]\nmin_elevation_deg = 4\n";
            return ConfigFx::ConfMap{{"paths.state_dir", "\"" + dir + "\""}, {"receiver.alt_m", "30"}};
        },
        {"stats.save_s=60"}, {{"SKYCUE_WEB_TRAIL_SECONDS", "300"}});
    auto config = parsed(request(f.port, http::verb::get, "/api/config").body);
    auto keys = keysOf(config);
    BOOST_TEST(keys.order == skycued::knownConfigKeys(), boost::test_tools::per_element());
    for (const auto& name : keys.order)
        BOOST_TEST(name.find('.') != std::string::npos, name);  // dotted, never FOO_BAR
    expectKey(keys, "receiver.alt_m", "30", "conf", true);
    expectKey(keys, "filters.min_elevation_deg", "4", "overrides", false);
    expectKey(keys, "web.trail_seconds", "300", "env", true);
    expectKey(keys, "stats.save_s", "60", "set", true);
    expectKey(keys, "web.port", "0", "flag", true);
    expectKey(keys, "cues.keyframe_interval_s", "10", "default", true);
    const auto put = request(f.port, http::verb::put, "/api/filters", "{\"max_range_nm\":20}");
    BOOST_TEST_REQUIRE(put.status == 200u, put.body);
    config = parsed(request(f.port, http::verb::get, "/api/config").body);
    keys = keysOf(config);
    expectKey(keys, "filters.max_range_nm", "20", "runtime", false);
    expectKey(keys, "web.trail_seconds", "300", "env", true);
    expectKey(keys, "stats.save_s", "60", "set", true);
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
    req.method = "DELETE";
    const auto put = f.fx->t.call([&] {
        skycued::webapi::ApiContext ctx{*f.fx->engine, *f.rc, f.tmp.path, &f.info};
        return skycued::webapi::handleApi(req, ctx);
    });
    BOOST_TEST(put.status == 405u);
}

BOOST_AUTO_TEST_CASE(config_daemon_mode_and_locations)
{
    ConfigFx f;
    // Live by default, both built-in decoders used, no location files.
    auto daemon = parsed(request(f.port, http::verb::get, "/api/config").body).getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getString("mode").value_or("") == "live");
    BOOST_TEST(!daemon->getObject("replay").has_value());
    BOOST_TEST(daemon->getBool("readsb").value_or(false) == true);
    BOOST_TEST(daemon->getBool("dump978").value_or(false) == true);
    auto loc = daemon->getObject("locations");
    BOOST_REQUIRE(loc);
    BOOST_TEST(loc->getString("learned_path").value_or("x") == "");
    BOOST_TEST(loc->getString("operator_path").value_or("x") == "");
    BOOST_TEST(loc->getInt("learned").value_or(-1) == 0);
    BOOST_TEST(loc->getInt("operator").value_or(-1) == 0);

    // A replay without readsb/dump978, a learned file and an operator CSV
    // (two rows, one rejected line).
    const std::string learned = f.tmp.path + "/fisb-locations.json", csv = f.tmp.path + "/locs.csv";
    std::ofstream(learned) << "{\"version\":1,\"locations\":{\"KPRG\":[39.699097,-87.670212,3]}}\n";
    std::ofstream(csv) << "id,lat,lon\nIND,39.81,-86.37\nROD,40.29,-84.04\nnot a row\n";
    f.fx->t.call([&] {
        f.info.replay_file = "/data/received.jsonl";
        f.info.replay_speed = 0;
        f.info.replay_loop = true;
        f.info.readsb = f.info.dump978 = false;
        f.fx->engine->loadLocations(learned, csv);
    });
    daemon = parsed(request(f.port, http::verb::get, "/api/config").body).getObject("daemon");
    BOOST_REQUIRE(daemon);
    BOOST_TEST(daemon->getString("mode").value_or("") == "replay");
    const auto replay = daemon->getObject("replay");
    BOOST_REQUIRE(replay);
    BOOST_TEST(replay->getString("file").value_or("") == "/data/received.jsonl");
    BOOST_TEST(replay->getDouble("speed").value_or(-1) == 0.0);
    BOOST_TEST(replay->getBool("loop").value_or(false) == true);
    BOOST_TEST(daemon->getBool("readsb").value_or(true) == false);
    BOOST_TEST(daemon->getBool("dump978").value_or(true) == false);
    loc = daemon->getObject("locations");
    BOOST_REQUIRE(loc);
    BOOST_TEST(loc->getString("learned_path").value_or("") == learned);
    BOOST_TEST(loc->getInt("learned").value_or(-1) == 1);
    BOOST_TEST(loc->getString("operator_path").value_or("") == csv);
    BOOST_TEST(loc->getInt("operator").value_or(-1) == 2);
    BOOST_TEST(loc->getInt("operator_rejected").value_or(-1) == 1);
    BOOST_TEST(loc->getInt("operator_overflow").value_or(-1) == 0);
}

BOOST_AUTO_TEST_CASE(schema_lists_every_key_with_type_and_class)
{
    ConfigFx f;
    const auto r = request(f.port, http::verb::get, "/api/config/schema");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto keys = keysOf(parsed(r.body));
    BOOST_TEST(keys.order == skycued::knownConfigKeys(), boost::test_tools::per_element());
    const auto& port = keys.by_key.at("web.port");
    BOOST_TEST(port.getString("type").value_or("") == "int");
    BOOST_TEST(port.getString("class").value_or("") == "bootstrap");
    BOOST_TEST(port.getDouble("min").value_or(-1) == 0);
    BOOST_TEST(port.getDouble("max").value_or(-1) == 65535);
    const auto& dec = keys.by_key.at("inputs.decoder");
    BOOST_TEST(dec.getString("type").value_or("") == "enum");
    BOOST_TEST((dec.getStringArray("values").value_or(std::vector<std::string>{}) ==
                std::vector<std::string>{"readsb", "skycued"}));
    const auto& fmt = keys.by_key.at("recording.formats");
    BOOST_TEST(fmt.getString("type").value_or("") == "string_set");
    BOOST_TEST((fmt.getStringArray("values").value_or(std::vector<std::string>{}) ==
                std::vector<std::string>{"jsonl", "csv"}));
    BOOST_TEST(keys.by_key.at("cues.udp_targets").getString("type").value_or("") == "host_port_list");
    BOOST_TEST(keys.by_key.at("paths.map_dir").getString("type").value_or("") == "path");
    BOOST_TEST(keys.by_key.at("filters.max_range_nm").getString("class").value_or("") == "live");
    BOOST_TEST(keys.by_key.at("sdr.gain_1090").getString("class").value_or("") == "external");
    BOOST_TEST(keys.by_key.at("receiver.lat").has("min"));
    BOOST_TEST(keys.by_key.at("receiver.lat").getString("doc").value_or("") == "decimal degrees");
    BOOST_TEST(keys.by_key.at("stats.aircraft_max").getString("doc").value_or("").rfind("past it", 0) == 0);
    BOOST_TEST(keys.by_key.at("web.allow_restart").getString("doc").value_or("") ==
               "let the web UI restart skycued; only under a supervisor that starts it again");
    BOOST_TEST(keys.by_key.at("web.trail_seconds").getString("default").value_or("") == "120");
    for (const auto& [name, entry] : keys.by_key) {
        BOOST_TEST(!entry.getString("doc").value_or("").empty(), name);
        BOOST_TEST(entry.has("default"), name);
    }
    BOOST_TEST(request(f.port, http::verb::put, "/api/config/schema", "{}").status == 405u);
}

BOOST_AUTO_TEST_CASE(config_entries_carry_class_editable_file_value_default)
{
    {
        ConfigFx f([](const std::string&) { return ConfigFx::ConfMap{{"web.trail_seconds", "300"}}; });
        const auto keys = keysOf(parsed(request(f.port, http::verb::get, "/api/config").body));
        const auto& t = keys.by_key.at("web.trail_seconds");
        BOOST_TEST(t.getString("class").value_or("") == "restart");
        BOOST_TEST(t.getBool("editable").value_or(false));
        BOOST_TEST(t.getString("file_value").value_or("?") == "300");
        BOOST_TEST(t.getString("default").value_or("?") == "120");
        BOOST_TEST(!keys.by_key.at("web.bind").getBool("editable").value_or(true));
        BOOST_TEST(!keys.by_key.at("cues.min_interval_s").has("file_value"));
    }
    ConfigFx g(nullptr, {"filters.max_range_nm=88"});
    const auto keys = keysOf(parsed(request(g.port, http::verb::get, "/api/config").body));
    const auto& m = keys.by_key.at("filters.max_range_nm");
    BOOST_TEST(m.getString("source").value_or("") == "set");
    BOOST_TEST(!m.getBool("editable").value_or(true));
    ConfigFx h(nullptr, {}, {{"SKYCUE_FILTERS_MAX_RANGE_NM", "77"}});
    const auto ek = keysOf(parsed(request(h.port, http::verb::get, "/api/config").body));
    BOOST_TEST(ek.by_key.at("filters.max_range_nm").getString("source").value_or("") == "env");
    BOOST_TEST(!ek.by_key.at("filters.max_range_nm").getBool("editable").value_or(true));
}

BOOST_AUTO_TEST_CASE(file_value_follows_hand_edits)
{
    ConfigFx f([](const std::string&) { return ConfigFx::ConfMap{{"web.trail_seconds", "300"}}; });
    f.fx->t.call([&] { f.rc->startWatching(); });
    const auto write = [&](ConfigFx::ConfMap extra) {
        ConfigFx::ConfMap kv{{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"filters.max_range_nm", "120"}};
        for (const auto& e : extra)
            kv[e.first] = e.second;
        std::ofstream(f.conf) << skycue_test::tomlConf(kv);
    };
    const auto fileValue = [&](const char* key) {
        const auto keys = keysOf(parsed(request(f.port, http::verb::get, "/api/config").body));
        return keys.by_key.at(key).getString("file_value");
    };
    const auto waitFor = [&](const char* key, std::optional<std::string> want) {
        for (int i = 0; i < 100 && fileValue(key) != want; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return fileValue(key);
    };
    write({{"web.trail_seconds", "400"}, {"cues.min_interval_s", "2"}});
    BOOST_TEST(waitFor("web.trail_seconds", "400").value_or("?") == "400");
    BOOST_TEST(waitFor("cues.min_interval_s", "2").value_or("?") == "2");
    write({{"web.trail_seconds", "400"}});
    BOOST_TEST(!waitFor("cues.min_interval_s", std::nullopt).has_value());
}

namespace {
// Waits (up to 5 s) until pending_restart is non-empty (`want`) or empty.
void waitPending(ConfigFx& f, bool want)
{
    for (int i = 0; i < 100; ++i) {
        const auto p = parsed(request(f.port, http::verb::get, "/api/config").body).getObjectArray("pending_restart");
        if (p && p->empty() != want)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
}  // namespace

BOOST_AUTO_TEST_CASE(pending_restart_lists_hand_edited_restart_keys)
{
    ConfigFx f;
    f.fx->t.call([&] { f.rc->startWatching(); });
    BOOST_TEST(parsed(request(f.port, http::verb::get, "/api/config").body).getObjectArray("pending_restart")->empty());
    std::ofstream(f.conf) << skycue_test::tomlConf({{"receiver.lat", "38.9"},
                                                    {"receiver.lon", "-77.0"},
                                                    {"filters.max_range_nm", "120"},
                                                    {"fisb.enable", "false"}});
    waitPending(f, true);
    const auto p = parsed(request(f.port, http::verb::get, "/api/config").body).getObjectArray("pending_restart");
    BOOST_TEST_REQUIRE(p.has_value());
    BOOST_TEST_REQUIRE(p->size() == 1u);
    BOOST_TEST((*p)[0].getString("key").value_or("") == "fisb.enable");
    BOOST_TEST((*p)[0].getString("running").value_or("") == "true");
    BOOST_TEST((*p)[0].getString("configured").value_or("") == "false");
    std::ofstream(f.conf) << skycue_test::tomlConf(
        {{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"filters.max_range_nm", "120"}});
    waitPending(f, false);
    BOOST_TEST(parsed(request(f.port, http::verb::get, "/api/config").body).getObjectArray("pending_restart")->empty());
}

BOOST_AUTO_TEST_CASE(daemon_reports_allow_restart_and_supervisor)
{
    ConfigFx f;
    const auto daemon = parsed(request(f.port, http::verb::get, "/api/config").body).getObject("daemon");
    BOOST_TEST_REQUIRE(daemon.has_value());
    BOOST_TEST(!daemon->getBool("allow_restart").value_or(true));
    BOOST_TEST(daemon->getString("supervisor").value_or("") == "none");
}

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Reply put(ConfigFx& f, const std::string& body) { return request(f.port, http::verb::put, "/api/config", body); }

std::vector<std::string> names(const JsonObject& o, const char* key)
{
    return o.getStringArray(key).value_or(std::vector<std::string>{"<missing>"});
}

}  // namespace

BOOST_AUTO_TEST_CASE(put_sets_restart_key_and_reports_pending)
{
    ConfigFx f;
    const auto r = put(f, R"({"set":{"fisb.enable":false}})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto j = parsed(r.body);
    BOOST_TEST(j.getBool("ok").value_or(false));
    BOOST_TEST(names(j, "applied") == std::vector<std::string>{"fisb.enable"}, boost::test_tools::per_element());
    const auto p = j.getObjectArray("pending_restart");
    BOOST_TEST_REQUIRE((p && p->size() == 1u));
    BOOST_TEST((*p)[0].getString("key").value_or("") == "fisb.enable");
    BOOST_TEST((*p)[0].getString("running").value_or("") == "true");
    BOOST_TEST((*p)[0].getString("configured").value_or("") == "false");
    BOOST_TEST(slurp(f.tmp.path + "/skycue.overrides.toml").find("[fisb]\nenable = false") != std::string::npos);
    const auto keys = keysOf(parsed(request(f.port, http::verb::get, "/api/config").body));
    BOOST_TEST(keys.by_key.at("fisb.enable").getString("source").value_or("") == "overrides");
    BOOST_TEST(!keys.by_key.at("fisb.enable").has("file_value"));
}

BOOST_AUTO_TEST_CASE(put_live_key_applies_at_once)
{
    ConfigFx f;
    const auto r = put(f, R"({"set":{"filters.max_range_nm":150}})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto j = parsed(r.body);
    BOOST_TEST(names(j, "applied") == std::vector<std::string>{"filters.max_range_nm"}, boost::test_tools::per_element());
    BOOST_TEST(j.getObjectArray("pending_restart")->empty());
    BOOST_TEST(f.fx->t.call([&] { return f.fx->engine->snapshot().filters.max_range_nm; }) == 150.0);
    const auto h = parsed(request(f.port, http::verb::get, "/api/filters/history").body).getObjectArray("changes");
    BOOST_TEST_REQUIRE((h && !h->empty()));
    BOOST_TEST(h->back().getString("origin").value_or("") == "ui");
}

BOOST_AUTO_TEST_CASE(put_empty_leaves_a_missing_overrides_file_missing)
{
    ConfigFx f;
    const auto r = put(f, R"({"set":{},"clear":[]})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    BOOST_TEST(parsed(r.body).getStringArray("applied")->empty());
    BOOST_TEST(::access((f.tmp.path + "/skycue.overrides.toml").c_str(), F_OK) != 0);
}

BOOST_AUTO_TEST_CASE(put_filter_key_clears_overlay_value)
{
    ConfigFx f;
    BOOST_TEST_REQUIRE(request(f.port, http::verb::put, "/api/filters", R"({"max_range_nm":130})").status == 200u);
    BOOST_TEST(slurp(f.rc->overlayPath()).find("max_range_nm") != std::string::npos);
    const auto r = put(f, R"({"set":{"filters.max_range_nm":140}})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    BOOST_TEST(f.fx->t.call([&] { return f.fx->engine->snapshot().filters.max_range_nm; }) == 140.0);
    BOOST_TEST(slurp(f.rc->overlayPath()).find("max_range_nm") == std::string::npos);
    const auto keys = keysOf(parsed(request(f.port, http::verb::get, "/api/config").body));
    BOOST_TEST(keys.by_key.at("filters.max_range_nm").getString("source").value_or("") == "overrides");
}

BOOST_AUTO_TEST_CASE(put_rejects_wrong_json_types)
{
    ConfigFx f;
    for (const char* body : {R"({"set":{"web.trail_seconds":"300"}})", R"({"set":{"fisb.enable":1}})",
                             R"({"set":{"web.trail_seconds":1.5}})", R"({"set":{"recording.formats":"csv"}})"}) {
        const auto r = put(f, body);
        BOOST_TEST(r.status == 400u, body);
        BOOST_TEST(parsed(r.body).getString("key").has_value(), r.body);
    }
    const auto r = put(f, R"({"set":{"web.trail_seconds":"300"}})");
    BOOST_TEST(parsed(r.body).getString("key").value_or("") == "web.trail_seconds");
    BOOST_TEST(parsed(r.body).getString("error").value_or("") == "web.trail_seconds: expected int");
    BOOST_TEST(slurp(f.tmp.path + "/skycue.overrides.toml").empty());
}

BOOST_AUTO_TEST_CASE(put_bootstrap_403_pinned_409_unknown_400)
{
    {
        ConfigFx f;
        const auto r = put(f, R"({"set":{"web.bind":"0.0.0.0"}})");
        BOOST_TEST(r.status == 403u, r.body);
        BOOST_TEST(parsed(r.body).getString("key").value_or("") == "web.bind");
        BOOST_TEST(put(f, R"({"set":{"nope.key":1}})").status == 400u);
        BOOST_TEST(put(f, "[1]").status == 400u);
    }
    ConfigFx g(nullptr, {"filters.min_elevation_deg=2"});
    const auto r = put(g, R"({"set":{"filters.min_elevation_deg":3}})");
    BOOST_TEST(r.status == 409u, r.body);
    BOOST_TEST(parsed(r.body).getString("key").value_or("") == "filters.min_elevation_deg");
    BOOST_TEST(parsed(r.body).getString("source").value_or("") == "set");
}

BOOST_AUTO_TEST_CASE(put_cross_key_error_names_key)
{
    ConfigFx f([](const std::string&) { return ConfigFx::ConfMap{{"cues.max_interval_s", "10"}}; });
    const auto r = put(f, R"({"set":{"cues.min_interval_s":12}})");
    BOOST_TEST(r.status == 400u, r.body);
    const std::string e = parsed(r.body).getString("error").value_or("");
    BOOST_TEST(e.find("cues.min_interval_s") != std::string::npos, e);
    BOOST_TEST(e.find("cues.max_interval_s") != std::string::npos, e);
    BOOST_TEST(e.find("skycue") == std::string::npos, e);
    BOOST_TEST(parsed(r.body).getString("key").value_or("") == "cues.max_interval_s");
    BOOST_TEST(slurp(f.tmp.path + "/skycue.overrides.toml").empty());
}

BOOST_AUTO_TEST_CASE(put_clear_is_idempotent)
{
    ConfigFx f;
    auto r = put(f, R"({"clear":["web.trail_seconds"]})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    BOOST_TEST(parsed(r.body).getStringArray("applied")->empty());
    BOOST_TEST(put(f, R"({"set":{"web.trail_seconds":300}})").status == 200u);
    BOOST_TEST(slurp(f.tmp.path + "/skycue.overrides.toml").find("trail_seconds = 300") != std::string::npos);
    r = put(f, R"({"clear":["web.trail_seconds"]})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    BOOST_TEST(slurp(f.tmp.path + "/skycue.overrides.toml").find("trail_seconds") == std::string::npos);
    BOOST_TEST(parsed(r.body).getObjectArray("pending_restart")->empty());
}

BOOST_AUTO_TEST_CASE(put_external_key_listed_as_external)
{
    ConfigFx f;
    const auto r = put(f, R"({"set":{"sdr.gain_1090":"49.6"}})");
    BOOST_TEST_REQUIRE(r.status == 200u, r.body);
    const auto j = parsed(r.body);
    BOOST_TEST(names(j, "external") == std::vector<std::string>{"sdr.gain_1090"}, boost::test_tools::per_element());
    BOOST_TEST(j.getObjectArray("pending_restart")->empty());
}

BOOST_AUTO_TEST_CASE(put_unwritable_dir_500_names_state_dir)
{
    if (::geteuid() == 0)
        return;
    ConfigFx f;
    ::chmod(f.tmp.path.c_str(), 0500);
    const auto r = put(f, R"({"set":{"fisb.enable":false}})");
    ::chmod(f.tmp.path.c_str(), 0700);
    BOOST_TEST(r.status == 500u, r.body);
    const std::string e = parsed(r.body).getString("error").value_or("");
    BOOST_TEST(e.find(f.tmp.path) != std::string::npos, e);
    BOOST_TEST(e.find("paths.state_dir") != std::string::npos, e);
}
