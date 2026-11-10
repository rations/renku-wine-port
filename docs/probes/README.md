# Probes

Small standalone programs that establish the facts
[OSMESA-GL-PLAN.md](../OSMESA-GL-PLAN.md) and [NATIVE-DRIVER-PLAN.md](../NATIVE-DRIVER-PLAN.md)
are built on. They are the reason the plan chose OSMesa
over EGL and over Vulkan/DXVK — rerun them before trusting any of those conclusions on a
different RenkuOS revision or Mesa version.

Build and run on the target machine:

| Probe | Build | Establishes |
|---|---|---|
| `osprobe.c` | `cc osprobe.c -o osprobe -lOSMesa` | **The plan's foundation.** OSMesa gives headless desktop GL with no window: `OSMesaCreateContextAttribs` + `OSMesaMakeCurrent` into a malloc'd RGBA8 buffer yields GL 4.5 compat / llvmpipe and renders a correct frame. Plan §1. |
| `eglprobe.c` | `cc eglprobe.c -o eglprobe -lEGL -lGL` | **The dead end that ruled out EGL.** Surfaceless/device EGL is advertised as a *client* extension but `eglInitialize` fails with `EGL_NOT_INITIALIZED`, and there is no Wayland EGL platform at all — so `winewayland.drv`'s existing EGL path can never work on RenkuOS. Plan §1. |
| `vkenum.c` | `cc vkenum.c -o vkenum -lvulkan` | Vulkan instance/device enumeration. |
| `vkext.c` | `cc vkext.c -o vkext -lvulkan` | **The dead end that ruled out DXVK.** The Vulkan loader exposes no surface extensions, so there is no WSI to present through. Plan §7. |
| `beprobe.cpp` | `g++ -O1 -Wall beprobe.cpp -o beprobe -lbe` | **The native driver's threading model** (NATIVE-DRIVER-PLAN.md §3, P1+P2): a BApplication run off the main thread, a window created, locked and invalidated from another thread, looper events waking that thread's `poll()` through a pipe. Prints PASS/FAIL per check; exit status = failures. Opens one small window for about a second. Passed 11/11 on the RenkuOS build VM, 2026-11-10. |
| `winprobe.cpp` | `g++ -O1 -Wall winprobe.cpp -o winprobe -lbe` | **Window behaviour the native driver relies on** (NATIVE-DRIVER-PLAN.md §3, P4): exact and off-screen placement, popups that never take focus, modal subsets, decorator insets and size limits. Passed 14/14 on the RenkuOS build VM, 2026-11-10. |
| `sigprobe.c`, `sigprobe_be.cpp`, `sigprobe.h` | Winelib, against a built Wine tree `$W`: `g++ -O1 -Wall -fPIC -c sigprobe_be.cpp` then `$W/tools/winegcc/winegcc --wine-objdir $W -mconsole -o sigprobe.exe sigprobe.c sigprobe_be.o -L$W/libs/winecrt0 -L$W/dlls/kernel32 -L$W/dlls/ntdll -lwinecrt0 -lkernel32 -lntdll -lbe -lstdc++`. With `--wine-objdir`, winegcc adds no default libraries, so they are listed. Run `$W/loader/wine sigprobe.exe.so [--unblocked|--fault]` | **Wine's signals and Be threads in one process** (NATIVE-DRIVER-PLAN.md §3, P3): thread masks on the loopers, thread- and process-directed SIGINT, a fault on a looper. Needs a prefix seeded with `sigprobe-user.reg` (`Graphics=null`), with libEGL shadowed and `WINEDLLOVERRIDES=mscoree,mshtml=` while it is created (see §3). Built 2026-11-10, **not run yet**. |
| `gltest.c` | winegcc, run under Wine | Gate 2 of the plan's test plan (§5): a Win32 app creating a GL context *through Wine*. Under the OSMesa backend it should print llvmpipe 4.5 with no `Failed to find a suitable pixel format`. This is the gate that is not yet green. |

Recorded on the test laptop, 2026-07-24, Mesa 22.0.5, system revision hrev59867.
