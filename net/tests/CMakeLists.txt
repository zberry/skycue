# Boost.Test targets for skycue_net.
if(NOT TARGET Boost::unit_test_framework)
  find_package(Boost 1.66 REQUIRED COMPONENTS unit_test_framework)
endif()

# Local to net/ so that it builds alone (it does not include the project's
# cmake/SkycueTests.cmake). Target test_net_<name>, ctest net_<name>.
function(skycue_net_unit_test name source)
  add_executable(test_net_${name} ${source})
  target_link_libraries(test_net_${name} PRIVATE skycue_net Boost::unit_test_framework Threads::Threads)
  target_compile_definitions(test_net_${name} PRIVATE BOOST_TEST_DYN_LINK)
  if(COMMAND skycue_set_warnings)
    skycue_set_warnings(test_net_${name})
  endif()
  add_test(NAME net_${name} COMMAND test_net_${name})
  set_tests_properties(net_${name} PROPERTIES TIMEOUT 60)
endfunction()

skycue_net_unit_test(endpoint test_endpoint.cpp)
skycue_net_unit_test(framer test_framer.cpp)
skycue_net_unit_test(backoff test_backoff.cpp)
skycue_net_unit_test(write_queue test_write_queue.cpp)
skycue_net_unit_test(dialer test_dialer.cpp)
skycue_net_unit_test(listener test_listener.cpp)
skycue_net_unit_test(udp test_udp.cpp)
skycue_net_unit_test(fanout test_fanout.cpp)

# Timeouts and SKYCUE_TEST_SLOW scaled by the slow factor when the host
# project provides the helper (cmake/Hardening.cmake); standalone, unscaled.
if(COMMAND skycue_scale_test_timeouts)
  skycue_scale_test_timeouts()
endif()
