#!/bin/sh
# rebase.sh — move the port to a new Wine release.
#
# Usage:
#   tools/rebase.sh [<version>]         start; <version> defaults to the newest release upstream
#   tools/rebase.sh --finish <version>  once every patch is applied
#
# Start:
#   1. finds the newest release: the highest wine-X.Y tag in the repository the recipe's
#      SOURCE_URI downloads from (release candidates are left out);
#   2. downloads that version's tarball by the same SOURCE_URI pattern (or takes WINE_TARBALL)
#      and records its SHA256;
#   3. commits the tarball into the dev tree as a new root commit, tagged pristine-<version>,
#      without touching the dev tree's checkout or any uncommitted work there;
#   4. adds a worktree beside the dev tree, on a new branch renku-<version> at that tag, and
#      applies the current patchset there with `git am -3`, keeping each patch's author;
#   5. prints which driver interfaces changed between the old and new pristine trees: the
#      changes winerenku.drv has to follow, which the reference drivers show how to make.
# If a patch does not apply, git am stops and this script says which patch and which files.
# Resolve them in the worktree, `git add` them, `git am --continue`, until all are applied.
# A patch upstream has made unnecessary is dropped with `git am --skip`; say so in the README.
#
# Finish:
#   1. checks the worktree: all applied, nothing uncommitted;
#   2. regenerates the patchset from pristine-<version>..renku-<version> and checks that it
#      applies to pristine-<version> and gives the same tree;
#   3. in this repo: renames the recipe to wine-<version>.recipe with the new CHECKSUM_SHA256
#      and REVISION="1", and replaces the patchset with wine-<version>.patchset. Nothing is
#      committed;
#   4. removes the worktree. Branch renku-<version> stays in the dev tree.
# Then: build it, which needs a clean `haikuporter -c` (a new tarball), test it, update the
# README's patch table and revision history, and commit.

set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/common.sh"
need_dev

die() { echo "rebase: $*" >&2; exit 2; }
# The pristine commit, its tag and the applied patches need a committer. They are not
# patches, so a neutral one is used, without changing the dev tree's git config. The global
# excludes are off, so that a personal ignore rule cannot leave a file out of the import.
gitc() { git -c user.name="devtree" -c user.email="devtree@localhost" \
             -c core.excludesFile=/dev/null "$@"; }
recipe_value() {
    sed -n "s/^$1=\"\\(.*\\)\"/\\1/p" "$REPO/$RECIPE" | head -n1 | sed "s/\${portVersion}/$2/g; s/\$portVersion/$2/g"
}

OLD=$VER
INTERFACES="include/wine/gdi_driver.h include/wine/opengl_driver.h include/wine/vulkan_driver.h include/ntuser.h"
REFERENCE="dlls/win32u dlls/winemac.drv dlls/winewayland.drv programs/explorer"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

interface_report() {
    git -C "$DEV" rev-parse -q --verify "pristine-$OLD" >/dev/null || {
        echo "(no tag pristine-$OLD in the dev tree: no interface report)"; return 0; }
    echo
    echo "Driver interface versions, wine-$OLD -> wine-$NEW:"
    for f in $INTERFACES; do
        for t in "pristine-$OLD" "pristine-$NEW"; do
            git -C "$DEV" show "$t:$f" 2>/dev/null | grep -E '^#define WINE_[A-Z_]*_VERSION ' \
                | sed "s|^|  $t $f: |" || true
        done
    done
    echo
    echo "What changed in the interfaces and in the reference drivers (git -C $DEV diff pristine-$OLD pristine-$NEW -- <path>):"
    git -C "$DEV" diff --stat=100 "pristine-$OLD" "pristine-$NEW" -- $INTERFACES $REFERENCE | sed 's/^/  /'
}

start() {
    uri_pattern=$(sed -n 's/^SOURCE_URI="\(.*\)"/\1/p' "$REPO/$RECIPE" | head -n1)
    upstream=$(printf '%s' "$uri_pattern" | sed 's|/archive/.*||')
    NEW=${1:-}
    if [ -z "$NEW" ]; then
        NEW=$(git ls-remote --tags "$upstream.git" 'refs/tags/wine-*' \
              | sed -n 's|.*refs/tags/wine-\([0-9][0-9]*\.[0-9][0-9]*\(\.[0-9][0-9]*\)\{0,1\}\)$|\1|p' \
              | sort -V | tail -n1)
        [ -n "$NEW" ] || die "cannot list the release tags of $upstream"
    fi
    if [ "$(printf '%s\n%s\n' "$OLD" "$NEW" | sort -V | tail -n1)" = "$OLD" ]; then
        echo "The port is on wine-$OLD; the newest release is wine-$NEW. Nothing to do."
        return 0
    fi
    WT="${DEV%/}-rebase-$NEW"
    git -C "$DEV" rev-parse -q --verify "refs/tags/pristine-$NEW" >/dev/null \
        && die "the dev tree already has pristine-$NEW; continue in $WT, or delete the tag and branch renku-$NEW to start again"
    [ -e "$WT" ] && die "$WT already exists"
    echo "Moving the port from wine-$OLD to wine-$NEW."

    tarball=${WINE_TARBALL:-}
    if [ -z "$tarball" ]; then
        uri=$(recipe_value SOURCE_URI "$NEW")
        echo "Downloading $uri"
        tarball="$tmp/wine.tar.gz"
        curl -fL --retry 3 -o "$tarball" "$uri"
    fi
    sha=$(sha256sum "$tarball" | cut -d' ' -f1)
    srcdir=$(recipe_value SOURCE_DIR "$NEW")
    tar -xzf "$tarball" -C "$tmp"
    [ -d "$tmp/$srcdir" ] || die "the tarball has no $srcdir/"

    # The import: a tree from the unpacked tarball, through a private index, so the dev
    # tree's own index and checkout are not involved.
    gd=$(cd "$DEV" && cd "$(git rev-parse --git-common-dir)" && pwd)
    GIT_INDEX_FILE="$tmp/index" GIT_WORK_TREE="$tmp/$srcdir" gitc --git-dir="$gd" add -A
    tree=$(GIT_INDEX_FILE="$tmp/index" git --git-dir="$gd" write-tree)
    commit=$(echo "wine-$NEW pristine" | gitc --git-dir="$gd" commit-tree "$tree")
    gitc -C "$DEV" tag -a "pristine-$NEW" -m "wine-$NEW pristine, tarball SHA256 $sha" "$commit"
    echo "Tagged pristine-$NEW (tree $(git -C "$DEV" rev-parse --short "$tree"), tarball SHA256 $sha)."

    git -C "$DEV" worktree add -q -b "renku-$NEW" "$WT" "pristine-$NEW"
    echo "Worktree $WT, branch renku-$NEW."
    n=$(grep -c '^From [0-9a-f]\{40\} ' "$REPO/$PSET")
    echo "Applying the $n patches of $PSET..."
    if (cd "$WT" && gitc am -q -3 --committer-date-is-author-date "$REPO/$PSET"); then
        echo "All $n patches applied."
        interface_report
        echo
        echo "Check the report, fix what the new interfaces need as commits in $WT, build and"
        echo "test there, then: tools/rebase.sh --finish $NEW"
    else
        interface_report
        echo
        ga=$(git -C "$WT" rev-parse --git-path rebase-apply)
        case $ga in /*) ;; *) ga="$WT/$ga" ;; esac
        echo "STOPPED at patch $(cat "$ga/next") of $n:" \
             "$(git -C "$WT" am --show-current-patch=raw | sed -n 's/^Subject: //p' | head -n1)"
        echo "Files in conflict:"
        git -C "$WT" diff --name-only --diff-filter=U | sed 's/^/  /'
        echo
        echo "In $WT: resolve them, git add them, git am --continue (or --skip to drop the"
        echo "patch), until all are applied. Then: tools/rebase.sh --finish $NEW"
        return 1
    fi
}

finish() {
    NEW=${1:-}
    [ -n "$NEW" ] || die "--finish needs the version"
    WT="${DEV%/}-rebase-$NEW"
    [ -d "$WT" ] || die "no worktree $WT"
    ga=$(git -C "$WT" rev-parse --git-path rebase-apply)
    case $ga in /*) ;; *) ga="$WT/$ga" ;; esac
    [ -d "$ga" ] && die "git am is still in progress in $WT"
    [ "$(git -C "$WT" symbolic-ref --short HEAD)" = "renku-$NEW" ] || die "$WT is not on renku-$NEW"
    [ -z "$(git -C "$WT" status --porcelain)" ] || die "$WT has uncommitted changes"
    sha=$(git -C "$DEV" tag -l --format='%(contents)' "pristine-$NEW" | sed -n 's/.*SHA256 \([0-9a-f]\{64\}\).*/\1/p')
    [ -n "$sha" ] || die "tag pristine-$NEW does not record the tarball's SHA256"

    git -C "$WT" format-patch -q --stdout "pristine-$NEW..HEAD" > "$tmp/new.patchset"
    n=$(grep -c '^From [0-9a-f]\{40\} ' "$tmp/new.patchset")
    git -C "$DEV" worktree add -q --detach "$tmp/verify" "pristine-$NEW"
    (cd "$tmp/verify" && gitc am -q "$tmp/new.patchset")
    want=$(git -C "$WT" rev-parse 'HEAD^{tree}')
    have=$(git -C "$tmp/verify" rev-parse 'HEAD^{tree}')
    git -C "$DEV" worktree remove --force "$tmp/verify"
    [ "$want" = "$have" ] || die "the regenerated patchset gives tree $have, not $want"
    echo "The $n-patch patchset applies to pristine-$NEW and gives tree $want."

    new_recipe="app-emulation/wine/wine-$NEW.recipe"
    new_pset="app-emulation/wine/patches/wine-$NEW.patchset"
    git -C "$REPO" mv "$RECIPE" "$new_recipe"
    sed -i "s/^CHECKSUM_SHA256=\".*\"/CHECKSUM_SHA256=\"$sha\"/; s/^REVISION=\".*\"/REVISION=\"1\"/" \
        "$REPO/$new_recipe"
    git -C "$REPO" mv "$PSET" "$new_pset"
    cp "$tmp/new.patchset" "$REPO/$new_pset"
    git -C "$DEV" worktree remove "$WT"
    echo "This repo now has $new_recipe (REVISION 1) and $new_pset; not committed."
    echo "Branch renku-$NEW is in $DEV: git -C $DEV checkout renku-$NEW"
    echo "Next: a clean build (haikuporter -c, a new tarball), tests, the README, a commit."
}

case "${1:-}" in
    --finish) shift; finish "$@" ;;
    -h|--help) sed -n '2,34p' "$0" ;;
    *) start "$@" ;;
esac
