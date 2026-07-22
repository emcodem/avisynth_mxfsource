add_executable(mxf_parser_tests
  smoke_test.cpp
)
target_link_libraries(mxf_parser_tests PRIVATE mxf_core)
add_test(NAME mxf_parser_smoke_test COMMAND mxf_parser_tests)

add_executable(mxf_header_parser_tests
  header_parser_test.cpp
)
target_compile_features(mxf_header_parser_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_header_parser_tests PRIVATE mxf_core)
add_test(NAME mxf_header_parser_tests COMMAND mxf_header_parser_tests)

add_executable(mxf_essence_walker_tests
  essence_walker_test.cpp
)
target_compile_features(mxf_essence_walker_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_essence_walker_tests PRIVATE mxf_core)
add_test(NAME mxf_essence_walker_tests COMMAND mxf_essence_walker_tests)

add_executable(mxf_decoder_tests
  decoder_test.cpp
)
target_compile_features(mxf_decoder_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_decoder_tests PRIVATE mxf_core)
mxfsrc_deploy_ffmpeg_runtime(mxf_decoder_tests)
add_test(NAME mxf_decoder_tests COMMAND mxf_decoder_tests)

add_executable(mxf_source_engine_tests
  source_engine_test.cpp
)
target_compile_features(mxf_source_engine_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_source_engine_tests PRIVATE mxf_core)
mxfsrc_deploy_ffmpeg_runtime(mxf_source_engine_tests)
add_test(NAME mxf_source_engine_tests COMMAND mxf_source_engine_tests)

add_executable(mxf_http_byte_source_tests
  http_byte_source_test.cpp
)
target_compile_features(mxf_http_byte_source_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_http_byte_source_tests PRIVATE mxf_core)
if(WIN32)
  target_link_libraries(mxf_http_byte_source_tests PRIVATE ws2_32)
endif()
add_test(NAME mxf_http_byte_source_tests COMMAND mxf_http_byte_source_tests)

add_executable(mxf_http_source_engine_tests
  http_source_engine_test.cpp
)
target_compile_features(mxf_http_source_engine_tests PRIVATE cxx_std_17)
target_link_libraries(mxf_http_source_engine_tests PRIVATE mxf_core)
if(WIN32)
  target_link_libraries(mxf_http_source_engine_tests PRIVATE ws2_32)
endif()
mxfsrc_deploy_ffmpeg_runtime(mxf_http_source_engine_tests)
add_test(NAME mxf_http_source_engine_tests COMMAND mxf_http_source_engine_tests)

# Manual verification tools (not part of ctest) -- see gen_test_mxf.cpp / read_test_mxf.cpp.
add_executable(gen_test_mxf EXCLUDE_FROM_ALL
  gen_test_mxf.cpp
)
target_compile_features(gen_test_mxf PRIVATE cxx_std_17)
target_link_libraries(gen_test_mxf PRIVATE mxf_core)

add_executable(read_test_mxf EXCLUDE_FROM_ALL
  read_test_mxf.cpp
)
target_compile_features(read_test_mxf PRIVATE cxx_std_17)
target_link_libraries(read_test_mxf PRIVATE mxf_core)
mxfsrc_deploy_ffmpeg_runtime(read_test_mxf)
