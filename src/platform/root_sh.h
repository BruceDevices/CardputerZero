/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

namespace platform {

// Internal `--root-sh <script>` subcommand: runs the given fixed shell script
// via /bin/sh -c. The GUI-launched app is non-root, so root-needing catalog
// rows (iw scans, monitor mode, responder, ...) reach this as
// `sudo -n <this binary> --root-sh <script>`; the /etc/sudoers.d rule already
// authorizes exactly this binary, so no rule change is needed. Catalog scripts
// are static literals; the Terminal screen additionally passes user-typed
// commands through the same path - that grants nothing new, since the rule
// already covers this binary with ANY argv. Replaces the process image on
// success (signals/Stop button hit the real script).
int root_sh_main(int argc, char** argv);

} // namespace platform
