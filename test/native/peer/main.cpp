// supersonic_test_link_peer — minimal Link Audio peer used by SuperSonic
// integration tests. Publishes named channels with deterministic audio
// generators so tests can subscribe and assert exactly what arrives.
//
// Runs until SIGTERM / SIGINT. Lives in its own process so it stays
// isolated from the test binary's address space (matches how a real
// Link peer like Live participates on the mesh).
//
// Built only when SUPERSONIC_ENABLE_LINK is ON.

// MSVC's <cmath> doesn't expose M_PI unless _USE_MATH_DEFINES is set
// before the include — define it ourselves to keep portability simple.
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#include <ableton/LinkAudio.hpp>
#if defined(_WIN32)
#  include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#  include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif
#include <ableton/util/FloatIntConversion.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> gShouldQuit{false};
void handleSignal(int /*sig*/) {
    gShouldQuit.store(true, std::memory_order_relaxed);
}

struct ChannelSpec {
    std::string name;
    int         numChannels = 2;
    std::string generator   = "silence";
};

// Per-frame sample generator. `frame` is monotonic across the run so
// signals stay continuous across buffer boundaries.
double generateSample(const std::string& gen, uint64_t frame, int ch,
                       int sampleRate) {
    if (gen == "silence") return 0.0;
    if (gen == "sine440") {
        return std::sin(2.0 * M_PI * 440.0 * double(frame) / sampleRate);
    }
    if (gen == "sine440-880") {
        const double hz = (ch == 0) ? 440.0 : 880.0;
        return std::sin(2.0 * M_PI * hz * double(frame) / sampleRate);
    }
    if (gen.rfind("dc:", 0) == 0) {
        return std::atof(gen.c_str() + 3);
    }
    return 0.0;
}

bool parseInt(const char* s, int& out) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

bool parseDouble(const char* s, double& out) {
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

bool parseChannelArg(const char* arg, ChannelSpec& out) {
    const std::string s = arg;
    const auto p1 = s.find(':');
    if (p1 == std::string::npos) return false;
    const auto p2 = s.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;
    out.name = s.substr(0, p1);
    if (!parseInt(s.substr(p1 + 1, p2 - p1 - 1).c_str(), out.numChannels))
        return false;
    out.generator = s.substr(p2 + 1);
    return true;
}

void printUsage() {
    std::fputs(
        "Usage: supersonic_test_link_peer [options]\n"
        "  --name <string>            Peer name (default: TestPeer)\n"
        "  --loopback-only            Restrict discovery to lo0\n"
        "  --bpm <double>             Initial BPM (default: 120)\n"
        "  --block-size <int>         Frames per audio buffer (default: 1024)\n"
        "  --sample-rate <int>        Sample rate Hz (default: 48000)\n"
        "  --channel <name>:<numCh>:<gen>\n"
        "                             Add a channel (repeatable). Generators:\n"
        "                               silence, sine440, sine440-880, dc:<value>\n"
        "                             e.g. Main:2:sine440-880\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    std::string peerName    = "TestPeer";
    bool        loopbackOnly = false;
    double      bpm         = 120.0;
    int         blockSize   = 1024;
    int         sampleRate  = 48000;
    std::vector<ChannelSpec> channels;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--name"        && i + 1 < argc) { peerName = argv[++i]; }
        else if (a == "--loopback-only")               { loopbackOnly = true; }
        else if (a == "--bpm"         && i + 1 < argc) {
            if (!parseDouble(argv[++i], bpm)) { printUsage(); return 1; }
        }
        else if (a == "--block-size"  && i + 1 < argc) {
            if (!parseInt(argv[++i], blockSize)) { printUsage(); return 1; }
        }
        else if (a == "--sample-rate" && i + 1 < argc) {
            if (!parseInt(argv[++i], sampleRate)) { printUsage(); return 1; }
        }
        else if (a == "--channel"     && i + 1 < argc) {
            ChannelSpec c;
            if (!parseChannelArg(argv[++i], c)) { printUsage(); return 1; }
            channels.push_back(std::move(c));
        }
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else {
            std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            printUsage();
            return 1;
        }
    }

    std::signal(SIGTERM, handleSignal);
    std::signal(SIGINT, handleSignal);

#if defined(_WIN32)
    ableton::platforms::windows::loopbackOnly().store(
        loopbackOnly, std::memory_order_relaxed);
#else
    ableton::platforms::posix::loopbackOnly().store(
        loopbackOnly, std::memory_order_relaxed);
#endif

    ableton::LinkAudio link{bpm, peerName};
    link.enable(true);
    link.enableLinkAudio(true);

    constexpr size_t kSinkMaxSamples = 4096;
    struct ActiveChannel {
        ChannelSpec            spec;
        ableton::LinkAudioSink sink;
    };
    std::vector<ActiveChannel> sinks;
    sinks.reserve(channels.size());
    for (const auto& c : channels) {
        if (c.numChannels < 1 || c.numChannels > 2) {
            std::fprintf(stderr,
                "Channel %s: numChannels must be 1 or 2 (got %d)\n",
                c.name.c_str(), c.numChannels);
            return 1;
        }
        sinks.push_back({c, ableton::LinkAudioSink{link, c.name, kSinkMaxSamples}});
    }

    // Ready signal: write to stdout + flush so a parent test fixture
    // can block on this line via a pipe.
    std::fputs("supersonic_test_link_peer ready\n", stdout);
    std::fflush(stdout);

    const auto blockDuration = std::chrono::nanoseconds{
        static_cast<int64_t>(1'000'000'000.0 * double(blockSize) / sampleRate)};
    auto nextDeadline = std::chrono::steady_clock::now();
    uint64_t frameOffset = 0;

    while (!gShouldQuit.load(std::memory_order_relaxed)) {
        for (auto& a : sinks) {
            ableton::LinkAudioSink::BufferHandle buf(a.sink);
            if (!buf) continue;  // No subscriber → skip silently.
            const size_t needed = size_t(blockSize) * size_t(a.spec.numChannels);
            if (needed > buf.maxNumSamples) continue;
            for (int f = 0; f < blockSize; ++f) {
                for (int ch = 0; ch < a.spec.numChannels; ++ch) {
                    const float v = static_cast<float>(generateSample(
                        a.spec.generator, frameOffset + f, ch, sampleRate));
                    buf.samples[f * a.spec.numChannels + ch] =
                        ableton::util::floatToInt16(v);
                }
            }
            auto sessionState = link.captureAppSessionState();
            const auto hostMicros = link.clock().micros();
            const double beatsAtBegin =
                sessionState.beatAtTime(hostMicros, /*quantum=*/4.0);
            buf.commit(sessionState, beatsAtBegin, /*quantum=*/4.0,
                       size_t(blockSize), size_t(a.spec.numChannels),
                       uint32_t(sampleRate));
        }
        frameOffset += blockSize;
        nextDeadline += blockDuration;
        std::this_thread::sleep_until(nextDeadline);
    }

    link.enableLinkAudio(false);
    link.enable(false);
    return 0;
}
