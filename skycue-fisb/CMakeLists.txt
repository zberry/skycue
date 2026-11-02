# skycue-fisb: decodes FIS-B uplinks (dump978 lines, rawrec captures, received
# JSONL; plain or .gz, zlib reads both) to product JSONL or a summary.
find_package(ZLIB REQUIRED)
add_executable(skycue-fisb main.cpp)
target_link_libraries(skycue-fisb PRIVATE skycued_core Boost::program_options ZLIB::ZLIB)
skycue_set_warnings(skycue-fisb)
install(TARGETS skycue-fisb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
