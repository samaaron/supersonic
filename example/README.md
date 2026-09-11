# SuperSonic - Demonstration

A working example of SuperSonic in the browser.

Send and receive OSC, view debug output, built-in scope and load Sonic Pi synthdefs to experiment with.

## Running

From the repository root, after `scripts/build-web.sh`:

```bash
node scripts/serve-demo.mjs
```

Then open http://localhost:8080/example/demo.html. The server sends the
COOP/COEP headers the SAB transport needs; `dist` here is a symlink to the
build.

## Publishing

```bash
scripts/export-site.sh          # -> build/site
```

`build/site` is this directory with `dist` copied in as a real directory —
what sonic-pi.net/supersonic serves. The host must send
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (`serve.json` does this for
`npx serve`). The test suite boots the exported copy as well as this one.




