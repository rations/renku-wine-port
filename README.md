# renku-wine-port

> This repository is the source of the `wine` package for RenkuOS. Built
> packages are on its [Releases](https://github.com/rations/renku-wine-port/releases) page, and
> [renku-audio](https://github.com/rations/renku-audio) lists the whole audio stack.

Patches for **Wine 11.21** on RenkuOS, built as a port with `haikuporter`. They exist so that
[vstbridge-renku](https://github.com/rations/vstbridge-renku) can run **Windows VST2 and VST3
plug-ins** inside [Kasen](https://github.com/rations/kasen) — the stock Wine package is
missing the fixes the bridge depends on.

To change the port, read [AGENTS.md](AGENTS.md) first: the rules and the workflow, for people
and coding agents alike. `tools/` has the scripts it describes.

> **This is the `renku-driver` branch: work in progress, not released.** It is the base for
> `winerenku.drv`, a native graphics driver that gives Windows programs real RenkuOS windows
> with no Wayland in between. The plan, its probes and its gates are in
> [docs/NATIVE-DRIVER-PLAN.md](docs/NATIVE-DRIVER-PLAN.md). **The driver works but is not
> finished (patches 16-20):** Windows programs open in native RenkuOS windows and take
> keyboard and mouse input, and the windows can be moved, resized, minimized, zoomed and
> closed. Keys type what the RenkuOS keymap says, AltGr included. The pointer stays RenkuOS's
> own arrow. Text copied in a Windows program pastes into RenkuOS programs and the other way
> round. Not yet: accent (dead) keys, images on the clipboard, OpenGL. configure runs `--without-wayland --without-x --without-opengl`. OpenGL
> is off because win32u's EGL path hangs explorer on RenkuOS; it comes back through the
> driver. The released package
> (Wine 11.19, Wayland) is on `master`.

**This repository is the source of truth for these patches.** They were offered upstream and
declined (the upstream ports tree does not accept AI-assisted contributions), so they are maintained here
instead. Nothing else is authoritative — not the build machine, not any working tree. See
[Where things live](#where-things-live).

The repo carries no Wine sources, only the recipe and the patchsets (~450 KB). `haikuporter`
downloads the upstream tarball (checksum pinned in the recipe) and applies the patchset to it.

## Layout

```
app-emulation/wine/            drop-in replacement for the haikuports port directory
  wine-11.21.recipe            REVISION=6
  patches/
    wine-11.21.patchset        the 20 patches below (this is the deliverable)
  additional-files/
    wine.rdef.in               Deskbar icon/version resource template
tools/                         scripts for maintaining the port (settings: tools/local.conf.example)
  devtree.sh                   make the dev tree: the tarball + the patchset, as git commits
  winework.sh                  the build-machine loop: push, build, test, sync, package, check
  rebase.sh                    move the port to a new Wine release
  check.sh                     the checks: REVISION, patchset vs dev tree, authors, leftovers
  status.sh                    where things stand: port, dev tree, sources, build machine
  git-hooks/pre-commit         runs check.sh --staged (git config core.hooksPath tools/git-hooks)
  common.sh                    shared settings, sourced by both
docs/
  NATIVE-DRIVER-PLAN.md        design, probes and gates for the native driver (winerenku.drv)
  OSMESA-GL-PLAN.md            design + test plan for the software-GL work
  probes/                      standalone programs establishing both plans' facts
attic/                         kept for reference, not built
  winerenku-cursor-shapes.diff Windows cursor shapes for winerenku.drv, not applied
.claude/                       Claude Code hooks that run tools/status.sh and tools/check.sh
AGENTS.md                      the rules and workflow, for people and coding agents
```

## The patchset

Twenty patches. **1–3 are from the upstream port** (see *Licensing and credit*), with original authorship preserved in each
`From:` line. **4–20 are local.**

| # | Patch | Author | Why |
|---|---|---|---|
| 1 | Add Wayland input header | X512 | upstream. **Not compiled** (configure runs `--without-wayland`), but required: patch 2's winewayland hunks include it, and makedep resolves every quoted `#include` of every module, built or not, so configure fails without it.
| 2 | 11.8 patches | X512 | upstream — the bulk of the port. Rebased onto 11.19 and then 11.21; the rebase notes are in the commit body. Its two winewayland hunks are kept but not compiled (configure runs `--without-wayland`).
| 3 | Opensound support | Ken Mays | upstream — audio |
| 4 | Run static initializers in Winelib modules | local | Winelib modules never ran their C++ static constructors. vstbridge's Wine host is a C++ Winelib module, so without this it starts with uninitialised globals. **This is the essential one.** |
| 5 | Do not block in the SIGSEGV handler | local | The port's handler ended in a blocking `fgetc(stdin)` so a developer could inspect the fault. Anything without a usable stdin — a DAW, any GUI app, anything from the Deskbar — hung there instead of letting Wine's handler recover. |
| 6 | loader: find the loader path via `find_path()` | local | RenkuOS has no `/proc`, so `get_self_exe()` returned NULL and the loader resolved `argv[0]` relative to the current directory — a bare `wine` from `PATH` failed with `could not load ntdll.so`. Fixed with `find_path(B_FIND_PATH_IMAGE_PATH)`. |
| 7 | dxgi: wait for a refresh interval in `IDXGIOutput::WaitForVBlank()` | local | `WaitForVBlank()` was a stub printing a FIXME and returning `E_NOTIMPL` immediately. JUCE 8's Direct2D renderer drives all repainting from a dedicated thread that calls it in a loop, so with no wait that thread spun — measured at 62,024 calls in ~25 s, saturating a core — and the editor rendered one frame then never responded to input again. Now sleeps to the next refresh-interval boundary. Not specific to RenkuOS: the same gap is why these plug-ins are reported as needing DXVK on Linux, DXVK shipping its own DXGI with a real vblank wait. |
| 8 | sechost: report no failure actions in `QueryServiceConfig2W` | local | `services.exe` keeps no failure actions, and `QueryServiceConfig2W` failed with `ERROR_INVALID_LEVEL` for them. WiX's ServiceConfig custom action (used by iLok's PACE License Support installer) reads them before setting them and aborts the whole install when the query fails. Now reports what Windows reports for a service with none: the structure, zeroed. Not specific to RenkuOS; carried from the user's ARM64 Wine build. |
| 9 | msiexec, rundll32: declare Windows 10 compatibility in their manifests | local | Without a `supportedOS` list, Wine reports Windows 8 to the process an MSI custom action runs in, as Windows does. Installers that check for Windows 10 there (iLok's PACE License Support) refuse to install. Adds the same list Windows' own `msiexec.exe`/`rundll32.exe` carry. Not specific to RenkuOS; carried from the user's ARM64 Wine build. On a kernel without LDT support these two only help 64-bit installers, since 32-bit Windows programs need patch 13 and a kernel with LDT support. |
| 10 | server: detach asyncs from a queue before terminating them in `free_async_queue()` | local | wineserver aborted (`release_object: obj->refcount`) every time a prefix shut down, so registry changes since the last 30-second periodic save were lost — `wine regedit file.reg` never persisted, which broke licences kept in the registry (MachineGuid, per-plugin IDs) — and each crash orphaned the prefix's `services.exe`. When an APC cannot be delivered, `async_terminate()` completes the async through `async_set_result()`, which unlinked it and dropped the queue's reference while `free_async_queue()` still held it, and `free_async_queue()` then dropped it again. On RenkuOS delivery always failed (before patch 11) for a thread not already waiting, because the client reports no Unix thread id (`get_unix_tid()` returns -1) and wineserver cannot signal it; `services.exe` closing its listening `svcctl` pipe at shutdown hits this every time. Not specific to RenkuOS in itself. |
| 11 | ntdll, server: deliver signals to specific threads | local | `get_unix_tid()` had no branch for this OS and returned -1, so wineserver could never signal one thread: an APC queued to a thread that was not already waiting was destroyed and the pending I/O completed with `STATUS_ALERTED`. Named-pipe readers saw `ERROR_ALERTED` — WiX Burn's engine and UI lost their pipe (`0x800702e3`). Reports `find_thread(NULL)` and signals with `send_signal()`. Also removes the trigger of patch 10's crash. |
| 12 | winegcc: do not link with `-ldl` | local | RenkuOS has no `libdl` (`dlopen` is in libroot), so every Winelib link failed unless a hand-made empty `libdl.a` stub was installed. |
| 13 | ntdll: support WoW64 (32-bit code) | local | 32-bit Windows code needs a 32-bit code selector and a per-thread `%fs` segment for its TEB; every 32-bit program exited with "No LDT support on this platform". Uses new `arch_thread_control()` calls (`THREAD_SET_LDT_ENTRY`, `THREAD_SET/GET_FS_BASE`, `THREAD_GET_GS_BASE`) that **only a locally built kernel provides** (per-team LDT, FS/DS/ES kept per thread, FS saved across signals); FreeBSD's model. On a stock kernel 32-bit programs exit with Wine's "No LDT support" error and 64-bit ones are unaffected. Enables the iLok PACE installer's 32-bit parts (Bonjour, custom actions) and 32-bit installers in general. |
| 14 | ntdll, server: use the xattr functions from libgnu | local | Patch 2 disabled `HAVE_SYS_XATTR_H`, so reparse points (Windows symbolic links) failed with `ENOSYS`. Wine Mono's installer creates symlinks (`CREATELINKS64`, error 1603), so Mono never installed and every .NET custom action failed — iLok's License Support MSI with `0x80070643`. configure now finds `fgetxattr` in libgnu, which maps xattrs to BFS attributes. |
| 15 | ntdll, server: support hardware breakpoints | local | Copy protection (PACE/iLok: Neural DSP, Toontrack and others) points Dr0–Dr3 at a probe with `SetThreadContext` and expects `EXCEPTION_SINGLE_STEP` when it runs; a probe that does not trap marks the process as tampered and the licence check fails later ("no activation", or `LoadLibraryW` "DLL initialization failed" for a plug-in). The server cannot set debug registers on RenkuOS (no ptrace), so each thread loads its own through `THREAD_SET_DEBUG_REGISTERS`, which **only the locally built kernel provides**; `trap_handler` reports DR6 with the reserved bits set, as Windows does, which PACE checks. On a stock kernel the breakpoints are ignored with a warning. (Developed as "patch 18" in the 11.19 numbering; an SMBIOS patch numbered 17 there was dropped because it changed the hardware IDs licences are bound to.) |
| 16 | winerenku.drv: add a native RenkuOS (Be API) graphics driver | local | **Work in progress, stage 1 of the plan.** Every other Wine port for Haiku-family systems draws through Wayland, which on RenkuOS is an in-process server that turns the protocol back into BWindows. This driver uses the Be API directly: each shown top-level window gets a BWindow (titled for `WS_CAPTION` windows, so RenkuOS draws the tab and frame; borderless otherwise), Wine's window surface is copied into a `B_RGB32` BBitmap, and the screen is reported from `BScreen`. The BApplication starts on its own thread the first time it is needed, and every Be thread starts with Wine's signals blocked (without that, the sigprobe test shows a SIGINT killing a window's thread). configure builds it on Haiku and fails if the Be headers are missing; `--without-renku` turns it off. Verified: notepad opens in one native window and draws correctly (title, menu bar, edit area, caret). **Not done yet:** keyboard and mouse input, focus, moving/resizing/closing, the Deskbar-free work area, window shapes and layering, OpenGL. A crash on a Be thread freezes that thread silently. |
| 17 | explorer: try the RenkuOS driver first | local | explorer loads the first driver in `default_driver` that exists; `renku` now comes first. explorer is Windows (PE) code and cannot test `__HAIKU__`, but it does not need to: where `winerenku.drv` is not built the load fails and the next name is tried, as `mac` already is everywhere but macOS. |
| 18 | winerenku.drv: keyboard and mouse input, activation, and the decorator | local | **Work in progress, stage 2 of the plan.** Be threads cannot call Wine, so they queue events and one Wine thread passes them to win32u. Mouse input flushes the moves win32u holds back (without that, a drag reached the program only on release). Keys are sent as scancodes, from a table generated from RenkuOS's own scancode map, and are taken before BWindow's shortcuts (Command+Q, Control+Tab, Print Screen), so programs get them all. RenkuOS activations become Wine's foreground window and the other way round, without the two undoing each other; menus and tooltips, including JUCE's unowned ones, never take the focus. The decorator's close, minimize, zoom and resize follow the window style and become Wine's system commands, and moves and resizes are read back from the window. Also: a resized window keeps its picture until Wine has painted the new one, and click-through layered windows (drop shadows) are not shown, since RenkuOS cannot blend windows. Verified on notepad and Amped - Roots. The pointer stays RenkuOS's own arrow; programs' cursor shapes are not used, by choice. **Not done yet** (keyboard layouts: patch 19): showing a normal window without activating it, a work area that leaves out the Deskbar, Deskbar "Quit". |
| 19 | winerenku.drv: keyboard layout from the RenkuOS keymap | local | **Work in progress, stage 2 of the plan.** Without it win32u typed with its US tables whatever the RenkuOS keymap was: on German the Z key typed y and there was no AltGr. The driver now builds Wine's layout tables from the keymap in Keymap preferences, as winewayland does from xkb. Letters, digits and the + , - . keys get the virtual key of their character (so Ctrl+Z is the key marked Z), other keys keep their US one where free. Characters come from the keymap's normal, Shift, Caps Lock and Option tables. When the keymap makes Right Alt an Option key (German, French, US-International), Right Alt is sent as Windows sends AltGr, Left Ctrl + Right Alt. Verified with [docs/probes/kbdprobe.c](docs/probes/kbdprobe.c) on the US, US-International, German and French keymaps, and by typing in notepad on US-International (AltGr characters, Alt+F, Ctrl shortcuts). **Not done yet:** accent (dead) keys type the accent at once, since win32u's tables have no dead keys; a keymap change applies to programs started after it. |
| 20 | winerenku.drv: text clipboard, both ways | local | **Work in progress, stage 2 of the plan.** Wine's clipboard and RenkuOS's were separate. The driver now gives explorer's clipboard manager window its window procedure, as winemac does: a looper watches RenkuOS's clipboard and puts new text on Wine's as `CF_UNICODETEXT`, and text a Windows program copies goes to RenkuOS's as `text/plain` UTF-8 (a copy of anything else empties it, as on Windows). Line ends become CR LF and LF. Verified in the build VM between notepad and StyledEdit, both ways, from the menus and with Alt+C / Ctrl+C / Ctrl+V. **Not done yet:** images and other formats; non-ASCII text and line ends not checked yet. |

On RenkuOS, patches 13 and 15 do nothing: its kernel has neither the LDT calls nor
`THREAD_SET_DEBUG_REGISTERS`, so 32-bit programs exit and
hardware breakpoints are ignored. Only 64-bit Windows programs run. The kernel side is
maintained separately, to be submitted to RenkuOS.

Patch 6 removes the need for the `export WINELOADER=/boot/system/bin/wine` workaround earlier
Wine builds required in `~/config/settings/profile`: winegcc's generated `.exe` wrapper
scripts fall back to invoking a bare `wine`, which now works from any directory.

### Dropped in the 11.21 rebase

This branch drops what only served the Wayland path, or does nothing on RenkuOS:

- **winewayland: add OSMesa software OpenGL backend** (local, was patch 8). It lives inside
  `winewayland.drv`, which this branch no longer builds. The OSMesa approach, and everything
  [docs/OSMESA-GL-PLAN.md](docs/OSMESA-GL-PLAN.md) established for it, returns inside the
  native driver (NATIVE-DRIVER-PLAN.md, phase 6). Until then **D3D/GL-drawn plug-in editors
  have no software GL on this branch.** The patch is preserved on the dev tree's
  `renku-11.21-wayland` branch, rebased onto 11.21 unchanged.
- **Disable Bluetooth on Beta 5** (Peppersawce, was patch 3). It only acts when
  `B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5`; RenkuOS defines
  `B_HAIKU_VERSION_1_PRE_BETA_7` (`headers/config/HaikuVersion.h`), so it compiled to nothing.

Two upstream changes needed conflict resolution, both in `configure.ac` and recorded in the
commit bodies: patch 2 (upstream split the Wayland dependency check into one check per
library) and patch 3 (upstream now uses `AC_CHECK_MEMBER` for the OSS check).

### Dropped in the 11.19 rebase

- **Define `get_current_thread_data`** (Peppersawce) — upstream Wine made the function
  unconditional in wine-11.11, so the patch has nothing left to do.
- **ntdll: debug — name the faulting caller by scanning the stack** — development
  instrumentation from the 11.8 GL work, never a fix. It printed to stdout on every fault.

## Building

On the target RenkuOS machine, with a [haikuports](https://github.com/haikuports/haikuports)
checkout and `haikuporter` configured:

```sh
# 1. Overlay this port onto your haikuports checkout
cp -a app-emulation/wine/. ~/haikuports/app-emulation/wine/

# 2. Build (about 30 minutes with -j12 in the RenkuOS build VM; builds i386 and x86_64)
cd ~/haikuports
haikuporter -y -j8 wine-11.21
```

The package lands in `~/haikuports/packages/wine-11.21-6-x86_64.hpkg`.

That plain `haikuporter` build is right for a first build. **Do not rebuild a changed port
that way:** without `-c`, haikuporter does not re-apply a changed patchset, and the revision is
compiled into the binaries. `configure` records `prefix = /packages/wine-<version>-<revision>/.self`,
and Wine bakes that into `bindir`/`libdir`; the loader then resolves `ntdll.so` through it.
Re-running haikuporter after a `REVISION` bump re-runs `configure` (so the `Makefile` says the
new revision) but recompiles nothing, because no source changed, and the package ships
binaries still pointing at the **previous** revision's package directory. That package works on
any machine where the old revision is still installed, and fails everywhere else with:

```
wine: could not load ntdll.so: No such file or directory
runtime_loader: Cannot open file /packages/wine-11.8-8/.self/lib/wine/x86_64-unix/ntdll.so
```

So a changed port is always built clean, and left alone until it ends:

```sh
haikuporter -y -c wine-11.21 && haikuporter -y -j12 wine-11.21   # 19 min in the build VM
```

That is only needed for a package. Testing a change needs no package at all (below), so a
clean build comes at a milestone, not per change. Check the package before shipping it:
`tools/winework.sh checkpkg` extracts it and fails if a path of any other revision is compiled
in, as text or as UTF-16 (mscoree, mshtml and appwiz.cpl hold it in wide strings), or if none
holds the recipe's. On an installed system:

```sh
strings -a /boot/system/bin/wine | grep packages/wine   # must match the installed revision
```

Binaries whose hashes are *identical* to the previous revision's are not reassurance that the
payload is unchanged; they are the symptom.

**Stop the build VM cleanly.** A hard stop loses recently written file data on BFS (a fresh
hpkg came back zero-filled: `package list` reported "Invalid magic"), but not the older build
tree. After one, check the recent outputs. If only the package was lost, and the work tree
was built for the same `REVISION`, `haikuporter -y -j12 wine-11.21` without `-c` makes it again:
`flag.build` makes it skip the build. Otherwise build clean.

Build dependencies are declared in the recipe (`BUILD_REQUIRES`/`BUILD_PREREQUIRES`) and include
llvm21 + clang + lld for the PE cross-build; `haikuporter` will tell you what is missing.
When installing them by hand with `pkgman`, `devel:libSDL2_2.0` is not found by name on
RenkuOS: install `libsdl2_devel`. This branch declares no Wayland, xkb, EGL or OSMesa
dependencies: nothing built here uses them.

### Iterating without rebuilding everything

`haikuporter` applies `.patchset` files **with git**, so `work-11.21/sources/wine-wine-11.21`
on the build machine is a git repo whose `HEAD^{tree}` is the patchset's tree, and every change
made in it since is visible to git. `tools/winework.sh` builds the loop on that. Set it up once:
copy `tools/local.conf.example` to `tools/local.conf` and fill in your dev tree (see
*Changing a patch*), the build machine's ssh host, and its haikuports path.

```sh
tools/winework.sh push dlls/winerenku.drv/window.c   # dev tree -> build machine
tools/winework.sh build dlls/winerenku.drv/all       # incremental make there (seconds)
tools/winework.sh howto                              # test without installing (below)
tools/winework.sh status    # edits that exist only on the build machine (exit 1 if any)
tools/winework.sh pull      # copy those into the dev tree
tools/winework.sh sync      # make the build machine's tree equal the dev tree's HEAD
tools/winework.sh verify    # check that it does
tools/winework.sh checkpkg  # check the built package of the recipe's REVISION
```

`push` and `build` touch only the files you name and the targets you ask for, and only while
no haikuporter build is running. Never change a work tree while haikuporter is building in it.

Nothing needs to be installed to test a change. Wine sets its internal `build_dir` when its
`ntdll.so` sits under `<tree>/dlls/ntdll`, and then omits the installed `/boot/system/lib/wine`
from the DLL search path entirely, so the build tree wins outright:

```sh
export WINELOADER=<work tree>/loader/wine
```

(`WINEDLLPATH` does *not* work for this: its entries are appended *after* the installed
directory, so an installed builtin always wins.)

> **`haikuporter -c` deletes `work-11.21/sources/` without warning.** An edit that exists only
> there is one command from being lost; that has happened twice here. `tools/winework.sh
> status` lists such edits and `pull` copies them into the dev tree. Every source change
> ultimately belongs in the patchset (see below).

### Installing

```sh
pkgman install ./wine-11.21-6-x86_64.hpkg
```

- **On RenkuOS, build it there.** A package built on RenkuOS requires the build machine's
  system package as a floor (RenkuOS build 66 gave `haiku>=r1~beta6_hrev60072_422-1`;
  RenkuOS's system package is still named `haiku`).
  `haikuporter` bakes the build machine's system revision in as a floor, so an older system
  refuses to install it; newer is fine, as this is a userland package, not a kernel add-on. A
  package built on another system with the same `wine-11.21-1` label does not install here,
  so do not mix builds in one repository or on one machine.
- Runtime dependencies, all pulled from the system repositories automatically. If the target machine is
  offline, bring them along too.

  | Declared since | Resolvable | Package |
  |---|---|---|
  | always | `lib:libfreetype` | `freetype` |
  | always | `lib:libvulkan` | `vulkan` |
  | 11.19 REVISION 4 | `lib:libgnutls` | `gnutls` |
  | 11.8 REVISION 10 | `lib:libusb_1.0` | `libusb` |

  On `master` (11.19) the package also requires `lib:libOSMesa` (`mesa`) and the Wayland
  libraries (`wayland`, `wayland_server`, `libxkbcommon`). This branch builds neither
  `winewayland.drv` nor the OSMesa backend, so it requires none of them. The native driver
  will add its own dependencies, each read off the package that provides it.

  The last row was `lib:libusb` through REVISION 8, which resolves to `libusb_compat` — the
  libusb-0.1 API. `wineusb.so` links `libusb-1.0.so.0`, provided by the `libusb` package as
  `lib:libusb_1.0`. Nothing in the port links libusb-0.1 at all, so the old entry pulled in a
  library no module used while leaving the one `wineusb.so` needs undeclared.

**Lesson from 11.8 REVISION 8 and earlier, which declared none of the Wayland libraries: that made the package
un-GUI-able on a machine that did not already have them.** `winewayland.so` links
`libwayland-client`, `libwayland-egl`, `libxkbcommon` and `libxkbregistry` directly
(`objdump -p` shows them as `NEEDED`), and a recipe's `REQUIRES` is hand-written —
nothing scans the binaries for you. On a machine with those libraries already installed for
other reasons the package worked; on a clean one it installed happily and then had no display
driver. See *Graphics driver* below for the failure signature.

A recipe's `REQUIRES` must list everything the built modules link or dlopen; nothing checks it for you.

## Changing a patch

Do not edit the patchset by hand. Rebuild it from a git tree, which is how the current one was
produced:

```sh
# One-time: make the dev tree from this repo: the recipe's tarball (SHA256 checked), committed
# and tagged pristine-11.21 (tree db03e4c9..., the same as upstream's wine-11.21 tag), then the
# patchset applied as one commit per patch on branch renku-11.21
tools/devtree.sh ~/src/wine-renku-dev

# Then, for every change: edit, commit, regenerate, commit the patchset here
git format-patch --stdout pristine-11.21..HEAD \
  > /path/to/renku-wine-port/app-emulation/wine/patches/wine-11.21.patchset
```

Each commit is one patch, in order. Bump `REVISION` in the recipe whenever the patchset or
recipe changes — never reuse a published revision.

To verify a regenerated patchset reproduces the tree it came from:

```sh
git worktree add --detach /tmp/verify pristine-11.21
cd /tmp/verify && git am /path/to/patches/wine-11.21.patchset
git rev-parse HEAD^{tree}   # must equal the dev tree's HEAD^{tree}
```

## Moving to a new Wine release

```sh
tools/rebase.sh             # or: tools/rebase.sh <version>
```

With no version it takes the newest release tag of the repository the recipe downloads from,
and does nothing if the port is already on it. Otherwise it downloads that release's tarball,
commits it into the dev tree as a new root tagged `pristine-<version>` (the dev tree's checkout
and uncommitted work are not touched), and applies the current patchset with `git am -3` on a
new branch `renku-<version>`, in a worktree beside the dev tree. It prints the driver interface
versions (`WINE_GDI_DRIVER_VERSION` and the others) of both releases, and what changed between
them in the interfaces and in the reference drivers (win32u, winemac.drv, winewayland.drv,
explorer): `winerenku.drv` has to follow those changes.

When a patch does not apply, it stops and names the patch and the files. Resolve them in the
worktree, `git add`, `git am --continue`; a patch upstream made unnecessary is dropped with
`git am --skip` and the README says so. Then:

```sh
tools/rebase.sh --finish <version>
```

It regenerates the patchset, checks that it applies to `pristine-<version>` and reproduces the
branch's tree, renames the recipe to `wine-<version>.recipe` with the new `CHECKSUM_SHA256` and
`REVISION="1"`, and replaces the patchset. Nothing is committed: build it cleanly (a new tarball
needs `haikuporter -c`), test, update the patch table and revision history here, and commit.

## Where things live

| | Role |
|---|---|
| **this repo** | **Authoritative.** The recipe and patchsets, in git. If it is not committed here, it is not kept. |
| the dev tree (`WINE_DEV_TREE`) | Working tree for editing source and regenerating the patchset, made by `tools/devtree.sh`: the pristine tarball tagged `pristine-<version>`, and the patches as commits on branch `renku-<version>`. Regenerable; safe to delete and recreate. |
| the build machine's `<haikuports>/app-emulation/wine/` | Build machine only: a RenkuOS install (a VM is fine) of the release the package is built for. Copy the port in, build it clean, copy the `.hpkg` out. Has no git history of its own: anything edited only there is one `haikuporter -c` from gone (`tools/winework.sh status`). |

## Graphics driver

**On this branch the driver is `winerenku.drv` (patches 16 and 18), and it is unfinished.**
Patch 17 puts it first in explorer's default list (`renku,mac,x11,wayland`,
`programs/explorer/desktop.c:46`), so a prefix with no `Graphics` setting uses it. Windows
programs open in native RenkuOS windows, take keyboard and mouse input, and are moved, resized,
minimized, zoomed and closed with the RenkuOS decorator. What is still missing is listed under
patch 18 above; the stages are in [docs/NATIVE-DRIVER-PLAN.md](docs/NATIVE-DRIVER-PLAN.md).

Like every builtin DLL outside prefix creation, the driver loads only if the prefix's
`system32` has a placeholder file for it, which `wineboot` writes when it creates or updates a
prefix. A prefix created before patch 16 has none until it is updated. Wine updates a prefix
by itself on the first run after `wine.inf` changes (`programs/wineboot/wineboot.c:1697`), so
installing a new package should be enough. This hasn't been tried with an older prefix yet.
The exception is a prefix whose `.update-timestamp` reads `disable`: it is never updated, so
explorer cannot load the driver and the prefix behaves as described under *When no driver
loads at all*.

The `null` driver is part of win32u, not a module (`programs/explorer/desktop.c:1030`), so it is
still available, and it is still the right setting for a prefix that only hosts bridged
plug-ins:

| Setting | Result |
|---|---|
| `null` | vstbridge captures the plug-in's window and repaints it inside the host's FX window (Kasen's). The `null` driver declines to create a driver window surface, which sends win32u down its offscreen path: the window still draws and still receives `WM_PAINT`, but never reaches a display. Not yet tested on this branch. |
| (unset) | `winerenku.drv`: native windows. |

```sh
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
```

The released 11.19 package on `master` uses `winewayland.drv` through RenkuOS's in-process
Wayland server. Its README describes that arrangement and the dependencies it needs.

### When no driver loads at all

`explorer.exe` picks the driver by calling `LoadLibraryW()` on each name in its list (for example `winewayland.drv`) and writing the
result to `GraphicsDriver` under `HKLM\System\CurrentControlSet\Control\Video\{guid}\0000`
(`programs/explorer/desktop.c:1001,1040,1066`). If the load fails it writes `DriverError`
instead (`desktop.c:1069`), and `win32u` falls back to a null driver whose `nodrv_CreateWindow`
fails **every** window creation after logging one line (`dlls/win32u/driver.c:762-774`):

```
err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be loaded.
err:winediag:nodrv_CreateWindow L"Make sure that your display server is running and that its variables are set."
```

What this looks like from the outside is *not* a crash:

- Wine runs and `wine --version` works.
- A bridged plug-in scans and loads, and appears in Kasen's FX window list — but opening its
  editor shows nothing.
- **The "The Wine configuration in ... is being updated, please wait..." dialog never appears on
  first run.** That text is `IDD_WAITDLG`, a dialog resource (`programs/wineboot/wineboot.rc:45`),
  so it is the first casualty. Its absence on a fresh prefix is the cleanest early signal that no
  display driver loaded.

  **The prefix is still created correctly** — the dialog is decorative.
  `update_wineprefix()` does its work in `rundll32` child processes and waits on the *process*
  handle, not the window; `show_wait_window()`'s return value is never checked, and
  `install_root_pnp_devices()` and `update_user_profile()` run afterwards regardless
  (`programs/wineboot/wineboot.c:1511-1516,1660-1690`). So there is no need to delete and
  recreate `~/.wine` once a display driver is available.

To confirm on a suspect machine:

```sh
wine reg query 'HKLM\System\CurrentControlSet\Control\Video' /s | grep -i driver
WINEDEBUG=+winediag wine notepad 2>&1 | head       # the ERR above, if any
```

## Revision history

### wine-11.21 (branch `renku-driver`, not released)

| REVISION | Notes |
|---|---|
| 6 | Patch 20 (`winerenku.drv` text clipboard, both ways). The patchset applies to pristine 11.21 and gives tree `99051cfc`, the dev tree's. Verified before packaging, from the work tree on the RenkuOS build VM, by the user: text copied in StyledEdit pastes into notepad and text copied in notepad pastes into StyledEdit, from the menus and with Alt+C / Ctrl+C / Ctrl+V (Alt+V was taken by QEMU's own window). **Built** 2026-11-11 on that VM, clean (`haikuporter -y -c` then `-y -j12`): all 20 patches applied from the committed patchset; the work tree's `HEAD^{tree}` is `99051cfc`; `tools/winework.sh checkpkg` finds `/packages/wine-11.21-6/` 27 times, as text and as UTF-16, and no other revision's path; the package contains `winerenku.drv` (i386 and x86_64 PE) and `x86_64-unix/winerenku.so`, and its requires are unchanged (the recipe changed only REVISION). **Not installed or run as a package yet.** |
| 5 | Patch 19 (`winerenku.drv` keyboard layout from the RenkuOS keymap). The patchset applies to pristine 11.21 and gives tree `2aa3600d`, the dev tree's. Verified before packaging, from the work tree on the RenkuOS build VM: kbdprobe matches the US, US-International, German and French keymaps; the user typed in notepad on US-International, AltGr characters included. **Built** 2026-11-11 on that VM with `haikuporter -c` then `-y -j12` (the first build of this revision was lost: the VM was stopped uncleanly and the fresh hpkg came back zero-filled, so it was rebuilt): all 19 patches applied from the committed patchset; the work tree's `HEAD^{tree}` is `2aa3600d`; the extracted package holds `/packages/wine-11.21-5/` in 7 files and no other revision's path; it contains `winerenku.drv` (i386 and x86_64 PE) and `x86_64-unix/winerenku.so`, and its requires are unchanged from REVISION 4. Clean compile 10 min, package creation 9 min. **Not installed or run as a package yet.** |
| 4 | Patch 18 (`winerenku.drv` input, activation and the decorator). The patchset applies to pristine 11.21 and gives tree `21a34855`, the dev tree's. Verified before packaging, from the work tree on the RenkuOS build VM, by the user: notepad and Amped - Roots (JUCE 6.1.5) take typing, mouse, menus and dialogs, and are moved, resized, zoomed, minimized and closed with the decorator. **Built** 2026-11-11 on that VM with `haikuporter -c` then `-y -j12`: all 18 patches applied from the committed patchset; the work tree's `HEAD^{tree}` is `21a34855`; `bin/wine` and `ntdll.so` carry `/packages/wine-11.21-4`; the package contains `winerenku.drv` (i386 and x86_64 PE) and `x86_64-unix/winerenku.so`, and its requires are unchanged from REVISION 3. **Not installed or run as a package yet.** |
| 3 | Patches 16 (`winerenku.drv`, first stage: native windows that draw, no input) and 17 (explorer tries `renku` first), and `lib:libstdc++` in REQUIRES, because the driver is C++. The patchset applies to pristine 11.21 and gives tree `b811720b`, the dev tree's. Verified before packaging, from the work tree on the RenkuOS build VM (hrev60072+518): notepad opens in one native window with its title, menu bar, edit area and caret drawn correctly, and the sigprobe test passes 15/15. **Built** 2026-11-11 on that VM with `haikuporter -c` then `-y -j12`: all 17 patches applied from the committed patchset; the work tree's `HEAD^{tree}` is `b811720b`; the Makefile has `RENKU_LIBS = -lbe -lstdc++` and `config.h` leaves `SONAME_LIBEGL` undefined; `bin/wine` and `ntdll.so` carry `/packages/wine-11.21-3`; the package contains `winerenku.drv` (i386 and x86_64 PE) and `x86_64-unix/winerenku.so`, and requires `haiku>=r1~beta6_hrev60072_518-1`, `lib:libfreetype`, `lib:libgnutls`, `lib:libstdc++`, `lib:libusb_1.0`, `lib:libvulkan`. **Not installed or run as a package yet.** |
| 2 | `--without-opengl`: with `SONAME_LIBEGL` defined, explorer hung for good in win32u's EGL setup, so no prefix could be created. **Built** 2026-11-10 on the RenkuOS build VM. It was used only from the work tree, for the sigprobe test and the driver work, and was never installed. Its recipe change was committed together with REVISION 3. |
| 1 | Rebase onto Wine 11.21 (tarball SHA256 `046cb051…cfb8029`, tree `db03e4c9`, the same as upstream's `wine-11.21` tag) as the base for the native driver. Fifteen patches: the 11.19 set minus the winewayland OSMesa backend and "Disable Bluetooth on Beta 5". configure runs `--without-wayland --without-x`, and the Wayland, xkb, EGL and OSMesa dependencies are gone from the recipe. Two `configure.ac` conflicts resolved (patches 2 and 3; notes in the commit bodies). The patchset applies to pristine 11.21 and gives tree `ff313c87`, the dev tree's. A first build attempt without HaikuPorts' Wayland input header failed in configure (makedep, see patch 1), so it is kept. Driver interface versions unchanged from 11.19 (gdi 110, opengl 41, vulkan 48). **Built** 2026-11-10 on the RenkuOS build VM (hrev60072+518) with `haikuporter -c` then `-y -j12`, 19 minutes: the work tree's `HEAD^{tree}` is `ff313c87`; `config.h` has `HAVE_SYS_XATTR_H 1`, the Makefile `XATTR_LIBS = -lgnu`, empty `WAYLAND_CLIENT_LIBS`/`XKBCOMMON_LIBS`; the package contains `wineoss.so` and no `winewayland.so`; `ntdll.so` and the `bin/wine` launcher carry `/packages/wine-11.21-1`; it requires `haiku>=r1~beta6_hrev60072_518-1`, `lib:libfreetype`, `lib:libgnutls`, `lib:libusb_1.0`, `lib:libvulkan`. configure still found `libEGL` in the build chroot (`SONAME_LIBEGL` is defined) although `devel:libEGL` is no longer declared, so win32u will try to `dlopen` it and carry on without it if it is missing. **Not installed or run anywhere.** |

### wine-11.19

| REVISION | Notes |
|---|---|
| 4 | Patch 17 (hardware breakpoints, for PACE/iLok copy protection), and `lib:libgnutls` in REQUIRES: Wine dlopens it, so haikuporter could not see the dependency, and without it TLS (and iLok sign-in) failed. Patch 17 was verified from the work tree on hrev60247: the Windows breakpoint tests pass, and the Fortin NTS and Archetype Gojira X VST3s load licensed in the DAW. The patchset applies to pristine 11.19 and gives tree `26fbb2d8`. **Built** 2026-10-16 with `haikuporter -c` on stock hrev60256-1: all 17 patches applied from the committed patchset; the package requires `haiku>=r1~beta6_hrev60256-1` and `lib:libgnutls`. **Verified installed** (VM, the 0.5 stack installer on the custom hrev60256-2 system package): a 32-bit `syswow64\cmd.exe` reports `x86`, Wine Mono installs, and the hardware breakpoint test passes. **Not yet run on the test machine.** **Built for RenkuOS** 2026-10-30 in a RenkuOS build 66 VM (`haikuporter -y -j12`, ~30 min, recipe and patchset unchanged): the package requires `haiku>=r1~beta6_hrev60072_422-1`. Installed on the RenkuOS test laptop with vstbridge 0.3.0-1; `wine --version` reports `wine-11.19`. On first run the Wine Mono download stalled. No plug-in has been loaded yet. |
| 3 | Patches 13–16: thread-directed signals, no `-ldl` in winegcc, WoW64 (32-bit code) on a kernel with LDT support, xattrs through libgnu. Built on hrev60247, so it requires `haiku>=r1~beta6_hrev60247-1`. Verified with the work tree before packaging (VM, custom hrev60247 kernel): a 32-bit `syswow64\cmd.exe` runs and reports `x86`; Wine Mono 11.3.0 installs; iLok's PACE License Support bundle installs (exit `0x0`) and iLok License Manager starts. **Built** 2026-10-15 with `haikuporter -c`: the work tree's `HEAD^{tree}` equals the verified patchset's (`3035eeed`); `config.h` has `HAVE_SYS_XATTR_H 1` and the Makefile `XATTR_LIBS = -lgnu`; `ntdll.so` and `wineserver` link `libgnu.so`; `bin/wine` carries `/packages/wine-11.19-3`; the package requires `haiku>=r1~beta6_hrev60247-1`. **Not yet run as an installed package or on the test machine.** |
| 2 | Patch 12 (wineserver `free_async_queue()` double release). Built on hrev60236. |
| 1 | Rebase onto Wine 11.19 (tarball SHA256 `73eb0b02…c4087f`). Eleven patches: the 11.8 set minus `get_current_thread_data` (upstreamed in 11.11) and the debug stack scan, plus two installer fixes for iLok/PACE (sechost failure actions, Windows 10 manifests for msiexec/rundll32). Patch 2 needed two conflict resolutions; the OSMesa backend (patch 8) was ported to the version-41 GL driver interface and compile-checked only. Dependency declarations unchanged from 11.8-10. **Built** on hrev60236 (2026-10-14), after one more patch 2 fix: 11.19 adds a static `spawn_thread()` to `dlls/ntdll/unix/thread.c` that clashes with the system's `OS.h`, so it is renamed around that include like `cpu_info` and `exit_thread`. Confirmed in the build: `HAVE_OSMESA 1` in the work tree's `config.h`; the package requires `haiku>=r1~beta6_hrev60236-1` and `lib:libosmesa`; `bin/wine` carries `/packages/wine-11.19-1/.self`. In 11.19, `bin/wine` is a small launcher (`tools/wine`), and the loader that patch 7 changes is `lib/wine/x86_64-unix/Wine`, which imports `find_path`. **Not yet run on a test machine.** |

### wine-11.8

Revisions 3–10 of Wine 11.8 (July 2026) came before the rebase onto 11.19. Their notes are in
this file's git history. The lessons that still apply are under *Building* (a `REVISION` bump
needs a clean build) and *Installing* (the Wayland libraries must be declared).

## Attic

- `winerenku-cursor-shapes.diff`: `winerenku.drv` drawing Windows cursor shapes (I-beam,
  resize arrows) instead of keeping RenkuOS's pointer. Left out on purpose, so the pointer stays
  RenkuOS's; kept in case a program turns out to need its own cursors.

Removed, and still in this repo's history: X512's original Wine 7.1 port (`wine-7.1.recipe` and
`.patchset`) and upstream's optional `wine-11.8-home.patchset`, both also in
[haikuports](https://github.com/haikuports/haikuports) under `app-emulation/wine/`; and
`winehaiku.drv-11.8-abandoned.patch`, an in-tree native driver for 11.8 that did not work
(commit `64ff11f`). `winerenku.drv` is a fresh design and does not build on it.

## Licensing and credit

Wine is **LGPL v2.1**; these patches are derived work under the same licence. The first patches
(1–3 on 11.21) are upstream work from the HaikuPorts project (including X512's original port),
with authorship preserved in each `From:` line. The recipe is derived from the haikuports `app-emulation/wine` recipe.
