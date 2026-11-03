#pragma once

// Reads one line of any recording skycued writes or replays (see
// docs/recording-format.md): a received-stream record ("type": "obs" or
// "uat_uplink"), a sent-stream record ("event": "update", "keyframe", "drop"
// or "filter_change"), or a readsb JSON-port aircraft line (a numeric "now"
// and no "time", as in tests/fixtures/readsb-sample.jsonl).
//
// ReplaySource plays the received and readsb lines; skycue-stats reads all of
// them.

#include "observation.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace skycued {

enum class RecordKind { obs, uat_uplink, update, keyframe, drop, filter_change };

// "obs", "uat_uplink", "update", "keyframe", "drop", "filter_change": the
// record's "type" or "event" text (a readsb line is an "obs").
const char* recordKindName(RecordKind kind);

struct RecordingRecord {
    std::chrono::system_clock::time_point time;  // the record's "time" (readsb: "now")
    RecordKind kind = RecordKind::obs;
    // obs: the observation, with input_id from the record's "input" (empty
    // when absent, and for a readsb line) and on_ground unset when the record
    // has no "on_ground". update/keyframe: the sent track's fields that have
    // a TrackObservation counterpart (received_time = time). drop: id only.
    // Otherwise empty.
    TrackObservation obs;
    std::optional<float> vrate_fpm;  // update/keyframe: "vrate_fpm"
    bool keyframe_flag = false;      // update/keyframe: "keyframe"
    std::string drop_reason;         // drop: one of the DropReason names
    std::string note;                // filter_change: "note"
    std::string uplink_hex;          // uat_uplink: "uplink_hex" (empty when absent)
    std::size_t file = 0;            // index of the input file; set by the reader of the files
};

// Which stream a line came from.
enum class LineKind {
    ignore,      // blank, or a "uat_uplink" record without a valid "time"
    received,    // "obs" or "uat_uplink" record
    sent,        // "update", "keyframe", "drop" or "filter_change" record
    readsb_raw,  // readsb JSON-port aircraft line
    bad,         // anything else, or a record of a known kind that does not parse
};

// Parses `line` into `out` (left in an unspecified state unless the result
// is received, sent or readsb_raw). Never throws on bad input.
LineKind parseRecordingLine(const std::string& line, RecordingRecord& out);

}  // namespace skycued
