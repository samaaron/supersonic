// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// See SuperSonicFront.h.
#include "SuperSonicFront.h"

#include "IOscTransport.h"
#include "ClockworkEngine.h"
#include "buffer_commands.h"
#include "clockwork_asset_pool.h"
#include "clockwork_audio_file.h"
#include "AudioFormats.h"
#include "dsp_api.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>


namespace {

namespace fs = std::filesystem;

bool isMessage(const uint8_t* data, uint32_t size, const char* address) {
    const size_t n = std::strlen(address) + 1;   // the NUL too: "/b_allocRead" is not "/b_allocReadChannel"
    return size >= n && std::memcmp(data, address, n) == 0;
}

// The gap between one /d_recv and the next: a block drains everything that
// arrived since the last one, and each synthdef is parsed on the audio
// thread when it does, so this is what bounds how many a block parses.
constexpr auto kDefRecvGap = std::chrono::milliseconds(1);

// `*` and `?` in a file name, the way scsynth's glob read them.
bool wildcardMatch(const char* pat, const char* str) {
    for (; *pat; ++pat, ++str) {
        if (*pat == '*') {
            for (++pat; *pat == '*'; ++pat) {}
            if (!*pat) return true;
            for (; *str; ++str) if (wildcardMatch(pat, str)) return true;
            return false;
        }
        if (!*str || (*pat != '?' && *pat != *str)) return false;
    }
    return !*str;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::vector<fs::path> synthDefFiles(const std::string& spec, bool directory) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (directory) {
        if (!fs::is_directory(spec, ec)) return files;
        for (fs::recursive_directory_iterator it(spec, ec), end; it != end && !ec; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".scsyndef") files.push_back(it->path());
    } else {
        const fs::path pattern(spec);
        const std::string leaf = pattern.filename().string();
        if (leaf.find_first_of("*?") == std::string::npos) {
            if (fs::is_regular_file(pattern, ec)) files.push_back(pattern);
        } else {
            const fs::path dir = pattern.parent_path().empty() ? fs::path(".") : pattern.parent_path();
            for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".scsyndef") continue;
                if (wildcardMatch(leaf.c_str(), it->path().filename().string().c_str())) files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A decoded file, cut to the frames asked for and, when channels were
// named, to those channels in that order. False: nothing to load, and why.
struct Frames {
    std::vector<float> samples;
    uint32_t           channels = 0;
    int64_t            frames = 0;
    float              rate = 0.f;
};
bool decodeFor(const std::string& path, int32_t start, int32_t want, const std::vector<int32_t>& pick,
               Frames& out, std::string& why) {
    ClockworkAudioInfo info {};
    info.struct_bytes = sizeof info;
    float* decoded = nullptr;
    const ClockworkStatus st = clockwork_audio_decode_file(path.c_str(), &info, &decoded);
    if (st != CLOCKWORK_OK) {
        why = "File '" + path + "' could not be opened: " + std::to_string(st);
        return false;
    }
    const int64_t total = static_cast<int64_t>(info.frames);
    const uint32_t ch   = info.channels;
    int64_t s = start < 0 ? 0 : start;
    if (s > total) s = total;
    int64_t n = want;
    if (n <= 0 || s + n > total) n = total - s;
    for (int32_t c : pick) {
        if (c < 0 || static_cast<uint32_t>(c) >= ch) {
            clockwork_audio_free(decoded);
            why = "Channel " + std::to_string(c) + " is out of range: '" + path + "' has "
                + std::to_string(ch) + " channels";
            return false;
        }
    }
    out.rate     = static_cast<float>(info.sample_rate);
    out.frames   = n < 0 ? 0 : n;
    out.channels = pick.empty() ? ch : static_cast<uint32_t>(pick.size());
    out.samples.resize(static_cast<size_t>(out.frames) * out.channels);
    if (pick.empty()) {
        if (out.frames > 0)
            std::memcpy(out.samples.data(), decoded + static_cast<size_t>(s) * ch,
                        static_cast<size_t>(out.frames) * ch * sizeof(float));
    } else {
        for (int64_t f = 0; f < out.frames; ++f)
            for (size_t k = 0; k < pick.size(); ++k)
                out.samples[static_cast<size_t>(f) * pick.size() + k] =
                    decoded[static_cast<size_t>(s + f) * ch + static_cast<size_t>(pick[k])];
    }
    clockwork_audio_free(decoded);
    return true;
}

} // namespace

SuperSonicFront::SuperSonicFront(ClockworkEngine& engine, IOscTransport* replies)
    : mEngine(engine), mReplies(replies), mRecorder(engine), mWorker([this] { run(); }) {}

SuperSonicFront::~SuperSonicFront() {
    {
        std::lock_guard<std::mutex> lock(mQueueMut);
        mStop = true;
    }
    mQueueCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
    std::lock_guard<std::mutex> lock(mMut);
    if (mPool) clockwork_asset_pool_close(mPool);
    if (mOutPool) clockwork_asset_pool_close(mOutPool);
    mPool = mOutPool = nullptr;
}

// ── Ingress: the file verbs are ours ─────────────────────────────────────────

bool SuperSonicFront::ingress(const uint8_t* data, uint32_t size, uint32_t token) {
    if (recordVerb(data, size, token)) return true;
    Job job;
    job.token = token;
    if (isMessage(data, size, "/b_allocRead"))             job.kind = Kind::AllocRead;
    else if (isMessage(data, size, "/b_allocReadChannel")) job.kind = Kind::AllocReadChannel;
    else if (isMessage(data, size, "/b_read"))             job.kind = Kind::ReadInto;
    else if (isMessage(data, size, "/b_readChannel"))      job.kind = Kind::ReadIntoChannel;
    else if (isMessage(data, size, "/b_write"))            job.kind = Kind::Write;
    else if (isMessage(data, size, "/d_load"))             job.kind = Kind::SynthDefFile;
    else if (isMessage(data, size, "/d_loadDir"))          job.kind = Kind::SynthDefDir;
    else return false;
    try {
        osc::ReceivedMessage m(osc::ReceivedPacket(reinterpret_cast<const char*>(data),
                                                   static_cast<osc::osc_bundle_element_size_t>(size)));
        auto it = m.ArgumentsBegin();
        const auto end = m.ArgumentsEnd();
        auto geti = [&](int32_t dflt) { return (it != end && it->IsInt32()) ? (it++)->AsInt32Unchecked() : dflt; };
        auto gets = [&](const char* dflt) { return (it != end && it->IsString()) ? std::string((it++)->AsStringUnchecked()) : std::string(dflt); };
        switch (job.kind) {
        case Kind::SynthDefFile:
        case Kind::SynthDefDir:
            if (it == end || !it->IsString()) return false;
            job.path = gets("");
            break;
        case Kind::AllocRead:
        case Kind::AllocReadChannel:
            if (it == end || !it->IsInt32()) return false;
            job.bufnum = geti(0);
            if (it == end || !it->IsString()) return false;
            job.path   = gets("");
            job.start  = geti(0);
            job.frames = geti(0);
            break;
        case Kind::ReadInto:
        case Kind::ReadIntoChannel:
            if (it == end || !it->IsInt32()) return false;
            job.bufnum = geti(0);
            if (it == end || !it->IsString()) return false;
            job.path      = gets("");
            job.start     = geti(0);
            job.frames    = geti(-1);
            job.bufStart  = geti(0);
            job.leaveOpen = geti(0);
            break;
        case Kind::Write:
            if (it == end || !it->IsInt32()) return false;
            job.bufnum = geti(0);
            if (it == end || !it->IsString()) return false;
            job.path      = gets("");
            job.header    = gets("aiff");
            job.sample    = gets("int16");
            job.frames    = geti(-1);
            job.bufStart  = geti(0);
            job.leaveOpen = geti(0);
            break;
        case Kind::Encode: return false;
        }
        // The ...Channel verbs: the channels to keep, as many as are given.
        if (job.kind == Kind::AllocReadChannel || job.kind == Kind::ReadIntoChannel)
            while (it != end && it->IsInt32()) job.channels.push_back((it++)->AsInt32Unchecked());
        if (it != end && it->IsBlob()) {
            const void* blob = nullptr;
            osc::osc_bundle_element_size_t n = 0;
            it->AsBlobUnchecked(blob, n);
            if (blob && n > 0) job.completion.assign(static_cast<const uint8_t*>(blob),
                                                     static_cast<const uint8_t*>(blob) + n);
        }
    } catch (const osc::Exception&) {
        return false;   // malformed: the engine's refusal says what is wrong
    }
    {
        std::lock_guard<std::mutex> lock(mQueueMut);
        mQueue.push_back(std::move(job));
    }
    mQueueCv.notify_one();
    return true;
}

// ── The worker ───────────────────────────────────────────────────────────────

void SuperSonicFront::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mQueueMut);
            mQueueCv.wait(lock, [this] { return mStop || !mQueue.empty(); });
            if (mStop) return;
            job = std::move(mQueue.front());
            mQueue.pop_front();
        }
        switch (job.kind) {
        case Kind::AllocRead: case Kind::AllocReadChannel:
        case Kind::ReadInto:  case Kind::ReadIntoChannel:   loadSample(job); break;
        case Kind::Write:                                    startWrite(job); break;
        case Kind::Encode:                                   encode(job);     break;
        case Kind::SynthDefFile: case Kind::SynthDefDir:     loadDefs(job);   break;
        }
    }
}

bool SuperSonicFront::ensurePools() {
    const uint32_t in = mEngine.guestInboxBytes(), out = mEngine.guestOutboxBytes();
    if (!mEngine.guestInbox() || in == 0) return false;
    if (!mPool || mPoolBytes != in) {
        if (mPool) clockwork_asset_pool_close(mPool);   // a different lane: start over
        mPool = clockwork_asset_pool_open(in);
        mPoolBytes = mPool ? in : 0;
        mLive.clear();
    }
    if (out > 0 && (!mOutPool || mOutPoolBytes != out)) {
        if (mOutPool) clockwork_asset_pool_close(mOutPool);
        mOutPool = clockwork_asset_pool_open(out);
        mOutPoolBytes = mOutPool ? out : 0;
    }
    return mPool != nullptr;
}

bool SuperSonicFront::takePending(Pending::What what, uint32_t token, int32_t bufnum, Pending* out) {
    for (auto it = mPending.begin(); it != mPending.end(); ++it) {
        if (it->what != what || it->token != token || it->bufnum != bufnum) continue;
        *out = std::move(*it);
        mPending.erase(it);
        return true;
    }
    return false;
}

// /b_allocRead, /b_allocReadChannel, /b_read, /b_readChannel: decode, stage in
// the lane, and hand over — as an asset that becomes the buffer, or as a range
// the guest copies into one.
void SuperSonicFront::loadSample(const Job& job) {
    const bool alloc = job.kind == Kind::AllocRead || job.kind == Kind::AllocReadChannel;
    const char* cmd = job.kind == Kind::AllocRead        ? "/b_allocRead"
                    : job.kind == Kind::AllocReadChannel ? "/b_allocReadChannel"
                    : job.kind == Kind::ReadInto         ? "/b_read" : "/b_readChannel";
    if (!alloc && job.leaveOpen) {
        sendFail(job.token, cmd, std::string(cmd) + " leaveOpen is not available: this engine does not stream from disk", job.bufnum);
        return;
    }
    Frames fr;
    std::string why;
    if (!decodeFor(job.path, job.start, job.frames, job.channels, fr, why)) {
        sendFail(job.token, cmd, why, job.bufnum);
        return;
    }
    if (alloc && (fr.frames <= 0 || fr.channels == 0)) {
        sendFail(job.token, cmd, "File '" + job.path + "' has no frames in the requested range", job.bufnum);
        return;
    }

    const uint32_t guardBefore = alloc ? supersonic_buffer_guard_before() : 0;
    const uint32_t guardAfter  = alloc ? supersonic_buffer_guard_after()  : 0;
    const uint64_t frameBytes  = static_cast<uint64_t>(fr.channels) * sizeof(float);
    const uint64_t slotBytes64 = (guardBefore + static_cast<uint64_t>(fr.frames) + guardAfter) * frameBytes;
    const uint32_t need        = slotBytes64 < 16 ? 16 : static_cast<uint32_t>(slotBytes64);
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(mMut);
        if (!ensurePools()) {
            sendFail(job.token, cmd, "the engine has no inbox lane to load into", job.bufnum);
            return;
        }
        if (slotBytes64 > 0xFFFFFFFFull || clockwork_asset_pool_alloc(mPool, need, &slot) != 0) {
            sendFail(job.token, cmd, "inbox lane full: " + std::to_string(slotBytes64) + " bytes needed, "
                     + std::to_string(clockwork_asset_pool_free_bytes(mPool)) + " free", job.bufnum);
            return;
        }
        if (alloc) {
            // The buffer is replaced, as /b_allocRead always replaced: whatever
            // it held goes first, and the guest lets its asset go with it. The
            // reply to that free is ours to swallow.
            mFreeing[job.bufnum] += 1;
        }
        Pending p { alloc ? Pending::Commit : Pending::ReadInto, job.token, job.bufnum, cmd, slot, job.completion, {} };
        mPending.push_back(std::move(p));
    }

    auto* lane = const_cast<uint8_t*>(mEngine.guestInbox());
    const uint32_t payload = slot + static_cast<uint32_t>(guardBefore * frameBytes);
    if (alloc) std::memset(lane + slot, 0, static_cast<size_t>(slotBytes64));
    if (!fr.samples.empty()) std::memcpy(lane + payload, fr.samples.data(), fr.samples.size() * sizeof(float));

    char buf[256];
    if (alloc) {
        {
            osc::OutboundPacketStream p(buf, sizeof buf);
            p << osc::BeginMessage("/b_free") << job.bufnum << osc::EndMessage;
            mEngine.ingest(reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()), job.token);
        }
        osc::OutboundPacketStream p(buf, sizeof buf);
        p << osc::BeginMessage("/clockwork/asset/commit")
          << job.bufnum << static_cast<int32_t>(CLOCKWORK_ASSET_AUDIO_F32)
          << static_cast<int32_t>(payload) << static_cast<int32_t>(fr.frames * frameBytes)
          << static_cast<int32_t>(fr.channels) << static_cast<int32_t>(fr.frames) << fr.rate
          << osc::EndMessage;
        mEngine.ingest(reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()), job.token);
    } else {
        osc::OutboundPacketStream p(buf, sizeof buf);
        p << osc::BeginMessage("/supersonic/buffer/read")
          << job.bufnum << job.bufStart << static_cast<int32_t>(payload)
          << static_cast<int32_t>(fr.frames) << static_cast<int32_t>(fr.channels) << fr.rate
          << osc::EndMessage;
        mEngine.ingest(reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()), job.token);
    }
}

// /b_write, first half: the format is checked here at once (as it was checked
// at Init), then the buffer is asked about, and its /b_info carries on below.
void SuperSonicFront::startWrite(const Job& job) {
    if (job.leaveOpen) {
        sendFail(job.token, "/b_write", "/b_write leaveOpen is not available: this engine does not stream to disk", job.bufnum);
        return;
    }
    Job carried = job;
    carried.format   = supersonic_formats::headerFormat(job.header);
    carried.encoding = supersonic_formats::sampleEncoding(job.sample);
    if (!clockwork_audio_can_write(carried.format, carried.encoding)) {
        sendFail(job.token, "/b_write", "Cannot write '" + job.path + "' as " + job.header + " " + job.sample, job.bufnum);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mMut);
        if (!ensurePools() || !mOutPool) {
            sendFail(job.token, "/b_write", "the engine has no outbox lane to publish into", job.bufnum);
            return;
        }
        Pending p { Pending::WriteQuery, job.token, job.bufnum, "/b_write", 0, job.completion, carried };
        mPending.push_back(std::move(p));
    }
    char buf[64];
    osc::OutboundPacketStream p(buf, sizeof buf);
    p << osc::BeginMessage("/b_query") << job.bufnum << osc::EndMessage;
    mEngine.ingest(reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()), job.token);
}

// /b_write, last half: the frames are in the outbox; encode them to the file.
void SuperSonicFront::encode(const Job& job) {
    ClockworkAudioWriterConfig cfg {};
    cfg.struct_bytes = sizeof cfg;
    cfg.format      = job.format;
    cfg.encoding    = job.encoding;
    cfg.channels    = job.outChannels;
    cfg.sample_rate = static_cast<uint32_t>(job.outRate + 0.5f);
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkAudioWriter* w = clockwork_audio_writer_open(job.path.c_str(), &cfg, &st);
    if (w) {
        if (job.outFrames > 0) {
            const auto* frames = reinterpret_cast<const float*>(mEngine.guestOutbox() + job.outSlot);
            st = clockwork_audio_writer_write(w, frames, job.outFrames);
        }
        const ClockworkStatus closed = clockwork_audio_writer_close(w, nullptr, nullptr);
        if (st == CLOCKWORK_OK) st = closed;
    }
    {
        std::lock_guard<std::mutex> lock(mMut);
        if (mOutPool && job.outFrames > 0) clockwork_asset_pool_free(mOutPool, job.outSlot);
    }
    if (!w) {
        sendFail(job.token, "/b_write", "File '" + job.path + "' could not be opened: " + std::to_string(st), job.bufnum);
        return;
    }
    if (st != CLOCKWORK_OK) {
        sendFail(job.token, "/b_write", "File '" + job.path + "' could not be written: " + std::to_string(st), job.bufnum);
        return;
    }
    finish(job.token, "/b_write", job.bufnum, job.completion);
}

// /d_load, /d_loadDir: read the synthdefs, hand each over as /d_recv.
void SuperSonicFront::loadDefs(const Job& job) {
    const char* cmd = job.kind == Kind::SynthDefDir ? "/d_loadDir" : "/d_load";
    std::vector<std::vector<uint8_t>> defs;
    for (const fs::path& p : synthDefFiles(job.path, job.kind == Kind::SynthDefDir)) {
        std::vector<uint8_t> bytes;
        if (readFile(p, bytes)) defs.push_back(std::move(bytes));
    }
    if (defs.empty()) {
        sendFailCmd(job.token, cmd, job.path);   // as the engine said it: the path, not a sentence
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mMut);
        const auto n = static_cast<uint32_t>(defs.size());
        mDefLoads.push_back({ job.token, cmd, job.path, n, n, 0, job.completion });
    }
    std::vector<char> buf;
    for (size_t i = 0; i < defs.size(); ++i) {
        buf.resize(defs[i].size() + 64);
        osc::OutboundPacketStream p(buf.data(), buf.size());
        p << osc::BeginMessage("/d_recv")
          << osc::Blob(defs[i].data(), static_cast<osc::osc_bundle_element_size_t>(defs[i].size()))
          << osc::EndMessage;
        mEngine.ingest(reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()), job.token);
        if (i + 1 < defs.size()) std::this_thread::sleep_for(kDefRecvGap);
    }
}

// ── Egress: the engine's replies to what the front sent ──────────────────────

bool SuperSonicFront::egress(uint32_t token, const uint8_t* data, uint32_t size) {
    if (size < 8 || data[0] != '/') return false;
    enum { Committed, Refused, Released, Done, Fail, Info, Published, None } kind = None;
    if      (isMessage(data, size, "/clockwork/asset/committed"))   kind = Committed;
    else if (isMessage(data, size, "/clockwork/asset/refused"))     kind = Refused;
    else if (isMessage(data, size, "/clockwork/asset/released"))    kind = Released;
    else if (isMessage(data, size, "/done"))                        kind = Done;
    else if (isMessage(data, size, "/fail"))                        kind = Fail;
    else if (isMessage(data, size, "/b_info"))                      kind = Info;
    else if (isMessage(data, size, "/supersonic/buffer/published")) kind = Published;
    if (kind == None) return false;

    int32_t     id = -1, a1 = 0, a2 = 0;
    float       f3 = 0.f;
    std::string cmd, why;
    try {
        osc::ReceivedMessage m(osc::ReceivedPacket(reinterpret_cast<const char*>(data),
                                                   static_cast<osc::osc_bundle_element_size_t>(size)));
        auto it = m.ArgumentsBegin();
        const auto end = m.ArgumentsEnd();
        if (kind == Done || kind == Fail) {
            if (it == end || !it->IsString()) return false;
            cmd = (it++)->AsStringUnchecked();
            if (kind == Fail && it != end && it->IsString()) why = (it++)->AsStringUnchecked();
            if (it != end && it->IsInt32()) id = it->AsInt32Unchecked();
        } else {
            if (it == end || !it->IsInt32()) return false;
            id = (it++)->AsInt32Unchecked();
            if (kind == Refused && it != end && it->IsString()) why = it->AsStringUnchecked();
            if (kind == Info || kind == Published) {
                if (it != end && it->IsInt32()) a1 = (it++)->AsInt32Unchecked();
                if (it != end && it->IsInt32()) a2 = (it++)->AsInt32Unchecked();
                if (it != end && it->IsFloat()) f3 = it->AsFloatUnchecked();
            }
        }
    } catch (const osc::Exception&) {
        return false;
    }

    // /d_recv replies: a load in flight for that token, oldest first.
    if ((kind == Done || kind == Fail) && cmd == "/d_recv") {
        DefLoad finished;
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(mMut);
            auto it = mDefLoads.begin();
            for (; it != mDefLoads.end(); ++it) if (it->token == token) break;
            if (it == mDefLoads.end()) return false;
            if (kind == Fail) ++it->failed;
            if (--it->remaining == 0) { finished = std::move(*it); mDefLoads.erase(it); last = true; }
        }
        if (last) {
            if (finished.failed == finished.total) {
                sendFailCmd(token, finished.cmd.c_str(), finished.path);
            } else {
                if (!finished.completion.empty())
                    mEngine.ingest(finished.completion.data(), static_cast<uint32_t>(finished.completion.size()), token);
                sendDoneCmd(token, finished.cmd.c_str());
            }
        }
        return true;
    }

    // /b_free replies: ours if we issued one for that buffer (a failed free
    // names no buffer; the oldest of ours is the one it answers).
    if ((kind == Done || kind == Fail) && cmd == "/b_free") {
        std::lock_guard<std::mutex> lock(mMut);
        auto it = id >= 0 ? mFreeing.find(id) : mFreeing.begin();
        if (it == mFreeing.end()) return false;
        if (--it->second == 0) mFreeing.erase(it);
        return true;
    }

    // /supersonic/buffer/read replies: the /b_read or /b_readChannel they answer.
    if ((kind == Done || kind == Fail) && cmd == "/supersonic/buffer/read") {
        Pending p;
        {
            std::lock_guard<std::mutex> lock(mMut);
            if (!takePending(Pending::ReadInto, token, id, &p)) return false;
            if (mPool) clockwork_asset_pool_free(mPool, p.slot);   // copied: the lane range is free again
        }
        if (kind == Done) finish(token, p.cmd.c_str(), id, p.completion);
        else              sendFail(token, p.cmd.c_str(), why, id);
        return true;
    }

    // /supersonic/buffer/publish failed: the /b_write it answers.
    if (kind == Fail && cmd == "/supersonic/buffer/publish") {
        Pending p;
        {
            std::lock_guard<std::mutex> lock(mMut);
            if (!takePending(Pending::WritePublish, token, id, &p)) return false;
            if (mOutPool && p.job.outFrames > 0) clockwork_asset_pool_free(mOutPool, p.slot);
        }
        sendFail(token, "/b_write", why, id);
        return true;
    }
    if (kind == Done || kind == Fail) return false;

    if (kind == Released) {
        std::lock_guard<std::mutex> lock(mMut);
        auto it = mLive.find(id);
        if (it == mLive.end()) return false;   // not a buffer of ours
        if (mPool) clockwork_asset_pool_free(mPool, it->second);
        mLive.erase(it);
        return true;
    }

    if (kind == Committed || kind == Refused) {
        Pending p;
        {
            std::lock_guard<std::mutex> lock(mMut);
            if (!takePending(Pending::Commit, token, id, &p)) return false;
            if (kind == Committed) mLive[id] = p.slot;
            else if (mPool)        clockwork_asset_pool_free(mPool, p.slot);
        }
        if (kind == Committed) finish(token, p.cmd.c_str(), id, p.completion);
        else                   sendFail(token, p.cmd.c_str(), why.empty() ? "the engine refused the sample" : why, id);
        return true;
    }

    // /b_info for a /b_write in flight: now the range is known, ask the guest
    // to publish it into the outbox.
    if (kind == Info) {
        Pending p;
        uint32_t slot = 0;
        int32_t start = 0, n = 0;
        {
            std::lock_guard<std::mutex> lock(mMut);
            if (!takePending(Pending::WriteQuery, token, id, &p)) return false;
            const int32_t frames = a1, channels = a2;
            if (channels <= 0) {
                // Nothing to publish; fall through to an empty file below.
            }
            start = p.job.bufStart < 0 ? 0 : p.job.bufStart;
            int32_t toEnd = frames - start;
            if (toEnd < 0) toEnd = 0;
            n = p.job.frames < 0 || p.job.frames > toEnd ? toEnd : p.job.frames;
            p.job.outChannels = channels <= 0 ? 1 : static_cast<uint32_t>(channels);
            p.job.outFrames   = static_cast<uint32_t>(n);
            p.job.outRate     = f3;
            if (n > 0) {
                const uint64_t bytes = static_cast<uint64_t>(n) * p.job.outChannels * sizeof(float);
                if (!mOutPool || bytes > 0xFFFFFFFFull || clockwork_asset_pool_alloc(mOutPool, static_cast<uint32_t>(bytes), &slot) != 0) {
                    sendFail(token, "/b_write", "outbox lane full: " + std::to_string(bytes) + " bytes needed", id);
                    return true;
                }
                p.job.outSlot = slot;
                p.what = Pending::WritePublish;
                p.slot = slot;
                mPending.push_back(p);
            }
        }
        if (n <= 0) {
            // An empty range writes an empty file, as /b_write did.
            Job enc = p.job;
            enc.kind = Kind::Encode;
            std::lock_guard<std::mutex> lock(mQueueMut);
            mQueue.push_back(std::move(enc));
            mQueueCv.notify_one();
            return true;
        }
        char buf[128];
        osc::OutboundPacketStream o(buf, sizeof buf);
        o << osc::BeginMessage("/supersonic/buffer/publish") << id << start << n << static_cast<int32_t>(slot)
          << osc::EndMessage;
        mEngine.ingest(reinterpret_cast<const uint8_t*>(o.Data()), static_cast<uint32_t>(o.Size()), token);
        return true;
    }

    if (kind == Published) {
        Pending p;
        {
            std::lock_guard<std::mutex> lock(mMut);
            if (!takePending(Pending::WritePublish, token, id, &p)) return false;
        }
        Job enc = p.job;
        enc.kind        = Kind::Encode;
        enc.outChannels = static_cast<uint32_t>(a1);
        enc.outFrames   = static_cast<uint32_t>(a2);
        enc.outRate     = f3;
        {
            std::lock_guard<std::mutex> lock(mQueueMut);
            mQueue.push_back(std::move(enc));
        }
        mQueueCv.notify_one();
        return true;
    }
    return false;
}

// ── Session recording: the master tap, pulled into a file here ──────────────
//
// /clockwork/record/start <path> [<format> [<bits>]] and /clockwork/record/stop,
// answered at once — opening a file is the receive thread's to do — in the
// shape the engine used to answer them: record/start.reply <ok> <path|error>.

bool SuperSonicFront::recordVerb(const uint8_t* data, uint32_t size, uint32_t token) {
    const bool start = isMessage(data, size, "/clockwork/record/start");
    const bool stop  = isMessage(data, size, "/clockwork/record/stop");
    if (!start && !stop) return false;
    std::string path, header = "wav", err;
    int bits = 24;
    if (start) {
        try {
            osc::ReceivedMessage m(osc::ReceivedPacket(reinterpret_cast<const char*>(data),
                                                       static_cast<osc::osc_bundle_element_size_t>(size)));
            auto it = m.ArgumentsBegin();
            const auto end = m.ArgumentsEnd();
            if (it != end && it->IsString()) path = (it++)->AsStringUnchecked();
            if (it != end && it->IsString()) header = (it++)->AsStringUnchecked();
            if (it != end && it->IsInt32())  bits = it->AsInt32Unchecked();
        } catch (const osc::Exception&) {
            return false;
        }
    }
    const bool ok = start ? mRecorder.start(path, header, bits, &err) : mRecorder.stop(&path, &err);
    std::vector<char> buf(path.size() + err.size() + 128);
    osc::OutboundPacketStream p(buf.data(), buf.size());
    p << osc::BeginMessage(start ? "/clockwork/record/start.reply" : "/clockwork/record/stop.reply")
      << static_cast<int32_t>(ok ? 1 : 0) << (ok ? path.c_str() : err.c_str()) << osc::EndMessage;
    reply(token, reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()));
    if (ok) fprintf(stderr, "[recording] %s: %s\n", start ? "started" : "stopped", path.c_str());
    return true;
}

// ── Replies, in scsynth's words ──────────────────────────────────────────────

void SuperSonicFront::reply(uint32_t token, const uint8_t* data, uint32_t size) {
    if (mReplies) mReplies->send(token, data, size, /*networkOnly*/ false);
}

void SuperSonicFront::finish(uint32_t token, const char* cmd, int32_t bufnum, const std::vector<uint8_t>& completion) {
    if (!completion.empty())
        mEngine.ingest(completion.data(), static_cast<uint32_t>(completion.size()), token);
    sendDone(token, cmd, bufnum);
}

void SuperSonicFront::sendDone(uint32_t token, const char* cmd, int32_t bufnum) {
    char buf[128];
    osc::OutboundPacketStream p(buf, sizeof buf);
    p << osc::BeginMessage("/done") << cmd << bufnum << osc::EndMessage;
    reply(token, reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()));
}

void SuperSonicFront::sendFail(uint32_t token, const char* cmd, const std::string& why, int32_t bufnum) {
    std::vector<char> buf(why.size() + 128);
    osc::OutboundPacketStream p(buf.data(), buf.size());
    p << osc::BeginMessage("/fail") << cmd << why.c_str() << bufnum << osc::EndMessage;
    reply(token, reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()));
}

void SuperSonicFront::sendDoneCmd(uint32_t token, const char* cmd) {
    char buf[128];
    osc::OutboundPacketStream p(buf, sizeof buf);
    p << osc::BeginMessage("/done") << cmd << osc::EndMessage;
    reply(token, reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()));
}

void SuperSonicFront::sendFailCmd(uint32_t token, const char* cmd, const std::string& what) {
    std::vector<char> buf(what.size() + 128);
    osc::OutboundPacketStream p(buf.data(), buf.size());
    p << osc::BeginMessage("/fail") << cmd << what.c_str() << osc::EndMessage;
    reply(token, reinterpret_cast<const uint8_t*>(p.Data()), static_cast<uint32_t>(p.Size()));
}

uint32_t SuperSonicFront::laneBytes() {
    std::lock_guard<std::mutex> lock(mMut);
    return ensurePools() ? mPoolBytes : 0;
}

uint32_t SuperSonicFront::laneFreeBytes() {
    std::lock_guard<std::mutex> lock(mMut);
    return ensurePools() ? clockwork_asset_pool_free_bytes(mPool) : 0;
}

uint32_t SuperSonicFront::outboxFreeBytes() {
    std::lock_guard<std::mutex> lock(mMut);
    return (ensurePools() && mOutPool) ? clockwork_asset_pool_free_bytes(mOutPool) : 0;
}

// The product hook Main.cpp calls: SuperSonic's front is this one.
std::unique_ptr<OscFront> clockwork_product_front(ClockworkEngine& engine, IOscTransport* transport) {
    return std::unique_ptr<OscFront>(new SuperSonicFront(engine, transport));
}
