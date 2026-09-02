# Defines the interface target `bv_ffmpeg`, which BlitzView links against for
# its direct FFmpeg use (video thumbnail extraction, see FfmpegThumbExtractor).
#
# Dev builds (Linux): pkg-config finds the system FFmpeg. No variables needed.
#
# Portable builds (Linux + Windows + macOS): Qt 6.8 ships its own FFmpeg
# .so/.dll/.dylib (avcodec-61, avformat-61, avutil-59, swscale-8,
# swresample-5 = FFmpeg 7.x), but provides no headers. The build sets:
#   FFMPEG_INCLUDE_DIR — upstream FFmpeg 7.x headers (see docker/deps.sh,
#                        macbuild/fetch-ffmpeg-headers.sh)
#   FFMPEG_LIB_DIR     — Qt's bundled .so/.dll/.dylib directory
# This keeps exactly one FFmpeg in the package, matching Qt Multimedia.
#
# The non-Windows portable branch below (find_library with plain NAMES) is
# shared by Linux and macOS: find_library appends the platform's native
# prefix/suffix (lib*.so on Linux, lib*.dylib on macOS) automatically.

add_library(bv_ffmpeg INTERFACE)

if(FFMPEG_INCLUDE_DIR AND FFMPEG_LIB_DIR)
    # Portable build: explicit include + lib dirs.
    # Works on any platform (Linux or Windows).
    target_include_directories(bv_ffmpeg INTERFACE "${FFMPEG_INCLUDE_DIR}")

    if(WIN32)
        foreach(_lib avformat avcodec swscale avutil)
            find_library(FFMPEG_${_lib}_LIB
                NAMES ${_lib} lib${_lib} ${_lib}.dll lib${_lib}.dll.a
                HINTS "${FFMPEG_LIB_DIR}"
                REQUIRED NO_DEFAULT_PATH)
            target_link_libraries(bv_ffmpeg INTERFACE "${FFMPEG_${_lib}_LIB}")
        endforeach()
    else()
        foreach(_lib avformat avcodec swscale avutil)
            find_library(FFMPEG_${_lib}_LIB
                NAMES ${_lib} lib${_lib}
                HINTS "${FFMPEG_LIB_DIR}"
                REQUIRED NO_DEFAULT_PATH)
            target_link_libraries(bv_ffmpeg INTERFACE "${FFMPEG_${_lib}_LIB}")
        endforeach()
    endif()
elseif(NOT WIN32)
    # Dev build: use system FFmpeg via pkg-config.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libswscale
        libavutil
    )
    target_link_libraries(bv_ffmpeg INTERFACE PkgConfig::FFMPEG)
else()
    message(FATAL_ERROR
        "FFMPEG_INCLUDE_DIR and FFMPEG_LIB_DIR must be set for the Windows build. "
        "See docker/deps.env and winbuild/build.sh.")
endif()
