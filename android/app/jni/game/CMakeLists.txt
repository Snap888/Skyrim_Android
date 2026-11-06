# The game: host runtime + recompiled code from port/runtime and port/gen.
get_filename_component(PORT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
file(GLOB GEN_SRC ${PORT}/gen/*.c)
file(GLOB RT_SRC ${PORT}/runtime/*.c)
file(GLOB RT_CXX ${PORT}/runtime/*.cpp)

add_library(lbgen OBJECT ${GEN_SRC})
set_property(TARGET lbgen PROPERTY JOB_POOL_COMPILE gen)
target_include_directories(lbgen PRIVATE ${PORT}/runtime ${PORT}/gen)
target_compile_definitions(lbgen PRIVATE GAME_SKYRIM)
option(LB_STRICT_ORDER "x86 (TSO) ordering for guest memory accesses" OFF)
if(LB_STRICT_ORDER)
  target_compile_definitions(lbgen PRIVATE LB_STRICT_ORDER)
endif()
target_compile_options(lbgen PRIVATE -std=gnu11 -O2 -mtune=cortex-a73 -g0 -fno-strict-aliasing -fwrapv -fsigned-char -w -fPIC)
file(GLOB D3DC_SRC ${PORT}/gen/x3da_*.c)
set_source_files_properties(${D3DC_SRC} PROPERTIES COMPILE_OPTIONS "-O2")

add_library(main SHARED ${RT_SRC} ${RT_CXX} $<TARGET_OBJECTS:lbgen>)
target_include_directories(main PRIVATE ${PORT}/runtime ${PORT}/gen ../sdlinc
    ../dxvk/include/native/directx ../dxvk/include/native/windows ../dxvk/include/native
    ../snapframe)
target_compile_definitions(main PRIVATE DXVK_NATIVE GAME_SKYRIM)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char -Wno-everything
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
target_link_libraries(main PRIVATE SDL2 dxvk_d3d9 snapframe_host
    SnapAI::Engine SnapFrame::Engine SnapFrame::MotionV2
    m $<$<BOOL:${ANDROID}>:log> $<$<BOOL:${ANDROID}>:android>)

# Optional CI-only ABI probe. Keep it out of normal APK builds.
option(SNAP_DXVK_LINKAGE_CHECK "Build the DXVK D3D9 ABI linkage probe" OFF)
if(SNAP_DXVK_LINKAGE_CHECK)
  add_library(dxvk_linkage_check SHARED ${PORT}/tools/ci/dxvk_linkage_check.c)
  target_link_libraries(dxvk_linkage_check PRIVATE dxvk_d3d9)
  target_compile_options(dxvk_linkage_check PRIVATE -fvisibility=default)
endif()

# Compile the Android JNI settings bridge independently of generated game
# code. This catches JNI/native API drift in CI without pretending to build an APK.
add_library(snapframe_jni_compile_check OBJECT ${PORT}/runtime/main.c ${PORT}/runtime/snapframe_bootstrap.c)
target_include_directories(snapframe_jni_compile_check PRIVATE
    ${PORT}/runtime ${PORT}/gen ../sdlinc ../snapframe)
target_compile_definitions(snapframe_jni_compile_check PRIVATE DXVK_NATIVE GAME_SKYRIM)
target_link_libraries(snapframe_jni_compile_check PRIVATE SnapAI::Engine)
target_compile_options(snapframe_jni_compile_check PRIVATE
    -O2 -fno-strict-aliasing -fwrapv -fsigned-char -Wno-everything
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)

# ---- xWMA / FFmpeg (optional) ------------------------------------------------
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg)
set(_ffmpeg_avcodec "${FFMPEG_DIR}/lib/${ANDROID_ABI}/libavcodec.so")
set(_ffmpeg_avutil  "${FFMPEG_DIR}/lib/${ANDROID_ABI}/libavutil.so")
set(_ffmpeg_hdr     "${FFMPEG_DIR}/include/libavcodec/avcodec.h")

set(_ffmpeg_ready FALSE)
if(EXISTS "${_ffmpeg_avcodec}" AND EXISTS "${_ffmpeg_avutil}" AND EXISTS "${_ffmpeg_hdr}")
  set(_ffmpeg_ready TRUE)
endif()

if(NOT DEFINED LB_HAS_FFMPEG)
  if(_ffmpeg_ready)
    set(LB_HAS_FFMPEG ON)
  else()
    set(LB_HAS_FFMPEG OFF)
  endif()
endif()
option(LB_HAS_FFMPEG "Decode xWMA voices via FFmpeg libavcodec (WMAV2)" ${LB_HAS_FFMPEG})

if(LB_HAS_FFMPEG)
  if(NOT _ffmpeg_ready)
    message(WARNING "LB_HAS_FFMPEG=ON but FFmpeg incomplete (need include/ + lib/${ANDROID_ABI}/*.so) — disabling")
    set(LB_HAS_FFMPEG OFF)
  endif()
endif()

if(LB_HAS_FFMPEG)
  message(STATUS "xWMA: FFmpeg enabled (${FFMPEG_DIR})")
  target_sources(main PRIVATE ${PORT}/runtime/xwma_ffmpeg.c)
  target_include_directories(main PRIVATE ${FFMPEG_DIR}/include)
  target_compile_definitions(main PRIVATE LB_HAS_FFMPEG)
  target_link_libraries(main PRIVATE "${_ffmpeg_avcodec}" "${_ffmpeg_avutil}")
else()
  if(EXISTS "${_ffmpeg_avcodec}" AND NOT EXISTS "${_ffmpeg_hdr}")
    message(STATUS "xWMA: FFmpeg .so present but headers missing (include/libavcodec/avcodec.h) — disabled")
  else()
    message(STATUS "xWMA: FFmpeg disabled (voices/music in xWMA play silent)")
  endif()
endif()
