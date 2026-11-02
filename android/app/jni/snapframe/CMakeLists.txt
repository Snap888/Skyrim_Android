# Experimental host scaffold. This directory is intentionally not added by
# the parent JNI CMakeLists.txt until the Vulkan backend is ready to integrate.
add_library(snapframe_host STATIC
    snapframe_host.cpp
)
target_include_directories(snapframe_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(snapframe_host PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wno-unused-parameter>"
)
