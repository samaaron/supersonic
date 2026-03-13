/*
 * OscTestUtils.h — OSC message builder and reply parser for tests.
 * Uses the vendored oscpack library.
 */
#pragma once

#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace osc_test {

// A self-contained OSC packet (owns its data).
struct Packet {
    std::vector<uint8_t> data;
    const uint8_t* ptr()  const { return data.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(data.size()); }
};

// Build a simple OSC message with typed args.
Packet message(const char* address);
Packet message(const char* address, int32_t a);
Packet message(const char* address, int32_t a, int32_t b);
Packet message(const char* address, int32_t a, int32_t b, int32_t c);
Packet message(const char* address, const char* s);
Packet messageWithBlob(const char* address, const void* blobData, size_t blobSize);

// General-purpose builder: call begin(), stream args, call end().
class Builder {
public:
    Builder();
    osc::OutboundPacketStream& begin(const char* address);
    Packet end();
private:
    static constexpr size_t kBufSize = 65536;
    char mBuf[kBufSize];
    std::unique_ptr<osc::OutboundPacketStream> mStream;
};

// Parse the address pattern from a raw OSC reply.
std::string parseAddress(const uint8_t* data, uint32_t size);

// Parsed OSC reply — wraps oscpack for convenient field access.
struct ParsedReply {
    std::string address;
    std::vector<uint8_t> raw;

    // Access typed arguments by index. Returns 0/0.0f/"" on out-of-range.
    int32_t     argInt(int index) const;
    float       argFloat(int index) const;
    std::string argString(int index) const;
    int         argCount() const;
};

ParsedReply parseReply(const uint8_t* data, uint32_t size);

} // namespace osc_test
