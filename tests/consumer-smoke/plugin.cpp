// A shared library with libskycue (static) linked into it: it only builds if
// libskycue is position-independent.
#include <skycue/client.hpp>
#include <skycue/protocol.hpp>

#include <cstddef>

extern "C" __attribute__((visibility("default"))) std::size_t consumer_plugin_check()
{
    skycue::Config cfg;
    cfg.run_io_thread = false;
    skycue::Client client(cfg);
    client.onTrackUpdate([](const skycue::Track&) {});
    client.stop();
    const auto frame = skycue::encodeHeartbeat(skycue::Heartbeat{1});
    return client.trackCount() == 0 ? frame.size() : 0;
}
