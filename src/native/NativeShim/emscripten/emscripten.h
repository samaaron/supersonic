/*
 * Native stub for emscripten/emscripten.h
 *
 * EMSCRIPTEN_KEEPALIVE prevents dead-code elimination in the WASM linker.
 * In native builds there is no WASM linker — define it as nothing.
 */
#pragma once

#define EMSCRIPTEN_KEEPALIVE

// Stub for EM_JS / EM_ASM (not used in paths we compile)
#define EM_JS(ret, name, args, ...)
#define EM_ASM(...)
#define EM_ASM_INT(...) 0

#define EM_LOG_ERROR 0
static inline void emscripten_log(int /*flags*/, const char* /*fmt*/, ...) {}
