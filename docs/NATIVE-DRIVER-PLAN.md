# Plan: `winerenku.drv`, a native Be API graphics driver for Wine on RenkuOS

Goal: Windows programs get real native windows on RenkuOS (BApplication, BWindow, BView,
BBitmap), behaving as they do under Wine on Linux, with no X11 and no Wayland in between.

Why: every other Wine port for this OS family draws through `winewayland.drv`. On RenkuOS,
Wayland is X512's in-process `wayland-server-inproc.so`, an add-on in each client that turns
Wayland requests back into native windows and bitmaps. So every window crosses an extra
protocol layer, and that brings Wayland's limits onto a platform that doesn't have them. The
main one: a Wayland client cannot position its own windows, but a BWindow can be placed
anywhere.

The driver is maintained as RenkuOS-only patches, rebased onto each Wine release. Upstream
Wine is not expected to carry it.

This is a fresh design. An earlier native driver for Wine 11.8 (`attic/winehaiku.drv-11.8-abandoned.patch`)
did not work, and why was never recorded. Nothing here is based on it. This time every gate's
result is written down, below.

---

## 1. Base: Wine 11.21 without Wayland

The native work starts from Wine 11.21 (tarball SHA256 `046cb051…cfb8029`, which reproduces
upstream's `wine-11.21` tree `db03e4c9` exactly). It carries the port's existing patches
**except** the ones that only exist for the Wayland path or do nothing on RenkuOS:

| Dropped | Why |
|---|---|
| winewayland OSMesa backend (was patch 8) | Tied to `winewayland.drv`. The OSMesa approach returns inside `winerenku.drv` (§5, phase 6). |
| "Disable Bluetooth on Beta 5" (HaikuPorts) | Only acts when `B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5`. RenkuOS defines `B_HAIKU_VERSION_1_PRE_BETA_7` (`headers/config/HaikuVersion.h:45`, same on the build VM), so it compiles to nothing. |

**Kept, although Wayland is not built:** HaikuPorts' "Add Wayland input header". The core
port patch ("11.8 patches") changes two winewayland files to include it. configure is run
`--without-wayland`, so those files are never compiled, but makedep still resolves every
quoted `#include` of every module. Without the header, configure fails: `dlls/winewayland.drv/wayland_keyboard.c:30:
error: input-event-codes.h: No such file or directory` / `config.status: error: could not
create Makefile`. Observed on the build VM, 2026-11-10.

**EGL:** win32u calls the display driver's own GL setup whether or not EGL is available
(`dlls/win32u/opengl.c:3504-3506`), and EGL cannot initialize on RenkuOS anyway, so the driver
does not need it. Note that the build still compiles win32u's EGL code: configure found
`libEGL` in the build chroot even though the recipe no longer declares `devel:libEGL`, so
`SONAME_LIBEGL` is defined, and at run time win32u will try to `dlopen("libEGL.so.1")` and
carry on without it if it is missing (`opengl.c:1679-1681`). Whether to force it off is a
phase 6 decision.

**configure checks for the driver go into Makefile variables, not `config.h`.** Every unix-side
object depends on `include/config.h` in the generated Makefile, so a new `config.h` define
recompiles all of them (ntdll, win32u, wineserver, the unix libs). An `AC_SUBST`'d
`OSMESA_CFLAGS`/`OSMESA_LIBS` used only by `dlls/winerenku.drv/Makefile.in`, the pattern
upstream uses for `WAYLAND_CLIENT_LIBS`, rebuilds only the driver. This keeps the fast loop
fast; a release is always a clean build anyway, because the REVISION is compiled in (19
minutes on the build VM for 11.21).

**Driver interfaces in 11.21:** `WINE_GDI_DRIVER_VERSION` 110, `WINE_OPENGL_DRIVER_VERSION`
41 and `WINE_VULKAN_DRIVER_VERSION` 48, all unchanged from 11.19. The only struct change on the
GL side is a new `hwnd` field in `struct opengl_drawable`, filled in by win32u
(`dlls/win32u/opengl.c`, upstream e6ea24c9d02).

## 2. Architecture

The model is `winemac.drv`: a native toolkit with its own event threads, behind one plain-C
seam. Wine's internal headers are not C++-safe, so they never share a file with the Be
headers.

- `dlls/winerenku.drv/*.c`: the Wine side (`user_driver_funcs`, `window_surface_funcs`).
- `dlls/winerenku.drv/be_*.cpp`: the Be API side, reached only through `renku_be.h`.
- **One BApplication per Wine process**, created lazily on a thread the driver starts when the
  first window is needed. `wine.rdef.in` already declares `B_MULTIPLE_LAUNCH` and
  `application/x-vnd.wine`, so every Wine process can register.
- **Only top-level HWNDs get a BWindow.** win32u draws child windows into the top-level
  surface.
- **Surfaces:** `window_surface_create()` with a 32bpp top-down BITMAPINFO. `flush` copies the
  dirty rect into a `B_RGB32` BBitmap owned by the window's view and invalidates that rect.
- **Events:** BView/BWindow hooks run on the window's looper thread. They only append an
  event to the owning Win32 thread's queue and write one byte to that queue's pipe. The pipe
  is handed to wineserver with `set_queue_fd`, as in `set_queue_display_fd`
  (`dlls/winemac.drv/macdrv_main.c:470`). The Win32 thread's `ProcessEvents` drains the queue
  and calls `NtUserSendHardwareInput`, `NtUserSetWindowPos` and so on.

### Rules the design depends on

1. **The BApplication is constructed and `Run()` on the same thread.** `BLooper::Loop()`
   asserts the looper is locked (`src/kits/app/Looper.cpp:501`), and a looper is locked by the
   thread that constructs it.
2. **A looper thread never blocks on a Win32 thread.** Looper → Win32 is one non-blocking pipe
   write. Win32 → BWindow is `PostMessage` or `LockWithTimeout`. Any lock taken in the other
   direction needs a written justification in the code.
3. **Every Be thread starts with Wine's asynchronous signals blocked.** On x86-64, Wine's
   signal handlers find the thread's data from the stack pointer
   (`rsp & ~signal_stack_mask`, `dlls/ntdll/unix/signal_x86_64.c:568`). That only works on the
   signal stack Wine gives its own threads, so a Wine signal on a looper thread would read
   garbage. A new RenkuOS user thread inherits its creator's signal mask
   (`src/system/kernel/thread.cpp:694`). So the driver blocks those signals around the calls
   that spawn Be threads (creating the app thread, and a window's first `Show()`, which starts
   its looper), and the Be threads begin with them blocked. **To be confirmed in a running
   Wine by P3.**
4. **Every BBitmap is freed before the BApplication goes away.** Freeing a server-backed
   BBitmap after the app's app_server connection is gone drops into the debugger:
   `You need to have a valid app_server connection first!` in `BBitmap::_CleanUp`. Observed by
   `beprobe` (§3). The simplest correct policy is to never quit the BApplication before the
   process exits.
5. **Pixels need no conversion.** `B_RGB32` is `BGR-` in memory
   (`headers/os/interface/GraphicsDefs.h:176`), the byte order of a 32bpp Windows DIB.

## 3. Probes

Programs in `probes/` that settle platform facts before driver code depends on them.

| Probe | Question | Result |
|---|---|---|
| P1+P2 `beprobe.cpp` | Can a BApplication run off the main thread while another thread creates, locks and draws into a window, with looper events waking that thread's `poll()` through a pipe? | **Passed, 11/11**, 2026-11-10, on the RenkuOS build VM (hrev60072+518, `B_HAIKU_VERSION_1_PRE_BETA_7`). Show → first Draw event 2.2 ms; Invalidate → Draw event 1.8 ms, both with a 12-job Wine build loading every core. The first run found rule 4. |
| P3 | Inside a Wine process: does the BApplication thread start, open a window, and survive Wine's signals (rule 3)? | Not run yet; needs the 11.21 base build. |
| P4 `winprobe.cpp` | Window behaviour: exact and off-screen placement, popups that never take focus, owned/modal windows, decorator geometry, size limits. | **Passed, 14/14**, 2026-11-10, same VM, run over ssh. See below. |

What P4 established:

- **Placement is exact, including off screen.** A titled window's `Frame()` is its client area,
  exactly where `MoveTo()` put it. `MoveTo(-40, -30)` is honoured, not clamped. Win32 programs
  rely on both, and Wayland allows neither.
- **Popups behave like Windows menus and tooltips.** `B_NO_BORDER_WINDOW_LOOK` with
  `B_AVOID_FOCUS` lands exactly where asked, has no decorator (`DecoratorFrame() == Frame()`),
  leaves the active window active when shown, and refuses `Activate()`. This holds with both
  `B_NORMAL_WINDOW_FEEL` and `B_FLOATING_APP_WINDOW_FEEL`.
- **Owned windows:** `AddToSubset()` puts an owner into a `B_MODAL_SUBSET_WINDOW_FEEL` window's
  subset, and the modal window becomes active. `AddToSubset()` takes a `BWindow*`, so this works
  within one team only, which means one Wine process.
- **Decorator geometry must be read, not assumed.** With the default decorator the insets are
  left 5, top 29, right 5, bottom 5 (`"tab frame"` 175×25, `"border width"` 5, from
  `GetDecoratorSettings()`). RenkuOS has decorator themes, so the driver reads
  `DecoratorFrame()` / `GetDecoratorSettings()` at runtime.
- **Titled windows have a minimum size.** `ResizeTo(0, 0)` gave 55×14. When the system enforces
  a limit, the driver has to report the real size back to win32u, not assume the requested one.
- **No shaped or translucent windows in the public API.** `Window.h` has nothing for a window
  shape or a per-window alpha. Unless app_server turns out to support one privately,
  `SetWindowRgn` and `WS_EX_LAYERED` alpha degrade to rectangular, opaque windows.

## 4. Gates

Checked by the user on bare-metal RenkuOS. The build VM is for building and the probes.

| Gate | What has to work | Status |
|---|---|---|
| G1 | notepad and winecfg appear in native windows | not started |
| G2 | typing, clicking, menus, dialogs | not started |
| G3 | moving, resizing, minimizing and closing from the system decorator | not started |
| G4 | clipboard, both ways | not started |
| G5 | OSMesa GL gates 1–5 of [OSMESA-GL-PLAN.md](OSMESA-GL-PLAN.md) §5, under the new driver | not started |
| G6 | a VST editor through vstbridge rebuilt on this Wine | not started |

## 5. Phases

0. Rebase onto 11.21 without Wayland (§1).
1. Probes P1–P4 (§3).
2. Skeleton: BApplication thread, displays from `BScreen`, BWindow per top-level, surfaces →
   G1.
3. Input and focus: event queue, keyboard (raw key code → scancode table read from RenkuOS's
   keyboard sources, then KBDTABLES from `get_key_map()`), mouse, capture, cursors → G2, G3.
4. Window management: menus and tooltips, owned and modal windows, layered windows and regions
   as far as P4 allows.
5. Desktop: clipboard → G4; then keyboard layouts, IME, systray, as separate patches.
6. OpenGL: OSMesa rendering, presented through the client surface → G5. Then evaluate BGLView.
7. vstbridge rebuilt on this Wine → G6.
8. Release, with the gates recorded in the README.

Kernel changes any of this needs (for example, 32-bit WoW64 support) are maintained and
submitted separately to RenkuOS, not carried in this port.
