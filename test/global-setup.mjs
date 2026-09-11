// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Runs once before any worker starts: export the demo site so the suite can
// boot the pages as they are published (build/site), not only as they sit in
// the repository. Done here rather than in a spec because workers run in
// parallel, and one re-exporting while another is mid-page-load would be a
// race with itself.

import { execFileSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

export default function globalSetup() {
  execFileSync(path.join(ROOT, "scripts", "export-site.sh"), { stdio: "inherit" });
}
