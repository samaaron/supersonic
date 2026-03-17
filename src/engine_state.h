/*
 * engine_state.h — Native engine lifecycle state
 *
 * Tracks the native engine's lifecycle as an atomic state machine.
 * Transitions drive the same lifecycle events the JS/WASM side already
 * emits: 'setup' (world ready), 'reload:start/complete' (rebuild), etc.
 *
 * On native, these are delivered as OSC: /supersonic/statechange and
 * /supersonic/setup.  The JS side uses its own event emitter and boolean
 * flags (#initialized, #initializing) rather than this enum.
 */
#pragma once

enum class EngineState {
    Booting,      // First init in progress (world being created)
    Running,      // World exists, audio callback active
    Restarting,   // Cold swap in progress (world destroyed, being rebuilt)
    Stopped,      // Audio callback stopped (device removed, shutdown)
    Error         // Device error
};

inline const char* engineStateToString(EngineState s) {
    switch (s) {
        case EngineState::Booting:    return "booting";
        case EngineState::Running:    return "running";
        case EngineState::Restarting: return "restarting";
        case EngineState::Stopped:    return "stopped";
        case EngineState::Error:      return "error";
    }
    return "unknown";
}
