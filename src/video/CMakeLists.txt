# Video: decoding video files for video tracks (frames for the video window and thumbnails).
#
#   - a built-in Motion-JPEG AVI reader (every platform, no dependencies)
#   - FFmpeg (libavformat/libavcodec/libswscale) where it is installed (Linux; optional)
#   - Windows Media Foundation on Windows (part of the OS)
add_library(dojo_video STATIC
    FfmpegSource.cpp
    FrameServer.cpp
    FrameServer.h
    MediaFoundationReader.cpp
    MediaFoundationReader.h
    MediaFoundationSource.cpp
    MjpegAvi.cpp
    MjpegAvi.h
    PlatformDecoders.h
    VideoSource.cpp
    VideoSource.h)
add_library(dojo::video ALIAS dojo_video)

target_link_libraries(dojo_video PUBLIC dojo::core dojo::juce)

set(DOJO_VIDEO_HAS_FFMPEG 0)
if(DOJO_WITH_FFMPEG AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DOJO_FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
    endif()
    if(DOJO_FFMPEG_FOUND)
        target_link_libraries(dojo_video PRIVATE PkgConfig::DOJO_FFMPEG)
        set(DOJO_VIDEO_HAS_FFMPEG 1)
        message(STATUS "Video: FFmpeg ${DOJO_FFMPEG_libavformat_VERSION} (MP4, MOV, MKV, WebM, ...)")
    else()
        message(STATUS "Video: FFmpeg not found (install libavformat-dev libavcodec-dev libswscale-dev); "
                       "video tracks read Motion-JPEG AVI files only")
    endif()
endif()
target_compile_definitions(dojo_video PUBLIC DOJO_HAS_FFMPEG=${DOJO_VIDEO_HAS_FFMPEG})

if(WIN32)
    target_link_libraries(dojo_video PRIVATE mfplat mfreadwrite mfuuid ole32 uuid)
endif()

dojo_apply_project_warnings(dojo_video)
set_target_properties(dojo_video PROPERTIES FOLDER "Dojo")
