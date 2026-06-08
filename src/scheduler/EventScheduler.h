/*
 * EventScheduler.h — a tiny, RT-safe "events in → stored → events out on time"
 * scheduler, modelled on scsynth's BundleScheduler.
 *
 * General shape:
 *   - enqueue(when, dest, osc): store an event to fire at OSC-timetag `when`.
 *   - tick(nextOscTime): emit every due event to the OUT ring.
 *
 * Threading: enqueue() and tick() run ONLY on the audio (RT) thread — enqueue
 * from the `/midi/at` ingress route, tick from process_audio — so the slot pool
 * is single-threaded (no lock, no in-ring). The OUT ring is the one cross-thread
 * boundary: the RT thread is its sole producer; a single consumer drains it
 * (native dispatch thread, web SAB worker, or web PM worklet-post). It uses the
 * same Message-framed ring mechanism as the engine's other rings.
 */
#pragma once

#include <atomic>
#include <cstdint>

class EventScheduler {
public:
    // Dispatch destinations (extensible; MIDI is the only one today).
    enum Dest : uint32_t { DEST_MIDI = 0 };

    // Store an event to fire at OSC-timetag `when` (same units as
    // ntp_to_osc_timetag). RT thread only; no lock. Returns false (and counts a
    // drop) if the pool is full or the payload exceeds the slot size.
    bool enqueue(int64_t when, uint32_t dest, const uint8_t* osc, uint32_t len);

    // Emit every event due at/through `nextOscTime` to the OUT ring, releasing
    // its slot. RT thread only. Block-granular, which is all MIDI timing needs.
    void tick(int64_t nextOscTime);

    // OUT ring accessors for the single consumer.
    uint8_t*              outBuffer() { return mOut; }
    uint32_t              outSize() const { return kOutSize; }
    std::atomic<int32_t>* outHead() { return &mOutHead; }
    std::atomic<int32_t>* outTail() { return &mOutTail; }

    uint32_t dropped() const { return mDropped.load(std::memory_order_relaxed); }

private:
    static constexpr int      kMaxEvents  = 256;
    static constexpr uint32_t kMaxPayload = 512;    // a MIDI message + small sysex
    static constexpr uint32_t kOutSize    = 65536;

    struct Slot {
        bool     inUse = false;
        int64_t  when  = 0;
        uint32_t dest  = 0;
        uint32_t len   = 0;
        uint8_t  data[kMaxPayload];
    };
    Slot mSlots[kMaxEvents];

    // OUT ring (events out, framed [dest:u32][osc]). RT writer, one consumer.
    uint8_t              mOut[kOutSize];
    std::atomic<int32_t> mOutHead{0};
    std::atomic<int32_t> mOutTail{0};
    std::atomic<int32_t> mOutSeq{0};
    std::atomic<int32_t> mOutLock{0};   // uncontended: a single RT producer
    std::atomic<uint32_t> mDropped{0};
};

// The process-wide instance (defined in audio_processor.cpp, where it is ticked).
EventScheduler& get_event_scheduler();

// Schedule an event from `ntp_seconds` (converted to an OSC timetag with the
// engine's own ntp_to_osc_timetag, so it shares SuperClock's / scsynth's clock
// domain). Called from the audio-thread `/midi/at` route.
extern "C" void ss_defer_schedule(double ntp_seconds, uint32_t dest,
                                  const uint8_t* osc, uint32_t len);

// As above but the caller already holds an OSC timetag (e.g. Sonic Pi, which
// timetags MIDI exactly like its scsynth bundles — same SuperClock domain).
extern "C" void ss_defer_schedule_raw(int64_t osc_timetag, uint32_t dest,
                                      const uint8_t* osc, uint32_t len);
