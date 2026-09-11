// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * SuperSonicFront.h — SuperSonic's socket front.
 *
 * The engine has no files. A sample is decoded by a CLIENT into the inbox
 * lane and handed over as an asset (/clockwork/asset/commit, keyed by buffer
 * number; clockwork/src/dsp_api.h). A sender on the socket that still says
 * /b_allocRead — Sonic Pi's spider, any scsynth client — is talking to
 * SuperSonic, and SuperSonic is a client of clockwork: it is the one that
 * does the decoding. This is that client, installed at the gateway
 * (clockwork/src/native/OscFront.h) where it sees every packet in and every
 * reply out, and it answers every file verb scsynth ever had:
 *
 *   /b_allocRead, /b_allocReadChannel   decode → lane, with the guest's guard
 *                                       frames → asset commit (the buffer is
 *                                       bound to the lane in place)
 *   /b_read, /b_readChannel             decode → lane → /supersonic/buffer/read
 *                                       (the guest copies into the buffer)
 *   /b_write                            /b_query → /supersonic/buffer/publish
 *                                       (the guest puts the frames in the
 *                                       outbox) → encode here
 *   /d_load, /d_loadDir                 read the files → one /d_recv each,
 *                                       paced so a block parses a few
 *   /clockwork/record/start, /stop      the master tap (audio slot 0) pulled
 *                                       into a file on a thread here
 *                                       (Recorder.h); the engine is not told
 *
 * The engine's replies to what the front sent stay behind the front; the
 * asker hears scsynth's words — /done "/b_read" bufnum, /fail "/b_write"
 * why bufnum — and a completion message goes on once the load is in. One
 * thread reads, decodes and encodes, in arrival order; the audio thread
 * only ever binds a pointer, copies a bounded range, or parses one synthdef.
 * Sonic Pi, 2026-09-07: the glitch on every sample not yet loaded, and the
 * 24 ms block at boot.
 */
#pragma once

#include "OscFront.h"
#include "Recorder.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ClockworkEngine;
class IOscTransport;
struct ClockworkAssetPool;

class SuperSonicFront final : public OscFront {
public:
    // `replies` is where the front's own answers go: the transport the
    // engine's replies also leave by, so a /done from here and a /b_info
    // from the engine reach the same socket in order.
    SuperSonicFront(ClockworkEngine& engine, IOscTransport* replies);
    ~SuperSonicFront() override;

    bool        ingress(const uint8_t* data, uint32_t size, uint32_t token) override;
    bool        egress(uint32_t token, const uint8_t* data, uint32_t size) override;
    const char* describe() const override {
        return "the file verbs (/b_allocRead, /b_read, /b_write, /d_load...) answered here, through the lanes";
    }

    // The lanes as this front carves them: size, and what is not held.
    uint32_t laneBytes();
    uint32_t laneFreeBytes();
    uint32_t outboxFreeBytes();
    // The session recording, if one is running.
    bool     recording() const { return mRecorder.recording(); }
    uint64_t recordingFramesLost() const { return mRecorder.framesLost(); }

private:
    enum class Kind { AllocRead, AllocReadChannel, ReadInto, ReadIntoChannel, Write, Encode,
                      SynthDefFile, SynthDefDir };
    struct Job {
        Kind                 kind = Kind::AllocRead;
        uint32_t             token = 0;
        int32_t              bufnum = 0;
        std::string          path;        // the file, the pattern, or the directory
        int32_t              start = 0;   // file frame to start at
        int32_t              frames = 0;  // frames wanted (0 or -1: to the end)
        int32_t              bufStart = 0;
        int32_t              leaveOpen = 0;
        std::vector<int32_t> channels;    // the ...Channel verbs' pick
        std::string          header, sample;   // /b_write's format names
        std::vector<uint8_t> completion;
        // Encode only: what the guest published.
        uint32_t             outSlot = 0, outChannels = 0, outFrames = 0;
        float                outRate = 0.f;
        uint32_t             format = 0, encoding = 0;
    };
    // What the front sent the engine and has not answered the asker for yet.
    struct Pending {
        enum What { Commit, ReadInto, WriteQuery, WritePublish } what;
        uint32_t             token;
        int32_t              bufnum;
        std::string          cmd;         // the verb the asker used
        uint32_t             slot;        // lane (Commit, ReadInto) or outbox (WritePublish)
        std::vector<uint8_t> completion;
        Job                  job;         // Write: carried through to the encode
    };
    struct DefLoad {              // /d_recv messages in flight for one /d_load or /d_loadDir
        uint32_t             token;
        std::string          cmd;
        std::string          path;
        uint32_t             total;
        uint32_t             remaining;
        uint32_t             failed;      // a def the engine refused is skipped, as the engine's own
        std::vector<uint8_t> completion;  // directory load skipped it; only nothing at all is a failure
    };

    void run();
    void loadSample(const Job& job);
    void loadDefs(const Job& job);
    void startWrite(const Job& job);
    void encode(const Job& job);
    bool ensurePools();           // under mMut
    bool takePending(Pending::What what, uint32_t token, int32_t bufnum, Pending* out);  // under mMut
    void reply(uint32_t token, const uint8_t* data, uint32_t size);
    void sendDone(uint32_t token, const char* cmd, int32_t bufnum);
    void sendFail(uint32_t token, const char* cmd, const std::string& why, int32_t bufnum);
    void sendDoneCmd(uint32_t token, const char* cmd);
    void sendFailCmd(uint32_t token, const char* cmd, const std::string& what);
    bool recordVerb(const uint8_t* data, uint32_t size, uint32_t token);   // true: it was one, and answered
    void finish(uint32_t token, const char* cmd, int32_t bufnum, const std::vector<uint8_t>& completion);

    ClockworkEngine& mEngine;
    IOscTransport*   mReplies;
    Recorder         mRecorder;

    std::mutex              mQueueMut;
    std::condition_variable mQueueCv;
    std::deque<Job>         mQueue;
    bool                    mStop = false;

    std::mutex                    mMut;        // everything below
    ClockworkAssetPool*           mPool = nullptr;      // over the inbox
    ClockworkAssetPool*           mOutPool = nullptr;   // over the outbox
    uint32_t                      mPoolBytes = 0, mOutPoolBytes = 0;
    std::map<int32_t, uint32_t>   mLive;       // bufnum → lane slot the guest holds
    std::deque<Pending>           mPending;    // in send order
    std::map<int32_t, uint32_t>   mFreeing;    // bufnum → /b_free replies of ours still to swallow
    std::deque<DefLoad>           mDefLoads;   // in order; a /d_recv reply answers the oldest for its token

    // LAST, and it must stay last. The constructor starts this thread in its
    // member-init list, and members are constructed in DECLARATION order — so
    // every member run() touches has to be declared above it. Declared any
    // earlier and the worker locks mQueueMut, and reads mStop, before they are
    // constructed: EINVAL out of std::mutex::lock, an uncaught system_error,
    // and a SIGABRT in whichever test happened to be running.
    std::thread                   mWorker;
};
