import { test, expect } from './fixtures.mjs';

test.describe('Schema Validation', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('getMetricsSchema() returns valid schema with metrics, layout, and sentinels', async ({ page }) => {
    const result = await page.evaluate(() => {
      const schema = window.SuperSonic.getMetricsSchema();
      const metrics = schema.metrics;
      const layout = schema.layout;
      const sentinels = schema.sentinels;

      const metricKeys = Object.keys(metrics);

      // Check each metric has required fields
      const invalidMetrics = [];
      for (const [key, def] of Object.entries(metrics)) {
        if (typeof def.offset !== 'number') invalidMetrics.push(`${key}: missing offset`);
        if (!def.type) invalidMetrics.push(`${key}: missing type`);
        if (!def.description) invalidMetrics.push(`${key}: missing description`);
      }

      // Check layout exists and has panels
      const hasPanels = Array.isArray(layout?.panels) && layout.panels.length > 0;

      // Check sentinels
      const hasHeadroomUnset = sentinels?.HEADROOM_UNSET === 0xFFFFFFFF;

      return {
        metricCount: metricKeys.length,
        invalidMetrics,
        hasPanels,
        panelCount: layout?.panels?.length ?? 0,
        hasHeadroomUnset,
      };
    });

    expect(result.metricCount).toBeGreaterThan(10);
    expect(result.invalidMetrics).toEqual([]);
    expect(result.hasPanels).toBe(true);
    expect(result.panelCount).toBeGreaterThan(5);
    expect(result.hasHeadroomUnset).toBe(true);
  });

  test('layout panel keys all reference valid metrics', async ({ page }) => {
    const result = await page.evaluate(() => {
      const schema = window.SuperSonic.getMetricsSchema();
      const metricKeys = new Set(Object.keys(schema.metrics));
      const missingKeys = [];

      for (const panel of schema.layout.panels) {
        for (const row of panel.rows) {
          if (row.type === 'bar') {
            // Bar rows reference usedKey, peakKey, capacityKey
            for (const k of [row.usedKey, row.peakKey, row.capacityKey]) {
              if (k && !metricKeys.has(k)) missingKeys.push(`${panel.title}: ${k}`);
            }
          } else if (row.cells) {
            for (const cell of row.cells) {
              if (cell.key && !metricKeys.has(cell.key)) {
                missingKeys.push(`${panel.title}/${row.label}: ${cell.key}`);
              }
            }
          }
        }
      }

      return { missingKeys };
    });

    expect(result.missingKeys).toEqual([]);
  });

  test('metric offsets are unique and within merged array bounds', async ({ page }) => {
    const result = await page.evaluate(() => {
      const schema = window.SuperSonic.getMetricsSchema();
      const offsets = new Map();
      const outOfBounds = [];

      for (const [key, def] of Object.entries(schema.metrics)) {
        if (offsets.has(def.offset)) {
          // Allow shared offsets only if explicitly expected (none currently)
          outOfBounds.push(`Duplicate offset ${def.offset}: ${offsets.get(def.offset)} and ${key}`);
        }
        offsets.set(def.offset, key);
        if (def.offset < 0 || def.offset >= 64) {
          outOfBounds.push(`${key}: offset ${def.offset} out of bounds [0, 63]`);
        }
      }

      return { outOfBounds, offsetCount: offsets.size };
    });

    expect(result.outOfBounds).toEqual([]);
    expect(result.offsetCount).toBeGreaterThan(10);
  });

  test('getTreeSchema() returns valid hierarchical schema', async ({ page }) => {
    const schema = await page.evaluate(() => {
      return window.SuperSonic.getTreeSchema();
    });

    // Schema should have expected top-level fields
    expect(schema).toHaveProperty('nodeCount');
    expect(schema).toHaveProperty('version');
    expect(schema).toHaveProperty('droppedCount');
    expect(schema).toHaveProperty('root');

    // Root should describe node structure
    expect(schema.root).toHaveProperty('schema');
    expect(schema.root.schema).toHaveProperty('id');
    expect(schema.root.schema).toHaveProperty('type');
    expect(schema.root.schema).toHaveProperty('defName');
    expect(schema.root.schema).toHaveProperty('children');
  });

  test('getRawTreeSchema() returns valid flat schema', async ({ page }) => {
    const schema = await page.evaluate(() => {
      return window.SuperSonic.getRawTreeSchema();
    });

    // Schema should have expected top-level fields
    expect(schema).toHaveProperty('nodeCount');
    expect(schema).toHaveProperty('version');
    expect(schema).toHaveProperty('droppedCount');
    expect(schema).toHaveProperty('nodes');

    // Nodes should have itemSchema with all raw fields
    expect(schema.nodes).toHaveProperty('itemSchema');
    expect(schema.nodes.itemSchema).toHaveProperty('id');
    expect(schema.nodes.itemSchema).toHaveProperty('parentId');
    expect(schema.nodes.itemSchema).toHaveProperty('isGroup');
    expect(schema.nodes.itemSchema).toHaveProperty('prevId');
    expect(schema.nodes.itemSchema).toHaveProperty('nextId');
    expect(schema.nodes.itemSchema).toHaveProperty('headId');
    expect(schema.nodes.itemSchema).toHaveProperty('defName');
  });

  test('getMetrics() keys match schema.metrics keys for current mode', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const metrics = sonic.getMetrics();
      const schema = window.SuperSonic.getMetricsSchema();
      const mode = metrics.mode;

      await sonic.destroy();

      // The getMetrics() object has a different shape from the merged array —
      // it includes derived objects (inBufferUsed, outBufferUsed, debugBufferUsed)
      // and excludes raw byte metrics that are wrapped into those objects.
      // Also excludes: inBufferCapacity, outBufferCapacity, debugBufferCapacity (in derived objects)
      // and some array-only fields (inBufferUsedBytes etc. are deleted in gatherMetrics).

      // Schema.metrics keys that DON'T appear in getMetrics() because they're array-only:
      const arrayOnlyKeys = new Set([
        'inBufferUsedBytes', 'outBufferUsedBytes', 'debugBufferUsedBytes',
        'inBufferPeakBytes', 'outBufferPeakBytes', 'debugBufferPeakBytes',
        'inBufferCapacity', 'outBufferCapacity', 'debugBufferCapacity',
      ]);

      // Keys in getMetrics() that aren't in schema.metrics (derived objects):
      const derivedKeys = new Set([
        'inBufferUsed', 'outBufferUsed', 'debugBufferUsed', 'ntpStartTime',
      ]);

      const schemaKeys = Object.keys(schema.metrics)
        .filter(k => !arrayOnlyKeys.has(k));

      const actualKeys = Object.keys(metrics)
        .filter(k => !derivedKeys.has(k));

      return {
        mode,
        missingFromMetrics: schemaKeys.filter(k => !actualKeys.includes(k)),
        extraInMetrics: actualKeys.filter(k => !schemaKeys.includes(k)),
      };
    }, sonicConfig);

    expect(result.missingFromMetrics).toEqual([]);
    expect(result.extraInMetrics).toEqual([]);
  });

  test('getTree() returns hierarchical structure matching schema', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create a synth to have something in the tree
      await sonic.loadSynthDef('sonic-pi-beep');
      await sonic.send('/s_new', 'sonic-pi-beep', 1001, 0, 0);
      await sonic.sync();

      const tree = sonic.getTree();
      const schema = window.SuperSonic.getTreeSchema();

      await sonic.destroy();

      // Check top-level keys
      const expectedTopKeys = Object.keys(schema);
      const actualTopKeys = Object.keys(tree);

      // Check root node structure
      const nodeSchema = schema.root.schema;
      const expectedNodeKeys = Object.keys(nodeSchema);

      // Recursively check all nodes have correct keys
      const checkNode = (node) => {
        const actualKeys = Object.keys(node);
        const missing = expectedNodeKeys.filter(k => !actualKeys.includes(k));
        if (missing.length > 0) {
          return { valid: false, error: `Node ${node.id} missing keys: ${missing.join(', ')}` };
        }
        for (const child of node.children) {
          const childResult = checkNode(child);
          if (!childResult.valid) return childResult;
        }
        return { valid: true };
      };

      const nodeCheck = tree.root ? checkNode(tree.root) : { valid: true };

      return {
        expectedTopKeys: expectedTopKeys.sort(),
        actualTopKeys: actualTopKeys.sort(),
        missingTopLevel: expectedTopKeys.filter(k => !actualTopKeys.includes(k)),
        nodeCount: tree.nodeCount,
        hasRoot: !!tree.root,
        rootType: tree.root?.type,
        rootId: tree.root?.id,
        nodeSchemaValid: nodeCheck.valid,
        nodeSchemaError: nodeCheck.error || null
      };
    }, sonicConfig);

    // All expected top-level keys should be present
    expect(result.missingTopLevel).toEqual([]);

    // Should have root node
    expect(result.hasRoot).toBe(true);
    expect(result.rootId).toBe(0);
    expect(result.rootType).toBe('group');

    // Node schema should be valid
    expect(result.nodeSchemaValid).toBe(true);
    if (result.nodeSchemaError) {
      console.error(result.nodeSchemaError);
    }
  });

  test('getTree() builds correct hierarchical structure from complex node tree', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');

      // Build a complex hierarchy:
      // Root (0)
      // ├── Group 100
      // │   ├── Synth 1000 (beep)
      // │   ├── Group 101
      // │   │   ├── Synth 1001 (beep)
      // │   │   └── Synth 1002 (beep)
      // │   └── Synth 1003 (beep)
      // ├── Group 200 (empty)
      // └── Synth 2000 (beep)

      await sonic.send('/g_new', 100, 0, 0);  // Group 100 at head of root
      await sonic.send('/g_new', 200, 1, 0);  // Group 200 at tail of root
      await sonic.send('/s_new', 'sonic-pi-beep', 2000, 1, 0, 'release', 60);  // Synth at tail of root

      await sonic.send('/s_new', 'sonic-pi-beep', 1000, 0, 100, 'release', 60);  // Synth at head of 100
      await sonic.send('/g_new', 101, 1, 100);  // Group 101 at tail of 100
      await sonic.send('/s_new', 'sonic-pi-beep', 1003, 1, 100, 'release', 60);  // Synth at tail of 100

      await sonic.send('/s_new', 'sonic-pi-beep', 1001, 0, 101, 'release', 60);  // Synth at head of 101
      await sonic.send('/s_new', 'sonic-pi-beep', 1002, 1, 101, 'release', 60);  // Synth at tail of 101

      await sonic.sync();

      const tree = sonic.getTree();

      // Clean up
      await sonic.send('/n_free', 100);  // Frees 100 and all children
      await sonic.send('/n_free', 200);
      await sonic.send('/n_free', 2000);

      // Helper to extract structure for comparison
      const extractStructure = (node) => ({
        id: node.id,
        type: node.type,
        defName: node.defName,
        children: node.children.map(extractStructure)
      });

      return {
        nodeCount: tree.nodeCount,
        version: tree.version,
        droppedCount: tree.droppedCount,
        structure: tree.root ? extractStructure(tree.root) : null
      };
    }, sonicConfig);

    expect(result.nodeCount).toBe(9);  // root + 3 groups + 5 synths
    expect(result.droppedCount).toBe(0);

    // Verify the full hierarchical structure
    const expected = {
      id: 0,
      type: 'group',
      defName: 'group',
      children: [
        {
          id: 100,
          type: 'group',
          defName: 'group',
          children: [
            { id: 1000, type: 'synth', defName: 'sonic-pi-beep', children: [] },
            {
              id: 101,
              type: 'group',
              defName: 'group',
              children: [
                { id: 1001, type: 'synth', defName: 'sonic-pi-beep', children: [] },
                { id: 1002, type: 'synth', defName: 'sonic-pi-beep', children: [] },
              ]
            },
            { id: 1003, type: 'synth', defName: 'sonic-pi-beep', children: [] },
          ]
        },
        {
          id: 200,
          type: 'group',
          defName: 'group',
          children: []
        },
        { id: 2000, type: 'synth', defName: 'sonic-pi-beep', children: [] },
      ]
    };

    expect(result.structure).toEqual(expected);
  });

  test('getRawTree() returns flat structure matching schema', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create a synth to have something in the tree
      await sonic.loadSynthDef('sonic-pi-beep');
      await sonic.send('/s_new', 'sonic-pi-beep', 1001, 0, 0);
      await sonic.sync();

      const tree = sonic.getRawTree();
      const schema = window.SuperSonic.getRawTreeSchema();

      await sonic.destroy();

      // Check top-level keys
      const expectedTopKeys = Object.keys(schema);
      const actualTopKeys = Object.keys(tree);

      // Check node structure if we have nodes
      let nodeSchemaValid = true;
      let nodeSchemaError = null;
      if (tree.nodes && tree.nodes.length > 0) {
        const nodeSchema = schema.nodes.itemSchema;
        const expectedNodeKeys = Object.keys(nodeSchema);
        for (const node of tree.nodes) {
          const actualNodeKeys = Object.keys(node);
          const missing = expectedNodeKeys.filter(k => !actualNodeKeys.includes(k));
          if (missing.length > 0) {
            nodeSchemaValid = false;
            nodeSchemaError = `Node missing keys: ${missing.join(', ')}`;
            break;
          }
        }
      }

      return {
        expectedTopKeys: expectedTopKeys.sort(),
        actualTopKeys: actualTopKeys.sort(),
        missingTopLevel: expectedTopKeys.filter(k => !actualTopKeys.includes(k)),
        nodeCount: tree.nodeCount,
        nodesIsArray: Array.isArray(tree.nodes),
        nodeSchemaValid,
        nodeSchemaError
      };
    }, sonicConfig);

    // All expected top-level keys should be present
    expect(result.missingTopLevel).toEqual([]);

    // Nodes should be an array
    expect(result.nodesIsArray).toBe(true);

    // Node schema should be valid
    expect(result.nodeSchemaValid).toBe(true);
    if (result.nodeSchemaError) {
      console.error(result.nodeSchemaError);
    }
  });

  test('counter metrics are populated after activity', async ({ page, sonicConfig }) => {
    // This test verifies that counter metrics have non-zero values after
    // sending/receiving messages. This catches bugs where metrics are declared
    // but not implemented.
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const schema = window.SuperSonic.getMetricsSchema();

      // Find counter metrics (counters should increment with activity, gauges might legitimately be 0)
      const counterMetrics = Object.entries(schema.metrics)
        .filter(([key, def]) => def.type === 'counter')
        .map(([key]) => key);

      // Get metrics before any activity
      const metricsBefore = sonic.getMetrics();

      // Generate activity: send messages and wait for replies
      // Load a synthdef (generates send + receive)
      await sonic.loadSynthDef('sonic-pi-beep');

      // Send a synth creation message (generates send)
      sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);

      // Send an immediate status query (should bypass prescheduler, generates reply)
      sonic.send('/status');

      // Wait a bit for metrics to update
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();

      await sonic.destroy();

      // Check which counters are still 0 after activity
      const zeroCounters = counterMetrics.filter(key => {
        const value = metricsAfter[key];
        if (value === undefined) return false;
        return value === 0;
      });

      // These specific counters MUST be non-zero after the activity above
      const requiredNonZero = [
        'oscOutMessagesSent',      // We sent messages
        'oscOutBytesSent',         // We sent bytes
        'oscInMessagesReceived', // We received /done and /status.reply
        'oscInBytesReceived',    // We received bytes
      ];

      const failedRequired = requiredNonZero.filter(key => {
        const value = metricsAfter[key];
        return value === undefined || value === 0;
      });

      return {
        mode: config.mode,
        counterMetrics,
        zeroCounters,
        failedRequired,
        relevantMetrics: {
          oscOutMessagesSent: metricsAfter.oscOutMessagesSent,
          oscOutBytesSent: metricsAfter.oscOutBytesSent,
          oscInMessagesReceived: metricsAfter.oscInMessagesReceived,
          oscInBytesReceived: metricsAfter.oscInBytesReceived,
          preschedulerBypassed: metricsAfter.preschedulerBypassed,
        }
      };
    }, sonicConfig);

    // Log for debugging
    console.log(`Mode: ${result.mode}`);
    console.log(`Metrics after activity:`, result.relevantMetrics);
    if (result.zeroCounters.length > 0) {
      console.log(`Warning: These counters are still 0: ${result.zeroCounters.join(', ')}`);
    }

    // Required metrics must be non-zero
    expect(result.failedRequired).toEqual([]);
  });

  test('immediate messages reach scsynth and increment bypass counter', async ({ page, sonicConfig }) => {
    // This test verifies that sendImmediate() actually delivers messages to scsynth.
    // Previously there was a bug where sendImmediate used type 'oscImmediate' but
    // the worklet only handled type 'osc', causing messages to be silently dropped.
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const metricsBefore = sonic.getMetrics();
      const bypassedBefore = metricsBefore.preschedulerBypassed || 0;
      const receivedBefore = metricsBefore.oscInMessagesReceived || 0;

      // Send multiple immediate messages (non-bundled messages bypass prescheduler)
      // /status returns /status.reply, so we can verify the message reached scsynth
      const statusPromises = [];
      for (let i = 0; i < 5; i++) {
        statusPromises.push(new Promise(resolve => {
          const handler = (reply) => {
            if (reply[0] === '/status.reply') {
              sonic.off('in', handler);
              resolve(true);
            }
          };
          sonic.on('in', handler);
          sonic.send('/status');
        }));
      }

      // Wait for all replies (with timeout)
      const timeoutPromise = new Promise(resolve =>
        setTimeout(() => resolve('timeout'), 2000)
      );
      const raceResult = await Promise.race([
        Promise.all(statusPromises),
        timeoutPromise
      ]);

      // Wait for metrics to update
      await new Promise(r => setTimeout(r, 100));

      const metricsAfter = sonic.getMetrics();
      const bypassedAfter = metricsAfter.preschedulerBypassed || 0;
      const receivedAfter = metricsAfter.oscInMessagesReceived || 0;

      await sonic.destroy();

      return {
        mode: config.mode,
        timedOut: raceResult === 'timeout',
        bypassedBefore,
        bypassedAfter,
        bypassedDelta: bypassedAfter - bypassedBefore,
        receivedBefore,
        receivedAfter,
        receivedDelta: receivedAfter - receivedBefore,
      };
    }, sonicConfig);

    console.log(`Mode: ${result.mode}`);
    console.log(`Bypassed: ${result.bypassedBefore} -> ${result.bypassedAfter} (delta: ${result.bypassedDelta})`);
    console.log(`Received: ${result.receivedBefore} -> ${result.receivedAfter} (delta: ${result.receivedDelta})`);

    // Messages should not have timed out waiting for replies
    expect(result.timedOut).toBe(false);

    // We should have received at least 5 /status.reply messages
    expect(result.receivedDelta).toBeGreaterThanOrEqual(5);

    // In postMessage mode, bypass counter should increment for immediate messages
    if (result.mode === 'postMessage') {
      expect(result.bypassedDelta).toBeGreaterThanOrEqual(5);
    }
  });
});
