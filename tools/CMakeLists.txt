# Command-line tools. Each is one source file here, except skycue-iqgen,
# which has its own directory (iqgen/) because its coding and sample layers
# are a static library the tests link.
find_package(ZLIB REQUIRED)

# skycue-dump: prints the skycue stream one line per frame. For the stream a
# pure libskycue consumer; skycued_asterix (pure code) decodes capture files.
add_executable(skycue-dump skycue-dump.cpp)
target_link_libraries(skycue-dump PRIVATE skycue::skycue skycued_asterix skycued_names Boost::program_options)

# skycue-rawrec: records readsb's Beast output and dump978's raw UAT lines.
add_executable(skycue-rawrec skycue-rawrec.cpp)
target_link_libraries(skycue-rawrec PRIVATE skycued_core skycue::net Boost::program_options Threads::Threads)

# skycue-rawplay: replays raw Beast, epoch,HEX and UAT captures into readsb.
# Links zlib to read .gz captures.
add_executable(skycue-rawplay skycue-rawplay.cpp)
target_link_libraries(skycue-rawplay PRIVATE skycued_core skycue::net Boost::program_options ZLIB::ZLIB Threads::Threads)

# skycue-pingsim: plays a pingStation 3 (capture files, or recordings
# synthesised as pingStation JSON or CAT021) to a skycued input.
add_executable(skycue-pingsim skycue-pingsim.cpp)
target_link_libraries(skycue-pingsim PRIVATE skycued_core skycue::net Boost::program_options ZLIB::ZLIB)

# skycue-stats: summarises received/sent recordings and readsb JSON captures
# (plain or .gz; zlib reads both).
add_executable(skycue-stats skycue-stats.cpp)
target_link_libraries(skycue-stats PRIVATE skycued_core Boost::program_options ZLIB::ZLIB)

# skycue-fisb: decodes FIS-B uplinks (dump978 lines, rawrec captures, received
# JSONL; plain or .gz, zlib reads both) to product JSONL or a summary.
add_executable(skycue-fisb skycue-fisb.cpp)
target_link_libraries(skycue-fisb PRIVATE skycued_core Boost::program_options ZLIB::ZLIB)

foreach(_t skycue-dump skycue-rawrec skycue-rawplay skycue-pingsim skycue-stats skycue-fisb)
  skycue_set_warnings(${_t})
  install(TARGETS ${_t} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()

# skycue-iqgen and skycue_iqgen_core.
add_subdirectory(iqgen)
