// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * Recorder.h — a session recording, as a client makes one.
 *
 * The engine opens no files. The master mix leaves through clockwork's OUT
 * tap — slot CLOCKWORK_TAP_OUT of the arena's audio taps, written at the
 * device edge from boot, held by nobody. A session recording pulls that
 * slot from its live position and writes the frames through clockwork's
 * writer API (clockwork_audio_file.h, a client library): on a thread of its
 * own, or, made without one, whenever its host calls pull(). It asks the
 * guest for nothing; a GUI recording video reads the same slot with its own
 * cursor.
 *
 * The tap is a ring of a second; the thread pulls every couple of
 * milliseconds and a lap is counted, never hidden (framesLost).
 */
#pragma once

#include "shm_audio_buffer.hpp"   // the tap's slot: the engine's wire format, read in place

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ClockworkEngine;
struct ClockworkAudioWriter;

class Recorder {
public:
    // `ownThread`: a recording pulls on a thread started for it. Without, the
    // host pulls (pull()), and stop() pulls what is left on the thread that calls it.
    Recorder(ClockworkEngine& engine, bool ownThread);
    ~Recorder();

    // Opens `path` for `header` ("wav", "flac", …) at `bits` (16, 24, or 32
    // for float) and starts pulling the tap from its live position. False,
    // with `err`, if already recording or the codecs cannot write that.
    bool start(const std::string& path, const std::string& header, int bits, std::string* err);
    // Pulls what is left, closes the file, and says which path it was.
    bool stop(std::string* path, std::string* err);
    // Made without a thread: what the tap has waiting, written to the file on
    // the calling thread. Nothing when not recording, or with a thread.
    void pull();
    bool recording() const { return mRunning.load(); }
    const std::string& path() const { return mPath; }
    uint64_t framesWritten() const { return mWritten.load(); }
    uint64_t framesLost() const { return mLost.load(); }

private:
    void run();
    uint32_t pullOnce();
    void drain();
    shm_audio_buffer* masterSlot();

    ClockworkEngine&      mEngine;
    const bool            mOwnThread;
    std::mutex            mMut;
    std::thread           mThread;
    // The recording's place in the tap: set by start() before anything pulls,
    // then the puller's alone (the thread's, or the host's under mMut).
    shm_audio_buffer*       mSlot = nullptr;
    shm_audio_buffer_reader mReader;
    std::vector<float>      mBuf;
    std::atomic<bool>     mRunning { false };
    std::atomic<bool>     mStop { false };
    ClockworkAudioWriter* mWriter = nullptr;
    std::string           mPath;
    uint32_t              mChannels = 2;
    std::atomic<uint64_t> mWritten { 0 };
    std::atomic<uint64_t> mLost { 0 };
};
