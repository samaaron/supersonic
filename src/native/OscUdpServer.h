/*
 * OscUdpServer.h — UDP OSC listener on port 57110
 *
 * Handles incoming OSC over UDP, routing messages to either the ring
 * buffer (for scsynth) or handling /supersonic/* commands directly.
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

    void sendInProcess(const uint8_t* data, uint32_t size);

    // Send a reply back to the most recently registered client
    void sendReply(const uint8_t* data, uint32_t size);

    // Register a notify target (deduped by ip:port)
    void setNotifyTarget(const juce::String& ip, int port);

    // Send /supersonic/devices + /supersonic/info to all notify targets
    void sendDeviceReport();

    // Send raw OSC data to all registered notify targets
    void broadcastToTargets(const uint8_t* data, uint32_t size);

    // Broadcast /supersonic/statechange to all notify targets
    void sendStateChange(const char* state, const char* reason);

    // Broadcast /supersonic/setup to all notify targets (world is ready)
    void sendSetup(int sampleRate, int bufferSize);

    // Set engine pointer for /supersonic/* command interception
    void setEngine(SupersonicEngine* engine) { mEngine = engine; }

private:
    void run() override;
    void handlePacket(const uint8_t* data, uint32_t size,
                      const juce::String& senderIP, int senderPort);
    bool handleSupersonicCommand(const uint8_t* data, uint32_t size);

    int                   mPort          = 57110;
    std::string           mBindAddress;
    Prescheduler*         mPrescheduler  = nullptr;
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
    std::unique_ptr<std::thread> mDebounceSwitchThread;
    std::atomic<bool>          mDebounceSwitchRunning{false};

    void executePendingSwitch();
};
