# Source-tarball cache for the web UI pins.
#
# Every pinned tarball is declared once in cmake/web-sources.cmake with
# skycue_declare_source(). cmake/VendoredWeb.cmake looks tarballs up with
# skycue_source_path() / skycue_source_sha256(). Nothing is ever downloaded
# here: a missing tarball is a configure error that says where to put it.

include_guard(GLOBAL)

# skycue_declare_source(<name> URL <url> SHA256 <hex> FILE <basename>)
function(skycue_declare_source name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;FILE" "")
  foreach(key IN ITEMS URL SHA256 FILE)
    if(NOT arg_${key})
      message(FATAL_ERROR "skycue: skycue_declare_source(${name}) is missing ${key}")
    endif()
  endforeach()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "skycue: skycue_declare_source(${name}) got unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()
  get_property(names GLOBAL PROPERTY SKYCUE_SOURCE_NAMES)
  if(name IN_LIST names)
    message(FATAL_ERROR "skycue: source ${name} declared twice")
  endif()
  set_property(GLOBAL PROPERTY SKYCUE_SOURCE_${name}_URL "${arg_URL}")
  set_property(GLOBAL PROPERTY SKYCUE_SOURCE_${name}_SHA256 "${arg_SHA256}")
  set_property(GLOBAL PROPERTY SKYCUE_SOURCE_${name}_FILE "${arg_FILE}")
  set_property(GLOBAL APPEND PROPERTY SKYCUE_SOURCE_NAMES "${name}")
endfunction()

# Fetch one declared property, failing loudly for undeclared names.
function(_skycue_source_property name key out_var)
  get_property(is_set GLOBAL PROPERTY SKYCUE_SOURCE_${name}_${key} SET)
  if(NOT is_set)
    message(FATAL_ERROR "skycue: source ${name} is not declared in cmake/web-sources.cmake")
  endif()
  get_property(value GLOBAL PROPERTY SKYCUE_SOURCE_${name}_${key})
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# What the missing-file and hash-mismatch errors tell the user to do. The
# default configure keeps these defaults; a project that includes this file without
# a fetch target sets its own before calling. An empty
# SKYCUE_SOURCE_MISMATCH_HINT ends the mismatch error after the two hashes.
if(NOT DEFINED SKYCUE_SOURCE_MISSING_HINT)
  set(SKYCUE_SOURCE_MISSING_HINT "run with -DSKYCUE_FETCH_SOURCES=ON on a connected machine")
endif()
if(NOT DEFINED SKYCUE_SOURCE_MISMATCH_HINT)
  set(SKYCUE_SOURCE_MISMATCH_HINT "replace it with -DSKYCUE_FETCH_SOURCES=ON on a connected machine")
endif()

# skycue_source_path(<name> <out_var>)
function(skycue_source_path name out_var)
  _skycue_source_property(${name} FILE file)
  set(path "${SKYCUE_SOURCE_CACHE}/${file}")
  if(NOT SKYCUE_FETCH_SOURCES AND NOT EXISTS "${path}")
    # The leading space marks the text as preformatted so CMake prints it on
    # one line instead of word-wrapping the path (tests grep for it).
    message(FATAL_ERROR " skycue: missing source tarball ${path} for ${name}; ${SKYCUE_SOURCE_MISSING_HINT}")
  endif()
  set(${out_var} "${path}" PARENT_SCOPE)
endfunction()

# skycue_source_sha256(<name> <out_var>)
function(skycue_source_sha256 name out_var)
  _skycue_source_property(${name} SHA256 sha)
  set(${out_var} "${sha}" PARENT_SCOPE)
endfunction()

# skycue_parallel_jobs(<out_var>)
# Job count for `make -jN` in Makefile-based deps: the processor count,
# or 2 when CMake cannot determine it.
function(skycue_parallel_jobs out_var)
  include(ProcessorCount)
  ProcessorCount(n)
  if(n EQUAL 0)
    set(n 2)
  endif()
  set(${out_var} ${n} PARENT_SCOPE)
endfunction()

# Offline mode: fail at configure time if any declared tarball is absent or
# its SHA-256 differs from the manifest. The whole cache is about 10 MB, so
# hashing it on every configure is cheap, and a bad file is reported before
# anything is built (URL_HASH in ExternalProject would only catch it at the
# download step of that one dependency).
function(skycue_check_sources_present)
  get_property(names GLOBAL PROPERTY SKYCUE_SOURCE_NAMES)
  foreach(name IN LISTS names)
    skycue_check_source(${name} path)
  endforeach()
endfunction()

# skycue_check_source(<name> <out_var>)
# One tarball: present (else the missing-file error) and matching its
# manifest SHA-256 (else the mismatch error). Sets <out_var> to its path.
function(skycue_check_source name out_var)
  skycue_source_path(${name} path)
  skycue_source_sha256(${name} want)
  file(SHA256 "${path}" have)
  if(NOT have STREQUAL want)
    set(hint "")
    if(NOT SKYCUE_SOURCE_MISMATCH_HINT STREQUAL "")
      set(hint "; ${SKYCUE_SOURCE_MISMATCH_HINT}")
    endif()
    # Leading space: printed on one line, as for the missing-file error.
    message(FATAL_ERROR " skycue: hash mismatch for ${path} (${name}): manifest SHA256 ${want}, file SHA256 ${have}${hint}")
  endif()
  set(${out_var} "${path}" PARENT_SCOPE)
endfunction()

# Fetch mode: generate ${CMAKE_BINARY_DIR}/fetch-sources.cmake and a
# `fetch-sources` target that runs it. Files already in the cache with the
# right hash are skipped; files with a wrong hash are deleted and re-fetched.
function(skycue_define_fetch_target)
  set(script "${CMAKE_BINARY_DIR}/fetch-sources.cmake")
  set(content [=[
# Generated by cmake/SkycueSources.cmake; do not edit.
function(skycue_fetch name url sha path)
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
    if(have STREQUAL sha)
      message(STATUS "skycue: ${name}: cached ${path}")
      return()
    endif()
    message(STATUS "skycue: ${name}: wrong hash for ${path}, re-fetching")
    file(REMOVE "${path}")
  endif()
  message(STATUS "skycue: ${name}: fetching ${url}")
  file(DOWNLOAD "${url}" "${path}" EXPECTED_HASH SHA256=${sha} SHOW_PROGRESS STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${path}")
    message(FATAL_ERROR "skycue: ${name}: download of ${url} failed: ${status}")
  endif()
endfunction()
]=])
  string(APPEND content "file(MAKE_DIRECTORY \"${SKYCUE_SOURCE_CACHE}\")\n")
  get_property(names GLOBAL PROPERTY SKYCUE_SOURCE_NAMES)
  foreach(name IN LISTS names)
    _skycue_source_property(${name} URL url)
    skycue_source_sha256(${name} sha)
    skycue_source_path(${name} path)
    string(APPEND content "skycue_fetch(\"${name}\" \"${url}\" \"${sha}\" \"${path}\")\n")
  endforeach()
  file(WRITE "${script}" "${content}")
  add_custom_target(fetch-sources
    COMMAND "${CMAKE_COMMAND}" -P "${script}"
    COMMENT "Fetching source tarballs into ${SKYCUE_SOURCE_CACHE}"
    VERBATIM)
endfunction()
