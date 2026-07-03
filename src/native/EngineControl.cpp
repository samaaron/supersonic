/*
 * EngineControl.cpp — the supersonic/ and link/ control endpoints. Replies and
 * subscriber pushes go through the egress hub (mEgress); device/driver/record/
 * link state comes from the engine + SuperClock.
 */
#include "EngineControl.h"

#include "EngineClock.h"
#include "OscEgress.h"
#include "SupersonicEngine.h"
#include "src/SuperClock.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include "DevicePolicy.h"
#include "supersonic_config.h"  // SUPERSONIC_VERSION_MAJOR / _MINOR
#ifdef __APPLE__
#include "AggregateDeviceHelper.h"
#include "JuceAudioCallback.h"  // get_audio_first_private_bus_idx()
#endif
#include <juce_core/juce_core.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

bool EngineControl::handleLinkCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (!mSuperClock || size < 16) return false;
    if (std::memcmp(data, "/clock/", 7) != 0) return false;
    // Clock-core verbs (tempo/transport/sync/rpc/queries) go through the shared
    // handler — the same one the web worklet uses. Native-only Link-session
    // verbs (visibility, peers, Link Audio, notify) fall through to below.
    if (handleClockCoreOsc(*mSuperClock, data, size,
            [this, token](const uint8_t* d, uint32_t n) { mEgress->reply(token, d, n); }))
        return true;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        // Correlation-token echo, same convention as EngineClock.cpp: a
        // request may carry an int32 as its FINAL argument; the reply echoes
        // it as ITS final argument so clients can match replies exactly.
        int32_t echoToken = 0;
        bool    hasEchoToken = false;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            auto next = it; ++next;
            if (next == msg.ArgumentsEnd() && it->IsInt32()) {
                hasEchoToken = true;
                echoToken = it->AsInt32Unchecked();
            }
        }

        if (std::strcmp(addr, "/clock/visibility") == 0) {
            // Write: /clock/visibility <int 0|1|2> = Off | LoopbackOnly | NetworkWide.
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return false;
            const int32_t mode = it->AsInt32Unchecked();
            using V = SuperClock::LinkVisibility;
            switch (mode) {
            case 0: mSuperClock->setLinkVisibility(V::Off); break;
            case 1: mSuperClock->setLinkVisibility(V::LoopbackOnly); break;
            case 2: mSuperClock->setLinkVisibility(V::NetworkWide); break;
            default: return false;
            }
            return true;
        }

        if (std::strcmp(addr, "/clock/visibility/get") == 0) {
            // Read: reply /clock/visibility.reply <int> to the sender.
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/visibility.reply")
              << static_cast<int32_t>(mSuperClock->getLinkVisibility());
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/publish/set") == 0) {
            // Write: /clock/audio/publish/set <int 0|1>.
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return false;
            mSuperClock->setLinkAudioPublish(it->AsInt32Unchecked() != 0);
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/publish/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/audio/publish.reply")
              << static_cast<int32_t>(mSuperClock->isLinkAudioPublishEnabled() ? 1 : 0);
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/peer_name/set") == 0) {
            // Write: /clock/peer_name/set <string>. Identifies us to other
            // Link peers (replaces the default engine name).
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsString()) return false;
            mSuperClock->setPeerName(it->AsStringUnchecked());
            return true;
        }

        if (std::strcmp(addr, "/clock/peer_name/get") == 0) {
            char buf[512];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/peer_name.reply")
              << mSuperClock->peerName();
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/channels/get") == 0) {
            // Reply /clock/audio/channels.reply <count> [channelId channelName
            //   peerId peerName] * count.
            auto chs = mSuperClock->listLinkAudioChannels();
            std::vector<char> buf(8192);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage("/clock/audio/channels.reply")
              << static_cast<int32_t>(chs.size());
            for (const auto& c : chs) {
                s << c.channelId.c_str() << c.channelName.c_str()
                  << c.peerId.c_str() << c.peerName.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/input/add") == 0) {
            // /clock/audio/input/add <peerName:string> <channelName:string>
            //   <busIdx:int32>
            // Reply /clock/audio/input/add.reply <success:int 0|1>.
            // One Link channel into one bus; multiple subscriptions
            // run concurrently. Adding (peerName, channelName) twice
            // replaces the prior entry.
            // busIdx must point into the PRIVATE region of scsynth's
            // bus pool — the audio thread writes (busIdx, busIdx+1)
            // unconditionally, so a low busIdx would clobber the
            // engine's hardware output / input buses.
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            int32_t busIdx = -1;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                busIdx = it->AsInt32Unchecked(); ++it;
            }
            const int firstPrivate = get_audio_first_private_bus_idx();
            const int busCount     = get_audio_bus_count();
            // Each sub claims (busIdx, busIdx+1); reject below the
            // private region and reject if either bus is out of range.
            const bool inRange = busCount > 0 && busIdx >= firstPrivate
                              && busIdx + 1 < busCount;
            const bool ok = peerName && channelName && inRange
                && mSuperClock->addLinkAudioInput(peerName, channelName,
                                                  static_cast<uint32_t>(busIdx));
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/audio/input/add.reply")
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/input/remove") == 0) {
            // /clock/audio/input/remove <peerName:string> <channelName:string>
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (peerName && channelName) {
                mSuperClock->removeLinkAudioInput(peerName, channelName);
            }
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/input/clear") == 0) {
            mSuperClock->clearLinkAudioInputs();
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/input/latency/set") == 0) {
            // /clock/audio/input/latency/set <peer:str> <chan:str> <seconds:float>
            // Equivalent to Live's per-track latency slider (0..2 s).
            // Reply /clock/audio/input/latency/set.reply <success:int 0|1>.
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            float seconds = -1.0f;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsFloat()) {
                seconds = it->AsFloatUnchecked(); ++it;
            }
            const bool ok = peerName && channelName && seconds >= 0.0f
                && mSuperClock->setLinkAudioInputLatencySeconds(
                       peerName, channelName, seconds);
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/audio/input/latency/set.reply")
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/sink/add") == 0) {
            // /clock/audio/sink/add <name:string> <busIdx:int> <numChannels:int>
            // → /clock/audio/sink/add.reply <success:int 0|1>
            auto it = msg.ArgumentsBegin();
            const char* name = nullptr;
            int32_t busIdx = -1;
            int32_t numChans = -1;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                name = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                busIdx = it->AsInt32Unchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                numChans = it->AsInt32Unchecked(); ++it;
            }
            const bool ok = name && busIdx >= 0 && numChans > 0
                && mSuperClock->addLinkAudioSink(name,
                                                  static_cast<uint32_t>(busIdx),
                                                  static_cast<uint32_t>(numChans));
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/clock/audio/sink/add.reply")
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/sink/remove") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                mSuperClock->removeLinkAudioSink(it->AsStringUnchecked());
            }
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/sinks/get") == 0) {
            // Reply /clock/audio/sinks.reply <count>
            //   [name busIdx numChans hasSubscriber:int 0|1]*
            auto sinks = mSuperClock->listLinkAudioSinks();
            std::vector<char> buf(4096);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage("/clock/audio/sinks.reply")
              << static_cast<int32_t>(sinks.size());
            for (const auto& as : sinks) {
                s << as.name.c_str()
                  << static_cast<int32_t>(as.busIdx)
                  << static_cast<int32_t>(as.numChannels)
                  << static_cast<int32_t>(as.hasSubscriber ? 1 : 0);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, "/clock/audio/inputs/get") == 0) {
            // Reply /clock/audio/inputs.reply <count>
            //   [peerName:s channelName:s busIdx:i sampleRate:i
            //    sourceNumChannels:i (1 or 2, 0 until first buffer)
            //    bufferedMs:f connectionState:i (0..3)
            //    droppedSourceBuffers:i networkGapBuffers:i
            //    totalSourceBufferCalls:i duplicateCountCalls:i
            //    latencySeconds:f]*
            auto inputs = mSuperClock->listLinkAudioInputs();
            // Pre-size for the actual reply; 512 B/entry is a generous
            // upper bound (two ≤64-char strings + 7 int32 + 2 floats +
            // tag/alignment). Avoids overflow when many subs or long
            // peer/channel names are present.
            constexpr size_t kBytesPerInputEntry = 512;
            const size_t bufSize = 1024 + inputs.size() * kBytesPerInputEntry;
            std::vector<char> buf(bufSize);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage("/clock/audio/inputs.reply")
              << static_cast<int32_t>(inputs.size());
            for (const auto& st : inputs) {
                s << st.peerName.c_str()
                  << st.channelName.c_str()
                  << static_cast<int32_t>(st.busIdx)
                  << static_cast<int32_t>(st.sampleRate)
                  << static_cast<int32_t>(st.sourceNumChannels)
                  << (st.bufferedSeconds * 1000.0f)
                  << static_cast<int32_t>(st.state)
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.droppedSourceBuffers,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.networkGapBuffers,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.totalSourceBufferCalls,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.duplicateCountCalls,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<float>(st.latencySeconds);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }


        if (std::strcmp(addr, "/clock/reset") == 0) {
            // Cycle through setLinkVisibility so the full Link Audio
            // teardown (inputSubs, auxSinks, main sink, threadPriority,
            // enableLinkAudio) runs — setLinkEnabled alone leaves all
            // of those wired to the old session.
            const auto prev = mSuperClock->getLinkVisibility();
            mSuperClock->setLinkVisibility(SuperClock::LinkVisibility::Off);
            if (prev != SuperClock::LinkVisibility::Off) {
                mSuperClock->setLinkVisibility(prev);
            }
            return true;
        }


        if (std::strcmp(addr, "/clock/notify/subscribe") == 0) {
            // Subscribe the caller to Link events, then push a tempo + peers
            // snapshot immediately so it starts in sync. Link's change-callbacks
            // only fire on actual deltas — without this, joining a session at the
            // same tempo as our local default leaves the subscriber stuck on its
            // own default until something moves.
            if (!mEgress->subscribeCallerToLinkNotify(token)) return true;  // no addressable caller
            const double initTempo = mSuperClock->getBpm();
            const int32_t initPeers = static_cast<int32_t>(mSuperClock->numPeers());
            {
                char buf[64];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/clock/notify/tempo") << initTempo
                  << osc::EndMessage;
                mEgress->sendToCaller(token, reinterpret_cast<const uint8_t*>(s.Data()),
                                   static_cast<uint32_t>(s.Size()));
            }
            {
                char buf[64];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/clock/notify/peers") << initPeers
                  << osc::EndMessage;
                mEgress->sendToCaller(token, reinterpret_cast<const uint8_t*>(s.Data()),
                                   static_cast<uint32_t>(s.Size()));
            }
            return true;
        }

        if (std::strcmp(addr, "/clock/notify/unsubscribe") == 0) {
            mEgress->unsubscribeCallerFromLinkNotify(token);
            return true;
        }

        if (std::strcmp(addr, "/clock/peers/get") == 0) {
            // Read: reply /clock/peers.reply <count> [<nodeId> <gatewayIp>
            //   <isLoopback:int 0|1> <measurementIp> <measurementPort>
            //   <audioIp> <audioPort>] * count.
            // audioIp is "" when peer has no Link Audio capability.
            auto peers = mSuperClock->listPeers();
            // IPv6 peers cost ~200 bytes per entry (four address strings
            // + 16-hex nodeId + two ports + isLoopback + alignment).
            // 32 KiB covers ~150 peers.
            std::vector<char> buf(32768);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage("/clock/peers.reply")
              << static_cast<int32_t>(peers.size());
            for (const auto& p : peers) {
                s << p.nodeId.c_str()
                  << p.gatewayIp.c_str()
                  << static_cast<int32_t>(p.isLoopback ? 1 : 0)
                  << p.measurementIp.c_str()
                  << static_cast<int32_t>(p.measurementPort)
                  << p.audioIp.c_str()
                  << static_cast<int32_t>(p.audioPort);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }
    } catch (...) {
        // The packet was /clock/* but reply generation threw (typically
        // OutboundPacketStream overflow). Return true to consume it
        // rather than letting handlePacket forward to scsynth's input
        // ring buffer.
        return true;
    }
    return false;
}

bool EngineControl::handleSupersonicCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (!mEngine || size < 20) return false;

    // Fast prefix check
    if (std::memcmp(data, "/supersonic/", 12) != 0) return false;

    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        if (std::strcmp(addr, "/supersonic/notify") == 0) {
            // Register the caller as a notify target for lifecycle events.
            const bool firstRegistration = mEgress->subscribeCaller(token);
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/notify.reply")
              << static_cast<osc::int32>(1)
              << SUPERSONIC_VERSION_STRING
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));

            // Send current device state ONLY for newly registered clients.
            // Re-registrations from existing clients (e.g. spider's boot
            // retry loop firing 20 notifies before its first ack arrives)
            // would otherwise each trigger a fresh listDevices() — and on
            // Windows that probes every WASAPI device via createDevice +
            // initialise (full COM activation per device), taking ~10 s
            // per call and starving the OSC thread for new packets.
            if (firstRegistration) mEngine->sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/notify/unregister") == 0) {
            // Remove the caller from notify targets (polite shutdown).
            mEgress->unsubscribeCaller(token);
            return true;

        } else if (std::strcmp(addr, "/supersonic/notify/clear") == 0) {
            // Remove all notify targets (used before Spider restart).
            mEgress->clearSubscribers();
            return true;

        } else if (std::strcmp(addr, "/supersonic/summary") == 0) {
            // Build a one-shot summary of what this build was compiled with and
            // what it's currently running with, and push it down the debug
            // channel so the GUI shows it in its info pane. The GUI requests
            // this once it is tailing the debug ring.
            // Boot-banner shown in the GUI Info pane, under the ascii logo
            // (which the GUI seeds): a single blank line then a
            // "vMAJOR.MINOR Ready..." line. The leading \x01 byte marks this as
            // a banner so the GUI renders it verbatim (no log timestamp).
            std::string s = "\x01";   // banner sentinel (stripped by the GUI)
            s += "\n";                // single blank line under the logo
            s += "v" + std::to_string(SUPERSONIC_VERSION_MAJOR) + "."
               + std::to_string(SUPERSONIC_VERSION_MINOR) + " Ready...";
            mEgress->debug(s.c_str(), static_cast<uint32_t>(s.size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/list") == 0) {
            auto devices = mEngine->listDevices();
            for (auto& dev : devices) {
                if (dev.isWirelessTransport()) continue;
                char buf[4096];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/supersonic/devices/list.reply")
                  << dev.name.c_str()
                  << dev.typeName.c_str()
                  << static_cast<osc::int32>(dev.maxOutputChannels)
                  << static_cast<osc::int32>(dev.maxInputChannels);
                for (auto r : dev.availableSampleRates)
                    s << static_cast<float>(r);
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }
            // Done marker
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/list.done") << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/current") == 0) {
            auto dev = mEngine->currentDevice();
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/current.reply")
              << dev.name.c_str()
              << dev.typeName.c_str()
              << static_cast<float>(dev.activeSampleRate)
              << static_cast<osc::int32>(dev.activeBufferSize)
              << static_cast<osc::int32>(dev.activeOutputChannels)
              << static_cast<osc::int32>(dev.activeInputChannels)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/switch") == 0) {
            // Args: outputDevice(str), sampleRate(float), bufferSize(int32), [inputDevice(str)]
            auto it = msg.ArgumentsBegin();
            std::string devName, inputDevName;
            double sr = 0;
            int bufSz = 0;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                devName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsFloat()) {
                sr = it->AsFloatUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                bufSz = it->AsInt32Unchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                inputDevName = it->AsStringUnchecked();
            }

            // "__system__" sentinel means "follow system default output"
            if (devName == "__system__") {
                auto error = mEngine->setDeviceMode("");
                char buf[1024];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/supersonic/devices/switch.reply")
                  << static_cast<osc::int32>(error.empty() ? 1 : 0);
                if (!error.empty()) s << error.c_str();
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
                if (error.empty()) mEngine->sendDeviceReport();
                return true;
            }

            // "__none__" sentinel from GUI means "disable audio inputs"
            if (inputDevName == "__none__") {
                // Lock device mode so changeListenerCallback doesn't interfere
                auto curDev = mEngine->currentDevice();
                if (!curDev.name.empty())
                    mEngine->forceDeviceMode(curDev.name);
                auto result = mEngine->enableInputChannels(0);
                char buf[1024];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/supersonic/devices/switch.reply")
                  << static_cast<osc::int32>(result.success ? 1 : 0);
                if (!result.success) s << result.error.c_str();
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
                if (result.success) mEngine->sendDeviceReport();
                return true;
            }

            // Debounce: the transport stores the request and runs only the
            // last one after a quiet period.
            mEngine->scheduleDeviceSwitch(devName, inputDevName, sr, bufSz);

            // Ack immediately so the GUI knows we received it
            {
                char buf[128];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/supersonic/devices/switch.reply")
                  << static_cast<osc::int32>(1) << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/reopen") == 0) {
            // Reopen the current device to re-read its properties (e.g. the
            // user just bumped channel count in MOTU Pro Audio Control). The
            // transport gates this — rejected while a reopen is in flight or
            // within the cooldown after one completes. Replies: .reply
            // (accepted=1|0 + reason) immediately; .done (success, device,
            // rate, buffer, error) when the swap finishes.
            std::string reason;
            const bool accepted = mEngine->requestReopen(reason);
            char buf[512];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/reopen.reply")
              << static_cast<osc::int32>(accepted ? 1 : 0)
              << reason.c_str() << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/report") == 0) {
            // GUI sends this with a reply port; engine sends /supersonic/devices to that port
            auto it = msg.ArgumentsBegin();
            int replyPort = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                replyPort = it->AsInt32Unchecked();
            }
            if (replyPort > 0) {
                mEgress->subscribeNotifyPort(replyPort);
            }
            mEngine->sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/mode") == 0) {
            auto it = msg.ArgumentsBegin();
            std::string mode;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                mode = it->AsStringUnchecked();
            }

            auto error = mEngine->setDeviceMode(mode);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/mode.reply")
              << mEngine->deviceMode().c_str()
              << static_cast<osc::int32>(error.empty() ? 1 : 0);
            if (!error.empty())
                s << error.c_str();
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/drivers/list") == 0) {
            auto drivers = mEngine->listDrivers();
            auto current = mEngine->currentDriver();
            char buf[4096];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/drivers/list.reply");
            s << current.c_str();
            for (auto& d : drivers)
                s << d.c_str();
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/drivers/switch") == 0) {
            auto it = msg.ArgumentsBegin();
            std::string driverName;
            if (it != msg.ArgumentsEnd() && it->IsString())
                driverName = it->AsStringUnchecked();

            auto result = mEngine->switchDriver(driverName);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/drivers/switch.reply");
            if (result.success) {
                s << static_cast<osc::int32>(1)
                  << mEngine->currentDriver().c_str()
                  << static_cast<float>(result.sampleRate)
                  << static_cast<osc::int32>(result.bufferSize);
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                mEngine->sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/inputs/enable") == 0) {
            // Enable/disable audio input channels.
            // Args: numChannels(int32) — 0 to disable, >0 to enable that many channels
            auto it = msg.ArgumentsBegin();
            int numChannels = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                numChannels = it->AsInt32Unchecked();

            // Lock device mode so changeListenerCallback doesn't interfere
            auto curDev = mEngine->currentDevice();
            if (!curDev.name.empty())
                mEngine->forceDeviceMode(curDev.name);

            auto result = mEngine->enableInputChannels(numChannels);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/inputs/enable.reply");
            if (result.success) {
                s << static_cast<osc::int32>(1)
                  << static_cast<osc::int32>(numChannels);
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                mEngine->sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/record/start") == 0) {
            auto it = msg.ArgumentsBegin();
            std::string path, format = "wav";
            int bitDepth = 24;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                path = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                format = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                bitDepth = it->AsInt32Unchecked();
            }

            auto result = mEngine->startRecording(path, format, bitDepth);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/record/start.reply");
            if (result.success) {
                s << static_cast<osc::int32>(1) << result.path.c_str();
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/record/stop") == 0) {
            auto result = mEngine->stopRecording();
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/record/stop.reply");
            if (result.success) {
                s << static_cast<osc::int32>(1) << result.path.c_str();
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, "/supersonic/clock/offset") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsFloat()) {
                mEngine->setClockOffset(it->AsFloatUnchecked());
            }
            return true;
        }
    } catch (...) {
        // Don't let parsing errors crash the server
    }

    return false;
}
