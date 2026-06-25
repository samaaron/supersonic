/*
 * MidiControl.h — the "/midi/" engine seam.
 *
 * Owns the Rust midir-based MIDI subsystem (ss_midi_*, dual MIT/GPL crate) and
 * bridges it to the engine: forwards "/midi/" control OSC into it, and routes
 * its callbacks back out — "/midi/in/" events + "/midi/ports" to the egress hub,
 * clock-in BPM and transport to SuperClock. Subscription manages the egress
 * audience.
 */
#pragma once

#include <cstdint>

struct SsMidi;       // rust/supersonic-midi/cpp/ss_midi.h
class OscEgress;
class SuperClock;
struct DrainCallCtx;

class MidiControl {
public:
    void init(OscEgress* egress, SuperClock* clock);
    void shutdown();

    // Handle one "/midi/" command off the audio thread (NRT gateway). Returns
    // true if it belongs to this subsystem (always, for a "/midi/" prefix).
    // Reached by both immediate "/midi/" traffic and scheduled "/midi/" events
    // re-ingested on time (the scheduler feeds the same dispatch).
    bool handleMidiCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

    // Re-enumerate MIDI devices and broadcast /midi/ports — called from the
    // engine's device-change (hotplug) listener.
    void refreshDevices();

private:
    // ss_midi_* host callbacks (ctx = this). clock/transport carry the
    // normalised port handle + raw OS name (length-delimited, not NUL-term).
    static void    emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);
    static void    clockCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                           const uint8_t* raw, uint32_t rawLen, uint64_t tsUs);
    static void    transportCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                               const uint8_t* raw, uint32_t rawLen, int32_t kind, double beat);
    // Broadcast a /clock/timelines push when the timeline set changes.
    void           broadcastTimelines();
    // Route /midi/clock/beat (Sonic Pi's midi_clock_beat) to MidiClockOut.
    // Returns false if the message is not the beat verb.
    // Safe on any non-RT thread (command path and deferred dispatch).
    bool           handleClockOutVerb(const uint8_t* data, uint32_t size);

    SsMidi*     mMidi   = nullptr;
    OscEgress*  mEgress = nullptr;
    SuperClock* mClock  = nullptr;
    // The current command's origin token, held for the duration of a synchronous
    // ss_midi_handle_osc call so emitCb (a Rust callback with no call ctx) can
    // route its REPLY back to the caller. NRT-thread-only; REPLY emits are
    // synchronous within handleMidiCommand. Async emits (/midi/in, ports) broadcast.
    uint32_t    mReplyToken = 0;
};
