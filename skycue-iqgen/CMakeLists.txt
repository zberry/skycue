# skycue-iqgen: synthesises 1090/978 MHz IQ from recorded Mode S and UAT
# frames. skycue_iqgen_core holds the frame coding (Mode S CRC, UAT
# Reed-Solomon and framing) and the sample layer (synthesisers, CU8 writer,
# resampler) so the tests can link it.
find_package(ZLIB REQUIRED)
add_library(skycue_iqgen_core STATIC
  bits.cpp
  iq_writer.cpp
  modes_crc.cpp
  reed_solomon.cpp
  resample.cpp
  synth.cpp
  uat_frame.cpp)
target_include_directories(skycue_iqgen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(skycue_iqgen_core PUBLIC skycued_core ZLIB::ZLIB)
skycue_set_warnings(skycue_iqgen_core)
