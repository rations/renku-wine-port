#!/bin/sh
# check.sh — the checks that catch this port's silent failures (AGENTS.md §2 and §3).
#
#   tools/check.sh            check the working tree: report problems, exit 1 if there are any
#   tools/check.sh --staged   check what is staged for commit; run by tools/git-hooks/pre-commit
#                             (enable once with: git config core.hooksPath tools/git-hooks).
#                             Only problems in what is being committed fail it (1-3); the dev
#                             tree's and the build machine's state (4, 5) are only shown.
#
# What it checks:
#   1. the recipe or the patchset changed, but REVISION did not;
#   2. the patchset does not reproduce the dev tree: its patch count differs from the dev
#      tree's commits after pristine-<version>, or, when the patchset changed, applying it to
#      pristine-<version> does not give the dev tree's HEAD^{tree};
#   3. a patch has a placeholder author (its From: line ships in the patchset), or the dev
#      tree's own git identity is one, so the next commit there would ship with it;
#   4. the dev tree has uncommitted work, which is in no patchset;
#   5. the build machine's copy of the port differs from this repo's, so a build there would
#      use old files (only when the port changed and the machine answers).
# Checks 2, 4 and 5 need the settings in tools/local.conf, and are skipped without them.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/common.sh"
cd "$REPO" || exit 2

staged=0
[ "${1:-}" = "--staged" ] && staged=1
problems=""
notes=""
problem() { problems="${problems}$*
"; }
# Shown, but fails only a check of the working tree: not a reason to refuse a commit.
note() { if [ $staged -eq 1 ]; then notes="${notes}$*
"; else problem "$@"; fi; }

# The recipe and the patchset as they are about to be committed (or as they are now).
if [ $staged -eq 1 ]; then
    changed=$(git diff --cached --name-only HEAD -- "$RECIPE" "$PSET" 2>/dev/null || true)
    content() { git show ":$1" 2>/dev/null; }
else
    changed=$(git diff --name-only HEAD -- "$RECIPE" "$PSET" 2>/dev/null || true)
    content() { cat "$1" 2>/dev/null; }
fi
revision() { sed -n 's/^REVISION="\(.*\)"/\1/p' | head -n1; }

# --- 1. changed without a REVISION bump -------------------------------------------------------
if [ -n "$changed" ] && git cat-file -e "HEAD:$RECIPE" 2>/dev/null; then
    rev_now=$(content "$RECIPE" | revision)
    rev_head=$(git show "HEAD:$RECIPE" | revision)
    if [ -n "$rev_now" ] && [ "$rev_now" = "$rev_head" ]; then
        problem "REVISION is still \"$rev_now\", but the port changed ($(echo $changed)).
  Bump it: pkgman offers a package as an upgrade only by its revision."
    fi
fi

# --- 3a. placeholder authors in the patchset ---------------------------------------------------
PLACEHOLDER='@(localhost|local|example\.(com|org|net))>|<(dev|devtree|root|user|test)@|noreply'
bad=$(content "$PSET" | sed -n 's/^From: //p' | grep -Ei "$PLACEHOLDER" | sort -u || true)
[ -n "$bad" ] && problem "Patches with a placeholder author; their From: lines ship in the patchset:
$(printf '%s\n' "$bad" | sed 's/^/  /')
  Fix the author in the dev tree (git commit --amend --author=...), then regenerate."

if [ -n "$DEV" ] && [ -d "$DEV/.git" ]; then
    # --- 2. patchset against the dev tree ------------------------------------------------------
    if ! git -C "$DEV" rev-parse -q --verify "$PRISTINE" >/dev/null; then
        problem "The dev tree has no tag $PRISTINE: it is not a tree of wine-$VER."
    elif ! git -C "$DEV" merge-base --is-ancestor "$PRISTINE" HEAD 2>/dev/null; then
        problem "The dev tree's branch is not built on $PRISTINE; check out the branch for wine-$VER."
    elif [ $staged -eq 0 ] || printf '%s\n' "$changed" | grep -qx "$PSET"; then
        n_pset=$(content "$PSET" | grep -c '^From [0-9a-f]\{40\} ' || true)
        n_dev=$(git -C "$DEV" rev-list --count "$PRISTINE..HEAD")
        if [ "$n_pset" != "$n_dev" ]; then
            problem "The patchset has $n_pset patches, the dev tree $n_dev commits after $PRISTINE.
  Regenerate: git -C \"\$WINE_DEV_TREE\" format-patch --stdout $PRISTINE..HEAD > $PSET"
        elif printf '%s\n' "$changed" | grep -qx "$PSET"; then
            tmp=$(mktemp -d)
            content "$PSET" > "$tmp/patchset"
            git -C "$DEV" worktree add -q --detach "$tmp/verify" "$PRISTINE" 2>/dev/null
            if (cd "$tmp/verify" && git -c user.name=check -c user.email=check@localhost \
                    am -q "$tmp/patchset") >/dev/null 2>&1; then
                got=$(git -C "$tmp/verify" rev-parse 'HEAD^{tree}')
            else
                got="(it does not apply)"
            fi
            git -C "$DEV" worktree remove --force "$tmp/verify" 2>/dev/null
            rm -rf "$tmp"
            want=$(git -C "$DEV" rev-parse 'HEAD^{tree}')
            [ "$got" = "$want" ] || problem "The patchset applied to $PRISTINE gives tree $got, not the dev tree's $want.
  Regenerate it from the dev tree; never edit it by hand."
        fi
    fi

    # --- 3b. the identity the next dev-tree commit would ship -----------------------------------
    who="$(git -C "$DEV" config user.name) <$(git -C "$DEV" config user.email)>"
    if printf '%s\n' "$who" | grep -Eiq "$PLACEHOLDER|^ <>$|<>"; then
        problem "The dev tree's git identity is \"$who\": a patch committed there ships with it.
  Set your own: git -C \"\$WINE_DEV_TREE\" config user.name/user.email"
    fi

    # --- 4. uncommitted dev-tree work -----------------------------------------------------------
    dirty=$(git -C "$DEV" status --porcelain 2>/dev/null)
    [ -n "$dirty" ] && note "The dev tree has $(printf '%s\n' "$dirty" | wc -l) uncommitted file(s); that work is in no patchset:
$(printf '%s\n' "$dirty" | head -6 | sed 's/^/  /')"
fi

# --- 5. the build machine's copy of the port ------------------------------------------------------
if [ -n "$changed" ] && [ -n "$HOST" ] && command -v ssh >/dev/null 2>&1; then
    remote=$(timeout 20 ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST" \
        "cd $PORTS/app-emulation/wine && sha256sum patches/wine-$VER.patchset wine-$VER.recipe" \
        2>/dev/null | awk '{print $1}' | tr '\n' ' ')
    local_sums=$(sha256sum "$PSET" "$RECIPE" | awk '{print $1}' | tr '\n' ' ')
    [ -n "$remote" ] && [ "$remote" != "$local_sums" ] && note "The build machine's copy of the port differs from this repo; a build there would use the old files.
  Copy it: rsync -a --delete --exclude 'work-*' --exclude 'download/' app-emulation/wine/ <host>:<haikuports>/app-emulation/wine/"
fi

[ -n "$notes" ] && printf 'tools/check.sh notes (not blocking):\n%s' "$notes" | sed '2,$s/^\([^ ]\)/- \1/'
[ -n "$problems" ] || exit 0
printf 'tools/check.sh found:\n%s' "$problems" | sed '2,$s/^\([^ ]\)/- \1/'
exit 1
