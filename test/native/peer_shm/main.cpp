/*
 * peer_shm/main.cpp — out-of-process SHM command-plane peer used by
 * test_shm_peer_crash.cpp. Attaches to a running engine's segment and writes
 * /sync frames into the peer command ring exactly as a real peer (spider)
 * would: server_shared_memory_client + shm_peer_attach + RingBufferWriter.
 *
 * Modes (prints "ready" on stdout once attached):
 *   burst <port> <count>   write /sync ids 1..count (retry on ring-full), exit 0
 *   spam <port>            write /sync frames with increasing ids until killed
 *   hold-lock <port>       set cmd_write_lock and spin — a corpse holding the
 *                          writer lock, for the attach-recovery test
 */
#include "src/synth/common/server_shm.hpp"
#include "src/shm_peer_plane.h"
#include "src/workers/RingBufferWriter.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

// A minimal OSC "/sync <id>" message (address + ",i" + big-endian int32).
uint32_t encodeSync(uint8_t out[16], int32_t id) {
    std::memcpy(out, "/sync\0\0\0", 8);
    std::memcpy(out + 8, ",i\0\0", 4);
    out[12] = static_cast<uint8_t>(id >> 24);
    out[13] = static_cast<uint8_t>(id >> 16);
    out[14] = static_cast<uint8_t>(id >> 8);
    out[15] = static_cast<uint8_t>(id);
    return 16;
}

// Write one frame, retrying on ring-full (the engine drains every block).
void writeSync(ShmPeerPlaneHeader* plane, int32_t id) {
    uint8_t msg[16];
    uint32_t len = encodeSync(msg, id);
    while (!RingBufferWriter::write(
               shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
               &plane->cmd_head, &plane->cmd_tail,
               &plane->cmd_sequence, &plane->cmd_write_lock,
               msg, len, 0)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <burst|spam|hold-lock> <port> [count]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const unsigned port    = static_cast<unsigned>(std::atoi(argv[2]));

    ShmPeerPlaneHeader* plane = nullptr;
    try {
        // The client mapping must outlive every plane access; keep it for the
        // whole process lifetime.
        static server_shared_memory_client client(port);
        plane = client.get_peer_plane();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "peer_shm: cannot open segment SuperSonic_%u: %s\n", port, e.what());
        return 3;
    }
    if (!plane) return 3;

    if (mode == "probe") {
        // No attach — report the plane as-is (diagnostics).
        std::printf("gen=%u pid=%u cmd_head=%d cmd_tail=%d cmd_lock=%d "
                    "rep_head=%d rep_tail=%d rep_dropped=%u\n",
                    plane->generation.load(), plane->owner_pid.load(),
                    plane->cmd_head.load(), plane->cmd_tail.load(),
                    plane->cmd_write_lock.load(),
                    plane->rep_head.load(), plane->rep_tail.load(),
                    plane->rep_dropped.load());
        return 0;
    }

    shm_peer_attach(plane, static_cast<uint32_t>(getpid()));
    // hold-lock: take the lock BEFORE announcing readiness — the parent kills
    // us the moment it reads "ready", and the corpse must be holding the lock.
    if (mode == "hold-lock")
        plane->cmd_write_lock.store(1, std::memory_order_relaxed);
    std::printf("ready\n");
    std::fflush(stdout);

    if (mode == "burst") {
        const int count = (argc > 3) ? std::atoi(argv[3]) : 100;
        for (int32_t id = 1; id <= count; ++id)
            writeSync(plane, id);
        return 0;
    }
    if (mode == "spam") {
        for (int32_t id = 1;; ++id)
            writeSync(plane, id);
    }
    if (mode == "hold-lock") {
        for (;;)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fprintf(stderr, "peer_shm: unknown mode %s\n", mode.c_str());
    return 2;
}
