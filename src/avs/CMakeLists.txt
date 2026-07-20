add_library(mxfsource_avs SHARED
  avs_adapter.cpp
)

target_link_libraries(mxfsource_avs PRIVATE mxf_core)
target_include_directories(mxfsource_avs PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/avisynth
)

set_target_properties(mxfsource_avs PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mxfsource"
)

if(NOT WIN32)
  set_target_properties(mxfsource_avs PROPERTIES OUTPUT_NAME "mxfsource_avs")
endif()
