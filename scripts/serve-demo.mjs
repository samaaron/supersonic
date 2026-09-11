// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Sam Aaron
//
// serve-demo.mjs — a static server for example/demo.html.
//
// The one thing that makes this different from `python -m http.server`: the
// COOP/COEP pair. Without them the page is not cross-origin isolated, the
// browser withholds SharedArrayBuffer, and the SAB transport cannot start —
// which surfaces as a boot failure deep in the worklet rather than as anything
// resembling "you are missing two headers".
//
// Serves the repository root, so /dist/... and /example/... both resolve.

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, "..");
const PORT = Number(process.env.PORT) || 8080;

const MIME = {
  ".html": "text/html",
  ".js": "application/javascript",
  ".mjs": "application/javascript",
  ".wasm": "application/wasm",
  ".json": "application/json",
  ".css": "text/css",
  ".flac": "audio/flac",
  ".wav": "audio/wav",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".scsyndef": "application/octet-stream",
};

const server = http.createServer((req, res) => {
  const urlPath = decodeURIComponent(req.url.split("?")[0]);
  const rel = urlPath === "/" ? "/example/demo.html" : urlPath;
  const filePath = path.join(ROOT, rel);

  // Refuse to serve outside the root. path.join already normalises "..", so
  // the prefix test is the whole check.
  if (!filePath.startsWith(ROOT)) {
    res.writeHead(403).end("Forbidden");
    return;
  }

  fs.stat(filePath, (err, stats) => {
    if (err || !stats.isFile()) {
      res.writeHead(404).end("Not found");
      return;
    }
    // The isolation headers, and CORP so the worklet may load its own wasm.
    res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
    res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
    res.setHeader("Cross-Origin-Resource-Policy", "cross-origin");
    res.setHeader("Content-Type", MIME[path.extname(filePath).toLowerCase()] || "application/octet-stream");
    res.setHeader("Content-Length", stats.size);
    // The wasm and the sample pack are large and rebuilt often; caching them
    // is how you end up debugging a build you are not running.
    res.setHeader("Cache-Control", "no-store");

    if (req.method === "HEAD") { res.writeHead(200).end(); return; }
    fs.createReadStream(filePath).pipe(res);
  });
});

server.listen(PORT, () => {
  console.log(`SuperSonic demo → http://localhost:${PORT}/example/demo.html`);
  console.log(`  (cross-origin isolated: COOP/COEP set, SharedArrayBuffer available)`);
});
