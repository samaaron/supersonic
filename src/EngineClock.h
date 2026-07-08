/*
 * EngineClock.h — the portable clock-core /clock verbs, shared by every build.
 *
 * tempo, transport, start/stop sync, the rpc beat/time conversions, peer count
 * and the enabled/now queries — all expressed against SuperClock, whose one
 * composition root (SuperClockNative) backs every build. The native engine calls
 * this from EngineControl (UDP egress); the web worklet calls it from audio_processor's
 * IN-drain (OUT-ring egress). One implementation, so /clock behaves identically
 * everywhere. Replies leave through `reply`. Native-only Link-session verbs
 * (visibility, peers, Link Audio, notify) are NOT here — they stay in
 * EngineControl, capability-gated to the native build.
 */
#pragma once

#include <cstdint>
#include <functional>

class SuperClock;

using ClockReply = std::function<void(const uint8_t* data, uint32_t size)>;

// Returns true if `data` is a clock-core /clock verb it handled (so the caller
// stops), false for a non-/clock message or a /clock verb it doesn't own.
bool handleClockCoreOsc(SuperClock& clock, const uint8_t* data, uint32_t size,
                        const ClockReply& reply);

// The one refusal for a /clock verb no route owns: replies
// "/clock/unsupported s:address", echoing the request's trailing int32
// correlation token like every other /clock reply. Both dispatch tails
// (native EngineControl, wasm audio_processor) call this so the refusal
// wire contract has a single authoring site.
void replyClockUnsupported(const uint8_t* data, uint32_t size,
                           const ClockReply& reply);
