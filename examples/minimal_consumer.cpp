// minimal_consumer: the smallest useful libskycue program. Prints one line per
// connection change, track update and track drop until SIGINT or SIGTERM.
//
//   minimal_consumer tcp HOST:PORT     connect to skycued (its CUE_TCP_PORT)
//   minimal_consumer listen BIND:PORT  let skycued connect (its CUE_TCP_CONNECT)
//   minimal_consumer udp BIND:PORT     receive datagrams (its CUE_UDP_TARGETS)
#include <skycue/client.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }

const char* reasonName(skycue::DropReason r)
{
    switch (r) {
    case skycue::DropReason::timeout: return "timeout";
    case skycue::DropReason::filtered: return "filtered";
    case skycue::DropReason::source_lost: return "source_lost";
    case skycue::DropReason::shutdown: return "shutdown";
    case skycue::DropReason::stale: return "stale";
    }
    return "other"; // a value added by a newer sender
}

} // namespace

int main(int argc, char** argv)
{
    using Transport = skycue::Config::Transport;
    const std::string mode = argc == 3 ? argv[1] : "";
    const std::string addr = argc == 3 ? argv[2] : "";
    const auto colon = addr.rfind(':');
    char* end = nullptr;
    const unsigned long port =
        colon == std::string::npos ? 0 : std::strtoul(addr.c_str() + colon + 1, &end, 10);

    skycue::Config cfg;
    bool ok = colon != std::string::npos && colon > 0 && end != addr.c_str() + colon + 1 &&
              *end == '\0' && port <= 65535;
    if (mode == "tcp")
        cfg.transport = Transport::tcp;
    else if (mode == "listen")
        cfg.transport = Transport::tcp_listen;
    else if (mode == "udp")
        cfg.transport = Transport::udp;
    else
        ok = false;
    if (!ok) {
        std::fprintf(stderr, "usage: %s tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT\n", argv[0]);
        return 2;
    }
    cfg.host = addr.substr(0, colon);
    cfg.port = static_cast<uint16_t>(port);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Callbacks run on the client's I/O thread (cfg.run_io_thread is true).
    skycue::Client client(cfg);
    client.onConnectionChange([](bool up) { std::printf("CONN %s\n", up ? "up" : "down"); });
    client.onTrackUpdate([](const skycue::Track& t) {
        const std::string alt = t.alt_baro_ft ? std::to_string(*t.alt_baro_ft) + "ft" : "-";
        std::printf("UPDATE %06x %-8s %.5f,%.5f %s%s\n", t.id.address, t.callsign.value_or("-").c_str(),
                    t.lat_deg, t.lon_deg, alt.c_str(), t.keyframe ? " keyframe" : "");
    });
    client.onTrackDropped([](skycue::TrackId id, skycue::DropReason r) {
        std::printf("DROP %06x %s\n", id.address, reasonName(r));
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    client.start();
    if (cfg.transport == Transport::tcp_listen) // 0: not bound yet, retrying
        std::printf("LISTEN port %u\n", static_cast<unsigned>(client.localPort()));
    while (!g_stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.stop(); // no callback runs after this returns
    return 0;
}
