# Helpers that register the project's tests. Included from the top-level
# CMakeLists.txt after Hardening.cmake, so skycue_set_warnings() exists.
#
# skycue_add_unit_test(<name>
#   [SOURCE <file>] [EXTRA_SOURCES <files...>] [LIBS <targets...>]
#   [DEFS <KEY=VALUE...>] [DATA_DIR] [NET_SUPPORT]
#   [TIMEOUT <seconds>] [SKIPPABLE] [LABELS <labels...>])
#   A Boost.Test executable test_<name> (source test_<name>.cpp unless SOURCE
#   is given), registered as ctest <name>. Links skycued_core unless LIBS is
#   given, and always Boost::unit_test_framework (plus stdc++fs on GCC older
#   than 9, where std::filesystem is a separate library). DATA_DIR defines
#   SKYCUE_TEST_DATA_DIR as <this directory>/data; NET_SUPPORT adds
#   net/tests to the include path. SKIPPABLE: exit status 77 is a skip.
#   Labels always include "unit".
#
# skycue_add_script_test(<name> SCRIPT <path>
#   [ARGS <args...>] [TIMEOUT <seconds>] [SKIPPABLE] [NESTED_CMAKE]
#   [ENVIRONMENT <KEY=VALUE...>] [WORKING_DIRECTORY <dir>] [LABELS <labels...>])
#   ctest <name> runs `bash <path> <args...>`. NESTED_CMAKE passes what a
#   script needs to configure a nested CMake project against the same
#   toolchain and Boost. Labels default to "integration".
#
# skycue_label_tests(<labels> <test names...>)
#   Appends labels to tests registered by hand.
#
# A property that is not asked for stays unset: a test without TIMEOUT keeps
# none, which skycue_scale_test_timeouts() relies on.

include_guard(GLOBAL)

# The project's warnings for a Boost.Test executable.
function(skycue_test_warnings target)
  skycue_set_warnings(${target})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Boost.Test 1.90's assertion_result conversion trips -Wconversion under
    # GCC 15 at every BOOST_TEST with a message; product code keeps it.
    target_compile_options(${target} PRIVATE -Wno-conversion)
  endif()
endfunction()

function(skycue_add_unit_test name)
  cmake_parse_arguments(PARSE_ARGV 1 A "DATA_DIR;NET_SUPPORT;SKIPPABLE"
    "SOURCE;TIMEOUT" "EXTRA_SOURCES;LIBS;DEFS;LABELS")
  if(A_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "skycue_add_unit_test(${name}): unknown arguments ${A_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT A_SOURCE)
    set(A_SOURCE test_${name}.cpp)
  endif()
  if(NOT A_LIBS AND NOT "LIBS" IN_LIST A_KEYWORDS_MISSING_VALUES)
    set(A_LIBS skycued_core)
  endif()
  set(target test_${name})
  add_executable(${target} ${A_SOURCE} ${A_EXTRA_SOURCES})
  # GCC 8 (EL8) ships std::filesystem in a separate library.
  target_link_libraries(${target} PRIVATE ${A_LIBS} Boost::unit_test_framework $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9>>:stdc++fs>)
  target_compile_definitions(${target} PRIVATE BOOST_TEST_DYN_LINK ${A_DEFS})
  if(A_DATA_DIR)
    target_compile_definitions(${target} PRIVATE
      SKYCUE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  endif()
  if(A_NET_SUPPORT)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/net/tests)
  endif()
  skycue_test_warnings(${target})
  add_test(NAME ${name} COMMAND ${target})

  set(props)
  if(A_TIMEOUT)
    list(APPEND props TIMEOUT ${A_TIMEOUT})
  endif()
  if(A_SKIPPABLE)
    list(APPEND props SKIP_RETURN_CODE 77)
  endif()
  if(props)
    set_tests_properties(${name} PROPERTIES ${props})
  endif()
  set(_l unit ${A_LABELS})
  set_tests_properties(${name} PROPERTIES LABELS "${_l}")
endfunction()

function(skycue_add_script_test name)
  cmake_parse_arguments(PARSE_ARGV 1 A "SKIPPABLE;NESTED_CMAKE"
    "SCRIPT;TIMEOUT;WORKING_DIRECTORY" "ARGS;ENVIRONMENT;LABELS")
  if(A_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "skycue_add_script_test(${name}): unknown arguments ${A_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT A_SCRIPT)
    message(FATAL_ERROR "skycue_add_script_test(${name}): SCRIPT is required")
  endif()
  set(labels integration ${A_LABELS})
  if(A_WORKING_DIRECTORY)
    add_test(NAME ${name} COMMAND bash ${A_SCRIPT} ${A_ARGS}
      WORKING_DIRECTORY ${A_WORKING_DIRECTORY})
  else()
    add_test(NAME ${name} COMMAND bash ${A_SCRIPT} ${A_ARGS})
  endif()

  set(env ${A_ENVIRONMENT})
  if(A_NESTED_CMAKE)
    set(boost_dir "")
    if(Boost_DIR)
      set(boost_dir "${Boost_DIR}")
    endif()
    string(REPLACE ";" "\;" prefix_path "${CMAKE_PREFIX_PATH}")
    list(INSERT env 0 "CMAKE=${CMAKE_COMMAND}" "CXX=${CMAKE_CXX_COMPILER}"
      "SKYCUE_SMOKE_BOOST_DIR=${boost_dir}" "SKYCUE_SMOKE_PREFIX_PATH=${prefix_path}")
  endif()

  set(props)
  if(A_TIMEOUT)
    list(APPEND props TIMEOUT ${A_TIMEOUT})
  endif()
  if(A_SKIPPABLE)
    list(APPEND props SKIP_RETURN_CODE 77)
  endif()
  if(props)
    set_tests_properties(${name} PROPERTIES ${props})
  endif()
  set_tests_properties(${name} PROPERTIES LABELS "${labels}")
  if(env)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${env}")
  endif()
endfunction()

function(skycue_label_tests labels)
  foreach(t IN LISTS ARGN)
    set_property(TEST ${t} APPEND PROPERTY LABELS ${labels})
  endforeach()
endfunction()
