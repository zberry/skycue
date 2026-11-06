# The packaged skycue.conf, written from packaging/skycue.conf.example.
# Script mode: cmake -DIN=<skycue.conf.example> -DOUT=<skycue.conf> -P PackageConfig.cmake
#
# The package-shape values (STATE_DIR, MAP_DIR, the list files under
# /var/lib/skycue) are set and the keys only skycue-start.sh uses are
# commented out; everything else is the example's own text. Every edit must
# match exactly one whole line of the example, else this stops naming the
# line and writes nothing, so the example cannot drift away from what the
# packages install. The receiver position stays empty: the daemon refuses to
# start until it is set.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED IN OR NOT DEFINED OUT)
  message(FATAL_ERROR "usage: cmake -DIN=<skycue.conf.example> -DOUT=<skycue.conf> -P PackageConfig.cmake")
endif()
if(NOT EXISTS "${IN}")
  message(FATAL_ERROR "PackageConfig: ${IN} does not exist")
endif()

# Pairs: <line in the example> <line in the packaged config>.
set(_edit_count 9)
set(_from_1 "STATE_DIR=")
set(_to_1 "STATE_DIR=/var/lib/skycue")
set(_from_2 "MAP_DIR=")
set(_to_2 "MAP_DIR=/var/lib/skycue/maps")
set(_from_3 "RECORD_DIR=")
set(_to_3 "#RECORD_DIR=/var/lib/skycue/recordings")
set(_from_4 "ALLOW_LIST_FILE=")
set(_to_4 "ALLOW_LIST_FILE=allow.list")
set(_from_5 "BLOCK_LIST_FILE=")
set(_to_5 "BLOCK_LIST_FILE=block.list")
set(_from_6 "SDR_1090_SERIAL=")
set(_to_6 "#SDR_1090_SERIAL=")
set(_from_7 "SDR_978_SERIAL=")
set(_to_7 "#SDR_978_SERIAL=")
set(_from_8 "GAIN_1090=auto")
set(_to_8 "#GAIN_1090=auto")
set(_from_9 "GAIN_978=auto")
set(_to_9 "#GAIN_978=auto")
set(_header "# skycue configuration (packaged). Set RECEIVER_LAT and RECEIVER_LON, then: systemctl enable --now skycued")

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
  else()
    foreach(_i RANGE 1 ${_edit_count})
      if(_line STREQUAL _from_${_i})
        set(_line "${_to_${_i}}")
        math(EXPR _seen_${_i} "${_seen_${_i}} + 1")
        break()
      endif()
    endforeach()
  endif()
  string(APPEND _out "${_line}\n")
endwhile()

foreach(_i RANGE 1 ${_edit_count})
  if(_seen_${_i} EQUAL 0)
    message(FATAL_ERROR "PackageConfig: ${IN} has no line '${_from_${_i}}'")
  elseif(NOT _seen_${_i} EQUAL 1)
    message(FATAL_ERROR "PackageConfig: ${IN} has the line '${_from_${_i}}' ${_seen_${_i}} times, want once")
  endif()
endforeach()

file(WRITE "${OUT}" "${_out}")
