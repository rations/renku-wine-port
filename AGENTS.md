# Working on renku-wine-port

Rules and workflow for anyone changing this port, person or coding agent. They are binding for
every change. README.md has the patch table, the build steps and the history; this file says
how to work.

## What this repo is

Wine for RenkuOS, packaged as a HaikuPorts port (`app-emulation/wine`): a recipe and a
patchset against the pinned upstream Wine tarball, built by `haikuporter` on RenkuOS. It holds
no Wine source. Almost every task is "change a patch", which means changing the dev tree and
regenerating the patchset, never editing the patchset.

**This repo is the source of truth.** A build machine's work tree and anyone's dev tree are
products of it. If they disagree with this repo, this repo is right.

A plausible guess here does not fail loudly. It gives a patch that still applies and still
builds, and silently drops a feature or ships a debug hack. If a rule and a deadline conflict,
the rule wins.

## Setting up

1. A RenkuOS machine to build on (a VM is fine) with a
   [haikuports](https://github.com/haikuports/haikuports) checkout and `haikuporter`, reachable
   with ssh. The recipe's `BUILD_REQUIRES` lists the toolchain; `haikuporter` says what is
   missing.
2. `cp tools/local.conf.example tools/local.conf` and fill it in: the dev tree's path, the
   build machine's ssh host, its haikuports path. `tools/local.conf` is not tracked.
3. `tools/devtree.sh <dir>` makes the dev tree: the recipe's tarball (SHA256 checked),
   committed and tagged `pristine-<version>`, plus the patchset as one commit per patch.
4. Set `user.name` and `user.email` in the dev tree to your own identity before committing
   there: the `From:` line of each patch ships in the patchset.

5. `git config core.hooksPath tools/git-hooks`: the pre-commit hook runs
   `tools/check.sh --staged` and refuses a commit that changes the port without a REVISION
   bump, a patchset that does not reproduce the dev tree, or a placeholder author.

Guard rails: `tools/status.sh` says where things stand (run it at the start of a session),
`tools/check.sh` finds the silent failures (run it before you stop), and
`tools/winework.sh status` lists work that exists only on the build machine. Claude Code runs
them by itself through the hooks in `.claude/`, which also refuse a hand edit of a patchset and
a `haikuporter -c` while the build machine holds unmirrored work. Other agents: run them
yourself, at the same points.

**Nothing from your own machine goes into a tracked file**: no home-directory paths, host
names, user names, passwords or keys. Machine settings go in `tools/local.conf`; documents
refer to the settings (`WINE_DEV_TREE`, `WINE_BUILD_HOST`) or to public URLs.

## 1. Research, never guess

Every API, struct field, enum value and package name is read from a real source before use:
not from memory, not from how Linux does it, not from what another Wine version did. If a fact
cannot be verified, stop and say so, in the commit body as well as in conversation. A flagged
unknown is acceptable; a silent guess is not.

Sources of truth:

- **The Be API on RenkuOS**: [RenkuOS/Source](https://github.com/RenkuOS/Source): `headers/os/`,
  `headers/posix/`, `src/kits/`, `src/servers/app/`. Haiku's source is secondary: RenkuOS may
  differ. What is installed on the build machine (`/boot/system/develop/headers/`) beats both.
- **Wine**: the dev tree is the source with this port's patches. Upstream history, blame and
  "did upstream change this?" come from [wine-mirror/wine](https://github.com/wine-mirror/wine).
  Wine's internal API changes between releases: a blog post or an old release is not
  evidence about the current one.
- **What `haikuporter` really compiled**: the work tree on the build machine, in particular its
  `include/config.h` for what `configure` detected. Before trusting a package, check that and
  `package list -i <hpkg>`.
- **Packaging**: neighbouring recipes in the haikuports checkout.
- **What a package provides or requires**: ask the package, on RenkuOS:
  `package list -i <hpkg>` for `provides:`/`requires:`, `package list -p <hpkg>` for files.
  Never guess a resolvable name.
- **This repo's history** (`git log`) and `docs/`: the design documents and `docs/probes/`, the
  programs that established the platform facts below. Rerun a probe rather than trust its
  recorded result on a new RenkuOS build.

### Platform facts already established

Verified with evidence (`docs/OSMESA-GL-PLAN.md`, `docs/NATIVE-DRIVER-PLAN.md`, `docs/probes/`).
Do not re-derive or contradict them without new evidence.

- Mesa on this platform has no DRI swrast and no Wayland EGL platform: `eglInitialize` fails.
  OSMesa gives headless desktop GL 4.5 compatibility (llvmpipe), pure software.
- The Vulkan loader exposes no surface extensions, so there is no WSI and no DXVK.
- `PFD_TYPE_RGBA` is 0 and is not the WGL pixel-type enum; use `WGL_TYPE_RGBA_ARB`.
- Cross-team BView embedding does not exist, and Wayland cannot position a plug-in window.
  Never propose XEmbed-style reparenting.
- Winelib modules did not run C++ static constructors (a patch fixes it), and there is no
  `/proc`.

## 2. Patch discipline

- **Never hand-edit a `.patchset`.** Edit the source in the dev tree, one commit per patch,
  then regenerate:
  `git format-patch --stdout pristine-<version>..HEAD > app-emulation/wine/patches/wine-<version>.patchset`.
  A hand edit desynchronizes the patchset, and the next regeneration silently reverts it.
- **Verify a regenerated patchset** before committing it: in a worktree at
  `pristine-<version>`, `git am` it, and `git rev-parse HEAD^{tree}` must equal the dev tree's.
  "It looks right" and `git apply --check` are not verification.
- **Bump `REVISION`** in the recipe whenever the recipe or the patchset changes. Never reuse a
  published revision: `haikuporter` and `pkgman` key upgrades off it.
- **The revision is compiled in.** `configure` sets
  `prefix = /packages/wine-<version>-<revision>/.self`, and some objects bake that path in.
  Re-running `haikuporter` alone after a bump recompiles none of them, and the package then
  fails on every machine but the build machine with `wine: could not load ntdll.so`. Build a
  changed port only with `tools/winework.sh package`, or with a clean `haikuporter -c` build.
  Either way, `tools/winework.sh checkpkg` must pass before the package is shipped. Binaries
  that hash identical to the previous revision's are the symptom, not reassurance.
- **Preserve upstream authorship.** The first patches are HaikuPorts' (on 11.21: 1–2 by X512,
  3 by Ken Mays; `tools/status.sh` lists the authors by patch). Their `From:` lines are the
  licence's attribution trail: never rewrite, reorder or absorb them.
- **Local patches carry a real author**, never a placeholder: check
  `git log --format='%h %an <%ae> %s' pristine-<version>..HEAD` in the dev tree.
- **Debug instrumentation goes last**, labelled as debug, so it can be dropped alone.
- **A new build dependency is declared in the recipe**, its name read off the package. A
  `configure` check that silently disables a feature when a header is missing is a trap.

## 3. Build loops

`haikuporter` applies the patchset with git, so its work tree
(`work-<version>/sources/wine-wine-<version>` in the port directory) is a git repo whose tree
is the patchset's, and every change made in it since is visible to git. `tools/winework.sh`
works on that tree:

- **A change** (seconds to minutes): `push` the files from the dev tree, `build` the target,
  test from the build tree without installing (`howto`).
- **A package** (minutes): commit in the dev tree, regenerate and verify the patchset, bump
  `REVISION`, then `package`. It syncs the work tree to the dev tree's HEAD, deletes only the
  objects that hold another revision's path, re-runs `haikuporter` without `-c`, and runs
  `checkpkg`.
- **A clean build** (`haikuporter -c`, everything recompiled) only for a new Wine version or a
  damaged work tree.

Invariants:

- **Never leave work only on the build machine.** `haikuporter -c` deletes its work tree
  without warning. `tools/winework.sh status` lists edits that exist nowhere else (exit 1),
  `pull` copies them into the dev tree. Never run `-c` or delete a work tree while `status`
  lists any.
- **Stop the build machine cleanly.** A hard stop loses recently written files on BFS (a fresh
  package came back zero-filled). After one, check the recent outputs and run `package`
  again; no clean build is needed.
- Compiling happens on RenkuOS, never on another OS: a Linux build proves nothing about this
  target. `configure` runs only inside `haikuporter`'s chroot.

## 4. A new Wine release

`tools/rebase.sh` finds the newest release upstream, imports it as `pristine-<version>`,
applies the patchset on a new branch in a worktree beside the dev tree, and prints which
driver interfaces changed (`WINE_GDI_DRIVER_VERSION` and the others, win32u, the reference
drivers winemac.drv and winewayland.drv): `winerenku.drv` follows those. Resolve conflicts
there, then `tools/rebase.sh --finish <version>` writes the recipe and the patchset. Build it
cleanly, test, update the README's patch table and history, commit. README.md, *Moving to a
new Wine release*, has the details.

## 5. Honest reporting

- Never report a build, a test or a milestone as passing without having run it. If a step was
  skipped, say so. "It should work" is not "it works".
- GUI behaviour is verified by a person running it on RenkuOS. Report what was run and what was
  seen, and by whom.
- When a patch is dropped, deferred or known broken, record it in its commit message and in the
  README table, not only in conversation.

## 6. Scope and provenance

- Keep patches minimal and single-purpose, so each can still be rebased onto a new Wine
  release, or dropped when upstream fixes the problem, years from now.
- Guard platform divergence with `#ifdef __HAIKU__` (the compiler's macro on RenkuOS) or a
  `configure` result. Never fork or delete an upstream file.
- Every local patch has a commit body that explains the failure it fixes and lists what it has
  not verified.
- Wine is LGPL 2.1 and these patches are derived work under the same licence. Keep licence
  headers and authorship.
- No Wine sources, tarballs or built `.hpkg` files in this repo. It stays a small, text-only
  port.
