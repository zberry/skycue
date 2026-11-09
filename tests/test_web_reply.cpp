#define BOOST_TEST_MODULE web_reply
#include <boost/test/unit_test.hpp>

#include "config.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "outputs/output_def.hpp"
#include "runtime_config.hpp"
#include "web_reply.hpp"

#include <stdexcept>
#include <string>

using namespace skycued;
using namespace skycued::webapi;

BOOST_AUTO_TEST_CASE(reply_fail_not_allowed_shapes)
{
    const ApiResponse ok = reply(200, "{}");
    BOOST_TEST(ok.status == 200u);
    BOOST_TEST(ok.body == "{}");
    BOOST_TEST(ok.content_type == "application/json");
    BOOST_TEST(ok.allow == "");

    const ApiResponse nf = fail(404, "no such thing");
    BOOST_TEST(nf.status == 404u);
    BOOST_TEST(nf.body == "{\"error\":\"no such thing\"}");

    const ApiResponse na = notAllowed("GET, PUT");
    BOOST_TEST(na.status == 405u);
    BOOST_TEST(na.body == "{\"error\":\"method not allowed\"}");
    BOOST_TEST(na.allow == "GET, PUT");
}

BOOST_AUTO_TEST_CASE(pinned_response_orders_keys_error_key_source)
{
    const ApiResponse r = pinnedResponse(PinnedKeyError("filters.max_range_nm", "set"));
    BOOST_TEST(r.status == 409u);
    BOOST_TEST(r.body == "{\"error\":\"filters.max_range_nm is set by --set and cannot be changed here\","
                         "\"key\":\"filters.max_range_nm\",\"source\":\"set\"}");
}

namespace {
template <class E> ApiResponse guarded(const E& e)
{
    return runGuarded([&]() -> ApiResponse { throw e; });
}
}  // namespace

BOOST_AUTO_TEST_CASE(resource_family_ladder)
{
    const auto check = [](const ApiResponse& r, unsigned status) {
        BOOST_TEST(r.status == status);
        BOOST_TEST(r.body == "{\"error\":\"a\"}");
    };
    check(guarded(InputDefError("a")), 400);
    check(guarded(OutputDefError("a")), 400);
    check(guarded(InputForbidden("a")), 403);
    check(guarded(InputNotFound("a")), 404);
    check(guarded(InputConflict("a")), 409);
    check(guarded(std::runtime_error("a")), 500);
    check(guarded(std::invalid_argument("a")), 500);
    const ConfigError ce("f", 1, "a");
    const ApiResponse c = guarded(ce);
    BOOST_TEST(c.status == 500u);
    BOOST_TEST(c.body == errorJson(ce.what()));

    const ApiResponse own = runGuarded([] { return reply(201, "{\"x\":1}"); });
    BOOST_TEST(own.status == 201u);
    BOOST_TEST(own.body == "{\"x\":1}");
}

BOOST_AUTO_TEST_CASE(config_family_ladder)
{
    const auto fam = [](const std::exception& e) { return failureResponse(e, ErrorFamily::config); };
    const PinnedKeyError pinned("filters.max_range_nm", "env");
    const ApiResponse p = fam(pinned);
    BOOST_TEST(p.status == 409u);
    BOOST_TEST(p.body.find("\"key\":\"filters.max_range_nm\",\"source\":\"env\"") != std::string::npos);
    BOOST_TEST(fam(std::invalid_argument("a")).status == 400u);
    BOOST_TEST(fam(ConfigError("f", 1, "a")).status == 400u);
    BOOST_TEST(fam(std::runtime_error("a")).status == 500u);
    BOOST_TEST(fam(InputNotFound("a")).status == 500u);
}

BOOST_AUTO_TEST_CASE(parse_body_object_accepts_objects_only)
{
    BOOST_TEST(parseBodyObject("{}", true).has_value());
    const auto o = parseBodyObject(" \n{\"a\":1}", true);
    BOOST_REQUIRE(o.has_value());
    BOOST_REQUIRE(o->getInt("a").has_value());
    BOOST_TEST(*o->getInt("a") == 1);
    for (const char* bad : {"[]", " []", "[1]", "", "   ", "not json", "{"})
        BOOST_TEST_CONTEXT(std::string(bad)) { BOOST_TEST(!parseBodyObject(bad, true).has_value()); }

    // Non-strict: the parser reads an array as an empty object (observed; the routes that pass
    // false answer an empty-array body as an empty object today).
    BOOST_TEST(parseBodyObject("[]", false).has_value());
    BOOST_TEST(parseBodyObject(" []", false).has_value());
    BOOST_TEST(!parseBodyObject("not json", false).has_value());
    BOOST_TEST(!parseBodyObject("{", false).has_value());
}
