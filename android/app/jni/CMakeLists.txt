cmake_minimum_required(VERSION 3.18)
project(skyrim C CXX)

include(FetchContent)
FetchContent_Declare(adrenotools
    GIT_REPOSITORY https://github.com/Pipetto-crypto/libadrenotools.git
    GIT_TAG 8483dfdaa2abf97ee89ad0e5f337e7b508550c6b
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(adrenotools)
set(CMAKE_CXX_STANDARD 17)
# Each recompiled file needs 1-2 GB of RAM to compile; cap parallel jobs.
set(LB_GEN_JOBS 3 CACHE STRING "parallel compiles of recompiled code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS gen=${LB_GEN_JOBS})

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL2)
add_subdirectory(dxvk)
add_subdirectory(lsfg)

# SnapFrameBackendV1: use the portable engine + DXVK adapter from Snap_AI_Engine.
# The adapter is built against this exact DXVK fork, after dxvk_d3d9 exists.
option(SNAPFRAME_VALIDATION_ONLY
    "Build the DXVK/SnapFrame native adapter without the game runtime (CI syntax/link validation)" OFF)
set(SNAP_AI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAP_AI_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_DXVK_LEGACY ON CACHE BOOL "" FORCE)
set(SNAP_FRAME_DXVK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dxvk" CACHE PATH "" FORCE)
FetchContent_Declare(snap_ai_engine
    GIT_REPOSITORY https://github.com/Snap888/Snap_AI_Engine.git
    GIT_TAG 12d89ff6b8adbb4da642912c13afec95f58bdc5e
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(snap_ai_engine)
if(NOT TARGET snap_frame_dxvk_legacy)
    message(FATAL_ERROR "SnapFrameBackendV1 DXVK adapter target was not created")
endif()
add_library(snapframe_host ALIAS snap_frame_dxvk_legacy)

if(NOT SNAPFRAME_VALIDATION_ONLY)
    add_subdirectory(game)
endif()
