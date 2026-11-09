#pragma once

// Per-key state of the 1090 decoder. A key is (address, address type, class):
// a DF17 even frame never pairs with a DF18 TIS-B odd one (design section 3).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>

#include <skycue/types.hpp>

#include "decode/cpr.hpp"
#include "decode/decoder_types.hpp"
#include "observation.hpp"

namespace skycued::decode {

struct StateKey {
    uint32_t address = 0;
    skycue::AddressType type = skycue::AddressType::icao;
    bool rebroadcast = false;  // class: direct (false) or rebroadcast (TIS-B, ADS-R)
    bool operator==(const StateKey& o) const {
        return address == o.address && type == o.type && rebroadcast == o.rebroadcast;
    }
};

struct StateKeyHash {
    std::size_t operator()(const StateKey& k) const noexcept {
        return std::hash<uint64_t>()((static_cast<uint64_t>(k.address) << 4) |
                                     (static_cast<uint64_t>(k.type) << 1) | (k.rebroadcast ? 1u : 0u));
    }
};

// A value and the (max-so-far) time it was learned.
template <typename T>
struct Timed {
    T value{};
    TimePoint at{};
    bool has = false;
    void set(const T& v, TimePoint t) {
        value = v;
        at = t;
        has = true;
    }
    // The value when it is no older than max_age at now.
    std::optional<T> fresh(TimePoint now, std::chrono::seconds max_age) const {
        if (has && now - at <= max_age) return value;
        return std::nullopt;
    }
};

struct CprHalf {
    cpr::Msg msg{};
    TimePoint at{};
    bool has = false;
};

struct KeyState {
    StateKey key;
    skycue::Source source = skycue::Source::adsb_1090;
    TimePoint last_seen{};
    std::list<StateKey>::iterator lru;

    // Address confirmation (design section 5.2): set by clean DF17, DF18 CF 0
    // and DF11 with residual 0; only the (icao, direct) key ever has it.
    Timed<bool> confirmed;

    // CPR: [kind][odd] with kind 0 airborne, 1 surface.
    std::array<std::array<CprHalf, 2>, 2> halves{};
    int cpr_kind = -1;  // kind of the last position message, -1 none yet
    std::optional<cpr::Position> last_pos;
    TimePoint last_pos_at{};
    std::optional<cpr::Position> first_pos;
    bool need_global = false;                 // set by a rejection, cleared by a global decode
    std::optional<float> last_gs_kt;

    // Fields, each with the time it was learned (observations use <= 60 s).
    Timed<std::string> callsign, category, squawk, emergency;
    Timed<int32_t> alt_baro, alt_gnss, alt_derived;
    Timed<int32_t> gnss_diff;  // last TC19 GNSS-minus-baro difference (signed, positive: GNSS above baro)
    Timed<float> gs, track, baro_rate, geom_rate;
    Timed<uint8_t> nic, nac_p;
    Timed<bool> on_ground;
    // Comm-B, target-state and ACAS fields (see CommBFields).
    Timed<float> heading_mag, heading_true, tas, mach, roll, track_rate, qnh, sel_heading;
    Timed<uint16_t> ias;
    Timed<int32_t> sel_alt, sel_alt_fms;
    Timed<uint8_t> nav_modes, acas_sl, acas_ri;
    Timed<AcasRaState> acas_ra;
    // Track, ground speed and the two vertical rates are set by ADS-B velocity messages and by
    // Comm-B; the flag says the stored value came from Comm-B, which never replaces a fresh ADS-B one.
    bool track_commb = false, gs_commb = false, baro_rate_commb = false, geom_rate_commb = false;
    // Intent fields whose current value came from BDS 4,0 (a TC29 value under 60 s old outranks it).
    bool sel_alt_commb = false, sel_alt_fms_commb = false, qnh_commb = false, nav_modes_commb = false;

    // TC31 (kept for the key's life, applied to later position messages).
    struct Tc31 {
        int version = 0;
        bool nic_a = false;
        bool nic_c = false;
        bool seen = false;
    } tc31;

    RssiRing rssi;  // level of the last frames that carried a signal byte

    // Change detection (design section 7.2).
    EmitGate gate;
};

}  // namespace skycued::decode
