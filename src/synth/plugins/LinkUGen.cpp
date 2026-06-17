// LinkTempo / LinkPhase / LinkJump — upstream-compatible Link UGens
// backed by SuperClock's LinkAudio. Tempo I/O is CPS (BPM / 60).

// Ableton's asio dependency transitively includes <imm.h> on Windows,
// which uses the bare Win32 SAL keyword macros 'IN' and 'OUT'.
// SC_Unit.h (included via SC_PlugIn.hpp below) redefines those names
// as function-like buffer-access macros, so we have to pull in
// <windows.h> + <ableton/LinkAudio.hpp> BEFORE the scsynth headers.
#ifdef SUPERSONIC_LINK
#  ifdef _WIN32
#    include <windows.h>
#  endif
#  include <ableton/LinkAudio.hpp>
#endif

#include "SuperClock.h"
#include "SC_InterfaceTable.h"
#include "SC_Unit.h"
#include "SC_PlugIn.hpp"

#include <algorithm>
#include <atomic>

static InterfaceTable* ft;

#ifdef SUPERSONIC_LINK

// Cached at Unit ctor to avoid an atomic load on every k-block.
static ableton::LinkAudio* activeLink() {
    SuperClock* sc = g_active_superclock.load(std::memory_order_acquire);
    if (!sc) return nullptr;
    return static_cast<ableton::LinkAudio*>(sc->audioThreadLinkAudioPtr());
}

class LinkTempo : public SCUnit {
public:
    LinkTempo() : mLink(activeLink()) {
        set_calc_function<LinkTempo, &LinkTempo::next_k>();
    }
private:
    ableton::LinkAudio* mLink;
    bool mWarned = false;
    void next_k(int /*numSamples*/) {
        float* out = mOutBuf[0];
        if (mLink && mLink->isEnabled()) {
            mWarned = false;
            auto state = mLink->captureAudioSessionState();
            if (in0(0) > 0.0f) {
                state.setTempo(in0(1) * 60.0f, mLink->clock().micros());
                mLink->commitAudioSessionState(state);
            }
            *out = static_cast<float>(state.tempo()) / 60.0f;
        } else {
            if (!mWarned) {
                Print("Error: Link not active, can not access tempo\n");
                mWarned = true;
            }
            *out = -1.0f;
        }
    }
};

class LinkPhase : public SCUnit {
public:
    LinkPhase() : mLink(activeLink()) {
        set_calc_function<LinkPhase, &LinkPhase::next_k>();
    }
private:
    ableton::LinkAudio* mLink;
    bool mWarned = false;
    void next_k(int /*numSamples*/) {
        float* out = mOutBuf[0];
        if (mLink && mLink->isEnabled()) {
            mWarned = false;
            // phaseAtTime divides by quantum.
            const double quantum = std::max(static_cast<double>(in0(0)), 1e-6);
            auto state = mLink->captureAudioSessionState();
            auto phase = state.phaseAtTime(mLink->clock().micros(), quantum);
            *out = static_cast<float>(phase);
        } else {
            if (!mWarned) {
                Print("Error: Link not active, can not access phase\n");
                mWarned = true;
            }
            *out = -1.0f;
        }
    }
};

class LinkJump : public SCUnit {
public:
    LinkJump() : mLink(activeLink()) {
        set_calc_function<LinkJump, &LinkJump::next_k>();
    }
private:
    ableton::LinkAudio* mLink;
    bool mWarned = false;
    void next_k(int /*numSamples*/) {
        float* out = mOutBuf[0];
        if (mLink && mLink->isEnabled()) {
            mWarned = false;
            if (in0(0) > 0.0f) {
                const double quantum = std::max(static_cast<double>(in0(2)), 1e-6);
                auto state = mLink->captureAudioSessionState();
                if (in0(3) > 0.0f) {
                    state.forceBeatAtTime(in0(1), mLink->clock().micros(), quantum);
                } else {
                    state.requestBeatAtTime(in0(1), mLink->clock().micros(), quantum);
                }
                mLink->commitAudioSessionState(state);
            }
            *out = 0.0f;
        } else {
            if (!mWarned) {
                Print("Error: Link not active, can not jump\n");
                mWarned = true;
            }
            *out = -1.0f;
        }
    }
};

#endif  // SUPERSONIC_LINK

PluginLoad(LinkUGen) {
    ft = inTable;
#ifdef SUPERSONIC_LINK
    registerUnit<LinkTempo>(ft, "LinkTempo", false);
    registerUnit<LinkPhase>(ft, "LinkPhase", false);
    registerUnit<LinkJump>(ft, "LinkJump", false);

    // /cmd linkclock <0|1> dispatches inline on the audio thread, so
    // the blocking enable/disable is offloaded to SuperClock's worker.
    ft->fDefinePlugInCmd(
        "linkclock",
        [](World* /*inWorld*/, void*, sc_msg_iter* args, void*) {
            int flag = args->geti(-1);
            if (flag != 0 && flag != 1) {
                Print("ERROR: linkclock requires 0 or 1\n");
                return;
            }
            SuperClock* sc =
                g_active_superclock.load(std::memory_order_acquire);
            if (sc) sc->requestSetLinkEnabledAsync(flag == 1);
        },
        nullptr);
#else
    ft->fDefinePlugInCmd(
        "linkclock",
        [](World*, void*, sc_msg_iter*, void*) {
            Print("ERROR: linkclock requires a SUPERSONIC_LINK build\n");
        },
        nullptr);
#endif  // SUPERSONIC_LINK
}
