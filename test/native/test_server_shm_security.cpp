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
#include "src/scsynth/common/server_shm.hpp"

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

#endif  // !_WIN32
