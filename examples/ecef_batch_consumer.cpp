// ecef_batch_consumer: the other way to absorb libskycue. The Client runs in
// its default thread mode and feeds the tracker; the program's own loop pulls
// what changed, once a second, and works on the batch.
//
//   ecef_batch_consumer [--sensor LAT,LON,H_M] tcp HOST:PORT
//   ecef_batch_consumer [--sensor LAT,LON,H_M] listen BIND:PORT
//   ecef_batch_consumer [--sensor LAT,LON,H_M] udp BIND:PORT
//
// Push or pull? ecef_consumer.cpp uses poll mode plus callbacks: every state
// arrives the moment it is computed, on your thread, with no second thread and
// the lowest latency. Pick that when you want to react to each update.
// This program uses thread mode plus takeChanges(): the I/O thread only feeds
// the tracker (it is thread-safe), and your loop, running on its own clock,
// asks "what changed since I last looked" and gets one entry per track.
// Pick that when your loop already exists and wants batches, not a stream of
// events. No tracker callbacks are registered here, and nothing needs a mutex
// of ours: the tracker holds the one that matters.
#include <skycue/client.hpp>
#include <skycue/ecef.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

namespace {

using skycue::TrackId;
using namespace skycue::geo;

std::atomic<bool> g_stop{false};
extern "C" void onSignal(int) { g_stop = true; }

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
const char* errName(ErrorSource s)
{
    switch (s) {
    case ErrorSource::nac_p: return "nacp";
    case ErrorSource::nic: return "nic";
    case ErrorSource::default_: return "default";
    }
    return "?";
}

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

void printSummary(const EcefTracker& tracker)
{
    std::printf("SUMMARY tracks=%zu\n", tracker.size());
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
    if (!ok) {
        std::fprintf(stderr,
                     "usage: %s [--sensor LAT,LON,H_M] tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT\n",
                     argv[0]);
        return 2;
    }
    cfg.host = addr.substr(0, colon);
    cfg.port = static_cast<uint16_t>(port);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    // cfg.run_io_thread stays true: the I/O thread runs the two callbacks below.

    Options opt;
    opt.collect_changes = true; // record changes for takeChanges()
    opt.derive_velocity = true;
    opt.stale_after = std::chrono::seconds(30);
    EcefTracker tracker(opt);

    skycue::Client client(cfg);
    // The only two callbacks, and the only code that runs on the I/O thread.
    // Keep it that way: the tracker is thread-safe, so no lock is needed here.
    client.onTrackUpdate([&](const skycue::Track& t) { tracker.update(t); });
    client.onTrackDropped([&](TrackId id, skycue::DropReason) { tracker.drop(id); });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    client.start();

    while (!g_stop) {
        // Our clock, our cadence: once a second take the batch. The record is
        // cleared by the call, so nothing is reported twice.
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const auto changes = tracker.takeChanges();
        // Apply drops before updates: a track dropped and re-created within
        // one batch is in both lists, and the update is the live one.
        for (const auto& d : changes.dropped)
            std::printf("drop %06x %s\n", d.first.address, dropName(d.second));
        for (const auto& c : changes.updated) {
            const EcefState& s = c.state;
            std::printf("%s %06x pos=%.3f,%.3f,%.3fkm ", c.is_new ? "new" : "upd", s.id.address,
                        s.position.x / 1000.0, s.position.y / 1000.0, s.position.z / 1000.0);
            if (s.velocity)
                std::printf("vel=%s%.1f,%.1f,%.1fm/s ", s.velocity_source == VelocitySource::derived ? "~" : "",
                            s.velocity->x, s.velocity->y, s.velocity->z);
            else
                std::printf("v: - ");
            std::printf("sigma=%.0f,%.0fm alt=%s err=%s", s.horizontal_sigma_m, s.vertical_sigma_m,
                        altName(s.altitude_source), errName(s.error_source));
            if (sensor) {
                // Look angles are as of the state's own time; propagate() first
                // if you need "now" (see ecef_consumer.cpp).
                const auto look = lookFrom(*sensor, s.position);
                std::printf(" az=%.1f el=%.1f range=%.1fkm", look.azimuth_deg, look.elevation_deg,
                            look.range_m / 1000.0);
            }
            std::printf("\n");
        }

        // expire() is also ours to call; stale tracks then show up in the
        // next takeChanges() as drops.
        tracker.expire();
        printSummary(tracker);
    }
    client.stop();
    printSummary(tracker);
    return 0;
}
