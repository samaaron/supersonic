#include "FakeLinkPeerProcess.h"

#ifdef CLOCKWORK_LINK

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef CLOCKWORK_TEST_LINK_PEER_BINARY
#  error "CLOCKWORK_TEST_LINK_PEER_BINARY must be defined by CMake"
#endif

namespace {

// ── Shared helpers ────────────────────────────────────────────────────────

// Build the option list as plain strings; each backend converts to its
// platform-native argv / command-line form.
std::vector<std::string> buildArgList(const FakeLinkPeerProcess::Options& opts,
                                      const std::string& stallFlag,
                                      const std::string& report) {
    std::vector<std::string> args;
    args.emplace_back(CLOCKWORK_TEST_LINK_PEER_BINARY);
    args.emplace_back("--name");        args.push_back(opts.name);
    if (opts.loopbackOnly) args.emplace_back("--loopback-only");
    args.emplace_back("--bpm");         args.push_back(std::to_string(opts.bpm));
    args.emplace_back("--block-size");  args.push_back(std::to_string(opts.blockSize));
    args.emplace_back("--sample-rate"); args.push_back(std::to_string(opts.sampleRate));
    if (opts.stallFor.count() > 0) {
        args.emplace_back("--stall");
        args.push_back(std::to_string(opts.stallFor.count()) + ":" + stallFlag);
    }
    if (opts.reportTiming) {
        args.emplace_back("--report");  args.push_back(report);
    }
    for (const auto& c : opts.channels) {
        args.emplace_back("--channel");
        args.push_back(c.name + ":" + std::to_string(c.numChannels) + ":" + c.generator);
    }
    return args;
}

// A path of this process's own for one of a peer's files, nowhere yet.
std::string freshPath(const char* what) {
    static std::atomic<unsigned> serial{0};
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (dir / ("supersonic_peer_" + std::string(what) + "_" + std::to_string(now) + "_"
                   + std::to_string(serial.fetch_add(1)))).string();
}

void removePeerFiles(const std::string& stallFlag, const std::string& report) {
    std::error_code ec;
    std::filesystem::remove(stallFlag, ec);
    std::filesystem::remove(report, ec);
    std::filesystem::remove(report + ".tmp", ec);
}

std::chrono::steady_clock::time_point steadyAt(int64_t ns) {
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

}  // namespace

void FakeLinkPeerProcess::stall() {
    std::ofstream(mStallFlag).put('\n');
}

std::optional<FakeLinkPeerProcess::Timing>
FakeLinkPeerProcess::timing(std::chrono::steady_clock::time_point after) const {
    if (!mOptions.reportTiming) return std::nullopt;
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        std::ifstream in(mReport);
        Timing t;
        std::chrono::steady_clock::time_point written;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string key;
            row >> key;
            int64_t v = 0;
            if (key == "t0" && row >> v)           t.t0 = steadyAt(v);
            else if (key == "written" && row >> v) written = steadyAt(v);
            else if (key == "late_us") while (row >> v) t.lateUs.push_back(static_cast<int32_t>(v));
            else if (key == "wait_us") while (row >> v) t.waitUs.push_back(static_cast<int32_t>(v));
            else if (key == "skipped") while (row >> v) t.skipped.push_back(static_cast<int32_t>(v));
        }
        in.close();
        if (written >= after) return t;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < giveUp);
    return std::nullopt;
}

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

FakeLinkPeerProcess::FakeLinkPeerProcess(const Options& opts)
    : mOptions(opts), mStallFlag(freshPath("stall")), mReport(freshPath("timing")) {
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

        const auto args = buildArgList(mOptions, mStallFlag, mReport);
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

// Once the peer is gone: it writes its timing a last time as it stops.
FakeLinkPeerProcess::~FakeLinkPeerProcess() {
    if (mProcess > 0) {
        const pid_t pid = static_cast<pid_t>(mProcess);
        ::kill(pid, SIGTERM);
        int status = 0;
        const auto killDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool reaped = false;
        while (!reaped && std::chrono::steady_clock::now() < killDeadline) {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            reaped = r == pid || r < 0;
            if (!reaped) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
    }
    removePeerFiles(mStallFlag, mReport);
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

FakeLinkPeerProcess::FakeLinkPeerProcess(const Options& opts)
    : mOptions(opts), mStallFlag(freshPath("stall")), mReport(freshPath("timing")) {
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

    const auto args = buildArgList(mOptions, mStallFlag, mReport);
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
        CLOCKWORK_TEST_LINK_PEER_BINARY,
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
            CLOCKWORK_TEST_LINK_PEER_BINARY,
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

// Once the peer is gone, which may have been writing its timing.
FakeLinkPeerProcess::~FakeLinkPeerProcess() {
    if (mProcess != 0 && mProcess != -1) {
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
    removePeerFiles(mStallFlag, mReport);
}

#endif  // _WIN32

#endif  // CLOCKWORK_LINK
