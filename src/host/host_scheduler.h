/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Host-side scheduling logic for the standalone scheduler: parse the control
    vocabulary into timed events, hold them in the generic Scheduler, and at tick
    time dispatch due events through the OscIngress — the same scheduler core and
    fire loop (ss_fire_due) the engine runs, with the host's osc/midi backends in
    place of the synth default. Decoupled from sockets so it is unit-testable:
    ingest() may be called from a recv thread (it only locks the inbox), tick() is
    called from the single scheduler thread that owns the core.
*/

#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "osc_reader.h"
#include "scheduler/Scheduler.h"
#include "scheduler/schedule_parse.h"
#include "scheduler/fire_due.h"
#include "OscIngress.h"

namespace ss_host {

// Per-event metadata. The host has no reply path so `origin` is unused (always
// 0), but the field matches EngineMeta so the shared ss_fire_due loop reads
// ev.meta->origin uniformly across both schedulers.
struct HostMeta { uint32_t origin = 0; };

class HostScheduler {
public:
    explicit HostScheduler(OscIngress& ingress) : mIngress(ingress) {}

    // Parse one control message and queue it. Thread-safe (locks the inbox only);
    // safe to call from the ingress recv thread. Unknown/malformed messages are
    // ignored.
    void ingest(const uint8_t* data, size_t len) {
        OscReader r(data, len);
        if (!r.ok()) return;
        const char* addr = r.address();

        if (std::strcmp(addr, "/schedule") == 0) {
            // "/schedule <timetag> <inner blob>": store the inner message; the
            // outbound side routes it by address (/osc/send or /midi/*).
            SchedulePacket sp = ss_parse_schedule(data, static_cast<uint32_t>(len));
            if (sp.ok)
                push(Command{Command::Schedule, sp.when, SCHED_TAG_DEFAULT,
                             std::vector<uint8_t>(sp.blob, sp.blob + sp.blobLen)});
        } else if (std::strcmp(addr, "/sched/flush") == 0) {
            uint32_t tag = 0;  // empty/missing tag = flush all
            const char* t;
            if (r.peekType() == 's' && r.readString(t) && *t) tag = sched_tag_hash(t, std::strlen(t));
            push(Command{Command::Flush, 0, tag, {}});
        }
    }

    // Apply queued commands, then dispatch every event due at/through `now`
    // through the OscIngress — the same fire loop the engine runs (ss_fire_due),
    // with the host's osc/midi backends in place of the synth default. Single
    // thread only (the core is not shared). No reply path → no call ctx.
    void tick(int64_t now) {
        std::vector<Command> batch;
        { std::lock_guard<std::mutex> lk(mMx); batch.swap(mInbox); }
        for (auto& c : batch) {
            if (c.kind == Command::Flush) {
                mCore.flush(c.tag);
            } else {
                mCore.add(c.when, c.tag, HostMeta{},
                          c.bytes.data(), static_cast<uint32_t>(c.bytes.size()));
            }
        }
        ss_fire_due(mCore, now, /*blockTime*/ 0,
            [this](const uint8_t* d, uint32_t n, uint32_t, int64_t, int64_t) {
                mIngress.ingest(d, n, /*callCtx*/ nullptr);
            });
    }

    int pending() const { return mCore.size(); }

private:
    static constexpr int kSlots    = 256;
    static constexpr int kDataPool = kSlots * 1024;
    using Core = Scheduler<HostMeta, kSlots, kDataPool>;

    struct Command {
        enum Kind { Schedule, Flush } kind;
        int64_t              when;
        uint32_t             tag;
        std::vector<uint8_t> bytes;
    };

    void push(Command&& c) {
        std::lock_guard<std::mutex> lk(mMx);
        mInbox.push_back(std::move(c));
    }

    Core                 mCore;
    OscIngress&          mIngress;
    std::mutex           mMx;
    std::vector<Command> mInbox;
};

}  // namespace ss_host
