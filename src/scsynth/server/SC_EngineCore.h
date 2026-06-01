/*
 * SC_EngineCore.h — drive-it-yourself engine bring-up.
 *
 * scsynth normally assumes an OS audio driver (or an input soundfile in NRT)
 * creates and starts the world. Embedders that drive the engine themselves —
 * the WASM AudioWorklet, the ESP32 I2S task, the native headless harness — need
 * the same bring-up sequence rather than re-deriving it from audio_processor.cpp.
 * This factors it into core so every target shares one tested implementation.
 *
 * The per-block render sequence (begin / run / flush) is a separate seam layered
 * on top of this one.
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

#ifdef __cplusplus
}
#endif

#endif /* SC_ENGINECORE_H */
