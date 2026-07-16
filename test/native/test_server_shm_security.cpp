/*
 * test_server_shm_security.cpp — the public POSIX shm segment is a trust
 * boundary on a multi-user machine: the engine dereferences ring cursors that
 * live inside it. These tests pin the hardening:
 *   - the segment is created 0600 (owner-only), not world-accessible,
 *   - creation is exclusive (O_EXCL), so a pre-existing segment under the
 *     predictable name is never silently adopted, and
 *   - a reader rejects a too-small (foreign/truncated) segment before reading
 *     any header field.
 *
 * POSIX-only (the hardening is in the shm_open path); Windows uses named file
 * mappings and is out of scope here.
 */
#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>
#include "src/synth/common/server_shm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

using detail_server_shm::server_shared_memory_creator;
using detail_server_shm::server_shared_memory_client;
using detail_server_shm::make_shmem_name;

namespace {
std::string posixName(unsigned port) { return "/" + make_shmem_name(port); }
}

TEST_CASE("shm-security: segment is created owner-only (0600)", "[shm][security]") {
    constexpr unsigned kPort = 57311;
    server_shared_memory_creator::cleanup(kPort);  // clear any stale leftover
    server_shared_memory_creator creator(kPort, 0);

    // Re-open the live segment read-only and inspect its permission bits.
    int fd = ::shm_open(posixName(kPort).c_str(), O_RDONLY, 0);
    REQUIRE(fd >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    ::close(fd);

    CHECK((st.st_mode & 0777) == 0600);
    CHECK((st.st_mode & (S_IWGRP | S_IWOTH | S_IRGRP | S_IROTH)) == 0);
}

TEST_CASE("shm-security: creation is exclusive — a planted segment is rejected",
          "[shm][security]") {
    constexpr unsigned kPort = 57312;
    server_shared_memory_creator::cleanup(kPort);

    // Simulate a local attacker pre-creating the predictable name.
    int planted = ::shm_open(posixName(kPort).c_str(), O_CREAT | O_RDWR, 0666);
    REQUIRE(planted >= 0);
    ::close(planted);

    // O_EXCL must refuse to adopt it rather than ftruncating attacker memory.
    bool threw = false;
    try {
        server_shared_memory_creator creator(kPort, 0);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    ::shm_unlink(posixName(kPort).c_str());
}

TEST_CASE("shm-security: reader rejects a too-small segment before dereferencing",
          "[shm][security]") {
    constexpr unsigned kPort = 57313;
    server_shared_memory_creator::cleanup(kPort);

    // A tiny segment carrying a valid MAGIC in its first word: the magic check
    // alone would pass, so only a size check stops the client building region
    // pointers into a few bytes of memory.
    int fd = ::shm_open(posixName(kPort).c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 64) == 0);
    void* p = ::mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(p != MAP_FAILED);
    *reinterpret_cast<uint32_t*>(p) =
        detail_server_shm::shm_segment_header::MAGIC;
    ::munmap(p, 64);
    ::close(fd);

    bool threw = false;
    try {
        server_shared_memory_client client(kPort);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    ::shm_unlink(posixName(kPort).c_str());
}

TEST_CASE("shm-security: scope readers pin the client mapping (no dangling reads)",
          "[shm][security]") {
    constexpr unsigned kPort = 57314;
    server_shared_memory_creator::cleanup(kPort);
    server_shared_memory_creator creator(kPort, 0);
    creator.publish();

    // A GUI hands reader copies to long-lived widgets, then remaps on engine
    // cold swap by destroying its client. The reader must keep the old mapping
    // alive: 2026-07-11 this exact sequence was a GUI segfault (poll timer →
    // atomic load through the munmapped slot pointer).
    auto client = std::make_unique<server_shared_memory_client>(kPort);
    auto reader = client->get_scope_stream_reader(0);
    client.reset();

    float scratch[64 * 2] = {};
    (void)reader.valid();
    (void)reader.write_position();
    (void)reader.copy_window(reader.write_position(), 64, scratch);
    SUCCEED("reader survived client teardown without touching unmapped memory");
}

TEST_CASE("shm-security: scope reader clamps corrupt slot geometry",
          "[shm][security]") {
    constexpr unsigned kPort = 57315;
    server_shared_memory_creator::cleanup(kPort);
    server_shared_memory_creator creator(kPort, 0);
    creator.publish();
    server_shared_memory_client client(kPort);
    auto reader = client.get_scope_stream_reader(0);

    // channels/capacity_frames are re-read from shared memory on every
    // copy_window and index into the inline data array; a corrupt or hostile
    // slot must clamp to the compile-time ring geometry rather than push
    // reads out of bounds (or divide by a zero capacity).
    auto* slot = reinterpret_cast<shm_scope_stream*>(
        creator.get_base() + SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE);
    slot->state.store(1, std::memory_order_release);
    slot->channels = 0xFFFFu;
    slot->capacity_frames = 0;
    slot->write_position.store(1u << 20, std::memory_order_release);

    CHECK(reader.channels() == SHM_SCOPE_STREAM_CHANNELS);
    CHECK(reader.capacity_frames() == SHM_SCOPE_RING_FRAMES);
    std::vector<float> scratch(1024 * SHM_SCOPE_STREAM_CHANNELS, 0.0f);
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("copy_window stayed inside the inline ring under corrupt geometry");

    slot->capacity_frames = 0x7FFFFFFFu;  // absurdly large
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("oversized capacity clamped");
}

TEST_CASE("shm-security: client rejects a segment with a foreign layout",
          "[shm][security]") {
    constexpr unsigned kPort = 57318;
    server_shared_memory_creator::cleanup(kPort);
    server_shared_memory_creator creator(kPort, 0);
    creator.publish();

    // Tamper one self-described offset, as if the engine were built with a
    // different memory profile (e.g. BUILD_TESTS capture-ring sizing). The
    // client must refuse rather than read garbage at its compile-time offsets.
    auto* hdr = reinterpret_cast<detail_server_shm::shm_segment_header*>(
        creator.get_base() - detail_server_shm::SHM_BLOB_OFFSET);
    hdr->scope_offset += 4096;

    bool threw = false;
    try {
        server_shared_memory_client client(kPort);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("shm client exposes the observer views inside its own mapping",
          "[shm]") {
    constexpr unsigned kPort = 57316;
    server_shared_memory_creator::cleanup(kPort);
    server_shared_memory_creator creator(kPort, 0);
    creator.publish();
    server_shared_memory_client client(kPort);
    uint8_t* base = client.get_base();

    auto in = client.get_in_ring();
    CHECK(in.base == base + IN_BUFFER_START);
    CHECK(in.size == IN_BUFFER_SIZE);
    REQUIRE(in.head != nullptr);
    REQUIRE(in.tail != nullptr);

    auto out = client.get_out_ring();
    CHECK(out.base == base + OUT_BUFFER_START);
    CHECK(out.size == OUT_BUFFER_SIZE);

    auto nrt = client.get_nrt_out_ring();
    CHECK(nrt.base == base + NRT_OUT_BUFFER_START);
    CHECK(nrt.size == NRT_OUT_BUFFER_SIZE);

    CHECK(client.get_metrics_flat()
          == reinterpret_cast<const std::atomic<uint32_t>*>(base + METRICS_START));
    CHECK(client.metrics_field_count() == METRICS_SIZE / 4);

    auto tree = client.get_node_tree();
    CHECK(tree.header == base + NODE_TREE_START);
    CHECK(tree.entries == base + NODE_TREE_START + NODE_TREE_HEADER_SIZE);
    CHECK(tree.max_nodes == NODE_TREE_MIRROR_MAX_NODES);
    CHECK(tree.entry_bytes == NODE_TREE_ENTRY_SIZE);

    CHECK(client.has_native_stats());
    (void)client.get_native_stats();
}

#endif  // !_WIN32
