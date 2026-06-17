/*
 * EngineControl.h — the supersonic/ and link/ control-endpoint implementations,
 * owned by the engine.
 *
 * The handlers run on the ingest thread. They reach the engine for device /
 * driver / record / link state, and the egress hub (`mEgress`) for replies and
 * subscriber pushes — never the transport directly. The engine registers
 * EngineControl's sinks on its OscIngress.
 */
#pragma once

#include <cstddef>
#include <cstdint>

class OscEgress;
class SupersonicEngine;
class SuperClock;
struct DrainCallCtx;

class EngineControl {
public:
    void init(SupersonicEngine* engine, OscEgress* egress, SuperClock* clock) {
        mEngine     = engine;
        mEgress     = egress;
        mSuperClock = clock;
    }

    // The NRT reader resolves the origin then calls these directly (see
    // SupersonicEngine's NRT gateway control drain).
    bool handleLinkCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);
    bool handleSupersonicCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

private:
    SupersonicEngine* mEngine     = nullptr;
    OscEgress*        mEgress     = nullptr;
    SuperClock*       mSuperClock = nullptr;
};
