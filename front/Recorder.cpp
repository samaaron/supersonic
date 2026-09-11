// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// See Recorder.h.
#include "Recorder.h"

#include "AudioFormats.h"
#include "ClockworkEngine.h"
#include "clockwork_audio_file.h"
#include "clockwork_client.h"
#include "shm_audio_buffer.hpp"

#include <chrono>
#include <vector>

Recorder::Recorder(ClockworkEngine& engine) : mEngine(engine) {}

Recorder::~Recorder() {
    std::string path, err;
    stop(&path, &err);
}

shm_audio_buffer* Recorder::masterSlot() {
    ClockworkClient* c = mEngine.egressClient();
    if (!c) return nullptr;
    ClockworkRegion taps {};
    if (clockwork_client_region(c, CLOCKWORK_REGION_AUDIO_TAPS, &taps) != CLOCKWORK_OK || !taps.base) return nullptr;
    return static_cast<shm_audio_buffer*>(taps.base) + SHM_AUDIO_MASTER_SLOT;
}

bool Recorder::start(const std::string& path, const std::string& header, int bits, std::string* err) {
    std::lock_guard<std::mutex> lock(mMut);
    if (mRunning.load()) { if (err) *err = "already recording"; return false; }
    shm_audio_buffer* slot = masterSlot();
    if (!slot) { if (err) *err = "the engine publishes no audio taps"; return false; }

    // The tap's own geometry, which is the device's: the file's rate must be
    // the frames'. Live from boot when the device has outputs at all.
    const bool live = slot->enabled.load(std::memory_order_acquire) != 0;
    uint32_t rate = live ? slot->sample_rate : 0;
    if (rate == 0) rate = static_cast<uint32_t>(mEngine.currentDevice().activeSampleRate + 0.5);
    if (rate == 0) rate = 48000;
    mChannels = live && slot->channels > 0 && slot->channels <= SHM_AUDIO_CHANNELS ? slot->channels : SHM_AUDIO_CHANNELS;

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
    mThread = std::thread([this] { run(); });
    return true;
}

bool Recorder::stop(std::string* path, std::string* err) {
    std::lock_guard<std::mutex> lock(mMut);
    if (!mRunning.load()) { if (err) *err = "not recording"; return false; }
    mStop.store(true);
    if (mThread.joinable()) mThread.join();
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


void Recorder::run() {
    // From the live position: a recording is of what happens from now. The
    // tap has been flowing since boot, so "now" is simply the writer's
    // cursor; the one thing that can move it backwards is a device restart,
    // which re-formats the slot (see pullOnce).
    shm_audio_buffer* slot = masterSlot();
    shm_audio_buffer_reader reader(slot);
    reader.seek_to_live();
    std::vector<float> buf(4096 * SHM_AUDIO_CHANNELS);
    auto pullOnce = [&] {
        // The counter went backwards: the slot was re-formatted under us (a
        // device restart). Everything since the reset is the recording's;
        // start from the slot's first frame.
        if (slot && reader.writer_position() < reader.last_read_position()) reader.seek_to_start();
        uint64_t gap = 0;
        const uint32_t n = reader.pull(buf.data(), 4096, &gap);
        if (gap) mLost.fetch_add(gap);
        if (n && mWriter && clockwork_audio_writer_write(mWriter, buf.data(), n) == CLOCKWORK_OK)
            mWritten.fetch_add(n);
        return n;
    };
    while (!mStop.load(std::memory_order_relaxed)) {
        if (pullOnce() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (pullOnce() > 0) {}   // what arrived before the stop
}
