#define BOOST_TEST_MODULE net_endpoint
#include <boost/test/unit_test.hpp>

#include "net_test_support.hpp"
#include "skycue/net/endpoint.hpp"
#include "skycue/net/log.hpp"

#include <string>
#include <vector>

using namespace skycue::net;

BOOST_AUTO_TEST_CASE(tcp_listen_defaults_and_describe) {
    auto e = parseEndpoint({"tcp_listen", "", "", "30005", "", "", "", "", ""}, Role::receive);
    BOOST_TEST(e.bind == "0.0.0.0"); BOOST_TEST(e.port == 30005);
    BOOST_TEST(describe(e) == "tcp_listen 0.0.0.0:30005");
}
BOOST_AUTO_TEST_CASE(tcp_connect_requires_host) {
    BOOST_CHECK_THROW(parseEndpoint({"tcp_connect", "", "", "30005", "", "", "", "", ""}, Role::receive), EndpointError);
    auto e = parseEndpoint({"tcp_connect", "", "receiver.example", "30005", "", "", "", "", ""}, Role::send);
    BOOST_TEST(describe(e) == "tcp_connect receiver.example:30005");
}
BOOST_AUTO_TEST_CASE(udp_receive_multicast_group_and_interface) {
    auto e = parseEndpoint({"udp", "", "", "4001", "", "239.1.1.5", "192.0.2.10", "", ""}, Role::receive);
    BOOST_TEST(e.group == "239.1.1.5"); BOOST_TEST(e.iface == "192.0.2.10");
    BOOST_TEST(describe(e) == "udp 239.1.1.5:4001 via 192.0.2.10");
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "", "4001", "", "192.0.2.5", "", "", ""}, Role::receive), EndpointError); // not multicast
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "", "4001", "", "", "192.0.2.10", "", ""}, Role::receive), EndpointError); // iface without group
}
BOOST_AUTO_TEST_CASE(udp_send_rules) {
    auto e = parseEndpoint({"udp", "", "", "4001", "", "239.1.1.5", "", "4", "no"}, Role::send);
    BOOST_TEST(e.ttl == 4); BOOST_TEST(e.loopback == false);
    BOOST_TEST(describe(e) == "udp 239.1.1.5:4001 ttl 4");
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "", "4001", "", "", "", "", ""}, Role::send), EndpointError);             // neither host nor group
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "h", "4001", "", "239.1.1.5", "", "", ""}, Role::send), EndpointError);   // both
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "h", "4001", "", "", "", "4", ""}, Role::send), EndpointError);           // ttl without group
    BOOST_CHECK_THROW(parseEndpoint({"udp", "", "", "4001", "", "239.1.1.5", "", "0", ""}, Role::send), EndpointError);   // ttl range
}
BOOST_AUTO_TEST_CASE(port_rules) {
    BOOST_CHECK_THROW(parseEndpoint({"tcp_listen", "", "", "", "", "", "", "", ""}, Role::receive), EndpointError);
    BOOST_CHECK_THROW(parseEndpoint({"tcp_listen", "", "", "0", "", "", "", "", ""}, Role::receive), EndpointError);
    BOOST_TEST(parseEndpoint({"tcp_listen", "", "", "0", "", "", "", "", ""}, Role::receive, true).port == 0);
    BOOST_CHECK_THROW(parseEndpoint({"tcp_listen", "", "", "65536", "", "", "", "", ""}, Role::receive), EndpointError);
    BOOST_CHECK_THROW(parseEndpoint({"bogus", "", "", "1", "", "", "", "", ""}, Role::receive), EndpointError);
}
BOOST_AUTO_TEST_CASE(ipv6_literals_accepted) {
    auto e = parseEndpoint({"tcp_listen", "::1", "", "30005", "", "", "", "", ""}, Role::receive);
    BOOST_TEST(describe(e) == "tcp_listen [::1]:30005");
    BOOST_TEST(isMulticast(boost::asio::ip::make_address("ff02::1")));
    BOOST_TEST(!isMulticast(boost::asio::ip::make_address("10.0.0.1")));
}
BOOST_AUTO_TEST_CASE(log_sink_receives_messages) {
    std::vector<std::string> got;
    setLogSink([&](LogLevel, const std::string& m) { got.push_back(m); });
    log(LogLevel::info, "hello");
    setLogSink(nullptr);
    log(LogLevel::info, "dropped");
    BOOST_TEST(got == std::vector<std::string>{"hello"});
}
BOOST_AUTO_TEST_CASE(unmap_v4_mapped_addresses) {
    namespace ip = boost::asio::ip;
    BOOST_TEST(unmapV4(ip::make_address("::ffff:192.0.2.7")) == ip::make_address("192.0.2.7"));
    BOOST_TEST(unmapV4(ip::make_address("192.0.2.7")) == ip::make_address("192.0.2.7"));
    BOOST_TEST(unmapV4(ip::make_address("2001:db8::1")) == ip::make_address("2001:db8::1"));
    BOOST_TEST(unmapV4(ip::make_address("::1")) == ip::make_address("::1"));
}
