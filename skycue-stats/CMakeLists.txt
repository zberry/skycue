# skycue-stats: summarises received/sent recordings and readsb JSON captures
# (plain or .gz; zlib reads both).
find_package(ZLIB REQUIRED)
add_executable(skycue-stats main.cpp)
target_link_libraries(skycue-stats PRIVATE skycued_core Boost::program_options ZLIB::ZLIB)
skycue_set_warnings(skycue-stats)
install(TARGETS skycue-stats RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
