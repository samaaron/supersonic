/*
 * IngressCallCtx.h — the per-call context the audio-thread drain passes to
 * OscIngress::ingest(), shared by the drain (audio_processor.cpp) and the native
 * control routes (SupersonicEngine.cpp). The wasm `/clock` handler reads `reply`;
 * the native forward-to-NRT handler reads `sourceId` (the origin token).
 */
#pragma once

#include <cstdint>

struct ReplyAddress;  // scsynth (SC_ReplyImpl.hpp) — pointer only, fwd-decl suffices

struct DrainCallCtx {
    ReplyAddress* reply    = nullptr;  // reply address of the message being drained
    uint32_t      sourceId = 0;        // IN-ring Message.sourceId — the origin token (native)
};
