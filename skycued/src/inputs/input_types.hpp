#pragma once

// Types shared by the network inputs (pingStation JSON and CAT021) and the
// code that reports on them.

#include "decode/decoder_types.hpp"

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
    // The 1090 decoder of a beast_raw input with decode_raw (frame decoder spec 8.2); all 0 otherwise.
    // `decoder_observations` is DecodeCounters::observations; the three *_skipped are frames the
    // frame source dropped before the decoder.
    uint64_t frames = 0, crc_fail = 0, crc_fixed = 0, ap_unconfirmed = 0, no_position = 0, cpr_pending = 0,
             cpr_rejected = 0, decoded = 0, unmapped = 0, unknown_type = 0, evicted = 0, decoder_observations = 0,
             mlat_skipped = 0, modeac_skipped = 0, other_skipped = 0;
    // Comm-B register inference and ACAS RAs of the 1090 decoder (DecodeCounters).
    uint64_t commb_bds40 = 0, commb_bds50 = 0, commb_bds60 = 0, commb_ambiguous = 0, commb_other = 0,
             commb_empty = 0, commb_unknown = 0, acas_ra = 0, acas_ra_tc28 = 0;
    uint64_t rs_errors = 0;  // UAT: Reed-Solomon symbol errors corrected, summed
    // pingStation `source: decode`: datagrams without rawData, and byte-equal rawData skipped within 1 s.
    uint64_t no_raw = 0, raw_duplicates = 0;

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
        no_raw += o.no_raw;
        raw_duplicates += o.raw_duplicates;
        frames += o.frames;
        crc_fail += o.crc_fail;
        crc_fixed += o.crc_fixed;
        ap_unconfirmed += o.ap_unconfirmed;
        no_position += o.no_position;
        cpr_pending += o.cpr_pending;
        cpr_rejected += o.cpr_rejected;
        decoded += o.decoded;
        unmapped += o.unmapped;
        unknown_type += o.unknown_type;
        evicted += o.evicted;
        decoder_observations += o.decoder_observations;
        mlat_skipped += o.mlat_skipped;
        modeac_skipped += o.modeac_skipped;
        other_skipped += o.other_skipped;
        rs_errors += o.rs_errors;
        commb_bds40 += o.commb_bds40;
        commb_bds50 += o.commb_bds50;
        commb_bds60 += o.commb_bds60;
        commb_ambiguous += o.commb_ambiguous;
        commb_other += o.commb_other;
        commb_empty += o.commb_empty;
        commb_unknown += o.commb_unknown;
        acas_ra += o.acas_ra;
        acas_ra_tc28 += o.acas_ra_tc28;
        return *this;
    }
};

// Adds a decoder's counters to the decoder fields of `c`. A pingStation decode input calls it
// once per decoder (1090 and UAT), so the sums stay in one place.
inline void addDecodeCounters(InputCounters& c, const decode::DecodeCounters& d)
{
    c.frames += d.frames;
    c.crc_fail += d.crc_fail;
    c.crc_fixed += d.crc_fixed;
    c.ap_unconfirmed += d.ap_unconfirmed;
    c.no_position += d.no_position;
    c.cpr_pending += d.cpr_pending;
    c.cpr_rejected += d.cpr_rejected;
    c.decoded += d.decoded;
    c.unmapped += d.unmapped;
    c.unknown_type += d.unknown_type;
    c.evicted += d.evicted;
    c.commb_bds40 += d.commb_bds40;
    c.commb_bds50 += d.commb_bds50;
    c.commb_bds60 += d.commb_bds60;
    c.commb_ambiguous += d.commb_ambiguous;
    c.commb_other += d.commb_other;
    c.commb_empty += d.commb_empty;
    c.commb_unknown += d.commb_unknown;
    c.acas_ra += d.acas_ra;
    c.acas_ra_tc28 += d.acas_ra_tc28;
    c.rs_errors += d.rs_errors;
    c.decoder_observations += d.observations;
}

// One input's line in the periodic stats report.
struct InputStatsLine {
    std::string id;
    bool connected = false;
    uint64_t messages = 0, observations = 0, uplinks = 0, rejected = 0;
    // Set for an input running the 1090 decoder; `decoder` is then its counters.
    bool decoding = false;
    InputCounters decoder;
    uint64_t decoder_keys = 0;  // aircraft keys held now (a gauge, not a counter)
};

}  // namespace skycued
