/*
 * OscUdpServer.h — UDP OSC listener on port 57110
 *
 * Handles incoming OSC over UDP, routing messages to either the ring
 * buffer (for scsynth) or handling "/supersonic/..." commands directly.
 *
 * ## Notify targets (multi-notify)
 *
 * Multiple clients can register for proactive push notifications by
 * sending /supersonic/notify.  Each sender's IP:port is added to a
 * deduped list; all registered targets receive:
 *
 *   /supersonic/devices     — device list + compatibility flags
 *   /supersonic/info        — hardware info + available rates/buffer sizes
 *   /supersonic/statechange — engine lifecycle transitions (state, reason)
 *   /supersonic/setup       — world ready (sampleRate, bufferSize)
 *
 * On registration the new target immediately receives a device report
 * so it starts with an accurate picture (important after client restart).
 *
 * The older /supersonic/devices/report command (with explicit reply port)
 * still works and also registers a notify target.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "WallClock.h"
#include "OscClassifier.h"
#include "RingBufferWriter.h"
#include "src/shared_memory.h"

class Prescheduler;
class SupersonicEngine;
class SuperClock;
struct SwapResult;  // declared in DeviceInfo.h, carried into sendSwitchDone

class OscUdpServer : public juce::Thread {
public:
    OscUdpServer();
    ~OscUdpServer() override;

    void initialise(int                   port,
                    Prescheduler*         prescheduler,
                    uint8_t*              inBufferStart,
                    uint32_t              inBufferSize,
                    std::atomic<int32_t>* inHead,
                    std::atomic<int32_t>* inTail,
                    std::atomic<int32_t>* inSequence,
                    std::atomic<int32_t>* inWriteLock,
                    PerformanceMetrics*   metrics,
                    double lookaheadS = 0.500,
                    const std::string& bindAddress = "");

    // Wire the engine's SuperClock — single source of NTP "now" for the
    // OSC-receive thread. Must be set before initialise() / startThread().
    void setSuperClock(SuperClock* sc) { mSuperClock = sc; }

    void sendInProcess(const uint8_t* data, uint32_t size);

    // Send a reply back to the most recently registered client
    void sendReply(const uint8_t* data, uint32_t size);

    // Register a notify target (deduped by ip:port).
    // Returns true if a new entry was added, false if already registered.
    bool setNotifyTarget(const juce::String& ip, int port);

    // Send /supersonic/devices + /supersonic/info to all notify targets
    void sendDeviceReport();

    // Send raw OSC data to all registered notify targets
    void broadcastToTargets(const uint8_t* data, uint32_t size);

    // Broadcast /supersonic/statechange to all notify targets
    void sendStateChange(const char* state, const char* reason);

    // Broadcast /supersonic/setup to all notify targets (world is ready).
    // `generation` carries the engine's cold-swap counter — see
    // SupersonicEngine::setupGeneration().
    void sendSetup(int sampleRate, int bufferSize, uint32_t generation);

    // Broadcast /supersonic/devices/switch.done with the truthful outcome
    // of a debounced switch. Two failure shapes are surfaced explicitly:
    //   - success == false              → entire swap failed; GUI shows
    //                                      a modal with `result.error`.
    //   - success == true, inputUnavailable
    //                                   → output opened, the requested
    //                                      input couldn't; GUI shows
    //                                      `inputUnavailableReason` and
    //                                      reverts the input dropdown.
    // The engine does not diagnose — JUCE's verbatim string is carried.
    void sendSwitchDone(const SwapResult& result,
                        const std::string& requestedOutput,
                        const std::string& requestedInput);

    // Set engine pointer for /supersonic/* command interception
    void setEngine(SupersonicEngine* engine) { mEngine = engine; }

private:
    void run() override;
    void handlePacket(const uint8_t* data, uint32_t size,
                      const juce::String& senderIP, int senderPort);
    bool handleSupersonicCommand(const uint8_t* data, uint32_t size);
    bool handleLinkCommand(const uint8_t* data, uint32_t size);

public:
    // Link-event notify subscription (separate from /supersonic/notify
    // targets — different consumer audience). Called from OSC handlers
    // and from SuperClock callback wiring in SupersonicEngine.
    void addLinkNotifyTarget(const juce::String& ip, int port);
    void removeLinkNotifyTarget(const juce::String& ip, int port);
    // Sends `data` to every registered Link notify target. Safe to call
    // from Link's network thread (uses mSocket->write under a brief lock).
    void broadcastLinkNotify(const uint8_t* data, uint32_t size);

private:

    int                   mPort          = 57110;
    std::string           mBindAddress;
    Prescheduler*         mPrescheduler  = nullptr;
    SuperClock*           mSuperClock    = nullptr;
    SupersonicEngine*     mEngine        = nullptr;
    uint8_t*              mInBufferStart = nullptr;
    uint32_t              mInBufferSize  = 0;
    std::atomic<int32_t>* mInHead        = nullptr;
    std::atomic<int32_t>* mInTail        = nullptr;
    std::atomic<int32_t>* mInSequence    = nullptr;
    std::atomic<int32_t>* mInWriteLock   = nullptr;
    PerformanceMetrics*   mMetrics       = nullptr;
    OscClassifier         mClassifier;

    std::unique_ptr<juce::DatagramSocket> mSocket;
    std::vector<uint8_t>                  mRecvBuf;

    // Serialises ALL mSocket->write calls. JUCE's DatagramSocket::write
    // is not concurrent-write safe (internal getaddrinfo/freeaddrinfo
    // state races otherwise). Writers: OSC receive thread (replies),
    // Link network thread (broadcastLinkNotify), device-report path.
    juce::CriticalSection mSocketWriteLock;

    // Last sender — replies go back here
    juce::CriticalSection mSenderLock;
    juce::String          mLastSenderIP;
    int                   mLastSenderPort = 0;

    // Notification targets for proactive device change pushes
    struct NotifyTarget {
        juce::String ip;
        int port;
    };
    std::vector<NotifyTarget> mNotifyTargets;

    // Separate list for Link-event notify subscribers (tempo / peer
    // count / transport-state pushes). Separate from mNotifyTargets so
    // device-only consumers (the GUI) aren't spammed with Link events
    // and Link-only consumers (Sonic Pi) aren't spammed with device events.
    juce::CriticalSection     mLinkNotifyLock;
    std::vector<NotifyTarget> mLinkNotifyTargets;

    // Debounced device switch — rapid clicks settle into one final switch
    struct PendingSwitch {
        std::string devName;
        std::string inputDevName;
        double sampleRate = 0;
        int bufferSize = 0;
        std::chrono::steady_clock::time_point timestamp;
        bool active = false;
    };
    PendingSwitch              mPendingSwitch;
    std::mutex                 mPendingSwitchMutex;
    std::thread                mDebounceSwitchThread;
    std::atomic<bool>          mDebounceSwitchRunning{false};
    std::atomic<bool>          mDebounceSwitchStop{false};

    void executePendingSwitch();

    // Reopen — rejects while an existing reopen is in flight and within a
    // short cooldown after completion. Accepted requests run on a worker
    // thread so the OSC handler thread isn't blocked by the cold swap;
    // two OSC messages go back to the caller — .reply (accepted/rejected,
    // immediate) and .done (completion result).
    std::atomic<bool>          mReopenInProgress{false};
    std::chrono::steady_clock::time_point mLastReopenFinishedAt{};
    std::thread                mReopenThread;

    void executeReopen();
};
