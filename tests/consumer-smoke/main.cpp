// Links against libskycue, constructs a Client and exits.
#include <skycue/client.hpp>
#include <skycue/protocol.hpp>

#include <cstdio>

int main()
{
    skycue::Config cfg;
    cfg.run_io_thread = false;
    skycue::Client client(cfg);
    client.onTrackUpdate([](const skycue::Track&) {});
    client.stop();
    const auto frame = skycue::encodeHeartbeat(skycue::Heartbeat{1});
    std::printf("skycue consumer ok: %zu tracks, heartbeat frame %zu bytes\n", client.trackCount(), frame.size());
    return frame.size() == skycue::kHeaderSize + 8 ? 0 : 1;
}
