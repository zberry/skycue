# Examples

Programs that use libskycue (see [docs/libskycue.md](../docs/libskycue.md)). Read them in this
order. They are built with the project, so they keep compiling. Each prints its usage and
exits with status 2 when run without arguments.

1. `minimal_consumer.cpp`: connect to skycued and print one line per connection change, track
   update and track drop. Run: `minimal_consumer tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT`.
   This file is embedded verbatim in `docs/libskycue.md`, and a ctest
   (`script_test_docs_libskycue_example`) keeps the two identical.
2. `ecef_consumer.cpp`: callbacks in poll mode (no I/O thread), and a table of tracks that is
   printed once a second. With `--sensor` it prints a pointing line for the highest target.
   Run: `ecef_consumer [--sensor LAT,LON,H_M] tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT`.
3. `ecef_batch_consumer.cpp`: thread mode and `takeChanges()`, so your own loop pulls one
   entry per changed track. Run: `ecef_batch_consumer [--sensor LAT,LON,H_M] tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT`.
4. `parent-project/`: a small separate CMake project that builds libskycue with
   `add_subdirectory()`, or uses an installed one through `find_package(skycue)`. Its own
   README explains it; it is not part of this build, and the ctest `parent_project` builds it.
