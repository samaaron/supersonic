/*
 * OscUdpServer.cpp
 */
#include "OscUdpServer.h"
#include "SupersonicEngine.h"
#include "Prescheduler.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include <cstring>

OscUdpServer::OscUdpServer()
    : juce::Thread("SuperSonic-OscUdpServer")
{
    mRecvBuf.resize(65536);
}

OscUdpServer::~OscUdpServer() {
    signalThreadShouldExit();
    if (mSocket) mSocket->shutdown();
    stopThread(2000);
}

void OscUdpServer::initialise(int                   port,
                               Prescheduler*         prescheduler,
                               uint8_t*              inBufferStart,
                               uint32_t              inBufferSize,
                               std::atomic<int32_t>* inHead,
                               std::atomic<int32_t>* inTail,
                               std::atomic<int32_t>* inSequence,
                               std::atomic<int32_t>* inWriteLock,
                               PerformanceMetrics*   metrics,
                               double lookaheadS,
                               const std::string& bindAddress)
{
    mPort          = port;
    mBindAddress   = bindAddress;
    mPrescheduler  = prescheduler;
    mInBufferStart = inBufferStart;
    mInBufferSize  = inBufferSize;
    mInHead        = inHead;
    mInTail        = inTail;
    mInSequence    = inSequence;
    mInWriteLock   = inWriteLock;
    mMetrics       = metrics;
    mClassifier.setLookahead(lookaheadS);
}

void OscUdpServer::sendInProcess(const uint8_t* data, uint32_t size) {
    handlePacket(data, size, "127.0.0.1", 0);
}

void OscUdpServer::sendReply(const uint8_t* data, uint32_t size) {
    juce::String ip;
    int port;
    {
        juce::ScopedLock sl(mSenderLock);
        ip   = mLastSenderIP;
        port = mLastSenderPort;
    }
    if (mSocket && ip.isNotEmpty() && port > 0)
        mSocket->write(ip, port, data, static_cast<int>(size));
}

void OscUdpServer::setNotifyTarget(const juce::String& ip, int port) {
    juce::ScopedLock sl(mSenderLock);
    // Add to list if not already registered
    for (auto& t : mNotifyTargets)
        if (t.ip == ip && t.port == port) return;
    mNotifyTargets.push_back({ip, port});
}

void OscUdpServer::sendDeviceReport() {
    std::vector<NotifyTarget> targets;
    {
        juce::ScopedLock sl(mSenderLock);
        targets = mNotifyTargets;
    }
    if (targets.empty() || !mEngine || !mSocket) return;

    auto allDevices = mEngine->listDevices();
    auto current = mEngine->currentDevice();
    auto mode    = mEngine->deviceMode();

    // Split devices into output-capable and input-capable lists.
    // Bluetooth and AirPlay devices are excluded from input — they force
    // low-quality codec modes (HFP 16kHz mono) that break audio quality.
    std::vector<decltype(allDevices)::value_type> outputDevices, inputDevices;
    for (auto& dev : allDevices) {
        if (dev.maxOutputChannels > 0)
            outputDevices.push_back(dev);
        if (dev.maxInputChannels > 0 && dev.isSuitableForInput())
            inputDevices.push_back(dev);
    }

    // Build output device list message
    // Format: mode(str), current(str), device1(str), ..., deviceN(str),
    //         sampleRate(int32), compat1(int32), ..., compatN(int32)
    //         (compat: 1 = device supports current rate, 0 = rate change needed)
    char devBuf[8192];
    osc::OutboundPacketStream devMsg(devBuf, sizeof(devBuf));
    devMsg << osc::BeginMessage("/supersonic/devices")
           << (mode.empty() ? "system" : mode.c_str())
           << current.name.c_str();
    for (auto& dev : outputDevices)
        devMsg << dev.name.c_str();
    devMsg << static_cast<osc::int32>(current.activeSampleRate);
    int curRate = static_cast<int>(current.activeSampleRate);
    for (auto& dev : outputDevices) {
        bool compat = false;
        for (auto r : dev.availableSampleRates)
            if (static_cast<int>(r) == curRate)
                compat = true;
        devMsg << static_cast<osc::int32>(compat ? 1 : 0);
    }
    devMsg << osc::EndMessage;

    // Build input device list message
    // Format: currentInput(str), numDevices(int32), device1(str), ..., deviceN(str)
    char inDevBuf[2048];
    osc::OutboundPacketStream inDevMsg(inDevBuf, sizeof(inDevBuf));
    inDevMsg << osc::BeginMessage("/supersonic/input-devices")
             << current.inputDeviceName.c_str()
             << static_cast<osc::int32>(inputDevices.size());
    for (auto& dev : inputDevices)
        inDevMsg << dev.name.c_str();
    inDevMsg << osc::EndMessage;

    // Build hardware info message
    double sr = current.activeSampleRate;
    double outLatMs = sr > 0 ? (current.outputLatencySamples / sr) * 1000.0 : 0.0;
    double inLatMs  = sr > 0 ? (current.inputLatencySamples  / sr) * 1000.0 : 0.0;

    char info[1024];
    snprintf(info, sizeof(info),
             "Device:      %s\n"
             "Driver:      %s\n"
             "Sample Rate: %.0f Hz\n"
             "Buffer Size: %d samples\n"
             "Channels:    %d out / %d in\n"
             "Latency:     %.1f / %.1f ms (out/in)",
             current.name.c_str(),
             current.typeName.c_str(),
             sr,
             current.activeBufferSize,
             current.activeOutputChannels,
             current.activeInputChannels,
             outLatMs, inLatMs);

    // Info message with config data appended
    // Format: info_string, sampleRate(int32), bufferSize(int32),
    //         numRates(int32), rate1..rateN, numBufs(int32), buf1..bufN,
    //         numDrivers(int32), driver1..driverN, currentDriver(str),
    //         outputChannels(int32), inputChannels(int32)
    auto drivers = mEngine->listDrivers();
    auto curDriver = mEngine->currentDriver();

    // Compute usable sample rates: intersection of output and input device rates.
    // If no input device is active, use the output device's rates.
    // When an aggregate is active, the device rates already reflect the
    // combined capabilities — skip intersection with standalone device info.
    std::vector<double> usableRates = current.availableSampleRates;
    bool onAggregate = !mEngine->realOutputDeviceName().empty();
    if (!onAggregate && !current.inputDeviceName.empty()) {
        for (auto& dev : allDevices) {
            if (dev.name == current.inputDeviceName) {
                std::vector<double> intersection;
                for (auto r : current.availableSampleRates)
                    for (auto ir : dev.availableSampleRates)
                        if (static_cast<int>(r) == static_cast<int>(ir))
                            intersection.push_back(r);
                if (!intersection.empty())
                    usableRates = intersection;
                break;
            }
        }
    }

    char infoBuf[4096];
    osc::OutboundPacketStream infoMsg(infoBuf, sizeof(infoBuf));
    infoMsg << osc::BeginMessage("/supersonic/info")
            << info
            << static_cast<osc::int32>(current.activeSampleRate)
            << static_cast<osc::int32>(current.activeBufferSize)
            << static_cast<osc::int32>(usableRates.size());
    for (auto r : usableRates)
        infoMsg << static_cast<osc::int32>(r);

    // Same intersection for buffer sizes (skip when on aggregate)
    std::vector<int> usableBufferSizes = current.availableBufferSizes;
    if (!onAggregate && !current.inputDeviceName.empty()) {
        for (auto& dev : allDevices) {
            if (dev.name == current.inputDeviceName) {
                std::vector<int> intersection;
                for (auto b : current.availableBufferSizes)
                    for (auto ib : dev.availableBufferSizes)
                        if (b == ib)
                            intersection.push_back(b);
                if (!intersection.empty())
                    usableBufferSizes = intersection;
                break;
            }
        }
    }

    infoMsg << static_cast<osc::int32>(usableBufferSizes.size());
    for (auto b : usableBufferSizes)
        infoMsg << static_cast<osc::int32>(b);
    infoMsg << static_cast<osc::int32>(drivers.size());
    for (auto& d : drivers)
        infoMsg << d.c_str();
    infoMsg << curDriver.c_str();
    infoMsg << static_cast<osc::int32>(current.activeOutputChannels);
    infoMsg << static_cast<osc::int32>(current.activeInputChannels);
    infoMsg << osc::EndMessage;

    // Send to all registered targets
    for (auto& t : targets) {
        mSocket->write(t.ip, t.port, devMsg.Data(), static_cast<int>(devMsg.Size()));
        mSocket->write(t.ip, t.port, inDevMsg.Data(), static_cast<int>(inDevMsg.Size()));
        mSocket->write(t.ip, t.port, infoMsg.Data(), static_cast<int>(infoMsg.Size()));
    }
}

void OscUdpServer::broadcastToTargets(const uint8_t* data, uint32_t size) {
    std::vector<NotifyTarget> targets;
    {
        juce::ScopedLock sl(mSenderLock);
        targets = mNotifyTargets;
    }
    if (targets.empty() || !mSocket) return;

    for (auto& t : targets)
        mSocket->write(t.ip, t.port, data, static_cast<int>(size));
}

void OscUdpServer::sendStateChange(const char* state, const char* reason) {
    std::vector<NotifyTarget> targets;
    {
        juce::ScopedLock sl(mSenderLock);
        targets = mNotifyTargets;
    }
    if (targets.empty() || !mSocket) return;

    char buf[512];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/statechange")
      << state
      << reason
      << osc::EndMessage;

    for (auto& t : targets)
        mSocket->write(t.ip, t.port, s.Data(), static_cast<int>(s.Size()));
}

void OscUdpServer::sendSetup(int sampleRate, int bufferSize) {
    std::vector<NotifyTarget> targets;
    {
        juce::ScopedLock sl(mSenderLock);
        targets = mNotifyTargets;
    }
    if (targets.empty() || !mSocket) return;

    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/setup")
      << static_cast<osc::int32>(sampleRate)
      << static_cast<osc::int32>(bufferSize)
      << osc::EndMessage;

    for (auto& t : targets)
        mSocket->write(t.ip, t.port, s.Data(), static_cast<int>(s.Size()));
}

bool OscUdpServer::handleSupersonicCommand(const uint8_t* data, uint32_t size) {
    if (!mEngine || size < 20) return false;

    // Fast prefix check
    if (std::memcmp(data, "/supersonic/", 12) != 0) return false;

    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        if (std::strcmp(addr, "/supersonic/notify") == 0) {
            // Register the sender as a notify target for lifecycle events.
            // mLastSenderIP/Port are set to the current sender by
            // handlePacket's save/restore logic for /supersonic/* commands.
            juce::String senderIP;
            int senderPort;
            {
                juce::ScopedLock sl(mSenderLock);
                senderIP = mLastSenderIP;
                senderPort = mLastSenderPort;
            }
            setNotifyTarget(senderIP, senderPort);
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/notify.reply")
              << static_cast<osc::int32>(1)
              << osc::EndMessage;
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));

            // Send current device state so the new target starts
            // with an accurate picture (important after client restart).
            sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/list") == 0) {
            auto devices = mEngine->listDevices();
            for (auto& dev : devices) {
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
                sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }
            // Done marker
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/list.done") << osc::EndMessage;
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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

            // "__none__" sentinel from GUI means "disable audio inputs"
            if (inputDevName == "__none__") {
                auto result = mEngine->enableInputChannels(0);
                char buf[1024];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage("/supersonic/devices/switch.reply")
                  << static_cast<osc::int32>(result.success ? 1 : 0);
                if (!result.success) s << result.error.c_str();
                s << osc::EndMessage;
                sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
                if (result.success) sendDeviceReport();
                return true;
            }

            auto result = mEngine->switchDevice(devName, sr, bufSz, false, inputDevName);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage("/supersonic/devices/switch.reply");
            if (result.success) {
                s << static_cast<osc::int32>(1);
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/devices/report") == 0) {
            // GUI sends this with a reply port; engine sends /supersonic/devices to that port
            auto it = msg.ArgumentsBegin();
            int replyPort = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                replyPort = it->AsInt32Unchecked();
            }
            if (replyPort > 0) {
                setNotifyTarget("127.0.0.1", replyPort);
            }
            sendDeviceReport();
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, "/supersonic/inputs/enable") == 0) {
            // Enable/disable audio input channels.
            // Args: numChannels(int32) — 0 to disable, >0 to enable that many channels
            auto it = msg.ArgumentsBegin();
            int numChannels = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                numChannels = it->AsInt32Unchecked();

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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                sendDeviceReport();
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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
            sendReply(reinterpret_cast<const uint8_t*>(s.Data()),
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

void OscUdpServer::handlePacket(const uint8_t* data, uint32_t size,
                                 const juce::String& senderIP, int senderPort)
{
    if (size == 0) return;

    // Intercept /supersonic/* commands before ring buffer routing.
    // These are handled directly — reply to the actual sender without
    // clobbering the ring buffer reply target (mLastSenderIP/Port).
    if (size >= 20 && data[0] == '/' && std::memcmp(data, "/supersonic/", 12) == 0) {
        // Temporarily point sendReply() at the current sender so the
        // command handler's direct replies reach the right client.
        juce::String savedIP;
        int savedPort;
        {
            juce::ScopedLock sl(mSenderLock);
            savedIP  = mLastSenderIP;
            savedPort = mLastSenderPort;
            mLastSenderIP   = senderIP;
            mLastSenderPort = senderPort;
        }

        bool handled = handleSupersonicCommand(data, size);

        // Restore so async ring-buffer replies still go to the correct client
        {
            juce::ScopedLock sl(mSenderLock);
            mLastSenderIP   = savedIP;
            mLastSenderPort = savedPort;
        }

        if (handled) return;
    }

    // /status flows through the ring buffer like upstream scsynth — meth_status
    // in SC_MiscCmds.cpp handles it via the sequenced command infrastructure,
    // returning real values for unit count, synth count, CPU, and sample rate.

    // This message will be routed through the ring buffer.
    // Update reply target so async replies from ReplyReader go back to this sender.
    if (senderPort > 0) {
        juce::ScopedLock sl(mSenderLock);
        mLastSenderIP   = senderIP;
        mLastSenderPort = senderPort;
    }

    if (mMetrics) {
        mMetrics->osc_out_messages_sent.fetch_add(1, std::memory_order_relaxed);
        mMetrics->osc_out_bytes_sent.fetch_add(size, std::memory_order_relaxed);
    }

    double wallNow = wallClockNTP();
    OscCategory cat = mClassifier.classify(data, size, wallNow);

    switch (cat) {
    case OscCategory::FAR_FUTURE:
        if (mPrescheduler) {
            double tagSec = OscClassifier::bundleTimeSec(data, size);
            mPrescheduler->schedule(data, size, tagSec);
            if (mMetrics)
                mMetrics->prescheduler_bypassed.fetch_add(1, std::memory_order_relaxed);
        }
        break;

    case OscCategory::IMMEDIATE:
        if (mMetrics)
            mMetrics->bypass_immediate.fetch_add(1, std::memory_order_relaxed);
        [[fallthrough]];
    case OscCategory::NEAR_FUTURE:
        if (cat == OscCategory::NEAR_FUTURE && mMetrics)
            mMetrics->bypass_near_future.fetch_add(1, std::memory_order_relaxed);
        [[fallthrough]];
    case OscCategory::LATE:
        if (cat == OscCategory::LATE && mMetrics)
            mMetrics->bypass_late.fetch_add(1, std::memory_order_relaxed);

        if (mInBufferStart) {
            RingBufferWriter::write(
                mInBufferStart,
                mInBufferSize,
                mInHead,
                mInTail,
                mInSequence,
                mInWriteLock,
                data,
                size
            );
        }
        break;
    }
}

void OscUdpServer::run() {
    mSocket = std::make_unique<juce::DatagramSocket>();

    if (!mSocket->bindToPort(mPort, mBindAddress.empty() ? juce::String() : juce::String(mBindAddress))) {
        fprintf(stderr, "[osc] failed to bind to port %d\n", mPort);
        return;
    }

    while (!threadShouldExit()) {
        juce::String senderIP;
        int senderPort = 0;

        int bytesRead = mSocket->read(mRecvBuf.data(),
                                       static_cast<int>(mRecvBuf.size()),
                                       false, senderIP, senderPort);

        if (bytesRead > 0) {
            // Sender tracking is handled inside handlePacket():
            // - Ring-buffer-bound messages update mLastSenderIP/Port
            //   so async replies from ReplyReader go to the right client.
            // - Directly-handled commands (/supersonic/*) reply
            //   to the actual sender without clobbering the reply target.
            try {
                handlePacket(mRecvBuf.data(),
                             static_cast<uint32_t>(bytesRead),
                             senderIP, senderPort);
            } catch (const std::exception& e) {
                fprintf(stderr, "[osc] exception in handlePacket: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "[osc] unknown exception in handlePacket\n");
            }
        } else if (bytesRead < 0) {
            if (threadShouldExit()) break;
            juce::Thread::sleep(1);
        }
    }

    mSocket.reset();
}
