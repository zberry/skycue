#include "recording_reader.hpp"

#include "json_lite.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "time_util.hpp"

#include <utility>

namespace skycued {

const char* recordKindName(RecordKind kind)
{
    switch (kind) {
    case RecordKind::obs: return "obs";
    case RecordKind::uat_uplink: return "uat_uplink";
    case RecordKind::update: return "update";
    case RecordKind::keyframe: return "keyframe";
    case RecordKind::drop: return "drop";
    case RecordKind::filter_change: return "filter_change";
    }
    return "obs";
}

LineKind parseRecordingLine(const std::string& line, RecordingRecord& out)
{
    if (line.find_first_not_of(" \t\r") == std::string::npos)
        return LineKind::ignore;
    auto record = JsonObject::parse(line);
    if (!record)
        return LineKind::bad;
    const std::string type = record->getString("type").value_or("");
    if (type == "uat_uplink") {
        auto time_text = record->getString("time");
        auto time = time_text ? parseIso8601Ms(*time_text) : std::nullopt;
        if (!time)
            return LineKind::ignore;
        out = RecordingRecord{};
        out.time = *time;
        out.kind = RecordKind::uat_uplink;
        return LineKind::received;
    }
    if (type == "obs") {
        auto obs = recording::observationFromRecord(*record);
        if (!obs)
            return LineKind::bad;
        out = RecordingRecord{};
        out.time = obs->received_time;
        out.obs = std::move(*obs);
        return LineKind::received;
    }
    if (!record->has("time") && record->getDouble("now")) {  // a readsb JSON-port capture
        auto obs = parseReadsbLine(line);
        if (!obs)
            return LineKind::bad;
        out = RecordingRecord{};
        out.time = obs->received_time;
        out.obs = std::move(*obs);
        return LineKind::readsb_raw;
    }
    if (record->has("event"))
        return recording::sentRecordFromRecord(*record, out) ? LineKind::sent : LineKind::bad;
    return LineKind::bad;
}

}  // namespace skycued
