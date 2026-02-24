/**
 * remark-typedoc-summary
 *
 * A remark plugin that generates summary tables for TypeDoc-generated markdown.
 * Inserts a table of members (methods + accessors) after each class/interface h3 heading.
 * Also inlines related interfaces into class sections and enhances tables.
 */

/**
 * Extract plain text from a node tree (handles text, inlineCode, link children etc.)
 */
function extractText(node) {
  if (node.type === 'text' || node.type === 'inlineCode') return node.value;
  if (node.children) return node.children.map(extractText).join('');
  return '';
}

/**
 * Build a markdown table AST node from rows of [name, description, anchor].
 */
function buildTable(rows) {
  const headerRow = {
    type: 'tableRow',
    children: [
      { type: 'tableCell', children: [{ type: 'text', value: 'Member' }] },
      { type: 'tableCell', children: [{ type: 'text', value: 'Description' }] },
    ],
  };

  const dataRows = rows.map(({ name, description, anchor }) => ({
    type: 'tableRow',
    children: [
      {
        type: 'tableCell',
        children: [
          {
            type: 'link',
            url: `#${anchor}`,
            children: [{ type: 'inlineCode', value: name }],
          },
        ],
      },
      {
        type: 'tableCell',
        children: [{ type: 'text', value: description }],
      },
    ],
  }));

  return {
    type: 'table',
    align: [null, null],
    children: [headerRow, ...dataRows],
  };
}

/**
 * Slugify a heading the same way typedoc-plugin-markdown does.
 * Lowercases, strips non-alphanumeric (keeping hyphens), collapses spaces to hyphens.
 */
function slugify(text) {
  return text
    .toLowerCase()
    .replace(/[^a-z0-9 -]/g, '')
    .replace(/\s+/g, '-')
    .replace(/-+/g, '-')
    .trim();
}

/**
 * Find all heading indices at a given depth that are children of the heading at parentIdx.
 * "Children" means they appear after parentIdx and before the next heading at parentDepth or less.
 */
function findChildHeadings(children, parentIdx, parentDepth, childDepth) {
  const results = [];
  for (let i = parentIdx + 1; i < children.length; i++) {
    const node = children[i];
    if (node.type === 'heading' && node.depth <= parentDepth) break;
    if (node.type === 'heading' && node.depth === childDepth) {
      results.push(i);
    }
  }
  return results;
}

/**
 * Extract the description for a member (h5).
 *
 * The description is a paragraph that directly follows a blockquote (the
 * signature line). For methods with multiple Call Signatures, we take the
 * LAST description found — TypeDoc lists specific overloads first and the
 * general catch-all last. Paragraphs under h6 "Returns"/"Deprecated" etc.
 * are skipped since they are return types or deprecation notices.
 */
function extractDescription(children, h5Idx) {
  let afterBlockquote = false;
  let result = '';
  for (let i = h5Idx + 1; i < children.length; i++) {
    const node = children[i];
    // Stop at next heading of same or higher level
    if (node.type === 'heading' && node.depth <= 5) break;
    // Blockquote = signature line; the next paragraph is the description
    if (node.type === 'blockquote') {
      afterBlockquote = true;
      continue;
    }
    // h6 sections (Returns, Parameters, Deprecated, Call Signature) reset the flag
    if (node.type === 'heading' && node.depth === 6) {
      afterBlockquote = false;
      continue;
    }
    // A paragraph right after a blockquote is the description
    if (node.type === 'paragraph' && afterBlockquote) {
      // Collapse newlines to spaces for clean table cells
      const text = extractText(node).replace(/\n/g, ' ');
      // Take first sentence, protecting abbreviations like "e.g." and "i.e."
      const safe = text.replace(/\b(e\.g|i\.e)\./g, '$1\x00');
      const sentenceEnd = safe.match(/\.\s/);
      result = sentenceEnd
        ? text.slice(0, sentenceEnd.index + 1)
        : text;
      afterBlockquote = false;
      continue;
    }
    // Anything else (table, code, list) resets
    afterBlockquote = false;
  }
  return result;
}

/**
 * Configuration for interface/type-alias inlining.
 */
const INLINES = [
  {
    source: 'SuperSonicEventMap',
    sourceSection: 'Interfaces',
    targetClass: 'SuperSonic',
    insertAfter: { type: 'method', name: 'removeAllListeners()' },
    heading: 'Event Types',
    transform: 'eventMap',
  },
  {
    source: 'SuperSonicOptions',
    sourceSection: 'Interfaces',
    targetClass: 'SuperSonic',
    insertAfter: { type: 'section', name: 'Constructors' },
    heading: 'Constructor Options',
  },
  {
    source: 'ScsynthOptions',
    sourceSection: 'Interfaces',
    targetClass: 'SuperSonic',
    insertAfter: { type: 'inline', name: 'Constructor Options' },
    heading: 'Server Options',
  },
  {
    source: 'OscArg',
    sourceSection: 'Type Aliases',
    targetClass: 'SuperSonic',
    insertAfter: { type: 'method', name: 'send()' },
    heading: 'OSC Argument Types',
    transform: 'typeAlias',
  },
];

/**
 * Category mapping for grouped quick reference tables (Phase 4).
 */
const CATEGORY_MAP = {
  Core: [
    'init()', 'shutdown()', 'destroy()', 'recover()', 'suspend()', 'resume()',
    'reload()', 'reset()', 'send()', 'sendOSC()', 'sync()', 'purge()',
    'cancelAll()', 'cancelSession()', 'cancelSessionTag()', 'cancelTag()',
  ],
  'Asset Loading': [
    'loadSynthDef()', 'loadSynthDefs()', 'loadSample()', 'sampleInfo()',
  ],
  Events: ['on()', 'off()', 'once()', 'removeAllListeners()'],
  'Node Tree': ['getTree()', 'getRawTree()', 'getSnapshot()'],
  Metrics: ['getMetrics()', 'getMetricsArray()', 'getMetricsSchema()'],
  Properties: [
    'initialized', 'initializing', 'audioContext', 'node',
    'loadedSynthDefs', 'bootStats',
  ],
  Advanced: [
    'getInfo()', 'createOscChannel()', 'startCapture()', 'stopCapture()',
    'getCaptureFrames()', 'isCaptureEnabled()', 'getMaxCaptureDuration()',
    'setClockOffset()',
  ],
};

/**
 * Build a grouped set of tables for the SuperSonic class.
 */
function buildGroupedTables(rows) {
  // Build a lookup: name → row data
  const lookup = new Map();
  for (const row of rows) lookup.set(row.name, row);

  const nodes = [];
  const placed = new Set();

  for (const [category, members] of Object.entries(CATEGORY_MAP)) {
    const catRows = [];
    for (const member of members) {
      const row = lookup.get(member);
      if (row) {
        catRows.push(row);
        placed.add(member);
      }
    }
    if (catRows.length === 0) continue;

    // Bold category label
    nodes.push({
      type: 'paragraph',
      children: [{ type: 'strong', children: [{ type: 'text', value: category }] }],
    });

    nodes.push(buildTable(catRows));
  }

  // Anything not placed goes into Advanced
  const extras = rows.filter((r) => !placed.has(r.name));
  if (extras.length > 0) {
    nodes.push({
      type: 'paragraph',
      children: [{ type: 'strong', children: [{ type: 'text', value: 'Advanced' }] }],
    });
    nodes.push(buildTable(extras));
  }

  return nodes;
}

/**
 * Constructor Options "Required" column mapping (Phase 5).
 */
const REQUIRED_MAP = { baseURL: 'Yes*' };

/**
 * Server Options default/range mapping (Phase 5).
 */
const SERVER_OPTIONS_MAP = {
  bufLength: { default: '128', range: '128 (fixed)' },
  loadGraphDefs: { default: '0', range: '0–1' },
  maxGraphDefs: { default: '1024', range: '1+' },
  maxNodes: { default: '1024', range: '1+' },
  maxWireBufs: { default: '64', range: '1+' },
  memoryLocking: { default: 'false', range: '—' },
  numAudioBusChannels: { default: '128', range: '1+' },
  numBuffers: { default: '1024', range: '1–65535' },
  numControlBusChannels: { default: '4096', range: '1+' },
  numInputBusChannels: { default: '2', range: '0+' },
  numOutputBusChannels: { default: '2', range: '1–128' },
  numRGens: { default: '64', range: '1+' },
  preferredSampleRate: { default: '0', range: '0, 8000–384000' },
  realTime: { default: 'false', range: '—' },
  realTimeMemorySize: { default: '8192', range: '1+' },
  verbosity: { default: '0', range: '0–4' },
};

/**
 * Extract the property name from a table cell, stripping HTML anchor tags.
 */
function extractPropertyName(cell) {
  const text = extractText(cell);
  // Strip HTML anchor tags that TypeDoc adds, e.g. <a id="foo"></a>
  return text.replace(/<a[^>]*>.*?<\/a>\s*/g, '').replace(/\?$/, '').trim();
}

/**
 * Add a column to a table. headerText is the new column header.
 * valueFn(row, rowIdx) returns the cell text for each data row.
 */
function addTableColumn(table, headerText, valueFn) {
  for (let i = 0; i < table.children.length; i++) {
    const row = table.children[i];
    if (i === 0) {
      // Header row
      row.children.push({
        type: 'tableCell',
        children: [{ type: 'text', value: headerText }],
      });
    } else {
      const value = valueFn(row, i);
      row.children.push({
        type: 'tableCell',
        children: [{ type: 'text', value: value || '' }],
      });
    }
  }
  if (table.align) table.align.push(null);
}

/**
 * Find a heading by depth and text within a range. Returns index or -1.
 */
function findHeading(children, depth, name, start = 0, end = children.length) {
  for (let i = start; i < end; i++) {
    const n = children[i];
    if (n.type === 'heading' && n.depth === depth && extractText(n) === name) return i;
  }
  return -1;
}

/**
 * Find the end of a section (exclusive) — the next heading at boundaryDepth or shallower.
 */
function findSectionEnd(children, idx, boundaryDepth) {
  for (let i = idx + 1; i < children.length; i++) {
    if (children[i].type === 'heading' && children[i].depth <= boundaryDepth) return i;
  }
  return children.length;
}

/**
 * Extract the Properties table from an interface section.
 * Looks for the h4 "Properties" heading and returns the table node after it.
 */
function extractInterfaceTable(children, h3Idx) {
  const end = findSectionEnd(children, h3Idx, 3);
  for (let i = h3Idx + 1; i < end; i++) {
    const n = children[i];
    if (n.type === 'heading' && n.depth === 4 && extractText(n) === 'Properties') {
      // The table should be right after this heading
      for (let j = i + 1; j < end; j++) {
        if (children[j].type === 'table') return children[j];
      }
    }
  }
  return null;
}

/**
 * Transform a 3-column EventMap table (Property | Type | Description) into
 * a 2-column table (Event | Description), dropping the Type column.
 */
function transformEventMap(table) {
  const newRows = table.children.map((row, idx) => {
    const cells = row.children;
    if (idx === 0) {
      // Header row: rename Property → Event, keep Description (skip Type)
      return {
        type: 'tableRow',
        children: [
          { type: 'tableCell', children: [{ type: 'text', value: 'Event' }] },
          cells.length > 2 ? cells[2] : cells[1],
        ],
      };
    }
    // Data rows: take first cell (property name) and last cell (description)
    return {
      type: 'tableRow',
      children: [cells[0], cells.length > 2 ? cells[2] : cells[1]],
    };
  });
  return { type: 'table', align: [null, null], children: newRows };
}

/**
 * Extract content for a type alias section.
 * Collects description paragraphs, lists, and code blocks (skips initial blockquote).
 */
function extractTypeAliasContent(children, h3Idx) {
  const end = findSectionEnd(children, h3Idx, 3);
  const nodes = [];
  for (let i = h3Idx + 1; i < end; i++) {
    const n = children[i];
    // Skip blockquotes (type signature) and thematic breaks
    if (n.type === 'blockquote' || n.type === 'thematicBreak') continue;
    // Skip h4 "Example" headings — take their content but not the heading itself
    if (n.type === 'heading' && n.depth === 4) continue;
    // Take paragraphs, lists, and code blocks
    if (n.type === 'paragraph' || n.type === 'list' || n.type === 'code') {
      nodes.push(n);
    }
  }
  return nodes;
}

/**
 * Find the insertion index within a target class based on insertAfter config.
 * Returns the index where new nodes should be spliced in.
 */
const INSERT_DEPTH = { method: 5, section: 4, inline: 4 };

function findInsertionPoint(children, classIdx, insertAfter) {
  const classEnd = findSectionEnd(children, classIdx, 3);
  const depth = INSERT_DEPTH[insertAfter.type];
  const idx = findHeading(children, depth, insertAfter.name, classIdx + 1, classEnd);
  if (idx === -1) return -1;
  const end = findSectionEnd(children, idx, depth);
  return Math.min(end, classEnd);
}

/**
 * Phase 2: Inline interface/type-alias content into target class sections,
 * then remove the original source sections.
 */
function inlineSections(children) {
  const removals = []; // { h3Idx } — source sections to remove after all insertions

  for (const cfg of INLINES) {
    // 1. Find source h3 under the specified h2 section
    const h2Idx = findHeading(children, 2, cfg.sourceSection);
    if (h2Idx === -1) continue;
    const h2End = findSectionEnd(children, h2Idx, 2);
    const h3Idx = findHeading(children, 3, cfg.source, h2Idx + 1, h2End);
    if (h3Idx === -1) continue;

    // 2. Extract content
    let contentNodes;
    if (cfg.transform === 'typeAlias') {
      contentNodes = extractTypeAliasContent(children, h3Idx);
    } else {
      const table = extractInterfaceTable(children, h3Idx);
      if (!table) continue;
      contentNodes = cfg.transform === 'eventMap' ? [transformEventMap(table)] : [table];
    }

    if (contentNodes.length === 0) continue;

    // 3. Find insertion point in target class
    const classIdx = findHeading(children, 3, cfg.targetClass);
    if (classIdx === -1) continue;
    const insertIdx = findInsertionPoint(children, classIdx, cfg.insertAfter);
    if (insertIdx === -1) continue;

    // 4. Build h4 heading node
    const heading = {
      type: 'heading',
      depth: 4,
      children: [{ type: 'text', value: cfg.heading }],
    };

    // 5. Insert heading + content
    children.splice(insertIdx, 0, heading, ...contentNodes);

    // 6. Mark source for removal (re-find since indices shifted)
    removals.push(cfg.source);
  }

  // Remove source sections in reverse index order
  const toRemove = [];
  for (const sourceName of removals) {
    // Re-find each time since we process in order
    for (let i = 0; i < children.length; i++) {
      const n = children[i];
      if (n.type === 'heading' && n.depth === 3 && extractText(n) === sourceName) {
        const end = findSectionEnd(children, i, 3);
        // Include preceding thematicBreak if present
        const start = i > 0 && children[i - 1].type === 'thematicBreak' ? i - 1 : i;
        toRemove.push({ start, end });
        break;
      }
    }
  }

  // Sort by start descending and splice
  toRemove.sort((a, b) => b.start - a.start);
  for (const { start, end } of toRemove) {
    children.splice(start, end - start);
  }
}

/**
 * Find the first table inside an h4 section of a class. Returns { table, tableIdx } or null.
 */
function findSectionTable(children, className, sectionName) {
  const classIdx = findHeading(children, 3, className);
  if (classIdx === -1) return null;
  const classEnd = findSectionEnd(children, classIdx, 3);
  const h4Idx = findHeading(children, 4, sectionName, classIdx + 1, classEnd);
  if (h4Idx === -1) return null;
  const h4End = findSectionEnd(children, h4Idx, 4);
  for (let i = h4Idx + 1; i < h4End; i++) {
    if (children[i].type === 'table') return { table: children[i], tableIdx: i };
  }
  return null;
}

/**
 * Phase 5: Enhance Constructor Options and Server Options tables.
 */
function enhanceTables(children) {
  // Constructor Options: add Required column + footnote
  const ctor = findSectionTable(children, 'SuperSonic', 'Constructor Options');
  if (ctor) {
    addTableColumn(ctor.table, 'Required', (row) => {
      const propName = extractPropertyName(row.children[0]);
      return REQUIRED_MAP[propName] || '';
    });
    children.splice(ctor.tableIdx + 1, 0, {
      type: 'paragraph',
      children: [
        {
          type: 'emphasis',
          children: [
            { type: 'text', value: 'Required unless both ' },
            { type: 'inlineCode', value: 'coreBaseURL' },
            { type: 'text', value: '/' },
            { type: 'inlineCode', value: 'workerBaseURL' },
            { type: 'text', value: ' and ' },
            { type: 'inlineCode', value: 'wasmBaseURL' },
            { type: 'text', value: ' are provided.' },
          ],
        },
      ],
    });
  }

  // Server Options: add Default and Range columns
  const srv = findSectionTable(children, 'SuperSonic', 'Server Options');
  if (srv) {
    addTableColumn(srv.table, 'Default', (row) => {
      const propName = extractPropertyName(row.children[0]);
      return SERVER_OPTIONS_MAP[propName]?.default || '';
    });
    addTableColumn(srv.table, 'Range', (row) => {
      const propName = extractPropertyName(row.children[0]);
      return SERVER_OPTIONS_MAP[propName]?.range || '';
    });
  }
}

export default function remarkTypedocSummary() {
  return (tree) => {
    const children = tree.children;

    // Rename the h1 to "API Reference"
    const h1 = children.find((n) => n.type === 'heading' && n.depth === 1);
    if (h1) h1.children = [{ type: 'text', value: 'API Reference' }];

    // Phase 1+4: Build and insert summary tables (grouped for SuperSonic)
    const inserts = [];

    for (let i = 0; i < children.length; i++) {
      const node = children[i];
      if (node.type !== 'heading' || node.depth !== 3) continue;

      const className = extractText(node);
      const rows = [];

      // Find h4 children (Methods, Accessors, Properties, etc.)
      const h4Indices = findChildHeadings(children, i, 3, 4);

      for (const h4Idx of h4Indices) {
        const h4Text = extractText(children[h4Idx]);
        if (h4Text !== 'Methods' && h4Text !== 'Accessors') continue;

        // Find h5 children under this h4
        const h5Indices = findChildHeadings(children, h4Idx, 4, 5);

        for (const h5Idx of h5Indices) {
          const name = extractText(children[h5Idx]);
          const description = extractDescription(children, h5Idx);
          const anchor = slugify(name);
          rows.push({ name, description, anchor });
        }
      }

      if (rows.length === 0) continue;

      // Find insert position: after the class description paragraph(s),
      // before the first h4 (Example, Constructors, etc.)
      let insertIdx = i + 1;
      for (let j = i + 1; j < children.length; j++) {
        const n = children[j];
        if (n.type === 'heading' && n.depth <= 3) break;
        if (n.type === 'heading' && n.depth === 4) {
          insertIdx = j;
          break;
        }
        insertIdx = j + 1;
      }

      if (className === 'SuperSonic') {
        inserts.push({ index: insertIdx, nodes: buildGroupedTables(rows) });
      } else {
        inserts.push({ index: insertIdx, nodes: [buildTable(rows)] });
      }
    }

    // Insert tables in reverse order to preserve indices
    for (let k = inserts.length - 1; k >= 0; k--) {
      children.splice(inserts[k].index, 0, ...inserts[k].nodes);
    }

    // Phase 2: Inline interface/type-alias sections
    inlineSections(children);

    // Phase 5: Enhance tables
    enhanceTables(children);
  };
}
