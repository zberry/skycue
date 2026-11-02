# parent-project: consuming libskycue from your own CMake tree

A deliberately small stand-in for a larger system (here `client_demo`) that
vendors the whole `skycue/` directory and builds libskycue with
`add_subdirectory()`. It is **not** part of the skycue build: skycue's own
`examples/CMakeLists.txt` does not add it. It is built by the `parent_project`
test (against a copy of `skycue/` in a temporary directory) and by the
clean-room container check (`containers/verify.sh`), which also runs it
against a replaying skycued.

## What the parent sets

Before `add_subdirectory(${SKYCUE_SOURCE_DIR} skycue)`:

| Variable | Value | Effect |
|---|---|---|
| `SKYCUE_BUILD_DAEMON` | `OFF` | no skycued, skycue-dump/-rawrec/-rawplay/-stats/-fisb or web UI; Boost.ProgramOptions and zlib are not needed |
| `SKYCUE_BUILD_TESTS` | `OFF` | no skycue unit tests; Boost.Test is not needed (default `OFF` when skycue is not the top-level project) |
| `SKYCUE_BUILD_EXAMPLES` | `OFF` | no `minimal_consumer` (default `OFF` when not top-level) |
| `SKYCUE_INSTALL` | `OFF` | your `cmake --install` installs only your binaries, not libskycue's headers, archive or CMake package |

Plain `set()` is enough (skycue's `option()`s honour normal variables). The
parent then links **`skycue::skycue`**, which brings the include path,
`cxx_std_17`, `Boost::system` and `Threads::Threads`. With these settings the
only skycue sources compiled are libskycue's two, and the only build
dependencies are a C++17 compiler, CMake 3.26 or newer (skycue's own
minimum), Boost 1.66 or newer (headers and `system`) and threads.

The parent's own settings (C++17 without extensions, PIC, hidden visibility,
`-Wall -Wextra -Werror` on `client_demo` only, the `CLIENT_DEMO_WERROR`
option) do not leak into skycue, and skycue's warnings do not leak into the
parent's targets.

`SKYCUE_SOURCE_DIR` defaults to `../..` from this directory, i.e. the
`skycue/` that contains it; point it at your vendored copy otherwise.

## Building and running

```sh
cmake -S skycue/examples/parent-project -B build-client \
      -DSKYCUE_SOURCE_DIR=/path/to/vendored/skycue   # optional
cmake --build build-client
build-client/client_demo --seconds 10                # TCP client to 127.0.0.1:30500
build-client/client_demo --listen --port 30600       # skycued: CUE_TCP_CONNECT=host:30600
build-client/client_demo --udp --port 30600          # skycued: CUE_UDP_TARGETS=host:30600
```

It prints `connection up|down`, `track updated <addr> <callsign> <lat>,<lon>`
and `track dropped <addr> <reason>` lines, and exits 0 after `--seconds N`
(or on SIGINT/SIGTERM when `--seconds` is not given).
