# skycue-dump: prints the skycue stream one line per frame. For the stream a
# pure libskycue consumer; skycued_asterix (pure code) decodes capture files.
add_executable(skycue-dump main.cpp)
target_link_libraries(skycue-dump PRIVATE skycue::skycue skycued_asterix Boost::program_options)
skycue_set_warnings(skycue-dump)
install(TARGETS skycue-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
