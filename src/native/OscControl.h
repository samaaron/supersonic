/*
 * OscControl.h — the "/osc/" engine seam (native-only).
 *
 * A thin bridge to the Rust OSC subsystem (supersonic-osc-net, ss_osc.h), which
 * owns the external-facing UDP sockets: the cue server (inbound external OSC,
 * re-framed to /external-osc-cue and pushed to the /osc/notify audience) and the
 * outbound user-OSC sender. This seam only translates between the engine and the
 * Rust C ABI: control verbs → ss_osc_configure, the deferred-event drain →
 * ss_osc_send, and the subsystem's emit callback → the egress.
 */
#pragma once

#include <cstdint>
#include <string>

struct SsOsc;       // rust/supersonic-osc-net/cpp/ss_osc.h
class OscEgress;
struct DrainCallCtx;

class OscControl {
public:
    void init(OscEgress* egress);
    void shutdown();

    // Handle one "/osc/" control command off the audio thread (NRT gateway):
    //   /osc/send <host:s> <port:i> <inner:b>  — send inner to host:port. Reached
    //       immediately, or as the inner blob of a /schedule event (the scheduler
    //       re-ingests it on time → this same dispatch).
    //   /osc/cue-server/config <port:i> <loopback:i|T/F> <cues_on:i|T/F>
    //   /osc/cue-server/cues-on  <i|T/F>   — toggle inbound cue forwarding
    //   /osc/cue-server/loopback <i|T/F>   — loopback-only vs all interfaces
    //   /osc/notify/subscribe | /osc/notify/unsubscribe
    // Returns true (always, for an "/osc/" prefix).
    bool handleOscCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

private:
    // ss_osc emit callback (ctx = this): an /external-osc-cue push for inbound
    // external OSC. May fire on the cue server's recv thread.
    static void emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);
    // Push the current (port, loopback, cues_on) to the subsystem (idempotent;
    // the Rust side only rebinds when the port/loopback actually change).
    void applyCueConfig();

    OscEgress* mEgress = nullptr;
    SsOsc*     mOsc      = nullptr;
    int        mPort     = 0;      // cue port (0 = unbound)
    bool       mLoopback = true;   // loopback-only by default
    bool       mCuesOn   = false;  // forward inbound external OSC as cues
};
