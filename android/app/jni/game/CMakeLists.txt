# The game: host runtime + recompiled code from port/runtime and port/gen.
get_filename_component(PORT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
file(GLOB GEN_SRC ${PORT}/gen/*.c)
file(GLOB RT_SRC ${PORT}/runtime/*.c)
file(GLOB RT_CXX ${PORT}/runtime/*.cpp)

add_library(lbgen OBJECT ${GEN_SRC})
set_property(TARGET lbgen PROPERTY JOB_POOL_COMPILE gen)
target_include_directories(lbgen PRIVATE ${PORT}/runtime ${PORT}/gen)
target_compile_definitions(lbgen PRIVATE GAME_SKYRIM)
# -DLB_STRICT_ORDER=ON: full x86 memory ordering in the recompiled code (as 0.17-0.20; slower)
option(LB_STRICT_ORDER "x86 (TSO) ordering for guest memory accesses" OFF)
if(LB_STRICT_ORDER)
  target_compile_definitions(lbgen PRIVATE LB_STRICT_ORDER)
endif()
target_compile_options(lbgen PRIVATE -std=gnu11 -O2 -mtune=cortex-a73 -g0 -fno-strict-aliasing -fwrapv -fsigned-char -w -fPIC)
# The lifted D3DCompiler runs every runtime shader compile (on the phone's GPU
# path): build it with full optimisation. Everything is -O2 now (speed).
file(GLOB D3DC_SRC ${PORT}/gen/x3da_*.c)
set_source_files_properties(${D3DC_SRC} PROPERTIES COMPILE_OPTIONS "-O2")

add_library(main SHARED ${RT_SRC} ${RT_CXX} $<TARGET_OBJECTS:lbgen>)
target_include_directories(main PRIVATE ${PORT}/runtime ${PORT}/gen ../sdlinc
    ../dxvk/include/native/directx ../dxvk/include/native/windows ../dxvk/include/native)
target_compile_definitions(main PRIVATE DXVK_NATIVE GAME_SKYRIM)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char -Wno-everything
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
target_link_libraries(main PRIVATE SDL2 dxvk_d3d9 m $<$<BOOL:${ANDROID}>:log> $<$<BOOL:${ANDROID}>:android>)

# ---- xWMA / FFmpeg (optional) ------------------------------------------------
# Prebuilt tree from workflow artifact "ffmpeg-android-arm64-wmav2-tree":
#   android/app/jni/ffmpeg/include/...
#   android/app/jni/ffmpeg/lib/arm64-v8a/libavcodec.so
#   android/app/jni/ffmpeg/lib/arm64-v8a/libavutil.so
#
# Enable: -DLB_HAS_FFMPEG=ON  (default ON if libs exist, else OFF)
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg)
set(_ffmpeg_avcodec "${FFMPEG_DIR}/lib/${ANDROID_ABI}/libavcodec.so")
set(_ffmpeg_avutil  "${FFMPEG_DIR}/lib/${ANDROID_ABI}/libavutil.so")

if(NOT DEFINED LB_HAS_FFMPEG)
  if(EXISTS "${_ffmpeg_avcodec}" AND EXISTS "${_ffmpeg_avutil}")
    set(LB_HAS_FFMPEG ON)
  else()
    set(LB_HAS_FFMPEG OFF)
  endif()
endif()
option(LB_HAS_FFMPEG "Decode xWMA voices via FFmpeg libavcodec (WMAV2)" ${LB_HAS_FFMPEG})

if(LB_HAS_FFMPEG)
  if(NOT EXISTS "${_ffmpeg_avcodec}")
    message(FATAL_ERROR "LB_HAS_FFMPEG=ON but missing ${_ffmpeg_avcodec}. Run workflow build-ffmpeg-android and unpack artifact into jni/ffmpeg/")
  endif()
  message(STATUS "xWMA: FFmpeg enabled (${FFMPEG_DIR})")
  target_sources(main PRIVATE ${PORT}/runtime/xwma_ffmpeg.c)
  target_include_directories(main PRIVATE ${FFMPEG_DIR}/include)
  target_compile_definitions(main PRIVATE LB_HAS_FFMPEG)
  target_link_libraries(main PRIVATE "${_ffmpeg_avcodec}" "${_ffmpeg_avutil}")
else()
  message(STATUS "xWMA: FFmpeg disabled (voices/music in xWMA play silent)")
endif()
