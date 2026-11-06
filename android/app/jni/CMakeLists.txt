cmake_minimum_required(VERSION 3.18)
project(skyrim C CXX)

include(FetchContent)
FetchContent_Declare(adrenotools
    GIT_REPOSITORY https://github.com/Pipetto-crypto/libadrenotools.git
    GIT_TAG 8483dfdaa2abf97ee89ad0e5f337e7b508550c6b
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(adrenotools)

# Portable Snap AI + SnapFrame cores. Pin to the reviewed SnapMotionV2 + adaptive Flow Scale
# commit rather than tracking a moving branch. DXVK-specific GPU work remains
# in the Skyrim adapter; the portable libraries do not own Vulkan resources.
set(SNAP_AI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAP_AI_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SNAP_FRAME_BUILD_DXVK_LEGACY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build Snap AI and SnapFrame as separate shared libraries" FORCE)
FetchContent_Declare(snap_ai_engine
    GIT_REPOSITORY https://github.com/Snap888/Snap_AI_Engine.git
    GIT_TAG 9621c32981ad818d2ff3c01dcecaa127da107c64
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(snap_ai_engine)

set(CMAKE_CXX_STANDARD 17)
# Each recompiled file needs 1-2 GB of RAM to compile; cap parallel jobs.
set(LB_GEN_JOBS 3 CACHE STRING "parallel compiles of recompiled code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS gen=${LB_GEN_JOBS})

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL2)
add_subdirectory(dxvk)
# SnapFrame is the sole frame-generation backend.
add_subdirectory(snapframe)
add_subdirectory(game)
