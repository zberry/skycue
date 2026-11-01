include_guard(GLOBAL)

option(SKYCUE_WERROR "Treat compiler warnings as errors" OFF)

# skycue_set_warnings(<target>): the warning set every skycue target builds with.
function(skycue_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(SKYCUE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
