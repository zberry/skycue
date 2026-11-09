#pragma once

// Internal to the recorder: the field list a record is built from and the
// builders and writers the Recorder class uses. The public side of the codec
// (keys, headers, reading records back) is declared in recorder.hpp.

#include "recorder.hpp"

#include <string>
#include <vector>

namespace skycued {
namespace recording {

using Record = std::vector<Field>;

Field textField(const char* key, std::string value);

// Sent-stream key by name; throws std::logic_error for a name the table does not list.
const char* sentKey(const char* name);

Record observationRecord(const TrackObservation& obs);
Record trackRecord(const skycue::Track& t);

const std::vector<const char*>& receivedColumns();
const std::vector<const char*>& sentColumns();
std::string csvRow(const std::vector<const char*>& columns, const Record& r);
std::string jsonLine(const Record& r);

}  // namespace recording
}  // namespace skycued
