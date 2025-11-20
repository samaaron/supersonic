#!/usr/bin/env node
// Quick test to verify config structure after refactoring

import { ScsynthConfig } from '../js/scsynth_options.js';

console.log('Testing ScsynthConfig structure...\n');

console.log('Has memory:', !!ScsynthConfig.memory);
console.log('Has worldOptions:', !!ScsynthConfig.worldOptions);
console.log('');
console.log('Memory config:');
console.log('  totalPages:', ScsynthConfig.memory.totalPages);
console.log('  totalMemory:', ScsynthConfig.memory.totalMemory, 'bytes');
console.log('  wasmHeapSize:', ScsynthConfig.memory.wasmHeapSize, 'bytes');
console.log('  bufferPoolOffset:', ScsynthConfig.memory.bufferPoolOffset);
console.log('  bufferPoolSize:', ScsynthConfig.memory.bufferPoolSize);
console.log('');
console.log('WorldOptions sample:');
console.log('  numBuffers:', ScsynthConfig.worldOptions.numBuffers);
console.log('  maxNodes:', ScsynthConfig.worldOptions.maxNodes);
console.log('  realTimeMemorySize:', ScsynthConfig.worldOptions.realTimeMemorySize);
console.log('');
console.log('✓ Config structure is valid!');
