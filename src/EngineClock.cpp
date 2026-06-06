/*
 * EngineClock.cpp — see EngineClock.h. The clock-core /clock verbs, once.
 */
#include "EngineClock.h"

#include "SuperClock.h"
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

        // ── Tempo ────────────────────────────────────────────────────────
        if (std::strcmp(addr, "/clock/tempo/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsFloat())
                clock.setBpm(it->AsFloatUnchecked(), 0.0);
            return true;
        }
        if (std::strcmp(addr, "/clock/tempo/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/tempo.reply") << clock.getBpm() << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Transport ────────────────────────────────────────────────────
        if (std::strcmp(addr, "/clock/transport/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                // Stamp with wallNow — verb carries no timestamp; 0.0 would mean
                // "transitioned at NTP epoch 1900".
                clock.setIsPlaying(it->AsInt32Unchecked() != 0, clock.wallNow());
            return true;
        }
        if (std::strcmp(addr, "/clock/transport/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/transport.reply")
              << static_cast<int32_t>(clock.isPlaying() ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(addr, "/clock/transport/time/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/transport/time.reply")
              << static_cast<osc::int64>(clock.timeForIsPlayingMicros()) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Start/stop sync ──────────────────────────────────────────────
        if (std::strcmp(addr, "/clock/start_stop_sync/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                clock.setStartStopSyncEnabled(it->AsInt32Unchecked() != 0);
            return true;
        }
        if (std::strcmp(addr, "/clock/start_stop_sync/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/start_stop_sync.reply")
              << static_cast<int32_t>(clock.isStartStopSyncEnabled() ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── Queries: enabled / time-now / peer-count ─────────────────────
        if (std::strcmp(addr, "/clock/enabled/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/enabled.reply")
              << static_cast<int32_t>(clock.isLinkEnabled() ? 1 : 0) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(addr, "/clock/time/now/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/time/now.reply")
              << static_cast<osc::int64>(clock.linkClockMicros()) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(addr, "/clock/peers/count/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/peers/count.reply")
              << static_cast<int32_t>(clock.numPeers()) << osc::EndMessage;
            send(s);
            return true;
        }

        // ── RPC: beat ↔ time conversions in the clock domain ─────────────
        if (std::strcmp(addr, "/clock/rpc/beat_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/rpc/beat_at_time.reply")
              << clock.beatAtLinkTime(t, q) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(addr, "/clock/rpc/phase_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/rpc/phase_at_time.reply")
              << clock.phaseAtLinkTime(t, q) << osc::EndMessage;
            send(s);
            return true;
        }
        if (std::strcmp(addr, "/clock/rpc/time_at_beat") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float b = it->AsFloatUnchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/rpc/time_at_beat.reply")
              << static_cast<osc::int64>(clock.timeAtBeatLinkMicros(b, q)) << osc::EndMessage;
            send(s);
            return true;
        }
    } catch (...) {
        return true;  // malformed /clock — consumed, never reaches scsynth
    }
    return false;  // a /clock verb this handler doesn't own (native Link-session)
}
