#!/bin/sh
# status.sh — where things stand, for the start of a work session (person or agent).
#
# Prints the port's version, REVISION and patch authors, the dev tree's state, where the
# sources of truth are (AGENTS.md §1), and whether the build machine answers. Read-only.
# Optional settings in tools/local.conf name local copies of the reference sources:
#   WINE_UPSTREAM_TREE   a clone of https://github.com/wine-mirror/wine
#   RENKU_SOURCE_TREE    a clone of https://github.com/RenkuOS/Source

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/common.sh"

echo "AGENTS.md is binding: never guess (read the sources below); never hand-edit a .patchset;"
echo "never leave an edit only in the build machine's work tree; bump REVISION on every recipe or"
echo "patchset change; never report a build or test as passing without running it."
echo

# The port.
if [ -f "$REPO/$RECIPE" ]; then
    rev=$(sed -n 's/^REVISION="\(.*\)"/\1/p' "$REPO/$RECIPE")
    n=$(grep -c '^From [0-9a-f]\{40\} ' "$REPO/$PSET" 2>/dev/null || echo 0)
    echo "Port: wine-$VER REVISION ${rev:-?}, $n patches in $PSET. Authors, by patch:"
    # Consecutive patches by the same author, as ranges: upstream ones keep their From: lines.
    sed -n 's/^From: //p' "$REPO/$PSET" | awk '
        function range() {
            if (start == stop) printf "  %d  %s\n", start, last
            else printf "  %d-%d  %s\n", start, stop, last
        }
        { if ($0 != last) { if (NR > 1) range(); start = NR; last = $0 } stop = NR }
        END { if (NR) range() }'
fi
echo

# The dev tree.
if [ -n "$DEV" ] && [ -d "$DEV/.git" ]; then
    br=$(git -C "$DEV" branch --show-current 2>/dev/null)
    head=$(git -C "$DEV" log -1 --format='%h %s' 2>/dev/null | cut -c1-70)
    echo "Dev tree $DEV: branch ${br:-?}, HEAD ${head:-?}"
    git -C "$DEV" merge-base --is-ancestor "$PRISTINE" HEAD 2>/dev/null \
        || echo "  WARNING: not built on $PRISTINE; check out the branch for wine-$VER."
    dirty=$(git -C "$DEV" status --porcelain 2>/dev/null | grep -c . || true)
    [ "${dirty:-0}" -gt 0 ] && echo "  WARNING: $dirty uncommitted file(s): that work is in no patchset yet."
else
    echo "Dev tree: none set up (WINE_DEV_TREE in tools/local.conf). Make one with tools/devtree.sh"
    echo "  before changing any patch."
fi

# The sources of truth.
echo
echo "Sources of truth (read these instead of assuming):"
echo "  Be API: RenkuOS source, https://github.com/RenkuOS/Source (headers/os/, src/kits/, src/servers/app/)"
[ -n "${RENKU_SOURCE_TREE:-}" ] && [ -d "$RENKU_SOURCE_TREE/.git" ] && \
    echo "          local clone $RENKU_SOURCE_TREE ($(git -C "$RENKU_SOURCE_TREE" log -1 --format='%h %ad' --date=short))"
echo "  Wine history: https://github.com/wine-mirror/wine"
[ -n "${WINE_UPSTREAM_TREE:-}" ] && [ -d "$WINE_UPSTREAM_TREE/.git" ] && \
    echo "          local clone $WINE_UPSTREAM_TREE ($(sed 's/Wine version //' "$WINE_UPSTREAM_TREE/VERSION" 2>/dev/null))"
echo "  Wine with this port's patches: the dev tree. What was really built: the build machine's"
echo "  work tree (include/config.h) and \`package list -i <hpkg>\`."

# The build machine.
echo
if [ -z "$HOST" ]; then
    echo "Build machine: none set (WINE_BUILD_HOST). No build or on-target check is possible."
elif timeout 6 ssh -o BatchMode=yes -o ConnectTimeout=4 "$HOST" true 2>/dev/null; then
    echo "Build machine $HOST: reachable. tools/winework.sh status lists edits that exist only there."
else
    echo "Build machine $HOST: not reachable. Do not claim a build or test result."
fi
exit 0
