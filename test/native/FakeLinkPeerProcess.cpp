#include "FakeLinkPeerProcess.h"

#ifdef SUPERSONIC_LINK

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef SUPERSONIC_TEST_LINK_PEER_BINARY
#  error "SUPERSONIC_TEST_LINK_PEER_BINARY must be defined by CMake"
#endif

namespace {

// ── Shared helpers ────────────────────────────────────────────────────────

// Build the option list as plain strings; each backend converts to its
// platform-native argv / command-line form.
std::vector<std::string> buildArgList(const FakeLinkPeerProcess::Options& opts) {
    std::vector<std::string> args;
    args.emplace_back(SUPERSONIC_TEST_LINK_PEER_BINARY);
    args.emplace_back("--name");        args.push_back(opts.name);
    if (opts.loopbackOnly) args.emplace_back("--loopback-only");
    args.emplace_back("--bpm");         args.push_back(std::to_string(opts.bpm));
    args.emplace_back("--block-size");  args.push_back(std::to_string(opts.blockSize));
    args.emplace_back("--sample-rate"); args.push_back(std::to_string(opts.sampleRate));
    for (const auto& c : opts.channels) {
        args.emplace_back("--channel");
        args.push_back(c.name + ":" + std::to_string(c.numChannels) + ":" + c.generator);
    }
    return args;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// POSIX implementation
// ──────────────────────────────────────────────────────────────────────────
#if !defined(_WIN32)

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool waitForReadyLine(int fd, std::chrono::steady_clock::time_point deadline) {
    std::string accum;
    char buf[256];
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        struct pollfd pfd{fd, POLLIN, 0};
        const int p = ::poll(&pfd, 1, static_cast<int>(std::max<int64_t>(remaining, 0)));
        if (p <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        const auto n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return false;  // EOF / child died
        accum.append(buf, static_cast<size_t>(n));
        if (accum.find("ready") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

FakeLinkPeerProcess::FakeLinkPeerProcess(const Options& opts) : mOptions(opts) {
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        std::fprintf(stderr, "FakeLinkPeerProcess: pipe() failed: %s\n",
                     std::strerror(errno));
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        std::fprintf(stderr, "FakeLinkPeerProcess: fork() failed: %s\n",
                     std::strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);

        const auto args = buildArgList(mOptions);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        ::execv(argv[0], argv.data());
        std::fprintf(stderr, "FakeLinkPeerProcess: execv(%s) failed: %s\n",
                     argv[0], std::strerror(errno));
        std::_Exit(127);
    }

    ::close(pipefd[1]);
    mProcess = static_cast<intptr_t>(pid);
    const auto deadline = std::chrono::steady_clock::now() + mOptions.readyTimeout;
    mReady = waitForReadyLine(pipefd[0], deadline);
    ::close(pipefd[0]);

    if (!mReady) {
        std::fprintf(stderr,
            "FakeLinkPeerProcess: peer didn't signal ready within %lld ms\n",
            static_cast<long long>(mOptions.readyTimeout.count()));
    }
}

FakeLinkPeerProcess::~FakeLinkPeerProcess() {
    if (mProcess <= 0) return;
    const pid_t pid = static_cast<pid_t>(mProcess);
    ::kill(pid, SIGTERM);
    int status = 0;
    const auto killDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < killDeadline) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Windows implementation
// ──────────────────────────────────────────────────────────────────────────
#else  // _WIN32

#include <windows.h>

namespace {

// Build CreateProcess's joined-and-quoted command line. Each token is
// wrapped in double quotes; embedded backslashes/quotes follow the
// Microsoft C runtime parser rules. Our args don't contain quotes in
// practice but we escape defensively.
std::string quoteArg(const std::string& a) {
    std::string out;
    out.reserve(a.size() + 2);
    out.push_back('"');
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { backslashes++; out.push_back(c); }
        else if (c == '"') {
            // Double up any preceding backslashes, then escape the quote.
            out.append(backslashes, '\\');
            out.push_back('\\'); out.push_back('"');
            backslashes = 0;
        }
        else { backslashes = 0; out.push_back(c); }
    }
    // Trailing backslashes need doubling so they don't escape the closing quote.
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

std::string buildCommandLine(const std::vector<std::string>& args) {
    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd.push_back(' ');
        cmd += quoteArg(args[i]);
    }
    return cmd;
}

bool waitForReadyHandle(HANDLE readEnd,
                         std::chrono::steady_clock::time_point deadline) {
    std::string accum;
    char buf[256];
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr)) {
            return false;  // pipe broken (child exited)
        }
        if (avail > 0) {
            DWORD got = 0;
            const DWORD want = (avail < sizeof(buf)) ? avail
                                                     : static_cast<DWORD>(sizeof(buf));
            if (!ReadFile(readEnd, buf, want, &got, nullptr) || got == 0) return false;
            accum.append(buf, got);
            if (accum.find("ready") != std::string::npos) return true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return false;
}

}  // namespace

FakeLinkPeerProcess::FakeLinkPeerProcess(const Options& opts) : mOptions(opts) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        std::fprintf(stderr,
            "FakeLinkPeerProcess: CreatePipe failed: %lu\n",
            static_cast<unsigned long>(GetLastError()));
        return;
    }
    // Parent's read end isn't inherited by the child.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    const auto args = buildArgList(mOptions);
    const std::string cmdline = buildCommandLine(args);
    std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writeEnd;
    si.hStdError  = writeEnd;

    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessA(
        SUPERSONIC_TEST_LINK_PEER_BINARY,
        cmdBuf.data(),
        nullptr, nullptr,
        TRUE,    // inherit handles
        0,
        nullptr, nullptr,
        &si, &pi);
    CloseHandle(writeEnd);  // parent doesn't write
    if (!ok) {
        std::fprintf(stderr,
            "FakeLinkPeerProcess: CreateProcess(%s) failed: %lu\n",
            SUPERSONIC_TEST_LINK_PEER_BINARY,
            static_cast<unsigned long>(GetLastError()));
        CloseHandle(readEnd);
        return;
    }

    mProcess   = reinterpret_cast<intptr_t>(pi.hProcess);
    mWinThread = reinterpret_cast<intptr_t>(pi.hThread);

    const auto deadline = std::chrono::steady_clock::now() + mOptions.readyTimeout;
    mReady = waitForReadyHandle(readEnd, deadline);
    CloseHandle(readEnd);

    if (!mReady) {
        std::fprintf(stderr,
            "FakeLinkPeerProcess: peer didn't signal ready within %lld ms\n",
            static_cast<long long>(mOptions.readyTimeout.count()));
    }
}

FakeLinkPeerProcess::~FakeLinkPeerProcess() {
    if (mProcess == 0 || mProcess == -1) return;
    HANDLE process = reinterpret_cast<HANDLE>(mProcess);
    // TerminateProcess is equivalent to SIGKILL — bypasses the peer's
    // signal handlers, so Link doesn't shut down gracefully and may
    // leave brief stale entries in other peers' session lists. Fine
    // for tests; switch to GenerateConsoleCtrlEvent if a graceful path
    // becomes important.
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 2000);
    CloseHandle(process);
    if (mWinThread) CloseHandle(reinterpret_cast<HANDLE>(mWinThread));
}

#endif  // _WIN32

#endif  // SUPERSONIC_LINK
