# Boost.Test targets for skycue_net.
if(NOT TARGET Boost::unit_test_framework)
  find_package(Boost 1.66 REQUIRED COMPONENTS unit_test_framework)
endif()

add_executable(test_net_endpoint test_endpoint.cpp)
target_link_libraries(test_net_endpoint PRIVATE skycue_net Boost::unit_test_framework Threads::Threads)
target_compile_definitions(test_net_endpoint PRIVATE BOOST_TEST_DYN_LINK)
if(COMMAND skycue_set_warnings)
  skycue_set_warnings(test_net_endpoint)
endif()
add_test(NAME net_endpoint COMMAND test_net_endpoint)
set_tests_properties(net_endpoint PROPERTIES TIMEOUT 60)
