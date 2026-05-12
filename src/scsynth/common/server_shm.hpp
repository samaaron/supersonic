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
#include "shm_scope_buffer.hpp"
#include "src/shared_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
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

static constexpr int    MAX_SHM_SCOPE_BUFFERS = 128;
// 8 MB base for scope/control-bus/metrics/node-tree/TLSF, plus the
// audio-buffer region (which scales with SUPERSONIC_SHM_AUDIO_SECONDS).
static constexpr size_t SEGMENT_SIZE      =
    8192 * 1024 + SHM_AUDIO_TOTAL_SIZE;

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
    h.fd = ::shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
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

// ──── Fixed-layout shared memory header ─────────────────────────────────
//
// Segment layout:
//
//   shm_segment_header                          (16 bytes, 16-aligned)
//   shm_scope_buffer[MAX_SHM_SCOPE_BUFFERS]            (128 scope slots)
//   float[control_bus_count]                   (control bus values)
//   PerformanceMetrics                         (engine perf metrics — observable)
//   NodeTreeHeader + NodeEntry[NODE_TREE_MIRROR_MAX_NODES]  (node tree mirror)
//   shm_audio_buffer[MAX_SHM_AUDIO_BUFFERS]    (PCM tap ring slots:
//                                               master capture + AudioOut2
//                                               UGens; fixed inline data
//                                               array per slot)
//   char[remaining]                            (TLSF pool for scope buffer
//                                               dynamic allocations)
//
// MAGIC history:
//   0x5C09E001  initial (scope + control busses only)
//   0x5C09E002  added metrics + node tree mirror
//   0x5C09E003  added shm_audio_buffer multi-slot ring
//
// Publication: writer stores MAGIC last with a release fence preceding
// it; the reader places an acquire fence after observing MAGIC. Once
// MAGIC is visible the whole header is, regardless of the OSC ready
// handshake.

struct shm_segment_header {
    static constexpr uint32_t MAGIC = 0x5C09E003;

    uint32_t magic;
    uint32_t num_shm_scope_buffers;
    uint32_t control_bus_count;
    uint32_t num_audio_buffers;
};

// shm_audio_buffer types come from shm_audio_buffer.hpp. The same
// struct shape is laid out here on native and in ring_buffer_storage
// (the SAB) on web.

// ──── server_shared_memory ──────────────────────────────────────────────
//
// Process-local view of the segment.  Each side constructs its own
// instance from the mapped pointer — this object is NOT in shared memory.

class server_shared_memory {
public:
    server_shared_memory(void* segment_base, int control_busses, bool init) {
        char* base = static_cast<char*>(segment_base);

        header_ = reinterpret_cast<shm_segment_header*>(base);

        // Scope buffers after header (16-aligned)
        size_t off = (sizeof(shm_segment_header) + 15) & ~size_t(15);
        scope_buffers_ = reinterpret_cast<shm_scope_buffer*>(base + off);

        // Control busses after scope buffers (16-aligned)
        off += MAX_SHM_SCOPE_BUFFERS * sizeof(shm_scope_buffer);
        off = (off + 15) & ~size_t(15);
        control_busses_ = reinterpret_cast<float*>(base + off);

        // PerformanceMetrics after control busses (16-aligned). Observable
        // by other processes — Sonic Pi reads this without OSC roundtrips.
        off += static_cast<size_t>(control_busses) * sizeof(float);
        off = (off + 15) & ~size_t(15);
        metrics_ = reinterpret_cast<PerformanceMetrics*>(base + off);

        // Node tree mirror after metrics (16-aligned, then 8-aligned for entries)
        off += sizeof(PerformanceMetrics);
        off = (off + 15) & ~size_t(15);
        node_tree_header_ = reinterpret_cast<NodeTreeHeader*>(base + off);
        off += NODE_TREE_HEADER_SIZE;
        node_tree_entries_ = reinterpret_cast<NodeEntry*>(base + off);
        off += static_cast<size_t>(NODE_TREE_MIRROR_MAX_NODES) * NODE_TREE_ENTRY_SIZE;

        // Audio tap slot array (16-aligned). Each slot is fully self-
        // contained: header + inline data ring, no TLSF involvement.
        off = (off + 15) & ~size_t(15);
        audio_buffers_ = reinterpret_cast<shm_audio_buffer*>(base + off);
        off += MAX_SHM_AUDIO_BUFFERS * sizeof(shm_audio_buffer);

        // TLSF pool after the audio buffer slots (16-aligned). Pool
        // size shrinks by the metrics + node tree + audio buffer
        // footprint but stays ample within the 8MB segment.
        off = (off + 15) & ~size_t(15);
        pool_base_ = base + off;
        pool_size_ = SEGMENT_SIZE - off;

        if (init) {
            header_->num_shm_scope_buffers = MAX_SHM_SCOPE_BUFFERS;
            header_->control_bus_count = static_cast<uint32_t>(control_busses);
            header_->num_audio_buffers = MAX_SHM_AUDIO_BUFFERS;

            memset(control_busses_, 0,
                   static_cast<size_t>(control_busses) * sizeof(float));

            for (int i = 0; i < MAX_SHM_SCOPE_BUFFERS; ++i)
                new (&scope_buffers_[i]) shm_scope_buffer();

            // Zero the metrics struct (all atomics start at 0).
            // PerformanceMetrics has no constructor — placement-new isn't
            // necessary. atomic<uint32_t> has trivial init from zero memory
            // per [atomics.types.generic]/4 (init via default constructor
            // produces an unspecified value; zero memory is well-defined).
            memset(metrics_, 0, sizeof(PerformanceMetrics));

            // Zero the node tree mirror — empty header + empty-slot entries.
            memset(node_tree_header_, 0, NODE_TREE_HEADER_SIZE);
            memset(node_tree_entries_, 0xFF,
                   static_cast<size_t>(NODE_TREE_MIRROR_MAX_NODES) * NODE_TREE_ENTRY_SIZE);

            // Audio buffer slot headers all start disabled with no data.
            // Same atomic-zero-init reasoning as PerformanceMetrics; the
            // void* cast silences -Wnontrivial-memcall.
            memset(static_cast<void*>(audio_buffers_), 0,
                   MAX_SHM_AUDIO_BUFFERS * sizeof(shm_audio_buffer));

            // MAGIC is the publication store: it must be the last write
            // into the header, fenced behind a release so a reader that
            // sees MAGIC sees every preceding field too. An aligned u32
            // store is atomic on every supported platform.
            std::atomic_thread_fence(std::memory_order_release);
            header_->magic = shm_segment_header::MAGIC;
        }
    }

    float* get_control_busses() { return control_busses_; }
    PerformanceMetrics* get_metrics() { return metrics_; }
    NodeTreeHeader*     get_node_tree_header()  { return node_tree_header_; }
    NodeEntry*          get_node_tree_entries() { return node_tree_entries_; }

    // Pointer to the cross-process slot array. Used by the audio thread
    // (via the global g_shm_audio_buffers) so writes land in the segment
    // visible to external readers, not in the in-band ring_buffer_storage.
    shm_audio_buffer* get_audio_buffers() { return audio_buffers_; }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        if (index < MAX_SHM_AUDIO_BUFFERS)
            return &audio_buffers_[index];
        return nullptr;
    }

    shm_scope_buffer* get_scope_buffer(unsigned int index) {
        if (index < MAX_SHM_SCOPE_BUFFERS) {
            if (!scope_buffers_)
                return nullptr;
            return &scope_buffers_[index];
        }
        return nullptr;
    }

    void* pool_base() const { return pool_base_; }
    size_t pool_size() const { return pool_size_; }

private:
    shm_segment_header*   header_;
    shm_scope_buffer*       scope_buffers_;
    float*              control_busses_;
    PerformanceMetrics* metrics_           = nullptr;
    NodeTreeHeader*     node_tree_header_  = nullptr;
    NodeEntry*          node_tree_entries_ = nullptr;
    shm_audio_buffer*   audio_buffers_ = nullptr;
    void*               pool_base_;
    size_t              pool_size_;
};

// ──── Creator (audio engine side) ───────────────────────────────────────

class server_shared_memory_creator {
public:
    server_shared_memory_creator(unsigned int port_number, unsigned int control_busses):
        shmem_name(make_shmem_name(port_number)),
        handle(shm_create(shmem_name, SEGMENT_SIZE))
    {
        memset(handle.ptr, 0, SEGMENT_SIZE);

        shm = new server_shared_memory(handle.ptr, control_busses, true);

        scope_pool.init(shm->pool_base(), shm->pool_size());
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

    float* get_control_busses() { return shm->get_control_busses(); }
    PerformanceMetrics* get_metrics() { return shm ? shm->get_metrics() : nullptr; }
    NodeTreeHeader*     get_node_tree_header()  { return shm ? shm->get_node_tree_header()  : nullptr; }
    NodeEntry*          get_node_tree_entries() { return shm ? shm->get_node_tree_entries() : nullptr; }
    shm_audio_buffer*   get_audio_buffers()     { return shm ? shm->get_audio_buffers()     : nullptr; }

    shm_scope_buffer_writer get_scope_buffer_writer(
            unsigned int index, unsigned int channels, unsigned int size) {
        if (!shm)
            return shm_scope_buffer_writer();
        shm_scope_buffer* buf = shm->get_scope_buffer(index);
        if (buf)
            return shm_scope_buffer_writer(buf, scope_pool, channels, size);
        else
            return shm_scope_buffer_writer();
    }

    void release_scope_buffer_writer(shm_scope_buffer_writer& writer) {
        writer.release(scope_pool);
    }

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
    shm_scope_buffer_pool     scope_pool;
};


// ──── Client (GUI / reader side) ────────────────────────────────────────

class server_shared_memory_client {
public:
    server_shared_memory_client(unsigned int port_number):
        shmem_name(make_shmem_name(port_number)),
        handle(shm_open_existing(shmem_name))
    {
        auto* header = static_cast<shm_segment_header*>(handle.ptr);
        if (header->magic != shm_segment_header::MAGIC)
            throw std::runtime_error(
                "Invalid shared memory magic — is the audio engine running?");

        // Acquire pairs with the writer's release before the MAGIC
        // store. control_bus_count is read just below — without the
        // fence the reader could observe MAGIC while still seeing a
        // zeroed control_bus_count.
        std::atomic_thread_fence(std::memory_order_acquire);

        shm = new server_shared_memory(
            handle.ptr,
            static_cast<int>(header->control_bus_count),
            false);
    }

    ~server_shared_memory_client() {
        shm_close(handle);
        delete shm;
    }

    float* get_control_busses() { return shm->get_control_busses(); }
    PerformanceMetrics* get_metrics() { return shm->get_metrics(); }
    NodeTreeHeader*     get_node_tree_header()  { return shm->get_node_tree_header();  }
    NodeEntry*          get_node_tree_entries() { return shm->get_node_tree_entries(); }

    shm_scope_buffer_reader get_scope_buffer_reader(unsigned int index) {
        shm_scope_buffer* buf = shm->get_scope_buffer(index);
        return shm_scope_buffer_reader(buf);
    }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm->get_audio_buffer(index);
    }

    shm_audio_buffer_reader get_audio_buffer_reader(unsigned int index) {
        return shm_audio_buffer_reader(shm->get_audio_buffer(index));
    }

private:
    string                shmem_name;
    shm_handle            handle;
    server_shared_memory* shm = nullptr;
};

} /* namespace detail_server_shm */

using detail_server_shm::shm_scope_buffer;
using detail_server_shm::shm_scope_buffer_reader;
using detail_server_shm::shm_scope_buffer_writer;
using detail_server_shm::server_shared_memory_client;
using detail_server_shm::server_shared_memory_creator;
// shm_audio_buffer + AUDIO_* names are exported by shm_audio_buffer.hpp.
