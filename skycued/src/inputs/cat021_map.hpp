#pragma once

// A decoded ASTERIX CAT021 record -> TrackObservation, per
// docs/asterix-cat021-reference.md §5/§5.1 with the fixed choices of the
// receivers spec §5.2. Pure; never throws.

#include "asterix/cat021.hpp"
#include "inputs/input_types.hpp"
#include "observation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace skycued {

struct Cat021Context {
    std::chrono::system_clock::time_point arrival;  // daemon clock at arrival; dates the time items
    TimeMode time_mode = TimeMode::utc;
    std::optional<uint8_t> sac, sic;  // expected 010; compared only when both are set
};

// Added to by mapCat021Record. records_skipped: SIM, TST, RAB or no 080.
// nogo_records: NOGO set (still mapped). sac_sic_mismatch: 010 differs from
// the context (still mapped). clock_rejects / clock_offset_s: the record's
// time item checked against arrival (checkDeviceTime).
struct Cat021Counts {
    uint64_t records_skipped = 0, nogo_records = 0, sac_sic_mismatch = 0, clock_rejects = 0;
    std::optional<double> clock_offset_s;
};

// nullopt for a skipped record. The position (131, else 130) is dropped when
// 040 marks it suspect (CL=1, CPR, RCF, IPC, LLC, LDPJ) or it is out of range;
// position_time comes with a position only: 071, else 073 (+074), else 077,
// else 075 (+076), else arrival, dated with timeOfDay (a time of day outside
// the day counts as absent) and clamped. on_ground
// only when 040 ext 1 is present. 132 never sets rssi.
std::optional<TrackObservation> mapCat021Record(const asterix::Cat021Record& r, const Cat021Context& ctx,
                                                Cat021Counts& counts);

// I021/020 emitter category -> readsb's "A1".."C5" (reference §5); 0 and
// values with no readsb equivalent -> nullopt.
std::optional<std::string> category020(uint8_t ecat);

}  // namespace skycued
