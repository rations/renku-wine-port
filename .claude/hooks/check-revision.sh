#!/bin/sh
# Stop hook: reports what tools/check.sh finds (REVISION not bumped, patchset out of step with
# the dev tree, placeholder authors, uncommitted dev-tree work, a stale build-machine copy;
# AGENTS.md §2 and §3) before the turn ends. Advisory: always exits 0.
REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
timeout 25 sh "$REPO/tools/check.sh" 2>/dev/null
exit 0
