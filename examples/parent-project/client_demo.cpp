// client_demo: receives skycued's cue stream through libskycue and prints one
// line per event. Built by the parent-project example, not by skycue itself.
//
//   client_demo [--host H] [--port P] [--seconds N]   connect to skycued (CUE_TCP_PORT)
//   client_demo --listen [--host BIND] [--port P] ... let skycued connect (CUE_TCP_CONNECT)
//   client_demo --udp [--host BIND] [--port P] ...    receive datagrams (CUE_UDP_TARGETS)
#include <skycue/client.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }

void usage(std::FILE* out, const char* argv0)
{
    std::fprintf(out,
                 "usage: %s [--listen | --udp] [--host HOST] [--port PORT] [--seconds N]\n"
                 "  (default)  connect over TCP to skycued at HOST:PORT (127.0.0.1:30500)\n"
                 "  --listen   listen on HOST:PORT for skycued to connect (its CUE_TCP_CONNECT)\n"
                 "  --udp      receive cue datagrams on HOST:PORT (its CUE_UDP_TARGETS)\n"
                 "  --seconds  exit 0 after N seconds; 0 (the default) runs until SIGINT/SIGTERM\n"
                 "Exit status 0 does not mean data arrived: connection and transport errors\n"
                 "are only printed (\"error: ...\" on stderr) while the client keeps retrying.\n",
                 argv0);
}

const char* reasonName(skycue::DropReason r)
{
    switch (r) {
    case skycue::DropReason::timeout: return "timeout";
    case skycue::DropReason::filtered: return "filtered";
    case skycue::DropReason::source_lost: return "source_lost";
    case skycue::DropReason::shutdown: return "shutdown";
    case skycue::DropReason::stale: return "stale";
    }
    return "other";
}

bool parseUnsigned(const char* s, unsigned long max, unsigned long& out)
{
    char* end = nullptr;
    out = std::strtoul(s, &end, 10);
    return *s != '\0' && *s != '-' && *end == '\0' && out <= max;
}

} // namespace

int main(int argc, char** argv)
{
    using Transport = skycue::Config::Transport;
    skycue::Config cfg;
    unsigned long seconds = 0;
    bool listen = false;
    bool udp = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasValue = i + 1 < argc;
        unsigned long v = 0;
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (std::strcmp(a, "--listen") == 0) {
            listen = true;
        } else if (std::strcmp(a, "--udp") == 0) {
            udp = true;
        } else if (std::strcmp(a, "--host") == 0 && hasValue) {
            cfg.host = argv[++i];
        } else if (std::strcmp(a, "--port") == 0 && hasValue && parseUnsigned(argv[i + 1], 65535, v)) {
            cfg.port = static_cast<uint16_t>(v);
            ++i;
        } else if (std::strcmp(a, "--seconds") == 0 && hasValue && parseUnsigned(argv[i + 1], 86400, v)) {
            seconds = v;
            ++i;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (listen && udp) {
        usage(stderr, argv[0]);
        return 2;
    }
    cfg.transport = listen ? Transport::tcp_listen : udp ? Transport::udp : Transport::tcp;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Callbacks run on the client's I/O thread.
    std::atomic<unsigned long> updates{0};
    std::atomic<unsigned long> drops{0};
    skycue::Client client(cfg);
    client.onConnectionChange([](bool up) { std::printf("connection %s\n", up ? "up" : "down"); });
    client.onError([](const std::string& message) { std::fprintf(stderr, "error: %s\n", message.c_str()); });
    client.onTrackUpdate([&updates](const skycue::Track& t) {
        ++updates;
        std::printf("track updated %06x %s %.5f,%.5f\n", static_cast<unsigned>(t.id.address),
                    t.callsign.value_or("-").c_str(), t.lat_deg, t.lon_deg);
    });
    client.onTrackDropped([&drops](skycue::TrackId id, skycue::DropReason r) {
        ++drops;
        std::printf("track dropped %06x %s\n", static_cast<unsigned>(id.address), reasonName(r));
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    client.start();
    if (cfg.transport == Transport::tcp_listen) {
        const uint16_t port = client.localPort(); // 0: not bound yet, retrying
        if (port != 0)
            std::printf("listening on port %u\n", static_cast<unsigned>(port));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop && (seconds == 0 || std::chrono::steady_clock::now() < deadline))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.stop(); // no callback runs after this returns
    std::printf("client_demo: %lu updates, %lu drops\n", updates.load(), drops.load());
    return 0;
}
