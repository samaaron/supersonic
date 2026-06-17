/*
 * IngressCallCtx.h — the immutable per-call metadata the dispatch path passes to
 * every backend via OscIngress::ingest(). One bundle (origin token + timetag)
 * travels with each message; each backend reads what it needs.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct DrainCallCtx {
    uint32_t      sourceId = 0;        // origin token — the reply metadata threaded to every
                                       // backend; a backend that replies builds its reply
                                       // from this (synth: ring_reply; control: OscEgress).
    int64_t       when     = 0;        // OSC timetag of this message; 0/1 = immediate.
                                       // The synth backend derives its sub-sample offset
                                       // from it; every other handler ignores it.
    int64_t       blockTime = 0;       // this block's start in OSC time — the synth
                                       // backend's offset reference (when - blockTime).
};

// The engine's default ingress route: performs a synth OSC bundle/message inline
// on the audio thread (the synth plane). Registered as the OscIngress default so
// the dispatcher carries no scsynth itself. Defined in the synth (audio_processor)
// TU; only registered when synth is built.
bool ss_synth_default_route(void* routeCtx, const void* callCtx,
                            const uint8_t* data, std::size_t len);
