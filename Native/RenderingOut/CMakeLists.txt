cmake_minimum_required(VERSION 3.21)

project(RenderingOut LANGUAGES C)

# The Windows build of RenderingOut (D3D11 zero-copy) lives in
# re-poem/RenderingOut and ships as Assets/Plugins/x86_64/RenderingOut.dll.
# This portable build is for macOS and Linux.
if(WIN32)
    message(FATAL_ERROR "Use the D3D11 RenderingOut build on Windows.")
endif()

set(FFMPEG_PREFIX "" CACHE PATH
    "FFmpeg install prefix (include/ + lib/), e.g. from build-macos.sh or $(brew --prefix ffmpeg)")
option(RENDERINGOUT_STATIC_FFMPEG
    "Link FFmpeg (and x264) statically into the plugin" OFF)
option(RENDERINGOUT_BUILD_TESTS "Build the encode_test harness" OFF)

if(NOT EXISTS "${FFMPEG_PREFIX}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg headers not found; set FFMPEG_PREFIX.")
endif()

if(RENDERINGOUT_STATIC_FFMPEG)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

set(FFMPEG_LIBRARIES "")
# Static archives must be listed dependents-first.
foreach(component avformat avcodec swscale swresample avutil)
    find_library(FFMPEG_${component}_LIBRARY ${component}
        PATHS "${FFMPEG_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${component}_LIBRARY}")
endforeach()

if(RENDERINGOUT_STATIC_FFMPEG)
    find_library(X264_LIBRARY x264
        PATHS "${FFMPEG_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    list(APPEND FFMPEG_LIBRARIES "${X264_LIBRARY}")
    if(APPLE)
        list(APPEND FFMPEG_LIBRARIES
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
            "-framework CoreServices")
    else()
        list(APPEND FFMPEG_LIBRARIES m dl)
    endif()
endif()

find_package(Threads REQUIRED)

add_library(RenderingOut SHARED
    src/RenderingOut.c
    src/RenderingOut.h)

target_compile_features(RenderingOut PRIVATE c_std_11)
target_include_directories(RenderingOut PRIVATE "${FFMPEG_PREFIX}/include")
target_link_libraries(RenderingOut PRIVATE ${FFMPEG_LIBRARIES} Threads::Threads)
set_target_properties(RenderingOut PROPERTIES
    C_VISIBILITY_PRESET hidden
    INSTALL_NAME_DIR "@rpath")

# Only the plugin API is exported; statically linked FFmpeg stays private.
if(APPLE)
    target_link_options(RenderingOut PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/exports.txt"
        "LINKER:-dead_strip")
else()
    target_link_options(RenderingOut PRIVATE "LINKER:--exclude-libs,ALL")
endif()

if(RENDERINGOUT_BUILD_TESTS)
    add_executable(encode_test test/encode_test.c)
    target_link_libraries(encode_test PRIVATE RenderingOut m)
    target_include_directories(encode_test PRIVATE src)
endif()
