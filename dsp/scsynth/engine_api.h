/*
 * engine_api.h — the contract between SuperSonic and its audio engine.
 *
 * SuperSonic reaches its engine through a small, stable surface: build a
 * world, run a block, hand it OSC, read its buffers. This header states that
 * surface in C so either implementation can stand behind it — scsynth's
 * server, or the Rust graph engine in rust/supersonic-engine.
 *
 * It exists because the declarations were previously spread across scsynth's
 * own headers with C++ linkage, which tied the host to that particular
 * implementation for no reason other than where the prototypes happened to
 * live.
 */
#ifndef CLOCKWORK_ENGINE_API_H
#define CLOCKWORK_ENGINE_API_H

#include <stdint.h>

struct World;
struct WorldOptions;
struct SndBuf;
struct ReplyAddress;
struct OSC_Packet;

#ifdef __cplusplus
extern "C" {
#endif

/* World lifecycle. */
struct World* World_New(struct WorldOptions* options);
void World_Cleanup(struct World* world, bool unload_plugins);
void World_Run(struct World* world);

/* World_SetSampleRate, PerformOSCMessage and PerformOSCBundle are absent on
 * purpose. The host's existing headers declare them with C++ linkage, so they
 * are carried across in engine_support.cpp; declaring them here as C would
 * silently give the definitions the wrong linkage, which is exactly what
 * happened the first time. */

/* Buffers. World_GetBuf and World_GetNRTBuf are not here: they are inline in
 * SC_World.h, which the ugens compile against, so they belong to the shared
 * plugin interface rather than to this contract. */
int World_CopySndBuf(struct World* world, uint32_t index, struct SndBuf* out,
                     bool onlyIfChanged, bool* didChange);

/* Self-driven bring-up and the per-block cycle. */
struct World* EngineCore_New(const struct WorldOptions* options, const char** outError);
void EngineCore_BeginBlock(struct World* world);
void EngineCore_RunBlock(struct World* world, unsigned int activeInputChannels);
void EngineCore_FlushNotifications(struct World* world);

/* How many synthdefs are loaded — for the metrics the host publishes. */
uint32_t supersonic_engine_synthdef_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_ENGINE_API_H */
