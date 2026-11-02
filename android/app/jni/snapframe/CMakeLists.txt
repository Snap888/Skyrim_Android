# Experimental SnapFrame host + DXVK capture prototype.
add_library(snapframe_host STATIC
    snapframe_host.cpp
)
target_include_directories(snapframe_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(snapframe_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/include/native
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/include/native/windows
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/include/native/directx
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src/util
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src/dxvk
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src/dxbc
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src/spirv
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/src/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/../dxvk/generated
)
target_compile_definitions(snapframe_host PRIVATE NOMINMAX DXVK_NATIVE _FILE_OFFSET_BITS=64)
target_compile_options(snapframe_host PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>"
    -Wno-everything
)
target_link_libraries(snapframe_host PUBLIC dxvk_d3d9)
if(ANDROID)
  target_link_libraries(snapframe_host PUBLIC log vulkan)
endif()
