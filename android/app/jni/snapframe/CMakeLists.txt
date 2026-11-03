# SnapFrame DXVK capture host and GPU preprocessing/motion/midpoint compute pipeline.
add_library(snapframe_host STATIC
    snapframe_host.cpp
)

# Compile SnapFrame GLSL on the build host before the NDK compiles the C++
# host. The Android toolchain itself does not need glslang or spirv-val.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(SNAPFRAME_GLSLANG_VALIDATOR glslangValidator)
find_program(SNAPFRAME_SPIRV_VAL spirv-val)
set(SNAPFRAME_GENERATED_SHADER_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/generated/snapframe_shaders.h")
if(Python3_Interpreter_FOUND AND SNAPFRAME_GLSLANG_VALIDATOR AND SNAPFRAME_SPIRV_VAL)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/compile_snapframe_shaders.py"
      --glslang "${SNAPFRAME_GLSLANG_VALIDATOR}"
      --spirv-val "${SNAPFRAME_SPIRV_VAL}"
    RESULT_VARIABLE SNAPFRAME_SHADER_RESULT
    OUTPUT_VARIABLE SNAPFRAME_SHADER_OUTPUT
    ERROR_VARIABLE SNAPFRAME_SHADER_ERROR
  )
  if(SNAPFRAME_SHADER_RESULT EQUAL 0)
    message(STATUS "${SNAPFRAME_SHADER_OUTPUT}")
  else()
    file(REMOVE "${SNAPFRAME_GENERATED_SHADER_HEADER}")
    message(WARNING "SnapFrame shader compilation failed; capture remains available but SnapFrame compute dispatches are disabled: ${SNAPFRAME_SHADER_ERROR}")
  endif()
else()
  file(REMOVE "${SNAPFRAME_GENERATED_SHADER_HEADER}")
  message(WARNING "SnapFrame shader tools not found on build host (Python3, glslangValidator, spirv-val); capture remains available but luma dispatch is disabled.")
endif()
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
