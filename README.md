# renku-wine-port

> This repository is the source of the `wine` package for RenkuOS. Built
> packages are on its [Releases](https://github.com/rations/renku-wine-port/releases) page, and
> [renku-audio](https://github.com/rations/renku-audio) lists the whole audio stack.

Patches for **Wine 11.19** on RenkuOS, built as a port with `haikuporter`. They exist so that
[vstbridge-renku](https://github.com/rations/vstbridge-renku) can run **Windows VST2 and VST3
plug-ins** inside [Kasen](https://github.com/rations/kasen) — the stock Wine package is
missing the fixes the bridge depends on.

**This repository is the source of truth for these patches.** They were offered upstream and
declined (the upstream ports tree does not accept AI-assisted contributions), so they are maintained here
instead. Nothing else is authoritative — not the build machine, not any working tree. See
[Where things live](#where-things-live).

The repo carries no Wine sources, only the recipe and the patchsets (~450 KB). `haikuporter`
downloads the upstream tarball (checksum pinned in the recipe) and applies the patchset to it.

## Layout

```
app-emulation/wine/            drop-in replacement for the haikuports port directory
  wine-11.19.recipe            REVISION=4
  patches/
    wine-11.19.patchset        the 17 patches below (this is the deliverable)
    wine-11.8-home.patchset    optional, not applied by the recipe (see Optional patches)
  additional-files/
    wine.rdef.in               Deskbar icon/version resource template
docs/
  OSMESA-GL-PLAN.md            design + test plan for the software-GL work (patch 8)
  probes/                      standalone programs establishing that plan's facts
attic/                         kept for reference, not built
  winehaiku.drv-11.8-abandoned.patch
  wine-7.1.recipe
  wine-7.1.patchset
```

## The patchset

Seventeen patches. **1–4 are from the upstream port** (see *Licensing and credit*), with original authorship preserved in each
`From:` line. **5–17 are local.**

| # | Patch | Author | Why |
|---|---|---|---|
| 1 | Add Wayland input header | X512 | upstream |
| 2 | 11.8 patches | X512 | upstream — the bulk of the port. Rebased onto 11.19 with two conflicts resolved; the rebase note is in the commit body. |
| 3 | Disable Bluetooth on Beta 5 | Peppersawce | upstream |
| 4 | Opensound support | Ken Mays | upstream — audio |
| 5 | Run static initializers in Winelib modules | local | Winelib modules never ran their C++ static constructors. vstbridge's Wine host is a C++ Winelib module, so without this it starts with uninitialised globals. **This is the essential one.** |
| 6 | Do not block in the SIGSEGV handler | local | The port's handler ended in a blocking `fgetc(stdin)` so a developer could inspect the fault. Anything without a usable stdin — a DAW, any GUI app, anything from the Deskbar — hung there instead of letting Wine's handler recover. |
| 7 | loader: find the loader path via `find_path()` | local | RenkuOS has no `/proc`, so `get_self_exe()` returned NULL and the loader resolved `argv[0]` relative to the current directory — a bare `wine` from `PATH` failed with `could not load ntdll.so`. Fixed with `find_path(B_FIND_PATH_IMAGE_PATH)`. |
| 8 | winewayland: add OSMesa software OpenGL backend | local | Gives Wine a software GL backend (OSMesa/llvmpipe) so wined3d can create a device and D3D-drawn plug-in editors can render — **without it those editors do not display at all**. The system's Mesa has no DRI swrast and no Wayland EGL platform, so `winewayland.drv`'s existing EGL path can never initialize there. Ported to Wine 11.19's GL driver interface; see the status below. |
| 9 | dxgi: wait for a refresh interval in `IDXGIOutput::WaitForVBlank()` | local | `WaitForVBlank()` was a stub printing a FIXME and returning `E_NOTIMPL` immediately. JUCE 8's Direct2D renderer drives all repainting from a dedicated thread that calls it in a loop, so with no wait that thread spun — measured at 62,024 calls in ~25 s, saturating a core — and the editor rendered one frame then never responded to input again. Now sleeps to the next refresh-interval boundary. Not specific to RenkuOS: the same gap is why these plug-ins are reported as needing DXVK on Linux, DXVK shipping its own DXGI with a real vblank wait. |
| 10 | sechost: report no failure actions in `QueryServiceConfig2W` | local | `services.exe` keeps no failure actions, and `QueryServiceConfig2W` failed with `ERROR_INVALID_LEVEL` for them. WiX's ServiceConfig custom action (used by iLok's PACE License Support installer) reads them before setting them and aborts the whole install when the query fails. Now reports what Windows reports for a service with none: the structure, zeroed. Not specific to RenkuOS; carried from the user's ARM64 Wine build. |
| 11 | msiexec, rundll32: declare Windows 10 compatibility in their manifests | local | Without a `supportedOS` list, Wine reports Windows 8 to the process an MSI custom action runs in, as Windows does. Installers that check for Windows 10 there (iLok's PACE License Support) refuse to install. Adds the same list Windows' own `msiexec.exe`/`rundll32.exe` carry. Not specific to RenkuOS; carried from the user's ARM64 Wine build. On a kernel without LDT support these two only help 64-bit installers, since 32-bit Windows programs need patch 15 and a kernel with LDT support. |
| 12 | server: detach asyncs from a queue before terminating them in `free_async_queue()` | local | wineserver aborted (`release_object: obj->refcount`) every time a prefix shut down, so registry changes since the last 30-second periodic save were lost — `wine regedit file.reg` never persisted, which broke licences kept in the registry (MachineGuid, per-plugin IDs) — and each crash orphaned the prefix's `services.exe`. When an APC cannot be delivered, `async_terminate()` completes the async through `async_set_result()`, which unlinked it and dropped the queue's reference while `free_async_queue()` still held it, and `free_async_queue()` then dropped it again. On RenkuOS delivery always failed (before patch 13) for a thread not already waiting, because the client reports no Unix thread id (`get_unix_tid()` returns -1) and wineserver cannot signal it; `services.exe` closing its listening `svcctl` pipe at shutdown hits this every time. Not specific to RenkuOS in itself. |
| 13 | ntdll, server: deliver signals to specific threads | local | `get_unix_tid()` had no branch for this OS and returned -1, so wineserver could never signal one thread: an APC queued to a thread that was not already waiting was destroyed and the pending I/O completed with `STATUS_ALERTED`. Named-pipe readers saw `ERROR_ALERTED` — WiX Burn's engine and UI lost their pipe (`0x800702e3`). Reports `find_thread(NULL)` and signals with `send_signal()`. Also removes the trigger of patch 12's crash. |
| 14 | winegcc: do not link with `-ldl` | local | RenkuOS has no `libdl` (`dlopen` is in libroot), so every Winelib link failed unless a hand-made empty `libdl.a` stub was installed. |
| 15 | ntdll: support WoW64 (32-bit code) | local | 32-bit Windows code needs a 32-bit code selector and a per-thread `%fs` segment for its TEB; every 32-bit program exited with "No LDT support on this platform". Uses new `arch_thread_control()` calls (`THREAD_SET_LDT_ENTRY`, `THREAD_SET/GET_FS_BASE`, `THREAD_GET_GS_BASE`) that **only a locally built kernel provides** (per-team LDT, FS/DS/ES kept per thread, FS saved across signals); FreeBSD's model. On a stock kernel 32-bit programs exit with Wine's "No LDT support" error and 64-bit ones are unaffected. Enables the iLok PACE installer's 32-bit parts (Bonjour, custom actions) and 32-bit installers in general. |
| 16 | ntdll, server: use the xattr functions from libgnu | local | Patch 2 disabled `HAVE_SYS_XATTR_H`, so reparse points (Windows symbolic links) failed with `ENOSYS`. Wine Mono's installer creates symlinks (`CREATELINKS64`, error 1603), so Mono never installed and every .NET custom action failed — iLok's License Support MSI with `0x80070643`. configure now finds `fgetxattr` in libgnu, which maps xattrs to BFS attributes. |
| 17 | ntdll, server: support hardware breakpoints | local | Copy protection (PACE/iLok: Neural DSP, Toontrack and others) points Dr0–Dr3 at a probe with `SetThreadContext` and expects `EXCEPTION_SINGLE_STEP` when it runs; a probe that does not trap marks the process as tampered and the licence check fails later ("no activation", or `LoadLibraryW` "DLL initialization failed" for a plug-in). The server cannot set debug registers on RenkuOS (no ptrace), so each thread loads its own through `THREAD_SET_DEBUG_REGISTERS`, which **only the locally built kernel provides**; `trap_handler` reports DR6 with the reserved bits set, as Windows does, which PACE checks. On a stock kernel the breakpoints are ignored with a warning. (Developed as "patch 18"; an SMBIOS patch numbered 17 was dropped because it changed the hardware IDs licences are bound to.) |

On RenkuOS, patches 15 and 17 do nothing: its kernel has neither the LDT calls nor
`THREAD_SET_DEBUG_REGISTERS`, so 32-bit programs exit and
hardware breakpoints are ignored. Only 64-bit Windows programs run.

Patch 7 removes the need for the `export WINELOADER=/boot/system/bin/wine` workaround earlier
Wine builds required in `~/config/settings/profile`: winegcc's generated `.exe` wrapper
scripts fall back to invoking a bare `wine`, which now works from any directory.

### Dropped in the 11.19 rebase

- **Define `get_current_thread_data`** (Peppersawce) — upstream Wine made the function
  unconditional in wine-11.11, so the patch has nothing left to do.
- **ntdll: debug — name the faulting caller by scanning the stack** — development
  instrumentation from the 11.8 GL work, never a fix. It printed to stdout on every fault.

### Status of the OSMesa backend (patch 8)

**On Wine 11.19 this backend has not run yet.** Wine 11.19 changed the GL driver interface
(version 37 to 41): contexts are now created and owned by the driver, win32u creates the
window's client surface itself, and drivers must supply a null surface for internal contexts
and a thread-cleanup hook. Patch 8 was ported to that interface and compiles against the 11.19
headers, but every gate below must be re-run on an 11.19 build before it is called working.
The port also fixes a known 11.8 bug: resizing a window used to reallocate the render buffer
in place while an OSMesa context was still bound to it.

On Wine 11.8 it was confirmed working for the case it was written for: without it the
Nembrini Audio VST3 editors did not display, with it they did. Rendering held from 11.8
REVISION 6; from REVISION 8, which added the dxgi patch, those editors were also interactive.

Against the five gates in [docs/OSMESA-GL-PLAN.md](docs/OSMESA-GL-PLAN.md) §5, **as last
measured on Wine 11.8**:

| Gate | Status |
|---|---|
| 1 — standalone `osprobe` shows GL 4.5 / llvmpipe | **passing** |
| 2 — Wine GL smoke test (`wglCreateContext`, `glGetString`) | never run as a discrete test; entailed by 3–5 passing |
| 3 — wined3d creates a device against llvmpipe | **passing** — the old `wined3d_caps_gl_ctx_create Failed to find a suitable pixel format` is gone |
| 4 — rendered frame reaches the floating Wayland window | **passing** — editors display and are usable; no formal colour/orientation check was performed |
| 5 — NA VST3 in the DAW: D3D GUI renders **and is interactive** | **passing, 2026-07-27** (REVISION 8) |
| 5b — repeated open/close of an editor leaves `jackd` and the DAW running | **passing, 2026-07-27** — the forced-restart symptom is gone |

Gate 2 is the only one never exercised on its own, so a regression in the raw WGL path would
first surface as a failure in one of the gates below it rather than as a clear GL error.

The verified groundwork, and why the alternatives (DXVK/Vulkan WSI, porting Mesa's
Wayland-EGL platform) are dead ends on RenkuOS, is all in that document.

Patch 8 is gated at build time on `HAVE_OSMESA`, which `configure` derives from `GL/osmesa.h`
plus `libOSMesa`. **If those are missing the port still builds — silently, without software
GL**, falling back to the EGL path that cannot work on RenkuOS. The recipe now declares
`devel:libOSMesa` and `lib:libOSMesa` so this cannot happen by accident; it did not before
REVISION 7.

### Optional patches

`patches/wine-11.8-home.patchset` (Peppersawce, "Move wine dir to `/config/settings`") is
**not** listed in the recipe's `PATCHES` and is not applied. It is upstream's and marked buggy;
kept only so it is not lost.

## Building

On the target RenkuOS machine, with a [haikuports](https://github.com/haikuports/haikuports)
checkout and `haikuporter` configured:

```sh
# 1. Overlay this port onto your haikuports checkout
cp -a app-emulation/wine/. ~/haikuports/app-emulation/wine/

# 2. Build (about 30 minutes with -j12 in the RenkuOS build VM; builds i386 and x86_64)
cd ~/haikuports
haikuporter -y -j8 wine-11.19
```

The package lands in `~/haikuports/packages/wine-11.19-1-x86_64.hpkg`.

> **Always `haikuporter -y -c wine-11.19` first when `REVISION` changed.** Not just when the
> patchset changed — **the revision is compiled into the binaries.** `configure` records
> `prefix = /packages/wine-<version>-<revision>/.self`, and Wine bakes that into `bindir`/
> `libdir`; the loader then resolves `ntdll.so` through it. Skipping `-c` after only a recipe
> edit re-runs `configure` (so the `Makefile` says the new revision) but recompiles nothing,
> because no source changed — and the package ships binaries still pointing at the **previous**
> revision's package directory.
>
> That package works perfectly on any machine where the old revision is still installed, and
> fails on every machine where it is not, with:
>
> ```
> wine: could not load ntdll.so: No such file or directory
> runtime_loader: Cannot open file /packages/wine-11.8-8/.self/lib/wine/x86_64-unix/ntdll.so
> ```
>
> Diagnose it directly — the shipped binary disagrees with the recipe:
>
> ```sh
> strings -a /boot/system/bin/wine | grep packages/wine   # must match the installed revision
> ```
>
> A tell-tale sign is a suspiciously fast rebuild and binaries whose hashes are *identical* to
> the previous revision's. That is not reassurance that the payload is unchanged; it is the
> symptom.

Build dependencies are declared in the recipe (`BUILD_REQUIRES`/`BUILD_PREREQUIRES`) and include
llvm21 + clang + lld for the PE cross-build; `haikuporter` will tell you what is missing.
When installing them by hand with `pkgman`, `devel:libSDL2_2.0`, `devel:libEGL` and
`devel:libOSMesa` are not found by name on RenkuOS: install `libsdl2_devel` and `mesa_devel`.

### Iterating without an hour-long rebuild

A clean rebuild is ~55 minutes, which is not a per-edit cycle. Two things make a fast loop
possible:

- `haikuporter` applies `.patchset` files **with git**, so `work-11.19/sources/wine-wine-11.19`
  is a git repo whose `HEAD` is the pristine tarball plus these patches. Editing it and
  rebuilding one target inside the chroot (`haikuporter -E wine-11.19`, then
  `make -j8 dlls/dxgi/dxgi.dll`) takes minutes, and `git diff` there is exactly your change.
- Nothing needs to be installed to test it. Wine sets its internal `build_dir` when its
  `ntdll.so` sits under `<tree>/dlls/ntdll`, and then omits the installed `/boot/system/lib/wine`
  from the DLL search path entirely — so the build tree wins outright:

  ```sh
  export WINELOADER=<work tree>/loader/wine
  ```

  (`WINEDLLPATH` does *not* work for this: its entries are appended *after* the installed
  directory, so an installed builtin always wins.)

> **The catch: `haikuporter -c` deletes `work-11.19/sources/` without warning.** An edit that
> exists only there is one command from being lost — that has happened twice here. Replay every
> such edit into the patchset (`git diff` in the work tree, apply it to the tree the patchset is
> generated from) *before* any clean rebuild. Every source change ultimately belongs in the
> patchset — see below.

### Installing

```sh
pkgman install ./wine-11.19-4-x86_64.hpkg
```

- **On RenkuOS, build it there.** A package built on RenkuOS build 66 requires
  `haiku>=r1~beta6_hrev60072_422-1` (RenkuOS's system package is still named `haiku`).
  `haikuporter` bakes the build machine's system revision in as a floor, so an older system
  refuses to install it; newer is fine, as this is a userland package, not a kernel add-on. A
  package built on another system with the same `wine-11.19-4` label does not install here,
  so do not mix builds in one repository or on one machine.
- Runtime dependencies, all pulled from the system repositories automatically. If the target machine is
  offline, bring them along too.

  | Declared since | Resolvable | Package |
  |---|---|---|
  | always | `lib:libfreetype` | `freetype` |
  | always | `lib:libvulkan` | `vulkan` |
  | REVISION 7 | `lib:libOSMesa` | `mesa` |
  | **REVISION 10** | `lib:libwayland_client`, `lib:libwayland_egl` | `wayland` |
  | **REVISION 10** | `lib:libxkbcommon`, `lib:libxkbregistry` | `libxkbcommon` |
  | **REVISION 10** | `lib:wayland_server_inproc` | `wayland_server` |
  | **REVISION 10**, corrected | `lib:libusb_1.0` | `libusb` |

  (Written for REVISION 9, but 9 was withdrawn before it ever worked — see the revision
  history. REVISION 10 is the first package that actually ships them.)

  The last row was `lib:libusb` through REVISION 8, which resolves to `libusb_compat` — the
  libusb-0.1 API. `wineusb.so` links `libusb-1.0.so.0`, provided by the `libusb` package as
  `lib:libusb_1.0`. Nothing in the port links libusb-0.1 at all, so the old entry pulled in a
  library no module used while leaving the one `wineusb.so` needs undeclared.

**REVISION 8 and earlier declared none of the Wayland ones, and that made the package
un-GUI-able on a machine that did not already have them.** `winewayland.so` links
`libwayland-client`, `libwayland-egl`, `libxkbcommon` and `libxkbregistry` directly
(`objdump -p` shows them as `NEEDED`), and a recipe's `REQUIRES` is hand-written —
nothing scans the binaries for you. On a machine with those libraries already installed for
other reasons the package worked; on a clean one it installed happily and then had no display
driver. See *Graphics driver* below for the failure signature.

If you are stuck on an older package, the missing pieces install by hand:

```sh
pkgman install wayland wayland_server libxkbcommon
```

## Changing a patch

Do not edit the patchset by hand. Rebuild it from a git tree, which is how the current one was
produced:

```sh
# One-time: reconstruct the working tree from this repo
tar xf wine-wine-11.19.tar.gz && cd wine-wine-11.19
git init -q && git add -A && git commit -qm "wine-11.19 pristine"
git am /path/to/renku-wine-port/app-emulation/wine/patches/wine-11.19.patchset

# Then, for every change: edit, commit, regenerate, commit the patchset here
git format-patch --stdout <pristine-sha>..HEAD \
  > /path/to/renku-wine-port/app-emulation/wine/patches/wine-11.19.patchset
```

Each commit is one patch, in order. Bump `REVISION` in the recipe whenever the patchset or
recipe changes — never reuse a published revision.

To verify a regenerated patchset reproduces the tree it came from:

```sh
git worktree add --detach /tmp/verify <pristine-sha>
cd /tmp/verify && git am /path/to/patches/wine-11.19.patchset
git rev-parse HEAD^{tree}   # must equal the dev tree's HEAD^{tree}
```

## Where things live

| | Role |
|---|---|
| **this repo** | **Authoritative.** The recipe and patchsets, in git. If it is not committed here, it is not kept. |
| `third_party/wine-haiku-dev` | Scratch working tree for editing source and regenerating the patchset (the `git am` tree above): pristine 11.19 plus one commit per patch. Regenerable; safe to delete and recreate. |
| `third_party/wine-osmesa-dev` | The 11.8 tree the 11.19 rebase cherry-picked from. History only. Tag `pre-consolidate-backup-20260726` marks its pre-cleanup history. |
| build VM `~/haikuports/app-emulation/wine/` | Build machine only (a QEMU VM running the RenkuOS build the package is built for). Copy the port in, build, copy the `.hpkg` out. Has no git history of its own — anything edited only there is one `haikuporter -c` from gone. |

## Graphics driver

The port builds `--with-wayland --without-x`. Which driver a prefix uses is set once, per
prefix, in `HKCU\Software\Wine\Drivers\Graphics`, and the right choice depends on what you are
doing:

| Setting | Result |
|---|---|
| `null` | **Use this for plug-ins.** vstbridge captures the plug-in's window and repaints it inside the host's FX window (Kasen's), so the plug-in's own on-screen window is an unwanted duplicate. The `null` driver declines to create a driver window surface, which sends win32u down its offscreen path — the window still draws and still receives `WM_PAINT`, but never reaches a display. |
| `winewayland.drv` (default) | A floating window for any Windows program. For a bridged plug-in you get that floating window *in addition to* the copy in the FX window. Keep this if the prefix also runs ordinary Windows applications, since `null` leaves those with no window at all. |

```sh
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
```

vstbridge logs a warning naming the current driver whenever it is not `null`.

Note that patch 8's software GL lives in `winewayland.drv`, so it is only in play when that
driver is selected — the `null` driver has no GL backend at all.

### How a Wayland window exists on RenkuOS at all

There is no compositor process. The `wayland_server` package installs exactly one file,
`lib/wayland-server-inproc.so` (`package list -p`), and `libwayland-client` loads it at runtime:
the library imports `dlopen`/`dlsym`, carries the literal string `wayland-server-inproc.so`, and
does **not** list it in `NEEDED`. The system's Wayland is X512's fork
(`github.com/X547/wayland`), whose server port is not a separate server process: it is an
add-on loaded into each Wayland client, drawing with native windows and the system's bitmap
drawing API.

That is why `ps` shows nothing compositor-shaped and `WAYLAND_DISPLAY` is unset, yet a plug-in
editor still gets a real floating window.

It is also why the dependency is easy to miss. `wayland` does not require `wayland_server`, and
neither did this port before REVISION 9. On the build laptop the only installed package that
requires `lib:wayland_server_inproc` is **`gtk3`** — so the in-process compositor was present
there by coincidence, dragged in behind some unrelated GTK application, and every Wine GUI test
to date silently depended on it.

### When no driver loads at all

`explorer.exe` picks the driver by calling `LoadLibraryW("winewayland.drv")` and writing the
result to `GraphicsDriver` under `HKLM\System\CurrentControlSet\Control\Video\{guid}\0000`
(`programs/explorer/desktop.c:998,1037,1063`). If the load fails it writes `DriverError`
instead (`desktop.c:1066`), and `win32u` falls back to a null driver whose `nodrv_CreateWindow`
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
  recreate `~/.wine` after installing the missing packages.

To confirm on a suspect machine:

```sh
objdump -p /boot/system/lib/wine/x86_64-unix/winewayland.so | grep NEEDED   # what it wants
wine reg query 'HKLM\System\CurrentControlSet\Control\Video' /s | grep -i driver
WINEDEBUG=+winediag wine notepad 2>&1 | head       # the ERR above, if any
```

## Revision history

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

- `winehaiku.drv-11.8-abandoned.patch` — an in-tree native graphics driver tried during
  development and **abandoned**; it did not work. Deliberately not part of the port. The
  arrangement that does work is *Graphics driver* above.
- `wine-7.1.recipe`, `wine-7.1.patchset` — X512's original Wine 7.1 port (13 patches),
  the ancestor of this work. Reference only.

## Licensing and credit

Wine is **LGPL v2.1**; these patches are derived work under the same licence. Patches 1–4 are
upstream work from the HaikuPorts project (including X512's original port), with authorship
preserved in each `From:` line. The recipe is derived from the haikuports `app-emulation/wine` recipe.
