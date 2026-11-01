# skycue-dump: prints the skycue stream one line per frame. A pure libskycue
# consumer: it links skycue::skycue and nothing from skycued.
add_executable(skycue-dump main.cpp)
target_link_libraries(skycue-dump PRIVATE skycue::skycue Boost::program_options)
skycue_set_warnings(skycue-dump)
install(TARGETS skycue-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
