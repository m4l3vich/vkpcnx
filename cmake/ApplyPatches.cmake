# Applies every *.patch found under PATCH_DIR (recursively) to SOURCE_DIR.
# A patch at PATCH_DIR/<sub/dir>/x.patch is applied inside SOURCE_DIR/<sub/dir>,
# which lets one patch set cover a project and its git submodules. Patches
# that are already applied are skipped, so re-running configure (or a failed
# earlier attempt) is safe.
#
# Usage: cmake -DSOURCE_DIR=<dir> -DPATCH_DIR=<dir> -P ApplyPatches.cmake

find_package(Git REQUIRED)

file(GLOB_RECURSE PATCHES RELATIVE "${PATCH_DIR}" "${PATCH_DIR}/*.patch")
list(SORT PATCHES)

foreach (REL ${PATCHES})
    get_filename_component(NAME ${REL} NAME)
    if (NAME MATCHES "^\\._")
        continue() # macOS AppleDouble sidecar files
    endif ()
    get_filename_component(SUBDIR ${REL} DIRECTORY)
    set(PATCH "${PATCH_DIR}/${REL}")
    set(WORKDIR "${SOURCE_DIR}/${SUBDIR}")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check --ignore-whitespace ${PATCH}
        WORKING_DIRECTORY ${WORKDIR}
        RESULT_VARIABLE ALREADY_APPLIED
        OUTPUT_QUIET ERROR_QUIET
    )
    if (ALREADY_APPLIED EQUAL 0)
        message(STATUS "patch ${REL}: already applied")
        continue()
    endif ()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace ${PATCH}
        WORKING_DIRECTORY ${WORKDIR}
        RESULT_VARIABLE RESULT
    )
    if (NOT RESULT EQUAL 0)
        message(FATAL_ERROR "patch ${REL}: failed to apply in ${WORKDIR}")
    endif ()
    message(STATUS "patch ${REL}: applied")
endforeach ()
