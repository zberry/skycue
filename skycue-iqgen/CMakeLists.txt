# skycue-iqgen: synthesises 1090/978 MHz IQ from recorded Mode S and UAT
# frames. skycue_iqgen_core holds the input readers and frame placement, the
# frame coding (Mode S CRC, UAT Reed-Solomon and framing) and the sample layer
# (synthesisers, CU8 writer, resampler), so the tests can link it.
find_package(ZLIB REQUIRED)
add_library(skycue_iqgen_core STATIC
  bits.cpp
  inputs.cpp
  iq_writer.cpp
  modes_crc.cpp
  placement.cpp
  reed_solomon.cpp
  resample.cpp
  synth.cpp
  uat_frame.cpp)
target_include_directories(skycue_iqgen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(skycue_iqgen_core PUBLIC skycued_core ZLIB::ZLIB)
skycue_set_warnings(skycue_iqgen_core)

add_executable(skycue-iqgen main.cpp)
target_link_libraries(skycue-iqgen PRIVATE skycue_iqgen_core Boost::program_options)
skycue_set_warnings(skycue-iqgen)
install(TARGETS skycue-iqgen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
