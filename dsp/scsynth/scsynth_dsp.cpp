// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_dsp.cpp — scsynth as a clockwork guest.
 *
 * Clockwork reaches every DSP through dsp_api.h's seven calls. scsynth
 * already had a boundary of its own — engine_api.h's World_New /
 * EngineCore_BeginBlock / EngineCore_RunBlock / PerformOSCMessage — drawn when
 * the Rust graph engine was made an alternative to it. This file is the
 * mapping between the two, and it is a mapping rather than a design: both
 * headers were written to describe the same thing from opposite sides.
 *
 *      dsp_new      -> EngineCore_New
 *      dsp_process  -> EngineCore_BeginBlock + RunBlock + FlushNotifications
 *      dsp_osc      -> PerformOSCMessage / PerformOSCBundle
 *      dsp_free     -> World_Cleanup
 *      dsp_status   -> the engine's own counters
 *
 * WHAT TAU NO LONGER PROVIDES, and scsynth therefore owns: buses, the
 * node tree, the buffer table, the wire pool. The seam passes channels and
 * frames; everything inside the graph is the guest's business, which is why
 * the eight sizing options the old options block carried stop at this file.
 *
 * COMPLETE. Every entry point is implemented and the engine is driven: dsp_new
 * builds a World from the client's config block, dsp_process runs a block,
 * dsp_osc performs messages and bundles with sample-accurate offsets.
 */
#include "dsp_api.h"
#include "engine_api.h"
#include "scsynth_config.h"
#include "buffer_commands.h"

#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_HiddenWorld.h"
#include "SC_Group.h"        // mTopGroup is only a forward declaration in SC_World.h
// SC_Reply.h only forward-declares ReplyAddress; the layout — and mReplyData,
// which is where the origin token rides — is in the Impl header. OSC_Packet is
// the server's own.
#include "shared_memory.h"   // GUEST_CONFIG_START: the client's config block
#include "node_tree.h"

// clockwork's shared-memory base, where the mirror lives. Declared the same
// way SC_Node.cpp declares it, because it is the same C-linkage symbol.
extern "C" {
    extern uint8_t* shared_memory;
}
#include "SC_Reply.h"
#include "SC_ReplyImpl.hpp"
#include "OSC_Packet.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <new>

namespace {

/*
 * WHAT SCSYNTH KEEPS IN DspConfig::persistent.
 *
 * /notify registers a client inside the World — mHiddenWorld's user table, via
 * NotifyCmd — and a World does not survive a device switch or a worklet death.
 * The registration therefore dies with it, and every /n_go and /n_end after the
 * rebuild goes nowhere, silently, to a client that has no reason to suspect it
 * needs to ask again.
 *
 * The origins themselves DO survive: an origin token is minted by the transport
 * (internOrigin) and lives in the transport's registry, which a rebuild does not
 * touch. So recording which of them had registered is enough to put the world
 * back the way the client left it.
 *
 * This is the guest's own business and it is spelled out here rather than in
 * clockwork on purpose: /notify is scsynth vocabulary, and a host that recognised
 * it would be a host that had learned an engine's verbs.
 */
struct NotifyPersist {
    static constexpr uint32_t kMagic = 0x53434E31;  // 'SCN1'
    static constexpr uint32_t kMaxOrigins = 64;

    uint32_t magic;
    // Odd WHILE WRITING. The region survives a worklet that was killed without
    // warning, so a reader can arrive at a half-finished update; an even
    // generation is the only evidence the bytes below are whole.
    uint32_t generation;
    uint32_t count;
    uint32_t origins[kMaxOrigins];
};

struct ScsynthDsp {
    DspConfig config{};
    DspHost   host{};
    World*    world = nullptr;
    // The timetag of the block currently rendering, so dsp_osc can place a
    // timestamped message at its exact sample within that block.
    int64_t   block_time = 0;
    // OSC 32.32 fixed-point units to samples: sample_rate / 2^32.
    double    osc_to_samples = 0.0;
    // The persistent notify table, or nullptr if the host offered no region —
    // which is a working state: nothing is remembered and nothing is restored.
    NotifyPersist* notify = nullptr;
};

// The one instance a reply can be routed through. scsynth answers a command by
// calling a function pointer on the ReplyAddress it was given, and that
// signature has no room for a context pointer of ours — mReplyData already
// carries the origin token. There is exactly one World per process here, so a
// file-scope pointer is honest rather than a shortcut.
ScsynthDsp* g_dsp = nullptr;

} // namespace

// scsynth declares these in its own headers with C++ linkage and does not
// export them anywhere this file can reach, which is the same reason
// engine_api.h leaves them out and says so. Declared here to match.
int  PerformOSCMessage(World* world, int size, char* data, ReplyAddress* reply);
void PerformOSCBundle(World* world, OSC_Packet* packet);

namespace {

// scsynth's reply path, forwarded to clockwork. `origin` came in with the
// message and goes back out with the answer, which is how clockwork routes a
// reply to the client that asked rather than broadcasting it.
void reply_to_host(struct ReplyAddress* addr, char* msg, int size) {
    if (!g_dsp || !g_dsp->host.emit_osc || size <= 0) return;
    const uint32_t origin =
        addr ? (uint32_t)(uintptr_t)addr->mReplyData : 0u;
    g_dsp->host.emit_osc(g_dsp->host.ctx, origin,
                         reinterpret_cast<const uint8_t*>(msg), (uint32_t)size);
}

// The origin of the message being dispatched right now.
//
// A synchronous reply reads it off the ReplyAddress it was handed, but the
// sample loader answers LATER, from its own thread, long after that address is
// gone — so the token has to be captured while the request is still being
// dispatched. Plain, not atomic, and deliberately: dsp_osc writes it and the
// loader's enqueue reads it on that same audio thread, within the same call.
// The loader's I/O thread never touches it; it carries the copy it was given.
uint32_t g_dispatch_origin = 0;

} // namespace

// The two doors the sample loader needs. Free functions, so there is no wiring
// step between construction and first use for a caller to miss.
extern "C" int supersonic_emit_osc(uint32_t origin, const uint8_t* bytes, uint32_t len) {
    if (!g_dsp || !g_dsp->host.emit_osc || !bytes || len == 0) return 0;
    return g_dsp->host.emit_osc(g_dsp->host.ctx, origin, bytes, len);
}

extern "C" uint32_t supersonic_dispatch_origin(void) { return g_dispatch_origin; }

namespace {

// Place a timestamped message at its exact sample within the current block.
// scsynth reads mSampleOffset when it creates a node, which is what makes a
// bundle sample-accurate rather than merely block-accurate.
void apply_offset(ScsynthDsp* d, int64_t when, int64_t block_time) {
    World* w = d->world;
    if (when == 0 || when == 1) {   // the OSC "immediately" sentinels
        w->mSampleOffset = 0;
        w->mSubsampleOffset = 0.0f;
        return;
    }
    /*
     * Against THIS block's start, not the last one rendered.
     *
     * This used d->block_time, which dsp_process sets — and dsp_osc runs
     * BEFORE dsp_process for the block a message belongs to, so it was always
     * the previous block's start. `when - block_time` came out a whole block
     * too large, the clamp below pinned it, and every scheduled synth landed
     * on a buffer boundary. Block-accurate, never sample-accurate, which is
     * exactly what OffsetOut.ar exists to avoid.
     */
    const float diff = (float)((double)(when - block_time) * d->osc_to_samples) + 0.5f;
    const float floored = std::floor(diff);
    w->mSampleOffset = (int)floored;
    w->mSubsampleOffset = diff - floored;
    if (w->mSampleOffset < 0) w->mSampleOffset = 0;
    else if (w->mSampleOffset >= w->mBufLength) w->mSampleOffset = w->mBufLength - 1;
}

// The engine's audio is one flat channel-major array: every OUTPUT bus of
// mBufLength samples, then every INPUT bus. In.ar reads the input region and
// Out.ar writes the output region, so the adapter's whole audio job is to fill
// one half before the block and drain the other after it.
inline float* outputRegion(World* w) { return w->mAudioBus; }
inline float* inputRegion(World* w)  {
    return w->mAudioBus + (size_t)w->mNumOutputs * (size_t)w->mBufLength;
}

// ── The notify table ─────────────────────────────────────────────────────────

/* Read a big-endian int32 at `off`, or fail. OSC is big-endian on the wire. */
bool osc_int32_at(const uint8_t* b, uint32_t len, uint32_t off, int32_t* out) {
    if (off + 4 > len) return false;
    *out = (int32_t)(((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16)
                   | ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3]);
    return true;
}

/* OSC pads every string to a 4-byte boundary, including its terminator. */
inline uint32_t osc_padded(uint32_t n) { return (n + 4u) & ~3u; }

/*
 * Is this "/notify <int>"? If so, hand back the flag.
 *
 * Deliberately a narrow hand-rolled match rather than a full parse: this runs on
 * the audio thread for EVERY message, and the answer is no for almost all of
 * them. Bundles are not unwrapped — /notify inside a timestamped bundle is not
 * a thing any client does, and guessing wrong here would mean recording a
 * registration that never happened.
 */
bool parse_notify(const uint8_t* b, uint32_t len, int32_t* flag) {
    static const char kAddr[] = "/notify";
    const uint32_t addr_len = sizeof(kAddr) - 1;
    if (len < osc_padded(addr_len) + 4 + 4) return false;
    if (std::memcmp(b, kAddr, addr_len) != 0 || b[addr_len] != 0) return false;

    const uint32_t tags = osc_padded(addr_len);
    // ",i" — anything else (",ii" with a client id, or no args) is not the
    // simple registration this restores.
    if (b[tags] != ',' || b[tags + 1] != 'i' || b[tags + 2] != 0) return false;
    return osc_int32_at(b, len, tags + 4, flag);
}

/* Add or drop an origin, wrapped in the generation stamp a torn read looks for. */
void notify_record(NotifyPersist* n, uint32_t origin, bool on) {
    if (!n) return;
    n->generation |= 1u;                       // odd: mid-write
    std::atomic_signal_fence(std::memory_order_seq_cst);

    uint32_t w = 0;
    for (uint32_t r = 0; r < n->count && r < NotifyPersist::kMaxOrigins; ++r)
        if (n->origins[r] != origin) n->origins[w++] = n->origins[r];
    if (on && w < NotifyPersist::kMaxOrigins) n->origins[w++] = origin;
    n->count = w;
    n->magic = NotifyPersist::kMagic;

    std::atomic_signal_fence(std::memory_order_seq_cst);
    n->generation = (n->generation + 1u) & ~1u;  // even: whole again
}

const DspInfo kInfo = {
    /* name                    */ "scsynth",
    /* version                 */ "3.14.1",
    /* holds_schedule          */ 0,          // clockwork holds timed messages for us
    /* arena_bytes_wanted      */ 0,          // no claim: allocate from the system, as always
    /* arena_bulk_bytes_wanted */ 0,
    /* wants_events            */ 0,          // a client relays a keyboard to us
    // What our window is: the node tree mirror, so a client reading the
    // window by hand can check it is ours, and which layout, from the
    // arena table (node_tree.h).
    /* window_magic            */ NODE_TREE_WINDOW_MAGIC,
    /* window_version          */ NODE_TREE_WINDOW_VERSION,
};

} // namespace

extern "C" {

const DspInfo* dsp_describe(void) { return &kInfo; }

/*
 * Guest-internal: SC code deep in the server only ever holds a World*, and the
 * host vtable lives on the ScsynthDsp instance. BufFreeCmd::Stage4 needs to
 * hand a finished block back to whoever allocated it, so the pointer is parked
 * here at bring-up. Guest plumbing for a guest problem — clockwork sees only
 * a call carrying a pointer.
 */
static DspHost g_host_for_free{};

/*
 * The session clock, and the time of the block being rendered.
 *
 * A UGen runs deep inside the server holding only a Unit*, so anything it needs
 * from clockwork has to be parked somewhere it can reach. These two are what
 * the clock UGens read: DspConfig::clock is the ClockworkClockState mirror the
 * clockwork publishes — the same bytes JavaScript polls, and the same ones Link
 * writes its converged tempo into when it is running — and block_time is the
 * OSC timetag for the block, already carried on every dsp_process.
 *
 * They exist so no UGen has to know whether Link is present. The clock is the
 * clock; who drives it is clockwork's business.
 */
static const ClockworkClockState* g_session_clock = nullptr;
static std::atomic<int64_t>   g_block_osc_time{0};

extern "C" const ClockworkClockState* supersonic_scsynth_session_clock() {
    return g_session_clock;
}

/*
 * The inbox, for /b_allocPtr. A client names a sample's position as an OFFSET
 * from the inbox base — dsp_api.h: offsets, not pointers, in any message that
 * names a position in a lane — and the guest adds the base it was given at
 * dsp_new. On the web the sum is the address the client already had; natively
 * the engine maps the lane wherever it likes, and only this sum is right.
 */
static const uint8_t* g_inbox_base  = nullptr;
static uint32_t       g_inbox_bytes = 0;
static uint8_t*       g_outbox_base  = nullptr;
static uint32_t       g_outbox_bytes = 0;
extern "C" uint8_t* supersonic_outbox_base(void)  { return g_outbox_base; }

extern "C" uint32_t supersonic_outbox_bytes(void) { return g_outbox_base ? g_outbox_bytes : 0; }

extern "C" const uint8_t* supersonic_inbox_base(void)  { return g_inbox_base; }
extern "C" uint32_t       supersonic_inbox_bytes(void) {
#if defined(__EMSCRIPTEN__)
    // On the web the inbox is the LAST region and grows by growing the memory
    // (memory_layout.js), so its size is "from its base to the end of the
    // memory" — measured now, because the number handed over at dsp_new was
    // the size at boot and a sample pool has grown past it since. Nothing
    // tells a guest a lane grew; this is how it does not need telling.
    if (!g_inbox_base) return 0;
    const uint64_t end = (uint64_t)__builtin_wasm_memory_size(0) * 65536u;
    const uint64_t base = (uint64_t)(uintptr_t)g_inbox_base;
    return end > base ? (uint32_t)(end - base) : 0u;
#else
    return g_inbox_bytes;
#endif
}

extern "C" double supersonic_scsynth_block_ntp() {
    // OSC timetag -> NTP seconds. 2^32 is one second.
    return static_cast<double>(g_block_osc_time.load(std::memory_order_relaxed))
         / 4294967296.0;
}

/*
 * Whether a pointer lies in the inbox. A buffer that points INTO THE INBOX was
 * never allocated by anyone on this side: the client wrote the sample there
 * and /b_allocPtr pointed the buffer at it in place. Freeing such a buffer
 * means the engine is done with the range — it does not mean the memory is
 * the engine's to give back — and handing a lane address to any allocator is
 * a crash on native and heap corruption on the web. So every free the engine
 * performs asks this first: the host-routed one below, and scsynth's own
 * sc_free, which World_Cleanup and a re-alloc reach directly.
 */
extern "C" int supersonic_inbox_contains(const void* ptr) {
    const uint8_t* base  = g_inbox_base;
    const uint8_t* p     = static_cast<const uint8_t*>(ptr);
    if (!base || !p) return 0;
    const uint64_t bytes = supersonic_inbox_bytes();
    return (p >= base && p < base + bytes) ? 1 : 0;
}

void supersonic_guest_free_bytes(void* ptr) {
    if (!ptr) return;
    if (supersonic_inbox_contains(ptr)) return;   // the client's range, not ours
    if (g_host_for_free.free_bytes)
        g_host_for_free.free_bytes(g_host_for_free.ctx, ptr);
}

// A buffer bound to an ASSET is letting go: its bytes are the client's, in
// the inbox, and the client reclaims them when clockwork tells it the asset
// is released (dsp_api.h, "Assets"). A buffer whose data came from /b_allocPtr
// (the web client's spelling) is not an asset clockwork knows; the release
// is then nothing, and that client hears /supersonic/buffer/freed as before.
extern "C" void supersonic_guest_release_buffer(int bufnum) {
    if (bufnum < 0) return;
    if (g_host_for_free.asset_release)
        g_host_for_free.asset_release(g_host_for_free.ctx, (uint32_t)bufnum);
}

struct Dsp* dsp_new(const DspConfig* config, const DspHost* host, const char** err) {
    if (!config) { if (err) *err = "no DspConfig"; return nullptr; }
    auto* d = new (std::nothrow) ScsynthDsp;
    if (!d) { if (err) *err = "out of memory"; return nullptr; }
    d->config = *config;
    if (host) { d->host = *host; g_host_for_free = *host; }
    g_inbox_base  = static_cast<const uint8_t*>(config->inbox);
    g_inbox_bytes = config->inbox_bytes;
    g_outbox_base  = static_cast<uint8_t*>(config->outbox);
    g_outbox_bytes = config->outbox_bytes;

    WorldOptions options;
    // Externally driven: clockwork owns the device and the clock, so the
    // engine must never start a driver or lock memory of its own. This is what
    // "nrt" means in scsynth-nrt — not offline, but not self-driving. It also
    // means every sequenced command runs its stages inline on the audio
    // thread, so a verb that names a FILE must be answered before it gets
    // here (the web client turns /b_allocRead into /b_allocPtr; native needs
    // the same in front of the engine) — decoding inside the tick is a glitch.
    options.mRealTime      = false;
    options.mMemoryLocking = false;

    // What only the host can know, and all that crosses the seam.
    options.mBufLength            = config->block_size;
    options.mNumInputBusChannels  = config->max_input_channels;
    options.mNumOutputBusChannels = config->max_output_channels;
    options.mPreferredSampleRate  = (uint32_t)(config->sample_rate + 0.5);

    /*
     * The shape of the graph, READ FROM THE CONFIG BLOCK the client wrote.
     *
     * These were compile-time SC_* constants until 2026-08-31, which meant
     * scsynthOptions could not change any of them: a caller asking for
     * maxNodes 2048 got 1024 and nothing said so. audio_bus_channels passed
     * only because SC_NUM_AUDIO_BUS_CHANNELS happened to be big enough for
     * what it asked.
     *
     * The demolition was right to stop forwarding these across the seam —
     * the host has no business sizing a graph it cannot see — but the other
     * half never landed: the guest has to read them itself. The block arrives
     * as DspConfig::guest_config, written before the engine is built, and the
     * SC_* constants are the fallback for a zero slot so a host that writes no
     * block still boots.
     *
     * THROUGH THE POINTER WE WERE HANDED, not through the host's segment. This
     * read the same bytes as `shared_memory + GUEST_CONFIG_START`, which only
     * worked because the guest is linked into the host and could see its
     * globals — the boundary hands over a base and a length precisely so a
     * guest does not have to know the host's layout to find its own block.
     *
     * A short block is refused rather than read past: slot 15 is the highest
     * this asks for, so anything smaller is a host and a guest disagreeing
     * about the shape, and the SC_* defaults are the honest answer.
     */
    static constexpr uint32_t kHighestSlot = 15;
    const uint32_t* cfg = nullptr;
    if (config->guest_config
        && config->guest_config_bytes >= (kHighestSlot + 1) * sizeof(uint32_t))
        cfg = static_cast<const uint32_t*>(config->guest_config);
    auto slot = [cfg](int i, uint32_t fallback) -> uint32_t {
        const uint32_t v = cfg ? cfg[i] : 0u;
        return v ? v : fallback;
    };
    // Slots that are legitimately zero cannot use `slot`; they still need the
    // absent-block case to read as zero rather than dereference nothing.
    auto raw = [cfg](int i) -> uint32_t { return cfg ? cfg[i] : 0u; };

    options.mNumBuffers           = slot(0,  SC_NUM_BUFFERS);
    options.mMaxNodes             = slot(1,  SC_MAX_NODES);
    options.mMaxGraphDefs         = slot(2,  SC_MAX_GRAPH_DEFS);
    options.mMaxWireBufs          = slot(3,  SC_MAX_WIRE_BUFS);
    options.mNumAudioBusChannels  = slot(4,  SC_NUM_AUDIO_BUS_CHANNELS);
    options.mNumControlBusChannels= slot(7,  SC_NUM_CONTROL_BUS_CHANNELS);
    options.mRealTimeMemorySize   = slot(9,  SC_REAL_TIME_MEMORY_SIZE);
    options.mNumRGens             = slot(10, SC_NUM_RGENS);
    // Slot 13 is loadGraphDefs and is legitimately 0: the host sends
    // synthdefs over OSC, so `slot`'s zero-means-absent rule cannot be used
    // for it. Same for verbosity.
    options.mLoadGraphDefs        = static_cast<int>(raw(13));
    options.mVerbosity            = static_cast<int>(raw(15));

    d->osc_to_samples = config->sample_rate / 4294967296.0;
    // The clock clockwork publishes, for this instance's UGens.
    g_session_clock = config->clock;

    // The node-tree mirror is the guest's, and so is initialising it.
    //
    // Clockwork provides the shared-memory region and the mirror mechanism —
    // a generic way for a guest to publish state a client can poll without
    // asking. What goes in it is the guest's business: scsynth publishes its
    // node tree, and a guest with no nodes publishes something else or nothing.
    //
    // Upstream initialised this from its audio_processor, back when the host
    // knew what a node was. Clockwork stopped knowing during the demolition,
    // correctly, and the call had nowhere to go until here.
    //
    // BEFORE THE WORLD, NOT AFTER. EngineCore_New creates the root group, and
    // creating it publishes it — Node_StateMsg(kNode_Go) writes it into the
    // mirror. Initialising afterwards therefore WIPED the root group straight
    // back out, and every later node landed in a mirror with no root: the tree
    // read one node short forever, and getTree() found no root to hang the
    // children off, so it returned nothing at all. 58 node_tree cases, one
    // line in the wrong order.
    // Bind the window clockwork handed us, then lay it out ourselves: the
    // host zeroes the region and knows nothing of its shape, so the empty
    // slots (-1) and the header are ours to write.
    supersonic_node_tree_bind(config->shm_window, config->shm_window_bytes);
    if (NodeEntry* e = supersonic_node_tree_entries()) {
        const uint32_t rows = (config->shm_window_bytes - NODE_TREE_HEADER_SIZE)
                            / NODE_TREE_ENTRY_SIZE;
        for (uint32_t i = 0; i < rows; ++i) e[i].id = -1;
    }
    NodeTree_InitIndices();

    const char* engineErr = nullptr;
    d->world = EngineCore_New(&options, &engineErr);
    if (!d->world) {
        delete d;
        if (err) *err = engineErr ? engineErr : "EngineCore_New failed";
        return nullptr;
    }

    // THE ROOT GROUP HAS TO BE PUBLISHED BY HAND.
    //
    // Every other node reaches the mirror through Node_StateMsg, which the
    // engine calls on kNode_Go / kNode_End / kNode_Move. The root group is
    // built directly inside World creation and never goes through that path,
    // so nothing ever announces it.
    //
    // Upstream did this in its audio_processor, right after World_New, with
    // the same comment. It was lost when clockwork stopped knowing what a
    // node is — correctly, since a root group is not a clockwork concept — and
    // the mirror has been one node short ever since. The cost was not one
    // test: getTree() looks for the root to hang the hierarchy off, so with no
    // root it returned an EMPTY tree rather than a tree missing its root, and
    // 58 node_tree cases failed on counts that were never off by one.
    if (d->world->mTopGroup) {
        NodeTreeHeader* tree_header  = supersonic_node_tree_header();
        NodeEntry*      tree_entries = supersonic_node_tree_entries();
        if (tree_header && tree_entries)
        NodeTree_Add(&d->world->mTopGroup->mNode, tree_header, tree_entries);
    }

    /*
     * PUT THE CLIENTS' /notify REGISTRATIONS BACK.
     *
     * Last, because it replays real commands into the engine and everything
     * they touch has to exist first. The generation stamp is the whole check:
     * an odd one means the previous instance was killed part-way through an
     * update and these bytes are not a table, so it is discarded rather than
     * half-believed. A wrong magic means nobody has ever written here.
     */
    if (config->persistent && config->persistent_bytes >= sizeof(NotifyPersist)) {
        d->notify = reinterpret_cast<NotifyPersist*>(config->persistent);
        NotifyPersist* n = d->notify;
        const bool intact = n->magic == NotifyPersist::kMagic
                         && (n->generation & 1u) == 0u
                         && n->count <= NotifyPersist::kMaxOrigins;
        if (intact) {
            for (uint32_t i = 0; i < n->count; ++i) {
                // Rebuild the command the client sent rather than reaching into
                // NotifyCmd: replaying the message is what makes this identical
                // to the client having asked again, reply and all.
                static const uint8_t kNotifyOn[] = {
                    '/','n','o','t','i','f','y', 0,
                    ',','i', 0, 0,
                    0, 0, 0, 1,
                };
                ReplyAddress reply = {};
                reply.mReplyFunc = reply_to_host;
                reply.mReplyData =
                    reinterpret_cast<void*>(static_cast<uintptr_t>(n->origins[i]));
                PerformOSCMessage(d->world, (int)sizeof(kNotifyOn),
                                  reinterpret_cast<char*>(
                                      const_cast<uint8_t*>(kNotifyOn)),
                                  &reply);
            }
        } else {
            // Start a fresh table rather than carrying forward a torn one.
            n->magic = NotifyPersist::kMagic;
            n->generation = 0;
            n->count = 0;
        }
    }

    g_dsp = d;
    return reinterpret_cast<struct Dsp*>(d);
}

// An asset is a sample: interleaved float frames the client decoded into the
// inbox, keyed by buffer number. Bind the buffer to them where they lie. A
// buffer that already holds something must be freed first — the audio thread
// cannot give heap memory back here, and the client's own /b_free is the
// honest way to ask.
int dsp_asset(struct Dsp* dsp, const ClockworkAsset* asset) {
    auto* d = reinterpret_cast<ScsynthDsp*>(dsp);
    if (!d || !d->world || !asset || asset->struct_bytes < sizeof(ClockworkAsset)) return 1;
    if (asset->kind != CLOCKWORK_ASSET_AUDIO_F32) return 2;   // a buffer holds audio; raw bytes mean nothing here
    World* w = d->world;
    const int bufnum = (int)asset->id;
    if (bufnum < 0 || bufnum >= (int)w->mNumSndBufs) return 3;
    SndBuf* nrt = World_GetNRTBuf(w, bufnum);
    if (nrt && nrt->data && !supersonic_inbox_contains(nrt->data)) return 4;  // occupied by heap data: /b_free it first
    if (asset->frames == 0 || asset->channels == 0 || !(asset->sample_rate > 0.f)) return 5;
    if (buffer_set_data(w, bufnum, (float*)asset->bytes, (int)asset->frames, (int)asset->channels,
                        (double)asset->sample_rate, false) != 0) return 6;
    return 0;
}

void dsp_free(struct Dsp* dsp) {
    auto* d = reinterpret_cast<ScsynthDsp*>(dsp);
    if (!d) return;
    if (g_dsp == d) g_dsp = nullptr;
    if (d->world) World_Cleanup(d->world, true);
    delete d;
}

void dsp_process(struct Dsp* dsp,
                 const float* const* in, uint32_t n_in,
                 float* const* out, uint32_t n_out,
                 uint32_t frames,
                 int64_t block_time) {
    auto* d = reinterpret_cast<ScsynthDsp*>(dsp);
    if (!d || !d->world) {
        for (uint32_t c = 0; c < n_out; ++c)
            if (out && out[c]) std::memset(out[c], 0, frames * sizeof(float));
        return;
    }
    World* w = d->world;
    d->block_time = block_time;
    g_block_osc_time.store(block_time, std::memory_order_relaxed);
    const uint32_t block = (uint32_t)w->mBufLength;
    const uint32_t n = frames < block ? frames : block;

    // IN: copy the host's channels into the input region before the graph runs,
    // because In.ar reads it during the pass.
    if (in) {
        float* dst = inputRegion(w);
        const uint32_t ins = n_in < (uint32_t)w->mNumInputs ? n_in : (uint32_t)w->mNumInputs;
        for (uint32_t c = 0; c < ins; ++c)
            if (in[c]) std::memcpy(dst + (size_t)c * block, in[c], n * sizeof(float));
    }

    // Zeroes the output buses and advances the block counter, so a channel
    // nothing writes this block reads as silence rather than as last block.
    EngineCore_BeginBlock(w);
    EngineCore_RunBlock(w, n_in);
    // /tr, /n_go, /n_end produced by this pass.
    EngineCore_FlushNotifications(w);

    // OUT: drain the output region. What leaves here is what clockwork taps
    // for a client (the OUT tap, written after this returns): no tap of our
    // own.
    const float* src = outputRegion(w);
    const uint32_t outs = n_out < (uint32_t)w->mNumOutputs ? n_out : (uint32_t)w->mNumOutputs;
    for (uint32_t c = 0; c < outs; ++c)
        if (out && out[c]) std::memcpy(out[c], src + (size_t)c * block, n * sizeof(float));
    // Channels the engine has no bus for still owe the host silence.
    for (uint32_t c = outs; c < n_out; ++c)
        if (out && out[c]) std::memset(out[c], 0, frames * sizeof(float));
}

/*
 * Hand one OSC message or bundle to the engine.
 *
 * `origin` is carried on the ReplyAddress rather than anywhere else because
 * that is the only channel scsynth gives a command for saying who asked; it
 * comes back out through reply_to_host and clockwork routes the answer.
 *
 * `when` becomes a sample offset within the current block. A DSP that declared
 * holds_schedule=0, as this one does, is handed messages by clockwork when
 * they are due — so `when` is not "later", it is "where in this block".
 */
void dsp_osc(struct Dsp* dsp, const uint8_t* bytes, uint32_t len,
             int64_t when, uint32_t origin, int64_t block_time) {
    auto* d = reinterpret_cast<ScsynthDsp*>(dsp);
    if (!d || !d->world || !bytes || len == 0) return;

    ReplyAddress reply = {};
    reply.mReplyFunc = reply_to_host;
    reply.mReplyData = reinterpret_cast<void*>(static_cast<uintptr_t>(origin));

    // For anything that answers after this call returns — see g_dispatch_origin.
    g_dispatch_origin = origin;

    // Note who is listening, BEFORE the engine acts on it. The table is what
    // survives this instance; see NotifyPersist.
    //
    // Origin 0 IS recorded, though it looks like it should not be. It is not a
    // missing sender — dsp_api.h gives it a meaning, "broadcast to whoever
    // listens", which is what an in-process or embedded caller gets
    // (ClockworkEngine::sendOSC assigns it). Such a caller receives /n_go like any
    // other, so its registration is as real as a socket's and is restored the
    // same way. Skipping it left the whole embedded and NIF surface unrestored.
    if (d->notify) {
        int32_t flag = 0;
        if (parse_notify(bytes, len, &flag))
            notify_record(d->notify, origin, flag != 0);
    }

    apply_offset(d, when, block_time);

    char* osc = reinterpret_cast<char*>(const_cast<uint8_t*>(bytes));
    // A bundle starts with "#bundle\0"; anything else is a single message.
    const bool is_bundle = len >= 8 && std::memcmp(bytes, "#bundle", 8) == 0;
    if (is_bundle) {
        OSC_Packet packet = {};
        packet.mData      = osc;
        packet.mSize      = (int)len;
        packet.mIsBundle  = true;
        packet.mReplyAddr = reply;
        PerformOSCBundle(d->world, &packet);
    } else {
        PerformOSCMessage(d->world, (int)len, osc, &reply);
    }
}

/*
 * The name of the first synthdef in a /d_recv blob.
 *
 * Clockwork caches definition blobs so a device switch can restore them, and
 * keys that cache by whatever this returns. It cannot parse the blob itself —
 * SCgf is scsynth's format, and a DSP with a different one would answer
 * differently — which is exactly why this call is on the DSP's side of the
 * seam. It used to be StateCache::extractSynthDefName in clockwork; moving
 * it here is what let clockwork stop knowing what a synthdef is.
 *
 * SCgf layout: magic(4) version(4:BE) numDefs(2) [defSize(4) if v3] nameLen(1)
 * name(nameLen).
 */
uint32_t dsp_definition_name(const uint8_t* bytes, uint32_t len,
                             char* out, uint32_t cap) {
    // magic(4) + version(4) + numDefs(2) + nameLen(1)
    if (!bytes || len < 11) return 0;
    if (std::memcmp(bytes, "SCgf", 4) != 0) return 0;

    const int32_t version = ((int32_t)bytes[4] << 24) | ((int32_t)bytes[5] << 16)
                          | ((int32_t)bytes[6] << 8)  |  (int32_t)bytes[7];

    uint32_t offset = 10;              // past magic, version, numDefs
    if (version == 3) offset += 4;     // v3 carries defSize before the name
    if (offset >= len) return 0;

    const uint32_t nameLen = bytes[offset];
    ++offset;
    if (nameLen == 0 || offset + nameLen > len) return 0;

    if (out && cap > 0) {
        const uint32_t n = nameLen < cap - 1 ? nameLen : cap - 1;
        std::memcpy(out, bytes + offset, n);
        out[n] = 0;
    }
    return nameLen;
}

uint32_t dsp_status(struct Dsp* dsp, uint32_t selector) {
    (void)dsp; (void)selector;
    return 0;
}

} // extern "C"
