# CMake toolchain file for Windows cross-compilation.
#
# Uses AmanoTeam's Linux-hosted MinGW GCC 16 (ELF binaries that produce
# Windows PE output). Wine is NOT used for compilation — only for Qt host
# tools (moc, uic, rcc) via CMAKE_CROSSCOMPILING_EMULATOR.
#
# Usage:
#   cmake -S . -B build \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake \
#       -DCMAKE_PREFIX_PATH=/opt/qt/6.8.3/mingw_64 \
#       -DQT_HOST_PATH=/opt/qt/6.8.3/gcc_64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ---------------------------------------------------------------------------
# Cross-compiler from AmanoTeam (Linux-hosted ELF binaries).
# GCC 16.2.1 targeting Win64. ABI-compatible with Qt 6.8.3's GCC 13.1.
# ---------------------------------------------------------------------------
set(MINGW_PREFIX /opt/mingw)

set(CMAKE_C_COMPILER   ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-windres)

# ---------------------------------------------------------------------------
# Wine runs Windows executables — only needed for:
#   - Qt host tools (moc, uic, rcc) during autogen
#   - windeployqt during deployment
#   - CMake try_run() checks
# ---------------------------------------------------------------------------
set(CMAKE_CROSSCOMPILING_EMULATOR wine)

# ---------------------------------------------------------------------------
# Search paths — MinGW CRT + Windows Qt prefix.
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH
    ${MINGW_PREFIX}/x86_64-w64-mingw32
    ${MINGW_PREFIX}/x86_64-w64-mingw32/lib
    /opt/qt/6.8.3/mingw_64
    /opt/ffmpeg)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
