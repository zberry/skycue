# Example libskycue consumers, built with the project so they keep compiling.
# docs/libskycue.md embeds minimal_consumer.cpp verbatim.
add_executable(minimal_consumer minimal_consumer.cpp)
target_link_libraries(minimal_consumer PRIVATE skycue::skycue)
skycue_set_warnings(minimal_consumer)

# With no arguments it must print its usage and exit 2.
add_test(NAME example_minimal_consumer_help
  COMMAND bash -c "out=$(\"$0\" 2>&1); rc=$?; echo \"$out\"; [ $rc -eq 2 ] && [[ $out == usage:* ]]"
    $<TARGET_FILE:minimal_consumer>)

# Timeouts and SKYCUE_TEST_SLOW scaled by the slow factor (Hardening.cmake).
skycue_scale_test_timeouts()
