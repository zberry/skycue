#include "web_inputs.hpp"

#include "engine.hpp"
#include "json_lite.hpp"
#include "time_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

const char kPrefix[] = "/api/inputs/";
constexpr std::size_t kPrefixLen = sizeof kPrefix - 1;

int64_t num(uint64_t v) { return static_cast<int64_t>(v); }

ApiResponse reply(unsigned status, std::string body)
{
    ApiResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

ApiResponse fail(unsigned status, const std::string& message) { return reply(status, errorJson(message)); }

ApiResponse notAllowed(const char* allow)
{
    ApiResponse r = fail(405, "method not allowed");
    r.allow = allow;
    return r;
}

void putCounters(JsonWriter& w, const InputCounters& c)
{
    w.add("connects", num(c.connects))
        .add("bytes", num(c.bytes))
        .add("messages", num(c.messages))
        .add("observations", num(c.observations))
        .add("uplinks", num(c.uplinks))
        .add("rejected", num(c.rejected))
        .add("records_skipped", num(c.records_skipped))
        .add("sac_sic_mismatch", num(c.sac_sic_mismatch))
        .add("nogo_records", num(c.nogo_records))
        .add("clock_rejects", num(c.clock_rejects))
        .add("rejected_peer", num(c.rejected_peer))
        .add("raw_unknown_len", num(c.raw_unknown_len))
        .add("status_ok", num(c.status_ok))
        .add("status_fail", num(c.status_fail))
        .add("capture_dropped", num(c.capture_dropped));
}

std::string deviceStatusJson(const DeviceStatus& d)
{
    JsonWriter w;
    if (d.lat)
        w.addExact("lat", *d.lat);
    if (d.lon)
        w.addExact("lon", *d.lon);
    if (d.alt_m)
        w.add("alt_m", *d.alt_m);
    for (const auto& kv : {std::make_pair("alt_type", &d.alt_type), std::make_pair("gps_status", &d.gps_status),
                           std::make_pair("receiver_status", &d.receiver_status)})
        if (*kv.second)
            w.add(kv.first, static_cast<int64_t>(**kv.second));
    if (!d.version.empty())
        w.add("version", d.version);
    if (!d.guid.empty())
        w.add("guid", d.guid);
    if (d.device_time)
        w.add("device_time", formatIso8601Ms(*d.device_time));
    w.add("received_at", formatIso8601Ms(d.received_at));
    return w.str();
}

std::string stateJson(const InputState& s, std::chrono::system_clock::time_point now)
{
    JsonWriter w;
    w.add("connected", s.connected).add("receiving", s.receiving).add("peers", static_cast<int64_t>(s.peers));
    if (s.last_data_time) {
        const double age = std::chrono::duration<double>(now - *s.last_data_time).count();
        w.add("last_data_age_s", std::max(0.0, std::round(age * 10) / 10));
    }
    if (s.clock_offset_s && std::isfinite(*s.clock_offset_s))
        w.add("clock_offset_s", *s.clock_offset_s);
    w.add("status_fail_streak", static_cast<int64_t>(s.status_fail_streak)).add("last_error", s.last_error);
    putCounters(w, s.counters);
    if (s.device_status)
        w.addRaw("device_status", deviceStatusJson(*s.device_status));
    return w.str();
}

std::string statusJson(const InputEntry& e)
{
    JsonWriter w;
    w.add("id", e.def.id)
        .add("status_url", e.def.status_url)
        .add("status_interval_s", static_cast<int64_t>(e.def.status_interval_s))
        .add("status_ok", num(e.state.counters.status_ok))
        .add("status_fail", num(e.state.counters.status_fail))
        .add("status_fail_streak", static_cast<int64_t>(e.state.status_fail_streak));
    if (e.state.device_status)
        w.addRaw("device_status", deviceStatusJson(*e.state.device_status));
    return w.str();
}

std::string entryJson(const InputEntry& e) { return inputJson(e, std::chrono::system_clock::now()); }

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

ApiResponse putRoute(InputManager& m, const std::string& id, const std::string& body)
{
    const JsonObject o = bodyObject(body);
    const auto found = m.find(id);
    if (!found)
        throw InputNotFound("no input '" + id + "'");
    if (found->def.builtin) {
        for (const auto& k : o.keys())
            if (k != "enabled")
                throw InputForbidden("built-in input '" + id + "' accepts only enabled");
        const auto enabled = o.getBool("enabled");
        if (!enabled)
            throw InputDefError("enabled must be true or false");
        return reply(200, entryJson(m.setEnabled(id, *enabled)));
    }
    if (o.has("id")) {
        const std::string given = o.getString("id").value_or("");
        if (given != id)
            throw InputDefError("id '" + clipForMessage(given) + "' does not match the path's '" + id + "'");
        return reply(200, entryJson(m.replace(id, parseInputDef(o))));
    }
    return reply(200, entryJson(m.replace(id, parseInputDef(bodyObject(withId(body, id))))));
}

ApiResponse toggleRoute(InputManager& m, const std::string& id, const std::string& body, bool enabled)
{
    if (!bodyObject(body).keys().empty())
        throw InputDefError("the body must be an empty JSON object");
    return reply(200, entryJson(m.setEnabled(id, enabled)));
}

ApiResponse noPath(const std::string& path) { return fail(404, "no such API path: " + clipForMessage(path)); }

// rejectUnsafe, then 409 while replaying: before the body is read or an id
// looked up.
std::optional<ApiResponse> rejectWrite(const ApiRequest& req, const InputManager& m)
{
    if (auto rejected = rejectUnsafe(req))
        return rejected;
    if (m.config().replaying)
        return fail(409, "inputs cannot be changed while replaying a recording");
    return std::nullopt;
}

ApiResponse route(const ApiRequest& req, InputManager& m)
{
    const std::string& method = req.method;
    if (req.path == "/api/inputs") {
        if (method == "GET")
            return reply(200, inputsJson(m));
        if (method != "POST")
            return notAllowed("GET, POST");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        return reply(201, entryJson(m.create(parseInputDef(bodyObject(req.body)))));
    }
    const std::string rest = req.path.substr(kPrefixLen);
    const std::size_t slash = rest.find('/');
    const std::string id = rest.substr(0, slash), sub = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (id.empty() || sub.find('/') != std::string::npos)
        return noPath(req.path);
    if (!isValidInputId(id))  // before any lookup; the segment is not echoed
        return fail(400, "invalid input id (1 to 32 characters of a-z, 0-9 and -)");
    if (slash == std::string::npos) {
        if (method != "PUT" && method != "DELETE")
            return notAllowed("PUT, DELETE");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        if (method == "PUT")
            return putRoute(m, id, req.body);
        m.remove(id);
        ApiResponse r = reply(204, "");
        r.content_type.clear();
        return r;
    }
    if (sub == "enable" || sub == "disable") {
        if (method != "POST")
            return notAllowed("POST");
        if (auto rejected = rejectWrite(req, m))
            return *rejected;
        return toggleRoute(m, id, req.body, sub == "enable");
    }
    if (sub == "status") {
        if (method != "GET")
            return notAllowed("GET");
        const auto found = m.find(id);
        if (!found)
            return fail(404, "no input '" + id + "'");
        return reply(200, statusJson(*found));
    }
    return noPath(req.path);
}

}  // namespace

std::string inputJson(const InputEntry& entry, std::chrono::system_clock::time_point now)
{
    std::string out = inputDefJson(entry.def);
    out.pop_back();  // the closing '}'
    out += entry.def.builtin ? ",\"builtin\":true" : ",\"builtin\":false";
    out += ",\"state\":" + stateJson(entry.state, now) + "}";
    return out;
}

std::string inputsJson(const InputManager& inputs, const char* type)
{
    const auto now = std::chrono::system_clock::now();
    std::vector<std::string> items;
    for (const auto& e : inputs.list())
        items.push_back(inputJson(e, now));
    JsonWriter w;
    if (type)
        w.add("type", type);
    w.addRaw("inputs", jsonArray(items));
    return w.str();
}

bool isInputsPath(const std::string& path)
{
    return path == "/api/inputs" || path.compare(0, kPrefixLen, kPrefix) == 0;
}

ApiResponse handleInputsApi(const ApiRequest& request, Engine& engine)
{
    try {
        return route(request, engine.inputs());
    } catch (const InputDefError& e) {
        return fail(400, e.what());
    } catch (const InputForbidden& e) {
        return fail(403, e.what());
    } catch (const InputNotFound& e) {
        return fail(404, e.what());
    } catch (const InputConflict& e) {
        return fail(409, e.what());
    } catch (const std::exception& e) {
        return fail(500, e.what());
    }
}

}  // namespace webapi
}  // namespace skycued
