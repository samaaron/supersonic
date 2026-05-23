// FakeLinkPeerProcess — RAII wrapper around the supersonic_test_link_peer
// binary. Spawns it as a child process with the requested options, blocks
// until the child writes its "ready" line on stdout, and terminates +
// reaps on destruction.
//
// POSIX:   fork + execv + waitpid + SIGTERM (then SIGKILL on timeout).
// Windows: CreateProcess + anonymous pipe + TerminateProcess +
//          WaitForSingleObject.
//
// Only available on builds with SUPERSONIC_LINK; the peer binary itself
// is gated on SUPERSONIC_ENABLE_LINK in CMakeLists.txt.

#pragma once

#ifdef SUPERSONIC_LINK

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class FakeLinkPeerProcess {
public:
    struct Channel {
        std::string name;
        int         numChannels = 2;
        std::string generator   = "silence";
    };

    struct Options {
        std::string name         = "TestPeer";
        bool        loopbackOnly = true;
        double      bpm          = 120.0;
        int         blockSize    = 1024;
        int         sampleRate   = 48000;
        std::vector<Channel> channels;
        std::chrono::milliseconds readyTimeout{5000};
    };

    explicit FakeLinkPeerProcess(const Options& opts);
    ~FakeLinkPeerProcess();

    FakeLinkPeerProcess(const FakeLinkPeerProcess&) = delete;
    FakeLinkPeerProcess& operator=(const FakeLinkPeerProcess&) = delete;

    // True if spawn succeeded and the child printed "ready" before
    // readyTimeout. False otherwise — caller should treat as a failed
    // fixture (the destructor still cleans up safely).
    bool ready() const { return mReady; }

    const Options& options() const { return mOptions; }

private:
    Options mOptions;
    // Opaque process handle. On POSIX this is a pid_t; on Windows a
    // HANDLE cast to intptr_t (-1 / 0 = invalid). The .cpp casts back
    // as needed under platform #ifdefs.
    intptr_t mProcess     = -1;
    intptr_t mWinThread   = 0;   // Windows only: the thread handle
                                 // returned by CreateProcess; needs its
                                 // own CloseHandle.
    bool    mReady        = false;
};

#endif  // SUPERSONIC_LINK
