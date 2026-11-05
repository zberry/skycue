# Leaflet and ECharts for the web UI when SKYCUE_LEAFLET_DIR or
# SKYCUE_ECHARTS_DIR is not given: the pinned npm tarballs are taken from
# SKYCUE_SOURCE_CACHE, checked against their SHA-256 in the repository's
# cmake/sources.cmake and extracted under ${PROJECT_BINARY_DIR}/web-tarballs/.
# Nothing is downloaded. The pins and the missing-file and hash checks are
# the superbuild's own (cmake/SkycueSources.cmake), included rather than
# copied, so this file is only usable inside the full source tree (or the
# release source tarball, which carries cmake/ and sources/ beside skycue/).

include_guard(GLOBAL)

get_filename_component(_skycue_repo_dir "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT EXISTS "${_skycue_repo_dir}/cmake/sources.cmake"
   OR NOT EXISTS "${_skycue_repo_dir}/cmake/SkycueSources.cmake")
  message(FATAL_ERROR " skycue: SKYCUE_LEAFLET_DIR and SKYCUE_ECHARTS_DIR are not set and ${_skycue_repo_dir}/cmake/sources.cmake is not there to find the pinned tarballs; set both")
endif()

# The superbuild's errors suggest its fetch mode, which skycue/ does not
# have: point at the release source tarball and the two variables instead.
set(SKYCUE_SOURCE_MISSING_HINT
  "it ships in the release source tarball (skycue-${PROJECT_VERSION}.tar.gz), or set SKYCUE_LEAFLET_DIR and SKYCUE_ECHARTS_DIR")
set(SKYCUE_SOURCE_MISMATCH_HINT "")
include("${_skycue_repo_dir}/cmake/SkycueSources.cmake")
# A parent build that already read the manifest must not declare it twice.
get_property(_skycue_declared GLOBAL PROPERTY SKYCUE_SOURCE_NAMES)
if(NOT "leaflet" IN_LIST _skycue_declared)
  include("${_skycue_repo_dir}/cmake/sources.cmake")
endif()

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
