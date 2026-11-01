// Calls into the shared library that has libskycue linked into it. It does
// not use libskycue itself (the library links it privately).
#include <cstddef>
#include <cstdio>

extern "C" std::size_t consumer_plugin_check();

int main()
{
    const std::size_t n = consumer_plugin_check();
    std::printf("skycue shared consumer ok: heartbeat frame %zu bytes\n", n);
    return n == 16 ? 0 : 1; // 8-byte header + 8-byte Heartbeat payload
}
