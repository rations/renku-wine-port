#!/bin/sh
# PreToolUse hook (Bash): AGENTS.md §3 — protect the build machine's work tree from being destroyed
# while it still holds edits that exist nowhere else.
#
# Editing work-<ver>/sources/ is ALLOWED and is the fast iteration loop: a clean build
# recompiles everything, an incremental one only what changed. What is not allowed is destroying that tree
# before the edits have been mirrored into the dev tree (WINE_DEV_TREE), because the tree is
# then the only copy. Two edits have already had to be rescued out of it.
#
# So this hook blocks the destructive commands — `haikuporter -c`, `rm -rf work-<ver>` — and
# only when tools/winework.sh reports unmirrored source edits. When that tree is clean
# (the normal case) nothing is blocked.
#
# Exit 0 = allow / not applicable.  Exit 2 = block, with the reason on stderr.
#
# Input: tool call JSON on stdin (we read .tool_input.command). Robust no-op if anything is
# missing, if python3 is unavailable, or if the build machine is unreachable — a hook must never
# strand a turn.

set -u

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
WINEWORK="$REPO/tools/winework.sh"
. "$REPO/tools/common.sh"

# --- extract the command from the hook JSON on stdin ------------------------------------------
cmd=""
if command -v python3 >/dev/null 2>&1; then
    cmd=$(python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    # Newlines would truncate our line-based matching below; fold them to spaces.
    print(" ".join((d.get("tool_input", {}).get("command", "") or "").splitlines()))
except Exception:
    print("")
' 2>/dev/null || true)
fi

[ -n "$cmd" ] || exit 0

# A command verb may be preceded by start-of-line, a shell operator, or a quote/paren --
# `ssh host 'rm ...'` and `ssh host "haikuporter -c ..."` are the common shapes, so \s alone
# is not enough of a boundary.
B="(^|[|;&[:space:]'\"(])"

what=""

# haikuporter -c / --clean deletes work-<ver>/sources/ without warning. Match -c alone or in a
# cluster (-yc), or the long form.
if printf '%s' "$cmd" | grep -Eq "${B}haikuporter[[:space:]]([^|;&]*[[:space:]])?(-[a-zA-Z]*c([[:space:]]|\$)|--clean)"; then
    what="haikuporter -c (it deletes the port's work-<ver>/sources/)"
elif printf '%s' "$cmd" | grep -Eq "${B}rm[[:space:]]+(-[^[:space:]]+[[:space:]]+)*[^|;&[:space:]]*work-[0-9]"; then
    what="rm of a work-<ver> tree"
fi

[ -n "$what" ] || exit 0

# Check the work tree the command names (haikuporter -c wine-11.19, rm work-11.19), which need
# not be this checkout's version: an old tree can still hold edits.
target=$(printf '%s' "$cmd" | sed -n 's/.*\(wine\|work\)-\([0-9][0-9]*\.[0-9][0-9.]*[0-9]\).*/\2/p' | head -n1)
[ -n "$target" ] && export WINE_PORT_VERSION="$target" && VER="$target"

# --- only block if that tree actually holds work that exists nowhere else ---------------------
[ -x "$WINEWORK" ] || exit 0
unmirrored=$(timeout 25 sh "$WINEWORK" status 2>/dev/null)
[ $? -eq 1 ] || exit 0   # 0 = clean, anything else = unreachable/error -> do not block

cat >&2 <<EOF
AGENTS.md §3: refusing $what — the build machine's work tree holds edits that exist nowhere else.

$unmirrored

Mirror them into the dev tree first, then this command is safe:

  tools/winework.sh diff     # review what changed on the build machine
  tools/winework.sh pull     # apply it to the dev tree
  git -C "\$WINE_DEV_TREE" commit      # or --amend into the right patch

Then regenerate the patchset, verify the tree hash, and bump REVISION (AGENTS.md §2).

If those edits are genuinely disposable, say so explicitly and re-run — this hook only
protects against losing them by accident.
EOF
exit 2
