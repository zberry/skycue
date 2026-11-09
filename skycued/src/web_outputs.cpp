#include "web_outputs.hpp"

#include "engine.hpp"
#include "json_lite.hpp"
#include "web_reply.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

const char kPrefix[] = "/api/outputs/";
constexpr std::size_t kPrefixLen = sizeof kPrefix - 1;

int64_t num(uint64_t v) { return static_cast<int64_t>(v); }

ApiResponse noPath(const std::string& path) { return fail(404, "no such API path: " + clipForMessage(path)); }

// The output as the Hub has it; without a Hub (no hub in this process) the
// definition alone, not running.
std::optional<OutputState> lookup(const Engine& engine, const std::string& id)
{
    if (const Hub* hub = engine.hub())
        return hub->find(id);
    for (const auto& def : engine.inputs().outputs())
        if (def.id == id) {
            OutputState s;
            s.def = def;
            return s;
        }
    return std::nullopt;
}

ApiResponse stateReply(unsigned status, const Engine& engine, const std::string& id)
{
    const auto found = lookup(engine, id);
    if (!found)
        throw std::runtime_error("output '" + id + "' is not running");
    return reply(status, outputJson(*found));
}

// The body as a JSON object; throws InputDefError when it is not one.
JsonObject bodyObject(const std::string& body)
{
    auto o = JsonObject::parse(body);
    if (!o)
        throw InputDefError("the body must be a JSON object");
    return *o;
}

// The PUT body with "id":"<id>" added (it parsed as an object, so it starts
// with '{' after any whitespace).
std::string withId(const std::string& body, const std::string& id)
{
    const std::size_t open = body.find('{');
    const std::size_t next = body.find_first_not_of(" \t\r\n", open + 1);
    std::string out = "{\"id\":";
    appendJsonString(out, id);
    if (next != std::string::npos && body[next] != '}')
        out += ',';
    return out + body.substr(open + 1);
}

ApiResponse putRoute(Engine& engine, const std::string& id, const std::string& body)
{
    InputManager& m = engine.inputs();
    const JsonObject o = bodyObject(body);
    const auto found = lookup(engine, id);
    if (!found)
        throw InputNotFound("no output '" + id + "'");
    if (found->def.builtin) {
        for (const auto& k : o.keys())
            if (k != "enabled")
                throw InputForbidden("built-in output '" + id + "' accepts only enabled");
        const auto enabled = o.getBool("enabled");
        if (!enabled)
            throw InputDefError("enabled must be true or false");
        m.setOutputEnabled(id, *enabled);
        return stateReply(200, engine, id);
    }
    if (o.has("id")) {
        const std::string given = o.getString("id").value_or("");
        if (given != id)
            throw InputDefError("id '" + clipForMessage(given) + "' does not match the path's '" + id + "'");
        m.replaceOutput(id, parseOutputDef(o));
    } else {
        m.replaceOutput(id, parseOutputDef(bodyObject(withId(body, id))));
    }
    return stateReply(200, engine, id);
}

ApiResponse toggleRoute(Engine& engine, const std::string& id, const std::string& body, bool enabled)
{
    if (!bodyObject(body).keys().empty())
        throw InputDefError("the body must be an empty JSON object");
    if (!lookup(engine, id))
        throw InputNotFound("no output '" + id + "'");
    engine.inputs().setOutputEnabled(id, enabled);
    return stateReply(200, engine, id);
}

// rejectUnsafe, then 409 while replaying: before the body is read or an id
// looked up.
std::optional<ApiResponse> rejectWrite(const ApiRequest& req, const InputManager& m)
{
    if (auto rejected = rejectUnsafe(req))
        return rejected;
    if (m.config().replaying)
        return fail(409, "outputs cannot be changed while replaying a recording");
    return std::nullopt;
}

ApiResponse route(const ApiRequest& req, Engine& engine)
{
    InputManager& m = engine.inputs();
    const std::string& method = req.method;
    if (req.path == "/api/outputs") {
        if (method == "GET") {
            if (const Hub* hub = engine.hub())
                return reply(200, outputsJson(*hub));
            return reply(200, "{\"outputs\":[]}");
        }
        if (method != "POST")
            return notAllowed("GET, POST");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        const OutputDef made = m.createOutput(parseOutputDef(bodyObject(req.body)));
        return stateReply(201, engine, made.id);
    }
    const std::string rest = req.path.substr(kPrefixLen);
    const std::size_t slash = rest.find('/');
    const std::string id = rest.substr(0, slash), sub = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (id.empty() || sub.find('/') != std::string::npos)
        return noPath(req.path);
    if (!isValidInputId(id))  // before any lookup; the segment is not echoed
        return fail(400, "invalid output id (1 to 32 characters of a-z, 0-9 and -)");
    if (slash == std::string::npos) {
        if (method != "PUT" && method != "DELETE")
            return notAllowed("PUT, DELETE");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        if (method == "PUT")
            return putRoute(engine, id, req.body);
        const auto found = lookup(engine, id);
        if (!found)
            throw InputNotFound("no output '" + id + "'");
        if (found->def.builtin)
            throw InputForbidden("output '" + id + "' is built in: it can only be enabled or disabled");
        m.removeOutput(id);
        ApiResponse r = reply(204, "");
        r.content_type.clear();
        return r;
    }
    if (sub == "enable" || sub == "disable") {
        if (method != "POST")
            return notAllowed("POST");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        return toggleRoute(engine, id, req.body, sub == "enable");
    }
    return noPath(req.path);
}

}  // namespace

std::string outputJson(const OutputState& state)
{
    std::string out = outputDefJson(state.def);
    out.pop_back();  // the closing '}'
    out += state.def.builtin ? ",\"builtin\":true" : ",\"builtin\":false";
    out += ",\"state\":" +
           JsonWriter()
               .add("up", state.up)
               .add("peers", num(state.counters.peers))
               .add("bytes", num(state.counters.bytes))
               .add("messages", num(state.counters.messages))
               .add("dropped", num(state.counters.dropped))
               .add("last_error", state.counters.last_error)
               .str() +
           "}";
    return out;
}

std::string outputsArrayJson(const Hub& hub)
{
    std::vector<std::string> items;
    for (const auto& s : hub.list())
        items.push_back(outputJson(s));
    return jsonArray(items);
}

std::string outputsJson(const Hub& hub, const char* type)
{
    JsonWriter w;
    if (type)
        w.add("type", type);
    w.addRaw("outputs", outputsArrayJson(hub));
    return w.str();
}

bool isOutputsPath(const std::string& path)
{
    return path == "/api/outputs" || path.compare(0, kPrefixLen, kPrefix) == 0;
}

ApiResponse handleOutputsApi(const ApiRequest& request, Engine& engine)
{
    return runGuarded([&] { return route(request, engine); });
}

}  // namespace webapi
}  // namespace skycued
