# Writes OUT (a header with VKPCNX_GIT_DESCRIBE: the commit, "-dirty" for local
# changes; tags are ignored, the version comes from CMake), run on
# every build so the stamp never goes stale. The file is only rewritten when
# the value changes, so an unchanged tree doesn't recompile build_info.cpp.
#
# Sources, in order: $VKPCNX_GIT_DESCRIBE (set by build-switch.sh --docker,
# where the container's git refuses the host-owned repo), git, $GITHUB_SHA.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DOUT=<header> -P GitVersion.cmake

set(DESCRIBE "$ENV{VKPCNX_GIT_DESCRIBE}")

if (NOT DESCRIBE)
    find_package(Git QUIET)
    if (GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=12 --exclude=*
            WORKING_DIRECTORY ${SOURCE_DIR}
            OUTPUT_VARIABLE DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif ()
endif ()

if (NOT DESCRIBE AND DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 12 DESCRIBE)
endif ()

if (NOT DESCRIBE)
    set(DESCRIBE "unknown")
endif ()

set(CONTENT "#pragma once\n#define VKPCNX_GIT_DESCRIBE \"${DESCRIBE}\"\n")
if (EXISTS ${OUT})
    file(READ ${OUT} OLD)
endif ()
if (NOT "${OLD}" STREQUAL "${CONTENT}")
    file(WRITE ${OUT} "${CONTENT}")
endif ()
