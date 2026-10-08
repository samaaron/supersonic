# Building from Source

SuperSonic is scsynth (`dsp/scsynth`) running on [clockwork](https://github.com/samaaron/clockwork),
which is a git submodule at `clockwork/`. Clone with the submodule, or fetch it
afterwards:

```bash
git clone --recurse-submodules https://github.com/samaaron/supersonic
# or, in an existing clone:
git submodule update --init
```

The native server, the web build and the NIF are one CMake tree — the same
lists of sources, the same flags, the same guest — configured three ways: for
this machine, for this machine with `-DCLOCKWORK_NIF=ON`, and under `emcmake`
for the AudioWorklet. `scripts/build-all.sh` builds all three; the sections
below build each on its own.

The engine's options — how many buffers, nodes, buses and so on — are listed
once, in `dsp/scsynth/scsynth_options.h`, and every host takes them by name
from there: the native server's `-n`/`-b`/`-a`/... flags, the NIF's config
map, and the web client's `scsynthOptions` (through the generated
`js/lib/scsynth_options_schema.js`; run `node scripts/gen-scsynth-options.mjs`
after changing the header).

## Native server

The native build produces `SuperSonic`, the standalone server Sonic Pi
launches, plus `clockwork-plugin-bridge`, the out-of-process plugin host the
engine spawns beside it.

**Prerequisites:** CMake 3.24+, a C++20 compiler, and a Rust toolchain
(stable; [rustup](https://rustup.rs) is the easiest way). On Linux, the audio
and windowing headers clockwork's device layer needs:

```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libasound2-dev libudev-dev libjack-jackd2-dev libpipewire-0.3-dev \
  libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxcomposite-dev
```

**macOS/Windows:** CMake, a C++20 compiler and Rust. No additional dependencies.

```bash
cmake -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release --parallel --target SuperSonic
```

or the wrapper scripts, which do the same:

```bash
scripts/build-native.sh            # Release build
scripts/build-native.sh --debug    # Debug build
scripts/build-native.sh --clean    # Clean rebuild
scripts/build-native.bat           # Windows
```

The binary lands at `build/native/SuperSonic` (`build/native/Release/SuperSonic.exe`
on Windows). `cmake --install build/native` installs it as `supersonic` with
the plugin bridge and the man page.

Ableton Link (tempo sync and Link Audio) is on by default; pass
`-DCLOCKWORK_LINK=OFF` for a build without it. Everything else about the build
is clockwork's: its options (`CLOCKWORK_MIDI`, `CLOCKWORK_GAMEPAD`,
`CLOCKWORK_OSC`, `CLOCKWORK_PLUGINS`, ...) are documented in
[clockwork/docs/BUILDING.md](../clockwork/docs/BUILDING.md).

### Native tests

The native test suite uses Catch2. Configure with `-DBUILD_TESTS=ON`, build
the tests and run them the two ways CI does, because each sees what the other
cannot. Through CTest every Catch2 case runs in a process of its own
(benchmarks excluded), alongside the guest boundary check
(`scripts/check-guest-boundary.sh`): a case that only passes after another has
run fails there. The binary runs every case in one process, in a shuffled
order: a case that leaves something behind for the next fails there.

```bash
cmake -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build/native --config Release --parallel --target SuperSonicNativeTests
ctest --test-dir build/native -C Release --output-on-failure -j 4
./build/native/test/native/SuperSonicNativeTests "~[benchmark]" --order rand
```

`scripts/test-native.sh` does all four steps. On Windows the binary is at
`build/native/test/native/Release/SuperSonicNativeTests.exe`. A shuffled run
prints its seed first ("Randomness seeded to"); `--order rand --rng-seed <seed>`
runs the same order again. The benchmarks are run with
`SuperSonicNativeTests "[benchmark]"`.

Note that the Link tests join a real Link session on the machine's loopback:
another Link-enabled app running at the same time (Sonic Pi, Ableton Live)
will change the tempo they observe and fail them.

### Command transports

`test/transport-harness/run.sh` (`run.ps1` on Windows) boots the built server
headless once per command transport — UDP, TCP, Unix sockets, the shared-memory
command plane, the Windows named pipe — and drives OSC over each with the
probe clockwork ships as an example (`clockwork/rust/clockwork-comms/examples/transport_probe.rs`):

```bash
test/transport-harness/run.sh                # defaults to build/native/SuperSonic
test/transport-harness/run.sh path/to/SuperSonic
```

## Web (AudioWorklet)

**Prerequisites:**

- The [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html),
  activated in the shell that runs the build (`source ./emsdk_env.sh`).
- A Rust **nightly** toolchain with `rust-src`, plus the `wasm32-unknown-emscripten`
  and `wasm32-unknown-unknown` targets. The worklet shares its heap with
  JavaScript, so `std` is rebuilt with atomics (`-Z build-std`), which only
  nightly can do. The build looks for `nightly-2026-07-02` first and falls
  back to a floating `nightly`.
- `wasm-bindgen-cli` at the version clockwork's `Cargo.lock` pins (the
  subsystem build tells you which if it is missing).
- Node.js and `npm install`, for esbuild and the test suite.

```bash
rustup toolchain install nightly-2026-07-02 --component rust-src \
  --target wasm32-unknown-emscripten --target wasm32-unknown-unknown
npm install
scripts/build-web.sh             # development build
scripts/build-web.sh --release   # what CI and npm publish use: __DEV__ off, minified, source maps
```

This will:

1. Configure and build the same CMake tree as the native server under
   `emcmake` (`build/web`): clockwork's CMake builds its Rust subsystems for
   `wasm32-unknown-emscripten` with the pinned nightly, compiles clockwork and
   the scsynth guest with the module's flags, and links
   `dist/wasm/scsynth-nrt.wasm` with the exports the worklet calls. The memory
   sizes come from `clockwork/js/memory_layout.js` at configure time.
2. Build clockwork's MIDI and gamepad modules (wasm-bindgen) into
   `clockwork/dist/`, and copy them under `dist/`.
3. Bundle the JavaScript client and the workers with esbuild.
4. Copy the synthdefs and samples into `dist/`, the engine and the worklet into
   `packages/supersonic-scsynth-core`, and generate `README.npm.md`.

The module alone, without the JavaScript, is plain CMake:

```bash
emcmake cmake -B build/web -DCMAKE_BUILD_TYPE=Release
cmake --build build/web --parallel
```

### Web tests

```bash
npm test          # Playwright, both transports, stops at the first failure
npm run test:all  # the whole suite
npm run test:types
npm run test:simd # the nova-simd wasm128 backend against a scalar reference (needs emcc)
```

The suite serves the repository over `test/server.mjs` and also boots the
exported demo site, so a web build must exist first.

## NIF (Erlang/Elixir)

The NIF is clockwork's (`clockwork/src/nif`); built here it carries scsynth as
its guest. clockwork vendors the `erl_nif` headers, so building needs no BEAM.

**Prerequisites:** the native server's, above — CMake 3.24+, a C++20 compiler,
a Rust toolchain and, on Linux, the same packages. The NIF drives an audio
device and links clockwork's Rust subsystems, so configuring it with
`-DCLOCKWORK_DEVICE=OFF` or `-DCLOCKWORK_RUST=OFF` is refused.

```bash
cmake -B build/nif -DCLOCKWORK_NIF=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/nif --target clockwork_nif --config Release --parallel
```

The library lands in the build root for every configuration:
`build/nif/clockwork.so` on macOS and Linux, `build/nif/clockwork.dll` on
Windows. It loads into Erlang/OTP 26 or later (the vendored headers are NIF
API 2.17, which OTP 26 introduced), through the module
`clockwork/src/nif/clockwork.erl`, which looks for it in the directory named
by `CLOCKWORK_NIF_PATH` — the directory, not the file. The other places it
looks, and how to use it, are in the [NIF guide](NIF.md).

### NIF tests

`scripts/test-nif.sh` configures and builds the library as above, then runs
`mix test` in `test/nif` with `CLOCKWORK_NIF_PATH` set to `build/nif` and
`SUPERSONIC_HEADLESS=1`, so the tests boot without an audio device. They need
Elixir 1.15 or later (`test/nif/mix.exs`), on an OTP the library loads into.

## Output

After a web build, `dist/` holds:

```
dist/
├── supersonic.js                 # The client (ES module) — SuperSonic on clockwork's Clockwork class
├── osc_channel.js, osc_fast.js   # OSC channel for workers, fast OSC codec
├── metrics_component.js, metrics-*.css
├── wasm/
│   └── scsynth-nrt.wasm          # The engine
├── workers/
│   ├── clockwork_audio_worklet.js  # AudioWorklet processor
│   ├── osc_in_worker.js            # OSC input handler
│   └── osc_out_log_sab_worker.js   # OSC output log
├── midi/, gamepad/               # clockwork's main-thread subsystems (wasm-bindgen)
├── synthdefs/                    # Compiled synth definitions
└── samples/                      # Audio samples
```

## Running the Demo

```bash
cd example && npx serve
```

Open http://localhost:3000/demo.html

## Docker

The Dockerfile builds the web bundle in an Emscripten image with a Rust
toolchain installed on top, and serves the demo:

```bash
docker build -t supersonic .
docker run --rm -it -p 3000:3000 supersonic
```

Open http://localhost:3000/demo.html

## Development Tips

**Rebuild just JavaScript:**

```bash
npx esbuild js/supersonic.js --bundle --format=esm --outfile=dist/supersonic.js --external:./scsynth-nrt.wasm
```

**Watch mode:**

```bash
npx esbuild js/supersonic.js --bundle --format=esm --outfile=dist/supersonic.js --external:./scsynth-nrt.wasm --watch
```

**Updating clockwork:** clockwork is a submodule, so a newer clockwork is a
pointer change here:

```bash
git -C clockwork fetch origin && git -C clockwork checkout <commit>
git add clockwork
```

## Troubleshooting

### Emscripten not found

Make sure you've run `source emsdk_env.sh` in your current shell session.

### "a nightly Rust toolchain is required"

Install one with `rust-src` (see the web prerequisites). If both a Homebrew or
distro Rust and rustup are installed, the build resolves the nightly through
rustup regardless of which `cargo` is first on `PATH`.

### wasm-bindgen version mismatch

The CLI must match the `wasm-bindgen` crate version in `clockwork/rust/Cargo.lock`.
The build prints the exact `cargo install` line when it does not.

### WASM compilation fails

Check that you have enough memory. The compilation can use several GB of RAM.
