// SPDX-License-Identifier: GPL-3.0-or-later
// guest_heap.h — making and unmaking the server's objects in the guest's
// own memory.
//
// Everything an instance of the server owns lives in the memory clockwork
// gives the guest (dsp_api.h: the fast span it lays out by hand, and the
// bulk span clockwork's allocator runs over, reached through
// supersonic_guest_alloc_bytes). Nothing of an instance is made with new or
// malloc: the system allocator is the one thing the audio thread may not
// reach, and what an instance left there was never reclaimed on a web
// reload, where the old engine is gone rather than torn down (measured
// 2026-10-15: 15.5 KB held more after every reload, which was exactly the
// World's tables, locks and client lists). Made in the guest's memory, an
// instance goes when its memory does.
//
// The helpers here are what upstream's new, delete, new[] and delete[] are,
// over the heap of the World's instance; GuestAllocator is the same for a
// standard container, and GuestMalloc for scsynth's own HashTable. Each one
// takes or holds the World, since the heap is the instance's and several
// instances may share a process. Out of memory is std::bad_alloc, as new's
// would be, for a caller inside a try; a caller that must not throw
// (dsp_new) takes the bytes itself.
#pragma once

#include "guest_doors.h"   // supersonic_guest_alloc_bytes / free_bytes

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

// The header ahead of an array's elements, holding its count so a free runs
// every destructor, as delete[] does. 16 bytes: the heap's alignment.
constexpr size_t kGuestArrayHeader = 16;

inline void* guest_alloc_or_throw(World* world, size_t bytes) {
    void* p = supersonic_guest_alloc_bytes(world, bytes);
    if (!p) throw std::bad_alloc();
    return p;
}

template <class T, class... A> T* guest_new(World* world, A&&... a) {
    return new (guest_alloc_or_throw(world, sizeof(T))) T(std::forward<A>(a)...);
}

template <class T> void guest_delete(World* world, T* p) {
    if (!p) return;
    p->~T();
    supersonic_guest_free_bytes(world, p);
}

template <class T> T* guest_new_array(World* world, size_t n) {
    // A count whose size in bytes does not fit in size_t is refused: wrapped,
    // it would allocate a few bytes and the loop below would write past them.
    if (n > (SIZE_MAX - kGuestArrayHeader) / sizeof(T)) throw std::bad_alloc();
    char* raw = static_cast<char*>(guest_alloc_or_throw(world, kGuestArrayHeader + sizeof(T) * n));
    *reinterpret_cast<size_t*>(raw) = n;
    T* items = reinterpret_cast<T*>(raw + kGuestArrayHeader);
    for (size_t i = 0; i < n; ++i) new (items + i) T();   // value-initialised: every count and pointer zero
    return items;
}

template <class T> void guest_delete_array(World* world, T* items) {
    if (!items) return;
    char* raw = reinterpret_cast<char*>(items) - kGuestArrayHeader;
    const size_t n = *reinterpret_cast<size_t*>(raw);
    for (size_t i = n; i-- > 0;) items[i].~T();
    supersonic_guest_free_bytes(world, raw);
}

// A standard allocator over the heap of one World's instance, for the
// server's containers (its client lists, the parser's scratch).
template <class T> struct GuestAllocator {
    using value_type = T;
    explicit GuestAllocator(World* world) : mWorld(world) {}
    template <class U> GuestAllocator(const GuestAllocator<U>& o) : mWorld(o.mWorld) {}
    T* allocate(size_t n) { return static_cast<T*>(guest_alloc_or_throw(mWorld, sizeof(T) * n)); }
    void deallocate(T* p, size_t) { supersonic_guest_free_bytes(mWorld, p); }
    template <class U> bool operator==(const GuestAllocator<U>& o) const { return mWorld == o.mWorld; }
    template <class U> bool operator!=(const GuestAllocator<U>& o) const { return mWorld != o.mWorld; }
    World* mWorld;
};

// scsynth's HashTable takes an allocator object with Alloc and Free: this
// one over the heap of one World's instance, where upstream's Malloc is the
// system's. Each HiddenWorld holds one (mGuestMalloc).
struct GuestMalloc {
    void* Alloc(size_t size) { return guest_alloc_or_throw(mWorld, size); }
    void Free(void* ptr) { supersonic_guest_free_bytes(mWorld, ptr); }
    World* mWorld = nullptr;
};
