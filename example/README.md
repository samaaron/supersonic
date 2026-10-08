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
scripts/export-site.sh --cdn    # -> build/site, as sonic-pi.net/supersonic serves it
```

Without `--cdn`, `build/site` is this directory with `dist` copied in as a
real directory. The test suite boots that copy as well as this one.

With `--cdn` it is what sonic-pi.net/supersonic serves: `demo.html` and the
library, with the samples and synthdefs fetched from the npm packages on
jsDelivr, at the versions in `packages/` — so those must be published first.

Either way, the host must send
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (`serve.json` does this for
`npx serve`). The demo asks for the SAB transport (`mode: "sab"`), so
without them it does not boot.
