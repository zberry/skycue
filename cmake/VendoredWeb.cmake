# Leaflet and ECharts for the web UI when SKYCUE_LEAFLET_DIR or
# SKYCUE_ECHARTS_DIR is not given: the pinned npm tarballs are taken from
# SKYCUE_SOURCE_CACHE, checked against their SHA-256 in cmake/web-sources.cmake
# and extracted under ${PROJECT_BINARY_DIR}/web-tarballs/. Nothing is
# downloaded. The missing-file and hash checks are in cmake/SkycueSources.cmake.

include_guard(GLOBAL)

set(SKYCUE_SOURCE_MISSING_HINT
  "it ships in the release tarball (skycue-${PROJECT_VERSION}.tar.gz), or fill sources/ with the two web tarballs, or set SKYCUE_LEAFLET_DIR and SKYCUE_ECHARTS_DIR")
set(SKYCUE_SOURCE_MISMATCH_HINT "")
include("${PROJECT_SOURCE_DIR}/cmake/SkycueSources.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/web-sources.cmake")

# skycue_vendored_web_dir(<leaflet|echarts> <out_var>)
# Checks the tarball, extracts it once (re-extracted only when the pinned
# hash changes) and sets <out_var> to what SKYCUE_LEAFLET_DIR (package/dist)
# or SKYCUE_ECHARTS_DIR (package) would be.
function(skycue_vendored_web_dir name out_var)
  if(name STREQUAL "leaflet")
    set(subdir package/dist)
  elseif(name STREQUAL "echarts")
    set(subdir package)
  else()
    message(FATAL_ERROR "skycue: skycue_vendored_web_dir: unknown name ${name}")
  endif()
  # Nothing is downloaded here: a -DSKYCUE_FETCH_SOURCES=ON given to this project
  # must not skip the missing-file check (function scope only).
  set(SKYCUE_FETCH_SOURCES OFF)
  skycue_check_source(${name} tarball)
  skycue_source_sha256(${name} sha)
  set(dest "${PROJECT_BINARY_DIR}/web-tarballs/${name}")
  set(stamp "${dest}/.extracted-sha256")
  set(have "")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" have)
  endif()
  if(NOT have STREQUAL sha OR NOT IS_DIRECTORY "${dest}/${subdir}")
    message(STATUS "skycue: extracting ${tarball}")
    file(REMOVE_RECURSE "${dest}")
    file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${dest}")
    if(NOT IS_DIRECTORY "${dest}/${subdir}")
      message(FATAL_ERROR " skycue: ${tarball} has no ${subdir}/")
    endif()
    file(WRITE "${stamp}" "${sha}")
  endif()
  # Reconfigure when the tarball is replaced.
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tarball}")
  set(${out_var} "${dest}/${subdir}" PARENT_SCOPE)
endfunction()
