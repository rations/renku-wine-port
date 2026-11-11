# Plan: software OpenGL for Wine on Haiku via OSMesa (Path B)

Goal: make Direct3D-drawn plugin editors (Nembrini Audio VST3s, etc.) display in
jackDAW-haiku. They load and run audio fine under vstbridge; they show no GUI because
wined3d needs an OpenGL context and Wine-on-Haiku currently has none. This plan gives Wine a
**software OpenGL backend (OSMesa / llvmpipe)** that works on **any machine** — no GPU, no
Vulkan WSI, no DXVK — by translating D3D9/10/11 → GL (wined3d) and rendering GL offscreen,
then presenting to the existing Wayland floating window via `wl_shm`.

Build on top of this port tree (`haiku-wine-port`, Wine 11.8 from HaikuPorts, already
running on the laptop). **No changes to win32u / opengl32 are required** — the change is a new
GL backend inside the Haiku display driver (`winewayland.drv`) plus recipe/build wiring.

> **Now in `winerenku.drv` (11.21, patch 19 since REVISION 9, 2026-11-11).** The backend described here moved
> out of `winewayland.drv`, which the 11.21 port does not build, into the native driver
> (`dlls/winerenku.drv/opengl.c`). §2 and the GL half of §3 still hold. The presentation is new:
> instead of `wl_shm` buffers, a GL window that nothing clips shows its frames in a BView of
> its own in its top-level's BWindow, and one that something clips has them drawn into it with
> GDI (`StretchDIBits`; OSMesa's BGRA with `OSMESA_Y_UP` 0 is a top-down 32-bpp DIB, so no
> conversion either way). OSMesa has its own configure check (`SONAME_LIBOSMESA`), separate
> from `--with-opengl`, which stays off. Gates 1–4 re-run under the new driver are in §5.

---

## 1. Verified facts (probed on haikulaptop, 2026-07-24, Mesa 22.0.5)

Probes live at `/boot/home/osprobe.c` and `/boot/home/eglprobe.c` (also in the jackDAW
scratchpad). Reproduce with `cc <probe>.c -o <probe> -lOSMesa` / `-lEGL -lGL`.

- **OSMesa gives headless desktop GL 4.5.** `OSMesaCreateContextAttribs` (compat, 3.3
  requested) + `OSMesaMakeCurrent` into a malloc'd RGBA8 buffer, **no window**, yields
  `GL_VERSION = 4.5 (Compatibility Profile) Mesa 22.0.5`, `GLSL 4.50`,
  `GL_RENDERER = llvmpipe (LLVM 12.0.1, 256 bits)`, and renders a correct frame
  (glClear → exact expected pixel). 4.5 compat is far above wined3d's floor (~3.0).
  Pure software → **any machine**.
- **Surfaceless / device / Wayland EGL is a dead end on Haiku.** `libEGL.so.1` advertises
  `EGL_MESA_platform_surfaceless` and `EGL_EXT_platform_device` as *client* extensions, and
  `eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,…)` returns a non-null display, but
  `eglInitialize` then fails with `EGL_NOT_INITIALIZED` (0x3001), even with
  `LIBGL_ALWAYS_SOFTWARE=1` / `GALLIUM_DRIVER=llvmpipe`. There is **no Wayland EGL platform**
  at all. Reason: Haiku's Mesa builds **no DRI/gallium `*_dri.so` modules** (no
  `/boot/system/lib/dri`); its software GL lives behind
  `/boot/system/add-ons/mesa/Software Pipe` (13.4 MB gallium renderer), reachable only via the
  **"haiku" EGL platform (BGLView → needs a BWindow)** — or via **OSMesa**.
- Consequence: **the existing `winewayland.drv` OpenGL path can never work on Haiku.** It uses
  `EGL_PLATFORM_WAYLAND_KHR` + `wl_egl_window` (`dlls/winewayland.drv/opengl.c:136,119-120`),
  and Haiku's libEGL provides neither the Wayland platform nor a swrast device to initialize.
  This is why wined3d logs `wined3d_caps_gl_ctx_create Failed to find a suitable pixel format`
  → `Failed to get a GL context` → the plugin null-derefs its failed D3D device.

`libOSMesa.so.8` + `GL/osmesa.h` are installed on the laptop (package `mesa_swpipe`
22.0.5-3; dev header from `mesa_devel`).

---

## 2. Why OSMesa plugs in with no win32u changes

Modern Wine (the 2025 `opengl_driver_funcs` refactor; identical byte-for-byte in 11.8 and
11.9) centralizes GL/WGL in `win32u`, and lets the display driver supply the backend. The
seam is `display_funcs_init()` in `dlls/win32u/opengl.c` (11.8):

- **:2687** `egl_init(&driver_funcs)` only **dlopens `libEGL` and loads symbols** (:879) — it
  does *not* initialize any platform/display. On Haiku it "succeeds" (libEGL dlopens) and
  leaves `egl_handle` non-null but otherwise inert.
- **:2689** `user_driver->pOpenGLInit(WINE_OPENGL_DRIVER_VERSION, &display_funcs, &driver_funcs)`
  — the display driver may **replace `driver_funcs` wholesale**. This is our injection point
  (`WAYLAND_OpenGLInit`, `dlls/winewayland.drv/opengl.c:247`).
- **:2691** `init_egl_platforms(&display_funcs, driver_funcs)` is a **no-op** when the driver
  doesn't set `p_init_egl_platform` (gate at `dlls/win32u/opengl.c:1032`:
  `if (!funcs->egl_handle || !driver_funcs->p_init_egl_platform) return;`). Our OSMesa driver
  omits `p_init_egl_platform`, so no EGL platform init runs.
- **:2693-2695** pixel formats come from the driver's `p_init_pixel_formats` /
  `p_describe_pixel_format`.
- **:2697-2702** **all GL entry points are resolved via
  `driver_funcs->p_get_proc_address(#func)`** — for OSMesa that is `OSMesaGetProcAddress`
  (with a `dlsym(libGL)` fallback for core symbols).
- Context lifecycle and binding go through the driver table:
  `p_context_create` / `p_context_destroy` / `p_make_current`
  (`opengl_driver.h:247-249`; egldrv defaults at `win32u/opengl.c:755` / `:843`), and each
  drawable's `swap` (`struct opengl_drawable_funcs.swap`, `opengl_driver.h:191`) is what
  `win32u_wglSwapBuffers` invokes.

The driver-funcs contract we must fill (`include/wine/opengl_driver.h:239-254`):

```
p_get_proc_address, p_init_pixel_formats, p_describe_pixel_format, p_init_extensions,
p_surface_create, p_context_create, p_context_destroy, p_make_current,
p_pbuffer_create, p_pbuffer_updated, p_pbuffer_bind
```

The `nulldrv_*` implementations in `win32u/opengl.c` (from `:1233`) are the reference
skeleton for the trivial ones (`p_init_pixel_formats`, `p_describe_pixel_format`,
`p_init_extensions`). Note the drawable's `EGLSurface surface` field is documented "for EGL
based drivers" (`opengl_driver.h:215`) — a non-EGL drawable simply doesn't use it, confirming
non-EGL drawables are a supported shape.

---

## 3. The implementation

A new backend inside `winewayland.drv`, selected on Haiku. It needs **libwayland-client**
(already a build dep, for the `wl_surface` + `wl_shm` present) and **libOSMesa** (new dep). It
does **not** use libEGL or libwayland-egl.

### 3.1 New file `dlls/winewayland.drv/opengl_osmesa.c` (unix lib)

Mirror the structure of the existing `opengl.c` but back it with OSMesa. Key pieces:

- **Drawable** — a CPU-buffer drawable analogous to `struct wayland_gl_drawable`
  (`opengl.c:48`) but with no EGL:
  ```c
  struct osmesa_gl_drawable {
      struct opengl_drawable base;            /* opengl_drawable_create(...) */
      struct wayland_client_surface *client;  /* wayland_client_surface_create(hwnd) */
      int width, height;
      void *buffer;                           /* WxH*4 render target (see 3.2 for zero-copy) */
  };
  ```
- **`p_get_proc_address(name)`** → `OSMesaGetProcAddress(name)`; if NULL, `dlsym(libGL_handle,
  name)` (open `libGL.so.1` once with `RTLD_NOW|RTLD_GLOBAL`). Covers core (from libGL) +
  extension entry points (from OSMesa).
- **`p_init_pixel_formats` / `p_describe_pixel_format`** — advertise a small set (one 32-bit
  RGBA, 24-bit depth, 8-bit stencil, double-buffered on-screen format is enough; add a
  pbuffer-capable variant). Copy `nulldrv`'s table shape (`win32u/opengl.c:1217`+) and set
  `pfd.dwFlags` PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW.
- **`p_context_create(format, share, attribs, void **context)`** → translate the incoming WGL
  `attribs` (major/minor/profile) to `OSMesaCreateContextAttribs` attributes
  (`OSMESA_FORMAT=OSMESA_BGRA`, `OSMESA_DEPTH_BITS=24`, `OSMESA_STENCIL_BITS=8`,
  `OSMESA_PROFILE=OSMESA_COMPAT_PROFILE`, `OSMESA_CONTEXT_MAJOR/MINOR_VERSION`). Store the
  `OSMesaContext` in `*context`. Fall back to `OSMesaCreateContextExt` if attribs unsupported.
  (wined3d requests compatibility contexts; llvmpipe gives 4.5 compat — verified.)
- **`p_context_destroy`** → `OSMesaDestroyContext`.
- **`p_make_current(draw, read, context)`** → `OSMesaMakeCurrent(context, draw->buffer,
  GL_UNSIGNED_BYTE, draw->width, draw->height)`. Call `OSMesaPixelStore(OSMESA_Y_UP, 0)` so the
  origin matches top-left `wl_shm` layout (OSMesa defaults to Y-up/bottom-origin). `read` is
  ignored for single-context software rendering (or assert draw==read; win32u passes the same
  drawable for the common case). NULL context → make-none-current.
- **Drawable `swap`** (the present, analogous to `wayland_drawable_swap`, `opengl.c:155`):
  1. `glFinish()` (ensure llvmpipe finished writing the buffer).
  2. Obtain/allocate a `wayland_shm_buffer` sized to the drawable (see 3.2).
  3. If not zero-copy, copy `buffer` → `shm->map_data`.
  4. `wayland_surface_attach_shm(wayland_surface, shm, damage)` then commit — reuse the
     driver's existing present path (`waylanddrv.h:319`, `wayland_surface.c:405`), the same
     one GDI window painting uses. `client_surface_present(base->client)` as in `opengl.c:159`.
- **Drawable `flush`** — handle resize like `wayland_gl_drawable_sync_size` (`opengl.c:70`):
  re-read `NtUserGetClientRect`, and on change realloc the render buffer + drop the cached
  `wayland_shm_buffer` so the next `swap` recreates it at the new size.
- **`p_surface_create(hwnd, format, **drawable)`** — `wayland_client_surface_create(hwnd)`
  (`waylanddrv.h:332`), `opengl_drawable_create(sizeof(struct osmesa_gl_drawable), &funcs,
  format, &client->client)`, size to client rect, allocate the render buffer,
  `set_client_surface(hwnd, client)`. Mirror `wayland_opengl_surface_create` (`opengl.c:83`)
  minus all EGL calls.
- **`p_pbuffer_create` / `_updated` / `_bind`** — offscreen render target with no on-screen
  surface: allocate a WxH*4 buffer, no `wayland_client_surface`. `_bind` returns `-1`
  ("use default implementation", as wayland does at `opengl.c:225`) so win32u's FBO-blit
  path handles texture-from-pbuffer.

### 3.2 Presentation format & zero-copy

- `wl_shm` `WL_SHM_FORMAT_XRGB8888` is **BGRA in little-endian memory**, which matches
  `OSMESA_BGRA`. So render **directly into `shm_buffer->map_data`** (zero-copy) when the shm
  stride equals `width*4`. `struct wayland_shm_buffer` exposes `void *map_data` + `width,
  height, format` (`waylanddrv.h:250-261`); create via `wayland_shm_buffer_create(width,
  height, WL_SHM_FORMAT_XRGB8888)` (`waylanddrv.h:348`).
  - **FLAG (verify, don't guess):** confirm `wayland_shm_buffer_create` uses stride ==
    `width*4` with no row padding (read `wayland_surface.c:779`+). If it pads, keep a private
    `width*height*4` OSMesa buffer and row-copy into `map_data` on swap.
  - **FLAG:** double-buffering. `wl_shm` buffers can be `busy` (held by the compositor) —
    reuse the driver's buffer-queue pattern (`window_surface.c:135`
    `wayland_buffer_queue_get_free_buffer`) rather than a single shared buffer, or keep the
    OSMesa render buffer private and copy into a free queue buffer per swap (simplest, one
    copy). Given editor redraw rates this copy is negligible.

### 3.3 Selection: `WAYLAND_OpenGLInit`

In `dlls/winewayland.drv/opengl.c:247`, install the OSMesa table on Haiku instead of the EGL
passthrough. Two clean options:

- **(preferred)** Guard the whole EGL body with the OSMesa path under a new
  `#ifdef HAVE_OSMESA` (set by configure, see 4). On Haiku, `WAYLAND_OpenGLInit` fills
  `wayland_driver_funcs` from the OSMesa backend (no `p_init_egl_platform`, no `egl_handle`
  requirement) and returns `STATUS_SUCCESS`. Keep the existing EGL path for non-Haiku Wayland
  builds.
- Do **not** copy `p_context_create`/`p_make_current`/`p_get_proc_address` from the incoming
  `egldrv` funcs (the current code does this at `opengl.c:259-265`) — the OSMesa backend
  supplies its own.

### 3.4 What we deliberately reuse (no reinvention)

`wayland_client_surface_create`, `set_client_surface`/`get_client_surface`,
`wayland_surface_attach_shm`, `wayland_shm_buffer_create`/`_ref`/`_unref`,
`client_surface_present`, `opengl_drawable_create`/`_release`,
`NtUserGetClientRect`/`NtUserGetDpiForWindow`. All already used by `opengl.c` / the GDI present
path; the OSMesa backend swaps only the GL production (OSMesa instead of EGL) and keeps the
same Wayland presentation.

---

## 4. Build system & recipe changes

- **`dlls/winewayland.drv/Makefile.in`** — add `opengl_osmesa.c` to sources and link OSMesa
  (`-lOSMesa`) on Haiku, guarded by the configure result. Follow how the tree already gates
  `wayland-egl`.
- **`configure.ac`** — add detection of `GL/osmesa.h` + `libOSMesa` producing `HAVE_OSMESA`
  (mirror the existing `HAVE_LIBWAYLAND_EGL` check). The recipe runs `autoreconf -i` already
  (`wine-11.8.recipe:102`), so a `configure.ac` edit regenerates cleanly.
- **`wine-11.8.recipe`**:
  - `BUILD_REQUIRES`: add `devel:libOSMesa` (from `mesa_devel`) — sits alongside the existing
    `devel:libEGL`, `devel:libwayland_egl` lines (`:67,77`).
  - `REQUIRES`: add `lib:libOSMesa` so the runtime dep is pulled (next to
    `lib:libvulkan`, `:49`).
  - Bump `REVISION` (currently `3`, `:8`) — never reuse a published revision.
- New patch: add `opengl_osmesa.c` + the `opengl.c` / `Makefile.in` / `configure.ac` diffs to
  `patches/wine-11.8.patchset` (this set already patches `winewayland.drv` input files, so the
  driver is in-scope for the port's patchset).

---

## 5. Test plan (each gate green before the next)

**Outcome: the target gate is fully green as of 2026-07-27.** Reaching it needed one thing this
plan did not anticipate — see the note after the list. Gate 2 remains the only one never run in
isolation.

1. **Unit probe:** `osprobe` prints GL 4.5 + correct pixel. ✅
2. **Wine GL smoke:** build the patched Wine; run a trivial Win GL app (or
   `WINEDEBUG=+wgl,+opengl wine winecfg`'s GL detection) and confirm `wglCreateContext` /
   `glGetString(GL_VERSION)` returns `4.5 ... llvmpipe` — i.e. `p_get_proc_address` +
   `p_context_create` path works end to end. Use `wine glxgears`-equivalent or Wine's
   `dlls/opengl32/tests` if runnable.
   ⬜ **Never run as a discrete test.** Gates 3–5 passing entails this path works, but no
   isolated GL smoke test was performed — so a regression here would first show up as a
   failure further down.
3. **wined3d:** run a small D3D9/11 sample (or `WINEDEBUG=+d3d` on any D3D app). The old
   `wined3d_caps_gl_ctx_create Failed to find a suitable pixel format` must be gone and a
   device must create against llvmpipe. ✅ — that failure is gone and devices create.
4. **Present path:** confirm a rendered frame appears in the floating Wayland window (color
   correct, not swizzled, not vertically flipped — validates OSMESA_BGRA + `OSMESA_Y_UP=0`).
   ✅ **with a caveat:** editors display and are usable, but no deliberate colour-swizzle or
   vertical-flip check was made — this passed on "the GUI looks right", not on a test pattern.
5. **Target:** in jackDAW-haiku, add an NA VST3 (e.g. NA Black) → open the FX editor → the D3D
   GUI renders and is interactive (knob drags update). Software rate is expected but usable.
   Confirm audio still runs and closing the FX window no longer forces a jackd/jackDAW restart
   (that symptom was scoped to the non-displaying plugins and should vanish once they display).
   ✅ **fully green** (REVISION 8, hrev59899, 2026-07-27): the GUI renders, it is interactive,
   and repeated open/close cycles of the plug-in editors leave both `jackd` and jackDAW
   running — the forced-restart symptom is gone, as predicted once the plug-ins displayed.

**Re-run under `winerenku.drv`, 2026-11-11** (RenkuOS build VM, hrev60072+518, Mesa 22.0.5,
from the work tree, three runs each): gate 1 `osprobe` passes; gates 2 and 4 by
`probes/glprobe.c`, 29/29, and gate 3 by `probes/d3dprobe.c`, 8/8 (D3D9 and D3D11 devices). Gate
4 now has what it lacked before: a test pattern read back from the screen itself (right way up,
right colours) by `probes/screencheck.cpp`, plus a GL child window, the GDI fallback under an
overlapping window, the last frame of a burst, and a resize. Gate 5 waits for vstbridge to be
rebuilt on 11.21. Bare metal not checked yet.

### What this plan missed

Rendering and interactivity turned out to be two separate problems. This backend delivered the
first: with it the NA editors display, without it they do not. They then stayed frozen — one
painted frame, no response to mouse or keyboard, including clicks delivered straight to the
floating Wayland window, bypassing all of vstbridge's input injection.

The cause was outside the GL path entirely: `dxgi_output_WaitForVBlank()` was a stub returning
`E_NOTIMPL` without waiting. JUCE 8's Direct2D renderer drives its whole repaint cadence from a
thread that calls it in a loop, so that thread spun — 62,024 calls in ~25 s, saturating a core
— while Wine was delivering input correctly all along. Making it sleep to the next refresh
boundary is the fix, and it is generic Wine rather than anything Haiku-specific: the same gap
is why these plug-ins are reported as needing DXVK on Linux, DXVK shipping its own DXGI with a
real vblank wait.

The lesson for this plan: "the GL backend works" and "the app is usable" are different claims,
and gate 5 is the only one that distinguishes them.

---

## 6. Risks / flagged unknowns (resolve against source, do not guess)

- **shm stride/padding** (§3.2) — verify before choosing zero-copy vs row-copy.
- **Context attribs translation** — confirm the WGL→OSMesa attrib mapping wined3d actually
  requests (core vs compat, forward-compat bit). llvmpipe offers 4.5 compat; wined3d is happy
  with compat. Read what wined3d passes via `wglCreateContextAttribsARB` and ensure
  `p_context_create` honors major/minor/profile.
- **`p_make_current` read≠draw** — software OSMesa binds a single buffer; verify win32u never
  requires distinct read/draw for this workload (pbuffer readback uses FBO blit in win32u, not
  separate GL contexts).
- **Threading** — OSMesa context is thread-local; Wine issues GL on the app thread. vstbridge
  runs the plugin editor on its Wine host process/thread — confirm make-current happens on the
  same thread that issues GL (it does, per the WGL contract).
- **Performance** — llvmpipe is CPU rasterization; fine for editor redraw. Measure knob-drag
  latency on a real plugin; if a heavy editor is sluggish, that's a UX note, not a
  correctness failure, and the same code runs unchanged on the GPU/X547 box where a hardware
  ICD would replace llvmpipe.
- **OSMesa Y-orientation / format swizzle** — validated only by the §5.4 visual test; if
  colors are swapped, revisit `OSMESA_FORMAT` (BGRA vs ARGB vs RGBA) against the actual
  `wl_shm` format the compositor negotiates.

## 7. Why this is the right route (vs the alternatives)

- **vs DXVK/Vulkan-WSI:** dead on the laptop (Vulkan loader exposes zero surface extensions)
  and needs a GPU ICD elsewhere. OSMesa needs none of that and runs on any machine.
- **vs porting Mesa's Wayland-EGL platform to Haiku:** larger (Mesa build + compositor
  protocol work) and would still be software (llvmpipe) in the end. OSMesa reaches the same
  llvmpipe with a self-contained Wine-side patch.
- **vs the GPU/X547 path:** complementary, not competing. Because the whole GL layer is now
  driver-pluggable and format-agnostic, the GPU box can later swap llvmpipe for a hardware
  ICD; this software path proves the plumbing first and gives every machine a working GUI.
