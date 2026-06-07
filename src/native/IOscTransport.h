/*
 * IOscTransport.h — the engine→outside egress boundary.
 *
 * The engine deals only in opaque origin tokens and OSC bytes. A transport owns
 * everything platform-specific behind this interface: how a token resolves to a
 * destination, what the notify/link subscriber audiences actually are, and how a
 * packet physically leaves (UDP socket / in-process callback). This is the
 * *only* place address/socket/pid knowledge lives. Implementations: OscUdpServer
 * (UDP), NifTransport (BEAM — fans out to the registered Erlang pids via
 * enif_send), and CallbackTransport (in-process — embedders and tests, surfacing
 * through the engine's onReply callback).
 *
 * The NRT gateway is the sole caller of these methods (one thread), so
 * implementations need no internal send serialisation beyond their own resource
 * locks (e.g. a socket write lock shared with a recv thread).
 */
#pragma once

#include <cstdint>

class IOscTransport {
public:
    virtual ~IOscTransport() = default;

    // Reply to the peer an origin token resolves to. `networkOnly` means "do not
    // deliver to an in-process observer" (Link snapshots): a real transport
    // (UDP/NIF) sends regardless; a callback sink drops it. Returns true if it
    // reached a destination.
    virtual bool send(uint32_t token, const uint8_t* data, uint32_t size,
                      bool networkOnly) = 0;

    // Fan out to the device-notify / Link-notify subscriber audiences.
    virtual void broadcastNotify(const uint8_t* data, uint32_t size) = 0;
    virtual void broadcastLink(const uint8_t* data, uint32_t size) = 0;

    // Notify-audience registry. subscribeNotify is caller-relative (resolve the
    // token); subscribeNotifyPort targets an explicit local port (the GUI's
    // /supersonic/devices/report reply port). subscribe* return true if newly
    // added. hasNotifySubscribers gates whether the engine bothers to emit.
    virtual bool hasNotifySubscribers() const = 0;
    virtual bool subscribeNotify(uint32_t token) = 0;
    virtual void subscribeNotifyPort(int port) = 0;
    virtual void unsubscribeNotify(uint32_t token) = 0;
    virtual void clearNotify() = 0;

    // Link-notify audience (separate from device-notify). Caller-relative; an
    // unaddressable caller (e.g. in-process, no port) is rejected → false.
    virtual bool subscribeLink(uint32_t token) = 0;
    virtual void unsubscribeLink(uint32_t token) = 0;
};
