#!/bin/sh
# SessionStart hook: puts tools/status.sh's report (the port, the dev tree, the sources of
# truth, the build machine; AGENTS.md §1) in the session's context. Always exits 0.
REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
sh "$REPO/tools/status.sh" 2>/dev/null
exit 0
