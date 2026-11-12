#!/bin/sh
# PreToolUse hook (Edit|Write): AGENTS.md §2 — patchsets are generated, never hand-edited.
#
# A hand edit desynchronizes the patchset from the dev tree (WINE_DEV_TREE), and the next
# legitimate `git format-patch` silently reverts it. Legitimate regeneration is a shell redirect
# from git, not an Edit/Write call, so blocking these tools does not obstruct the real workflow.
#
# Exit 0 = allow / not applicable.  Exit 2 = block, with the reason on stderr (fed back to Claude).
#
# Input: tool call JSON on stdin (we read .tool_input.file_path). Robust no-op if anything is
# missing or python3 is unavailable.

set -eu

# --- extract the target file path from the hook JSON on stdin --------------------------------
file_path=""
if command -v python3 >/dev/null 2>&1; then
    file_path=$(python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    print(d.get("tool_input", {}).get("file_path", ""))
except Exception:
    print("")
' 2>/dev/null || true)
fi

[ -n "$file_path" ] || exit 0

# --- is this a generated patch artifact? ------------------------------------------------------
blocked=0
case "$file_path" in
    *.patchset) blocked=1 ;;
esac
[ "$blocked" -eq 1 ] || exit 0

cat >&2 <<EOF
AGENTS.md §2: never hand-edit a patchset ($file_path).

Patchsets are generated, so a hand edit here is silently reverted by the next legitimate
regeneration. Edit the source instead:

  1. work in the dev tree (WINE_DEV_TREE in tools/local.conf; tools/devtree.sh makes one),
     on the branch for this checkout's wine-<ver>.recipe
  2. edit, then commit: one commit = one patch, in order
  3. regenerate:
       git -C "\$WINE_DEV_TREE" format-patch --stdout pristine-<ver>..HEAD \\
         > app-emulation/wine/patches/wine-<ver>.patchset
  4. tools/check.sh: it applies the patchset to pristine-<ver> and compares the tree
  5. bump REVISION in wine-<ver>.recipe

To drop a patch, remove that commit in the dev tree and regenerate — do not delete text
from the patchset by hand.

If there is no dev tree yet, make one first: tools/devtree.sh <dir>.
EOF
exit 2
