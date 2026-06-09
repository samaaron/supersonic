/*
 * EngineClock.cpp — see EngineClock.h. The clock-core /clock verbs, once.
 */
#include "EngineClock.h"

#include "SuperClock.h"
#include "timeline_osc.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include <cstring>

// THREADING CONTRACT — this handler calls clock mutators (setBpm/setIsPlaying)
// and Link-app-session reads (beatAtLinkTime, numPeers, …) that are NOT
// realtime-safe. Its RT-safety therefore lives in *where it is invoked*, not
// here:
//   • Native (Link compiled in): runs on the NRT gateway thread — `/clock` is
//     forwarded off the audio thread (nrtForwardSink → NRT command ring →
//     EngineControl). Taking Link's lock here is fine.
//   • WASM (no Link): runs inline on the audio thread, but SUPERSONIC_LINK is
//     undefined, so every clock method compiles to a lock-free mirror path.
// Never call this on the native audio thread: it would be an RT violation.
bool handleClockCoreOsc(SuperClock& clock, const uint8_t* data, uint32_t size,
                        const ClockReply& reply) {
    if (size < 8 || std::memcmp(data, "/clock/", 7) != 0) return false;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        // Emit a one-message reply built into a stack buffer.
        auto send = [&](const osc::OutboundPacketStream& s) {
            reply(reinterpret_cast<const uint8_t*>(s.Data()),
                  static_cast<uint32_t>(s.Size()));
        };

        // ── Optional <timeline> segment ──────────────────────────────────
        // /clock/<tl>/<verb> where <tl> ∈ {link, midi, midi:<handle>}. Omitted
        // ⇒ link (back-compat: /clock/tempo/get ≡ /clock/link/tempo/get).
        // Parsed allocation-free (WASM runs this on the audio thread).
        int  id = 0;
        char tlname[64] = {0};   // bare segment ("link"/"midi"/"midi:<handle>"), "" = none
        const char* verb = addr + 7;  // past "/clock/"
        if (const char* slash = std::strchr(verb, '/')) {
            const size_t n = static_cast<size_t>(slash - verb);
            const bool isTl =
                (n == 4 && std::memcmp(verb, "link", 4) == 0) ||
                (n == 4 && std::memcmp(verb, "midi", 4) == 0) ||
                (n >  5 && std::memcmp(verb, "midi:", 5) == 0);
            if (isTl && n + 1 <= sizeof(tlname)) {
                std::memcpy(tlname, verb, n);
                tlname[n] = '\0';
                id = clock.resolveTimeline(tlname);
                verb = slash + 1;
            }
        }

        // Build a "/clock/<tlname>/<suffix>" reply address (omit the segment for
        // the bare/link case so /clock/tempo.reply stays wire-compatible).
        auto replyAddr = [&](const char* suffix, char* out, size_t cap) -> const char* {
            if (tlname[0]) std::snprintf(out, cap, "/clock/%s/%s", tlname, suffix);
            else           std::snprintf(out, cap, "/clock/%s", suffix);
            return out;
        };

        // ── Tempo ────────────────────────────────────────────────────────
        if (std::strcmp(verb, "tempo/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const double bpm = it->AsFloatUnchecked();
            if (id == 0) {
                clock.setBpm(bpm, 0.0);                 // Link timeline
            } else {
                // Manual set on a midi timeline (claim the slot if this port
                // hasn't clocked yet). A live external clock pulse overrides it
                // on its next tick — this is the manual-override / test hook.
                const int wid = clock.resolveOrClaimTimeline(tlname);
                if (wid > 0) clock.setMidiTimelineTempo(wid, bpm);
            }
            return true;
        }
        if (std::strcmp(verb, "tempo/get") == 0) {
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("tempo.reply", ra, sizeof(ra)))
              << clock.timelineBpm(id) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Transport ────────────────────────────────────────────────────
        if (std::strcmp(verb, "transport/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return true;
            const bool playing = it->AsInt32Unchecked() != 0;
            if (id == 0) {
                // Stamp with wallNow — verb carries no timestamp; 0.0 would mean
                // "transitioned at NTP epoch 1900".
                clock.setIsPlaying(playing, clock.wallNow());
            } else {
                // Manual transport on a midi timeline (claim if needed): play →
                // START at beat 0, stop → STOP. A live clock overrides.
                const int wid = clock.resolveOrClaimTimeline(tlname);
                if (wid > 0)
                    clock.setMidiTimelineTransport(wid, playing ? 0 : 2, 0.0);
            }
            return true;
        }
        if (std::strcmp(verb, "transport/get") == 0) {
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("transport.reply", ra, sizeof(ra)))
              << static_cast<int32_t>(clock.timelineIsPlaying(id) ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(verb, "transport/time/get") == 0) {
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("transport/time.reply", ra, sizeof(ra)))
              << static_cast<osc::int64>(clock.timelineTimeForIsPlayingMicros(id)) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── RPC: beat ↔ time conversions in the clock domain ─────────────
        if (std::strcmp(verb, "rpc/beat_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/beat_at_time.reply", ra, sizeof(ra)))
              << clock.timelineBeatAtLinkTime(id, t, q) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(verb, "rpc/phase_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/phase_at_time.reply", ra, sizeof(ra)))
              << clock.timelinePhaseAtLinkTime(id, t, q) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(verb, "rpc/time_at_beat") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float b = it->AsFloatUnchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/time_at_beat.reply", ra, sizeof(ra)))
              << static_cast<osc::int64>(clock.timelineTimeAtBeatLinkMicros(id, b, q)) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Enumerate timelines ──────────────────────────────────────────
        // /clock/timelines/get → /clock/timelines.reply with a flattened
        // [name(s) raw(s) bpm(f) clocking(i) stale(i) primary(i)] per active
        // timeline. `name` is the wire/address identity ("link" /
        // "midi:<handle>"); `raw` is the original OS device name for display.
        if (std::strcmp(verb, "timelines/get") == 0) {
            const auto tls = clock.listTimelines();
            char buf[2048];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/timelines.reply");
            appendTimelineRows(s, tls);
            s << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Start/stop sync (Link-session property; not timeline-scoped) ──
        if (std::strcmp(verb, "start_stop_sync/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                clock.setStartStopSyncEnabled(it->AsInt32Unchecked() != 0);
            return true;
        }
        if (std::strcmp(verb, "start_stop_sync/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/start_stop_sync.reply")
              << static_cast<int32_t>(clock.isStartStopSyncEnabled() ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Queries: enabled / time-now / peer-count (Link-global) ───────
        if (std::strcmp(verb, "enabled/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/enabled.reply")
              << static_cast<int32_t>(clock.isLinkEnabled() ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(verb, "time/now/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/time/now.reply")
              << static_cast<osc::int64>(clock.linkClockMicros()) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(verb, "peers/count/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/peers/count.reply")
              << static_cast<int32_t>(clock.numPeers()) << osc::EndMessage;
            send(s);
            return true;
        }
    } catch (...) {
        return true;  // malformed /clock — consumed, never reaches scsynth
    }
    return false;  // a /clock verb this handler doesn't own (native Link-session)
}
