#define BOOST_TEST_MODULE skycued_input_def
#include <boost/test/unit_test.hpp>

#include "file_util.hpp"
#include "inputs/input_def.hpp"
#include "inputs/inputs_file.hpp"
#include "outputs/output_def.hpp"
#include "json_lite.hpp"

#include <ftw.h>
#include <stdio.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace skycued;

namespace {

JsonObject obj(const std::string& text)
{
    auto o = JsonObject::parse(text);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + text);
    return *o;
}

// A valid definition of each type, as JSON members (without the braces), so
// a test can add or replace one key.
const char* const kReadsb = R"("id":"rb2","type":"readsb","host":"192.0.2.1","port":30047)";
const char* const kDump978 = R"("id":"uat2","type":"dump978","host":"uat.example","port":30978)";
const char* const kPsJson = R"("id":"ps-json","type":"pingstation_json","port":4000)";
const char* const kCatUdp = R"("id":"ps-north","type":"pingstation_cat021","transport":"udp","port":4001)";
const char* const kCatConnect =
    R"("id":"ps-south","type":"pingstation_cat021","transport":"tcp_connect","host":"192.0.2.10","port":4002)";

InputDef def(const std::string& members) { return parseInputDef(obj("{" + members + "}")); }

// The InputDefError text for `members`, or "" when they parse.
std::string err(const std::string& members)
{
    try {
        def(members);
    } catch (const InputDefError& e) {
        return e.what();
    }
    return "";
}

bool ok(const std::string& members) { return err(members).empty(); }

// kCatUdp (a type to which nearly every key applies) with one key added.
std::string cat(const std::string& extra) { return std::string(kCatUdp) + "," + extra; }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string inputsFileErr(const std::string& text)
{
    try {
        parseInputsFile(text);
    } catch (const InputDefError& e) {
        return e.what();
    }
    return "";
}

std::string setErr(const std::vector<InputDef>& set)
{
    try {
        validateInputSet(set);
    } catch (const InputDefError& e) {
        return e.what();
    }
    return "";
}

int removeEntry(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }

struct TempDir {
    std::string path;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-input-def-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~TempDir() { ::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS); }
};

}  // namespace

BOOST_AUTO_TEST_CASE(id_rules)
{
    BOOST_TEST(def(kCatUdp).id == "ps-north");
    BOOST_TEST(ok(R"("id":"a","type":"pingstation_json","port":4000)"));
    BOOST_TEST(ok(R"("id":"0123456789abcdef0123456789abcdef","type":"pingstation_json","port":4000)"));
    for (const char* bad : {"", "0123456789abcdef0123456789abcdefg", "PS", "a_b", "a b", "ps.north"}) {
        const std::string e = err(std::string(R"("id":")") + bad + R"(","type":"pingstation_json","port":4000)");
        BOOST_TEST(contains(e, "id"), bad << " -> " << e);
    }
}

BOOST_AUTO_TEST_CASE(name_rules)
{
    BOOST_TEST(def(cat(R"("name":")" + std::string(64, 'n') + "\"")).name == std::string(64, 'n'));
    BOOST_TEST(def(cat(R"("name":"pingStation north mast")")).name == "pingStation north mast");
    BOOST_TEST(ok(cat(R"("name":"Mât nord \u00e9")")));  // UTF-8: counted in characters
    BOOST_TEST(contains(err(cat(R"("name":")" + std::string(65, 'n') + "\"")), "name"));
    BOOST_TEST(contains(err(cat(R"("name":"\u0001")")), "name"));
    BOOST_TEST(contains(err(cat(R"("name":"tab\there")")), "name"));
    BOOST_TEST(def(kCatUdp).name.empty());
}

BOOST_AUTO_TEST_CASE(name_rejects_c1_and_bad_utf8)
{
    const auto named = [](const std::string& n) { return cat(R"("name":")" + n + "\""); };
    // C1 controls are valid UTF-8, so they reach the name rule both raw and as \u escapes.
    BOOST_CHECK_THROW(def(named("a\xC2\x85" "b")), InputDefError);  // U+0085
    BOOST_CHECK_THROW(def(named("a\xC2\x9F" "b")), InputDefError);  // U+009F, the last C1 control
    BOOST_CHECK_THROW(def(named("\\u0080")), InputDefError);
    BOOST_TEST(contains(err(named("\\u0085")), "name"));
    BOOST_CHECK_NO_THROW(def(named("\xC3\xA9t\xC3\xA9")));
    BOOST_CHECK_NO_THROW(def(named("\xC2\xA0" "x")));      // U+00A0, just past C1
    BOOST_CHECK_NO_THROW(def(named("\xF0\x9F\x9B\xA9")));  // a 4-byte character
    // Invalid UTF-8. PropertyTree's parser refuses some of it (a 0xFF byte, a
    // truncated sequence); what it lets through, the name rule refuses.
    int reached = 0;
    for (const std::string bad : {"a\xFF" "b", "\xE2\x82", "a\x82" "b", "\xC0\xAF", "\xED\xA0\x80",
                                  "\xF4\x90\x80\x80", "\xE2\x82" "a"}) {
        BOOST_TEST(!isValidInputName(bad));
        if (const auto o = JsonObject::parse("{" + named(bad) + "}")) {
            ++reached;
            BOOST_CHECK_THROW(parseInputDef(*o), InputDefError);
        }
    }
    BOOST_TEST(reached > 0);  // the parser alone does not guard the name
    BOOST_TEST(isValidInputName("\xC3\xA9t\xC3\xA9"));
    BOOST_TEST(isValidInputName(""));
    BOOST_TEST(!isValidInputName("\xC2\x85"));
}

BOOST_AUTO_TEST_CASE(clip_for_message_keeps_utf8_whole)
{
    const std::string s = std::string(63, 'a') + "\xC3\xA9" + "zzz";
    BOOST_TEST(clipForMessage(s) == std::string(63, 'a') + "...");
    BOOST_TEST(clipForMessage(std::string(64, 'a')) == std::string(64, 'a'));
}

BOOST_AUTO_TEST_CASE(type_rules)
{
    BOOST_TEST((def(kReadsb).type == InputType::readsb));
    BOOST_TEST((def(kDump978).type == InputType::dump978));
    BOOST_TEST((def(kPsJson).type == InputType::pingstation_json));
    BOOST_TEST((def(kCatUdp).type == InputType::pingstation_cat021));
    BOOST_TEST(contains(err(R"("id":"x","type":"sbs","port":4000)"), "type"));
    BOOST_TEST(contains(err(R"("id":"x","type":"","port":4000)"), "type"));
    BOOST_TEST(contains(err(R"("id":"x","port":4000,"host":"h")"), "type"));
    // The fixed transports.
    BOOST_TEST((def(kReadsb).transport == InputTransport::tcp_connect));
    BOOST_TEST((def(kDump978).transport == InputTransport::tcp_connect));
    BOOST_TEST((def(kPsJson).transport == InputTransport::udp));
    BOOST_TEST((def(kCatUdp).transport == InputTransport::udp));
    BOOST_TEST((def(kCatConnect).transport == InputTransport::tcp_connect));
    BOOST_TEST((def(R"("id":"x","type":"pingstation_cat021","transport":"tcp_listen","port":4000)").transport ==
                InputTransport::tcp_listen));
    BOOST_TEST(contains(err(R"("id":"x","type":"pingstation_cat021","transport":"tcp","port":4000)"), "transport"));
    BOOST_TEST(std::string(inputTypeName(InputType::pingstation_cat021)) == "pingstation_cat021");
    BOOST_TEST(std::string(transportName(InputTransport::tcp_listen)) == "tcp_listen");
    BOOST_TEST(!isTrackType(InputType::dump978));
    BOOST_TEST(isTrackType(InputType::readsb));
    BOOST_TEST(isTrackType(InputType::pingstation_json));
    BOOST_TEST(isTrackType(InputType::pingstation_cat021));
    BOOST_TEST(isTrackType(InputType::replay));
}

BOOST_AUTO_TEST_CASE(port_rules)
{
    BOOST_TEST(def(kPsJson).port == 4000);
    BOOST_TEST(ok(R"("id":"x","type":"pingstation_json","port":1)"));
    BOOST_TEST(def(R"("id":"x","type":"pingstation_json","port":65535)").port == 65535);
    for (const char* bad : {"0", "65536", "-1", "4000.5", "\"eighty\"", "true", "null", "[]"})
        BOOST_TEST(contains(err(std::string(R"("id":"x","type":"pingstation_json","port":)") + bad), "port"), bad);
    // PropertyTree keeps every scalar as its text, so a quoted number cannot
    // be told from a bare one: "80" reads as the port 80.
    BOOST_TEST(def(R"("id":"x","type":"pingstation_json","port":"80")").port == 80);
}

BOOST_AUTO_TEST_CASE(bind_must_be_ipv4)
{
    BOOST_TEST(def(kCatUdp).bind == "0.0.0.0");
    BOOST_TEST(def(cat(R"("bind":"192.0.2.1")")).bind == "192.0.2.1");
    BOOST_TEST(def(cat(R"("bind":"255.255.255.255")")).bind == "255.255.255.255");
    for (const char* bad : {"localhost", "256.0.0.1", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.", "", "::1", " 1.2.3.4"})
        BOOST_TEST(contains(err(cat(std::string(R"("bind":")") + bad + "\"")), "bind"), bad);
}

// The load-time repair of a stored name (inputs file only; the API stays
// strict): each invalid UTF-8 byte becomes U+FFFD, control characters (C0,
// DEL, C1) are dropped, and the result is cut to 64 characters.
BOOST_AUTO_TEST_CASE(repair_input_name_makes_a_valid_name)
{
    using skycued::isValidInputName;
    using skycued::repairInputName;
    BOOST_TEST(repairInputName("North") == "North");
    BOOST_TEST(repairInputName("\xC3\xA9t\xC3\xA9") == "\xC3\xA9t\xC3\xA9");
    BOOST_TEST(repairInputName("ps\xC0\xAF" "b") == "ps\xEF\xBF\xBD\xEF\xBF\xBD" "b");
    BOOST_TEST(repairInputName("a\xC2\x85" "b\x01" "c\x7F") == "abc");
    BOOST_TEST(repairInputName("a\xE2\x82") == "a\xEF\xBF\xBD\xEF\xBF\xBD");  // cut short
    const std::string longName(70, 'x');
    BOOST_TEST(repairInputName(longName) == std::string(64, 'x'));
    std::string wide;
    for (int i = 0; i < 70; ++i)
        wide += "\xC3\xA9";
    const auto cut = repairInputName(wide);
    BOOST_TEST(cut.size() == 128u);  // 64 two-byte characters, none split
    for (const std::string& s : {std::string("ps\xC0\xAF"), std::string("\xED\xA0\x80"), std::string("\xF4\x90\x80\x80"),
                                 std::string("\x80\x80"), longName, wide})
        BOOST_TEST(isValidInputName(repairInputName(s)));
}

BOOST_AUTO_TEST_CASE(host_ipv4_or_hostname)
{
    auto host = [](const std::string& h) {
        return std::string(R"("id":"x","type":"readsb","port":30047,"host":")") + h + "\"";
    };
    BOOST_TEST(def(host("192.0.2.10")).host == "192.0.2.10");
    BOOST_TEST(ok(host("localhost")));
    BOOST_TEST(ok(host("ps-north.example.net")));
    BOOST_TEST(ok(host("ps-north.example.net.")));
    BOOST_TEST(ok(host("a1")));
    // 253 characters: labels of 63, 63, 63 and 61 and three dots.
    std::string h253 = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." +
                       std::string(61, 'd');
    BOOST_REQUIRE(h253.size() == 253);
    BOOST_TEST(ok(host(h253)));
    BOOST_TEST(contains(err(host(h253 + "e")), "host"));  // 254
    for (const char* bad : {"", "-ps", "ps-", "a..b", ".a", "a_b", "ps north", "999.1.1.1", "1.2.3.256", "[::1]",
                            "host:80"})
        BOOST_TEST(contains(err(host(bad)), "host"), bad);
    BOOST_TEST(contains(err(host(std::string(64, 'a'))), "host"));  // a label over 63
}

BOOST_AUTO_TEST_CASE(edition_2_2_to_2_7)
{
    BOOST_TEST(def(kCatUdp).edition == "2.5");
    for (const char* good : {"2.2", "2.3", "2.4", "2.5", "2.6", "2.7"})
        BOOST_TEST(def(cat(std::string(R"("edition":")") + good + "\"")).edition == good);
    for (const char* bad : {"2.8", "2.1", "0.26", "2", "2.50", "1.x"})
        BOOST_TEST(contains(err(cat(std::string(R"("edition":")") + bad + "\"")), "edition"), bad);
}

BOOST_AUTO_TEST_CASE(sac_sic_0_255)
{
    const InputDef none = def(kCatUdp);
    BOOST_TEST(!none.sac.has_value());
    BOOST_TEST(!none.sic.has_value());
    const InputDef nulls = def(cat(R"("sac":null,"sic":null)"));
    BOOST_TEST(!nulls.sac.has_value());
    BOOST_TEST(!nulls.sic.has_value());
    const InputDef both = def(cat(R"("sac":0,"sic":255)"));
    BOOST_TEST(both.sac.value() == 0);
    BOOST_TEST(both.sic.value() == 255);
    for (const char* bad : {"-1", "256", "1.5", "\"x\""}) {
        BOOST_TEST(contains(err(cat(std::string(R"("sac":)") + bad)), "sac"), bad);
        BOOST_TEST(contains(err(cat(std::string(R"("sic":)") + bad)), "sic"), bad);
    }
}

BOOST_AUTO_TEST_CASE(status_url_rules)
{
    BOOST_TEST(def(cat(R"("status_url":"http://192.0.2.10/api/v1/status")")).status_url ==
               "http://192.0.2.10/api/v1/status");
    BOOST_TEST(def(cat(R"("status_url":null)")).status_url.empty());
    BOOST_TEST(def(kCatUdp).status_url.empty());
    for (const char* bad : {"https://192.0.2.10/api", "http://u:p@h/", "http://h:0/", "http://h:65536/", "http://",
                            "ftp://h/", "http://h:x/", "http://-h/", "http://h/a b", "192.0.2.10/api"})
        BOOST_TEST(contains(err(cat(std::string(R"("status_url":")") + bad + "\"")), "status_url"), bad);

    const auto u = parseHttpUrl("http://192.0.2.10:8080/api/v1/status?x=1");
    BOOST_REQUIRE(u.has_value());
    BOOST_TEST(u->host == "192.0.2.10");
    BOOST_TEST(u->port == 8080);
    BOOST_TEST(u->target == "/api/v1/status?x=1");
    const auto plain = parseHttpUrl("http://ps.example");
    BOOST_REQUIRE(plain.has_value());
    BOOST_TEST(plain->host == "ps.example");
    BOOST_TEST(plain->port == 80);
    BOOST_TEST(plain->target == "/");
    BOOST_TEST(!parseHttpUrl("http://h:/").has_value());
    BOOST_TEST(!parseHttpUrl("http://@h/").has_value());
    BOOST_TEST(!parseHttpUrl("HTTP://h/").has_value());
}

BOOST_AUTO_TEST_CASE(status_interval_5_3600)
{
    BOOST_TEST(def(kCatUdp).status_interval_s == 30);
    BOOST_TEST(def(cat(R"("status_interval_s":5)")).status_interval_s == 5);
    BOOST_TEST(def(cat(R"("status_interval_s":3600)")).status_interval_s == 3600);
    for (const char* bad : {"4", "3601", "30.5", "\"often\""})
        BOOST_TEST(contains(err(cat(std::string(R"("status_interval_s":)") + bad)), "status_interval_s"), bad);
}

BOOST_AUTO_TEST_CASE(time_mode_values)
{
    BOOST_TEST((def(kCatUdp).time_mode == TimeMode::utc));
    BOOST_TEST((def(cat(R"("time_mode":"gps")")).time_mode == TimeMode::gps));
    BOOST_TEST((def(cat(R"("time_mode":"utc")")).time_mode == TimeMode::utc));
    BOOST_TEST(contains(err(cat(R"("time_mode":"GPS")")), "time_mode"));
    BOOST_TEST(contains(err(cat(R"("time_mode":"tai")")), "time_mode"));
}

BOOST_AUTO_TEST_CASE(unknown_key_rejected)
{
    const std::string e = err(cat(R"("staus_url":"http://h/")"));
    BOOST_TEST(contains(e, "staus_url"), e);
    BOOST_TEST(contains(err(std::string(kReadsb) + R"(,"Port":1)"), "Port"));
    // The other booleans.
    BOOST_TEST(!def(cat(R"("enabled":false)")).enabled);
    BOOST_TEST(def(cat(R"("capture":true)")).capture);
    BOOST_TEST(contains(err(cat(R"("enabled":1)")), "enabled"));
    BOOST_TEST(contains(err(cat(R"("capture":"yes")")), "capture"));
}

BOOST_AUTO_TEST_CASE(field_not_for_type_rejected)
{
    BOOST_TEST(err(std::string(kReadsb) + R"(,"transport":"sctp")") == "transport must be udp, tcp_connect or tcp_listen");
    BOOST_TEST(err(std::string(kReadsb) + R"(,"edition":"2.5")") == "edition applies to pingstation_cat021 only");
    BOOST_TEST(err(std::string(kCatConnect) + R"(,"peer":"192.0.2.1")") == "peer applies to udp and tcp_listen only");
    BOOST_TEST(err(std::string(kReadsb) + R"(,"peer":"192.0.2.1")") == "peer applies to udp and tcp_listen only");
    BOOST_TEST(err(cat(R"("host":"192.0.2.1")")) == "host is not used with udp");
    BOOST_TEST(err(std::string(kPsJson) + R"(,"host":"192.0.2.1")") == "host is not used with udp");
    BOOST_TEST(contains(err(R"("id":"x","type":"pingstation_cat021","transport":"tcp_listen","port":1,"host":"h")"),
                        "host is not used with tcp_listen"));
    BOOST_TEST(contains(err(std::string(kCatConnect) + R"(,"bind":"0.0.0.0")"), "bind"));
    BOOST_TEST(contains(err(std::string(kReadsb) + R"(,"bind":"0.0.0.0")"), "bind"));
    for (const char* key : {R"("edition":"2.5")", R"("sac":1)", R"("sic":1)"}) {
        BOOST_TEST(contains(err(std::string(kPsJson) + "," + key), "pingstation_cat021 only"), key);
        BOOST_TEST(contains(err(std::string(kReadsb) + "," + key), "pingstation_cat021 only"), key);
    }
    for (const char* key :
         {R"("status_url":"http://h/")", R"("status_interval_s":30)", R"("time_mode":"utc")", R"("capture":false)"}) {
        BOOST_TEST(contains(err(std::string(kReadsb) + "," + key), "pingstation_json and pingstation_cat021 only"),
                   key);
        BOOST_TEST(contains(err(std::string(kDump978) + "," + key), "pingstation_json and pingstation_cat021 only"),
                   key);
        BOOST_TEST(ok(std::string(kPsJson) + "," + key), key);
    }
    // What does apply.
    BOOST_TEST(def(std::string(kPsJson) + R"(,"peer":"192.0.2.7","bind":"192.0.2.2")").peer == "192.0.2.7");
    BOOST_TEST(def(cat(R"("peer":null)")).peer.empty());
    BOOST_TEST(contains(err(cat(R"("peer":"ps.example")")), "peer"));
}

BOOST_AUTO_TEST_CASE(required_fields)
{
    BOOST_TEST(err(R"("type":"pingstation_json","port":4000)") == "id is required");
    BOOST_TEST(err(R"("id":"x","type":"pingstation_json")") == "port is required");
    BOOST_TEST(err(R"("id":"x","type":"readsb","port":30047)") == "host is required");
    BOOST_TEST(err(R"("id":"x","type":"dump978","port":30978)") == "host is required");
    BOOST_TEST(err(R"("id":"x","type":"pingstation_cat021","transport":"tcp_connect","port":4000)") ==
               "host is required");
    // No default transport: the error names the key, even with a host or bind.
    const std::string need = "transport is required for pingstation_cat021 (udp, tcp_connect or tcp_listen)";
    BOOST_TEST(err(R"("id":"x","type":"pingstation_cat021","port":4000)") == need);
    BOOST_TEST(err(R"("id":"x","type":"pingstation_cat021","host":"h","port":4000)") == need);
    BOOST_TEST(err(R"("id":"x","type":"pingstation_cat021","bind":"0.0.0.0","port":4000)") == need);
}

BOOST_AUTO_TEST_CASE(reserved_ids)
{
    for (const char* id : {"readsb", "dump978", "replay"})
        BOOST_TEST(err(std::string(R"("id":")") + id + R"(","type":"pingstation_json","port":4000)") ==
                       std::string("id '") + id + "' is reserved",
                   id);
}

BOOST_AUTO_TEST_CASE(json_round_trip_each_user_type)
{
    std::vector<InputDef> defs;
    InputDef a;
    a.id = "rb2";
    a.name = "second readsb \"quoted\"";
    a.type = InputType::readsb;
    a.enabled = false;
    a.host = "192.0.2.1";
    a.port = 30047;
    defs.push_back(a);

    InputDef b;
    b.id = "uat2";
    b.type = InputType::dump978;
    b.host = "uat.example";
    b.port = 30978;
    defs.push_back(b);

    InputDef c;
    c.id = "ps-json";
    c.name = "Mât";
    c.type = InputType::pingstation_json;
    c.transport = InputTransport::udp;
    c.bind = "192.0.2.2";
    c.port = 4000;
    c.peer = "192.0.2.7";
    c.status_url = "http://192.0.2.7/api/v1/status";
    c.status_interval_s = 5;
    c.time_mode = TimeMode::gps;
    c.capture = true;
    defs.push_back(c);

    for (InputTransport t : {InputTransport::udp, InputTransport::tcp_connect, InputTransport::tcp_listen}) {
        InputDef d;
        d.id = std::string("ps-") + transportName(t);
        for (char& ch : d.id)
            if (ch == '_')
                ch = '-';
        d.type = InputType::pingstation_cat021;
        d.transport = t;
        d.port = 4001;
        if (t == InputTransport::tcp_connect)
            d.host = "ps.example";
        if (t == InputTransport::tcp_listen)
            d.peer = "192.0.2.8";
        d.edition = "2.6";
        d.sac = 0;
        d.sic = 200;
        d.status_interval_s = 3600;
        defs.push_back(d);
    }
    // And the defaults alone.
    defs.push_back(def(kCatUdp));

    for (const InputDef& d : defs) {
        const std::string json = inputDefJson(d);
        BOOST_TEST(!contains(json, "builtin"), json);
        const InputDef back = parseInputDef(obj(json));
        BOOST_TEST((back == d), json);
    }
    // Only the keys that apply, defaults included.
    const std::string rb = inputDefJson(defs[0]);
    BOOST_TEST(!contains(rb, "transport"), rb);
    BOOST_TEST(!contains(rb, "edition"), rb);
    BOOST_TEST(contains(rb, "\"enabled\":false"), rb);
    const std::string udp = inputDefJson(def(kCatUdp));
    BOOST_TEST(contains(udp, "\"edition\":\"2.5\""), udp);
    BOOST_TEST(contains(udp, "\"bind\":\"0.0.0.0\""), udp);
    BOOST_TEST(contains(udp, "\"status_interval_s\":30"), udp);
    BOOST_TEST(!contains(udp, "\"host\""), udp);
}

BOOST_AUTO_TEST_CASE(builtin_and_state_keys_rejected)
{
    BOOST_TEST(contains(err(std::string(kReadsb) + R"(,"builtin":false)"), "builtin"));
    BOOST_TEST(contains(err(std::string(kReadsb) + R"(,"state":{"connected":true})"), "state"));
}

BOOST_AUTO_TEST_CASE(replay_type_rejected)
{
    const std::string e = err(R"("id":"r1","type":"replay","port":1,"host":"h")");
    BOOST_TEST(contains(e, "type"), e);
    BOOST_TEST(std::string(inputTypeName(InputType::replay)) == "replay");
}

BOOST_AUTO_TEST_CASE(inputs_file_round_trip)
{
    InputsFile f;
    f.inputs.push_back(def(kCatUdp));
    f.inputs.push_back(def(kCatConnect));
    f.inputs.push_back(def(kReadsb));
    f.builtin_enabled["readsb"] = true;
    f.builtin_enabled["dump978"] = false;
    const std::string text = inputsFileJson(f);
    BOOST_TEST(text.back() == '\n');
    BOOST_TEST(contains(text, "\n  \"version\": 1"), text);
    const InputsFile back = parseInputsFile(text);
    BOOST_REQUIRE(back.inputs.size() == f.inputs.size());
    for (size_t i = 0; i < f.inputs.size(); ++i)
        BOOST_TEST((back.inputs[i] == f.inputs[i]), i);
    BOOST_TEST((back.builtin_enabled == f.builtin_enabled));

    const InputsFile empty = parseInputsFile(inputsFileJson(InputsFile{}));
    BOOST_TEST(empty.inputs.empty());
    BOOST_TEST(empty.builtin_enabled.empty());
    // The spec's example, and {} for the list (PropertyTree reads it as []).
    const InputsFile spec = parseInputsFile(
        R"({"version":1,"inputs":[],"builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}})");
    BOOST_TEST(spec.builtin_enabled.size() == 2u);
    BOOST_TEST(parseInputsFile(R"({"version":1,"inputs":{}})").inputs.empty());
    BOOST_TEST(parseInputsFile(R"({"version":1})").inputs.empty());
}

BOOST_AUTO_TEST_CASE(inputs_file_bad_entry_names_index)
{
    const std::string e = inputsFileErr(R"({"version":1,"inputs":[{"id":"a","type":"pingstation_json","port":4000},)"
                                        R"({"id":"b","type":"pingstation_json","port":0}]})");
    BOOST_TEST(e.rfind("inputs[1] (b): ", 0) == 0, e);
    BOOST_TEST(contains(e, "port"), e);
    const std::string no_id = inputsFileErr(R"({"version":1,"inputs":[{"type":"pingstation_json","port":4000}]})");
    BOOST_TEST(no_id.rfind("inputs[0] ", 0) == 0, no_id);
    BOOST_TEST(contains(no_id, "id is required"), no_id);
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[1,2]})"), "inputs"));
    // A duplicate id across entries.
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"a","type":"pingstation_json","port":4000},)"
                                      R"({"id":"a","type":"pingstation_json","port":4001}]})"),
                        "duplicate id 'a'"));
}

BOOST_AUTO_TEST_CASE(inputs_file_version_and_unknown_key)
{
    BOOST_TEST(contains(inputsFileErr(R"({"version":3,"inputs":[]})"), "version"));
    BOOST_TEST(contains(inputsFileErr(R"({"inputs":[]})"), "version"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":"x","inputs":[]})"), "version"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[],"input":[]})"), "input'"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"replay":{"enabled":true}}})"), "replay"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"readsb":{"enabled":1}}})"), "enabled"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"readsb":{"enabled":true,"host":"h"}}})"), "host"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"readsb":true}})"), "readsb"));
    BOOST_TEST(!inputsFileErr("[1]").empty());
    BOOST_TEST(!inputsFileErr("{").empty());
    BOOST_TEST(!inputsFileErr("").empty());
}

BOOST_AUTO_TEST_CASE(inputs_file_over_1mib)
{
    InputsFile f;
    InputDef d = def(kPsJson);
    d.name = std::string(64, 'n');
    std::string text;
    for (int i = 0; text.size() <= 1024 * 1024; ++i) {
        d.id = "p" + std::to_string(i);
        d.port = static_cast<uint16_t>(1000 + i % 60000);
        d.enabled = false;  // no listener conflicts
        f.inputs.push_back(d);
        if (i % 500 == 0)
            text = inputsFileJson(f);
    }
    text = inputsFileJson(f);
    BOOST_REQUIRE(text.size() > 1024 * 1024);
    BOOST_TEST(contains(inputsFileErr(text), "1 MiB"), inputsFileErr(text).substr(0, 200));
    // Just under the cap parses.
    while (text.size() > 1024 * 1024) {
        f.inputs.resize(f.inputs.size() - 50);
        text = inputsFileJson(f);
    }
    BOOST_TEST(parseInputsFile(text).inputs.size() == f.inputs.size());
}

BOOST_AUTO_TEST_CASE(duplicate_listen_port_rejected)
{
    InputDef a = def(R"("id":"ps-north","type":"pingstation_json","port":4000)");
    InputDef b = def(R"("id":"ps-south","type":"pingstation_cat021","transport":"udp","port":4000)");
    BOOST_TEST(setErr({a, b}) == "port 4000/udp is already used by input ps-north");
    InputDef c = def(R"("id":"ps-west","type":"pingstation_json","port":4000)");
    BOOST_TEST(setErr({a, c}) == "port 4000/udp is already used by input ps-north");
    b.enabled = false;
    BOOST_TEST(setErr({a, b}).empty());
    a.enabled = false;
    b.enabled = true;
    BOOST_TEST(setErr({a, b}).empty());
    InputDef l1 = def(R"("id":"l1","type":"pingstation_cat021","transport":"tcp_listen","port":4000)");
    InputDef l2 = def(R"("id":"l2","type":"pingstation_cat021","transport":"tcp_listen","port":4000)");
    BOOST_TEST(setErr({b, l1}).empty());  // udp 4000 + tcp 4000
    BOOST_TEST(setErr({l1, l2}) == "port 4000/tcp is already used by input l1");
    // Outgoing connections never conflict.
    InputDef r1 = def(R"("id":"r1","type":"readsb","host":"h","port":4000)");
    InputDef r2 = def(R"("id":"r2","type":"pingstation_cat021","transport":"tcp_connect","host":"h","port":4000)");
    BOOST_TEST(setErr({r1, r2, l1, b}).empty());
}

BOOST_AUTO_TEST_CASE(duplicate_ids_rejected)
{
    InputDef a = def(R"("id":"ps","type":"pingstation_json","port":4000)");
    InputDef b = def(R"("id":"ps","type":"pingstation_json","port":4001)");
    BOOST_TEST(setErr({a, b}) == "duplicate id 'ps'");
    b.id = "ps2";
    BOOST_TEST(setErr({a, b}).empty());
}

// PropertyTree keeps a repeated key, so the parsers reject one instead of
// silently using either value.
BOOST_AUTO_TEST_CASE(duplicate_keys_rejected)
{
    BOOST_TEST(contains(err(R"("id":"a","id":"b","type":"pingstation_json","port":4000)"), "duplicate key 'id'"));
    BOOST_TEST(contains(err(std::string(kPsJson) + R"(,"port":4001)"), "duplicate key 'port'"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"version":1})"), "version"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[],"inputs":[]})"), "inputs"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"readsb":{"enabled":true},"readsb":{"enabled":false}}})"),
                        "duplicate input 'readsb'"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{},"builtin":{}})"), "builtin"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"readsb":{"enabled":true,"enabled":false}}})"),
                        "enabled"));
}

// The same PropertyTree limitation as quoted numerals: "true" reads as true.
BOOST_AUTO_TEST_CASE(quoted_boolean_accepted)
{
    BOOST_TEST(def(cat(R"("enabled":"true")")).enabled);
    BOOST_TEST(!def(cat(R"("enabled":"false")")).enabled);
    const auto file = parseInputsFile(R"({"version":1,"builtin":{"readsb":{"enabled":"false"}}})");
    BOOST_TEST(file.builtin_enabled.at("readsb") == false);
}

BOOST_AUTO_TEST_CASE(integer_overflow_rejected)
{
    for (const char* big : {"99999999999999999999", "4294967297", "18446744073709551616"}) {
        BOOST_TEST(contains(err(std::string(R"("id":"x","type":"pingstation_json","port":)") + big), "port"), big);
        BOOST_TEST(contains(err(cat(std::string(R"("sac":)") + big)), "sac"), big);
        BOOST_TEST(contains(err(cat(std::string(R"("status_interval_s":)") + big)), "status_interval_s"), big);
    }
    BOOST_TEST(contains(inputsFileErr(R"({"version":99999999999999999999})"), "version"));
}

BOOST_AUTO_TEST_CASE(settled_file_needs_two_stable_polls)
{
    TempDir tmp;
    const std::string path = tmp.path + "/skycue.inputs.json";
    SettledFile watch(path);
    BOOST_TEST(watch.path() == path);
    BOOST_TEST(!watch.poll());  // no file, none known
    BOOST_TEST(!FileStamp::of(path).exists);

    atomicWrite(path, "{\"version\":1}\n");
    BOOST_TEST(FileStamp::of(path).exists);
    BOOST_TEST(!watch.poll());  // changed: maybe still being written
    BOOST_TEST(watch.poll());   // the same stamp twice: settled
    BOOST_TEST(!watch.poll());  // and nothing new since

    // Our own write is accepted, so never reported.
    atomicWrite(path, "{\"version\":1,\"inputs\":[]}\n");
    watch.accept();
    BOOST_TEST(!watch.poll());
    BOOST_TEST(!watch.poll());

    // A change that keeps changing is not settled until it stops.
    {
        std::ofstream(path, std::ios::app) << " ";
    }
    BOOST_TEST(!watch.poll());
    {
        std::ofstream(path, std::ios::app) << " ";
    }
    BOOST_TEST(!watch.poll());
    BOOST_TEST(watch.poll());

    // Removal is a change too.
    ::unlink(path.c_str());
    BOOST_TEST(!watch.poll());
    BOOST_TEST(watch.poll());
    BOOST_TEST((FileStamp::of(path) == FileStamp::of(tmp.path + "/none")));
    BOOST_TEST((FileStamp::of(path) != FileStamp::of(tmp.path)));
}

// ---- v2: endpoint fields, raw input types, outputs in the set --------------

namespace {

const char* const kV1Example =
    R"({"version":1,"inputs":[{"id":"ps-north","name":"North mast","type":"pingstation_cat021","enabled":true,)"
    R"("transport":"udp","bind":"0.0.0.0","port":4001,"peer":null,"edition":"2.5","sac":null,"sic":null,)"
    R"("status_url":null,"status_interval_s":30,"time_mode":"utc","capture":false}],)"
    R"("builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}})";
const char* const kV1Sim =
    R"({"version":1,"inputs":[{"id":"sim-json","name":"Simulated JSON","type":"pingstation_json",)"
    R"("bind":"127.0.0.1","port":4000},{"id":"sim-cat","name":"Simulated CAT021","type":"pingstation_cat021",)"
    R"("transport":"udp","bind":"127.0.0.1","port":4001}]})";

OutputDef outDef(const std::string& members) { return parseOutputDef(obj("{" + members + "}")); }

std::string setErr3(const std::vector<InputDef>& in, const std::vector<OutputDef>& out,
                    const std::vector<ReservedPort>& reserved = {})
{
    try {
        validateInputSet(in, out, reserved);
    } catch (const InputDefError& e) {
        return e.what();
    }
    return "";
}

const char* const kBeastOut = R"("id":"beast-out","kind":"beast","transport":"tcp_listen","port":30005)";

}  // namespace

BOOST_AUTO_TEST_CASE(v2_transport_accepted_on_line_types)
{
    struct Row {
        const char* type;
        InputTransport def;
    };
    const Row rows[] = {{"readsb", InputTransport::tcp_connect},
                        {"dump978", InputTransport::tcp_connect},
                        {"uat_raw", InputTransport::tcp_connect},
                        {"beast_raw", InputTransport::tcp_connect},
                        {"pingstation_json", InputTransport::udp}};
    for (const Row& r : rows) {
        const std::string head = std::string(R"("id":"x","type":")") + r.type + R"(","port":4000)";
        const std::string connect = head + R"(,"host":"192.0.2.1")";
        BOOST_TEST((def(r.def == InputTransport::udp ? head : connect).transport == r.def), r.type);
        for (const char* t : {"tcp_connect", "tcp_listen", "udp"}) {
            const std::string members = head + R"(,"transport":")" + t + '"' +
                                        (std::string(t) == "tcp_connect" ? R"(,"host":"192.0.2.1")" : "");
            BOOST_TEST(ok(members), r.type << " " << t << " -> " << err(members));
            BOOST_TEST(std::string(transportName(def(members).transport)) == t);
        }
    }
    BOOST_TEST(def(R"("id":"x","type":"uat_raw","port":4000,"host":"h")").port == 4000);
    BOOST_TEST(contains(err(R"("id":"x","type":"beast_raw","port":4000)"), "host is required"));
}

BOOST_AUTO_TEST_CASE(group_requires_udp_and_multicast_address)
{
    const std::string udp = R"("id":"x","type":"beast_raw","transport":"udp","port":4000)";
    BOOST_TEST(def(udp + R"(,"group":"239.1.1.5")").group == "239.1.1.5");
    BOOST_TEST(ok(udp + R"(,"group":"224.0.0.0")"));
    BOOST_TEST(ok(udp + R"(,"group":"239.255.255.255")"));
    for (const char* bad : {"192.0.2.1", "223.255.255.255", "240.0.0.0", "239.1.1", "ff02::1", "host"})
        BOOST_TEST(err(udp + R"(,"group":")" + bad + R"(")") ==
                       "group must be a multicast address (224.0.0.0 to 239.255.255.255)",
                   bad);
    BOOST_TEST(err(R"("id":"x","type":"beast_raw","transport":"tcp_listen","port":4000,"group":"239.1.1.5")") ==
               "group applies to udp only");
    BOOST_TEST(err(R"("id":"x","type":"readsb","host":"h","port":4000,"group":"239.1.1.5")") ==
               "group applies to udp only");
    BOOST_TEST(ok(udp + R"(,"group":null)"));
}

BOOST_AUTO_TEST_CASE(interface_requires_group)
{
    const std::string udp = R"("id":"x","type":"uat_raw","transport":"udp","port":4000)";
    BOOST_TEST(def(udp + R"(,"group":"239.1.1.5","interface":"192.0.2.10")").iface == "192.0.2.10");
    BOOST_TEST(err(udp + R"(,"interface":"192.0.2.10")") == "interface applies to a multicast group only");
    BOOST_TEST(err(udp + R"(,"group":"239.1.1.5","interface":"eth0")") == "interface must be an IPv4 address");
    BOOST_TEST(err(R"("id":"x","type":"uat_raw","host":"h","port":4000,"interface":"192.0.2.10")") ==
               "interface applies to udp only");
}

BOOST_AUTO_TEST_CASE(bind_refused_with_group)
{
    const std::string udp = R"("id":"x","type":"beast_raw","transport":"udp","port":4000)";
    BOOST_TEST(err(udp + R"(,"group":"239.1.1.5","bind":"0.0.0.0")") == "bind does not apply when group is set");
    BOOST_TEST(ok(udp + R"(,"bind":"192.0.2.10")"));
}

BOOST_AUTO_TEST_CASE(beast_raw_format_values)
{
    const std::string base = R"("id":"x","type":"beast_raw","host":"h","port":4000)";
    BOOST_TEST(def(base).format == "beast");
    BOOST_TEST(def(base + R"(,"format":"avr")").format == "avr");
    BOOST_TEST(def(base + R"(,"format":"beast")").format == "beast");
    BOOST_TEST(err(base + R"(,"format":"mlat")") == "format must be beast or avr");
    BOOST_TEST(err(base + R"(,"format":1)") == "format must be beast or avr");
}

BOOST_AUTO_TEST_CASE(format_only_on_beast_raw)
{
    for (const char* line : {kReadsb, kDump978, kPsJson, kCatUdp, R"("id":"u","type":"uat_raw","host":"h","port":1)"})
        BOOST_TEST(err(std::string(line) + R"(,"format":"avr")") == "format applies to beast_raw only", line);
}

BOOST_AUTO_TEST_CASE(source_defaults_device_and_roundtrips)
{
    BOOST_CHECK(def(kPsJson).source == PingSource::device);
    BOOST_CHECK(def(std::string(kPsJson) + R"(,"source":"device")").source == PingSource::device);
    const InputDef d = def(std::string(kPsJson) + R"(,"source":"decode")");
    BOOST_CHECK(d.source == PingSource::decode);
    BOOST_TEST(inputDefJson(d).find(R"("source":"decode")") != std::string::npos);
    BOOST_TEST(inputDefJson(def(kPsJson)).find("source") == std::string::npos);  // default: not written
    BOOST_CHECK(def(inputDefJson(d).substr(1, inputDefJson(d).size() - 2)) == d);
}

BOOST_AUTO_TEST_CASE(source_rejects_other_value)
{
    BOOST_TEST(err(std::string(kPsJson) + R"(,"source":"mlat")") == "source must be device or decode");
    BOOST_TEST(err(std::string(kPsJson) + R"(,"source":1)") == "source must be device or decode");
}

BOOST_AUTO_TEST_CASE(source_only_for_pingstation_json)
{
    for (const char* line : {kReadsb, kDump978, kCatUdp, R"("id":"u","type":"uat_raw","host":"h","port":1)",
                             R"("id":"b","type":"beast_raw","host":"h","port":1)"})
        BOOST_TEST(err(std::string(line) + R"(,"source":"decode")") == "source applies to pingstation_json only", line);
}

BOOST_AUTO_TEST_CASE(capture_accepted_on_raw_types)
{
    for (const char* type : {"beast_raw", "uat_raw"}) {
        const std::string base = std::string(R"("id":"x","type":")") + type + R"(","host":"h","port":4000)";
        BOOST_TEST(!def(base).capture);
        BOOST_TEST(def(base + R"(,"capture":true)").capture);
    }
    BOOST_TEST(def(std::string(kPsJson) + R"(,"capture":true)").capture);
    BOOST_TEST(contains(err(std::string(kReadsb) + R"(,"capture":true)"), "capture applies to"));
    BOOST_TEST(contains(err(std::string(kDump978) + R"(,"capture":true)"), "capture applies to"));
}

BOOST_AUTO_TEST_CASE(track_types_exclude_raw_inputs)
{
    BOOST_TEST(!isTrackType(InputType::beast_raw));
    BOOST_TEST(!isTrackType(InputType::uat_raw));
    BOOST_TEST(!isTrackType(InputType::dump978));
    BOOST_TEST(isTrackType(InputType::readsb));
}

BOOST_AUTO_TEST_CASE(input_json_round_trip_new_types)
{
    const char* const members[] = {
        R"("id":"b1","type":"beast_raw","host":"192.0.2.9","port":30005,"capture":true)",
        R"("id":"b2","type":"beast_raw","transport":"tcp_listen","bind":"127.0.0.1","port":30006,"peer":"192.0.2.3","format":"avr")",
        R"("id":"b3","type":"beast_raw","transport":"udp","port":30007,"group":"239.1.1.5","interface":"192.0.2.10")",
        R"("id":"u1","type":"uat_raw","host":"h.example","port":30978)",
        R"("id":"u2","type":"uat_raw","transport":"udp","port":30979,"group":"239.1.1.6")",
        R"("id":"r1","type":"readsb","transport":"udp","port":30047)",
        R"("id":"r2","type":"readsb","transport":"tcp_listen","port":30048)",
        R"("id":"d1","type":"dump978","transport":"tcp_listen","port":30049)",
        R"("id":"j1","type":"pingstation_json","transport":"tcp_connect","host":"h","port":4000)",
        R"("id":"j2","type":"pingstation_json","port":4001,"group":"239.1.1.7")",
        R"("id":"c1","type":"pingstation_cat021","transport":"udp","port":4002,"group":"239.1.1.8","capture":true)"};
    for (const char* m : members) {
        const InputDef d = def(m);
        const auto reparsed = parseInputDef(obj(inputDefJson(d)));
        BOOST_TEST((reparsed == d), m << " -> " << inputDefJson(d));
    }
    // Keys that do not apply are not written.
    BOOST_TEST(!contains(inputDefJson(def(kReadsb)), "group"));
    BOOST_TEST(!contains(inputDefJson(def(kReadsb)), "transport"));
    BOOST_TEST(!contains(inputDefJson(def(kPsJson)), "transport"));
    BOOST_TEST(!contains(inputDefJson(def(members[2])), "bind"));
}

BOOST_AUTO_TEST_CASE(unicast_and_multicast_udp_same_port_conflict)
{
    const InputDef uni = def(R"("id":"uni","type":"beast_raw","transport":"udp","port":4978)");
    const InputDef multi = def(R"("id":"multi","type":"beast_raw","transport":"udp","port":4978,"group":"239.1.1.5")");
    BOOST_TEST(setErr({uni, multi}) == "port 4978/udp is already used by input uni");
    BOOST_TEST(setErr({multi, uni}) == "port 4978/udp is already used by input multi");
}

BOOST_AUTO_TEST_CASE(two_multicast_receivers_share_port_across_groups)
{
    const InputDef a = def(R"("id":"a","type":"beast_raw","transport":"udp","port":4978,"group":"239.1.1.5")");
    const InputDef b = def(R"("id":"b","type":"uat_raw","transport":"udp","port":4978,"group":"239.1.1.6")");
    const InputDef c = def(R"("id":"c","type":"uat_raw","transport":"udp","port":4978,"group":"239.1.1.5")");
    BOOST_TEST(setErr({a, b, c}) == "");
    BOOST_TEST(setErr({a, b, def(R"("id":"d","type":"uat_raw","transport":"udp","port":4978)")}) ==
               "port 4978/udp is already used by input a");
}

BOOST_AUTO_TEST_CASE(output_vs_input_port_conflict)
{
    const InputDef ps = def(R"("id":"ps","type":"pingstation_cat021","transport":"tcp_listen","port":30005)");
    const OutputDef beast = outDef(kBeastOut);
    BOOST_TEST(setErr3({ps}, {beast}) == "port 30005/tcp is already used by input ps");
    // The first one named is the input even when the outputs were given first
    // (inputs before outputs), and an output pair names the first output.
    const OutputDef second = outDef(R"("id":"second","kind":"uat","transport":"tcp_listen","port":30005)");
    BOOST_TEST(setErr3({}, {beast, second}) == "port 30005/tcp is already used by output beast-out");
    // An input after an output: still the input, then the output.
    BOOST_TEST(setErr3({ps}, {second}) == "port 30005/tcp is already used by input ps");
    // Different protocols do not conflict.
    const InputDef udp = def(R"("id":"u","type":"beast_raw","transport":"udp","port":30005)");
    BOOST_TEST(setErr3({udp}, {beast}) == "");
    // Duplicate ids: separate namespaces.
    BOOST_TEST(setErr3({def(R"("id":"beast-out","type":"pingstation_json","port":4000)")}, {beast}) == "");
    BOOST_TEST(setErr3({}, {beast, beast}) == "duplicate output id 'beast-out'");
    BOOST_TEST(setErr({ps, ps}) == "duplicate id 'ps'");
}

BOOST_AUTO_TEST_CASE(output_vs_reserved_cue_port)
{
    const std::vector<ReservedPort> reserved = {{"tcp", 30500, "the cue stream (CUE_TCP_PORT)"}};
    const OutputDef out = outDef(R"("id":"o","kind":"beast","transport":"tcp_listen","port":30500)");
    BOOST_TEST(setErr3({}, {out}, reserved) == "port 30500/tcp is already used by the cue stream (CUE_TCP_PORT)");
    BOOST_TEST(setErr3({}, {out}) == "");
}

BOOST_AUTO_TEST_CASE(input_vs_reserved_port_not_checked)
{
    const std::vector<ReservedPort> reserved = {{"tcp", 30500, "the cue stream (CUE_TCP_PORT)"}};
    const InputDef in = def(R"("id":"i","type":"pingstation_cat021","transport":"tcp_listen","port":30500)");
    BOOST_TEST(setErr3({in}, {}, reserved) == "");
}

BOOST_AUTO_TEST_CASE(disabled_entries_do_not_conflict)
{
    InputDef ps = def(R"("id":"ps","type":"pingstation_cat021","transport":"tcp_listen","port":30005)");
    ps.enabled = false;
    OutputDef beast = outDef(kBeastOut);
    BOOST_TEST(setErr3({ps}, {beast}) == "");
    ps.enabled = true;
    beast.enabled = false;
    BOOST_TEST(setErr3({ps}, {beast}) == "");
    OutputDef reserved_out = outDef(R"("id":"o","kind":"beast","transport":"tcp_listen","port":30500,"enabled":false)");
    BOOST_TEST(setErr3({}, {reserved_out}, {{"tcp", 30500, "x"}}) == "");
    // A disabled output sending to a joined group is no loop.
    InputDef join = def(R"("id":"j","type":"beast_raw","transport":"udp","port":4978,"group":"239.1.1.5")");
    OutputDef send = outDef(R"("id":"s","kind":"beast","transport":"udp","port":4978,"group":"239.1.1.5","enabled":false)");
    BOOST_TEST(setErr3({join}, {send}) == "");
}

BOOST_AUTO_TEST_CASE(udp_output_never_conflicts)
{
    const InputDef in = def(R"("id":"u","type":"beast_raw","transport":"udp","port":4000)");
    const OutputDef uni = outDef(R"("id":"s","kind":"beast","transport":"udp","port":4000,"host":"192.0.2.9")");
    const OutputDef reserved_port = outDef(R"("id":"s2","kind":"beast","transport":"udp","port":30500,"host":"192.0.2.9")");
    BOOST_TEST(setErr3({in}, {uni, reserved_port}, {{"udp", 30500, "x"}, {"tcp", 30500, "y"}}) == "");
    const OutputDef connect = outDef(R"("id":"c","kind":"beast","transport":"tcp_connect","port":30005,"host":"192.0.2.9")");
    BOOST_TEST(setErr3({}, {connect, outDef(kBeastOut)}) == "");
}

BOOST_AUTO_TEST_CASE(multicast_output_to_joined_group_is_a_loop)
{
    const InputDef join = def(R"("id":"y","type":"beast_raw","transport":"udp","port":4978,"group":"239.1.1.5")");
    const OutputDef send = outDef(R"("id":"x","kind":"beast","transport":"udp","port":4978,"group":"239.1.1.5")");
    BOOST_TEST(setErr3({join}, {send}) == "output 'x' sends to 239.1.1.5:4978, which input 'y' joins (a loop)");
    const OutputDef other_group = outDef(R"("id":"x","kind":"beast","transport":"udp","port":4978,"group":"239.1.1.6")");
    BOOST_TEST(setErr3({join}, {other_group}) == "");
    const OutputDef other_port = outDef(R"("id":"x","kind":"beast","transport":"udp","port":4979,"group":"239.1.1.5")");
    BOOST_TEST(setErr3({join}, {other_port}) == "");
}

BOOST_AUTO_TEST_CASE(loopback_unicast_output_to_own_port_warns_not_throws)
{
    const InputDef in = def(R"("id":"y","type":"beast_raw","transport":"tcp_listen","port":4000)");
    const OutputDef send = outDef(R"("id":"x","kind":"beast","transport":"tcp_connect","port":4000,"host":"127.0.0.1")");
    const auto warns = validateInputSet({in}, {send});
    BOOST_REQUIRE(warns.size() == 1u);
    BOOST_TEST(warns[0] == "output 'x' sends to 127.0.0.1:4000, which input 'y' of this daemon listens on");
    // localhost, udp, and an output served by this daemon.
    const InputDef udp_in = def(R"("id":"z","type":"beast_raw","transport":"udp","port":4001)");
    const OutputDef udp_send = outDef(R"("id":"w","kind":"beast","transport":"udp","port":4001,"host":"localhost")");
    BOOST_TEST(validateInputSet({udp_in}, {udp_send}).size() == 1u);
    const OutputDef serve = outDef(kBeastOut);
    const OutputDef to_serve = outDef(R"("id":"v","kind":"beast","transport":"tcp_connect","port":30005,"host":"127.4.5.6")");
    const auto w2 = validateInputSet({}, {serve, to_serve});
    BOOST_REQUIRE(w2.size() == 1u);
    BOOST_TEST(w2[0] == "output 'v' sends to 127.4.5.6:30005, which output 'beast-out' of this daemon serves");
    // No warning: another port, a non-loopback host, a disabled input.
    BOOST_TEST(validateInputSet({in}, {outDef(R"("id":"x","kind":"beast","transport":"tcp_connect","port":4002,"host":"127.0.0.1")")})
                   .empty());
    BOOST_TEST(validateInputSet({in}, {outDef(R"("id":"x","kind":"beast","transport":"tcp_connect","port":4000,"host":"192.0.2.1")")})
                   .empty());
    InputDef off = in;
    off.enabled = false;
    BOOST_TEST(validateInputSet({off}, {send}).empty());
}

BOOST_AUTO_TEST_CASE(v1_file_loads_unchanged)
{
    for (const char* text : {kV1Example, kV1Sim}) {
        const InputsFile f = parseInputsFile(text);
        BOOST_TEST(f.version == 1);
        BOOST_TEST(f.outputs.empty());
        const std::string back = inputsFileJson(f);
        BOOST_TEST(contains(back, "\"version\": 1"), back);
        for (const char* key : {"outputs", "group", "interface", "format"})
            BOOST_TEST(!contains(back, std::string("\"") + key + "\""), key << " in " << back);
        const InputsFile again = parseInputsFile(back);
        BOOST_TEST((again.inputs == f.inputs));
        BOOST_TEST(inputsFileJson(again) == back);
    }
    // A v1 file with an empty outputs list is not v2 content.
    BOOST_TEST(inputsFileErr(R"({"version":1,"outputs":[]})") == "");
}

BOOST_AUTO_TEST_CASE(v1_header_with_v2_content_rejected_with_hint)
{
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"outputs":[{"id":"o","kind":"beast","transport":"tcp_listen","port":1}]})"),
                        "'outputs' needs inputs file version 2"));
    const std::string uat = inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"uat_raw","host":"h","port":1}]})");
    BOOST_TEST(uat == "inputs[0] (u): 'uat_raw' needs inputs file version 2", uat);
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"beast_raw","host":"h","port":1}]})"),
                        "'beast_raw' needs inputs file version 2"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"pingstation_json","port":1,)"
                                      R"("transport":"udp","group":"239.1.1.5"}]})"),
                        "'group' needs inputs file version 2"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"readsb","host":"h","port":1,)"
                                      R"("transport":"tcp_connect"}]})"),
                        "'transport' needs inputs file version 2"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"dump978","host":"h","port":1,)"
                                      R"("transport":"tcp_connect"}]})"),
                        "'transport' needs inputs file version 2"));
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"pingstation_json","port":1,)"
                                      R"("transport":"udp"}]})"),
                        "'transport' needs inputs file version 2"));
    // pingstation_cat021 always had transport.
    BOOST_TEST(inputsFileErr(kV1Example) == "");
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"cue-tcp":{"enabled":false}}})"),
                        "'cue-tcp' needs inputs file version 2"));
}

BOOST_AUTO_TEST_CASE(version_3_rejected)
{
    BOOST_TEST(inputsFileErr(R"({"version":3})") == "version must be 1 or 2");
    BOOST_TEST(inputsFileErr(R"({"version":0})") == "version must be 1 or 2");
    BOOST_TEST(inputsFileErr(R"({"inputs":[]})") == "version must be 1 or 2");
    BOOST_TEST(inputsFileErr(R"({"version":2})") == "");
}

BOOST_AUTO_TEST_CASE(v2_file_round_trip)
{
    InputsFile f;
    f.inputs.push_back(def(R"("id":"b","type":"beast_raw","transport":"udp","port":30007,"group":"239.1.1.5","format":"avr")"));
    f.inputs.push_back(def(R"("id":"r","type":"readsb","transport":"tcp_listen","port":30048)"));
    f.inputs.push_back(def(kCatUdp));
    f.outputs.push_back(outDef(kBeastOut));
    f.outputs.push_back(outDef(R"("id":"m","name":"Mcast","kind":"json","transport":"udp","port":4000,"group":"239.1.1.9","ttl":4,"loopback":false)"));
    f.outputs.push_back(outDef(R"("id":"c","kind":"cat021","transport":"tcp_connect","port":4100,"host":"192.0.2.77","enabled":false)"));
    f.builtin_enabled["cue-tcp"] = false;
    f.builtin_enabled["cue-connect-1"] = true;
    f.builtin_enabled["cue-udp-2"] = false;
    f.builtin_enabled["readsb"] = true;
    const std::string text = inputsFileJson(f);
    BOOST_TEST(contains(text, "\"version\": 2"), text);
    const InputsFile back = parseInputsFile(text);
    BOOST_TEST(back.version == 2);
    BOOST_TEST((back.inputs == f.inputs));
    BOOST_TEST((back.outputs == f.outputs));
    // An enabled cue output is the default and is not written.
    auto expected = f.builtin_enabled;
    expected.erase("cue-connect-1");
    BOOST_TEST((back.builtin_enabled == expected));
    BOOST_TEST(inputsFileJson(back) == text);
    // Errors in an output entry are prefixed.
    const std::string e = inputsFileErr(R"({"version":2,"outputs":[{"id":"o","kind":"beast","port":1}]})");
    BOOST_TEST(e == "outputs[0] (o): transport is required for outputs (tcp_listen, tcp_connect or udp)", e);
    BOOST_TEST(contains(inputsFileErr(R"({"version":2,"outputs":[{"kind":"beast"}]})"), "outputs[0] (?): "));
    BOOST_TEST(contains(inputsFileErr(R"({"version":2,"outputs":[1]})"), "outputs"));
    // A set problem in the file is an error, and WARNs are kept.
    BOOST_TEST(contains(inputsFileErr(R"({"version":2,"inputs":[{"id":"p","type":"pingstation_cat021","transport":"tcp_listen","port":30005}],)"
                                      R"("outputs":[{"id":"o","kind":"beast","transport":"tcp_listen","port":30005}]})"),
                        "port 30005/tcp is already used by input p"));
    const InputsFile warned = parseInputsFile(
        R"({"version":2,"inputs":[{"id":"p","type":"beast_raw","transport":"tcp_listen","port":4000}],)"
        R"("outputs":[{"id":"o","kind":"beast","transport":"tcp_connect","host":"127.0.0.1","port":4000}]})");
    BOOST_TEST(warned.warnings.size() == 1u);
}

BOOST_AUTO_TEST_CASE(v1_shape_written_when_no_v2_content)
{
    // A v2 header with nothing v2 in it is written back as v1.
    const InputsFile f = parseInputsFile(R"({"version":2,"inputs":[{"id":"ps","type":"pingstation_json","port":4000}],"outputs":[]})");
    BOOST_TEST(f.version == 2);
    const std::string text = inputsFileJson(f);
    BOOST_TEST(contains(text, "\"version\": 1"), text);
    BOOST_TEST(!contains(text, "outputs"), text);
    BOOST_TEST(text == inputsFileJson(parseInputsFile(text)));
    // Removing the last v2 content gives v1 again.
    InputsFile g = parseInputsFile(R"({"version":2,"outputs":[{"id":"o","kind":"beast","transport":"udp","port":1,"host":"h"}]})");
    BOOST_TEST(contains(inputsFileJson(g), "\"version\": 2"));
    g.outputs.clear();
    BOOST_TEST(contains(inputsFileJson(g), "\"version\": 1"));
    // An input with a non-default transport keeps the file at v2; the default does not.
    InputsFile h;
    h.inputs.push_back(def(kReadsb));
    BOOST_TEST(contains(inputsFileJson(h), "\"version\": 1"));
    h.inputs.push_back(def(R"("id":"r2","type":"readsb","transport":"udp","port":30047)"));
    BOOST_TEST(contains(inputsFileJson(h), "\"version\": 2"));
}

BOOST_AUTO_TEST_CASE(builtin_cue_ids_v2_only)
{
    const InputsFile f = parseInputsFile(
        R"({"version":2,"builtin":{"readsb":{"enabled":false},"cue-tcp":{"enabled":false},)"
        R"("cue-connect-3":{"enabled":true},"cue-udp-12":{"enabled":false}}})");
    BOOST_TEST(f.builtin_enabled.size() == 4u);
    BOOST_TEST(!f.builtin_enabled.at("cue-tcp"));
    BOOST_TEST(contains(inputsFileJson(f), "\"version\": 2"));
    for (const char* bad : {"cue-connect-", "cue-connect-x", "cue-udp-01", "cue-tcp-1", "cue", "cue-other"})
        BOOST_TEST(contains(inputsFileErr(std::string(R"({"version":2,"builtin":{")") + bad + R"(":{"enabled":true}}})"),
                            "unknown input"),
                   bad);
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"builtin":{"cue-udp-1":{"enabled":true}}})"),
                        "'cue-udp-1' needs inputs file version 2"));
}

BOOST_AUTO_TEST_CASE(ref_lat_without_ref_lon_rejected)
{
    const std::string base = R"("id":"x","type":"beast_raw","host":"h","port":4000)";
    BOOST_TEST(!def(base).ref_lat);
    BOOST_TEST(err(base + R"(,"ref_lat":51.5)") == "ref_lat and ref_lon must be set together");
    BOOST_TEST(err(base + R"(,"ref_lon":-0.1)") == "ref_lat and ref_lon must be set together");
    BOOST_TEST(ok(base + R"(,"ref_lat":null,"ref_lon":null)"));
    const InputDef d = def(base + R"(,"ref_lat":51.5,"ref_lon":-0.125)");
    BOOST_REQUIRE(d.ref_lat && d.ref_lon);
    BOOST_TEST(*d.ref_lat == 51.5);
    BOOST_TEST(*d.ref_lon == -0.125);
    // Round trip, and only beast_raw and uat_raw take them.
    const InputDef back = def(inputDefJson(d).substr(1, inputDefJson(d).size() - 2));
    BOOST_TEST((back == d));
    BOOST_TEST(def(R"("id":"u","type":"uat_raw","host":"h","port":1,"ref_lat":1,"ref_lon":2)").ref_lon.has_value());
    BOOST_TEST(err(std::string(kReadsb) + R"(,"ref_lat":1,"ref_lon":2)") == "ref_lat applies to beast_raw and uat_raw only");
}

BOOST_AUTO_TEST_CASE(ref_range_checked)
{
    const std::string base = R"("id":"x","type":"beast_raw","host":"h","port":4000)";
    BOOST_TEST(ok(base + R"(,"ref_lat":-90,"ref_lon":180)"));
    BOOST_TEST(ok(base + R"(,"ref_lat":90,"ref_lon":-180)"));
    BOOST_TEST(err(base + R"(,"ref_lat":90.5,"ref_lon":0)") == "ref_lat must be a number from -90 to 90");
    BOOST_TEST(err(base + R"(,"ref_lat":0,"ref_lon":-180.5)") == "ref_lon must be a number from -180 to 180");
    BOOST_TEST(err(base + R"(,"ref_lat":"north","ref_lon":0)") == "ref_lat must be a number from -90 to 90");
}

BOOST_AUTO_TEST_CASE(ref_keys_are_version_2_keys)
{
    BOOST_TEST(contains(inputsFileErr(R"({"version":1,"inputs":[{"id":"u","type":"readsb","host":"h","port":1,)"
                                      R"("ref_lat":1,"ref_lon":2}]})"),
                        "'ref_lat' needs inputs file version 2"));
    InputsFile f;
    f.version = 2;
    f.inputs.push_back(def(R"("id":"b","type":"beast_raw","host":"h","port":4000,"ref_lat":51.5,"ref_lon":-0.125)"));
    const std::string text = inputsFileJson(f);
    BOOST_TEST(contains(text, "\"ref_lat\""), text);
    BOOST_TEST((parseInputsFile(text).inputs == f.inputs));
}
