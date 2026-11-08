# The packaged skycue.toml, written from packaging/skycue.toml.example.
# Script mode: cmake -DIN=<skycue.toml.example> -DOUT=<skycue.toml> -P PackageConfig.cmake
#
# The package-shape values (paths.state_dir, paths.map_dir, the list files
# under /var/lib/skycue) are set and recording.dir is commented out;
# everything else is the example's own text, trailing comments included. Every
# edit must match exactly one whole `key = ""` line of its table in the
# example, else this stops naming the line and writes nothing, so the example
# cannot drift away from what the packages install. The receiver position
# stays 0.0: the daemon refuses to start until it is set.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED IN OR NOT DEFINED OUT)
  message(FATAL_ERROR "usage: cmake -DIN=<skycue.toml.example> -DOUT=<skycue.toml> -P PackageConfig.cmake")
endif()
if(NOT EXISTS "${IN}")
  message(FATAL_ERROR "PackageConfig: ${IN} does not exist")
endif()

# Edits: <table> <key> <the packaged line, before the example's trailing comment>.
set(_edit_count 5)
set(_table_1 "paths")
set(_key_1 "state_dir")
set(_to_1 "state_dir = \"/var/lib/skycue\"")
set(_table_2 "paths")
set(_key_2 "map_dir")
set(_to_2 "map_dir = \"/var/lib/skycue/maps\"")
set(_table_3 "recording")
set(_key_3 "dir")
set(_to_3 "#dir = \"/var/lib/skycue/recordings\"")
set(_table_4 "filters")
set(_key_4 "allow_list_file")
set(_to_4 "allow_list_file = \"allow.list\"")
set(_table_5 "filters")
set(_key_5 "block_list_file")
set(_to_5 "block_list_file = \"block.list\"")
set(_header "# skycue configuration (packaged). Set receiver lat and lon, then: systemctl enable --now skycued")

file(READ "${IN}" _text)
if(_text MATCHES "\r")
  message(FATAL_ERROR "PackageConfig: ${IN} has CR line endings")
endif()
if(NOT _text MATCHES "\n$")
  string(APPEND _text "\n")
endif()

# Line by line with string(FIND): a CMake list would mangle the ';', '[' and
# ']' the comments contain.
foreach(_i RANGE 1 ${_edit_count})
  set(_seen_${_i} 0)
endforeach()
set(_out "")
set(_lineno 0)
set(_table "")
while(NOT _text STREQUAL "")
  string(FIND "${_text}" "\n" _nl)
  string(SUBSTRING "${_text}" 0 ${_nl} _line)
  math(EXPR _rest "${_nl} + 1")
  string(SUBSTRING "${_text}" ${_rest} -1 _text)
  math(EXPR _lineno "${_lineno} + 1")
  if(_lineno EQUAL 1)
    if(NOT _line MATCHES "^# skycue configuration")
      message(FATAL_ERROR "PackageConfig: line 1 of ${IN} is not the '# skycue configuration' header")
    endif()
    set(_line "${_header}")
  elseif(_line MATCHES "^\\[([A-Za-z0-9_.]+)\\]")
    set(_table "${CMAKE_MATCH_1}")
  elseif(_line MATCHES "^([A-Za-z0-9_]+) = \"\"( +#.*)?$")
    set(_key "${CMAKE_MATCH_1}")
    set(_tail "${CMAKE_MATCH_2}")
    foreach(_i RANGE 1 ${_edit_count})
      if(_table STREQUAL _table_${_i} AND _key STREQUAL _key_${_i})
        set(_line "${_to_${_i}}${_tail}")
        math(EXPR _seen_${_i} "${_seen_${_i}} + 1")
        break()
      endif()
    endforeach()
  endif()
  string(APPEND _out "${_line}\n")
endwhile()

foreach(_i RANGE 1 ${_edit_count})
  if(_seen_${_i} EQUAL 0)
    message(FATAL_ERROR "PackageConfig: ${IN} has no line '${_key_${_i}} = \"\"' under [${_table_${_i}}]")
  elseif(NOT _seen_${_i} EQUAL 1)
    message(FATAL_ERROR "PackageConfig: ${IN} has the line '${_key_${_i}} = \"\"' under [${_table_${_i}}] ${_seen_${_i}} times, want once")
  endif()
endforeach()

file(WRITE "${OUT}" "${_out}")
