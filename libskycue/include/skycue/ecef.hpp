#pragma once

// skycue ECEF state: turns a skycue::Track (latitude, longitude, feet, knots)
// into a WGS-84 Earth-centred, Earth-fixed position and velocity (metres,
// metres per second) with a per-axis 1-sigma error, and computes look angles
// from a sensor site. Where the ideal fields are missing, fixed fallback
// ladders choose a substitute and the result names the rung it used, so a
// consumer can threshold on quality. See docs/libskycue.md.
//
// skycued passes along what it received and does not extrapolate; this
// header's conversion is likewise exact to the track's position_time. Use
// propagate() to move a state forward if you want "now".

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace skycue::geo {

struct Vec3 {
    double x{}, y{}, z{}; // metres, or metres per second for a velocity
};

// Which rung of the height ladder produced the height above the ellipsoid.
enum class AltitudeSource : uint8_t {
    geom_reported,  // alt_geom_ft, no uncertainty attached
    geom_estimated, // alt_geom_ft with alt_geom_uncertainty_ft present
    baro,           // alt_baro_ft used as height above the ellipsoid
    caller_default, // Options::default_height_m (on the ground / no altitude)
};
enum class VelocitySource : uint8_t {
    reported, // from ground_speed_kt and track_deg
    derived,  // from the displacement since the tracker's anchor fix
    none,     // no horizontal velocity; EcefState::velocity is nullopt
};
enum class VerticalSource : uint8_t {
    reported,     // vertical_rate_fpm
    derived,      // height difference since the anchor fix, same altitude kind
    assumed_zero, // neither; the vertical component is zero
};
enum class ErrorSource : uint8_t {
    nac_p,    // NACp (95 % EPU / 2.45)
    nic,      // NIC containment radius / 2.45
    default_, // Options::default_horizontal_sigma_m
};

struct EcefState {
    TrackId id;
    std::chrono::system_clock::time_point time; // Track::position_time
    Vec3 position;                              // ECEF, m
    std::optional<Vec3> velocity;               // ECEF, m/s; nullopt when VelocitySource::none
    double horizontal_sigma_m{};                // 1-sigma, from the error ladder
    double vertical_sigma_m{};                  // 1-sigma, from the altitude ladder
    AltitudeSource altitude_source{};
    VelocitySource velocity_source{};
    VerticalSource vertical_source{};
    ErrorSource error_source{};
};

struct Options {
    std::optional<double> default_height_m; // last altitude rung; nullopt disables it
    bool derive_velocity = false;           // enable the "derived" rungs (needs EcefTracker)
    // Below this, two fixes give no derived course. The derivation anchor is
    // a stored fix that moves only when a derivation succeeds (or is older
    // than max_fix_age), so slow targets still get a derived velocity after a
    // few updates.
    double min_fix_separation_m = 50.0;
    std::chrono::milliseconds max_fix_age{10000}; // older previous fix: no derived rungs
    double baro_sigma_m = 300.0;
    double default_horizontal_sigma_m = 1852.0;
    // EcefTracker only:
    std::chrono::milliseconds stale_after{30000}; // no accepted update for this long: dropped
    bool notify_only_new_position = true; // onState fires / a change is recorded only when position_time advanced
    // Record changes for takeChanges() (pull style). The pending drop list
    // grows without bound if takeChanges() is never called while this is on.
    bool collect_changes = false;
};

// Pure: reported rungs only (and caller_default). nullopt when no altitude rung applies.
std::optional<EcefState> toEcef(const Track&, const Options& = {});

// Why a track left an EcefTracker.
enum class TrackerDrop : uint8_t {
    explicit_, // drop() was called
    stale,     // expire(): no accepted update for stale_after
    cleared,   // clear()
};

// Stateful: keeps the last accepted state per TrackId so the derived rungs
// work, drops tracks that go quiet, and tells the consumer when a state is
// new, either by callback (push) or through takeChanges() (pull).
// Callbacks run on the calling thread; with several calling threads they may
// run concurrently and out of order with each other.
// Thread-safe: every method takes the tracker's mutex, and callbacks are
// invoked after it is released, so the Client may feed it from its I/O thread
// while the program's own loop calls takeChanges()/expire()/get() elsewhere.
class EcefTracker {
public:
    explicit EcefTracker(Options = {});
    ~EcefTracker();
    EcefTracker(const EcefTracker&) = delete;
    EcefTracker& operator=(const EcefTracker&) = delete;

    // Fires after update() produced a state: on every accepted update, or
    // (default) only when position_time moved on from the stored state.
    void onState(std::function<void(const EcefState&)> cb);
    // Fires when a track leaves the tracker: drop(), expire() or clear().
    void onDropped(std::function<void(TrackId, TrackerDrop)> cb);
    std::optional<EcefState> update(const Track&); // feed from Client::onTrackUpdate
    void drop(TrackId);                            // feed from Client::onTrackDropped
    // Drops every track whose last accepted update is older than stale_after,
    // measured on the caller's steady clock; call from a periodic timer.
    // Returns how many were dropped.
    std::size_t expire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Drops every track at once (a sender restart, a mode change in the
    // consumer); notifies onDropped(cleared) for each unless quiet is true.
    std::size_t clear(bool quiet = false);
    std::optional<EcefState> get(TrackId) const;
    std::size_t size() const;

    // Pull style. Requires Options::collect_changes; otherwise returns empty.
    struct Change {
        EcefState state;
        bool is_new; // first state for this id since it was (re)created
    };
    struct Changes {
        std::vector<Change> updated; // one entry per id, its latest state
        std::vector<std::pair<TrackId, TrackerDrop>> dropped;
    };
    // Returns everything that changed since the previous call and clears the
    // record. A track that was created and dropped between two calls appears
    // only in `dropped`. A track dropped and then re-created between two calls
    // appears in both lists: apply `dropped` before `updated`.
    Changes takeChanges();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Geodetic (WGS-84 degrees, height above the ellipsoid in metres) to ECEF metres.
Vec3 geodeticToEcef(double lat_deg, double lon_deg, double h_m) noexcept;
// A north/east/down vector (m or m/s) at a geodetic latitude and longitude, rotated into ECEF.
Vec3 nedToEcef(double lat_deg, double lon_deg, double vn, double ve, double vd) noexcept;
// Position plus velocity times the time elapsed from state.time to `to`
// (negative to go back); a state with no velocity is returned unmoved.
Vec3 propagate(const EcefState&, std::chrono::system_clock::time_point to) noexcept;

struct LookAngles {
    double azimuth_deg{};   // degrees clockwise from true north, [0, 360)
    double elevation_deg{}; // degrees above the sensor's local horizontal
    double range_m{};       // straight-line (slant) distance
};
// Where the target is as seen from the sensor. No refraction correction.
LookAngles lookFrom(const Vec3& sensor_ecef, const Vec3& target_ecef) noexcept;

} // namespace skycue::geo
