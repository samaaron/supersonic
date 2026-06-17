/*
 * SC_EngineCore.cpp — see SC_EngineCore.h.
 */
#include "SC_EngineCore.h"

#include <cstring>             // memset

#include "SC_World.h"          // World: mRunning, mAudioBus(Touched), hw...
#include "SC_HiddenWorld.h"    // HiddenWorld: mWireBufSpace, notification FIFOs
#include "SC_WorldOptions.h"   // WorldOptions, World_New
#include "SC_Prototypes.h"     // World_Start, World_SetSampleRate, World_Run

World* EngineCore_New(const WorldOptions* options, const char** outError) {
    auto fail = [&](const char* msg) -> World* {
        if (outError)
            *outError = msg;
        return nullptr;
    };
    if (outError)
        *outError = nullptr;

    World* world = World_New(const_cast<WorldOptions*>(options));
    if (!world)
        return fail("World_New returned null");

    // Realtime worlds get their sample rate from the audio driver, NRT worlds
    // from the input soundfile. A self-driven engine has neither, so set it
    // explicitly from the options (see header for the failure modes if 0).
    World_SetSampleRate(world, options->mPreferredSampleRate);

    if (!world->mAudioBusTouched || !world->mControlBusTouched)
        return fail("bus-touched arrays not allocated");

    // World_New only runs World_Start for realtime worlds; start it here if it
    // hasn't. mRunning is World_Start's own "started" flag, so this is idempotent.
    if (!world->mRunning)
        World_Start(world);

    if (!world->hw->mWireBufSpace)
        return fail("wire buffer allocation failed");

    return world;
}

void EngineCore_BeginBlock(World* world) {
    // Zero the output buses so output channels no synth writes this block read as
    // silence (the host copies them straight out). Channels that ARE written don't
    // rely on this: advancing mBufCounter makes Out overwrite on the first write
    // to a bus this block and accumulate only on later writes.
    memset(world->mAudioBus, 0,
           (size_t)world->mNumOutputs * (size_t)world->mBufLength * sizeof(float));
    world->mBufCounter++;
}

void EngineCore_RunBlock(World* world, unsigned int activeInputChannels) {
    world->mSampleOffset = 0;
    world->mSubsampleOffset = 0.f;

    // Mark live input buses "touched" for the current block so In.ar reads them
    // (the caller copies external input into the input-bus region beforehand).
    // Clamp to the World's input width — a device can report more channels.
    if (activeInputChannels > 0) {
        unsigned int n = activeInputChannels < (unsigned int)world->mNumInputs
                             ? activeInputChannels
                             : (unsigned int)world->mNumInputs;
        int32* inputTouched = world->mAudioBusTouched + world->mNumOutputs;
        const int32 bufCounter = world->mBufCounter;
        for (unsigned int i = 0; i < n; ++i)
            inputTouched[i] = bufCounter;
    }

    World_Run(world);
}

void EngineCore_FlushNotifications(World* world) {
    world->hw->mTriggers.Perform();
    world->hw->mNodeMsgs.Perform();
    world->hw->mNodeEnds.Perform();
}
