#!/usr/bin/env node
/**
 * SuperCollider UGen Metadata Parser
 *
 * Parses SC class library (.sc) and help files (.schelp) to generate
 * a comprehensive JSON specification of all UGens.
 *
 * Usage:
 *   node src/ugens/parser.js [sc_source_path] [output_path]
 *
 * Defaults:
 *   sc_source_path: ~/Development/supercollider
 *   output_path: ./src/ugens/metadata.json
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Default paths
const DEFAULT_SC_PATH = path.join(process.env.HOME, 'Development/supercollider');
const DEFAULT_OUTPUT_PATH = path.join(__dirname, 'metadata.json');

const SC_PATH = process.argv[2] || DEFAULT_SC_PATH;
const OUTPUT_PATH = process.argv[3] || DEFAULT_OUTPUT_PATH;

const SC_CLASS_PATH = path.join(SC_PATH, 'SCClassLibrary/Common/Audio');
const SC_HELP_PATH = path.join(SC_PATH, 'HelpSource/Classes');

// Known UGen base classes (to identify what is a UGen)
const UGEN_BASE_CLASSES = new Set([
  'UGen', 'PureUGen', 'MultiOutUGen', 'OutputProxy',
  'Filter', 'PureMultiOutUGen', 'BufInfoUGenBase'
]);

/**
 * Parse a .sc file and extract UGen class definitions
 */
function parseSCFile(filePath) {
  const content = fs.readFileSync(filePath, 'utf-8');
  const ugens = {};

  // Match class definitions: ClassName : ParentClass { ... }
  // We need to handle nested braces properly
  const classRegex = /^(\w+)\s*:\s*(\w+)\s*\{/gm;
  let match;

  while ((match = classRegex.exec(content)) !== null) {
    const className = match[1];
    const parentClass = match[2];
    const startIndex = match.index + match[0].length;

    // Find the matching closing brace
    let braceCount = 1;
    let endIndex = startIndex;
    for (let i = startIndex; i < content.length && braceCount > 0; i++) {
      if (content[i] === '{') braceCount++;
      if (content[i] === '}') braceCount--;
      endIndex = i;
    }

    const classBody = content.substring(startIndex, endIndex);

    // Parse methods (.ar, .kr, .ir)
    const methods = parseClassMethods(classBody);

    // Check for signalRange method
    let signalRange = null;
    const signalRangeMatch = classBody.match(/signalRange\s*\{\s*\^\s*\\(\w+)/);
    if (signalRangeMatch) {
      signalRange = signalRangeMatch[1];
    }

    // Include class if it has methods OR if it's an empty subclass of a known UGen class
    // (empty subclasses inherit parent's methods)
    const hasOwnMethods = methods.ar || methods.kr || methods.ir;
    const isEmptySubclass = classBody.trim() === '' || classBody.trim().match(/^\/\/.*$/);

    if (hasOwnMethods) {
      ugens[className] = {
        name: className,
        parent: parentClass,
        rates: [],
        args: [],
        ...methods,
        signalRange
      };

      if (methods.ar) ugens[className].rates.push('ar');
      if (methods.kr) ugens[className].rates.push('kr');
      if (methods.ir) ugens[className].rates.push('ir');

      // Use the first available method's args as the canonical args
      ugens[className].args = methods.ar?.args || methods.kr?.args || methods.ir?.args || [];
    } else if (isEmptySubclass) {
      // Empty subclass - will inherit from parent
      ugens[className] = {
        name: className,
        parent: parentClass,
        rates: [],
        args: [],
        signalRange,
        _needsInheritance: true
      };
    }
  }

  return ugens;
}

/**
 * Parse class methods to extract .ar, .kr, .ir definitions
 */
function parseClassMethods(classBody) {
  const methods = {};

  // Match *ar, *kr, *ir methods
  const methodRegex = /\*(\w+)\s*\{\s*(?:arg\s+([^;]+);)?/g;
  let match;

  while ((match = methodRegex.exec(classBody)) !== null) {
    const methodName = match[1];
    const argsString = match[2];

    if (['ar', 'kr', 'ir'].includes(methodName)) {
      const args = argsString ? parseArguments(argsString) : [];
      methods[methodName] = { args };
    }
  }

  return methods;
}

/**
 * Parse argument string into structured args array
 * e.g., "freq = 440.0, phase = 0.0, mul = 1.0, add = 0.0"
 */
function parseArguments(argsString) {
  const args = [];

  // Split by comma, but be careful of nested expressions
  const parts = smartSplit(argsString, ',');

  for (const part of parts) {
    const trimmed = part.trim();
    if (!trimmed) continue;

    // Skip mul and add - they're standard madd parameters, not UGen-specific
    if (trimmed.startsWith('mul') || trimmed.startsWith('add')) continue;

    // Parse "name = default" or just "name"
    const eqIndex = trimmed.indexOf('=');
    if (eqIndex !== -1) {
      const name = trimmed.substring(0, eqIndex).trim();
      const defaultStr = trimmed.substring(eqIndex + 1).trim();
      args.push({
        name,
        default: parseDefault(defaultStr)
      });
    } else {
      args.push({
        name: trimmed,
        default: null
      });
    }
  }

  return args;
}

/**
 * Smart split that respects nested parentheses/brackets
 */
function smartSplit(str, delimiter) {
  const parts = [];
  let current = '';
  let depth = 0;

  for (const char of str) {
    if (char === '(' || char === '[' || char === '{') depth++;
    if (char === ')' || char === ']' || char === '}') depth--;

    if (char === delimiter && depth === 0) {
      parts.push(current);
      current = '';
    } else {
      current += char;
    }
  }

  if (current) parts.push(current);
  return parts;
}

/**
 * Parse default value string into appropriate type
 */
function parseDefault(str) {
  str = str.trim();

  // Number
  if (/^-?\d+\.?\d*$/.test(str)) {
    return parseFloat(str);
  }

  // Negative number with space
  if (/^-\s*\d+\.?\d*$/.test(str)) {
    return parseFloat(str.replace(/\s/g, ''));
  }

  // inf
  if (str === 'inf') return 'inf';

  // nil/null
  if (str === 'nil') return null;

  // Symbol (e.g., \audio)
  if (str.startsWith('\\')) return str;

  // Otherwise return as string
  return str;
}

/**
 * Parse a .schelp file and extract documentation
 */
function parseSCHelpFile(filePath) {
  const content = fs.readFileSync(filePath, 'utf-8');
  const doc = {
    summary: null,
    description: null,
    categories: null,
    related: [],
    arguments: {}
  };

  // Parse summary
  const summaryMatch = content.match(/^summary::\s*(.+)$/m);
  if (summaryMatch) {
    doc.summary = summaryMatch[1].trim();
  }

  // Parse categories
  const categoriesMatch = content.match(/^categories::\s*(.+)$/m);
  if (categoriesMatch) {
    doc.categories = categoriesMatch[1].trim();
  }

  // Parse related
  const relatedMatch = content.match(/^related::\s*(.+)$/m);
  if (relatedMatch) {
    const relatedStr = relatedMatch[1];
    // Extract class names from "Classes/SinOsc, Classes/Osc" format
    const related = relatedStr.match(/Classes\/(\w+)/g);
    if (related) {
      doc.related = related.map(r => r.replace('Classes/', ''));
    }
  }

  // Parse description - capture until we hit a section marker
  const descMatch = content.match(/^Description::\s*([\s\S]*?)(?=^(?:classmethods|instancemethods|examples|section|subsection)::)/mi);
  if (descMatch) {
    doc.description = cleanDescription(descMatch[1]);
  }

  // Parse arguments - capture from argument::name until the next section/argument marker
  // We split by argument:: first, then process each section
  const argSections = content.split(/^argument::/gm);
  for (let i = 1; i < argSections.length; i++) {
    const section = argSections[i];
    // First word is the arg name
    const nameMatch = section.match(/^(\w+)/);
    if (!nameMatch) continue;

    const argName = nameMatch[1];
    // Rest is the doc, until next major section marker
    let argDoc = section.substring(nameMatch[0].length);

    // Trim at the first section marker
    const sectionMarkers = ['argument::', 'method::', 'returns::', 'discussion::',
                           'examples::', 'section::', 'subsection::',
                           'classmethods::', 'instancemethods::'];
    for (const marker of sectionMarkers) {
      const idx = argDoc.indexOf('\n' + marker);
      if (idx !== -1) {
        argDoc = argDoc.substring(0, idx);
      }
    }

    doc.arguments[argName] = cleanDescription(argDoc);
  }

  return doc;
}

/**
 * Clean up description text from schelp format
 */
function cleanDescription(text) {
  if (!text) return null;

  return text
    // Remove code:: ... :: blocks but keep content
    .replace(/code::([\s\S]*?)::/g, '$1')
    // Remove link:: ... :: but extract the display text
    .replace(/link::Classes\/(\w+)::/g, '$1')
    .replace(/link::([^:]+)::/g, '$1')
    // Remove emphasis:: ... ::
    .replace(/emphasis::([\s\S]*?)::/g, '$1')
    // Remove strong:: ... ::
    .replace(/strong::([\s\S]*?)::/g, '$1')
    // Remove note:: ... ::
    .replace(/note::([\s\S]*?)::/g, '(Note: $1)')
    // Remove warning:: ... ::
    .replace(/warning::([\s\S]*?)::/g, '(Warning: $1)')
    // Remove list markers
    .replace(/^LIST::$/gm, '')
    .replace(/^::$/gm, '')
    .replace(/^##\s*/gm, '- ')
    // Clean up whitespace
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

/**
 * Recursively find all files with given extension
 */
function findFiles(dir, ext) {
  const files = [];

  if (!fs.existsSync(dir)) {
    console.warn(`Directory not found: ${dir}`);
    return files;
  }

  const entries = fs.readdirSync(dir, { withFileTypes: true });

  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...findFiles(fullPath, ext));
    } else if (entry.name.endsWith(ext)) {
      files.push(fullPath);
    }
  }

  return files;
}

/**
 * Resolve inheritance to get complete arg list and rates
 * Uses recursive lookup to handle multi-level inheritance
 */
function resolveInheritance(ugens) {
  const resolved = {};

  // Copy all ugens first
  for (const [name, ugen] of Object.entries(ugens)) {
    resolved[name] = { ...ugen };
  }

  // Helper to get inherited info from ancestor chain
  function getInheritedInfo(ugenName, visited = new Set()) {
    if (visited.has(ugenName)) return null; // Prevent cycles
    visited.add(ugenName);

    const ugen = resolved[ugenName];
    if (!ugen) return null;

    // Collect what this ugen has
    const hasArgs = ugen.args && ugen.args.length > 0 && !ugen._needsInheritance;
    const hasRates = ugen.rates && ugen.rates.length > 0;

    // If this ugen has its own definition (either args or rates), use it
    if (hasArgs || hasRates) {
      return {
        args: ugen.args || [],
        rates: ugen.rates || [],
        from: ugenName
      };
    }

    // Otherwise, try to inherit from parent
    if (ugen.parent) {
      return getInheritedInfo(ugen.parent, visited);
    }

    return null;
  }

  // Resolve inheritance for all ugens that need it
  for (const [name, ugen] of Object.entries(resolved)) {
    const needsInheritance = ugen._needsInheritance ||
                             (ugen.rates.length === 0 && ugen.parent) ||
                             (ugen.args.length === 0 && ugen.parent);

    if (needsInheritance) {
      const inherited = getInheritedInfo(ugen.parent);
      if (inherited) {
        // Inherit args if we don't have any
        if (resolved[name].args.length === 0 && inherited.args.length > 0) {
          resolved[name].args = [...inherited.args];
          resolved[name].inheritedFrom = inherited.from;
        }

        // Inherit rates if we don't have any
        if (resolved[name].rates.length === 0 && inherited.rates.length > 0) {
          resolved[name].rates = [...inherited.rates];
        }
      }
    }

    // Clean up internal flag
    delete resolved[name]._needsInheritance;
  }

  return resolved;
}

/**
 * Main function
 */
function main() {
  console.log(`Parsing SuperCollider sources from: ${SC_PATH}`);
  console.log(`Output will be written to: ${OUTPUT_PATH}`);

  // Parse all .sc files
  console.log('\nParsing .sc files...');
  const scFiles = findFiles(SC_CLASS_PATH, '.sc');
  console.log(`Found ${scFiles.length} .sc files`);

  let allUgens = {};
  for (const file of scFiles) {
    const ugens = parseSCFile(file);
    allUgens = { ...allUgens, ...ugens };
  }
  console.log(`Extracted ${Object.keys(allUgens).length} UGen definitions`);

  // Resolve inheritance
  allUgens = resolveInheritance(allUgens);

  // Parse all .schelp files
  console.log('\nParsing .schelp files...');
  const helpFiles = findFiles(SC_HELP_PATH, '.schelp');
  console.log(`Found ${helpFiles.length} .schelp files`);

  const helpDocs = {};
  for (const file of helpFiles) {
    const className = path.basename(file, '.schelp');
    try {
      helpDocs[className] = parseSCHelpFile(file);
    } catch (e) {
      console.warn(`Failed to parse ${file}: ${e.message}`);
    }
  }
  console.log(`Extracted ${Object.keys(helpDocs).length} help documents`);

  // Merge UGen definitions with help docs
  console.log('\nMerging data...');
  const merged = {};

  for (const [name, ugen] of Object.entries(allUgens)) {
    const help = helpDocs[name] || {};

    merged[name] = {
      name: ugen.name,
      summary: help.summary || null,
      categories: help.categories || null,
      rates: ugen.rates,
      args: ugen.args.map(arg => ({
        ...arg,
        doc: help.arguments?.[arg.name] || null
      })),
      description: help.description || null,
      related: help.related || [],
      parent: ugen.parent,
      signalRange: ugen.signalRange || null
    };

    if (ugen.inheritedFrom) {
      merged[name].inheritedFrom = ugen.inheritedFrom;
    }
  }

  // Sort by name
  const sorted = {};
  for (const name of Object.keys(merged).sort()) {
    sorted[name] = merged[name];
  }

  // Write output
  const output = {
    _meta: {
      generated: new Date().toISOString(),
      scSourcePath: SC_PATH,
      ugenCount: Object.keys(sorted).length
    },
    ugens: sorted
  };

  fs.writeFileSync(OUTPUT_PATH, JSON.stringify(output, null, 2));
  console.log(`\nWrote ${Object.keys(sorted).length} UGens to ${OUTPUT_PATH}`);

  // Print some stats
  const withDocs = Object.values(sorted).filter(u => u.summary).length;
  const withArgs = Object.values(sorted).filter(u => u.args.length > 0).length;
  console.log(`\nStats:`);
  console.log(`  UGens with documentation: ${withDocs}`);
  console.log(`  UGens with arguments: ${withArgs}`);
}

main();
