# Optional LSFG host library (scaffold). Full FG requires DXVK present integration.
add_library(lsfg_host STATIC lsfg_host.cpp)
target_include_directories(lsfg_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(ANDROID)
  target_link_libraries(lsfg_host PUBLIC log)
endif()
