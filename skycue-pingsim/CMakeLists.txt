# skycue-pingsim: plays a pingStation 3 (capture files, or recordings
# synthesised as pingStation JSON or CAT021) to a skycued input.
find_package(ZLIB REQUIRED)
add_executable(skycue-pingsim main.cpp)
target_link_libraries(skycue-pingsim PRIVATE skycued_core Boost::program_options ZLIB::ZLIB)
skycue_set_warnings(skycue-pingsim)
install(TARGETS skycue-pingsim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
