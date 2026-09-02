cmake_minimum_required(VERSION 3.19)
project(blitzview LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------- Build metadata (git describe, dirty state, build time) --------
# Baked into a generated header so the About dialog can show which version was
# built. The header is regenerated on EVERY build run (not just configure) by
# cmake/GenerateBuildInfo.cmake — the build timestamp and the git dirty state
# must always describe the actual build. For the Windows cross-build that same
# script is run on the HOST by the Makefile (the builder image has no git)
# into ${CMAKE_BINARY_DIR}/generated/BuildInfo.h before configure runs;
# BV_USE_PREGENERATED_BUILDINFO then makes CMake use the file verbatim.

set(BV_USE_PREGENERATED_BUILDINFO OFF CACHE BOOL
    "Use the pre-generated BuildInfo.h written by the Makefile (Windows cross-build)")
if(BV_USE_PREGENERATED_BUILDINFO)
    message(STATUS "Using pre-generated BuildInfo.h written by the Makefile")
else()
    add_custom_target(GenerateBuildInfo ALL
        COMMAND ${CMAKE_COMMAND}
            -D "BV_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -D "BV_OUTPUT=${CMAKE_BINARY_DIR}/generated/BuildInfo.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake"
        VERBATIM)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets OpenGLWidgets Concurrent Core)

# Removable-device backend: UDisks2 over D-Bus on Linux, QStorageInfo elsewhere.
# Qt6::DBus is deliberately NOT a global requirement — on Windows there is no
# system bus and the dependency would be dead weight.
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    set(PLATFORM_SOURCES src/ExternalDeviceManagerUdisks.cpp)
    # `dl` backs dladdr() in QtDebugStacktrace.h. On glibc < 2.34 it lives in
    # libdl.so.2 (not libc); on glibc >= 2.34 -ldl is a harmless stub.
    set(PLATFORM_LIBS    Qt6::DBus dl)
else()
    set(PLATFORM_SOURCES src/ExternalDeviceManagerDrives.cpp)
    set(PLATFORM_LIBS    "")
endif()

set(SOURCES
    src/main.cpp
    src/AppSettings.cpp
    src/AboutDialog.cpp
    src/MainWindow.cpp
    src/DetailsPanel.cpp
    src/DirectoryPanel.cpp
    src/EditMetadataDialog.cpp
    src/ElidedLabel.cpp
    src/ExifToolService.cpp
    src/FileOps.cpp
    src/MetadataCache.cpp
    src/Path.cpp
    src/RenameDialog.cpp
    src/TagFilterDialog.cpp
    src/TagSymbols.cpp
    src/DirTree.cpp
    src/FileSystemModel.cpp
    src/MediaModel.cpp
    src/ThumbnailLoader.cpp
    src/FullscreenTransition.cpp
    src/GridDelegate.cpp
    src/GridView.cpp
    src/TableView.cpp
    src/MediaContextMenu.cpp
    src/PaddedListProxy.cpp
    src/MediaViewer.cpp
    src/ExifThumbExtractor.cpp
    src/FfmpegThumbExtractor.cpp
    src/ThumbnailCache.cpp
    src/ThumbnailDiskCache.cpp
    src/SettingsDialog.cpp
    src/SettingsPage.cpp
    src/ViewerSettingsPage.cpp
    src/InputSettingsPage.cpp
    src/TagSettingsPage.cpp
    src/CacheSettingsPage.cpp
    src/ExternalDeviceManager.cpp
    src/DeviceId.cpp
)


set(HEADERS
    src/AppSettings.h
    src/AppShutdown.h
    src/AboutDialog.h
    src/MainWindow.h
    src/DetailsPanel.h
    src/DirectoryPanel.h
    src/EditMetadataDialog.h
    src/ElidedLabel.h
    src/ExifToolService.h
    src/FileOps.h
    src/MediaMetadata.h
    src/MetadataCache.h
    src/Path.h
    src/RenameDialog.h
    src/TagFilterDialog.h
    src/TagSymbols.h
    src/DirTree.h
    src/FileSystemModel.h
    src/MediaItem.h
    src/MediaModel.h
    src/ThumbnailLoader.h
    src/FullscreenTransition.h
    src/GridDelegate.h
    src/GridView.h
    src/TableView.h
    src/MediaViewer.h
    src/ExifThumbExtractor.h
    src/FfmpegThumbExtractor.h
    src/ThumbnailCache.h
    src/ThumbnailDiskCache.h
    src/SettingsDialog.h
    src/SettingsPage.h
    src/Shortcuts.h
    src/ViewerSettingsPage.h
    src/InputSettingsPage.h
    src/TagSettingsPage.h
    src/CacheSettingsPage.h
    src/DeviceId.h
    src/ExternalDevice.h
    src/ExternalDeviceManager.h
    src/FocusFrame.h
    src/MediaContextMenu.h
    src/PaddedListProxy.h
    src/QtDebugStacktrace.h
    src/SlideTrace.h
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/FFmpegTarget.cmake)   # defines bv_ffmpeg

add_executable(blitzview ${SOURCES} ${PLATFORM_SOURCES} ${HEADERS} res/blitzview.qrc)

if(NOT BV_USE_PREGENERATED_BUILDINFO)
    add_dependencies(blitzview GenerateBuildInfo)
endif()

# WIN32_EXECUTABLE suppresses the console window on Windows; ignored elsewhere.
set_target_properties(blitzview PROPERTIES WIN32_EXECUTABLE TRUE)

target_link_libraries(blitzview PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    bv_ffmpeg
    ${PLATFORM_LIBS}
)

target_include_directories(blitzview PRIVATE src "${CMAKE_BINARY_DIR}/generated")

# Precompiled header. Qt headers dominate the build: ~40 sources plus the same
# number of AUTOMOC-generated units, each pulling in QtCore/QtGui/QtWidgets from
# scratch. Only headers that appear in most translation units belong here -- a
# large PCH becomes the bottleneck itself.
#
# NOT applied to blitzview-launcher: that one is plain C, statically linked and
# has AUTOMOC/AUTOUIC/AUTORCC off (see below).
#
# Caveat: a PCH masks missing #includes, because every unit sees these headers
# whether it includes them or not. Build with
# -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON now and then to catch that.
target_precompile_headers(blitzview PRIVATE
    <QString>
    <QStringList>
    <QList>
    <QHash>
    <QSet>
    <QSize>
    <QDir>
    <QFileInfo>
    <QTimer>
    <QDebug>
    <QPainter>
    <QPixmap>
    <QWidget>
    <QLabel>
    <QApplication>
)

target_compile_definitions(blitzview PRIVATE QT_USE_QSTRINGBUILDER)

# ---------------------------------------------------------------------------
# Portable-package launcher (Windows only). A tiny statically linked shim that
# starts app\blitzview.exe, so the user sees one clean BlitzView.exe instead of
# 60 MB of DLLs. See winbuild/launcher.c.
# ---------------------------------------------------------------------------
if(MINGW)
    set(LAUNCHER_SOURCES winbuild/launcher.c)
    # The icon is generated from res/BlitzViewIcon.png by the Makefile into the
    # build directory (build/windows/icon.ico) and is not checked in; without
    # it the launcher still builds, just plain.
    if(EXISTS "${CMAKE_BINARY_DIR}/icon.ico")
        list(APPEND LAUNCHER_SOURCES winbuild/launcher.rc)
    else()
        message(STATUS "icon.ico missing - launcher gets no icon")
    endif()

    add_executable(blitzview-launcher WIN32 ${LAUNCHER_SOURCES})
    # NOT named BlitzView.exe here: Wine's view of the filesystem is
    # case-insensitive, so BlitzView.exe and blitzview.exe would be the same
    # file to Ninja, which then considers the launcher already built and
    # silently produces nothing. deploy.sh does the rename when it copies.
    set_target_properties(blitzview-launcher PROPERTIES
        # Plain C, no Qt — the automatic Qt steps have nothing to do here.
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    # -municode: the entry point is wWinMain (wide, for non-Latin paths);
    # without it MinGW's startup code looks for WinMain and fails to link.
    # -static: the launcher must not need the very DLLs it hides away in app/.
    target_compile_options(blitzview-launcher PRIVATE -municode)
    target_link_options(blitzview-launcher PRIVATE -municode -static)
    target_link_libraries(blitzview-launcher PRIVATE shlwapi)
    # windres resolves "icon.ico" relative to the .rc file's directory, so the
    # build directory (where the generated icon.ico lives) is added to its path.
    set_source_files_properties(winbuild/launcher.rc PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_BINARY_DIR}")
endif()

if(WIN32)
    # windows.h defines min/max as macros and pulls in a great deal besides;
    # both collide with FFmpeg headers and the standard library.
    target_compile_definitions(blitzview PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # DbgHelp backs the stack trace in QtDebugStacktrace.h.
    target_link_libraries(blitzview PRIVATE dbghelp)
endif()

# ---------------------------------------------------------------------------
# macOS app bundle. Unlike Windows there is no separate launcher target: the
# .app bundle itself is the single user-visible entry point, so OUTPUT_NAME
# is set to the mixed-case product name directly (Wine's case-insensitivity
# problem that forces blitzview-launcher.exe's rename dance does not apply
# here). CMake generates Info.plist from its built-in template using the
# MACOSX_BUNDLE_* properties below -- no checked-in template needed.
# ---------------------------------------------------------------------------
if(APPLE)
    set_target_properties(blitzview PROPERTIES
        OUTPUT_NAME "BlitzView"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "BlitzView"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.blitzview.blitzview"
        MACOSX_BUNDLE_ICON_FILE "BlitzView.icns")

    # The icon is generated from res/BlitzViewIcon-macos.png by the Makefile
    # into the build directory (build/macos/BlitzView.icns) and is not
    # checked in; same "cosmetic asset must never break a build" rule as
    # winbuild/icon.ico. The source file's basename here MUST match
    # MACOSX_BUNDLE_ICON_FILE above -- MACOSX_PACKAGE_LOCATION copies it into
    # Resources under its own name, not under CFBundleIconFile's name, so a
    # mismatch silently produces a bundle with no visible icon.
    if(EXISTS "${CMAKE_BINARY_DIR}/BlitzView.icns")
        target_sources(blitzview PRIVATE "${CMAKE_BINARY_DIR}/BlitzView.icns")
        set_source_files_properties("${CMAKE_BINARY_DIR}/BlitzView.icns" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    else()
        message(STATUS "BlitzView.icns missing - app bundle gets no icon")
    endif()
endif()
