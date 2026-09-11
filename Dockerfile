# Builds the web bundle (clockwork + scsynth for the AudioWorklet) and serves
# the demo. The Emscripten image has no Rust; the web build needs a nightly
# with rust-src (std is rebuilt with atomics for the shared heap) and the two
# wasm targets, plus wasm-bindgen at the version clockwork's Cargo.lock pins.
FROM emscripten/emsdk:4.0.21 AS build

RUN npm install -g esbuild

ENV RUSTUP_HOME=/usr/local/rustup CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain nightly-2026-07-02 \
          --component rust-src \
          --target wasm32-unknown-emscripten --target wasm32-unknown-unknown \
 && cargo install wasm-bindgen-cli --version 0.2.127 --locked

WORKDIR /build

COPY clockwork /build/clockwork/
COPY dsp /build/dsp/
COPY js /build/js/
COPY rust /build/rust/
COPY scripts /build/scripts/
COPY packages /build/packages/
COPY docs /build/docs/
COPY package.json package-lock.json /build/
RUN --mount=type=cache,id=em-cache,target=/em_cache \
    --mount=type=cache,id=npm-cache,target=/root/.npm \
    npm install && EM_CACHE=/em_cache bash scripts/build-web.sh --release

FROM node:22-slim AS runtime

WORKDIR /app

RUN npm install -g serve

COPY ./example /app
COPY --from=build /build/dist /app/dist

EXPOSE 3000

CMD ["serve", "-l", "3000"]
