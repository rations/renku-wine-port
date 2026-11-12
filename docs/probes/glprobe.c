/* glprobe: OpenGL through Wine's display driver, gates 2 and 4 of OSMESA-GL-PLAN.md section 5.
 *
 * A Windows program (build it with a Windows compiler, as plug-ins are built):
 *   x86_64-w64-mingw32-gcc -O1 -Wall -o glprobe.exe glprobe.c -lopengl32 -lgdi32 -luser32
 * Run under Wine, on RenkuOS with screencheck (screencheck.cpp) reading what it prints:
 *   wine glprobe.exe --screen | ./screencheck
 * Without --screen only the checks Win32 itself can make are run.
 *
 * winerenku.drv shows a GL window's frames in one of two ways: in a view of its own (a layer)
 * when nothing clips the window, otherwise by drawing them into the window with GDI. A layer's
 * frames never pass through anything Win32 can read, so for those glprobe prints the screen
 * point and the colour it expects ("SCREEN x y rrggbb what"), and screencheck reads the screen.
 *
 * Gate 2: a legacy and a 3.2 core context through the driver, with the GL strings. Gate 4: a
 * test pattern (top half green, bottom half red), read back with glReadPixels and checked on
 * the screen: colour order and orientation. Then a GL child window (a plug-in editor's
 * shape), unclipped (a layer); the same child overlapped by a sibling window (drawn with GDI:
 * checked with GetPixel too, and the sibling must stay on top); and a resize. PASS/FAIL per
 * check; the exit status is the number of failures. */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#define GL_CONTEXT_PROFILE_MASK          0x9126
#define GL_SCISSOR_TEST_                 0x0C11
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);

static int failures, screen;

static void check(int ok, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
    fflush(stdout);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) return 1;  /* GL owns the client area */
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    do
    {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(10);
    } while ((LONG)(GetTickCount() - end) < 0);
}

static int set_format(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1 };
    int pf;

    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    if (!(pf = ChoosePixelFormat(hdc, &pfd))) return 0;
    DescribePixelFormat(hdc, pf, sizeof(pfd), &pfd);
    if (!SetPixelFormat(hdc, pf, &pfd)) return 0;
    return pf;
}

/* Top half green, bottom half red, by clear + scissor: no shaders, any GL version. */
static void draw_pattern(int width, int height)
{
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST_);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST_);
    glScissor(0, height / 2, width, height - height / 2);  /* GL's y runs upwards */
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST_);
}

static void fill(float r, float g, float b, int width, int height)
{
    glViewport(0, 0, width, height);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* What Win32 reads back: the window surface, which only GDI-drawn frames reach. */
static void check_gdi(HWND hwnd, int x, int y, COLORREF want, const char *what)
{
    char detail[128];
    HDC hdc = GetDC(hwnd);
    COLORREF got = GetPixel(hdc, x, y);
    ReleaseDC(hwnd, hdc);
    snprintf(detail, sizeof(detail), "(%d,%d) is %06lx, want %06lx", x, y, (unsigned long)got, (unsigned long)want);
    check(got == want, what, detail);
}

/* What the screen shows, checked by screencheck. want is 0xRRGGBB. */
static void expect_screen(HWND hwnd, int x, int y, unsigned int want, const char *what)
{
    POINT pt = { x, y };
    if (!screen) return;
    ClientToScreen(hwnd, &pt);
    printf("SCREEN %ld %ld %06x %s\n", pt.x, pt.y, want, what);
    fflush(stdout);
}

/* After the expectations are printed: gives screencheck time to read the screen before
 * anything changes. Each stage first lets its frames reach the screen (pump). */
static void let_check(void)
{
    if (screen) pump(700);
}

static HWND create_panel(HWND parent, int x, int y, int w, int h)
{
    WNDCLASSA wc = {0};
    HWND panel;

    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
    wc.lpszClassName = "glprobe-panel";
    RegisterClassA(&wc);
    panel = CreateWindowA("glprobe-panel", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, x, y, w, h,
                          parent, NULL, wc.hInstance, NULL);
    SetWindowPos(panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return panel;
}

int main(int argc, char **argv)
{
    PFN_wglCreateContextAttribsARB create_attribs;
    unsigned char px[4];
    const char *str;
    char detail[256];
    HWND top, child, panel;
    HDC hdc, chdc;
    HGLRC rc, core, crc;
    WNDCLASSA wc = {0};
    RECT rect;
    GLint profile = 0;
    DWORD start;
    int i, frames;

    screen = argc > 1 && !strcmp(argv[1], "--screen");

    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "glprobe";
    RegisterClassA(&wc);

    top = CreateWindowA("glprobe", "glprobe", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                        60, 60, 336, 279, NULL, NULL, wc.hInstance, NULL);
    check(top != NULL, "top-level window", NULL);
    if (!top) return failures;
    pump(300);
    GetClientRect(top, &rect);
    printf("      client area %ldx%ld\n", rect.right, rect.bottom);

    /* --- gate 2: contexts ------------------------------------------------------------------ */
    hdc = GetDC(top);
    i = set_format(hdc);
    snprintf(detail, sizeof(detail), "format %d", i);
    check(i != 0, "ChoosePixelFormat + SetPixelFormat", detail);
    rc = wglCreateContext(hdc);
    check(rc != NULL, "wglCreateContext", NULL);
    if (!rc || !wglMakeCurrent(hdc, rc)) { check(0, "wglMakeCurrent", NULL); return failures; }

    printf("      GL_VERSION  %s\n", (const char *)glGetString(GL_VERSION));
    printf("      GL_RENDERER %s\n", (const char *)glGetString(GL_RENDERER));
    printf("      GL_VENDOR   %s\n", (const char *)glGetString(GL_VENDOR));
    str = (const char *)glGetString(GL_RENDERER);
    check(str && strstr(str, "llvmpipe"), "renderer is llvmpipe (OSMesa)", NULL);

    create_attribs = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    check(create_attribs != NULL, "wglGetProcAddress(wglCreateContextAttribsARB)", NULL);
    if (create_attribs)
    {
        static const int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 2,
                                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        core = create_attribs(hdc, NULL, attribs);
        check(core != NULL, "wglCreateContextAttribsARB 3.2 core", NULL);
        if (core && wglMakeCurrent(hdc, core))
        {
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
            snprintf(detail, sizeof(detail), "%s, profile mask %#x", (const char *)glGetString(GL_VERSION), profile);
            check(profile & WGL_CONTEXT_CORE_PROFILE_BIT_ARB, "core context current", detail);
            wglMakeCurrent(hdc, rc);
            wglDeleteContext(core);
        }
    }

    /* --- gate 4: the pattern, in GL and on the screen (top-level, unclipped: a layer) -------- */
    draw_pattern(rect.right, rect.bottom);
    glFinish();
    glReadPixels(rect.right / 2, rect.bottom - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(detail, sizeof(detail), "top is %u,%u,%u", px[0], px[1], px[2]);
    check(px[0] == 0 && px[1] == 255 && px[2] == 0, "glReadPixels top half green", detail);
    glReadPixels(rect.right / 2, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(detail, sizeof(detail), "bottom is %u,%u,%u", px[0], px[1], px[2]);
    check(px[0] == 255 && px[1] == 0 && px[2] == 0, "glReadPixels bottom half red", detail);

    check(SwapBuffers(hdc), "SwapBuffers", NULL);
    pump(300);
    expect_screen(top, rect.right / 2, 4, 0x00ff00, "top-level: top is green (not flipped, not swizzled)");
    expect_screen(top, rect.right / 2, rect.bottom - 5, 0xff0000, "top-level: bottom is red");
    expect_screen(top, 0, 0, 0x00ff00, "top-level: first pixel of the client area is GL's");
    let_check();

    start = GetTickCount();
    for (frames = 0; GetTickCount() - start < 1000; frames++)
    {
        fill((frames & 1) ? 1.0f : 0.0f, 0.5f, 0.5f, rect.right, rect.bottom);
        SwapBuffers(hdc);
    }
    printf("      %d frames in 1 s at %ldx%ld (clear + swap)\n", frames, rect.right, rect.bottom);
    /* Straight after the burst, while the window may still be frames behind: the last frame
     * must be the one left on screen, not an older one. */
    fill(0, 1, 1, rect.right, rect.bottom);
    SwapBuffers(hdc);
    pump(300);
    expect_screen(top, rect.right / 2, rect.bottom / 2, 0x00ffff, "after a burst of frames the last one stays (cyan)");
    let_check();

    /* --- a GL child window, as a plug-in editor is: unclipped, so a layer ---------------------- */
    child = CreateWindowA("glprobe", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 40, 30, 100, 80,
                          top, NULL, wc.hInstance, NULL);
    check(child != NULL, "child window", NULL);
    pump(100);
    /* The top-level has a child now, which clips it (WS_CLIPCHILDREN): it is drawn with GDI. */
    draw_pattern(rect.right, rect.bottom);
    SwapBuffers(hdc);
    chdc = GetDC(child);
    set_format(chdc);
    crc = wglCreateContext(chdc);
    check(crc && wglMakeCurrent(chdc, crc), "context on the child", NULL);
    fill(0, 0, 1, 100, 80);
    SwapBuffers(chdc);
    pump(300);
    check_gdi(top, 150, 35, RGB(0, 255, 0), "top-level, now clipped by its child, drawn with GDI: green beside the child");
    expect_screen(child, 5, 5, 0x0000ff, "child: near corner is blue");
    expect_screen(child, 99, 79, 0x0000ff, "child: far corner is blue");
    expect_screen(top, 150, 35, 0x00ff00, "top-level: beside the child is green");
    expect_screen(top, 39, 29, 0x00ff00, "top-level: just outside the child's corner is green");
    let_check();

    /* --- the child overlapped by a sibling: clipped, so drawn with GDI ------------------------ */
    panel = create_panel(top, 90, 20, 80, 50);
    pump(200);
    fill(1, 1, 0, 100, 80);
    SwapBuffers(chdc);
    pump(300);
    check_gdi(child, 5, 5, RGB(255, 255, 0), "clipped child, drawn with GDI: yellow");
    expect_screen(child, 5, 5, 0xffff00, "clipped child: visible part is yellow");
    expect_screen(child, 70, 20, 0xff00ff, "clipped child: the sibling over it stays on top");
    expect_screen(child, 70, 70, 0xffff00, "clipped child: below the sibling is yellow");
    let_check();

    /* --- back to a layer, then a resize of the top-level, alone again ----------------------- */
    DestroyWindow(panel);
    pump(100);
    fill(0, 0, 1, 100, 80);
    SwapBuffers(chdc);
    pump(200);
    expect_screen(child, 70, 20, 0x0000ff, "unclipped again: where the sibling was is blue");
    let_check();

    wglMakeCurrent(hdc, rc);
    wglDeleteContext(crc);
    ReleaseDC(child, chdc);
    DestroyWindow(child);
    SetWindowPos(top, NULL, 0, 0, 436, 339, SWP_NOMOVE | SWP_NOZORDER);
    pump(300);
    GetClientRect(top, &rect);
    draw_pattern(rect.right, rect.bottom);
    SwapBuffers(hdc);
    pump(300);
    snprintf(detail, sizeof(detail), "new client area %ldx%ld", rect.right, rect.bottom);
    check(rect.right > 320, "resized", detail);
    expect_screen(top, rect.right - 5, rect.bottom - 5, 0xff0000, "resized: new bottom-right corner is red");
    expect_screen(top, rect.right - 5, 4, 0x00ff00, "resized: new top-right corner is green");
    let_check();

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    DestroyWindow(top);
    printf("%d failure(s)\n", failures);
    return failures;
}
