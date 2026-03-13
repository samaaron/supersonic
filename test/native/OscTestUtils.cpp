/*
 * OscTestUtils.cpp
 */
#include "OscTestUtils.h"
#include <cstring>

namespace osc_test {

static Packet streamToPacket(osc::OutboundPacketStream& s) {
    Packet p;
    p.data.assign(s.Data(), s.Data() + s.Size());
    return p;
}

Packet message(const char* address) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a, int32_t b) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << b << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a, int32_t b, int32_t c) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << b << c << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, const char* s) {
    char buf[256];
    osc::OutboundPacketStream stream(buf, sizeof(buf));
    stream << osc::BeginMessage(address) << s << osc::EndMessage;
    return streamToPacket(stream);
}

Packet messageWithBlob(const char* address, const void* blobData, size_t blobSize) {
    char buf[65536];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address)
      << osc::Blob(blobData, static_cast<osc::osc_bundle_element_size_t>(blobSize))
      << osc::EndMessage;
    return streamToPacket(s);
}

Builder::Builder() = default;

osc::OutboundPacketStream& Builder::begin(const char* address) {
    mStream = std::make_unique<osc::OutboundPacketStream>(mBuf, kBufSize);
    *mStream << osc::BeginMessage(address);
    return *mStream;
}

Packet Builder::end() {
    *mStream << osc::EndMessage;
    return streamToPacket(*mStream);
}

std::string parseAddress(const uint8_t* data, uint32_t size) {
    if (!data || size == 0) return {};
    const char* str = reinterpret_cast<const char*>(data);
    size_t len = strnlen(str, size);
    return std::string(str, len);
}

ParsedReply parseReply(const uint8_t* data, uint32_t size) {
    ParsedReply r;
    r.raw.assign(data, data + size);
    r.address = parseAddress(data, size);
    return r;
}

int32_t ParsedReply::argInt(int index) const {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(raw.data()), raw.size());
        osc::ReceivedMessage msg(pkt);
        auto it = msg.ArgumentsBegin();
        for (int i = 0; i < index && it != msg.ArgumentsEnd(); ++i, ++it) {}
        if (it != msg.ArgumentsEnd()) {
            if (it->IsInt32()) return it->AsInt32Unchecked();
            if (it->IsFloat()) return static_cast<int32_t>(it->AsFloatUnchecked());
        }
    } catch (...) {}
    return 0;
}

float ParsedReply::argFloat(int index) const {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(raw.data()), raw.size());
        osc::ReceivedMessage msg(pkt);
        auto it = msg.ArgumentsBegin();
        for (int i = 0; i < index && it != msg.ArgumentsEnd(); ++i, ++it) {}
        if (it != msg.ArgumentsEnd()) {
            if (it->IsFloat()) return it->AsFloatUnchecked();
            if (it->IsInt32()) return static_cast<float>(it->AsInt32Unchecked());
        }
    } catch (...) {}
    return 0.0f;
}

std::string ParsedReply::argString(int index) const {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(raw.data()), raw.size());
        osc::ReceivedMessage msg(pkt);
        auto it = msg.ArgumentsBegin();
        for (int i = 0; i < index && it != msg.ArgumentsEnd(); ++i, ++it) {}
        if (it != msg.ArgumentsEnd() && it->IsString())
            return std::string(it->AsStringUnchecked());
    } catch (...) {}
    return {};
}

int ParsedReply::argCount() const {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(raw.data()), raw.size());
        osc::ReceivedMessage msg(pkt);
        return static_cast<int>(msg.ArgumentCount());
    } catch (...) {}
    return 0;
}

} // namespace osc_test
