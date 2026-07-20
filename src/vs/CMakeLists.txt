add_library(mxfsource_vs SHARED
  vs_adapter.cpp
)

target_link_libraries(mxfsource_vs PRIVATE mxf_core)
target_include_directories(mxfsource_vs PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/vapoursynth
)

set_target_properties(mxfsource_vs PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mxfsource_vs"
)
