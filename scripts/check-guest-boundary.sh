#!/bin/sh
# check-guest-boundary.sh — the scsynth guest reaches clockwork through
# dsp_api.h and nothing else.
#
# What a guest may include of clockwork's: dsp_api.h, the sink header it
# pulls in, and clockwork_clock_state.h to read DspConfig::clock. What it may
# not: the arena layout (shared_memory.h and the shm_* headers), the heap, the
# memory profile, the scope rings, the device layer, or any clockwork global
# reached by an extern declaration. Every one of those is a breakage waiting
# for the day clockwork's layout moves — which its README says it will.
#
# Run from the repository root, or from CTest (test/native/CMakeLists.txt).
# Exit 0 when the guest is clean; otherwise the offending lines, and 1.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUEST="$ROOT/dsp/scsynth"

# Headers the guest may take from clockwork.
ALLOWED='dsp_api\.h|clockwork_event_sink\.h|clockwork_clock_state\.h'
# Headers that are clockwork's and not the boundary.
FORBIDDEN_INCLUDES='shared_memory\.h|shm_[a-z_]+\.(h|hpp)|clockwork_[a-z_]+\.(h|hpp)|mem_region\.h|memory_profile\.h|audio_config\.h|audio_processor\.h|scope_streams\.h|clock/[A-Za-z_]+\.h|engine_state\.h|metrics_schema\.h|rt_alloc\.h|clockwork/'
# Symbols that are clockwork's own, reached by an extern declaration or a call.
FORBIDDEN_SYMBOLS='clockwork_log|clockwork_heap_|clockwork_scope_|clockwork::mem|get_shared_memory_base|\bshared_memory\b|clockwork_shm_base|g_engine_frames|NATIVE_STAT|SHM_SCOPE|SHM_WINDOW|GUEST_CONFIG_START'

sources() {
    find "$GUEST" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' \) \
        -not -path "$GUEST/synth/external_libraries/*"
}

status=0
bad=$(sources | xargs grep -n -E "^[[:space:]]*#[[:space:]]*include[[:space:]]+[\"<]($FORBIDDEN_INCLUDES)" \
      | grep -v -E "[\"<]($ALLOWED)[\">]")
if [ -n "$bad" ]; then
    echo "guest includes past the boundary:"; echo "$bad"; status=1
fi
bad=$(sources | xargs grep -n -E "$FORBIDDEN_SYMBOLS" | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*(//|/?\*)')
if [ -n "$bad" ]; then
    echo "guest names clockwork symbols past the boundary:"; echo "$bad"; status=1
fi
[ "$status" -eq 0 ] && echo "guest boundary: clean"
exit $status
