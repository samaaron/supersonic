# ESP-IDF project: SuperSonic engine build-smoke for ESP32-S3.
#
# Compiles the shared engine for the real Xtensa/esp32s3 target (the SCP_TARGET_ESP32
# profile) and drives it through the lanes C ABI from a minimal app_main. Exists so
# CI can verify the engine BUILDS + LINKS for embedded — the no-byte-atomics path,
# EXT_RAM_BSS PSRAM placement and Xtensa codegen that the desktop freestanding guard
# cannot reach. Build-only in CI (no hardware): `idf.py set-target esp32s3 build`.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(supersonic_esp32s3)
