add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE Boost::unit_test_framework)
target_compile_definitions(test_smoke PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE skycue::skycue Boost::unit_test_framework)
target_compile_definitions(test_protocol PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_protocol)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE skycue::skycue Boost::unit_test_framework)
target_compile_definitions(test_client PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_client)
add_test(NAME client COMMAND test_client)
set_tests_properties(client PROPERTIES TIMEOUT 60)

get_filename_component(_skycue_example_conf
  "${CMAKE_CURRENT_SOURCE_DIR}/../../etc/skycue.conf.example" ABSOLUTE)
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_config PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_EXAMPLE_CONF="${_skycue_example_conf}")
skycue_set_warnings(test_config)
add_test(NAME config COMMAND test_config)

# Builds a tiny consumer against libskycue through add_subdirectory() and
# through find_package() on an install of it.
add_test(NAME consumer_smoke
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/consumer_smoke.sh
    ${PROJECT_SOURCE_DIR}/libskycue
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer-smoke
    ${CMAKE_CURRENT_BINARY_DIR}/consumer-smoke-work
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set(_smoke_boost_dir "")
if(Boost_DIR)
  set(_smoke_boost_dir "${Boost_DIR}")
endif()
string(REPLACE ";" "\\;" _smoke_prefix_path "${CMAKE_PREFIX_PATH}")
set_tests_properties(consumer_smoke PROPERTIES
  TIMEOUT 300
  ENVIRONMENT "CMAKE=${CMAKE_COMMAND};CXX=${CMAKE_CXX_COMPILER};SKYCUE_SMOKE_BOOST_DIR=${_smoke_boost_dir};SKYCUE_SMOKE_PREFIX_PATH=${_smoke_prefix_path}")
