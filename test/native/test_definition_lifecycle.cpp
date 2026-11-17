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
#include "SampleLane.h"           // the assets lane's pool, shared with the sample tests
#include "definition_assets.h"    // kDefinitionAssetFirst

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace {

// /status.reply args: unused(1), numUgens, numSynths, numGroups, numSynthDefs...
constexpr int kArgUgens      = 1;
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

// ── Definitions as assets (definition_assets.h) ─────────────────────────────

constexpr uint32_t kFirst = supersonic::kDefinitionAssetFirst;

// A packaged definition file's bytes.
std::vector<uint8_t> definitionFile(const std::string& name) {
    std::ifstream f(std::filesystem::path(CLOCKWORK_SYNTHDEFS_DIR) / (name + ".scsyndef"), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A version 2 file's header: "SCgf", int32 version, int16 count. Its definitions follow.
constexpr size_t kV2Header = 10;

bool isV2(const std::vector<uint8_t>& f) {
    return f.size() > kV2Header && std::memcmp(f.data(), "SCgf\0\0\0\2", 8) == 0;
}

// One file holding both files' definitions (both version 2).
std::vector<uint8_t> joined(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    REQUIRE(isV2(a));
    REQUIRE(isV2(b));
    REQUIRE(a[8] == 0);
    REQUIRE(b[8] == 0);
    std::vector<uint8_t> out(a.begin(), a.begin() + kV2Header);
    out[9] = static_cast<uint8_t>(a[9] + b[9]);
    out.insert(out.end(), a.begin() + kV2Header, a.end());
    out.insert(out.end(), b.begin() + kV2Header, b.end());
    return out;
}

// A one-definition version 2 file with its definition's name changed to `to`, a name of the same length.
std::vector<uint8_t> renamed(std::vector<uint8_t> f, const std::string& to) {
    REQUIRE(isV2(f));
    REQUIRE(f[kV2Header] == to.size());   // the name is a pascal string right after the header
    std::memcpy(f.data() + kV2Header + 1, to.data(), to.size());
    return f;
}

// Copies `bytes` into a slot of the assets lane and returns the message that commits them as definition asset `id`.
// The slot goes back to the pool when the guest releases the asset (sample_lane::poolFor).
osc_test::Packet stagedDefinition(EngineFixture& fix, uint32_t id, const std::vector<uint8_t>& bytes) {
    sample_lane::LanePool& lp = sample_lane::poolFor(fix);
    uint32_t slot = 0;
    REQUIRE(clockwork_asset_pool_alloc(lp.pool, static_cast<uint32_t>(bytes.size()), &slot) == 0);
    std::memcpy(const_cast<uint8_t*>(fix.engine().guestAssets()) + slot, bytes.data(), bytes.size());
    lp.held.push_back({ static_cast<int32_t>(id), slot });
    osc_test::Builder b;
    b.begin("/clockwork/asset/commit") << static_cast<int32_t>(id) << static_cast<int32_t>(CLOCKWORK_ASSET_RAW)
                                       << static_cast<int32_t>(slot) << static_cast<int32_t>(bytes.size())
                                       << int32_t{0} << int32_t{0} << 0.0f;
    return b.end();
}

bool commitDefinition(EngineFixture& fix, uint32_t id, const std::vector<uint8_t>& bytes) {
    fix.send(stagedDefinition(fix, id, bytes));
    OscReply r;
    return fix.waitForReply("/clockwork/asset/committed", r, 3000) && r.parsed().argInt(0) == static_cast<int32_t>(id);
}

// The ids in the /clockwork/asset/released replies since the replies were last cleared. statusField() clears them,
// so read these first.
std::multiset<int32_t> releasedIds(EngineFixture& fix) {
    std::multiset<int32_t> ids;
    for (const auto& r : fix.allReplies())
        if (r.address == "/clockwork/asset/released") ids.insert(r.parsed().argInt(0));
    return ids;
}

// How many unit generators run while one synth of `defName` plays: tells two definitions of one name apart.
int ugensOf(EngineFixture& fix, const char* defName, int32_t nodeId) {
    REQUIRE(newSynth(fix, defName, nodeId));
    const int ugens = statusField(fix, kArgUgens);
    fix.send(osc_test::message("/n_free", nodeId));
    fix.pumpBlock(2);
    return ugens;
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

// Every packaged definition plays: a synth of each is started with its default
// controls, renders 32 blocks and is freed, three times over, and the engine
// then still answers. Freeing a synth hands its unit generators' real-time
// memory back, so a unit that wrote outside it, or hands back the wrong
// pointer, shows here.
TEST_CASE("Definitions: every packaged definition plays and frees", "[definitions]") {
    EngineFixture fix;
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(CLOCKWORK_SYNTHDEFS_DIR))
        if (e.is_regular_file() && e.path().extension() == ".scsyndef") names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    REQUIRE(names.size() > 100);

    for (const auto& name : names) {
        REQUIRE(fix.loadSynthDef(name));
        for (int round = 0; round < 3; ++round) {
            osc_test::Builder play;
            play.begin("/s_new") << name.c_str() << -1 << 0 << 0;
            fix.send(play.end());
            fix.pumpBlock(32);
            fix.send(osc_test::message("/g_freeAll", 0));
            fix.pumpBlock(2);
        }
        fix.send(osc_test::message("/d_free", name.c_str()));
    }
    fix.clearReplies();
    fix.send(osc_test::message("/sync", 7));
    OscReply r;
    REQUIRE(fix.waitForReply("/synced", r));
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

// ── Definitions as assets ───────────────────────────────────────────────────
//
// A client can commit a definition file's bytes as a RAW asset with an id from kDefinitionAssetFirst up
// (definition_assets.h). Clockwork hands the assets it holds to each new instance, so these definitions are still
// there after a cold swap, unlike the ones sent with /d_recv above.

TEST_CASE("Definitions: a definition committed as an asset is still there after a cold swap",
          "[definitions][assets][ColdSwap]") {
    EngineFixture fix;
    const int before = synthDefCount(fix);
    REQUIRE(before >= 0);
    REQUIRE(commitDefinition(fix, kFirst, definitionFile("sonic-pi-beep")));
    REQUIRE(synthDefCount(fix) == before + 1);

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    CHECK(synthDefCount(fix) == before + 1);
    const int synths = synthCount(fix);
    REQUIRE(newSynth(fix, "sonic-pi-beep", 4300));
    CHECK(synthCount(fix) == synths + 1);
}

TEST_CASE("Definitions: a definition asset that replaces one of the same name releases the replaced one's asset",
          "[definitions][assets]") {
    EngineFixture fix;
    const auto beep = definitionFile("sonic-pi-beep");
    REQUIRE(commitDefinition(fix, kFirst, beep));
    fix.clearReplies();
    REQUIRE(commitDefinition(fix, kFirst + 1, beep));
    CHECK(releasedIds(fix) == std::multiset<int32_t>{ static_cast<int32_t>(kFirst) });
}

TEST_CASE("Definitions: /d_free releases a definition's asset once no definition from it is left",
          "[definitions][assets]") {
    EngineFixture fix;
    REQUIRE(commitDefinition(fix, kFirst, joined(definitionFile("sonic-pi-beep"), definitionFile("sonic-pi-piano"))));
    fix.clearReplies();

    fix.send(osc_test::message("/d_free", "sonic-pi-beep"));
    fix.pumpBlock(4);
    CHECK(releasedIds(fix).empty());   // sonic-pi-piano still comes from it

    fix.send(osc_test::message("/d_free", "sonic-pi-piano"));
    fix.pumpBlock(4);
    CHECK(releasedIds(fix) == std::multiset<int32_t>{ static_cast<int32_t>(kFirst) });
}

TEST_CASE("Definitions: /d_freeAll releases each definition asset once", "[definitions][assets]") {
    EngineFixture fix;
    REQUIRE(commitDefinition(fix, kFirst, joined(definitionFile("sonic-pi-beep"), definitionFile("sonic-pi-piano"))));
    REQUIRE(commitDefinition(fix, kFirst + 1, definitionFile("sonic-pi-rodeo")));
    fix.clearReplies();

    fix.send(osc_test::message("/d_freeAll"));
    fix.pumpBlock(4);
    CHECK(releasedIds(fix) == std::multiset<int32_t>{ static_cast<int32_t>(kFirst), static_cast<int32_t>(kFirst + 1) });
    CHECK(synthDefCount(fix) == 0);
}

TEST_CASE("Definitions: a definition asset scsynth cannot read is refused", "[definitions][assets]") {
    EngineFixture fix;
    const int before = synthDefCount(fix);
    auto truncated = definitionFile("sonic-pi-beep");
    REQUIRE(truncated.size() > 40);
    truncated.resize(40);

    fix.send(stagedDefinition(fix, kFirst, truncated));
    OscReply r;
    REQUIRE(fix.waitForReply("/clockwork/asset/refused", r, 3000));
    CHECK(r.parsed().argInt(0) == static_cast<int32_t>(kFirst));
    CHECK(synthDefCount(fix) == before);
}

// A new instance is handed the assets in Clockwork's table order, not the order they were committed in. With these
// two ids, in a 4096-slot table, the newer asset (kFirst + 2) comes before the older one (kFirst + 1). The older one
// still holds a live definition (sonic-pi-beep), so it is handed over too, with a sonic-pi-piano the newer one
// replaced. The newer definition must still be the one left afterwards. A second cold swap checks that neither asset
// was released during the first: before the fix, the older piano replaced the newer one and released its asset.
TEST_CASE("Definitions: after a cold swap, the newer of two definition assets with the same name wins",
          "[definitions][assets][ColdSwap]") {
    EngineFixture fix;
    const auto piano = definitionFile("sonic-pi-piano");
    const auto beep  = definitionFile("sonic-pi-beep");
    const auto rodeo = definitionFile("sonic-pi-rodeo");
    REQUIRE(commitDefinition(fix, kFirst + 1, joined(piano, beep)));
    const int pianoUgens = ugensOf(fix, "sonic-pi-piano", 4400);
    REQUIRE(commitDefinition(fix, kFirst + 2, renamed(rodeo, "sonic-pi-piano")));
    const int rodeoUgens = ugensOf(fix, "sonic-pi-piano", 4401);
    REQUIRE(pianoUgens != rodeoUgens);   // so the two can be told apart

    for (const double rate : { 44100.0, 48000.0 }) {
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Cold);
        CHECK(ugensOf(fix, "sonic-pi-piano", 4402) == rodeoUgens);
        CHECK(ugensOf(fix, "sonic-pi-beep", 4403) > 0);
    }
}
