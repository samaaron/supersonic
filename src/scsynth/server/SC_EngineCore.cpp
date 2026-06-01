/*
 * SC_EngineCore.cpp — see SC_EngineCore.h.
 */
#include "SC_EngineCore.h"

#include "SC_World.h"          // World: mRunning, mAudioBusTouched, hw...
#include "SC_HiddenWorld.h"    // HiddenWorld: mWireBufSpace
#include "SC_WorldOptions.h"   // WorldOptions, World_New / World_Start
#include "SC_Prototypes.h"     // World_SetSampleRate

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
