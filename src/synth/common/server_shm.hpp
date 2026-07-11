//  Shared memory IPC interface to SuperSonic.
//
//  A fixed-layout segment any process can mmap by name. POSIX shm_open/mmap
//  on Linux/macOS; Win32 named file mappings on Windows. Interface design
//  inspired by SuperCollider's server_shared_memory by Tim Blechmann and
//  Jakob Leben (2011); this is a clean-room re-implementation.
//
//  Copyright (C) 2026 SuperSonic contributors.
//  Dual-licensed under MIT and GPLv3-or-later, at the user's option.

#pragma once

#include "shm_audio_buffer.hpp"
#include "src/shared_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace detail_server_shm {

using std::string;

static constexpr int    MAX_SHM_SCOPE_BUFFERS = 128;  // retained for compat; scope is fixed-inline (SHM_SCOPE_MAX_SCOPES)

// The public segment is a small handshake header followed by the unified
// shared_memory.h arena blob — the SAME layout the engine uses in
// ring_buffer_storage (native) / the WASM SAB. One layout, one source of
// truth; no separate native layout to drift.
static constexpr size_t SHM_BLOB_OFFSET = 128;  // aligned, >= sizeof(shm_segment_header)
static constexpr size_t SEGMENT_SIZE    = SHM_BLOB_OFFSET + TOTAL_BUFFER_SIZE;

static inline string make_shmem_name(unsigned int port_number) {
    return string("SuperSonic_") + std::to_string(port_number);
}

// ──── Platform shared memory primitives ─────────────────────────────────

struct shm_handle {
    void*  ptr  = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#else
    int    fd  = -1;
#endif
};

inline shm_handle shm_create(const string& name, size_t size) {
    shm_handle h;
    h.size = size;
#ifdef _WIN32
    std::wstring wname(name.begin(), name.end());
    h.mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(size), wname.c_str());
    if (!h.mapping)
        throw std::runtime_error("CreateFileMapping failed for " + name);
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!h.ptr) {
        CloseHandle(h.mapping);
        throw std::runtime_error("MapViewOfFile failed for " + name);
    }
#else
    string posix_name = "/" + name;
    // 0600 + O_EXCL: the segment carries ring cursors the engine dereferences,
    // so only the owning user may map it, and a pre-existing segment (stale or
    // planted by another local user under the predictable name) is never
    // silently adopted — creation fails instead. Callers remove their own
    // leftovers first via shm_remove()/cleanup().
    h.fd = ::shm_open(posix_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (h.fd < 0)
        throw std::runtime_error("shm_open(create) failed for " + name);
    if (ftruncate(h.fd, static_cast<off_t>(size)) < 0) {
        ::close(h.fd);
        ::shm_unlink(posix_name.c_str());
        throw std::runtime_error("ftruncate failed for " + name);
    }
    h.ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        ::shm_unlink(posix_name.c_str());
        throw std::runtime_error("mmap failed for " + name);
    }
#endif
    return h;
}

inline shm_handle shm_open_existing(const string& name) {
    shm_handle h;
#ifdef _WIN32
    std::wstring wname(name.begin(), name.end());
    h.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (!h.mapping)
        throw std::runtime_error("OpenFileMapping failed for " + name);
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!h.ptr) {
        CloseHandle(h.mapping);
        throw std::runtime_error("MapViewOfFile failed for " + name);
    }
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(h.ptr, &info, sizeof(info));
    h.size = info.RegionSize;
#else
    string posix_name = "/" + name;
    h.fd = ::shm_open(posix_name.c_str(), O_RDWR, 0);
    if (h.fd < 0)
        throw std::runtime_error("shm_open(open) failed for " + name);
    struct stat st;
    fstat(h.fd, &st);
    h.size = static_cast<size_t>(st.st_size);
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        throw std::runtime_error("mmap failed for " + name);
    }
#endif
    return h;
}

inline void shm_close(shm_handle& h) {
#ifdef _WIN32
    if (h.ptr)     UnmapViewOfFile(h.ptr);
    if (h.mapping) CloseHandle(h.mapping);
#else
    if (h.ptr && h.ptr != MAP_FAILED) ::munmap(h.ptr, h.size);
    if (h.fd >= 0)                    ::close(h.fd);
#endif
    h.ptr = nullptr;
}

inline void shm_remove(const string& name) {
#ifdef _WIN32
    (void)name;  // Windows named mappings are reference-counted
#else
    ::shm_unlink(("/" + name).c_str());
#endif
}

// ──── Segment handshake header ──────────────────────────────────────────
//
// Segment layout (unified, MAGIC 0x5C09E004):
//
//   shm_segment_header        (16 B handshake: MAGIC + sanity sizes)
//   <pad to SHM_BLOB_OFFSET>
//   shared_memory.h arena blob (TOTAL_BUFFER_SIZE) — the *same* layout the
//   engine uses in ring_buffer_storage / the WASM SAB. The engine points its
//   `shared_memory` base at this blob, so rings, control pointers, metrics,
//   node-tree, audio taps and (fixed-inline) scope are all addressed by their
//   shared_memory.h offsets and observable cross-process for free.
//
// MAGIC history:
//   0x5C09E001  initial (scope + control busses only)
//   0x5C09E002  added metrics + node tree mirror
//   0x5C09E003  added shm_audio_buffer multi-slot ring
//   0x5C09E004  unified: segment == shared_memory.h arena blob (rings in
//               segment; scope fixed-inline; TLSF pool + control busses removed)
//
// Publication: the creator zeroes the whole segment and writes the header
// geometry, but defers the MAGIC store. The engine then populates the arena
// (init_memory(): node-tree empty-slot markers = 0xFF, metrics, scope headers,
// …) and only afterwards calls publish(), which stores MAGIC last behind a
// release fence. So a reader that observes MAGIC (after an acquire fence) sees
// both the geometry AND a fully-populated arena — never a half-zeroed one where
// e.g. node entries read id == 0 (aliasing the real root group) instead of the
// id == -1 empty marker. MAGIC visible ⇒ segment ready.

// Self-describing handshake: the creator publishes every region's offset and
// geometry, so a reader (e.g. Sonic Pi) needs no compile-time knowledge of the
// arena layout — it locates metrics, rings, node-tree, audio taps and scope
// purely from these fields. This makes the consumer drift-proof: layout/size
// changes propagate through the header rather than requiring a hand-synced copy.
// All offsets are relative to the arena blob base (segment + blob_offset).
struct shm_segment_header {
    static constexpr uint32_t MAGIC = 0x5C09E006;  // E006: + Link metrics fields (METRICS_SIZE 184→232)

    uint32_t magic;
    uint32_t blob_offset;          // segment base → arena blob
    uint32_t blob_size;            // == TOTAL_BUFFER_SIZE

    uint32_t in_ring_offset;       // OSC host→engine ring
    uint32_t in_ring_size;
    uint32_t out_ring_offset;      // OSC engine→host (replies) ring
    uint32_t out_ring_size;
    uint32_t nrt_out_ring_offset;  // NRT-thread egress ring (replies, notifications, debug)
    uint32_t nrt_out_ring_size;
    uint32_t control_offset;       // ControlPointers (ring head/tail/seq + cursors)

    uint32_t metrics_offset;       // PerformanceMetrics
    uint32_t metrics_field_count;  // contiguous u32 fields

    uint32_t node_tree_offset;     // NodeTreeHeader, then NodeEntry[]
    uint32_t node_tree_header_bytes;
    uint32_t node_tree_entry_bytes;
    uint32_t node_tree_max_nodes;

    uint32_t audio_offset;         // shm_audio_buffer[0]
    uint32_t audio_slot_count;
    uint32_t audio_slot_bytes;     // sizeof(shm_audio_buffer)

    uint32_t scope_offset;         // scope global header, then slots
    uint32_t scope_max;            // SHM_SCOPE_MAX_SCOPES
    uint32_t scope_header_bytes;   // SHM_SCOPE_HEADER_SIZE (global header)
    uint32_t scope_slot_bytes;     // SHM_SCOPE_SLOT_SIZE
    uint32_t scope_slot_header;    // SHM_SCOPE_SLOT_HEADER_SIZE
    uint32_t scope_frames;         // SHM_SCOPE_FRAMES_PER_SCOPE
    uint32_t scope_channels;       // SHM_SCOPE_CHANNELS

    uint32_t native_stats_offset;  // native-only live stats (synthdefs, buffers, buffer_bytes)
};
static_assert(sizeof(shm_segment_header) <= SHM_BLOB_OFFSET,
              "shm_segment_header must fit within SHM_BLOB_OFFSET");

// ──── server_shared_memory ──────────────────────────────────────────────
//
// Process-local view: holds the arena blob base and exposes the observable
// regions by their shared_memory.h offsets. NOT itself in shared memory.

class server_shared_memory {
public:
    server_shared_memory(void* segment_base, bool init) {
        char* seg = static_cast<char*>(segment_base);
        header_ = reinterpret_cast<shm_segment_header*>(seg);
        blob_   = reinterpret_cast<uint8_t*>(seg) + SHM_BLOB_OFFSET;

        if (init) {
            header_->blob_offset = static_cast<uint32_t>(SHM_BLOB_OFFSET);
            header_->blob_size   = static_cast<uint32_t>(TOTAL_BUFFER_SIZE);

            header_->in_ring_offset    = IN_BUFFER_START;
            header_->in_ring_size      = IN_BUFFER_SIZE;
            header_->out_ring_offset   = OUT_BUFFER_START;
            header_->out_ring_size     = OUT_BUFFER_SIZE;
            header_->nrt_out_ring_offset = NRT_OUT_BUFFER_START;
            header_->nrt_out_ring_size   = NRT_OUT_BUFFER_SIZE;
            header_->control_offset    = CONTROL_START;

            header_->metrics_offset      = METRICS_START;
            header_->metrics_field_count = METRICS_SIZE / 4;

            header_->node_tree_offset       = NODE_TREE_START;
            header_->node_tree_header_bytes = NODE_TREE_HEADER_SIZE;
            header_->node_tree_entry_bytes  = NODE_TREE_ENTRY_SIZE;
            header_->node_tree_max_nodes    = NODE_TREE_MIRROR_MAX_NODES;

            header_->audio_offset     = SHM_AUDIO_START;
            header_->audio_slot_count = MAX_SHM_AUDIO_BUFFERS;
            header_->audio_slot_bytes = static_cast<uint32_t>(sizeof(shm_audio_buffer));

            header_->scope_offset       = SHM_SCOPE_START;
            header_->scope_max          = SHM_SCOPE_MAX_SCOPES;
            header_->scope_header_bytes = SHM_SCOPE_HEADER_SIZE;
            header_->scope_slot_bytes   = SHM_SCOPE_SLOT_SIZE;
            header_->scope_slot_header  = SHM_SCOPE_SLOT_HEADER_SIZE;
            header_->scope_frames       = SHM_SCOPE_FRAMES_PER_SCOPE;
            header_->scope_channels     = SHM_SCOPE_CHANNELS;

            header_->native_stats_offset = NATIVE_STATS_START;

            // NOTE: MAGIC is deliberately NOT stored here. The geometry fields
            // above are written now (before init_memory()), but the arena blob
            // is still zeroed — and zero is NOT a safe default for every region
            // (the node-tree empty-slot marker is id == -1, i.e. 0xFF, not 0).
            // Publishing MAGIC now would expose a window where a reader observes
            // MAGIC but sees an unpopulated arena (e.g. node entries with id == 0
            // aliasing the real root group). The creator publishes MAGIC via
            // publish() only after the engine's init_memory() has populated the
            // arena, so "MAGIC visible ⇒ fully-populated segment" holds.
        }
    }

    // Publish the segment: release-fence, then store MAGIC last. Called once the
    // arena has been populated (after init_memory()). A reader that observes
    // MAGIC behind an acquire fence then sees both the header geometry and the
    // live arena structures — no publish-before-populate window.
    void publish() {
        std::atomic_thread_fence(std::memory_order_release);
        header_->magic = shm_segment_header::MAGIC;
    }

    // Arena blob base. The engine points `shared_memory` here; observers
    // address every region by its shared_memory.h offset from this pointer.
    uint8_t* get_base() const { return blob_; }

    PerformanceMetrics* get_metrics() {
        return reinterpret_cast<PerformanceMetrics*>(blob_ + METRICS_START);
    }
    NodeTreeHeader* get_node_tree_header() {
        return reinterpret_cast<NodeTreeHeader*>(blob_ + NODE_TREE_START);
    }
    NodeEntry* get_node_tree_entries() {
        return reinterpret_cast<NodeEntry*>(blob_ + NODE_TREE_START + NODE_TREE_HEADER_SIZE);
    }
    shm_audio_buffer* get_audio_buffers() {
        return reinterpret_cast<shm_audio_buffer*>(blob_ + SHM_AUDIO_START);
    }
    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        if (index < MAX_SHM_AUDIO_BUFFERS)
            return get_audio_buffers() + index;
        return nullptr;
    }

    // Base of a fixed-inline scope slot, or nullptr if out of range.
    uint8_t* get_scope_slot(unsigned int index) {
        if (index >= SHM_SCOPE_MAX_SCOPES)
            return nullptr;
        return blob_ + SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE
             + static_cast<size_t>(index) * SHM_SCOPE_SLOT_SIZE;
    }

private:
    shm_segment_header* header_;
    uint8_t*            blob_;
};

// ──── Fixed-inline scope reader ─────────────────────────────────────────
//
// Reads the triple-buffered scope slot written by SC_World.cpp's unified
// fixed-inline scope path (offsets only, no relative_ptr). Best-effort: it
// tracks the last-published region and reports new data when `stage` advances.

class shm_scope_buffer_reader {
public:
    // keepalive pins the mapping the slot pointer reaches into (see the
    // client's get_scope_buffer_reader): reader copies handed to long-lived
    // observers stay safe even after the client that minted them is destroyed.
    shm_scope_buffer_reader(uint8_t* slot = nullptr,
                            std::shared_ptr<const void> keepalive = nullptr)
        : slot_(slot), keepalive_(std::move(keepalive)) {}

    bool valid() {
        if (!slot_) return false;
        auto* state = reinterpret_cast<std::atomic<uint32_t>*>(slot_ + 0);
        return state->load(std::memory_order_acquire) == 1;
    }

    unsigned int channels() {
        if (!slot_) return 0;
        return *reinterpret_cast<uint32_t*>(slot_ + 4);
    }

    unsigned int max_frames() { return SHM_SCOPE_FRAMES_PER_SCOPE; }

    bool pull(unsigned int& frames) {
        if (!valid()) return false;
        auto* stage = reinterpret_cast<std::atomic<int32_t>*>(slot_ + 8);
        int s = stage->load(std::memory_order_acquire);
        if (s == last_stage_)
            return false;            // nothing newly published since last pull
        last_stage_ = s;
        frames = SHM_SCOPE_FRAMES_PER_SCOPE;
        return true;
    }

    float* data() {
        if (!slot_) return nullptr;
        auto* stage = reinterpret_cast<std::atomic<int32_t>*>(slot_ + 8);
        int s = stage->load(std::memory_order_acquire);
        // Untrusted runtime value re-read from the segment on every call:
        // only 0..2 keeps the pointer inside the triple buffer.
        if (s < 0 || s > 2) return nullptr;
        float* base = reinterpret_cast<float*>(slot_ + SHM_SCOPE_SLOT_HEADER_SIZE);
        return base + static_cast<size_t>(s)
             * (SHM_SCOPE_FRAMES_PER_SCOPE * SHM_SCOPE_CHANNELS);
    }

private:
    uint8_t* slot_;
    std::shared_ptr<const void> keepalive_;
    int      last_stage_ = -1;
};

// ──── Observer views (GUI / passive reader side) ────────────────────────
//
// Byte-level views onto observable regions, for consumers that render them
// generically (e.g. the Sonic Pi GUI panels). The observer tails rings with
// its own local cursor — head/tail here are the engine's, for reference.

struct ring_view {
    uint8_t* base = nullptr;
    uint32_t size = 0;
    std::atomic<int32_t>* head = nullptr;
    std::atomic<int32_t>* tail = nullptr;
};

struct node_tree_view {
    uint8_t* header      = nullptr;  // NodeTreeHeader
    uint8_t* entries     = nullptr;  // NodeEntry[]
    uint32_t max_nodes   = 0;
    uint32_t entry_bytes = 0;
};

// Native-only live engine stats snapshot.
struct native_stats {
    uint32_t synthdefs           = 0;
    uint32_t buffers             = 0;
    uint32_t buffer_bytes        = 0;
    uint32_t cpu_load_avg_centi  = 0;  // DSP load, percent * 100 (smoothed average)
    uint32_t cpu_load_peak_centi = 0;  // DSP load, percent * 100 (decaying peak)
    uint32_t callback_overruns   = 0;  // audio callbacks that overran their budget
};

// ──── Creator (audio engine side) ───────────────────────────────────────

class server_shared_memory_creator {
public:
    // control_busses is accepted for call-site compatibility but unused: the
    // unified arena is fixed-size and control busses are process-local (heap),
    // so they don't live in the segment.
    server_shared_memory_creator(unsigned int port_number, unsigned int /*control_busses*/):
        shmem_name(make_shmem_name(port_number)),
        handle(shm_create(shmem_name, SEGMENT_SIZE))
    {
        memset(handle.ptr, 0, SEGMENT_SIZE);
        shm = new server_shared_memory(handle.ptr, true);
    }

    static void cleanup(unsigned int port_number) {
        shm_remove(make_shmem_name(port_number));
    }

    ~server_shared_memory_creator() {
        if (shm)
            disconnect();
    }

    void disconnect() {
        shm_remove(shmem_name);
        shm_close(handle);
        delete shm;
        shm = nullptr;
    }

    // Arena blob base — the engine points `shared_memory` here.
    uint8_t* get_base() { return shm ? shm->get_base() : nullptr; }

    // Store MAGIC, making the segment visible to readers. Call once the engine
    // has populated the arena (after init_memory()) so observers never see a
    // published-but-unpopulated segment.
    void publish() { if (shm) shm->publish(); }

    PerformanceMetrics* get_metrics() { return shm ? shm->get_metrics() : nullptr; }
    NodeTreeHeader*     get_node_tree_header()  { return shm ? shm->get_node_tree_header()  : nullptr; }
    NodeEntry*          get_node_tree_entries() { return shm ? shm->get_node_tree_entries() : nullptr; }
    shm_audio_buffer*   get_audio_buffers()     { return shm ? shm->get_audio_buffers()     : nullptr; }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm ? shm->get_audio_buffer(index) : nullptr;
    }

    shm_audio_buffer_writer get_audio_buffer_writer(unsigned int index) {
        return shm_audio_buffer_writer(get_audio_buffer(index));
    }

private:
    string                shmem_name;
    shm_handle            handle;
    server_shared_memory* shm = nullptr;
};


// ──── Client (GUI / reader side) ────────────────────────────────────────

class server_shared_memory_client {
public:
    server_shared_memory_client(unsigned int port_number):
        shmem_name(make_shmem_name(port_number))
    {
        shm_handle handle = shm_open_existing(shmem_name);
        // The mapping is owned by a shared_ptr so readers minted from this
        // client can pin it beyond the client's own lifetime (a GUI destroys
        // and re-opens its client on engine cold swap while reader copies
        // live on in widgets). Until map_ owns it, close on any throw — the
        // destructor doesn't run for a throwing constructor.
        try {
            map_ = std::make_shared<mapping>(handle);
        } catch (...) {
            shm_close(handle);
            throw;
        }

        // Size check before touching any field: a foreign or truncated segment
        // must not be dereferenced via header offsets.
        if (handle.size < SEGMENT_SIZE)
            throw std::runtime_error(
                "Shared memory segment smaller than expected — stale or foreign segment?");

        auto* header = static_cast<shm_segment_header*>(handle.ptr);
        if (header->magic != shm_segment_header::MAGIC)
            throw std::runtime_error(
                "Invalid shared memory magic — is the audio engine running?");

        // Acquire pairs with the creator's release before the MAGIC store, so
        // observing MAGIC implies a fully-published header.
        std::atomic_thread_fence(std::memory_order_acquire);

        shm.reset(new server_shared_memory(handle.ptr, false));
    }

    uint8_t*            get_base() { return shm->get_base(); }
    PerformanceMetrics* get_metrics() { return shm->get_metrics(); }
    NodeTreeHeader*     get_node_tree_header()  { return shm->get_node_tree_header();  }
    NodeEntry*          get_node_tree_entries() { return shm->get_node_tree_entries(); }

    // Flat atomic u32 view of PerformanceMetrics, for observers that render
    // the fields generically.
    const std::atomic<uint32_t>* get_metrics_flat() {
        return reinterpret_cast<const std::atomic<uint32_t>*>(
            shm->get_base() + METRICS_START);
    }
    static constexpr uint32_t metrics_field_count() { return METRICS_SIZE / 4; }

    ring_view get_in_ring() {
        return { shm->get_base() + IN_BUFFER_START, IN_BUFFER_SIZE,
                 &control()->in_head, &control()->in_tail };
    }
    ring_view get_out_ring() {
        return { shm->get_base() + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                 &control()->out_head, &control()->out_tail };
    }
    ring_view get_nrt_out_ring() {
        return { shm->get_base() + NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE,
                 &control()->nrt_out_head, &control()->nrt_out_tail };
    }

    node_tree_view get_node_tree() {
        return { shm->get_base() + NODE_TREE_START,
                 shm->get_base() + NODE_TREE_START + NODE_TREE_HEADER_SIZE,
                 NODE_TREE_MIRROR_MAX_NODES, NODE_TREE_ENTRY_SIZE };
    }

    native_stats get_native_stats() {
        auto field = [this](uint32_t off) {
            return reinterpret_cast<const std::atomic<uint32_t>*>(
                shm->get_base() + NATIVE_STATS_START + off)
                ->load(std::memory_order_relaxed);
        };
        return { field(NATIVE_STAT_SYNTHDEFS),     field(NATIVE_STAT_BUFFERS),
                 field(NATIVE_STAT_BUFFER_BYTES),  field(NATIVE_STAT_CPU_AVG_CENTI),
                 field(NATIVE_STAT_CPU_PEAK_CENTI), field(NATIVE_STAT_CB_OVERRUNS) };
    }
    // Native segments always carry the stats region; a web-origin arena has no
    // shm client at all. Kept for observer-API symmetry.
    bool has_native_stats() const { return true; }

    shm_scope_buffer_reader get_scope_buffer_reader(unsigned int index) {
        return shm_scope_buffer_reader(shm->get_scope_slot(index), map_);
    }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm->get_audio_buffer(index);
    }

    shm_audio_buffer_reader get_audio_buffer_reader(unsigned int index) {
        return shm_audio_buffer_reader(shm->get_audio_buffer(index), map_);
    }

private:
    struct mapping {
        explicit mapping(const shm_handle& h): handle(h) {}
        mapping(const mapping&) = delete;
        mapping& operator=(const mapping&) = delete;
        ~mapping() { shm_close(handle); }
        shm_handle handle;
    };

    ControlPointers* control() {
        return reinterpret_cast<ControlPointers*>(shm->get_base() + CONTROL_START);
    }

    string                                shmem_name;
    std::shared_ptr<mapping>              map_;
    std::unique_ptr<server_shared_memory> shm;
};

} /* namespace detail_server_shm */

using detail_server_shm::shm_scope_buffer_reader;
using detail_server_shm::server_shared_memory_client;
using detail_server_shm::server_shared_memory_creator;
using detail_server_shm::ring_view;
using detail_server_shm::node_tree_view;
using detail_server_shm::native_stats;
// shm_audio_buffer + AUDIO_* names are exported by shm_audio_buffer.hpp.
