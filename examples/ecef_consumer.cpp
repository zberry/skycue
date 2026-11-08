// ecef_consumer: how an existing program absorbs libskycue when its own loop
// owns the work. The library contributes callbacks and nothing else.
//
//   ecef_consumer [--sensor LAT,LON,H_M] tcp HOST:PORT     connect to skycued
//   ecef_consumer [--sensor LAT,LON,H_M] listen BIND:PORT  let skycued connect
//   ecef_consumer [--sensor LAT,LON,H_M] udp BIND:PORT     receive datagrams
//
// With --sensor (latitude and longitude in degrees, height above the WGS-84
// ellipsoid in metres) it prints, once a second, a pointing line for the
// target: the track with the highest elevation above the sensor's horizon,
// moved forward to the present. Without it, it prints the states that changed
// since the last print. Either way it ends each second with a summary line.
//
// Why poll mode: Config::run_io_thread = false means no thread is created and
// every callback runs inside client.poll(), on main's thread. So the tables
// below need no mutex and no queue. In thread mode (the default) the callbacks
// run on the client's I/O thread: you would hand the state over through a
// mutex-protected table or a queue and do the work on your own thread. See
// ecef_batch_consumer.cpp for that style.
#include <skycue/client.hpp>
#include <skycue/ecef.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

using skycue::TrackId;
using namespace skycue::geo;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }

const char* dropName(TrackerDrop r)
{
    switch (r) {
    case TrackerDrop::explicit_: return "dropped";
    case TrackerDrop::stale: return "stale";
    case TrackerDrop::cleared: return "cleared";
    }
    return "other";
}
const char* altName(AltitudeSource s)
{
    switch (s) {
    case AltitudeSource::geom_reported: return "geom";
    case AltitudeSource::geom_estimated: return "geom_est";
    case AltitudeSource::baro: return "baro";
    case AltitudeSource::caller_default: return "default";
    }
    return "?";
}
const char* velName(VelocitySource s)
{
    switch (s) {
    case VelocitySource::reported: return "reported";
    case VelocitySource::derived: return "derived";
    case VelocitySource::none: return "none";
    }
    return "?";
}
const char* vertName(VerticalSource s)
{
    switch (s) {
    case VerticalSource::reported: return "reported";
    case VerticalSource::derived: return "derived";
    case VerticalSource::assumed_zero: return "zero";
    }
    return "?";
}
const char* errName(ErrorSource s)
{
    switch (s) {
    case ErrorSource::nac_p: return "nacp";
    case ErrorSource::nic: return "nic";
    case ErrorSource::default_: return "default";
    }
    return "?";
}

// A "~" in front of a value marks it as derived or assumed, not reported.
const char* mark(bool estimated) { return estimated ? "~" : ""; }

bool parseSensor(const std::string& s, double& lat, double& lon, double& h)
{
    char* end = nullptr;
    lat = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != ',') return false;
    const char* p = end + 1;
    lon = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    h = std::strtod(p, &end);
    return end != p && *end == '\0' && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

// Counts the tracks held, split by the rung each ladder used. This is the
// "is the data good enough for my job" view; thresholding on a source is how a
// consumer would decide whether to trust a track.
void printSummary(const std::unordered_map<TrackId, EcefState>& states)
{
    int vel[3] = {}, alt[4] = {};
    for (const auto& kv : states) {
        ++vel[static_cast<int>(kv.second.velocity_source)];
        ++alt[static_cast<int>(kv.second.altitude_source)];
    }
    std::printf("SUMMARY tracks=%zu vel(reported=%d derived=%d none=%d) "
                "alt(geom=%d geom_est=%d baro=%d default=%d)\n",
                states.size(), vel[0], vel[1], vel[2], alt[0], alt[1], alt[2], alt[3]);
}

} // namespace

int main(int argc, char** argv)
{
    using Transport = skycue::Config::Transport;
    std::optional<Vec3> sensor;
    std::string mode, addr;
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        double lat = 0, lon = 0, h = 0;
        if (a == "--sensor" && i + 1 < argc && parseSensor(argv[++i], lat, lon, h))
            sensor = geodeticToEcef(lat, lon, h);
        else if (a.rfind("--", 0) == 0)
            ok = false;
        else if (mode.empty())
            mode = a;
        else if (addr.empty())
            addr = a;
        else
            ok = false;
    }
    const auto colon = addr.rfind(':');
    char* end = nullptr;
    const unsigned long port =
        colon == std::string::npos ? 0 : std::strtoul(addr.c_str() + colon + 1, &end, 10);
    ok = ok && colon != std::string::npos && colon > 0 && end != addr.c_str() + colon + 1 &&
         *end == '\0' && port <= 65535;

    skycue::Config cfg;
    if (mode == "tcp")
        cfg.transport = Transport::tcp;
    else if (mode == "listen")
        cfg.transport = Transport::tcp_listen;
    else if (mode == "udp")
        cfg.transport = Transport::udp;
    else
        ok = false;
    if (!ok) { // before anything is opened
        std::fprintf(stderr,
                     "usage: %s [--sensor LAT,LON,H_M] tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT\n",
                     argv[0]);
        return 2;
    }
    cfg.host = addr.substr(0, colon);
    cfg.port = static_cast<uint16_t>(port);
    cfg.run_io_thread = false; // poll mode: our loop is the only thread
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // The program's own table. The tracker computes states; what the program
    // keeps, and for how long, is the program's business.
    std::unordered_map<TrackId, EcefState> states;
    std::unordered_set<TrackId> changed; // for the no-sensor print

    Options opt;
    opt.derive_velocity = true; // fill in a velocity from successive fixes when none is reported
    EcefTracker tracker(opt);

    // Tracker callbacks stay thin: store and mark, nothing more. They run
    // inside client.poll() because the Client feeds the tracker from there.
    tracker.onState([&](const EcefState& s) {
        states[s.id] = s;
        changed.insert(s.id);
    });
    tracker.onDropped([&](TrackId id, TrackerDrop why) {
        states.erase(id);
        changed.erase(id);
        std::printf("GONE %06x %s\n", id.address, dropName(why));
    });

    skycue::Client client(cfg);
    // Feed callbacks are one line each for the same reason: the tracker owns
    // the conversion, and the loop below owns the decisions.
    client.onTrackUpdate([&](const skycue::Track& t) { tracker.update(t); });
    client.onTrackDropped([&](TrackId id, skycue::DropReason) { tracker.drop(id); });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    client.start();

    // 10 Hz tick: poll() delivers whatever arrived (and keeps the connection's
    // liveness timers running, so it must be called often), then the program
    // does its job, then it sleeps the rest of the 100 ms. Change the rate to
    // match your application; once a second is for the human reading this.
    const auto tick = std::chrono::milliseconds(100);
    auto next = Clock::now() + tick;
    int ticks = 0;
    while (!g_stop) {
        client.poll();

        if (++ticks % 10 == 0) {
            // expire() lives in the loop because only the loop knows how often
            // it wants stale tracks removed; the tracker has no timer of its own.
            tracker.expire();

            const auto now = std::chrono::system_clock::now();
            if (sensor) {
                // The target rule is the application's: here, highest elevation.
                // States are exact at their position_time, so each is moved to
                // "now" with propagate() first. The library never extrapolates
                // unless you ask; replace this block with your own rule.
                const EcefState* best = nullptr;
                LookAngles bestLook{};
                for (const auto& kv : states) {
                    const auto look = lookFrom(*sensor, propagate(kv.second, now));
                    if (!best || look.elevation_deg > bestLook.elevation_deg) {
                        best = &kv.second;
                        bestLook = look;
                    }
                }
                if (best) {
                    const double age = std::chrono::duration<double>(now - best->time).count();
                    std::printf("POINT %06x az=%.1f el=%.1f range=%.1fkm age=%.1fs alt=%s%s vel=%s%s err=%s\n",
                                best->id.address, bestLook.azimuth_deg, bestLook.elevation_deg,
                                bestLook.range_m / 1000.0, age,
                                mark(best->altitude_source == AltitudeSource::baro ||
                                     best->altitude_source == AltitudeSource::caller_default),
                                altName(best->altitude_source),
                                mark(best->velocity_source == VelocitySource::derived),
                                velName(best->velocity_source), errName(best->error_source));
                }
            } else {
                // No sensor: report what changed since the last print, so the
                // output follows the data instead of repeating the whole table.
                for (const TrackId& id : changed) {
                    const EcefState& s = states[id];
                    std::printf("STATE %06x pos=%.3f,%.3f,%.3fkm ", id.address, s.position.x / 1000.0,
                                s.position.y / 1000.0, s.position.z / 1000.0);
                    if (s.velocity)
                        std::printf("vel=%s%.1f,%.1f,%.1fm/s ", mark(s.velocity_source == VelocitySource::derived),
                                    s.velocity->x, s.velocity->y, s.velocity->z);
                    else
                        std::printf("v: - ");
                    std::printf("sigma=%.0f,%.0fm alt=%s vert=%s err=%s\n", s.horizontal_sigma_m,
                                s.vertical_sigma_m, altName(s.altitude_source), vertName(s.vertical_source),
                                errName(s.error_source));
                }
                changed.clear();
            }
            printSummary(states);
        }

        std::this_thread::sleep_until(next);
        next += tick;
    }
    client.stop(); // no callback runs after this returns
    printSummary(states);
    return 0;
}
