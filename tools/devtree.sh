#!/bin/sh
# devtree.sh — make the dev tree: Wine's source as git, with this repo's patches as commits.
#
# The patchset is generated from such a tree, and patches are changed there (README.md,
# "Changing a patch"). Nobody needs a copy of anyone else's: the tree is the pinned upstream
# tarball plus the patchset, so it can always be made again, and it comes out the same. This
# script does that:
#
#   1. downloads the recipe's SOURCE_URI (or takes the tarball given as WINE_TARBALL) and
#      checks it against the recipe's CHECKSUM_SHA256;
#   2. unpacks it into <dir>, commits it as "wine-<ver> pristine" and tags that pristine-<ver>;
#   3. applies the patchset with `git am` on branch renku-<ver>, keeping each patch's author
#      and date;
#   4. checks that every patch became one commit, and prints the resulting tree hash.
#
# Usage: tools/devtree.sh <dir>        (<dir> must not exist yet, or be empty)
#
# Then point WINE_DEV_TREE at <dir> (tools/local.conf). Commits you make there carry your own
# git identity, and their From: lines ship in the patchset: check `git config user.email`
# in <dir> before committing.

set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/common.sh"

[ $# -eq 1 ] || { sed -n '2,20p' "$0"; exit 2; }
dir=$1
if [ -e "$dir" ] && [ -n "$(ls -A "$dir" 2>/dev/null)" ]; then
    echo "devtree: $dir exists and is not empty" >&2; exit 2
fi

recipe="$REPO/$RECIPE"
pset="$REPO/$PSET"
[ -f "$recipe" ] && [ -f "$pset" ] || { echo "devtree: no $RECIPE or $PSET" >&2; exit 2; }
value() { sed -n "s/^$1=\"\\(.*\\)\"/\\1/p" "$recipe" | head -n1 | sed "s/\${portVersion}/$VER/g; s/\$portVersion/$VER/g"; }
uri=$(value SOURCE_URI)
sha=$(value CHECKSUM_SHA256)
srcdir=$(value SOURCE_DIR)
[ -n "$uri" ] && [ -n "$sha" ] && [ -n "$srcdir" ] || { echo "devtree: cannot read the recipe" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

tarball=${WINE_TARBALL:-}
if [ -z "$tarball" ]; then
    echo "Downloading $uri"
    tarball="$tmp/wine.tar.gz"
    curl -fL --retry 3 -o "$tarball" "$uri"
fi
echo "$sha  $tarball" | sha256sum -c - >/dev/null \
    || { echo "devtree: $tarball does not match the recipe's CHECKSUM_SHA256" >&2; exit 1; }
echo "Tarball matches the recipe's SHA256."

tar -xzf "$tarball" -C "$tmp"
[ -d "$tmp/$srcdir" ] || { echo "devtree: the tarball has no $srcdir/" >&2; exit 1; }
mkdir -p "$dir"
# Move the contents, dotfiles included, so <dir> itself can be given by the caller.
(cd "$tmp/$srcdir" && tar -cf - .) | (cd "$dir" && tar -xf -)

cd "$dir"
# The pristine commit and the git am commits need a committer. They are not patches, so a
# neutral one is used here without touching the tree's git config; your own commits later use
# yours. The global excludes are off, so that a personal ignore rule cannot leave a file out.
gitc() { git -c user.name="devtree" -c user.email="devtree@localhost" \
             -c core.excludesFile=/dev/null "$@"; }
git init -q -b "renku-$VER"
gitc add -A
gitc commit -qm "wine-$VER pristine"
git tag "$PRISTINE"
echo "Pristine tree $(git rev-parse --short 'HEAD^{tree}') tagged $PRISTINE."

gitc am -q --committer-date-is-author-date "$pset"
want=$(grep -c '^From [0-9a-f]\{40\} ' "$pset")
have=$(git rev-list --count "$PRISTINE..HEAD")
[ "$want" = "$have" ] || { echo "devtree: $want patches in the patchset, $have commits" >&2; exit 1; }
echo "Applied $have patches on branch renku-$VER."
echo "Tree $(git rev-parse 'HEAD^{tree}')"
echo "Set WINE_DEV_TREE=$(pwd) in tools/local.conf."
