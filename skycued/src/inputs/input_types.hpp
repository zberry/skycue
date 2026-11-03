#pragma once

// Types shared by the network inputs (pingStation JSON and CAT021) and the
// code that reports on them.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace skycued {

// Which clock a device's timestamps are on. `gps` timestamps run 18 s ahead
// of UTC and are corrected before use (spec §9).
enum class TimeMode { utc, gps };

// The last status a pingStation reported (JSON `status` object or HTTP
// /api/v1/status).
struct DeviceStatus {
    std::optional<double> lat, lon, alt_m;
    std::optional<int> alt_type, gps_status, receiver_status;
    std::string version /* "major.minor.build" */, guid;
    std::optional<std::chrono::system_clock::time_point> device_time;  // as reported, not clamped
    std::chrono::system_clock::time_point received_at{};
};

// Running counts for one input.
struct InputCounters {
    uint64_t connects = 0, bytes = 0, messages = 0, observations = 0, uplinks = 0, rejected = 0,
             records_skipped = 0, sac_sic_mismatch = 0, nogo_records = 0, clock_rejects = 0,
             rejected_peer = 0, raw_unknown_len = 0, status_ok = 0, status_fail = 0,
             capture_dropped = 0;  // capture lines and raw frames a failed write dropped

    InputCounters& operator+=(const InputCounters& o)
    {
        connects += o.connects;
        bytes += o.bytes;
        messages += o.messages;
        observations += o.observations;
        uplinks += o.uplinks;
        rejected += o.rejected;
        records_skipped += o.records_skipped;
        sac_sic_mismatch += o.sac_sic_mismatch;
        nogo_records += o.nogo_records;
        clock_rejects += o.clock_rejects;
        rejected_peer += o.rejected_peer;
        raw_unknown_len += o.raw_unknown_len;
        status_ok += o.status_ok;
        status_fail += o.status_fail;
        capture_dropped += o.capture_dropped;
        return *this;
    }
};

// One input's line in the periodic stats report.
struct InputStatsLine {
    std::string id;
    bool connected = false;
    uint64_t messages = 0, observations = 0, uplinks = 0, rejected = 0;
};

}  // namespace skycued
