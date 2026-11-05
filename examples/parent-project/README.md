# parent-project: consuming libskycue from your own CMake tree

A deliberately small stand-in for a larger system (here `client_demo`) that
vendors the whole `skycue/` directory and builds libskycue with
`add_subdirectory()`, or, with `-DCLIENT_DEMO_FIND_PACKAGE=ON`, uses an
installed libskycue through `find_package(skycue)` (see "Using an installed
libskycue" below). It is **not** part of the skycue build: skycue's own
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
dependencies are a C++17 compiler, CMake 3.25 or newer (skycue's own
minimum), Boost 1.66 or newer (headers and `system`) and threads.

The parent's own settings: CMake 3.25 minimum (skycue's own, needed to
configure `skycue/` anyway), C++17 without extensions for its directory
(skycue's directory sets its own standard), and on `client_demo` only, as
target properties and options: PIC, hidden visibility, `-Wall -Wextra`, and
`-Werror` under the `CLIENT_DEMO_WERROR` option. None of the target-level
settings reach skycue's targets, and skycue's warning flags (private to its
targets) do not reach the parent's.

`SKYCUE_SOURCE_DIR` defaults to `../..` from this directory, i.e. the
`skycue/` that contains it; point it at your vendored copy otherwise.

## Using an installed libskycue

`CLIENT_DEMO_FIND_PACKAGE` (default `OFF`): when `ON`, the parent calls
`find_package(skycue CONFIG REQUIRED)` instead of `add_subdirectory()`, and
`SKYCUE_SOURCE_DIR` is not used or checked. The install can be a libskycue
development package or a `cmake --install` of `skycue/` (configure it with
`-DSKYCUE_BUILD_DAEMON=OFF` for libskycue alone). The package config is under
the install's library directory, `lib`, `lib64` or `lib/<multiarch>`
(`CMAKE_INSTALL_LIBDIR`), as `<libdir>/cmake/skycue/skycueConfig.cmake`; put
the install prefix in `CMAKE_PREFIX_PATH` unless it is a system prefix such
as `/usr`. The target is the same `skycue::skycue`, bringing the include
path, `cxx_std_17`, `Boost::system` and `Threads::Threads`, so `client_demo`
needs no other change. libskycue is found through its CMake package only; no
pkg-config file is installed.

```sh
cmake -S skycue -B build-lib -DSKYCUE_BUILD_DAEMON=OFF
cmake --build build-lib && cmake --install build-lib --prefix /path/to/prefix
cmake -S skycue/examples/parent-project -B build-client \
      -DCLIENT_DEMO_FIND_PACKAGE=ON -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build-client
```

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
and `track dropped <addr> <reason>` lines, and exits 0 after `--seconds N`;
`--seconds 0` (the default) runs until SIGINT/SIGTERM. Exit status 0 does
not mean any data arrived: connection and transport errors (refused
connection, port in use, ...) are only printed, as `error: ...` on stderr,
while the client keeps retrying. Check for `track updated` lines if you need
to know that data flowed.
