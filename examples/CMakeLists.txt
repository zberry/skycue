# Example libskycue consumers, built with the project so they keep compiling.
# docs/libskycue.md embeds minimal_consumer.cpp verbatim.
add_executable(minimal_consumer minimal_consumer.cpp)
target_link_libraries(minimal_consumer PRIVATE skycue::skycue)
skycue_set_warnings(minimal_consumer)

# With no arguments it must print its usage and exit 2.
add_test(NAME example_minimal_consumer_help
  COMMAND bash -c "out=$(\"$0\" 2>&1); rc=$?; echo \"$out\"; [ $rc -eq 2 ] && [[ $out == usage:* ]]"
    $<TARGET_FILE:minimal_consumer>)

add_executable(ecef_consumer ecef_consumer.cpp)
target_link_libraries(ecef_consumer PRIVATE skycue::skycue)
skycue_set_warnings(ecef_consumer)
add_executable(ecef_batch_consumer ecef_batch_consumer.cpp)
target_link_libraries(ecef_batch_consumer PRIVATE skycue::skycue)
skycue_set_warnings(ecef_batch_consumer)

add_test(NAME example_ecef_consumer_help
  COMMAND bash -c "out=$(\"$0\" 2>&1); rc=$?; echo \"$out\"; [ $rc -eq 2 ] && [[ $out == usage:* ]]"
    $<TARGET_FILE:ecef_consumer>)
# A malformed --sensor is rejected before anything is opened: exit 2 and usage.
add_test(NAME example_ecef_consumer_bad_sensor
  COMMAND bash -c "out=$(\"$0\" --sensor 95,0 tcp 127.0.0.1:1 2>&1); rc=$?; echo \"$out\"; [ $rc -eq 2 ] && [[ $out == usage:* ]]"
    $<TARGET_FILE:ecef_consumer>)
add_test(NAME example_ecef_batch_consumer_help
  COMMAND bash -c "out=$(\"$0\" 2>&1); rc=$?; echo \"$out\"; [ $rc -eq 2 ] && [[ $out == usage:* ]]"
    $<TARGET_FILE:ecef_batch_consumer>)

set_tests_properties(example_minimal_consumer_help example_ecef_consumer_help
  example_ecef_consumer_bad_sensor example_ecef_batch_consumer_help PROPERTIES LABELS integration)

# Timeouts and SKYCUE_TEST_SLOW scaled by the slow factor (Hardening.cmake).
skycue_scale_test_timeouts()
