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

This is a fresh design. An earlier native driver for Wine 11.8 (`attic/winehaiku.drv-11.8-abandoned.patch`, removed from the repo, in commit `64ff11f`)
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

**EGL is off: the recipe configures `--without-opengl` (REVISION 2).** win32u calls the display
driver's own GL setup whether or not EGL is available (`dlls/win32u/opengl.c:3504-3506`), and
EGL cannot initialize on RenkuOS anyway, so the driver does not need it. REVISION 1 still
compiled win32u's EGL code: configure found `libEGL` in the build chroot even though the recipe
no longer declares `devel:libEGL`, so `SONAME_LIBEGL` was defined, and win32u loaded
`libEGL.so.1` at run time. That hangs explorer (§3, problem 1), so it could not wait for
phase 6. `--without-opengl` is the only thing that gates the EGL check (`configure.ac:1259`);
the other two uses are the X11 and Wayland GL checks, neither built. Without `SONAME_LIBEGL`,
`egl_init` is a stub returning FALSE (`opengl.c:1855-1860`), the same result the empty-libEGL
workaround produced by failing at `dlsym`.

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
   its looper), and the Be threads begin with them blocked. Confirmed in a running Wine by P3
   (§3), which also shows the failure without it.
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
| P3 `sigprobe.c` + `sigprobe_be.cpp` | Inside a Wine process: does the BApplication thread start, open a window, and survive Wine's signals (rule 3)? | **Passed, 15/15**, 2026-11-11, on the build VM against 11.21 REVISION 2 plus the first `winerenku.drv` (run once the driver drew). The control fails as predicted, and a fault on a Be thread freezes that thread. See below. |
| P4 `winprobe.cpp` | Window behaviour: exact and off-screen placement, popups that never take focus, owned/modal windows, decorator geometry, size limits. | **Passed, 14/14**, 2026-11-10, same VM, run over ssh. See below. |

What P3 established. First, from the sources:

- **Which signals can reach a Be thread.** wineserver signals a specific thread by id
  (`server/ptrace.c`, `tkill()` → `send_signal()` under `__HAIKU__`), so its SIGUSR1 suspends
  only reach Wine threads. Process-directed signals are the risk: RenkuOS queues them on the
  team and wakes every thread that doesn't block them (`src/system/kernel/signal.cpp`,
  `send_signal_to_team_locked`). That covers Ctrl-C's SIGINT from a Terminal, and SIGQUIT and
  SIGHUP, all of which Wine handles.
- **Wine's handlers can't cope with a foreign thread.** `get_current_thread_data()` is
  `rsp & ~0xffff` with no check (`signal_x86_64.c:572`, `unix_private.h:182`).
- **What gets blocked.** The set to block is ntdll's `server_block_set`: ALRM, IO, INT, HUP,
  QUIT, USR1, USR2 and CHLD (`dlls/ntdll/unix/server.c:1643`). RenkuOS has no `SIGIO`; the port
  uses `SIGPOLL` instead (`server.c:75`). ntdll doesn't export the set, so the driver keeps its
  own copy.
- **Where Be threads come from.** `BWindow::Show()` spawns the looper from the calling thread
  (`Looper.cpp:480`), so that call needs the block as well as the app thread's creation.
- **Not blockable.** Synchronous faults (SIGSEGV) on a Be thread still run Wine's handler.

Then from the three runs (prefix `drvprefix`, a copy of the one described below):

- **Rule 3 applied (default mode): 15/15.** Both loopers start with ALRM, IO, INT, HUP, QUIT,
  USR1, USR2 and CHLD blocked, and the Win32 thread's own mask is restored afterwards. A
  SIGINT sent to the window looper alone stays pending: Wine's handler does not run and the
  looper keeps answering. All 10 process-directed SIGINTs reached the console Ctrl-C handler
  through a Wine thread.
- **Control (`--unblocked`): rule 3 is necessary.** The loopers inherit Wine's unblocked
  mask. One SIGINT to the window looper runs Wine's handler there, which faults at once
  (`segv_handler`, address `0x1c`: thread data read through `rsp & ~0xffff`). The looper never
  answers again and the process hangs at teardown (killed by `timeout`).
- **A fault on a Be thread (`--fault`) freezes that thread, silently.** The window looper
  writes through a null pointer; Wine's `segv_handler` runs on it and never returns. The rest
  of the process keeps running (checked for 10 s), with no crash report and no debugger.
  Consequence: a bug in the driver's Be-side code, or in libbe, shows up as one frozen
  window, not a crash. Not yet done: an ntdll change so that `segv_handler` passes faults on
  threads Wine did not create to the system's default action (haiku-rpi carries a kernel fix
  for the same class of fault).
- libbe prints `Error parsing B_ARGV_RECEIVED message` at BApplication start when the
  program has no arguments: the message says `argc = 2` and carries one `argv`. Harmless so
  far; not yet traced.

The probe is a Winelib program built against the 11.21 work tree (commands in
[probes/README.md](probes/README.md)). Getting a prefix for it found three problems with the
no-driver base, all on the build VM:

1. **A prefix never finishes while libEGL is visible.** `explorer /desktop` logs
   `err:wgl:internal_context_create Failed to create internal root context`, and then its only
   thread waits forever on a `pthread once` semaphore (seen with `ps -as`). With no other thread
   left, that is a recursive `pthread_once`. It happens with the `null` driver too. Shadowing
   `libEGL.so.1` with an empty library on `LIBRARY_PATH` removes it: win32u's `egl_init` fails
   at `dlsym`. Which `pthread_once` it is was not found: `Debugger -s --thread` could not stop
   that thread. **Consequence: EGL has to be off in this build**, now rather than in phase 6:
   REVISION 2 configures `--without-opengl` (§2).
2. **With no driver configured, explorer falls back to nothing.** `default_driver` is
   "mac,x11,wayland" and none of them exists, so programs get
   `nodrv_CreateWindow … no driver could be loaded`. For the probe, the prefix is seeded with
   `HKCU\Software\Wine\Drivers` `Graphics=null` ([probes/sigprobe-user.reg](probes/sigprobe-user.reg)).
   Patch 17 replaces this with `renku` first in `default_driver`.
3. **Wine Mono's install prompt waits for a click**, with 11.19 as well. Mono and Gecko are
   wanted, so prefixes are created with the prompt answered by hand, not skipped. The one
   unattended prefix, `p3prefix`, was made with `WINEDLLOVERRIDES=mscoree,mshtml=`
   and has neither. P3 ran in a copy of it, `drvprefix`, which is a test prefix only:
   automatic updates are off (`.update-timestamp` = `disable`) and `winerenku.drv` was copied
   into its `system32` by hand, because the prefix predates the driver. Outside prefix creation,
   ntdll finds a builtin DLL without a file in `system32` only for 16-bit names
   (`dlls/ntdll/loader.c`, `find_builtin_without_file`); a new prefix gets the file from
   wineboot.

With all three worked around, `wineboot -i` on 11.21 finished: exit 0, about 5 minutes, at
`p3prefix` on the VM. 32-bit code there logs `THREAD_SET_LDT_ENTRY … failed`, as
expected without the WoW64 kernel work.

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

What the first driver stage (patch 16) found on the build VM:

- **`BScreen` needs `be_app`.** Constructed before the BApplication runs, it is invalid, so the
  driver starts the application before asking for screens.
- **`BScreen::SetToNext()` fails on the last screen but leaves the `BScreen` valid.** A walk
  that stops on `IsValid()` never ends; the first explorer runs span on it at 100% CPU. The
  walk stops on the return value.
- **Hidden top-level windows are common.** explorer and every program create several that are
  never shown. A BWindow is made only once a window is first shown, so they get none.
- **Win32u locks.** Creating or releasing a window surface can take win32u's own locks, so the
  driver never does either while holding its window-table mutex.
- **Colour order not yet seen.** `B_RGB32` is B, G, R, X in memory (`GraphicsDefs.h`), the
  same as a 32bpp DIB, so the surface is copied without conversion. Notepad is black and white,
  so a coloured window still has to confirm it.

What the second stage (patch 18) found:

- **win32u holds back mouse moves.** Pure moves sent with `NtUserSendHardwareInput` reach the
  server only with the next button or wheel input, a move for another window, or an empty
  input for no window (`accum_mouse_motion`, `dlls/win32u/input.c`). Without that flush a drag
  arrived only on release: no selection while dragging, no highlight in menus. Found by
  counting: hundreds of moves sent, 8 `WM_MOUSEMOVE`s received.
- **app_server activates every window it shows**, unless it has `B_AVOID_FOCUS`. JUCE's menus
  are unowned tool-window popups; activated on show, they closed at once. `B_AVOID_FOCUS`
  windows are still brought to the front (`Desktop::ActivateWindow`).
- **Minimizing is the client's job.** app_server sends `B_MINIMIZE` to the window, from the
  decorator and from the Deskbar alike (`ServerWindow::NotifyMinimize`), and the
  `Minimize()` hook does the rest, so Wine can decide first.
- **`MoveTo`/`ResizeTo` update `Frame()` at once**, on the looper; app_server's own changes
  arrive as `B_WINDOW_MOVED`/`B_WINDOW_RESIZED`. A frame read from the looper therefore
  includes every change posted to it before.
- **BWindow takes some keys** before any view sees them (`_HandleKeyDown`): Command+Q quits
  the application, Control+Tab opens the Switcher, Print Screen takes a screenshot. The driver
  takes keys in `DispatchMessage`, ahead of it.
- **Layered windows cannot be shown.** JUCE draws four click-through layered windows as a drop
  shadow; shown opaque they were black edges. Click-through layered windows get no BWindow.

## 4. Gates

Checked by the user on bare-metal RenkuOS. The build VM is for building and the probes.

| Gate | What has to work | Status |
|---|---|---|
| G1 | notepad and winecfg appear in native windows | **partly**: notepad passed in the build VM on 2026-11-11 (patch 16): one titled native window with its title, menu bar, a white edit area and a blinking caret, nothing garbled. winecfg not tried yet, and not checked on bare metal yet. |
| G2 | typing, clicking, menus, dialogs | **passed in the build VM**, 2026-11-11 (patch 18): notepad (typing, drag selection, menus by mouse and Alt+F, the save dialog) and Amped - Roots, a JUCE 6.1.5 program (knobs, its own menus, settings dialogs). Not checked on bare metal yet. |
| G3 | moving, resizing, minimizing and closing from the system decorator | **passed in the build VM**, 2026-11-11 (patch 18): both programs; minimize goes to the Deskbar and restores from it. Not checked on bare metal yet. |
| G4 | clipboard, both ways | **passed for text in the build VM**, 2026-11-11 (patch 20): notepad and StyledEdit, both ways, from the menus and the shortcuts. Images and other formats are not carried yet. Not checked on bare metal yet. |
| G5 | OSMesa GL gates 1–5 of [OSMESA-GL-PLAN.md](OSMESA-GL-PLAN.md) §5, under the new driver | not started |
| G6 | a VST editor through vstbridge rebuilt on this Wine | not started |

## 5. Phases

0. Rebase onto 11.21 without Wayland (§1).
1. Probes P1–P4 (§3).
2. Skeleton: BApplication thread, displays from `BScreen`, BWindow per top-level, surfaces →
   G1. **Done as patches 16 and 17** (G1 partly checked, see §4).
3. Input and focus: event queue, keyboard (raw key code → scancode table read from RenkuOS's
   keyboard sources, then KBDTABLES from `get_key_map()`), mouse, capture, cursors → G2, G3.
   **Done as patches 18 and 19** (G2 and G3 passed in the VM). Patch 19 builds KBDTABLES
   from `get_key_map()`; `probes/kbdprobe.c` checked them on the US, US-International, German
   and French keymaps. Still open: dead keys (win32u's KBDTABLES code has no `pDeadKey`), and
   keymap changes while a program runs. Cursor shapes are left out by choice: the pointer
   stays RenkuOS's own arrow over Wine windows, which is more coherent than one that changes
   with every program. A `SetCursor` that maps Win32's standard cursors to `BCursorID` and
   draws the others was written and dropped (in its first test the I-beam did not show,
   cause not looked into); it is kept as `attic/winerenku-cursor-shapes.diff`. Revisit only
   if a program needs it, for example a plug-in that hides the pointer while a knob is
   dragged.
4. Window management: menus and tooltips, owned and modal windows, layered windows and regions
   as far as P4 allows.
5. Desktop: clipboard → G4 (text done as patch 20); keyboard layout (patch 19); then dead keys, IME, systray, as separate patches.
6. OpenGL: OSMesa rendering, presented through the client surface → G5. Then evaluate BGLView.
7. vstbridge rebuilt on this Wine → G6.
8. Release, with the gates recorded in the README.

Kernel changes any of this needs (for example, 32-bit WoW64 support) are maintained and
submitted separately to RenkuOS, not carried in this port.
