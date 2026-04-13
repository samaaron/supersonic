/*
 * OscBuilder.h — Header-only OSC message/bundle builder
 *
 * Promotes the oscpack builder pattern from test utils to a public engine API.
 * Uses C++17 fold expressions over oscpack's operator<< overloads.
 */
#pragma once

#include "osc/OscOutboundPacketStream.h"
#include "osc/OscTypes.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>

// Self-contained OSC packet (owns its data)
struct OscPacket {
    std::vector<uint8_t> data;
    const uint8_t* ptr()  const { return data.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(data.size()); }
};

class OscBuilder {
public:
    // Blob wrapper — explicitly marks binary data for OSC blob encoding
    struct Blob {
        const void* data;
        size_t size;
    };

    // Build a single OSC message with typed args
    template<typename... Args>
    static OscPacket message(const char* address, Args&&... args) {
        thread_local char buf[65536];
        osc::OutboundPacketStream s(buf, sizeof(buf));
        s << osc::BeginMessage(address);
        (append(s, std::forward<Args>(args)), ...);
        s << osc::EndMessage;
        return OscPacket{{
            reinterpret_cast<const uint8_t*>(s.Data()),
            reinterpret_cast<const uint8_t*>(s.Data()) + s.Size()
        }};
    }

    // Build an OSC bundle from pre-built messages
    // Constructs wire format directly: "#bundle\0" + timetag + [size+data]*
    static OscPacket bundle(uint64_t ntpTimeTag,
                            std::initializer_list<OscPacket> messages) {
        // Calculate total size
        size_t total = 8 + 8; // "#bundle\0" + timetag
        for (auto& m : messages)
            total += 4 + m.size(); // size prefix + data

        std::vector<uint8_t> result(total);
        uint8_t* p = result.data();

        // Write "#bundle\0"
        std::memcpy(p, "#bundle\0", 8);
        p += 8;

        // Write timetag (big-endian uint64)
        p[0] = static_cast<uint8_t>((ntpTimeTag >> 56) & 0xFF);
        p[1] = static_cast<uint8_t>((ntpTimeTag >> 48) & 0xFF);
        p[2] = static_cast<uint8_t>((ntpTimeTag >> 40) & 0xFF);
        p[3] = static_cast<uint8_t>((ntpTimeTag >> 32) & 0xFF);
        p[4] = static_cast<uint8_t>((ntpTimeTag >> 24) & 0xFF);
        p[5] = static_cast<uint8_t>((ntpTimeTag >> 16) & 0xFF);
        p[6] = static_cast<uint8_t>((ntpTimeTag >> 8) & 0xFF);
        p[7] = static_cast<uint8_t>(ntpTimeTag & 0xFF);
        p += 8;

        // Write each message with size prefix
        for (auto& m : messages) {
            uint32_t sz = m.size();
            p[0] = static_cast<uint8_t>((sz >> 24) & 0xFF);
            p[1] = static_cast<uint8_t>((sz >> 16) & 0xFF);
            p[2] = static_cast<uint8_t>((sz >> 8) & 0xFF);
            p[3] = static_cast<uint8_t>(sz & 0xFF);
            p += 4;
            std::memcpy(p, m.ptr(), sz);
            p += sz;
        }

        return OscPacket{std::move(result)};
    }

private:
    // Type dispatch — explicit overloads give clear compile errors for unsupported types
    static void append(osc::OutboundPacketStream& s, int v)                { s << static_cast<osc::int32>(v); }
    static void append(osc::OutboundPacketStream& s, float v)              { s << v; }
    static void append(osc::OutboundPacketStream& s, double v)             { s << v; }
    static void append(osc::OutboundPacketStream& s, const char* v)        { s << v; }
    static void append(osc::OutboundPacketStream& s, const std::string& v) { s << v.c_str(); }
    static void append(osc::OutboundPacketStream& s, const Blob& b) {
        s << osc::Blob(b.data, static_cast<osc::osc_bundle_element_size_t>(b.size));
    }
};
