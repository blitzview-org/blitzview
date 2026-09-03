# Combined builder image for BlitzView — both Windows and Linux portable builds.
#
# AlmaLinux 8 (glibc 2.28): the oldest glibc reachable with Qt 6.8.3 prebuilts
# (Qt docs: supported on RHEL 8.10+). This gives the Linux portable package the
# broadest possible glibc floor without bundling glibc (which would break
# NSS/PAM/threading).
#
# This image contains:
#   - AmanoTeam MinGW GCC 16 cross-compiler (Linux-hosted ELF binaries)
#   - Wine 6.0.2 (EPEL) — runs Windows Qt tools: windeployqt, moc, uic, rcc
#   - Qt 6.8.3 for BOTH platforms (Linux native + Windows target)
#   - FFmpeg 7.1.1 headers (matching Qt's bundled FFmpeg)
#   - kimageformats plugins for both platforms (XCF, PSD, TGA, QOI, ...)
#   - CMake >= 3.28 (via pip — needed for CMAKE_CROSSCOMPILING_EMULATOR)
#   - Xvfb for headless smoke testing
#
# The Windows build is a CROSS-COMPILE: the Linux-hosted MinGW compiler
# produces PE binaries directly. Wine is only used for Qt host tools
# (moc, uic, rcc) and windeployqt, NOT for compilation.

FROM docker.io/library/almalinux:8

# ---------------------------------------------------------------------------
# 1. Base system: EPEL + CRB + Wine + Python 3.9
# ---------------------------------------------------------------------------
RUN dnf install -y epel-release && \
    dnf install -y 'dnf-command(config-manager)' && \
    dnf config-manager --set-enabled powertools && \
    dnf module enable python39 -y && \
    dnf install -y \
        ca-certificates curl xz tar file \
        python39 python39-pip \
        gcc-toolset-12 gcc-toolset-12-gcc gcc-toolset-12-gcc-c++ \
        make pkg-config \
        wine \
        # Linux build deps (X11/OpenGL/DBus)
        libX11-devel libxcb-devel libxkbcommon-devel \
        fontconfig-devel freetype-devel \
        dbus-devel mesa-libGL-devel mesa-libEGL-devel \
        pulseaudio-libs-devel alsa-lib-devel \
        # Qt xcb platform plugin runtime deps (bundled into the portable package)
        xcb-util xcb-util-wm xcb-util-image xcb-util-keysyms \
        xcb-util-renderutil xcb-util-cursor libxkbcommon-x11 \
        # Smoke test tools
        xorg-x11-server-Xvfb xorg-x11-utils \
        && dnf clean all

# ---------------------------------------------------------------------------
# 2. Wine prefix initialization
# ---------------------------------------------------------------------------
RUN wineboot --init 2>/dev/null; wineserver --wait 2>/dev/null || true

# ---------------------------------------------------------------------------
# 3. CMake >= 3.28 + Ninja via pip (distro CMake 3.26 is insufficient for
#    cross+emulator). Installed BEFORE deps.sh, which uses cmake to build
#    kimageformats.
# ---------------------------------------------------------------------------
RUN python3.9 -m pip install --no-cache-dir cmake==3.31.4 ninja && \
    cmake --version | head -1 && \
    ninja --version

# ---------------------------------------------------------------------------
# 4. aqtinstall — download Qt for both platforms, then build kimageformats
# ---------------------------------------------------------------------------
RUN python3.9 -m pip install --no-cache-dir aqtinstall==3.3.0

COPY docker/deps.env /usr/src/deps.env
COPY docker/deps.sh  /usr/src/deps.sh
RUN sh /usr/src/deps.sh && rm -rf /root/.cache

# Cross-tools (dlltool, objdump, windres) must be on PATH for the scripts below.
ENV QT_VERSION=6.8.3 \
    PATH=/opt/mingw/bin:$PATH

# ---------------------------------------------------------------------------
# 5. Import libraries for FFmpeg DLLs (Windows link step)
# ---------------------------------------------------------------------------
COPY docker/importlibs.sh /usr/src/importlibs.sh
RUN sh /usr/src/importlibs.sh

# ---------------------------------------------------------------------------
# Deliberately NOT in this image: the build/deploy scripts (winbuild/,
# linuxbuild/) and the MinGW toolchain file. They are read from the
# bind-mounted project at /project instead.
#
# The reason is the image cache. CI keys the cached image on a hash of this
# Dockerfile plus docker/deps.* only -- so anything COPYed in from elsewhere
# would go stale silently: an edited deploy.sh would still hit the cache and
# the container would keep running last week's copy. Adding those files to
# the key instead would mean a full ~4 GB rebuild (Qt download and all) for
# every one-line script edit.
#
# Keeping the image to the toolchain alone makes the cache key complete by
# construction: everything in here is covered by it.
# ---------------------------------------------------------------------------
