// SPLimiter / SPLimiter2: look-ahead brickwall limiter with latency
// equal to the look-ahead, rather than twice it.
//
// Thin wrappers: everything that decides what the output sounds like
// lives in SPLimiterCore.h, which has no SuperCollider dependencies so
// that its ceiling guarantee can be unit-tested directly. These only do
// the plumbing, reading inputs, and owning the core's scratch memory on
// the server's real-time heap.
//
//   SPLimiter.ar(in, level, lookahead, release)          -> limited
//   SPLimiter2.ar(l, r, level, lookahead, release)        -> [l, r, gain]
//
// SPLimiter2 links the detector across the pair, so one gain derived
// from whichever channel needs it most is applied to both. That is what
// keeps the stereo image still while limiting; two independent mono
// instances move it on any peak that is not symmetric.
//
// SPLimiter2's third output is the applied gain, so a meter can show
// measured gain reduction instead of inferring it from how far the input
// went over.
//
// `lookahead` is scalar-rate: it sizes the allocation, so it is fixed
// when the synth is created. `level` and `release` are read once per
// block.

#include "SPLimiterCore.h"

#include "SC_InterfaceTable.h"
#include "SC_PlugIn.hpp"
#include "SC_Unit.h"

static InterfaceTable* ft;

namespace {

using sonicpi::dsp::SPLimiterCore;

// Shared setup for both UGens: size the core from the lookahead input,
// take its scratch off the RT heap, and fail safe if that heap is full.
// Returns false when allocation failed, in which case the caller must
// return immediately, the unit has already been silenced.
template <class UnitT>
bool initCore(UnitT* unit, SPLimiterCore& core, void*& mem, int lookaheadInput, int channels) {
    const int lookahead =
        SPLimiterCore::lookaheadSamples(unit->in0(lookaheadInput), unit->sampleRate());

    mem = RTAlloc(unit->mWorld, SPLimiterCore::memoryBytes(lookahead, channels));
    if (!mem) {
        Print("SPLimiter: alloc failed, increase the server's RT memory\n");
        unit->mCalcFunc = (UnitCalcFunc)ft->fClearUnitOutputs;
        ClearUnitOutputs(unit, 1);
        unit->mDone = 1;
        return false;
    }

    core.configure(lookahead, channels, unit->sampleRate(), mem);
    return true;
}

struct SPLimiter : public SCUnit {
    SPLimiter() {
        if (!initCore(this, mCore, mMem, 2, 1)) return;
        set_calc_function<SPLimiter, &SPLimiter::next>();

        // Zero the first output sample rather than running one sample of
        // the core here. The graph recomputes sample 0 on the first real
        // block, and the core is stateful, processing it twice would
        // push the same input into the delay line and the sliding
        // minimum window twice over.
        ClearUnitOutputs(this, 1);
    }

    ~SPLimiter() {
        if (mMem) RTFree(mWorld, mMem);
    }

private:
    void next(int numSamples) {
        mCore.process(in(0), out(0), numSamples, in0(1), in0(3));
    }

    SPLimiterCore mCore;
    void* mMem = nullptr;
};

struct SPLimiter2 : public SCUnit {
    SPLimiter2() {
        if (!initCore(this, mCore, mMem, 3, 2)) return;
        set_calc_function<SPLimiter2, &SPLimiter2::next>();
        ClearUnitOutputs(this, 1);
    }

    ~SPLimiter2() {
        if (mMem) RTFree(mWorld, mMem);
    }

private:
    void next(int numSamples) {
        mCore.process(in(0), in(1), out(0), out(1), numSamples, in0(2), in0(4), out(2));
    }

    SPLimiterCore mCore;
    void* mMem = nullptr;
};

}  // namespace

PluginLoad(SPLimiterUGen) {
    ft = inTable;
    registerUnit<SPLimiter>(ft, "SPLimiter", false);
    registerUnit<SPLimiter2>(ft, "SPLimiter2", false);
}
