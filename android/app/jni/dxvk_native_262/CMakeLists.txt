# DXVK Native 2.6.2 Android ARM64 candidate.
#
# Opt-in only: this target consumes a separately checked-out upstream tree after
# tools/dxvk/apply_native_262_android_port.py has applied the Android WSI overlay.
# It deliberately does not replace the known-good native-1.9.2b target.
if(NOT ANDROID)
    message(FATAL_ERROR "DXVK Native 2.6.2 candidate is Android-only")
endif()

set(SNAP_DXVK_NATIVE_262_SOURCE_DIR "" CACHE PATH
    "Pinned DXVK v2.6.2 source checkout with Snap Android WSI overlay applied")
if(NOT IS_DIRECTORY "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/d3d9")
    message(FATAL_ERROR
        "Set SNAP_DXVK_NATIVE_262_SOURCE_DIR to the patched DXVK 2.6.2 source tree")
endif()

set(_dxvk_expected_revision "9d6f54a1ade20d1d27dd421024717a636f3d8c68")
execute_process(
    COMMAND git -C "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _dxvk_actual_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _dxvk_git_result)
if(NOT _dxvk_git_result EQUAL 0 OR NOT _dxvk_actual_revision STREQUAL _dxvk_expected_revision)
    message(FATAL_ERROR
        "DXVK source revision mismatch: expected ${_dxvk_expected_revision}, got '${_dxvk_actual_revision}'")
endif()
if(NOT EXISTS "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/wsi/sdl2/wsi_native_surface.cpp")
    message(FATAL_ERROR "DXVK Android WSI overlay is missing; run apply_native_262_android_port.py first")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(SNAP_GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DISPLAY_INFO REQUIRED IMPORTED_TARGET "libdisplay-info<0.2.0")
find_library(SNAP_VULKAN_LIBRARY vulkan REQUIRED)
find_library(SNAP_ANDROID_LIBRARY android REQUIRED)
find_library(SNAP_LOG_LIBRARY log REQUIRED)

if(NOT TARGET SDL2)
    message(FATAL_ERROR "SDL2 target must be provided by the parent Android build")
endif()

set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/dxvk-native-262-android-manifest.json")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/dxvk/generate_native_262_android_manifest.py"
        "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}" --output "${_manifest}"
    RESULT_VARIABLE _manifest_result
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT _manifest_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate DXVK 2.6.2 Android source manifest")
endif()
file(READ "${_manifest}" _manifest_json)
foreach(_group IN ITEMS d3d9 dxvk dxso util spirv vulkan wsi_common android_wsi android_wsi_overlay)
    string(JSON _count LENGTH "${_manifest_json}" groups "${_group}")
    if(_count LESS 1)
        message(FATAL_ERROR "DXVK Android source group '${_group}' is empty")
    endif()
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        string(JSON _relative GET "${_manifest_json}" groups "${_group}" ${_index})
        list(APPEND _dxvk_sources "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/${_relative}")
    endforeach()
endforeach()

# Compile upstream GLSL to the same generated C++ headers used by Meson.
set(_shader_headers)
foreach(_group IN ITEMS d3d9_shaders dxvk_shaders)
    string(JSON _count LENGTH "${_manifest_json}" groups "${_group}")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        string(JSON _relative GET "${_manifest_json}" groups "${_group}" ${_index})
        get_filename_component(_shader "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/${_relative}" ABSOLUTE)
        get_filename_component(_shader_name "${_shader}" NAME)
        string(REGEX REPLACE "\\.(comp|frag|vert|geom)$" "" _shader_stem "${_shader_name}")
        set(_header "${CMAKE_CURRENT_BINARY_DIR}/generated/${_shader_stem}.h")
        add_custom_command(
            OUTPUT "${_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND "${SNAP_GLSLANG_VALIDATOR}" --quiet --target-env vulkan1.3
                --vn "${_shader_stem}" "${_shader}" -o "${_header}"
            DEPENDS "${_shader}"
            VERBATIM)
        list(APPEND _shader_headers "${_header}")
    endforeach()
endforeach()

set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated_dir}")
set(BUILD_TARGET "aarch64")
set(BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID}")
set(BUILD_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
configure_file(
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/buildenv.h.in"
    "${_generated_dir}/buildenv.h" @ONLY)
execute_process(
    COMMAND git -C "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}" describe --dirty=+
    OUTPUT_VARIABLE VCS_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _describe_result)
if(NOT _describe_result EQUAL 0)
    set(VCS_TAG "v2.6.2-android")
endif()
configure_file(
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/version.h.in"
    "${_generated_dir}/version.h" @ONLY)

add_library(dxvk_d3d9_262 SHARED ${_dxvk_sources} ${_shader_headers})
target_include_directories(dxvk_d3d9_262 PRIVATE
    "${_generated_dir}"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include/native"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include/native/windows"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include/native/directx"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include/vulkan/include"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/include/spirv/include"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/util"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/dxvk"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/d3d9"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/dxso"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/wsi"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/spirv"
    "${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/vulkan")
target_compile_features(dxvk_d3d9_262 PRIVATE cxx_std_17)
target_compile_definitions(dxvk_d3d9_262 PRIVATE
    NOMINMAX _WIN32_WINNT=0xa00 DXVK_NATIVE DXVK_WSI_SDL2
    DXVK_PLATFORM_LINUX _FILE_OFFSET_BITS=64)
target_compile_options(dxvk_d3d9_262 PRIVATE
    -O2 -fvisibility=default
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>"
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstring>"
    -Wno-deprecated-declarations)
target_link_options(dxvk_d3d9_262 PRIVATE
    "-Wl,--version-script=${SNAP_DXVK_NATIVE_262_SOURCE_DIR}/src/d3d9/d3d9.sym")
target_link_libraries(dxvk_d3d9_262 PRIVATE
    SDL2 PkgConfig::DISPLAY_INFO "${SNAP_VULKAN_LIBRARY}"
    "${SNAP_ANDROID_LIBRARY}" "${SNAP_LOG_LIBRARY}")
set_target_properties(dxvk_d3d9_262 PROPERTIES
    OUTPUT_NAME "dxvk_d3d9_262"
    VERSION "0.2602.0"
    SOVERSION "0")
message(STATUS "DXVK Native 2.6.2 candidate enabled (source revision ${_dxvk_actual_revision})")
