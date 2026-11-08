#define BOOST_TEST_MODULE skycued_output_def
#include <boost/test/unit_test.hpp>

#include "outputs/output_def.hpp"

#include <skycue/net/endpoint.hpp>

#include <stdexcept>
#include <string>

using namespace skycued;

namespace {

JsonObject obj(const std::string& text)
{
    auto o = JsonObject::parse(text);
    if (!o)
        throw std::runtime_error("test JSON does not parse: " + text);
    return *o;
}

OutputDef def(const std::string& members) { return parseOutputDef(obj("{" + members + "}")); }

std::string err(const std::string& members)
{
    try {
        def(members);
    } catch (const OutputDefError& e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* const kListen = R"("id":"beast-out","kind":"beast","transport":"tcp_listen","port":30005)";
const char* const kConnect = R"("id":"c","kind":"uat","transport":"tcp_connect","host":"192.0.2.9","port":4100)";
const char* const kUdpHost = R"("id":"u","kind":"json","transport":"udp","host":"192.0.2.9","port":4000)";
const char* const kUdpGroup = R"("id":"m","kind":"cat021","transport":"udp","group":"239.1.1.9","port":4001)";

}  // namespace

BOOST_AUTO_TEST_CASE(kind_and_transport_required)
{
    const std::string kind_rule = "kind must be one of beast, uat, json, pingstation_json, cat021, cue";
    BOOST_TEST(err(R"("id":"x","transport":"udp","host":"h","port":1)") == kind_rule);
    BOOST_TEST(err(R"("id":"x","kind":"avr","transport":"udp","host":"h","port":1)") == kind_rule);
    BOOST_TEST(err(R"("id":"x","kind":"beast","port":1)") ==
               "transport is required for outputs (tcp_listen, tcp_connect or udp)");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"sctp","port":1)") ==
               "transport must be tcp_listen, tcp_connect or udp");
    BOOST_TEST(err(R"("kind":"beast","transport":"udp","port":1)") == "id is required");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_listen")") == "port is required");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_listen","port":0)") ==
               "port must be an integer from 1 to 65535");
    for (const char* kind : {"beast", "uat", "json", "pingstation_json", "cat021", "cue"})
        BOOST_TEST(err(std::string(R"("id":"x","kind":")") + kind + R"(","transport":"tcp_listen","port":1)") == "", kind);
    const OutputDef d = def(kListen);
    BOOST_TEST(d.enabled);
    BOOST_TEST(d.bind == "0.0.0.0");
    BOOST_TEST(d.name == "");
}

BOOST_AUTO_TEST_CASE(udp_needs_host_xor_group)
{
    BOOST_TEST(err(kUdpHost) == "");
    BOOST_TEST(err(kUdpGroup) == "");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"udp","port":1)") ==
               "udp needs a host (unicast) or a group (multicast)");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"udp","port":1,"host":"h","group":"239.1.1.1")") ==
               "host and group cannot both be set");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"udp","port":1,"group":"192.0.2.1")") ==
               "group must be a multicast address (224.0.0.0 to 239.255.255.255)");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_connect","port":1,"group":"239.1.1.1")") ==
               "group applies to udp only");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_connect","port":1)") == "host is required");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_listen","port":1,"host":"h")") ==
               "host is not used with tcp_listen");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_connect","host":"h","port":1,"bind":"0.0.0.0")") ==
               "bind applies to tcp_listen only");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"udp","host":"h","port":1,"peer":"192.0.2.1")") ==
               "peer applies to tcp_listen only");
    BOOST_TEST(err(R"("id":"x","kind":"beast","transport":"tcp_listen","port":1,"peer":"h")") ==
               "peer must be an IPv4 address");
}

BOOST_AUTO_TEST_CASE(ttl_loopback_interface_only_with_group)
{
    for (const char* key : {R"("ttl":4)", R"("loopback":false)", R"("interface":"192.0.2.10")"}) {
        const std::string name = std::string(key).substr(1, std::string(key).find('"', 1) - 1);
        const std::string want = name + " applies to a multicast group only";
        BOOST_TEST(err(std::string(kUdpHost) + "," + key) == want, key);
        BOOST_TEST(err(std::string(kListen) + "," + key) == want, key);
        BOOST_TEST(err(std::string(kConnect) + "," + key) == want, key);
    }
    const OutputDef d = def(std::string(kUdpGroup) + R"(,"ttl":4,"loopback":false,"interface":"192.0.2.10")");
    BOOST_TEST(d.ttl == 4);
    BOOST_TEST(!d.loopback);
    BOOST_TEST(d.iface == "192.0.2.10");
    const OutputDef defaults = def(kUdpGroup);
    BOOST_TEST(defaults.ttl == 1);
    BOOST_TEST(defaults.loopback);
    BOOST_TEST(err(std::string(kUdpGroup) + R"(,"interface":"eth0")") == "interface must be an IPv4 address");
}

BOOST_AUTO_TEST_CASE(ttl_range)
{
    for (const char* ok : {"1", "255", "\"16\""})
        BOOST_TEST(err(std::string(kUdpGroup) + R"(,"ttl":)" + ok) == "", ok);
    for (const char* bad : {"0", "256", "-1", "1.5", "\"x\"", "true"})
        BOOST_TEST(err(std::string(kUdpGroup) + R"(,"ttl":)" + bad) == "ttl must be an integer from 1 to 255", bad);
}

BOOST_AUTO_TEST_CASE(reserved_cue_ids)
{
    for (const char* id : {"cue-tcp", "cue-connect-1", "cue-connect-12", "cue-udp-1", "cue-udp-999"})
        BOOST_TEST(err(std::string(R"("id":")") + id + R"(","kind":"beast","transport":"tcp_listen","port":1)") ==
                       std::string("id '") + id + "' is reserved",
                   id);
    // Look-alikes are ordinary ids.
    for (const char* id : {"cue", "cue-tcp-2", "cue-connect-", "cue-connect-x", "cue-udp-01", "cue-out", "my-cue-tcp"})
        BOOST_TEST(err(std::string(R"("id":")") + id + R"(","kind":"beast","transport":"tcp_listen","port":1)") == "", id);
    BOOST_TEST(err(R"("id":"Bad_Id","kind":"beast","transport":"tcp_listen","port":1)") ==
               "id must be 1 to 32 characters of a-z, 0-9 and -");
    BOOST_TEST(isReservedCueId("cue-udp-3"));
    BOOST_TEST(!isReservedCueId("readsb"));
}

BOOST_AUTO_TEST_CASE(output_only_keys_refused)
{
    BOOST_TEST(err(std::string(kListen) + R"(,"builtin":false)") == "unknown key 'builtin'");
    BOOST_TEST(err(std::string(kListen) + R"(,"state":{})") == "unknown key 'state'");
}

BOOST_AUTO_TEST_CASE(unknown_key_refused)
{
    BOOST_TEST(err(std::string(kListen) + R"(,"colour":"red")") == "unknown key 'colour'");
    BOOST_TEST(err(std::string(kListen) + R"(,"port":2)") == "duplicate key 'port'");
    BOOST_TEST(err(std::string(kListen) + R"(,"format":"avr")") == "unknown key 'format'");
}

BOOST_AUTO_TEST_CASE(round_trip_every_kind)
{
    const char* const members[] = {
        R"("id":"a","kind":"beast","transport":"tcp_listen","port":30005)",
        R"("id":"b","name":"UAT out","kind":"uat","enabled":false,"transport":"tcp_listen","bind":"127.0.0.1","port":30978,"peer":"192.0.2.4")",
        R"("id":"c","kind":"json","transport":"tcp_connect","host":"feed.example","port":30047)",
        R"("id":"d","kind":"pingstation_json","transport":"udp","host":"192.0.2.9","port":4000)",
        R"("id":"e","kind":"cat021","transport":"udp","group":"239.1.1.9","port":4001,"interface":"192.0.2.10","ttl":8,"loopback":false)",
        R"("id":"f","kind":"cue","transport":"udp","group":"239.1.1.10","port":4002)"};
    for (const char* m : members) {
        const OutputDef d = def(m);
        const std::string json = outputDefJson(d);
        BOOST_TEST((def(json.substr(1, json.size() - 2)) == d), m << " -> " << json);
        BOOST_TEST(!contains(json, "builtin"));
    }
    OutputDef b = def(members[0]);
    b.builtin = true;
    BOOST_TEST(!contains(outputDefJson(b), "builtin"));
    // Key order and applicability.
    BOOST_TEST(outputDefJson(def(members[0])) ==
               R"({"id":"a","name":"","kind":"beast","enabled":true,"transport":"tcp_listen","bind":"0.0.0.0","port":30005,"peer":null})");
    BOOST_TEST(!contains(outputDefJson(def(members[3])), "ttl"));
    BOOST_TEST(!contains(outputDefJson(def(members[2])), "bind"));
}

BOOST_AUTO_TEST_CASE(name_rules)
{
    BOOST_TEST(err(std::string(kListen) + R"(,"name":"a\u0001b")") == "name must be at most 64 printable characters");
    bool repaired = false;
    const OutputDef d = parseOutputDef(obj(std::string("{") + kListen + R"(,"name":"a\u0001b"})"), NameRule::repair, &repaired);
    BOOST_TEST(repaired);
    BOOST_TEST(d.name == "ab");
}

BOOST_AUTO_TEST_CASE(to_endpoint_matches_parse_endpoint)
{
    using namespace skycue::net;
    const Endpoint listen = toEndpoint(def(std::string(kListen) + R"(,"bind":"127.0.0.1","peer":"192.0.2.4")"));
    BOOST_TEST((listen.transport == Transport::tcp_listen));
    BOOST_TEST(listen.bind == "127.0.0.1");
    BOOST_TEST(listen.peer == "192.0.2.4");
    BOOST_TEST(listen.port == 30005);

    const Endpoint connect = toEndpoint(def(kConnect));
    BOOST_TEST((connect.transport == Transport::tcp_connect));
    BOOST_TEST(connect.host == "192.0.2.9");

    const Endpoint uni = toEndpoint(def(kUdpHost));
    BOOST_TEST((uni.transport == Transport::udp));
    BOOST_TEST(uni.host == "192.0.2.9");
    BOOST_TEST(uni.group.empty());

    const Endpoint mc = toEndpoint(def(std::string(kUdpGroup) + R"(,"interface":"192.0.2.10","ttl":8,"loopback":false)"));
    BOOST_TEST(mc.group == "239.1.1.9");
    BOOST_TEST(mc.iface == "192.0.2.10");
    BOOST_TEST(mc.ttl == 8);
    BOOST_TEST(!mc.loopback);
    BOOST_TEST(describe(mc) == describe(parseEndpoint({"udp", "", "", "4001", "", "239.1.1.9", "192.0.2.10", "8", "false"},
                                                      Role::send)));
    const Endpoint mc_default = toEndpoint(def(kUdpGroup));
    BOOST_TEST(mc_default.ttl == 1);
    BOOST_TEST(mc_default.loopback);
}
