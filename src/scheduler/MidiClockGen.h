/*
 * MidiClockGen.h — pure 24-PPQN MIDI clock pulse generator.
 *
 * No threads, no allocation, no IO. The host drives it from its render context
 * (native audio thread, wasm worklet, embedded timer) with the current musical
 * beat from SuperClock and routes each emitted pulse-beat to a deferred MIDI
 * sink (the EngineScheduler). Pulse position is tracked in integer 1/24-beat
 * units so the core is fixed-point friendly; only the beat<->pulse boundary
 * touches floating point, which is the single thing an esp32 port would swap.
 */
#pragma once

#include <cstdint>

class MidiClockGenerator {
public:
    static constexpr int64_t PPQN = 24;  // pulses per quarter-note (beat)

    // Begin generating; the first pulse is the next whole 1/24-beat boundary at
    // or after beatNow. Continue (resume) uses the same call — musical position
    // is carried by the shared SuperClock beat, so there is no separate state.
    void start(double beatNow) {
        mRunning   = true;
        mNextPulse = pulseCeil(beatNow);
    }
    void stop()          { mRunning = false; }
    bool running() const { return mRunning; }

    // Hand out every pulse-beat due at or before beatHorizon, advancing the
    // internal position. sink(pulseBeat) is called once per pulse; the host
    // converts pulseBeat -> SuperClock time and schedules a 0xF8 to each clock
    // port. No allocation. Returns the number of pulses emitted.
    template <class Sink>
    int collect(double beatHorizon, Sink&& sink) {
        if (!mRunning || beatHorizon < 0.0) return 0;
        const int64_t limit = pulseFloor(beatHorizon);
        int n = 0;
        while (mNextPulse <= limit) {
            sink(static_cast<double>(mNextPulse) / static_cast<double>(PPQN));
            ++mNextPulse;
            ++n;
        }
        return n;
    }

    // Beat of the next not-yet-emitted pulse (lets a host bound its look-ahead).
    double nextPulseBeat() const {
        return static_cast<double>(mNextPulse) / static_cast<double>(PPQN);
    }

private:
    bool    mRunning   = false;
    int64_t mNextPulse = 0;  // next unemitted pulse, in 1/24-beat units

    static int64_t pulseFloor(double beat) {
        return static_cast<int64_t>(beat * static_cast<double>(PPQN));  // beat >= 0
    }
    static int64_t pulseCeil(double beat) {
        if (beat <= 0.0) return 0;
        const double p = beat * static_cast<double>(PPQN);
        const int64_t f = static_cast<int64_t>(p);
        return (static_cast<double>(f) < p) ? f + 1 : f;
    }
};
