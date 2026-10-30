add_library(lsfg_host STATIC
    lsfg_host.cpp
    lsfg_extract.cpp
    lsfg_log.cpp
    lsfg_translate.cpp
    lsfg_present.cpp
    lsfg_step3d.cpp
    lsfg_step3e.cpp
    lsfg_step3g.cpp
    lsfg_step3gb.cpp
    lsfg_step3h.cpp
    lsfg_step3hb.cpp
    lsfg_step3hc.cpp
    lsfg_step3hd.cpp
    lsfg_step3he.cpp
    lsfg_step3hf.cpp
    lsfg_step3hg.cpp
    dxbc_options_stub.cpp
)
target_include_directories(lsfg_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(lsfg_host PRIVATE
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
target_compile_definitions(lsfg_host PRIVATE NOMINMAX DXVK_NATIVE _FILE_OFFSET_BITS=64)
target_compile_options(lsfg_host PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>"
    -Wno-everything
)
target_link_libraries(lsfg_host PUBLIC dxvk_d3d9)
if(ANDROID)
  target_link_libraries(lsfg_host PUBLIC log vulkan)
endif()
