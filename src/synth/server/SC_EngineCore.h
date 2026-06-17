/*
 * SC_EngineCore.h — drive-it-yourself engine lifecycle.
 *
 * scsynth normally assumes an OS audio driver (or an input soundfile in NRT)
 * creates, starts, and pumps the world. Embedders that drive the engine
 * themselves — the WASM AudioWorklet, the ESP32 I2S task, the native headless
 * harness — need the same setup-and-render sequence rather than re-deriving it
 * from audio_processor.cpp. This factors it into core so every target shares one
 * tested implementation.
 *
 * Threading contract: the engine is single-threaded. EngineCore_BeginBlock /
 * RunBlock and all OSC-command execution must run on one thread and never
 * overlap — they share the node tree, RT pool, and buses. Producers on other
 * threads must hand work across a queue, not call in.
 */
#ifndef SC_ENGINECORE_H
#define SC_ENGINECORE_H

struct World;
struct WorldOptions;

#ifdef __cplusplus
extern "C" {
#endif

/* Create a World ready to render, with no audio driver or soundfile assumed.
 *
 * This is the one call an embedder needs to get from options to a renderable
 * engine. Beyond World_New it does the setup that World_New only performs for
 * realtime worlds (it otherwise expects the audio driver / input soundfile to
 * provide it):
 *   - sets the sample rate from options->mPreferredSampleRate (else mSampleRate
 *     is 0 -> every oscillator frequency is wrong and duration-sized UGens such
 *     as Normalizer build a 0-length buffer and loop forever);
 *   - calls World_Start to allocate the per-synth wire-buffer space (else unit
 *     output buffers are null -> the first /s_new stores through a null pointer);
 *   - verifies the bus-touched arrays and wire buffers actually allocated.
 *
 * Returns the World, or null on failure. If outError is non-null it receives a
 * static human-readable reason (and is cleared to null on success). Idempotent
 * w.r.t. World_Start: a realtime world that already started is not started again.
 */
World* EngineCore_New(const WorldOptions* options, const char** outError);

/* Begin one control block: zero the output buses (so output channels nothing
 * writes this block come out silent) and advance the block counter (which makes
 * Out overwrite on the first write to each bus this block, accumulate after).
 * Call before applying this block's time-stamped OSC — a scheduler sets per-event
 * sample offsets between Begin and Run — and before EngineCore_RunBlock. */
void EngineCore_BeginBlock(World* world);

/* Run the graph (the DSP pass) for the block opened by BeginBlock. Audio is left
 * in world->mAudioBus, planar: channel c, frame f at mAudioBus[c*mBufLength + f],
 * output channels first. activeInputChannels = number of input-bus channels the
 * caller has already filled (marked "touched" so In.ar reads them); 0 if none.
 *
 * Leaves the trigger / node notifications it produces queued in the world's
 * FIFOs; call EngineCore_FlushNotifications afterwards to deliver them. */
void EngineCore_RunBlock(World* world, unsigned int activeInputChannels);

/* Drain the trigger / node-notification FIFOs (/tr, /n_go, /n_end, ...) produced
 * by the block's graph pass, dispatching them through the world's reply path
 * (a host-supplied reply function). Call after EngineCore_RunBlock. */
void EngineCore_FlushNotifications(World* world);

#ifdef __cplusplus
}
#endif

#endif /* SC_ENGINECORE_H */
