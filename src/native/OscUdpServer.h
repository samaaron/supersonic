/*
 * OscUdpServer.h — UDP OSC listener on port 57110
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "NTPClock.h"
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
                    NTPClock*             clock,
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

    // Register a persistent listener (e.g. from /notify) for unsolicited messages
    void addListener(const juce::String& ip, int port);

    // Set the target for device change notifications
    void setNotifyTarget(const juce::String& ip, int port);

    // Send a /scsynth/devices report to the registered notify target
    void sendDeviceReport();

    // Set engine pointer for /supersonic/* command interception
    void setEngine(SupersonicEngine* engine) { mEngine = engine; }

    std::function<void(const uint8_t*, uint32_t)> onIncomingMessage;

private:
    void run() override;
    void handlePacket(const uint8_t* data, uint32_t size,
                      const juce::String& senderIP, int senderPort);
    bool handleSupersonicCommand(const uint8_t* data, uint32_t size);

    int                   mPort          = 57110;
    std::string           mBindAddress;
    NTPClock*             mClock         = nullptr;
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

    // Notification target for proactive device change pushes
    juce::String          mNotifyIP;
    int                   mNotifyPort = 0;
};
