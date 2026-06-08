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

class MidiControl {
public:
    void init(OscEgress* egress, SuperClock* clock);
    void shutdown();

    // Handle one "/midi/" command off the audio thread (NRT gateway). Returns
    // true if it belongs to this subsystem (always, for a "/midi/" prefix).
    bool handleMidiCommand(const uint8_t* data, uint32_t size);

    // Dispatch a now-due scheduled MIDI event (the inner OSC the EventScheduler
    // emitted). Called on the MIDI dispatch thread; sends immediately.
    void dispatchOsc(const uint8_t* osc, uint32_t len);

    // Re-enumerate MIDI devices and broadcast /midi/ports — called from the
    // engine's device-change (hotplug) listener.
    void refreshDevices();

private:
    // ss_midi_* host callbacks (ctx = this).
    static void    emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);
    static void    tempoCb(void* ctx, double bpm);
    static void    transportCb(void* ctx, int32_t kind, double beat);

    SsMidi*     mMidi   = nullptr;
    OscEgress*  mEgress = nullptr;
    SuperClock* mClock  = nullptr;
};
