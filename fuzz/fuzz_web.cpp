// libFuzzer harness: the web API (docs/web-api.md) and the JSON helpers.
// The first input byte picks the mode ('0' + mode in the seeds):
//   0  raw HTTP request bytes through Beast's request parser (64 KiB body
//      limit, as the server), built into an ApiRequest by makeApiRequest
//      exactly as WebServer does; an /api path goes to handleApi on the
//      stubbed daemon (api_stub.hpp): its status must be 2xx-4xx and a JSON
//      reply valid UTF-8; any other path goes to staticPath, whose result
//      must keep its rule (web_api.hpp).
//   1  JsonObject::parse on the payload; every getter on every key, into
//      nested objects and object arrays to depth 3. Nothing may throw.
//   2  JsonWriter().add("k", payload): valid UTF-8 that parses back, to the
//      payload itself when the payload is valid UTF-8.
// No socket is opened and the io_context never runs; a request that may
// have changed state (any method but GET/HEAD) resets the stub.

#include "api_stub.hpp"
#include "fuzz_util.hpp"
#include "json_lite.hpp"
#include "web_api.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace http = boost::beast::http;
using skycued::JsonObject;
using skycued::JsonWriter;

boost::asio::io_context* g_io = nullptr;
skycuefuzz::ApiStub* g_stub = nullptr;

template <class S> std::string toString(const S& view) { return std::string(view.data(), view.size()); }

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// web_api.hpp: starts with '/', every segment non-empty and neither "." nor
// "..", no NUL or backslash anywhere.
bool staticPathRuleHolds(const std::string& p)
{
    if (p.empty() || p[0] != '/')
        return false;
    if (p.find('\0') != std::string::npos || p.find('\\') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (true) {
        const std::size_t end = p.find('/', start);
        const std::string seg = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
}

void request(const std::string& payload)
{
    http::request_parser<http::string_body> parser;
    parser.eager(true);
    parser.body_limit(64 * 1024);
    boost::beast::error_code ec;
    std::size_t pos = 0;
    while (!parser.is_done() && pos < payload.size()) {
        const std::size_t n = parser.put(boost::asio::buffer(payload.data() + pos, payload.size() - pos), ec);
        if (ec == http::error::need_more) {
            ec = {};
            if (n == 0)
                break;
        }
        if (ec)
            return;
        pos += n;
        if (n == 0)
            break;
    }
    if (!parser.is_done())
        return;
    auto req = parser.release();
    const std::string method = toString(req.method_string());
    const std::string target = toString(req.target());
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& field : req)
        fields.emplace_back(toString(field.name_string()), toString(field.value()));
    const auto api = skycued::webapi::makeApiRequest(method, target, fields, std::move(req.body()));

    if (!skycued::webapi::isApiPath(api.path)) {
        const auto rel = skycued::webapi::staticPath(target);
        if (rel)
            FUZZ_CHECK(staticPathRuleHolds(*rel));
        return;
    }
    skycued::webapi::ApiContext ctx{*g_stub->engine, *g_stub->runtime, g_stub->www, &g_stub->daemon};
    skycued::webapi::ApiResponse r;
    try {
        r = skycued::webapi::handleApi(api, ctx);
    } catch (...) {
        const bool handle_api_threw = true;
        FUZZ_CHECK(!handle_api_threw);
    }
    // Every 5xx handleApi can give here is a bug: the stub's directory is
    // writable, so no file write fails, and DaemonInfo is set.
    FUZZ_CHECK(r.status >= 200 && r.status < 500);
    if (startsWith(r.content_type, "application/json"))
        FUZZ_CHECK(skycuefuzz::validUtf8(r.body));
    if (api.method != "GET" && api.method != "HEAD")
        g_stub->reset();
}

void visit(const JsonObject& o, int depth)
{
    for (const auto& k : o.keys()) {
        const char* key = k.c_str();
        (void)o.has(key);
        (void)o.getString(key);
        (void)o.getDouble(key);
        (void)o.getInt(key);
        (void)o.getBool(key);
        (void)o.getStringArray(key);
        const auto arr = o.getObjectArray(key);
        const auto sub = o.getObject(key);
        if (depth >= 3)
            continue;
        if (arr)
            for (const auto& e : *arr)
                visit(e, depth + 1);
        if (sub)
            visit(*sub, depth + 1);
    }
}

void parseJson(const std::string& payload)
{
    try {
        const auto o = JsonObject::parse(payload);
        if (o)
            visit(*o, 0);
    } catch (...) {
        const bool json_object_threw = true;
        FUZZ_CHECK(!json_object_threw);
    }
}

void writeJson(const std::string& payload)
{
    const std::string out = JsonWriter().add("k", payload).str();
    FUZZ_CHECK(skycuefuzz::validUtf8(out));
    const auto o = JsonObject::parse(out, out.size());
    FUZZ_CHECK(o.has_value());
    const auto back = o->getString("k");
    FUZZ_CHECK(back.has_value());
    if (skycuefuzz::validUtf8(payload))
        FUZZ_CHECK(*back == payload);
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    skycuefuzz::quietLogs();
    g_io = new boost::asio::io_context();
    g_stub = new skycuefuzz::ApiStub(*g_io, skycuefuzz::makeWorkDir("web"));
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    const auto in = skycuefuzz::splitMode(data, size, 3);
    switch (in.mode) {
    case 0: request(in.payload); break;
    case 1: parseJson(in.payload); break;
    default: writeJson(in.payload); break;
    }
    return 0;
}
