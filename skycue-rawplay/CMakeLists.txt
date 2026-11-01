# skycue-rawplay: replays raw Beast, epoch,HEX and UAT captures into readsb.
# The one skycue tool that links zlib (to read .gz captures).
find_package(ZLIB REQUIRED)
add_executable(skycue-rawplay main.cpp)
target_link_libraries(skycue-rawplay PRIVATE skycued_core Boost::program_options ZLIB::ZLIB Threads::Threads)
skycue_set_warnings(skycue-rawplay)
install(TARGETS skycue-rawplay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
