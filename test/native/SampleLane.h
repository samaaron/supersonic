// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * SampleLane.h — a sample, loaded the way a CLIENT loads one.
 *
 * The engine does not read files. A client decodes (clockwork_audio_file.h),
 * carves a slot in the inbox lane (clockwork_asset_pool.h), lays the frames
 * out with scsynth's guard frames around them, and hands the slot over as an
 * asset keyed by buffer number (/clockwork/asset/commit; dsp_api.h). That is
 * what the web client does in JavaScript and what SuperSonic's socket front
 * does for a sender that still says /b_allocRead. These tests are clients
 * too, so this is their copy of that path — with scsynth's replace semantics
 * kept from this side: loading over an occupied buffer frees it first, as
 * /b_allocRead used to.
 */
#pragma once

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_asset_pool.h"
#include "clockwork_audio_file.h"
#include "dsp_api.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sample_lane {

// One pool per engine instance, keyed on the fixture's generation (not its
// inbox address, which a fresh engine may well inherit from the last one): a
// new engine gets a new pool, and a slot the guest has released is handed
// back when its notification is seen.
struct LanePool {
    uint64_t            generation = 0;
    ClockworkAssetPool* pool = nullptr;
    struct Held { int32_t id; uint32_t slot; };
    std::vector<Held>   held;
};
inline LanePool& poolFor(EngineFixture& fx) {
    static LanePool lp;
    if (lp.generation != fx.generation() || !lp.pool) {
        if (lp.pool) clockwork_asset_pool_close(lp.pool);
        lp.generation = fx.generation();
        lp.pool = clockwork_asset_pool_open(fx.engine().guestInboxBytes());
        lp.held.clear();
    }
    // Reclaim what the guest has let go of since we last looked.
    for (const auto& r : fx.allReplies()) {
        if (r.address != "/clockwork/asset/released") continue;
        const int32_t id = r.parsed().argInt(0);
        for (size_t i = 0; i < lp.held.size(); ++i)
            if (lp.held[i].id == id) { clockwork_asset_pool_free(lp.pool, lp.held[i].slot); lp.held.erase(lp.held.begin() + (long)i); break; }
    }
    return lp;
}

// The client's work, short of the one message: decode `path`, keep frames
// [startFrame, startFrame + numFrames) (0 = to the end), free whatever the
// buffer held, and stage the frames in the lane with guard frames around
// them. What comes back is the commit message, ready to send. Split from the
// send so a test can time the audio blocks that carry the commit on its own.
struct Staged {
    bool             ok = false;
    osc_test::Packet commit;
    int32_t          frames = 0;
    int32_t          channels = 0;
};
inline Staged stage(EngineFixture& fx, int32_t bufnum, const std::string& path,
                    int32_t startFrame = 0, int32_t numFrames = 0) {
    Staged out;
    ClockworkAudioInfo info {};
    info.struct_bytes = sizeof info;
    float* decoded = nullptr;
    if (clockwork_audio_decode_file(path.c_str(), &info, &decoded) != CLOCKWORK_OK) return out;
    const int32_t total = (int32_t)info.frames;
    const int32_t ch    = (int32_t)info.channels;
    if (startFrame < 0) startFrame = 0;
    if (startFrame > total) startFrame = total;
    if (numFrames <= 0 || startFrame + numFrames > total) numFrames = total - startFrame;
    if (numFrames <= 0 || ch <= 0) { clockwork_audio_free(decoded); return out; }

    // scsynth's replace semantics, from the client side: whatever is in the
    // buffer goes first, and the guest lets its asset go with it.
    fx.send(osc_test::message("/b_free", bufnum));
    fx.waitForDone("/b_free", 2000);

    LanePool& lp = poolFor(fx);
    const uint32_t guardBefore = 3, guardAfter = 1;
    const uint32_t slotFrames  = guardBefore + (uint32_t)numFrames + guardAfter;
    const uint32_t slotBytes   = slotFrames * (uint32_t)ch * sizeof(float);
    uint32_t slot = 0;
    if (clockwork_asset_pool_alloc(lp.pool, slotBytes, &slot) != 0) { clockwork_audio_free(decoded); return out; }
    auto* lane = const_cast<uint8_t*>(fx.engine().guestInbox());
    std::memset(lane + slot, 0, slotBytes);
    const uint32_t payload = slot + guardBefore * (uint32_t)ch * sizeof(float);
    std::memcpy(lane + payload, decoded + (size_t)startFrame * ch,
                (size_t)numFrames * ch * sizeof(float));
    clockwork_audio_free(decoded);
    lp.held.push_back({ bufnum, slot });   // reclaimed when the guest says released

    osc_test::Builder b;
    auto& s = b.begin("/clockwork/asset/commit");
    s << bufnum << (int32_t)CLOCKWORK_ASSET_AUDIO_F32 << (int32_t)payload
      << (int32_t)((uint32_t)numFrames * (uint32_t)ch * sizeof(float))
      << ch << numFrames << (float)info.sample_rate;
    out.commit   = b.end();
    out.frames   = numFrames;
    out.channels = ch;
    out.ok       = true;
    return out;
}

// The whole load: stage, send, and wait for /clockwork/asset/committed.
inline bool load(EngineFixture& fx, int32_t bufnum, const std::string& path,
                 int32_t startFrame = 0, int32_t numFrames = 0) {
    const Staged st = stage(fx, bufnum, path, startFrame, numFrames);
    if (!st.ok) return false;
    fx.send(st.commit);
    OscReply r;
    return fx.waitForReply("/clockwork/asset/committed", r, 3000);
}

} // namespace sample_lane
