/*
 * OscControl.cpp — see OscControl.h. All sockets, threading, OSC reframing and
 * hostname resolution live in the Rust subsystem (supersonic-osc-net); this file
 * only marshals between the engine and the ss_osc_* C ABI.
 */
#include "OscControl.h"

#include "src/IngressCallCtx.h"
#include "OscEgress.h"
#include "ss_osc.h"
#include "supersonic_config.h"   // ss_log
#include "osc/OscReceivedElements.h"

#include <cstring>

// ── small arg readers (oscpack iterator) ─────────────────────────────────────
namespace {
int argInt(osc::ReceivedMessage::const_iterator& it,
           const osc::ReceivedMessage::const_iterator& end, int def) {
    if (it == end) return def;
    int v = def;
    if (it->IsInt32())      v = it->AsInt32Unchecked();
    else if (it->IsInt64()) v = static_cast<int>(it->AsInt64Unchecked());
    else if (it->IsFloat()) v = static_cast<int>(it->AsFloatUnchecked());
    ++it;
    return v;
}

bool argBool(osc::ReceivedMessage::const_iterator& it,
             const osc::ReceivedMessage::const_iterator& end, bool def) {
    if (it == end) return def;
    bool v = def;
    if (it->IsBool())        v = it->AsBoolUnchecked();
    else if (it->IsInt32())  v = it->AsInt32Unchecked() != 0;
    else if (it->IsFloat())  v = it->AsFloatUnchecked() != 0.0f;
    ++it;
    return v;
}
} // namespace

void OscControl::init(OscEgress* egress) {
    mEgress = egress;
    if (!mOsc) mOsc = ss_osc_create(this, &OscControl::emitCb);
}

void OscControl::shutdown() {
    if (mOsc) {
        ss_osc_destroy(mOsc);
        mOsc = nullptr;
    }
}

void OscControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<OscControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == SS_OSC_EMIT_BROADCAST)
        self->mEgress->broadcastOscNotify(osc, len);
}

void OscControl::applyCueConfig() {
    if (mOsc) ss_osc_configure(mOsc, mPort, mLoopback ? 1 : 0, mCuesOn ? 1 : 0);
}

bool OscControl::handleOscCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (size < 6 || std::memcmp(data, "/osc/", 5) != 0) return false;
    const char* addr = reinterpret_cast<const char*>(data);

    if (std::strcmp(addr, "/osc/notify/subscribe") == 0) {
        if (mEgress) mEgress->subscribeCallerToOscNotify(token);
        return true;
    }
    if (std::strcmp(addr, "/osc/notify/unsubscribe") == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromOscNotify(token);
        return true;
    }

    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(size)));
        auto it = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();

        if (std::strcmp(addr, "/osc/send") == 0) {
            // Self-routing outbound user OSC: <host:s> <port:i> <inner:blob>.
            // Send `inner` to host:port. Reached immediately, or as the inner blob
            // of a /schedule event — the deferred path re-dispatches it here.
            std::string host;
            int port = 0;
            const void* inner = nullptr;
            osc::osc_bundle_element_size_t innerLen = 0;
            if (it != end && it->IsString()) { host = it->AsStringUnchecked(); ++it; }
            if (it != end) port = argInt(it, end, 0);
            if (it != end && it->IsBlob()) it->AsBlobUnchecked(inner, innerLen);
            if (mOsc && inner && innerLen > 0 && !host.empty() && port > 0)
                ss_osc_send(mOsc,
                            reinterpret_cast<const uint8_t*>(host.data()),
                            static_cast<uint32_t>(host.size()), port,
                            static_cast<const uint8_t*>(inner),
                            static_cast<uint32_t>(innerLen));
            else
                ss_log("WARNING: /osc/send dropped — bad host/port/blob "
                       "(host='%s' port=%d innerLen=%d)",
                       host.c_str(), port, static_cast<int>(innerLen));
        } else if (std::strcmp(addr, "/osc/cue-server/config") == 0) {
            mPort     = argInt(it, end, mPort);
            mLoopback = argBool(it, end, mLoopback);
            mCuesOn   = argBool(it, end, mCuesOn);
            applyCueConfig();
        } else if (std::strcmp(addr, "/osc/cue-server/cues-on") == 0) {
            mCuesOn = argBool(it, end, mCuesOn);
            applyCueConfig();
        } else if (std::strcmp(addr, "/osc/cue-server/loopback") == 0) {
            mLoopback = argBool(it, end, mLoopback);
            applyCueConfig();
        }
    } catch (...) {
        // Malformed /osc/ control message — ignore.
    }
    return true;
}
