# Regenerates ${BV_OUTPUT} (BuildInfo.h) with the build's version metadata.
#
# Runs on EVERY build, not just at configure time, so the build timestamp and
# the git dirty state always describe the actual build. This is the single
# piece of generation logic for both platforms:
#   - native builds: CMakeLists.txt's GenerateBuildInfo custom target calls it
#   - Windows cross-build: the Makefile calls it on the HOST (the builder
#     image has no git) before the container runs; CMake then uses the file
#     verbatim via BV_USE_PREGENERATED_BUILDINFO
#
# Versioning scheme: `git describe --tags --always --long --dirty --match
# 'v*'`. The --match restricts describe to version tags (v0.0.0, v1.2.3, ...)
# -- without it, describe would also match operational tags like the CI's
# rolling `latest-dev` tag and produce
# nonsense identities such as "latest-dev-1-gabc1234". The repo carries a
# genesis tag (v0.0.0) on its first commit, so this always returns the long
# form "<tag>-<N>-g<hash>[-dirty]" -- there is no "no tag yet" case to
# handle. N == 0 and clean means HEAD IS a tagged release; anything else is
# a development build. BLITZVIEW_IDENTITY (portable archive name) is exactly
# this describe string, so the filename and the About dialog version text
# are always the same characters.
#
# Usage:
#   cmake -D BV_SOURCE_DIR=<repo root> -D BV_OUTPUT=<abs path>/BuildInfo.h \
#         -P GenerateBuildInfo.cmake

if(NOT DEFINED BV_SOURCE_DIR OR NOT DEFINED BV_OUTPUT)
    message(FATAL_ERROR "GenerateBuildInfo.cmake requires BV_SOURCE_DIR and BV_OUTPUT")
endif()

set(BLITZVIEW_GIT_DESCRIBE "")
set(BLITZVIEW_GIT_DATE "")
set(BLITZVIEW_GIT_DIRTY "")
set(BLITZVIEW_GIT_TAG "")
set(BLITZVIEW_GIT_DISTANCE "")
set(BLITZVIEW_GIT_HASH "")

# All git fields stay empty when git (or a .git directory) is unavailable --
# BLITZVIEW_IDENTITY then falls back to "Unknown". "Dirty" means tracked
# files differ from HEAD (staged or unstaged) -- purely new/untracked files
# must NOT mark the build dirty, otherwise local scratch files would forever
# show "uncommitted changes". The commit date is the committer's own local
# date (git --date=format renders the committer's embedded timezone, no
# conversion).
find_program(BV_GIT git)
if(BV_GIT AND EXISTS "${BV_SOURCE_DIR}/.git")
    execute_process(COMMAND ${BV_GIT} describe --tags --always --long --dirty --match "v*"
        WORKING_DIRECTORY "${BV_SOURCE_DIR}"
        OUTPUT_VARIABLE BV_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(COMMAND ${BV_GIT} show -s --format=%cd --date=format:%Y-%m-%d HEAD
        WORKING_DIRECTORY "${BV_SOURCE_DIR}"
        OUTPUT_VARIABLE BV_DATE OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(BV_DESCRIBE)
        set(BLITZVIEW_GIT_DESCRIBE "${BV_DESCRIBE}")
        if(BV_DESCRIBE MATCHES "-dirty$")
            set(BLITZVIEW_GIT_DIRTY "1")
        endif()
        # Long-form describe: <tag>-<N>-g<hash>, optionally with a -dirty
        # suffix already stripped by ERROR_QUIET-free matching above.
        if(BV_DESCRIBE MATCHES "^(.*)-([0-9]+)-g([0-9a-f]+)(-dirty)?$")
            set(BLITZVIEW_GIT_TAG "${CMAKE_MATCH_1}")
            set(BLITZVIEW_GIT_DISTANCE "${CMAKE_MATCH_2}")
            set(BLITZVIEW_GIT_HASH "${CMAKE_MATCH_3}")
        endif()
    endif()
    if(BV_DATE)
        set(BLITZVIEW_GIT_DATE "${BV_DATE}")
    endif()
endif()

# Local wall clock with minutes. For a dirty build the About dialog shows this
# as the build moment (there is no commit of its own to date yet).
string(TIMESTAMP BLITZVIEW_BUILD_TIME "%Y-%m-%d %H:%M")

# ---------------------------------------------------------------------------
# Two human-facing strings are built HERE, in this one place, so the About
# dialog and the portable archive name can never drift apart.
#
#   BLITZVIEW_IDENTITY — the raw `git describe` output verbatim (e.g.
#      "v0.3.0", "v0.3.0-15-gabc1234", "v0.3.0-15-gabc1234-dirty"). Used as
#      the portable archive name. Written to identity.txt where
#      the Makefile reads it back for the file name. "Unknown" when git
#      metadata isn't available at all (e.g. a source tarball without .git).
#
#   BLITZVIEW_VERSION_LINE — for the About dialog. "Version <tag> (<date>)"
#      when HEAD is exactly on a tag and clean; otherwise "Development build
#      <identity> (<date>)", where <date> is the commit date when clean or
#      the build timestamp (with time) when dirty.
# ---------------------------------------------------------------------------
set(BLITZVIEW_IS_RELEASE FALSE)
if(BLITZVIEW_GIT_TAG AND BLITZVIEW_GIT_DISTANCE STREQUAL "0" AND NOT BLITZVIEW_GIT_DIRTY)
    set(BLITZVIEW_IS_RELEASE TRUE)
endif()

if(BLITZVIEW_IS_RELEASE)
    # git describe --long always appends "-0-g<hash>" even on an exact tag;
    # strip that back off so the identity/filename is the bare tag, matching
    # what the About dialog shows ("Version v0.4.0", not "v0.4.0-0-g...").
    set(BLITZVIEW_IDENTITY "${BLITZVIEW_GIT_TAG}")
elseif(BLITZVIEW_GIT_DESCRIBE)
    set(BLITZVIEW_IDENTITY "${BLITZVIEW_GIT_DESCRIBE}")
else()
    set(BLITZVIEW_IDENTITY "Unknown")
endif()

if(BLITZVIEW_GIT_DIRTY)
    set(BLITZVIEW_VERSION_DATE "${BLITZVIEW_BUILD_TIME}")
else()
    set(BLITZVIEW_VERSION_DATE "${BLITZVIEW_GIT_DATE}")
endif()

if(BLITZVIEW_IS_RELEASE)
    set(BLITZVIEW_VERSION_LINE "Version ${BLITZVIEW_GIT_TAG} (${BLITZVIEW_VERSION_DATE})")
elseif(NOT BLITZVIEW_VERSION_DATE STREQUAL "")
    set(BLITZVIEW_VERSION_LINE "Development build ${BLITZVIEW_IDENTITY} (${BLITZVIEW_VERSION_DATE})")
else()
    set(BLITZVIEW_VERSION_LINE "Development build ${BLITZVIEW_IDENTITY}")
endif()

get_filename_component(BV_OUTPUT_DIR "${BV_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${BV_OUTPUT_DIR}")

file(WRITE "${BV_OUTPUT}"
    "#pragma once\n"
    "\n"
    "#define BLITZVIEW_GIT_DESCRIBE   \"${BLITZVIEW_GIT_DESCRIBE}\"\n"
    "#define BLITZVIEW_GIT_TAG        \"${BLITZVIEW_GIT_TAG}\"\n"
    "#define BLITZVIEW_GIT_DISTANCE   \"${BLITZVIEW_GIT_DISTANCE}\"\n"
    "#define BLITZVIEW_GIT_HASH       \"${BLITZVIEW_GIT_HASH}\"\n"
    "#define BLITZVIEW_GIT_DATE       \"${BLITZVIEW_GIT_DATE}\"\n"
    "#define BLITZVIEW_BUILD_TIME     \"${BLITZVIEW_BUILD_TIME}\"\n"
    "#define BLITZVIEW_GIT_DIRTY      \"${BLITZVIEW_GIT_DIRTY}\"\n"
    "#define BLITZVIEW_IDENTITY       \"${BLITZVIEW_IDENTITY}\"\n"
    "#define BLITZVIEW_VERSION_LINE   \"${BLITZVIEW_VERSION_LINE}\"\n")

# Cheap way for the shell (the Makefile) to reuse the exact same identity
# string for the portable archive name without reimplementing this logic.
file(WRITE "${BV_OUTPUT_DIR}/identity.txt" "${BLITZVIEW_IDENTITY}")