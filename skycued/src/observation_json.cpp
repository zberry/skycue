#include "observation_json.hpp"

#include "time_util.hpp"

#include <cmath>
#include <limits>

namespace skycued {

namespace {
const char* const kNavNames[] = {"autopilot", "vnav", "althold", "approach", "lnav", "tcas"};

std::optional<float> readFloat(const JsonObject& o, const char* k)
{
    auto v = o.getDouble(k);
    if (!v || std::fabs(*v) > static_cast<double>(std::numeric_limits<float>::max()))
        return std::nullopt;
    return static_cast<float>(*v);
}
}  // namespace

std::vector<std::string> navModeNames(uint8_t mask)
{
    std::vector<std::string> out;
    for (int i = 0; i < 6; ++i)
        if (mask & (1u << i))
            out.emplace_back(kNavNames[i]);
    return out;
}

std::string navModesJson(uint8_t mask)
{
    std::vector<std::string> q;
    for (const auto& n : navModeNames(mask))
        q.push_back("\"" + n + "\"");
    return jsonArray(q);
}

std::optional<uint8_t> parseNavModes(const JsonObject& j, const char* key)
{
    auto names = j.getStringArray(key);
    if (!names)
        return std::nullopt;
    uint8_t mask = 0;
    for (const auto& n : *names) {
        bool known = false;
        for (int i = 0; i < 6; ++i)
            if (n == kNavNames[i]) {
                mask |= static_cast<uint8_t>(1u << i);
                known = true;
            }
        if (!known)
            return std::nullopt;
    }
    return mask;
}

std::string acasRaJson(const AcasRaState& ra)
{
    JsonWriter w;
    w.add("active", ra.active)
        .add("ara", static_cast<int64_t>(ra.ara))
        .add("rac", static_cast<int64_t>(ra.rac))
        .add("rat", ra.rat)
        .add("mte", ra.mte)
        .add("text", ra.text);
    if (ra.tti != 0) {
        JsonWriter t;
        if (ra.tti == 1) {
            t.add("type", "address");
            if (ra.threat_address)
                t.add("address", hexAddress(*ra.threat_address));
        } else {
            t.add("type", "position");
        }
        if (ra.threat_alt_ft)
            t.add("alt_ft", static_cast<int64_t>(*ra.threat_alt_ft));
        if (ra.threat_range_nm && std::isfinite(*ra.threat_range_nm))
            t.addExact("range_nm", *ra.threat_range_nm);
        if (ra.threat_bearing_deg && std::isfinite(*ra.threat_bearing_deg))
            t.addExact("bearing_deg", *ra.threat_bearing_deg);
        w.addRaw("threat", t.str());
    }
    return w.str();
}

std::optional<AcasRaState> parseAcasRa(const JsonObject& o)
{
    AcasRaState ra;
    auto active = o.getBool("active");
    auto ara = o.getInt("ara");
    auto rac = o.getInt("rac");
    auto rat = o.getBool("rat");
    auto mte = o.getBool("mte");
    auto text = o.getString("text");
    if (!active || !ara || !rac || !rat || !mte || !text || *ara < 0 || *ara > 0x3fff || *rac < 0 || *rac > 15)
        return std::nullopt;
    ra.active = *active;
    ra.ara = static_cast<uint16_t>(*ara);
    ra.rac = static_cast<uint8_t>(*rac);
    ra.rat = *rat;
    ra.mte = *mte;
    ra.text = *text;
    if (o.has("threat")) {
        auto t = o.getObject("threat");
        auto type = t ? t->getString("type") : std::nullopt;
        if (!type || (*type != "address" && *type != "position"))
            return std::nullopt;
        ra.tti = *type == "address" ? 1 : 2;
        if (t->has("address")) {
            auto a = t->getString("address");
            auto h = a ? parseHexAddress(*a) : std::nullopt;
            if (!h)
                return std::nullopt;
            ra.threat_address = *h;
        }
        if (t->has("alt_ft")) {
            auto v = t->getInt("alt_ft");
            if (!v || std::llabs(*v) > 1000000)
                return std::nullopt;
            ra.threat_alt_ft = static_cast<int32_t>(*v);
        }
        if (t->has("range_nm")) {
            ra.threat_range_nm = readFloat(*t, "range_nm");
            if (!ra.threat_range_nm)
                return std::nullopt;
        }
        if (t->has("bearing_deg")) {
            ra.threat_bearing_deg = readFloat(*t, "bearing_deg");
            if (!ra.threat_bearing_deg)
                return std::nullopt;
        }
    }
    return ra;
}

}  // namespace skycued
