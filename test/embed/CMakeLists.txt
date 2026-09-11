# ─── Embedded-consumer regression guard ──────────────────────────────────────
# SuperSonic is consumed as a subproject (Sonic Pi builds it this way, and so
# can any external embedder) via add_subdirectory() from a tree whose
# CMAKE_SOURCE_DIR is NOT the SuperSonic root. Every source path inside
# SuperSonic's CMakeLists — and clockwork's, and the test suite's — must
# therefore anchor on CMAKE_CURRENT_SOURCE_DIR, never the top-level
# CMAKE_SOURCE_DIR.
#
# Standalone CI builds have CMAKE_SOURCE_DIR == the SuperSonic root, so they
# can never catch a CMAKE_SOURCE_DIR/CMAKE_CURRENT_SOURCE_DIR mix-up. This is
# the only build that exercises the embedded path.
#
# Configuring IS the assertion: a CMAKE_SOURCE_DIR-anchored source path fails
# the generate step with "Cannot find source file". No build/link required —
# the bug class is pure path resolution.
#
# Plugin hosting is off so the guard does not fetch the CLAP/VST3 SDKs; the
# paths under guard are the engine's, the host's and the test suite's.
cmake_minimum_required(VERSION 3.24)
project(supersonic_embed_guard LANGUAGES C CXX)

set(CLOCKWORK_PLUGINS OFF CACHE BOOL "" FORCE)

# Pull SuperSonic in by absolute path from outside its own tree, exactly as a
# real embedder does. test/embed/CMakeLists.txt -> SuperSonic root is ../../.
get_filename_component(SUPERSONIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory(${SUPERSONIC_ROOT} supersonic_build)
