// LinkTempo / LinkPhase — session-clock UGens.
//
// THESE USED TO BE LINK UGENS, and they reached Ableton Link directly: a
// file-scope `activeLink()` read tau's g_active_clockwork_clock, took a
// void* off ClockworkClock and cast it back to `ableton::LinkAudio*`, then called
// captureAudioSessionState() on the render thread. Guest DSP code including
// <ableton/LinkAudio.hpp>, holding a pointer to tau's Link instance,
// and — through LinkTempo's set input and LinkJump — COMMITTING session state
// from inside a UGen.
//
// It is tau's job to know whether Link exists. What a DSP needs is the
// session clock, and it already has one: DspConfig::clock is the
// ClockworkClockState mirror tau publishes, the same bytes JavaScript
// polls, and the same ones Link's tempo callback writes its converged tempo and
// re-anchored origin into when a session is running. Reading that instead means
// these work identically whether Link is compiled in, enabled, or absent —
// where before they printed "Error: Link not active" and output -1.0 on every
// web build.
//
// THE WRITE SIDE IS GONE. LinkTempo's set input committed a new session tempo
// and LinkJump forced a beat position, both from the audio thread, both around
// ClockworkClock's back. Changing the session's tempo is a control-plane act with
// its own verbs (/clockwork/clock/...), not something a synth graph should do
// mid-block. Nothing in this repository or upstream used either — the only
// references anywhere were these UGens, their registration, and their own
// tests.
//
// THE NAMES STAY. They are the public surface — sclang classes declare them and
// synthdefs are compiled against them — so renaming would break every existing
// definition to make a point about vocabulary. What changed is underneath: a
// UGen called LinkTempo now reports the session tempo, which is Link's when
// Link is running it and tau's own when it is not.

#include "SC_InterfaceTable.h"
#include "SC_Unit.h"
#include "SC_PlugIn.hpp"

#include "shared_memory.h"   // ClockworkClockState, readClockworkClock
#include "clock/clock_math.h"      // clockwork::wrapPhase

#include <algorithm>

static InterfaceTable* ft;

// Parked by the guest at dsp_new / dsp_process — a UGen holds only a Unit*, so
// this is how it reaches what tau handed over. See scsynth_dsp.cpp.
extern "C" const ClockworkClockState* supersonic_scsynth_session_clock();
extern "C" double supersonic_scsynth_block_ntp();

namespace {

// One coherent read per block. readClockworkClock applies the ordering rules so a
// UGen author does not have to; with no clock at all it answers with sane
// defaults rather than failing, which is why there is no "not active" branch
// here any more.
inline ClockworkClockSnapshot sessionClock() {
    return readClockworkClock(supersonic_scsynth_session_clock());
}

}  // namespace

// Session tempo, in CPS (cycles per second), matching the units the previous
// UGen reported.
class LinkTempo : public SCUnit {
public:
    LinkTempo() { set_calc_function<LinkTempo, &LinkTempo::next_k>(); }

private:
    void next_k(int /*numSamples*/) {
        *mOutBuf[0] = static_cast<float>(sessionClock().bpm / 60.0);
    }
};

// Phase within the quantum, in [0, quantum).
class LinkPhase : public SCUnit {
public:
    LinkPhase() { set_calc_function<LinkPhase, &LinkPhase::next_k>(); }

private:
    void next_k(int /*numSamples*/) {
        const double quantum = std::max(static_cast<double>(in0(0)), 1e-6);
        const auto clock = sessionClock();
        // The same arithmetic tau and the JS client use — one formula,
        // in clock_math.h, rather than a second opinion computed here.
        const double beat = clock.beatAt(supersonic_scsynth_block_ntp());
        *mOutBuf[0] = static_cast<float>(clockwork::wrapPhase(beat, quantum));
    }
};

PluginLoad(LinkUGen) {
    ft = inTable;
    registerUnit<LinkTempo>(ft, "LinkTempo", false);
    registerUnit<LinkPhase>(ft, "LinkPhase", false);
}
