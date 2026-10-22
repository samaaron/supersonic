// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * supersonic_commands.h — supersonic::Commands, what a client of SuperSonic
 * sends its commands through. The native server sends every command through
 * it, and so does the NIF.
 *
 * The engine has no files. A sample is decoded by a CLIENT into the assets area
 * lane and handed over as an asset (/clockwork/asset/commit, keyed by buffer
 * number; clockwork/src/dsp_api.h). A sender on the socket that still says
 * /b_allocRead — Sonic Pi's spider, any scsynth client — is talking to
 * SuperSonic, and SuperSonic is a client of clockwork: it is the one that
 * does the decoding. This is that client, installed at the gateway
 * (clockwork/src/comms/OscFront.h) where it sees every packet in and every
 * reply out, and it answers every file verb scsynth ever had:
 *
 *   /b_allocRead, /b_allocReadChannel   decode → lane, with the guest's guard
 *                                       frames → asset commit (the buffer is
 *                                       bound to the lane in place)
 *   /b_read, /b_readChannel             decode → lane → /supersonic/buffer/read
 *                                       (the guest copies into the buffer)
 *   /b_write                            /b_query → /supersonic/buffer/publish
 *                                       (the guest puts the frames in the
 *                                       returns) → encode here
 *   /d_load, /d_loadDir                 read the files → one /d_recv each,
 *                                       paced so a block parses a few
 *   /clockwork/record/start, /stop      the master tap (audio slot 0) pulled
 *                                       into a file here (Recorder.h); the
 *                                       engine is not told
 *   /clockwork/summary                  the host's version and features, down
 *                                       the debug channel for a GUI's info
 *                                       pane (setSummary)
 *   /quit                               /done, then the host's own stop
 *                                       (setQuit): scsynth's way of ending a
 *                                       server
 *
 * The engine's replies to what the front sent stay behind the front; the
 * asker hears scsynth's words — /done "/b_read" bufnum, /fail "/b_write"
 * why bufnum — and a completion message goes on once the load is in. One
 * thread reads, decodes and encodes, in arrival order — the front's own, or
 * its host's (Work::Host) — and the audio thread only ever binds a pointer,
 * copies a bounded range, or parses one synthdef.
 * Sonic Pi, 2026-09-07: the glitch on every sample not yet loaded, and the
 * 24 ms block at boot.
 */
#pragma once

#include "OscFront.h"
#include "Recorder.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ClockworkEngine;
class IOscTransport;
struct ClockworkAssetPool;

namespace supersonic {

class Commands final : public OscFront {
public:
    // Who does the front's work: the file jobs, and a recording's copying
    // from the tap into its file. Own: threads of its own, a worker started
    // here and one for each recording, as a server wants. Host: none; the
    // host calls runPending() on a thread of its own, as a host that drives
    // clockwork's control plane calls controlPass() (Config::hostDrivesControl).
    enum class Work { Own, Host };

    // `replies` is where the front's own answers go: the transport the
    // engine's replies also leave by, so a /done from here and a /b_info
    // from the engine reach the same socket in order.
    Commands(ClockworkEngine& engine, IOscTransport* replies, Work work = Work::Own);
    ~Commands() override;

    // Work::Host: every file job queued so far, in arrival order, then what a
    // recording has waiting written to its file, on the calling thread. That
    // thread is the front's one worker: never two at once. Work::Own: nothing,
    // since the front's own threads do it.
    void runPending();

    // What /clockwork/summary answers with: the host's to say (its product,
    // its version, what it was built with). Set before the transport starts;
    // unset, the verb is the engine's to refuse.
    void setSummary(std::string summary) { mSummary = std::move(summary); }

    // What /quit does: with `stop` set, the asker hears /done /quit and then
    // `stop` runs, as scsynth ends; unset, /quit goes on to the engine, which
    // refuses it. Set before the transport starts.
    void setQuit(std::function<void()> stop) { mQuit = std::move(stop); }

    // Loads every definition in `dir` as /d_loadDir does, and returns once the
    // engine has answered for each: scsynth's -D, before any client is heard.
    // The engine's replies must already be flowing through egress(). False,
    // with the reason in `why`, when the directory holds none or the engine
    // does not answer, and under Work::Host, whose load would wait on work
    // only the host runs: such a host sends /d_loadDir and runs until the
    // /done comes.
    bool loadDefinitions(const std::string& dir, std::string* why);

    bool        ingress(const uint8_t* data, uint32_t size, uint32_t token) override;
    bool        egress(uint32_t token, const uint8_t* data, uint32_t size) override;
    const char* describe() const override {
        return "the file verbs (/b_allocRead, /b_read, /b_write, /d_load...) answered here, through the lanes";
    }

    // The lanes as this front carves them: size, and what is not held.
    uint32_t laneBytes();
    uint32_t laneFreeBytes();
    uint32_t returnsFreeBytes();
    // The session recording, if one is running.
    bool     recording() const { return mRecorder.recording(); }
    uint64_t recordingFramesLost() const { return mRecorder.framesLost(); }
    // The bytes of definitions a callback may be handed, for each frame it renders (loadDefs).
    static constexpr uint32_t kDefBytesPerFrame = 64;
    // The batch with the most /d_recv definitions ever handed to the engine at once (loadDefs): how many, their
    // bytes, and the share of a callback they were sent under (0 for a load's first batch: no callback measured).
    struct DefBatch { uint32_t defs = 0, bytes = 0, budget = 0; };
    DefBatch largestDefBatch() { std::lock_guard<std::mutex> lock(mMut); return mLargestDefBatch; }
    // Batches of more than one definition whose bytes were over their share: none, by design (loadDefs).
    uint32_t defBatchesOverShare() { std::lock_guard<std::mutex> lock(mMut); return mDefBatchesOverShare; }

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
        uint32_t             slot;        // lane (Commit, ReadInto) or returns (WritePublish)
        std::vector<uint8_t> completion;
        Job                  job;         // Write: carried through to the encode
    };
    struct DefLoad {              // the /d_recv messages of one /d_load or /d_loadDir
        uint32_t             token;
        std::string          cmd;
        std::string          path;
        uint32_t             total;
        uint32_t             remaining;   // replies still to come
        uint32_t             failed;      // a def the engine refused is skipped, as the engine's own
        std::vector<uint8_t> completion;  // directory load skipped it; only nothing at all is a failure
        std::deque<std::vector<uint8_t>> pending;   // defs not yet handed over, in order
        uint32_t             inFlight = 0;           // the batch with the engine, unanswered
        uint32_t             batches = 0;            // batches handed over so far
        uint32_t             batchSentAt = 0;        // the engine's block count when the last one went
        bool                 refused = false;        // failed by a rebuild: its batch's answers are swallowed
    };
    // The next batch to hand over for `token`, if its oldest load has
    // definitions waiting and no batch in flight: as many as fit the share of
    // the callback the last batch took, and at least one, marked in flight.
    // Under mMut.
    bool takeNextDefs(uint32_t token, std::vector<std::vector<uint8_t>>& out);
    // The load whose batch the engine is answering for `token`; end() if
    // none is out. Under mMut.
    std::deque<DefLoad>::iterator defInFlight(uint32_t token);
    void sendDefs(uint32_t token, const std::vector<std::vector<uint8_t>>& defs);

    void run();
    void runJobs();
    void enqueue(Job job);
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
    const Work       mWork;
    Recorder         mRecorder;
    std::string      mSummary;
    std::function<void()> mQuit;

    // loadDefinitions' own asker: a token no transport hands out (theirs are
    // small and non-zero), so its answer is caught in reply() instead of sent.
    static constexpr uint32_t kOwnToken = 0xFFFFFFFFu;
    // The front's own /b_free of what a buffer held before a load replaces it:
    // another token no transport hands out, so everything said back to it,
    // a refusal included, is the front's, and egress() takes it.
    static constexpr uint32_t kFreeToken = 0xFFFFFFFEu;
    std::mutex              mOwnMut;
    std::condition_variable mOwnCv;
    bool                    mOwnAnswered = false;
    std::string             mOwnFailure;   // empty: /done

    std::mutex              mQueueMut;
    std::condition_variable mQueueCv;
    std::deque<Job>         mQueue;
    bool                    mStop = false;

    std::mutex                    mMut;        // everything below
    ClockworkAssetPool*           mPool = nullptr;      // over the assets area
    ClockworkAssetPool*           mOutPool = nullptr;   // over the returns area
    uint32_t                      mPoolBytes = 0, mOutPoolBytes = 0;
    std::map<int32_t, uint32_t>   mLive;       // bufnum → lane slot the guest holds
    std::deque<Pending>           mPending;    // in send order
    std::deque<DefLoad>           mDefLoads;   // in order; a /d_recv reply answers the oldest for its token
    DefBatch                      mLargestDefBatch;          // largestDefBatch()
    uint32_t                      mDefBatchesOverShare = 0;  // defBatchesOverShare()

    // LAST, and it must stay last. The constructor starts this thread (under
    // Work::Own) in its member-init list, and members are constructed in
    // DECLARATION order — so every member run() touches has to be declared
    // above it. Declared any
    // earlier and the worker locks mQueueMut, and reads mStop, before they are
    // constructed: EINVAL out of std::mutex::lock, an uncaught system_error,
    // and a SIGABRT in whichever test happened to be running.
    std::thread                   mWorker;
};

}  // namespace supersonic
