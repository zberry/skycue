cmake_minimum_required(VERSION 3.25)
project(skycue VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A parent project that vendors this directory and only wants libskycue sets
# SKYCUE_BUILD_DAEMON, SKYCUE_BUILD_TESTS and SKYCUE_BUILD_EXAMPLES OFF (and
# usually SKYCUE_INSTALL OFF, see libskycue/CMakeLists.txt) before
# add_subdirectory(); it then needs only Boost.System and Threads. Tests and
# examples default to OFF when skycue is not the top-level project.
option(SKYCUE_BUILD_DAEMON "Build skycued, the skycue-* tools and the web UI" ON)
option(SKYCUE_BUILD_TESTS "Build the skycue unit tests (needs SKYCUE_BUILD_DAEMON)" ${PROJECT_IS_TOP_LEVEL})
option(SKYCUE_BUILD_EXAMPLES "Build the example libskycue consumers" ${PROJECT_IS_TOP_LEVEL})
if(SKYCUE_BUILD_TESTS AND NOT SKYCUE_BUILD_DAEMON)
  message(FATAL_ERROR "skycue: SKYCUE_BUILD_TESTS=ON needs SKYCUE_BUILD_DAEMON=ON")
endif()

set(_skycue_boost_components system)
if(SKYCUE_BUILD_DAEMON)
  list(APPEND _skycue_boost_components program_options)
endif()
if(SKYCUE_BUILD_TESTS)
  list(APPEND _skycue_boost_components unit_test_framework)
endif()
find_package(Boost 1.66 REQUIRED COMPONENTS ${_skycue_boost_components})
find_package(Threads REQUIRED)

include(GNUInstallDirs)
include(cmake/CompilerWarnings.cmake)
include(cmake/Hardening.cmake)

enable_testing()

add_subdirectory(libskycue)
if(SKYCUE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(NOT SKYCUE_BUILD_DAEMON)
  return()
endif()

set(SKYCUE_LEAFLET_DIR "" CACHE PATH
  "Leaflet's extracted dist/ directory (leaflet.js, leaflet.css, images/); staged and installed under www/vendor/leaflet")
set(SKYCUE_ECHARTS_DIR "" CACHE PATH
  "ECharts' extracted npm package root (dist/echarts.min.js, LICENSE, NOTICE, licenses/LICENSE-d3); staged and installed under www/vendor/echarts")
set(SKYCUE_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/../sources" CACHE PATH
  "Directory holding the pinned leaflet and echarts npm tarballs, used when SKYCUE_LEAFLET_DIR or SKYCUE_ECHARTS_DIR is empty")
# Either directory left empty: the pinned tarball from SKYCUE_SOURCE_CACHE,
# hash-checked and extracted in the build tree (cmake/VendoredWeb.cmake). A
# directory that is given (the superbuild passes both) is used as it is.
if(NOT SKYCUE_LEAFLET_DIR OR NOT SKYCUE_ECHARTS_DIR)
  include(cmake/VendoredWeb.cmake)
  if(NOT SKYCUE_LEAFLET_DIR)
    skycue_vendored_web_dir(leaflet SKYCUE_LEAFLET_DIR)
  endif()
  if(NOT SKYCUE_ECHARTS_DIR)
    skycue_vendored_web_dir(echarts SKYCUE_ECHARTS_DIR)
  endif()
endif()
option(SKYCUE_UI_TESTS "Register the headless-browser test ui_playwright (skips itself without node/Playwright)" ON)

# The web UI: www/ plus Leaflet and ECharts, assembled in ${PROJECT_BINARY_DIR}/www-stage
# (what the tests serve with --www-dir) and installed to share/skycue/www.
# vendor/ and tiles/ in the source tree are local, untracked additions:
# neither is copied from there.
set(SKYCUE_WWW_STAGE "${PROJECT_BINARY_DIR}/www-stage")
file(GLOB _skycue_www_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/www/*.html"
  "${PROJECT_SOURCE_DIR}/www/*.js" "${PROJECT_SOURCE_DIR}/www/*.css" "${PROJECT_SOURCE_DIR}/www/*.svg")
set(_skycue_www_cmds COMMAND ${CMAKE_COMMAND} -E make_directory ${SKYCUE_WWW_STAGE})
if(_skycue_www_files)
  list(APPEND _skycue_www_cmds
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_skycue_www_files} ${SKYCUE_WWW_STAGE}/)
endif()
if(SKYCUE_LEAFLET_DIR)
  foreach(_f leaflet.js leaflet.css)
    if(NOT EXISTS "${SKYCUE_LEAFLET_DIR}/${_f}")
      message(FATAL_ERROR "skycue: SKYCUE_LEAFLET_DIR=${SKYCUE_LEAFLET_DIR} has no ${_f}")
    endif()
  endforeach()
  # Leaflet's BSD-2-Clause LICENSE sits beside dist/ in its package and
  # travels with the copied files.
  set(_skycue_leaflet_files ${SKYCUE_LEAFLET_DIR}/leaflet.js ${SKYCUE_LEAFLET_DIR}/leaflet.css)
  get_filename_component(_skycue_leaflet_license "${SKYCUE_LEAFLET_DIR}/../LICENSE" ABSOLUTE)
  if(EXISTS "${_skycue_leaflet_license}")
    list(APPEND _skycue_leaflet_files ${_skycue_leaflet_license})
  endif()
  list(APPEND _skycue_www_cmds
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SKYCUE_WWW_STAGE}/vendor/leaflet
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_skycue_leaflet_files}
      ${SKYCUE_WWW_STAGE}/vendor/leaflet/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SKYCUE_LEAFLET_DIR}/images
      ${SKYCUE_WWW_STAGE}/vendor/leaflet/images)
endif()
# ECharts: the one prebuilt file plus the licence files Apache-2.0 asks to
# travel with it (LICENSE names the d3-derived parts, licenses/LICENSE-d3).
if(SKYCUE_ECHARTS_DIR)
  foreach(_f dist/echarts.min.js LICENSE NOTICE licenses/LICENSE-d3)
    if(NOT EXISTS "${SKYCUE_ECHARTS_DIR}/${_f}")
      message(FATAL_ERROR "skycue: SKYCUE_ECHARTS_DIR=${SKYCUE_ECHARTS_DIR} has no ${_f}")
    endif()
  endforeach()
  list(APPEND _skycue_www_cmds
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SKYCUE_WWW_STAGE}/vendor/echarts/licenses
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYCUE_ECHARTS_DIR}/dist/echarts.min.js
      ${SKYCUE_ECHARTS_DIR}/LICENSE ${SKYCUE_ECHARTS_DIR}/NOTICE ${SKYCUE_WWW_STAGE}/vendor/echarts/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYCUE_ECHARTS_DIR}/licenses/LICENSE-d3
      ${SKYCUE_WWW_STAGE}/vendor/echarts/licenses/)
endif()
add_custom_target(www_stage ALL ${_skycue_www_cmds}
  COMMENT "Staging the web UI in ${SKYCUE_WWW_STAGE}" VERBATIM)
install(FILES ${_skycue_www_files} DESTINATION ${CMAKE_INSTALL_DATADIR}/skycue/www)
if(SKYCUE_LEAFLET_DIR)
  install(FILES ${_skycue_leaflet_files}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/skycue/www/vendor/leaflet)
  install(DIRECTORY ${SKYCUE_LEAFLET_DIR}/images/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/skycue/www/vendor/leaflet/images)
endif()
if(SKYCUE_ECHARTS_DIR)
  install(FILES ${SKYCUE_ECHARTS_DIR}/dist/echarts.min.js ${SKYCUE_ECHARTS_DIR}/LICENSE
    ${SKYCUE_ECHARTS_DIR}/NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/skycue/www/vendor/echarts)
  install(FILES ${SKYCUE_ECHARTS_DIR}/licenses/LICENSE-d3
    DESTINATION ${CMAKE_INSTALL_DATADIR}/skycue/www/vendor/echarts/licenses)
endif()

add_subdirectory(skycued)
add_subdirectory(skycue-dump)
add_subdirectory(skycue-rawrec)
add_subdirectory(skycue-rawplay)
add_subdirectory(skycue-pingsim)
add_subdirectory(skycue-stats)
add_subdirectory(skycue-fisb)
add_subdirectory(skycue-iqgen)
# Before tests/: fuzz_smoke there reads SKYCUE_FUZZ_TARGETS.
if(SKYCUE_FUZZ)
  add_subdirectory(fuzz)
endif()
if(SKYCUE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
