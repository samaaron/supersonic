/*
 * test_node_tree_mirror.cpp — Tests for NodeTreeHeader/NodeEntry in shared memory.
 *
 * Validates the node tree mirror written into ring_buffer_storage by scsynth,
 * including root/default group presence, node lifecycle, version tracking,
 * and structural fields (parent_id, head_id, is_group, def_name).
 */
#include "EngineFixture.h"
#include "src/shared_memory.h"

extern "C" uint8_t ring_buffer_storage[];

// ── Helper: find a node by ID in the mirror ─────────────────────────────────

static const NodeEntry* findNode(int32_t id) {
    auto* header = reinterpret_cast<NodeTreeHeader*>(ring_buffer_storage + NODE_TREE_START);
    auto* entries = reinterpret_cast<NodeEntry*>(ring_buffer_storage + NODE_TREE_START + NODE_TREE_HEADER_SIZE);
    uint32_t count = header->node_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count && i < NODE_TREE_MIRROR_MAX_NODES; i++) {
        if (entries[i].id == id) return &entries[i];
    }
    return nullptr;
}

static NodeTreeHeader* getHeader() {
    return reinterpret_cast<NodeTreeHeader*>(ring_buffer_storage + NODE_TREE_START);
}

// ── Helper: create a synth via /s_new ────────────────────────────────────────

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// ── Helper: /sync barrier — waits for engine to process all prior commands ───

static void syncBarrier(EngineFixture& fx, int32_t syncId) {
    fx.send(osc_test::message("/sync", syncId));
    OscReply syncR;
    fx.waitForReply("/synced", syncR);
}

// =============================================================================
// SECTION: Initial boot state
// =============================================================================

TEST_CASE("Root group (0) exists in mirror after boot", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* root = findNode(0);
    REQUIRE(root != nullptr);
}

TEST_CASE("Default group (1) exists in mirror after boot", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* defGroup = findNode(1);
    REQUIRE(defGroup != nullptr);
}

TEST_CASE("Root group has is_group=1", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* root = findNode(0);
    REQUIRE(root != nullptr);
    CHECK(root->is_group == 1);
}

TEST_CASE("Root group has parent_id=-1 (no parent)", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* root = findNode(0);
    REQUIRE(root != nullptr);
    CHECK(root->parent_id == -1);
}

TEST_CASE("Default group (1) has parent_id=0", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* defGroup = findNode(1);
    REQUIRE(defGroup != nullptr);
    CHECK(defGroup->parent_id == 0);
}

TEST_CASE("Default group (1) has is_group=1", "[node_tree_mirror]") {
    EngineFixture fx;

    const NodeEntry* defGroup = findNode(1);
    REQUIRE(defGroup != nullptr);
    CHECK(defGroup->is_group == 1);
}

TEST_CASE("node_count is >= 2 after boot (root + default group)", "[node_tree_mirror]") {
    EngineFixture fx;

    auto* header = getHeader();
    uint32_t count = header->node_count.load(std::memory_order_acquire);
    CHECK(count >= 2);
}

TEST_CASE("version is non-zero after boot (tree was modified)", "[node_tree_mirror]") {
    EngineFixture fx;

    auto* header = getHeader();
    uint32_t ver = header->version.load(std::memory_order_acquire);
    CHECK(ver > 0);
}

// =============================================================================
// SECTION: Node creation and deletion
// =============================================================================

TEST_CASE("Creating a new group increases node_count", "[node_tree_mirror]") {
    EngineFixture fx;

    auto* header = getHeader();
    uint32_t before = header->node_count.load(std::memory_order_acquire);

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    syncBarrier(fx, 100);

    uint32_t after = header->node_count.load(std::memory_order_acquire);
    CHECK(after > before);

    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("Creating a synth adds it to the mirror with correct def_name", "[node_tree_mirror]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());
    syncBarrier(fx, 101);

    const NodeEntry* synth = findNode(1000);
    REQUIRE(synth != nullptr);
    CHECK(std::string(synth->def_name) == "sonic-pi-beep");

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("Freeing a synth decreases node_count", "[node_tree_mirror]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());
    syncBarrier(fx, 102);

    auto* header = getHeader();
    uint32_t before = header->node_count.load(std::memory_order_acquire);

    fx.send(osc_test::message("/n_free", 1000));
    syncBarrier(fx, 103);

    uint32_t after = header->node_count.load(std::memory_order_acquire);
    CHECK(after < before);
}

// =============================================================================
// SECTION: Structural fields
// =============================================================================

TEST_CASE("Creating nested groups: parent_id is correct", "[node_tree_mirror]") {
    EngineFixture fx;

    // Create group 100 at head of root
    fx.send(osc_test::message("/g_new", 100, 0, 0));

    // Create group 200 inside group 100
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    syncBarrier(fx, 104);

    const NodeEntry* g200 = findNode(200);
    REQUIRE(g200 != nullptr);
    CHECK(g200->parent_id == 100);

    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("Group's head_id points to first child after adding synth", "[node_tree_mirror]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create a group
    fx.send(osc_test::message("/g_new", 100, 0, 0));

    // Add synth 1000 at head of group 100
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)100;
    fx.send(b.end());
    syncBarrier(fx, 105);

    const NodeEntry* g100 = findNode(100);
    REQUIRE(g100 != nullptr);
    CHECK(g100->head_id == 1000);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("Synth's parent_id matches the group it was added to", "[node_tree_mirror]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create group 100 at head of root
    fx.send(osc_test::message("/g_new", 100, 0, 0));

    // Add synth 1000 to group 100
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)100;
    fx.send(b.end());
    syncBarrier(fx, 106);

    const NodeEntry* synth = findNode(1000);
    REQUIRE(synth != nullptr);
    CHECK(synth->parent_id == 100);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
}

// =============================================================================
// SECTION: Version tracking
// =============================================================================

TEST_CASE("version increments when tree changes", "[node_tree_mirror]") {
    EngineFixture fx;

    auto* header = getHeader();
    uint32_t versionBefore = header->version.load(std::memory_order_acquire);

    // Create a group — should bump version
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    syncBarrier(fx, 107);

    uint32_t versionAfter = header->version.load(std::memory_order_acquire);
    CHECK(versionAfter > versionBefore);

    fx.send(osc_test::message("/n_free", 100));
}

// =============================================================================
// SECTION: Overflow / dropped count
// =============================================================================

TEST_CASE("dropped_count is 0 with few nodes", "[node_tree_mirror]") {
    EngineFixture fx;

    auto* header = getHeader();
    uint32_t dropped = header->dropped_count.load(std::memory_order_acquire);
    CHECK(dropped == 0);
}

// =============================================================================
// SECTION: Cleanup and edge cases
// =============================================================================

TEST_CASE("Freeing all synths returns to base count (2 groups)", "[node_tree_mirror]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    auto* header = getHeader();
    uint32_t baseCount = header->node_count.load(std::memory_order_acquire);

    // Create several synths
    for (int i = 0; i < 5; i++) {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)(2000 + i) << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    syncBarrier(fx, 108);

    uint32_t withSynths = header->node_count.load(std::memory_order_acquire);
    REQUIRE(withSynths > baseCount);

    // Free all synths
    for (int i = 0; i < 5; i++) {
        fx.send(osc_test::message("/n_free", 2000 + i));
    }
    syncBarrier(fx, 109);

    uint32_t afterFree = header->node_count.load(std::memory_order_acquire);
    CHECK(afterFree == baseCount);
}

TEST_CASE("def_name is \"group\" for group entries", "[node_tree_mirror]") {
    EngineFixture fx;

    // Check root group
    const NodeEntry* root = findNode(0);
    REQUIRE(root != nullptr);
    CHECK(std::string(root->def_name) == "group");

    // Check default group
    const NodeEntry* defGroup = findNode(1);
    REQUIRE(defGroup != nullptr);
    CHECK(std::string(defGroup->def_name) == "group");

    // Create a new group and check it too
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    syncBarrier(fx, 110);

    const NodeEntry* g100 = findNode(100);
    REQUIRE(g100 != nullptr);
    CHECK(std::string(g100->def_name) == "group");

    fx.send(osc_test::message("/n_free", 100));
}
