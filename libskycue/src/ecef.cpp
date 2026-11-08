// ECEF geodesy core: WGS-84 geodetic and NED conversions, look angles and
// propagation. See include/skycue/ecef.hpp.

#include <skycue/ecef.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace skycue::geo {

namespace {

constexpr double kA = 6378137.0;                    // WGS-84 semi-major axis, m
constexpr double kF = 1.0 / 298.257223563;          // WGS-84 flattening
constexpr double kE2 = kF * (2.0 - kF);             // first eccentricity squared
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Basis {
    Vec3 n, e, d; // unit vectors north, east, down in ECEF
};

Basis basisAt(double lat_rad, double lon_rad)
{
    const double sp = std::sin(lat_rad), cp = std::cos(lat_rad);
    const double sl = std::sin(lon_rad), cl = std::cos(lon_rad);
    return {{-sp * cl, -sp * sl, cp}, {-sl, cl, 0.0}, {-cp * cl, -cp * sl, -sp}};
}

constexpr double kKt = 0.514444;  // knots to m/s
constexpr double kFpm = 0.00508;   // feet per minute to m/s
constexpr double kFt = 0.3048;     // feet to m
constexpr double kSigmaDiv = 2.45; // 95 % radius to 1-sigma

// The previous accepted fix of a track, as the derived rungs need it.
struct Prev {
    std::chrono::system_clock::time_point time;
    double lat_deg, lon_deg, h_m;
    AltitudeSource alt_source;
};

// 1 geometric, 2 barometric, 0 caller default (never compared for a derived rate).
int altGroup(AltitudeSource s)
{
    switch (s) {
    case AltitudeSource::geom_reported:
    case AltitudeSource::geom_estimated: return 1;
    case AltitudeSource::baro: return 2;
    default: return 0;
    }
}

// 95 % radius tables indexed by NACp / NIC; 0 means unknown.
constexpr double kNacpEpu[12] = {0, 18520, 7408, 3704, 1852, 926, 555.6, 185.2, 92.6, 30, 10, 3};
constexpr double kNicRc[12] = {0, 37040, 14816, 7408, 3704, 1852, 1111, 370.4, 185.2, 75, 25, 7.5};

struct Computed {
    EcefState state;
    double h_m{};
};

std::optional<Computed> compute(const Track& t, const Options& o, const Prev* prev)
{
    EcefState s;
    s.id = t.id;
    s.time = t.position_time;

    // Height ladder.
    double h;
    if (t.alt_geom_ft && !t.alt_geom_uncertainty_ft) {
        h = *t.alt_geom_ft * kFt;
        s.altitude_source = AltitudeSource::geom_reported;
        s.vertical_sigma_m = 15.0;
    } else if (t.alt_geom_ft) {
        h = *t.alt_geom_ft * kFt;
        s.altitude_source = AltitudeSource::geom_estimated;
        s.vertical_sigma_m = *t.alt_geom_uncertainty_ft * kFt;
    } else if (t.alt_baro_ft) {
        h = *t.alt_baro_ft * kFt;
        s.altitude_source = AltitudeSource::baro;
        s.vertical_sigma_m = o.baro_sigma_m;
    } else if (o.default_height_m) {
        h = *o.default_height_m;
        s.altitude_source = AltitudeSource::caller_default;
        s.vertical_sigma_m = 30.0;
    } else {
        return std::nullopt;
    }
    s.position = geodeticToEcef(t.lat_deg, t.lon_deg, h);

    // Horizontal velocity ladder (north, east in m/s).
    double vn = 0, ve = 0;
    const double dt = prev ? std::chrono::duration<double>(t.position_time - prev->time).count() : 0.0;
    const bool prevUsable =
        prev && o.derive_velocity && dt > 0.0 && dt <= std::chrono::duration<double>(o.max_fix_age).count();
    if (t.ground_speed_kt && t.track_deg) {
        const double gs = *t.ground_speed_kt * kKt, tr = *t.track_deg * kDeg;
        vn = gs * std::cos(tr);
        ve = gs * std::sin(tr);
        s.velocity_source = VelocitySource::reported;
    } else {
        s.velocity_source = VelocitySource::none;
        if (prevUsable) {
            // Displacement in the local north/east frame at the midpoint
            // (ellipsoid-aware equivalent of great-circle bearing/distance).
            double dlon = t.lon_deg - prev->lon_deg;
            while (dlon > 180.0) dlon -= 360.0;
            while (dlon < -180.0) dlon += 360.0;
            const double mlat = 0.5 * (t.lat_deg + prev->lat_deg), mlon = prev->lon_deg + 0.5 * dlon;
            const Vec3 a = geodeticToEcef(prev->lat_deg, prev->lon_deg, 0), b = geodeticToEcef(t.lat_deg, t.lon_deg, 0);
            const Vec3 d{b.x - a.x, b.y - a.y, b.z - a.z};
            const Vec3 n = nedToEcef(mlat, mlon, 1, 0, 0), e = nedToEcef(mlat, mlon, 0, 1, 0);
            const double dn = d.x * n.x + d.y * n.y + d.z * n.z, de = d.x * e.x + d.y * e.y + d.z * e.z;
            if (std::hypot(dn, de) >= o.min_fix_separation_m) {
                vn = dn / dt;
                ve = de / dt;
                s.velocity_source = VelocitySource::derived;
            }
        }
    }

    // Vertical ladder (up positive), only with a horizontal velocity.
    if (s.velocity_source == VelocitySource::none) {
        s.vertical_source = VerticalSource::assumed_zero;
    } else {
        double vu = 0;
        if (t.vertical_rate_fpm) {
            vu = *t.vertical_rate_fpm * kFpm;
            s.vertical_source = VerticalSource::reported;
        } else if (prevUsable && altGroup(prev->alt_source) == altGroup(s.altitude_source) &&
                   altGroup(s.altitude_source) != 0) {
            vu = (h - prev->h_m) / dt;
            s.vertical_source = VerticalSource::derived;
        } else {
            s.vertical_source = VerticalSource::assumed_zero;
        }
        s.velocity = nedToEcef(t.lat_deg, t.lon_deg, vn, ve, -vu);
    }

    // Horizontal error ladder.
    if (t.nac_p && *t.nac_p >= 1 && *t.nac_p <= 11) {
        s.horizontal_sigma_m = kNacpEpu[*t.nac_p] / kSigmaDiv;
        s.error_source = ErrorSource::nac_p;
    } else if (t.nic && *t.nic >= 1 && *t.nic <= 11) {
        s.horizontal_sigma_m = kNicRc[*t.nic] / kSigmaDiv;
        s.error_source = ErrorSource::nic;
    } else {
        s.horizontal_sigma_m = o.default_horizontal_sigma_m;
        s.error_source = ErrorSource::default_;
    }
    return Computed{s, h};
}

} // namespace

Vec3 geodeticToEcef(double lat_deg, double lon_deg, double h_m)
{
    const double lat = lat_deg * kDeg, lon = lon_deg * kDeg;
    const double sp = std::sin(lat), cp = std::cos(lat);
    const double n = kA / std::sqrt(1.0 - kE2 * sp * sp);
    return {(n + h_m) * cp * std::cos(lon), (n + h_m) * cp * std::sin(lon), (n * (1.0 - kE2) + h_m) * sp};
}

Vec3 nedToEcef(double lat_deg, double lon_deg, double vn, double ve, double vd)
{
    const Basis b = basisAt(lat_deg * kDeg, lon_deg * kDeg);
    return {vn * b.n.x + ve * b.e.x + vd * b.d.x, vn * b.n.y + ve * b.e.y + vd * b.d.y,
            vn * b.n.z + ve * b.e.z + vd * b.d.z};
}

LookAngles lookFrom(const Vec3& sensor, const Vec3& target)
{
    // Geodetic latitude and longitude of the sensor, only to orient the ENU
    // axes (height is not needed). Iterate lat = atan2(z + e2 N sin(lat), p);
    // it converges to well under a nanoradian in a few steps.
    const double p = std::hypot(sensor.x, sensor.y);
    const double lon = std::atan2(sensor.y, sensor.x);
    double lat = std::atan2(sensor.z, p * (1.0 - kE2));
    for (int i = 0; i < 8; ++i) {
        const double sp = std::sin(lat);
        const double n = kA / std::sqrt(1.0 - kE2 * sp * sp);
        lat = std::atan2(sensor.z + kE2 * n * sp, p);
    }
    const Basis b = basisAt(lat, lon);
    const double dx = target.x - sensor.x, dy = target.y - sensor.y, dz = target.z - sensor.z;
    const double north = dx * b.n.x + dy * b.n.y + dz * b.n.z;
    const double east = dx * b.e.x + dy * b.e.y + dz * b.e.z;
    const double up = -(dx * b.d.x + dy * b.d.y + dz * b.d.z);
    const double horiz = std::hypot(east, north);

    LookAngles out;
    out.range_m = std::sqrt(horiz * horiz + up * up);
    out.azimuth_deg = std::atan2(east, north) / kDeg;
    if (out.azimuth_deg < 0.0)
        out.azimuth_deg += 360.0;
    if (out.azimuth_deg >= 360.0) // a tiny negative angle rounds up to 360
        out.azimuth_deg = 0.0;
    out.elevation_deg = std::atan2(up, horiz) / kDeg;
    return out;
}

Vec3 propagate(const EcefState& s, std::chrono::system_clock::time_point to)
{
    if (!s.velocity)
        return s.position;
    const double dt = std::chrono::duration<double>(to - s.time).count();
    return {s.position.x + s.velocity->x * dt, s.position.y + s.velocity->y * dt,
            s.position.z + s.velocity->z * dt};
}

} // namespace skycue::geo

namespace skycue::geo {

std::optional<EcefState> toEcef(const Track& t, const Options& o)
{
    auto c = compute(t, o, nullptr);
    if (!c)
        return std::nullopt;
    return c->state;
}

// Locking design: one mutex guards all of Impl. Callbacks are copied out under
// the lock and invoked after it is released, so a callback may call back into
// the tracker (get, drop, clear, expire, update).
struct EcefTracker::Impl {
    struct Entry {
        EcefState state;
        double lat_deg, lon_deg, h_m;
        std::chrono::steady_clock::time_point arrival;
    };

    explicit Impl(Options opts) : o(opts) {}

    mutable std::mutex mu;
    Options o;
    std::function<void(const EcefState&)> on_state;
    std::function<void(TrackId, TrackerDrop)> on_dropped;
    std::unordered_map<TrackId, Entry> tracks;
    std::unordered_map<TrackId, Change> pending_updated;
    std::vector<std::pair<TrackId, TrackerDrop>> pending_dropped;

    // Erase `ids` (caller holds the lock) and record them for pull style.
    void eraseLocked(const std::vector<TrackId>& ids, TrackerDrop why, bool record)
    {
        for (const TrackId& id : ids) {
            tracks.erase(id);
            pending_updated.erase(id);
            if (record && o.collect_changes)
                pending_dropped.emplace_back(id, why);
        }
    }

    void notifyDropped(const std::vector<TrackId>& ids, TrackerDrop why, const std::function<void(TrackId, TrackerDrop)>& cb)
    {
        if (!cb)
            return;
        for (const TrackId& id : ids)
            cb(id, why);
    }
};

EcefTracker::EcefTracker(Options o) : impl_(std::make_unique<Impl>(o)) {}
EcefTracker::~EcefTracker() = default;

void EcefTracker::onState(std::function<void(const EcefState&)> cb)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_state = std::move(cb);
}

void EcefTracker::onDropped(std::function<void(TrackId, TrackerDrop)> cb)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_dropped = std::move(cb);
}

std::optional<EcefState> EcefTracker::update(const Track& t)
{
    std::function<void(const EcefState&)> cb;
    EcefState out;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->tracks.find(t.id);
        std::optional<Prev> prev;
        if (it != impl_->tracks.end()) {
            const auto& e = it->second;
            prev = Prev{e.state.time, e.lat_deg, e.lon_deg, e.h_m, e.state.altitude_source};
        }
        auto c = compute(t, impl_->o, prev ? &*prev : nullptr);
        if (!c)
            return std::nullopt;
        out = c->state;
        const bool existed = it != impl_->tracks.end();
        const bool advanced = !existed || out.time > it->second.state.time;
        Impl::Entry e{out, t.lat_deg, t.lon_deg, c->h_m, std::chrono::steady_clock::now()};
        if (existed)
            it->second = e;
        else
            impl_->tracks.emplace(t.id, e);
        const bool notify = advanced || !impl_->o.notify_only_new_position;
        if (notify) {
            if (impl_->o.collect_changes) {
                auto p = impl_->pending_updated.find(t.id);
                if (p == impl_->pending_updated.end())
                    impl_->pending_updated.emplace(t.id, Change{out, !existed});
                else
                    p->second.state = out; // is_new keeps the value from the run's start
            }
            cb = impl_->on_state;
        }
    }
    if (cb)
        cb(out);
    return out;
}

void EcefTracker::drop(TrackId id)
{
    std::function<void(TrackId, TrackerDrop)> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->tracks.count(id))
            return;
        impl_->eraseLocked({id}, TrackerDrop::explicit_, true);
        cb = impl_->on_dropped;
    }
    if (cb)
        cb(id, TrackerDrop::explicit_);
}

std::size_t EcefTracker::expire(std::chrono::steady_clock::time_point now)
{
    std::vector<TrackId> ids;
    std::function<void(TrackId, TrackerDrop)> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (const auto& [id, e] : impl_->tracks)
            if (now - e.arrival > impl_->o.stale_after)
                ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        impl_->eraseLocked(ids, TrackerDrop::stale, true);
        cb = impl_->on_dropped;
    }
    impl_->notifyDropped(ids, TrackerDrop::stale, cb);
    return ids.size();
}

std::size_t EcefTracker::clear(bool quiet)
{
    std::vector<TrackId> ids;
    std::function<void(TrackId, TrackerDrop)> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (const auto& kv : impl_->tracks)
            ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        // Quiet: no callbacks and nothing added to the pull-style drop list.
        impl_->eraseLocked(ids, TrackerDrop::cleared, !quiet);
        impl_->pending_updated.clear();
        if (!quiet)
            cb = impl_->on_dropped;
    }
    impl_->notifyDropped(ids, TrackerDrop::cleared, cb);
    return ids.size();
}

std::optional<EcefState> EcefTracker::get(TrackId id) const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->tracks.find(id);
    if (it == impl_->tracks.end())
        return std::nullopt;
    return it->second.state;
}

std::size_t EcefTracker::size() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->tracks.size();
}

EcefTracker::Changes EcefTracker::takeChanges()
{
    Changes out;
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& kv : impl_->pending_updated)
        out.updated.push_back(kv.second);
    std::sort(out.updated.begin(), out.updated.end(),
              [](const Change& a, const Change& b) { return a.state.id < b.state.id; });
    impl_->pending_updated.clear();
    out.dropped.swap(impl_->pending_dropped);
    return out;
}

} // namespace skycue::geo
