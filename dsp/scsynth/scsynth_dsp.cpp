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
 *
 * WHAT TAU NO LONGER PROVIDES, and scsynth therefore owns: buses, the
 * node tree, the buffer table, the wire pool. The seam passes channels and
 * frames; everything inside the graph is the guest's business, which is why
 * the eight sizing options the old options block carried stop at this file.
 *
 * THE INSTANCE. Each dsp_new makes one SupersonicGuest: the host's table, the
 * memory and the clock the host handed over, the World, the node-tree mirror
 * and the piano table. The World names its instance (HiddenWorld::mGuest),
 * and every door the server has to the host (guest_doors.h) takes the World
 * and finds the instance through it, so nothing here is kept process-wide
 * and any number of instances can share a process.
 */
#include "SC_FloatToInt.h"
#include "dsp_api.h"
#include "engine_api.h"
#include "guest_doors.h"
#include "scsynth_config.h"
#include "scsynth_options.h"
#include "buffer_commands.h"   // buffer_set_data: a buffer onto an asset's frames

#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_HiddenWorld.h"
#include "SC_Group.h"        // mTopGroup is only a forward declaration in SC_World.h
#include "SC_InterfaceTable.h"   // ScopeBufferHnd, the ugens' shape of a scope handle
#include "node_tree.h"
#include "scsynth_scope.h"

// SC_Reply.h only forward-declares ReplyAddress; the layout — and mReplyData,
// which is where the origin token rides — is in the Impl header. OSC_Packet is
// the server's own.
#include "SC_Reply.h"
#include "SC_ReplyImpl.hpp"
#include "OSC_Packet.h"

#include "malloc_aligned.hpp"   // nova: the fallback when a host offers no heap

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

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

// One instance of the guest (guest_doors.h): what dsp_new makes, in the
// memory the host's heap gives it, and what a World belongs to.
struct SupersonicGuest {
    DspConfig config{};
    DspHost   host{};
    World*    world = nullptr;
    // The timetag of the block currently rendering, so dsp_osc can place a
    // timestamped message at its exact sample within that block, and the
    // clock UGens can read the block's time.
    ClockworkTimetag block_time{};
    // OSC 32.32 fixed-point units to samples: sample_rate / 2^32.
    double    osc_to_samples = 0.0;
    // The persistent notify table, or nullptr if the host offered no region —
    // which is a working state: nothing is remembered and nothing is restored.
    NotifyPersist* notify = nullptr;
    // The node-tree mirror in the window the host handed over (node_tree.h).
    NodeTreeMirror tree{};
    // MdaPiano's sample table, this instance's copy (piano_wavetable.cpp):
    // swapped on the audio thread, freed off it.
    short*  piano_table  = nullptr;
    size_t  piano_frames = 0;
    // How often ScopeOut2 found its slot released under it (guest_doors.h).
    uint32_t scope_defence_trips = 0;
};

namespace {

// The instance a World belongs to, or nullptr for no World.
inline SupersonicGuest* guestOf(World* world) {
    return world && world->hw ? world->hw->mGuest : nullptr;
}

// One line to a host's log, finished text in: cut to fit and terminated, since
// the text need not be (the OSC dump builds its line in place).
void log_line(const DspHost& host, const char* text, uint32_t len) {
    if (!text || len == 0 || !host.log) return;
    char line[1024];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    std::memcpy(line, text, len);
    line[len] = '\0';
    host.log(host.ctx, 6, line);
}

// Memory for the instance itself, before and after it exists: the host's
// heap when there is a host (guest_heap.h: nothing of an instance is on the
// system allocator), nova's aligned malloc when there is none.
void* instance_alloc(const DspHost& host, size_t bytes) {
    return host.alloc_bytes ? host.alloc_bytes(host.ctx, bytes) : nova::malloc_aligned(bytes);
}

void instance_free(const DspHost& host, void* ptr) {
    if (!ptr) return;
    if (host.alloc_bytes) {
        if (host.free_bytes) host.free_bytes(host.ctx, ptr);
        return;
    }
    nova::free_aligned(ptr);
}

void release_instance(SupersonicGuest* d) {
    const DspHost host = d->host;   // the table outlives the instance it frees
    d->~SupersonicGuest();
    instance_free(host, d);
}

/*
 * Whether a pointer lies in the assets area. A buffer that points INTO THE ASSETS was
 * never allocated by anyone on this side: the client wrote the sample there
 * and /b_allocPtr pointed the buffer at it in place. Freeing such a buffer
 * means the engine is done with the range — it does not mean the memory is
 * the engine's to give back — and handing a lane address to any allocator is
 * a crash on native and heap corruption on the web. So every free the engine
 * performs asks this first: the host-routed one below, and scsynth's own
 * sc_free, which World_Cleanup and a re-alloc reach directly.
 */
uint32_t assets_bytes(const SupersonicGuest* d) {
#if defined(__EMSCRIPTEN__)
    // On the web the assets area is the LAST region and grows by growing the memory
    // (memory_layout.js), so its size is "from its base to the end of the
    // memory" — measured now, because the number handed over at dsp_new was
    // the size at boot and a sample pool has grown past it since. Nothing
    // tells a guest a lane grew; this is how it does not need telling.
    if (!d->config.assets) return 0;
    const uint64_t end = (uint64_t)__builtin_wasm_memory_size(0) * 65536u;
    const uint64_t base = (uint64_t)(uintptr_t)d->config.assets;
    return end > base ? (uint32_t)(end - base) : 0u;
#else
    return d->config.assets_bytes;
#endif
}

bool assets_contains(const SupersonicGuest* d, const void* ptr) {
    const uint8_t* base = static_cast<const uint8_t*>(d->config.assets);
    const uint8_t* p    = static_cast<const uint8_t*>(ptr);
    if (!base || !p) return false;
    return p >= base && p < base + assets_bytes(d);
}

} // namespace

// scsynth declares these in its own headers with C++ linkage and does not
// export them anywhere this file can reach, which is the same reason
// engine_api.h leaves them out and says so. Declared here to match.
int  PerformOSCMessage(World* world, int size, char* data, ReplyAddress* reply);
void PerformOSCBundle(World* world, OSC_Packet* packet);

namespace {

// scsynth's reply path, forwarded to clockwork. `origin` came in with the
// message and goes back out with the answer, which is how clockwork routes a
// reply to the client that asked rather than broadcasting it. The World the
// message came to rides on the address too (ReplyAddress::mWorld), and names
// the instance whose host answers.
void reply_to_host(struct ReplyAddress* addr, char* msg, int size) {
    SupersonicGuest* d = addr ? guestOf(addr->mWorld) : nullptr;
    if (!d || !d->host.emit_osc || size <= 0) return;
    const uint32_t origin = (uint32_t)(uintptr_t)addr->mReplyData;
    d->host.emit_osc(d->host.ctx, origin, reinterpret_cast<const uint8_t*>(msg), (uint32_t)size);
}

// Place a timestamped message at its exact sample within the current block.
// scsynth reads mSampleOffset when it creates a node, which is what makes a
// bundle sample-accurate rather than merely block-accurate.
void apply_offset(SupersonicGuest* d, ClockworkTimetag when, ClockworkTimetag block_time) {
    World* w = d->world;
    if (clockwork_timetag_is_immediate(when)) {
        w->mSampleOffset = 0;
        w->mSubsampleOffset = 0.0f;
        return;
    }
    // Measured from the start of the block about to render: dsp_osc runs
    // before dsp_process sets d->block_time for that block. A bundle due
    // before the block starts at its first frame and one due after it at its
    // last; the clamp is in float because a late bundle's offset can be far
    // outside int's range.
    const float diff = (float)((double)clockwork_timetag_offset(block_time, when) * d->osc_to_samples) + 0.5f;
    const float floored = std::floor(diff);
    const float last = (float)(w->mBufLength - 1);
    w->mSampleOffset = !(floored > 0.0f) ? 0 : floored >= last ? w->mBufLength - 1 : sc_float_to_int32(floored);
    w->mSubsampleOffset = diff - floored;
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
    /* name                    */ SUPERSONIC_NAME_STRING,
    /* version                 */ SUPERSONIC_VERSION_STRING,
    /* holds_schedule          */ 0,          // clockwork holds timed messages for us
    /* memory_bytes_wanted      */ 0,          // nothing static: the pool's size is in the options (dsp_memory_bytes), and SC_World.cpp carves it
    /* memory_bulk_bytes_wanted */ 0,
    /* wants_events            */ 0,          // a client relays a keyboard to us
    // What our window is: the node tree mirror, so a client reading the
    // window by hand can check it is ours, and which layout, from the
    // arena table (node_tree.h).
    /* window_magic            */ NODE_TREE_WINDOW_MAGIC,
    /* window_version          */ NODE_TREE_WINDOW_VERSION,
    // A World is laid out at boot for one rate and one channel count — its
    // wire buffers, its busses, its UGen graphs — so no device change is
    // taken live: every rate or channel change is a cold restart, the
    // instance destroyed and built again (clockwork's docs/RESTARTS.md),
    // and dsp_reconfigure below is never asked.
    /* survives                */ 0,
    // Any number in a process: the server keeps nothing process-wide. Every
    // World names its instance and every door takes the World, its lookup
    // tables are constants, its registries are the World's (docs/
    // UPSTREAM_SYNC_GUIDE.md, "Plural guest"). scripts/check-guest-globals.sh
    // holds it to no mutable static at all, and
    // test/native/test_plural_engines.cpp to rendering beside another exactly
    // what it renders alone.
    /* instances               */ DSP_INSTANCES_ANY,
};

} // namespace

extern "C" {

const DspInfo* dsp_describe(void) { return &kInfo; }

// Never called: kInfo.survives declares nothing (above). Refused if it ever
// were, so clockwork does what it does for an instance that survives nothing.
int dsp_reconfigure(struct Dsp*, double, uint32_t, uint32_t) { return 1; }

// The fast memory scsynth will lay out by hand: its tables and its real-time
// pool, from -m. A config it will refuse makes no claim: dsp_new refuses it,
// with its line.
uint64_t dsp_memory_bytes(const void* guest_config, uint32_t guest_config_bytes) {
    ScsynthOptions so;
    char refused[256];
    if (scsynth_options_parse(static_cast<const char*>(guest_config), guest_config_bytes, &so,
                              refused, sizeof(refused)) != 0)
        return 0;
    return scsynth_memory_bytes(so.realTimeMemorySize);
}

// The bulk memory scsynth will allocate from as it runs, beyond the pool: the
// World's tables, synth definitions, the buffers /b_alloc makes
// (scsynth_options.h). The host gives at least this, or its own default when
// that is larger.
uint64_t dsp_memory_bulk_bytes(const void* guest_config, uint32_t guest_config_bytes) {
    ScsynthOptions so;
    char refused[256];
    if (scsynth_options_parse(static_cast<const char*>(guest_config), guest_config_bytes, &so,
                              refused, sizeof(refused)) != 0)
        return 0;
    return SCSYNTH_HEAP_HEADROOM_BYTES;
}

// ── The doors (guest_doors.h) ────────────────────────────────────────────────
//
// Everything the server wants from the host goes through one of these, so the
// server's own sources name no clockwork symbol: dsp_api.h is the whole of
// what the guest knows, and scripts/check-guest-boundary.sh holds it to that.
// Each takes the World and finds its instance; a World of no instance gets
// nothing.

World* supersonic_guest_world(struct Dsp* instance) {
    auto* d = reinterpret_cast<SupersonicGuest*>(instance);
    return d ? d->world : nullptr;
}

void* supersonic_guest_alloc_bytes_for(SupersonicGuest* d, size_t bytes) {
    if (!d || bytes == 0) return nullptr;
    return instance_alloc(d->host, bytes);
}

void supersonic_guest_free_bytes_for(SupersonicGuest* d, void* ptr) {
    if (!d || !ptr) return;
    if (assets_contains(d, ptr)) return;   // the client's range, not ours
    instance_free(d->host, ptr);
}

void* supersonic_guest_alloc_bytes(World* world, size_t bytes) {
    return supersonic_guest_alloc_bytes_for(guestOf(world), bytes);
}

void supersonic_guest_free_bytes(World* world, void* ptr) {
    supersonic_guest_free_bytes_for(guestOf(world), ptr);
}

const void* supersonic_guest_memory(World* world, uint32_t* bytes) {
    SupersonicGuest* d = guestOf(world);
    const void* memory = d ? d->config.memory : nullptr;
    if (bytes) *bytes = memory ? d->config.memory_bytes : 0;
    return memory;
}

void supersonic_guest_log(World* world, const char* text, uint32_t len) {
    if (SupersonicGuest* d = guestOf(world)) log_line(d->host, text, len);
}

void supersonic_flush_schedule(World* world) {
    SupersonicGuest* d = guestOf(world);
    if (d && d->host.flush_schedule) d->host.flush_schedule(d->host.ctx);
}

// The scope doors: ScopeBufferHnd is the ugens' shape of DspTapHandle.
static_assert(sizeof(ScopeBufferHnd) == sizeof(DspTapHandle),
              "ScopeBufferHnd and DspTapHandle must be the same four words");
static_assert(offsetof(ScopeBufferHnd, internalData) == offsetof(DspTapHandle, slot)
              && offsetof(ScopeBufferHnd, channels) == offsetof(DspTapHandle, channels),
              "ScopeBufferHnd and DspTapHandle must agree on slot and channels");

bool supersonic_scope_get(World* world, int index, int channels, int /*maxFrames*/, ScopeBufferHnd* hnd) {
    if (!hnd) return false;
    hnd->internalData = nullptr;
    hnd->data = nullptr;
    hnd->channels = 0;
    hnd->maxFrames = 0;
    SupersonicGuest* d = guestOf(world);
    if (!d || !d->host.tap_open || index < 0 || channels < 0) return false;
    return d->host.tap_open(d->host.ctx, (uint32_t)index, (uint32_t)channels,
                            reinterpret_cast<DspTapHandle*>(hnd)) != 0;
}

void supersonic_scope_push(World*, ScopeBufferHnd*, int) {}   // a stream publishes on write

void supersonic_scope_release(World* world, ScopeBufferHnd* hnd) {
    SupersonicGuest* d = guestOf(world);
    if (hnd && d && d->host.tap_close) d->host.tap_close(d->host.ctx, reinterpret_cast<DspTapHandle*>(hnd));
}

void supersonic_scope_write(World* world, ScopeBufferHnd* hnd, const float* const* channels,
                            uint32_t n_channels, uint32_t frames) {
    SupersonicGuest* d = guestOf(world);
    if (hnd && hnd->internalData && d && d->host.tap_write)
        d->host.tap_write(d->host.ctx, reinterpret_cast<DspTapHandle*>(hnd), channels, n_channels, frames);
}

void supersonic_scope_defence_tripped(World* world) {
    if (SupersonicGuest* d = guestOf(world)) ++d->scope_defence_trips;
}

uint32_t supersonic_scope_defence_trips(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d ? d->scope_defence_trips : 0;
}

/*
 * The session clock, and the time of the block being rendered: what the clock
 * UGens read. DspConfig::clock is the ClockworkClockState mirror clockwork
 * publishes — the same bytes JavaScript polls, and the same ones Link writes
 * its converged tempo into when it is running — and block_time is the OSC
 * timetag for the block, carried on every dsp_process. They exist so no UGen
 * has to know whether Link is present. The clock is the clock; who drives it
 * is clockwork's business.
 */
const ClockworkClockState* supersonic_scsynth_session_clock(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d ? d->config.clock : nullptr;
}

double supersonic_scsynth_block_ntp(World* world) {
    SupersonicGuest* d = guestOf(world);
    if (!d) return 0.0;
    // OSC timetag -> NTP seconds: whole seconds over a 32-bit fraction.
    const uint64_t t = d->block_time.ntp;
    return static_cast<double>(t >> 32) + static_cast<double>(t & 0xFFFFFFFFu) / 4294967296.0;
}

/*
 * The assets area, for /b_allocPtr. A client names a sample's position as an OFFSET
 * from the assets area base — dsp_api.h: offsets, not pointers, in any message that
 * names a position in a lane — and the guest adds the base it was given at
 * dsp_new. On the web the sum is the address the client already had; natively
 * the engine maps the lane wherever it likes, and only this sum is right.
 */
const uint8_t* supersonic_assets_base(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d ? static_cast<const uint8_t*>(d->config.assets) : nullptr;
}

uint32_t supersonic_assets_bytes(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d ? assets_bytes(d) : 0;
}

int supersonic_assets_contains(World* world, const void* ptr) {
    SupersonicGuest* d = guestOf(world);
    return d && assets_contains(d, ptr) ? 1 : 0;
}

uint8_t* supersonic_returns_base(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d ? static_cast<uint8_t*>(d->config.returns) : nullptr;
}

uint32_t supersonic_returns_bytes(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d && d->config.returns ? d->config.returns_bytes : 0;
}

// A buffer bound to an ASSET is letting go: its bytes are the client's, in
// the assets area, and the client reclaims them when clockwork tells it the asset
// is released (dsp_api.h, "Assets"). A buffer whose data came from /b_allocPtr
// (the web client's spelling) is not an asset clockwork knows; the release
// is then nothing, and that client hears /supersonic/buffer/freed as before.
void supersonic_guest_release_buffer(World* world, int bufnum) {
    SupersonicGuest* d = guestOf(world);
    if (bufnum < 0 || !d || !d->host.asset_release) return;
    d->host.asset_release(d->host.ctx, (uint32_t)bufnum);
}

const short* supersonic_piano_wavetable(World* world, size_t* frames) {
    SupersonicGuest* d = guestOf(world);
    if (frames) *frames = d ? d->piano_frames : 0;
    return d ? d->piano_table : nullptr;
}

short* supersonic_piano_wavetable_swap(World* world, short* fresh, size_t frames) {
    SupersonicGuest* d = guestOf(world);
    if (!d) return fresh;   // no instance to keep it: the caller frees what it made
    short* stale = d->piano_table;
    d->piano_table  = fresh;
    d->piano_frames = fresh ? frames : 0;
    return stale;
}

} // extern "C"

// The World's mirror while it is bound (node_tree.h).
NodeTreeMirror* supersonic_node_tree(World* world) {
    SupersonicGuest* d = guestOf(world);
    return d && d->tree.bound() ? &d->tree : nullptr;
}

extern "C" {

struct Dsp* dsp_new(const DspConfig* config, const DspHost* host, char* err, uint32_t err_bytes) {
    // Why it refused, into the host's buffer, cut to fit (dsp_api.h).
    const auto refuse = [err, err_bytes](const char* why) -> struct Dsp* {
        if (err && err_bytes) std::snprintf(err, err_bytes, "%s", why);
        return nullptr;
    };
    if (!config) return refuse("no DspConfig");
    // The instance itself is made in the guest's memory (guest_heap.h),
    // through the host's heap. Nothing of an instance is on the system
    // allocator, so a web reload — where the old engine is gone rather than
    // torn down — leaves nothing of it behind.
    const DspHost hostTable = host ? *host : DspHost{};
    void* mem = instance_alloc(hostTable, sizeof(SupersonicGuest));
    if (!mem) return refuse("out of memory: the guest's bulk memory cannot hold the instance");
    auto* d = new (mem) SupersonicGuest;
    d->config = *config;
    d->host = hostTable;

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
    options.mPreferredSampleRate  = sc_float_to<uint32_t>(config->sample_rate + 0.5);

    /*
     * The shape of the graph, READ FROM THE CONFIG BLOCK the host wrote.
     *
     * The block arrives as DspConfig::guest_config, opaque to clockwork, and
     * scsynth reads it as `name=value` lines against the one list of its
     * options (scsynth_options.h). A line it cannot take refuses the boot
     * with a message naming it: an option that looked applied and was not
     * is how every wrong-slot bug of the positional block survived.
     */
    ScsynthOptions so;
    char optionsErr[DSP_REFUSAL_BYTES];
    if (scsynth_options_parse(static_cast<const char*>(config->guest_config),
                              config->guest_config_bytes, &so,
                              optionsErr, sizeof(optionsErr)) != 0) {
        log_line(d->host, optionsErr, (uint32_t)std::strlen(optionsErr));
        release_instance(d);
        return refuse(optionsErr);
    }
    options.mNumBuffers            = so.numBuffers;
    options.mMaxNodes              = so.maxNodes;
    options.mMaxGraphDefs          = so.maxGraphDefs;
    options.mMaxWireBufs           = so.maxWireBufs;
    options.mNumAudioBusChannels   = so.numAudioBusChannels;
    // THE BUS HOLDS THE HOST'S CHANNELS. The first mNumOutputs + mNumInputs
    // channels of the audio bus are the host's in and out, and the host
    // chooses those ceilings (DspConfig: "size buffers against these in
    // dsp_new"), lanes for its ports and tracks included, which can be far
    // wider than the device. A bus narrower than that is written past its
    // end every block — found 2026-10-15 by clockwork's pool check: an
    // embedded-tight 64-channel bus under an 80-channel host, the audio bus
    // overrunning into the next block of the guest's heap. Upstream scsynth
    // refuses the option at its command line; a guest widens the bus and
    // says so, since the option was written against a device and the host
    // knows the lanes.
    {
        const uint32_t hostChannels = config->max_input_channels + config->max_output_channels;
        if (options.mNumAudioBusChannels < hostChannels) {
            char line[160];
            snprintf(line, sizeof(line), "numAudioBusChannels %u is narrower than the host's %u in + %u out: the bus is widened to %u",
                     options.mNumAudioBusChannels, config->max_input_channels, config->max_output_channels, hostChannels);
            log_line(d->host, line, (uint32_t)std::strlen(line));
            options.mNumAudioBusChannels = hostChannels;
        }
    }
    options.mNumControlBusChannels = so.numControlBusChannels;
    options.mRealTimeMemorySize    = so.realTimeMemorySize;
    options.mNumRGens              = so.numRGens;
    options.mLoadGraphDefs         = static_cast<int>(so.loadGraphDefs);
    options.mVerbosity             = static_cast<int>(so.verbosity);

    // A host that needs two renders of one score to agree pins the seeds.
    options.mDeterministicSeed     = so.randomSeed;

    d->osc_to_samples = config->sample_rate / 4294967296.0;

    char engineErr[DSP_REFUSAL_BYTES] = {0};
    d->world = EngineCore_New(&options, d, engineErr, sizeof(engineErr));
    if (!d->world) {
        release_instance(d);
        return refuse(engineErr[0] ? engineErr : "EngineCore_New failed");
    }

    // The node-tree mirror is the guest's, and so is laying it out.
    //
    // Clockwork provides the window and the mirror mechanism — a generic way
    // for a guest to publish state a client can poll without asking — and
    // zeroes the window at every dsp_new, knowing nothing of its shape. What
    // goes in it is the guest's business: scsynth publishes its node tree,
    // and a guest with no nodes publishes something else or nothing. The
    // indices come from this instance's heap, through the World, which is
    // why the World comes first; a heap that cannot hold them refuses the
    // boot rather than run with a mirror that cannot be written.
    try {
        NodeTree_Bind(&d->tree, d->world, config->shm_window, config->shm_window_bytes);
    } catch (const std::bad_alloc&) {
        World_Cleanup(d->world, true);
        release_instance(d);
        return refuse("out of memory: the guest's bulk memory cannot hold the node tree's indices");
    }

    // THE ROOT GROUP HAS TO BE PUBLISHED BY HAND. Every other node reaches
    // the mirror through Node_StateMsg, which the engine calls on kNode_Go /
    // kNode_End / kNode_Move. The root group is built directly inside World
    // creation and never goes through that path, so nothing else announces
    // it; without it getTree() has no root to hang the hierarchy off and
    // returns an empty tree rather than one missing its root.
    if (d->world->mTopGroup) NodeTree_Add(&d->tree, &d->world->mTopGroup->mNode);

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
                reply.mWorld = d->world;
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

    return reinterpret_cast<struct Dsp*>(d);
}

// An asset is a sample: interleaved float frames the client decoded into the
// assets, keyed by buffer number. Bind the buffer to them where they lie. A
// buffer that already holds something must be freed first — the audio thread
// cannot give heap memory back here, and the client's own /b_free is the
// honest way to ask.
int dsp_asset(struct Dsp* dsp, const ClockworkAsset* asset) {
    auto* d = reinterpret_cast<SupersonicGuest*>(dsp);
    if (!d || !d->world || !asset || asset->struct_bytes < sizeof(ClockworkAsset)) return 1;
    if (asset->kind != CLOCKWORK_ASSET_AUDIO_F32) return 2;   // a buffer holds audio; raw bytes mean nothing here
    World* w = d->world;
    const int bufnum = (int)asset->id;
    if (bufnum < 0 || bufnum >= (int)w->mNumSndBufs) return 3;
    SndBuf* nrt = World_GetNRTBuf(w, bufnum);
    if (nrt && nrt->data && !assets_contains(d, nrt->data)) return 4;  // occupied by heap data: /b_free it first
    if (asset->frames == 0 || asset->channels == 0 || !(asset->sample_rate > 0.f)) return 5;
    if (buffer_set_data(w, bufnum, (float*)asset->bytes, (int)asset->frames, (int)asset->channels,
                        (double)asset->sample_rate, false) != 0) return 6;
    return 0;
}

void dsp_free(struct Dsp* dsp) {
    auto* d = reinterpret_cast<SupersonicGuest*>(dsp);
    if (!d) return;
    // What the guest owes here (dsp_api.h): every ask still held, answered.
    // This World holds none across a block — every command stage runs inline
    // in the block it arrived in — and the asks that take longer than a
    // block (a definition's load, a sample's read) are the front's
    // (front/supersonic_commands.cpp), which hears clockwork's refusal of
    // what it had in flight and answers its asker in scsynth's words.
    if (d->world) {
        // The mirror and the piano table before the World: they are freed
        // through it. Nothing of this instance may be remembered past it
        // (guest_heap.h).
        NodeTree_Unbind(&d->tree, d->world);
        supersonic_guest_free_bytes(d->world, d->piano_table);
        d->piano_table = nullptr;
        d->piano_frames = 0;
        World_Cleanup(d->world, true);
        d->world = nullptr;
    }
    release_instance(d);
}

void dsp_process(struct Dsp* dsp,
                 const float* const* in, uint32_t n_in,
                 float* const* out, uint32_t n_out,
                 uint32_t frames,
                 ClockworkTimetag block_time) {
    auto* d = reinterpret_cast<SupersonicGuest*>(dsp);
    if (!d || !d->world) {
        for (uint32_t c = 0; c < n_out; ++c)
            if (out && out[c]) std::memset(out[c], 0, frames * sizeof(float));
        return;
    }
    World* w = d->world;
    d->block_time = block_time;
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
             ClockworkTimetag when, uint32_t origin, ClockworkTimetag block_time) {
    auto* d = reinterpret_cast<SupersonicGuest*>(dsp);
    if (!d || !d->world || !bytes || len == 0) return;

    ReplyAddress reply = {};
    reply.mWorld = d->world;
    reply.mReplyFunc = reply_to_host;
    reply.mReplyData = reinterpret_cast<void*>(static_cast<uintptr_t>(origin));

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

} // extern "C"
