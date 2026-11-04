// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// See Recorder.h.
#include "Recorder.h"

#include "AudioFormats.h"
#include "AudioThreadWitness.h"
#include "clockwork_audio_file.h"
#include "clockwork_client.h"
#include "clockwork_core.h"
#include "shm_audio_buffer.hpp"

#include <chrono>
#include <vector>

namespace {

// The tap is the core's memory, read only under a hold: a close waits for
// the hold, and refuses one once the core is closing (clockwork_core.h), so
// nothing here reads an arena that is gone.
class CoreHold {
public:
    explicit CoreHold(ClockworkClient* client)
        : mCore(client ? clockwork_client_core(client) : nullptr), mHeld(mCore && clockwork_core_hold(mCore)) {}
    ~CoreHold() { if (mHeld) clockwork_core_unhold(mCore); }
    CoreHold(const CoreHold&) = delete;
    CoreHold& operator=(const CoreHold&) = delete;
    explicit operator bool() const { return mHeld; }
private:
    ClockworkCore* const mCore;
    const bool mHeld;
};

} // namespace

Recorder::Recorder(ClockworkClient* client, bool ownThread)
    : mClient(client), mOwnThread(ownThread) {}

Recorder::~Recorder() {
    std::string path, err;
    stop(&path, &err);
}

// The scope's out slot, where the engine's table puts it, and the ring's
// shape: a core's profile chooses them (clockwork_client_scope_shape).
shm_audio_buffer* Recorder::outSlot(ClockworkScopeShape& shape) {
    ClockworkClient* c = mClient;
    if (!c) return nullptr;
    ClockworkRegion taps {};
    if (clockwork_client_region(c, CLOCKWORK_REGION_SCOPE, &taps) != CLOCKWORK_OK || !taps.base) return nullptr;
    shape = ClockworkScopeShape{};
    shape.struct_bytes = sizeof shape;
    if (clockwork_client_scope_shape(c, &shape) != CLOCKWORK_OK || shape.channels == 0) return nullptr;
    return reinterpret_cast<shm_audio_buffer*>(static_cast<uint8_t*>(taps.base)
                                               + size_t(SCOPE_OUT_SLOT) * shape.slot_bytes);
}

bool Recorder::start(const std::string& path, const std::string& header, int bits, std::string* err) {
    std::lock_guard<std::mutex> lock(mMut);
    if (mRunning.load()) { if (err) *err = "already recording"; return false; }
    ClockworkScopeShape shape {};
    shm_audio_buffer* slot = nullptr;
    uint32_t rate = 0;
    {
        const CoreHold held(mClient);
        if (!held) { if (err) *err = "the engine is closing"; return false; }
        slot = outSlot(shape);
        if (!slot) { if (err) *err = "the engine publishes no audio taps"; return false; }
        // The tap's own geometry, which is the device's: the file's rate must
        // be the frames'. Live from boot when the device has outputs at all.
        const bool live = slot->enabled.load(std::memory_order_acquire) != 0;
        rate = live ? slot->sample_rate : 0;
        mChannels = live && slot->channels > 0 && slot->channels <= shape.channels ? slot->channels : shape.channels;
    }
    if (rate == 0) {
        // The engine's rate, as the handle reports it.
        ClockworkClientInfo info {};
        info.struct_bytes = sizeof info;
        if (clockwork_client_info(mClient, &info) == CLOCKWORK_OK) rate = static_cast<uint32_t>(info.sample_rate + 0.5);
    }
    if (rate == 0) rate = 48000;

    ClockworkAudioWriterConfig cfg {};
    cfg.struct_bytes = sizeof cfg;
    cfg.format      = supersonic_formats::headerFormat(header);
    cfg.encoding    = supersonic_formats::bitDepthEncoding(bits);
    cfg.channels    = mChannels;
    cfg.sample_rate = rate;
    if (!clockwork_audio_can_write(cfg.format, cfg.encoding)) {
        if (err) *err = "unsupported format/bitDepth: " + header + "/" + std::to_string(bits);
        return false;
    }
    ClockworkStatus st = CLOCKWORK_OK;
    audio_thread_witness::fileWork();
    mWriter = clockwork_audio_writer_open(path.c_str(), &cfg, &st);
    if (!mWriter) {
        if (err) *err = "could not open '" + path + "' for writing: " + std::to_string(st);
        return false;
    }
    mPath = path;
    mWritten.store(0);
    mLost.store(0);
    mStop.store(false);
    mRunning.store(true);
    // From the live position as the start is answered, not as the thread first runs: a recording is of what
    // happens from the moment it was said to have started. Taken on the thread, a loaded machine — or a test
    // rendering faster than real time — had the take begin late, its head missing, or all of it.
    mSlot   = slot;
    mReader = shm_audio_buffer_reader(slot, shape.frames, shape.channels);
    mBuf.assign(size_t(kPullFrames) * shape.channels, 0.0f);
    mReader.seek_to_live();
    if (mOwnThread) mThread = std::thread([this] { run(); });
    return true;
}

bool Recorder::stop(std::string* path, std::string* err) {
    std::lock_guard<std::mutex> lock(mMut);
    if (!mRunning.load()) { if (err) *err = "not recording"; return false; }
    mStop.store(true);
    if (mThread.joinable()) mThread.join();   // its last pulls are what arrived before the stop
    else drain();                             // the host's: the same, here
    const ClockworkStatus closed = clockwork_audio_writer_close(mWriter, nullptr, nullptr);
    mWriter = nullptr;
    mRunning.store(false);
    if (path) *path = mPath;
    if (closed != CLOCKWORK_OK) {
        if (err) *err = "'" + mPath + "' could not be finished: " + std::to_string(closed);
        return false;
    }
    return true;
}


void Recorder::pull() {
    std::lock_guard<std::mutex> lock(mMut);
    if (mOwnThread || !mRunning.load()) return;
    drain();
}

// The tap's frames, written until it has none waiting.
void Recorder::drain() {
    while (pullOnce() > 0) {}
}

// `mReader` starts at the writer's cursor as start() answered (above). The tap
// has been flowing since boot, so that cursor is "now"; the one thing that can
// move it backwards is a device restart, which re-formats the slot.
uint32_t Recorder::pullOnce() {
    uint64_t gap = 0;
    uint32_t n = 0;
    {
        // Once the core is closing the tap is gone, and the recording is what
        // was pulled before: the host stops the front first to keep the tail.
        const CoreHold held(mClient);
        if (!held) return 0;
        // The counter went backwards: the slot was re-formatted under us (a
        // device restart). Everything since the reset is the recording's;
        // start from the slot's first frame.
        if (mSlot && mReader.writer_position() < mReader.last_read_position()) mReader.seek_to_start();
        n = mReader.pull(mBuf.data(), kPullFrames, &gap);
    }
    if (gap) mLost.fetch_add(gap);
    if (n) audio_thread_witness::fileWork();
    if (n && mWriter && clockwork_audio_writer_write(mWriter, mBuf.data(), n) == CLOCKWORK_OK)
        mWritten.fetch_add(n);
    return n;
}

void Recorder::run() {
    while (!mStop.load(std::memory_order_relaxed)) {
        if (pullOnce() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    drain();   // what arrived before the stop
}
