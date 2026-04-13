#!/usr/bin/env node
/**
 * Run WASM SIMD128 nova-simd backend tests.
 * Compiles simd_vec_test.cpp to WASM and runs it via Node.js.
 */
import { execSync } from 'child_process';
import { dirname, resolve } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const testDir = __dirname;
const novaSimdDir = resolve(__dirname, '../../src/scsynth/external_libraries/nova-simd');

// Recompile to ensure we test the latest code
console.log('Compiling SIMD test...');
execSync(`emcc ${testDir}/simd_vec_test.cpp \
  -I ${novaSimdDir} \
  -msimd128 -DNOVA_SIMD -O2 \
  -sEXPORTED_FUNCTIONS=_run_simd_tests \
  -sEXPORTED_RUNTIME_METHODS=ccall \
  -sINITIAL_MEMORY=1048576 \
  -sALLOW_MEMORY_GROWTH \
  --no-entry \
  -o ${testDir}/simd_test.mjs \
  -sMODULARIZE -sEXPORT_NAME=SIMDTest`, { stdio: 'inherit' });

console.log('Running SIMD tests...');
const { default: SIMDTest } = await import('./simd_test.mjs');
const module = await SIMDTest();
const failures = module.ccall('run_simd_tests', 'number', [], []);

if (failures === 0) {
  console.log('All SIMD tests passed.');
  process.exit(0);
} else {
  console.error(`${failures} SIMD test(s) FAILED.`);
  process.exit(1);
}
