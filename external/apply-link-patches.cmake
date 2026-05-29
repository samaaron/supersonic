# Apply the SuperSonic Link patches idempotently, run from the Link source
# dir by CMake's PATCH_COMMAND (cmake -P, so no Unix shell needed on Windows).
# Requires -DLINK_PATCH_DIR=<dir holding the .patch files>.

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED LINK_PATCH_DIR)
    message(FATAL_ERROR "LINK_PATCH_DIR not set")
endif()

set(PATCHES
    "${LINK_PATCH_DIR}/link-loopback-mode.patch"
    "${LINK_PATCH_DIR}/link-peer-enumeration.patch"
    "${LINK_PATCH_DIR}/link-monotonic-commit-timestamp.patch"
)

find_package(Git REQUIRED)

# --ignore-whitespace: git-for-Windows may check Link's source out CRLF while
# the patches are LF, so context lines won't match without it. No-op on Unix.
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --reverse --check ${PATCHES}
    RESULT_VARIABLE reverse_check
    OUTPUT_QUIET ERROR_QUIET
)
if(reverse_check EQUAL 0)
    message(STATUS "[supersonic-link-patches] already applied — skipping")
    return()
endif()

execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --check ${PATCHES}
    RESULT_VARIABLE forward_check
    OUTPUT_QUIET ERROR_QUIET
)
if(forward_check EQUAL 0)
    message(STATUS "[supersonic-link-patches] applying patches")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --whitespace=nowarn ${PATCHES}
        RESULT_VARIABLE apply_result
    )
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "[supersonic-link-patches] git apply failed")
    endif()
    return()
endif()

message(FATAL_ERROR
    "[supersonic-link-patches] patches don't apply cleanly to this Link source.\n"
    "  A patch file was likely edited while the cached abletonlink source still\n"
    "  has the old version applied. Delete the cached Link dirs (abletonlink-src,\n"
    "  abletonlink-build, abletonlink-subbuild) and re-run CMake. They live under:\n"
    "    <build>/_deps/\n"
    "  Or for sonic-pi's nested build, under:\n"
    "    <sonic-pi-build>/external/supersonic-prefix/src/supersonic-build/_deps/")
