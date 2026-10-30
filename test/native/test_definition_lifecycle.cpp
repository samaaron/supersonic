/*
 * test_definition_lifecycle.cpp — what happens to a synthdef, end to end.
 *
 * THIS REPLACES test_state_cache.cpp AND SEVEN CASES THAT WENT WITH IT.
 *
 * Until 2026-08-31 clockwork kept a definition cache: ClockworkEngine watched
 * /d_recv, /d_free and /d_freeAll go past — verbs it had been handed by one
 * engine — read a name out of each blob through dsp_definition_name(), and
 * held the bytes against a device switch. StateCache is gone, and rightly: it
 * asked clockwork to know which of a guest's messages carry state worth
 * keeping, which is a question it cannot answer.
 *
 * The tests went with the class, and that was the wrong call — the BEHAVIOUR
 * they were about did not go anywhere. A definition still loads, is still
 * usable, is still forgettable by name and wholesale, and still has to survive
 * or not survive a rebuild. So each case is ported to ask the engine directly
 * instead of asking clockwork's mirror of it:
 *
 *     stateCache().synthDefs().count(name)  ->  /status.reply numSynthDefs
 *     stateCache().synthDefs().size()       ->  the same, as a delta
 *     extractSynthDefName v1/v2/v3          ->  load a v1/v2/v3 blob and play it
 *     captureAll before a cold swap         ->  what actually survives one
 *
 * That is a better question in every case: the cache could agree with itself
 * while disagreeing with the engine, and one of these cases (cold swap) now
 * asserts the OPPOSITE of what its ancestor did, because the contract changed
 * and nothing had noticed.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscBuilder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// /status.reply args: unused(1), numUgens, numSynths, numGroups, numSynthDefs...
constexpr int kArgSynths     = 2;
constexpr int kArgSynthDefs  = 4;

int statusField(EngineFixture& fix, int index) {
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    OscReply r;
    if (!fix.waitForReply("/status.reply", r)) return -1;
    auto p = r.parsed();
    return p.argCount() > index ? p.argInt(index) : -1;
}

int synthDefCount(EngineFixture& fix) { return statusField(fix, kArgSynthDefs); }
int synthCount(EngineFixture& fix)    { return statusField(fix, kArgSynths); }

// The v1/v2/v3 fixtures the web suite loads over HTTP, read off disk here.
std::vector<uint8_t> versionFixture(const std::string& name) {
    const std::filesystem::path p =
        std::filesystem::path(CLOCKWORK_TEST_SYNTHDEFS_DIR) / "versions" / (name + ".scsyndef");
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

bool sendDef(EngineFixture& fix, const std::vector<uint8_t>& bytes) {
    auto pkt = osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size());
    return fix.sendAndExpectDone(pkt);
}

// /s_new takes a name then integers, so it needs the builder rather than the
// single-type message() helper. Followed by a /sync barrier: the reply is what
// says the node actually exists, where a fixed number of blocks only says time
// has passed.
bool newSynth(EngineFixture& fix, const char* defName, int32_t nodeId) {
    osc_test::Builder b;
    auto& m = b.begin("/s_new");
    m << defName << nodeId << (int32_t)0 << (int32_t)0;
    fix.send(b.end());

    fix.send(osc_test::message("/sync", nodeId));
    OscReply r;
    return fix.waitForReply("/synced", r);
}

} // namespace

// ── Loading ─────────────────────────────────────────────────────────────────

TEST_CASE("Definitions: /d_recv makes a definition known to the engine",
          "[definitions]") {
    EngineFixture fix;
    const int before = synthDefCount(fix);
    REQUIRE(before >= 0);

    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // A count, not a name: clockwork no longer holds a table to look a name
    // up in, and the engine's own tally is the thing that decides whether
    // /s_new will work.
    CHECK(synthDefCount(fix) == before + 1);
}

// Every definition the package ships loads. The front follows scsynth's rule
// for a directory: a file the engine refuses costs the load one definition
// and the /d_loadDir still answers /done, so a broken file fails at every
// boot with nothing but a debug line to show for it. This case names the
// file and gives the engine's reason.
TEST_CASE("Definitions: every packaged definition loads", "[definitions]") {
    EngineFixture fix;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(CLOCKWORK_SYNTHDEFS_DIR))
        if (e.is_regular_file() && e.path().extension() == ".scsyndef") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() > 100);

    for (const auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        REQUIRE(!bytes.empty());
        fix.clearReplies();
        fix.send(osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size()));
        OscReply done, fail;
        if (fix.waitForReply("/done", done)) continue;
        std::string why = "no answer";
        if (fix.waitForReply("/fail", fail, 0))
            why = fail.parsed().argCount() > 1 ? fail.parsed().argString(1) : "refused";
        FAIL(path.filename().string() << ": " << why);
    }
}

// The engine reads no files, on any platform: a definition reaches it as
// bytes, in /d_recv. Sent to the engine itself, /d_load and /d_loadDir are
// refused with what to do instead, and nothing is read. (supersonic::Commands,
// which the native server and the NIF send through, reads the files for it.)
TEST_CASE("Definitions: /d_load and /d_loadDir sent to the engine are refused, and nothing is read",
          "[definitions]") {
    EngineFixture fix;
    const int before = synthDefCount(fix);
    REQUIRE(before >= 0);
    const std::string dir = CLOCKWORK_SYNTHDEFS_DIR;
    const std::string file = dir + "/sonic-pi-beep.scsyndef";
    REQUIRE(std::filesystem::exists(file));

    for (const auto& [verb, path] : { std::pair<std::string, std::string>{ "/d_load", file },
                                      std::pair<std::string, std::string>{ "/d_loadDir", dir } }) {
        INFO(verb);
        fix.clearReplies();
        fix.send(osc_test::message(verb.c_str(), path.c_str()));
        OscReply fail;
        REQUIRE(fix.waitForReply("/fail", fail));
        CHECK(fail.parsed().argString(0) == verb);
        CHECK(fail.parsed().argString(1).find("/d_recv") != std::string::npos);
    }
    CHECK(synthDefCount(fix) == before);
}

TEST_CASE("Definitions: loading the same name twice replaces rather than adds",
          "[definitions]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    const int once = synthDefCount(fix);

    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Ports "StateCache: synthdef overwrite" — the old case asserted the map
    // had one entry; this asserts the engine has one definition.
    CHECK(synthDefCount(fix) == once);
}

// ── The three wire versions ─────────────────────────────────────────────────
//
// Ports "StateCache: extractSynthDefName v1/v2/v3". Clockwork had its own
// SCgf reader to key its cache by; there is none now, because nothing outside
// the guest needs to know what a definition is called. So the question becomes
// the one that always mattered: does a v1, v2 or v3 blob actually load and
// play? A name read wrongly shows up here as a synth that never starts.

TEST_CASE("Definitions: a v1 synthdef loads and plays", "[definitions][versions]") {
    EngineFixture fix;
    const auto bytes = versionFixture("test_simple_v1");
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(sendDef(fix, bytes));

    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "test_simple", 4001));
    CHECK(synthCount(fix) == before + 1);
}

TEST_CASE("Definitions: a v2 synthdef loads and plays", "[definitions][versions]") {
    EngineFixture fix;
    const auto bytes = versionFixture("test_simple_v2");
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(sendDef(fix, bytes));

    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "test_simple", 4002));
    CHECK(synthCount(fix) == before + 1);
}

TEST_CASE("Definitions: a v3 synthdef loads and plays", "[definitions][versions]") {
    EngineFixture fix;
    const auto bytes = versionFixture("test_simple_v3");
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(sendDef(fix, bytes));

    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "test_simple", 4003));
    CHECK(synthCount(fix) == before + 1);
}

TEST_CASE("Definitions: a multi-control v3 synthdef loads and plays",
          "[definitions][versions]") {
    EngineFixture fix;
    const auto bytes = versionFixture("test_multi_v3");
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(sendDef(fix, bytes));

    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "test_multi", 4004));
    CHECK(synthCount(fix) == before + 1);
}

TEST_CASE("Definitions: a truncated blob does not take the engine with it",
          "[definitions][versions]") {
    EngineFixture fix;
    auto bytes = versionFixture("test_simple_v3");
    REQUIRE(bytes.size() > 16);
    bytes.resize(bytes.size() / 2);        // half a definition

    // Ports "StateCache: extractSynthDefName invalid data". Whether the engine
    // accepts it is the engine's business — what this pins is that clockwork
    // is still answering afterwards, which is what a parse that ran off the
    // end of the blob would end.
    auto pkt = osc_test::messageWithBlob("/d_recv", bytes.data(), bytes.size());
    fix.send(pkt);
    fix.pumpBlock(4);

    CHECK(synthDefCount(fix) >= 0);        // /status still answers
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));   // and still works
}

// ── Forgetting ──────────────────────────────────────────────────────────────

TEST_CASE("Definitions: /d_free forgets one by name", "[definitions]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    const int loaded = synthDefCount(fix);

    fix.send(osc_test::message("/d_free", "sonic-pi-beep"));
    fix.pumpBlock(4);

    // Ports "DeviceManagement: /d_free removes from cache". Clockwork does
    // not watch this address any more — it forwards it like any other — so the
    // assertion is on the engine that acted on it.
    CHECK(synthDefCount(fix) == loaded - 1);
}

TEST_CASE("Definitions: /d_freeAll forgets all of them", "[definitions]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fix.loadSynthDef("sonic-pi-saw"));
    REQUIRE(synthDefCount(fix) >= 2);

    fix.send(osc_test::message("/d_freeAll"));
    fix.pumpBlock(4);

    // Ports "DeviceManagement: /d_freeAll clears cache".
    CHECK(synthDefCount(fix) == 0);
}

TEST_CASE("Definitions: /d_free of a name that is not loaded fails", "[definitions]") {
    EngineFixture fix;
    fix.clearReplies();
    fix.send(osc_test::message("/d_free", "no-such-definition"));
    OscReply r;
    REQUIRE(fix.waitForReply("/fail", r));
    CHECK(r.parsed().argString(0) == "/d_free");
}

// A definition a synth is playing outlives /d_freeAll: the library forgets it
// at once, the synth keeps it until the synth ends, and only then is it freed
// and /d_removed sent to the clients registered with /notify.
TEST_CASE("Definitions: /d_freeAll frees a definition a synth is playing when the synth ends",
          "[definitions]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    fix.send(osc_test::message("/notify", 1));
    OscReply r;
    REQUIRE(fix.waitForReply("/done", r));

    osc_test::Builder play;
    play.begin("/s_new") << "sonic-pi-beep" << 1000 << 0 << 0 << "sustain" << 1000.0f;
    fix.send(play.end());

    fix.clearReplies();
    fix.send(osc_test::message("/d_freeAll"));
    CHECK(synthDefCount(fix) == 0);
    // /status is answered after /d_freeAll, so a /d_removed it sent is in by now.
    for (const auto& reply : fix.allReplies())
        CHECK(reply.parsed().address != "/d_removed");

    // The synth's controls are found by name through its definition.
    osc_test::Builder set;
    set.begin("/n_set") << 1000 << "amp" << 0.25f;
    fix.send(set.end());
    osc_test::Builder get;
    get.begin("/s_get") << 1000 << "amp";
    fix.clearReplies();
    fix.send(get.end());
    REQUIRE(fix.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == 0.25f);

    fix.clearReplies();
    fix.send(osc_test::message("/n_free", 1000));
    REQUIRE(fix.waitForReply("/d_removed", r));
    CHECK(r.parsed().argString(0) == "sonic-pi-beep");
}

// ── Across a rebuild ────────────────────────────────────────────────────────

TEST_CASE("Definitions: a cold swap takes the definitions with it",
          "[definitions][ColdSwap]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    const int before = synthDefCount(fix);
    REQUIRE(before >= 1);

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    /*
     * THIS ASSERTS THE OPPOSITE OF THE CASE IT REPLACES.
     *
     * "RapidSwitch: synthdef cache survives rapid cold swaps" and "ColdSwap:
     * captureAll called before destroy" both asserted that something inside
     * clockwork held the definitions across a rebuild. Nothing does now, by
     * design: a cold swap destroys the guest and builds a new one, and putting
     * back what the client had is the client's job — SuperSonic does it in
     * restoreClientState(), and this repository has no native client to do it
     * here.
     *
     * So the definitions are gone, and that is the contract. If this ever
     * starts passing with `before`, something has quietly begun replaying a
     * guest's messages again.
     */
    CHECK(synthDefCount(fix) < before);
}

TEST_CASE("Definitions: re-sending after a cold swap restores them",
          "[definitions][ColdSwap]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // The other half of the same contract: what a client re-sends comes back,
    // and the engine is in a state to receive it. This is what
    // SuperSonic::restoreClientState() does on the web, one layer up.
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    CHECK(synthDefCount(fix) >= 1);

    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "sonic-pi-beep", 4100));
    CHECK(synthCount(fix) == before + 1);
}

TEST_CASE("Definitions: rapid cold swaps leave a usable engine",
          "[definitions][RapidSwitch]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Ports "RapidSwitch: synthdef cache survives rapid cold swaps" — the
    // sequence it drove was worth keeping even though its assertion was not.
    for (int i = 0; i < 5; ++i) {
        const double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Cold);
    }

    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    const int before = synthCount(fix);
    REQUIRE(newSynth(fix, "sonic-pi-beep", 4200));
    CHECK(synthCount(fix) == before + 1);
}
