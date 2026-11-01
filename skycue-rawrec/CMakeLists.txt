# skycue-rawrec: records readsb's Beast output and dump978's raw UAT lines.
add_executable(skycue-rawrec main.cpp)
target_link_libraries(skycue-rawrec PRIVATE skycued_core Boost::program_options Threads::Threads)
skycue_set_warnings(skycue-rawrec)
install(TARGETS skycue-rawrec RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
