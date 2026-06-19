# ─── Embedded-consumer regression guard ──────────────────────────────────────
# SuperSonic supports being consumed as a subproject (e.g. the BEAM NIF host,
# or any external embedder) via add_subdirectory() from a tree whose
# CMAKE_SOURCE_DIR is NOT the SuperSonic root. Every source path inside
# SuperSonic's CMakeLists must therefore anchor on CMAKE_CURRENT_SOURCE_DIR
# (SUPERSONIC_SRC / NATIVE_SRC), never the top-level CMAKE_SOURCE_DIR.
#
# Standalone CI builds have CMAKE_SOURCE_DIR == the SuperSonic root, so they
# can never catch a CMAKE_SOURCE_DIR/CMAKE_CURRENT_SOURCE_DIR mix-up. This is
# the only build that exercises the embedded path.
#
# Configuring IS the assertion: a CMAKE_SOURCE_DIR-anchored source path fails
# the generate step with "Cannot find source file". No build/link required —
# the bug class is pure path resolution.
#
# Synth + Link are off so the guard configures fast and JUCE-free. The paths
# under guard live in the always-compiled ENGINE_SOURCES, so synth/Link state
# is irrelevant to what this test exercises.
cmake_minimum_required(VERSION 3.24)
project(supersonic_embed_guard LANGUAGES C CXX)

set(SUPERSONIC_ENABLE_SYNTH OFF CACHE BOOL "" FORCE)
set(SUPERSONIC_ENABLE_LINK  OFF CACHE BOOL "" FORCE)

# Pull SuperSonic in by absolute path from outside its own tree, exactly as a
# real embedder does. test/embed/CMakeLists.txt -> SuperSonic root is ../../.
get_filename_component(SUPERSONIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory(${SUPERSONIC_ROOT} supersonic_build)
