/*
 * hostprobe: a window shown in a view of another program, winerenku.drv's hosted mode
 * (dlls/winerenku.drv/renku_host.h in the dev tree).
 *
 * This is the Wine side. It opts one top-level window in (the RENKU_HOST_PROP_HOSTED property),
 * opens one ordinary window beside it, and prints what each window receives. hostprobe_host
 * (hostprobe_host.cpp) reads that, shows the hosted window in a BView of its own, drives input
 * through the driver and judges; screencheck checks the screen. The hosted window has an OpenGL
 * child, as a plug-in editor may: with no BWindow to put a GL view in, its frames have to reach
 * the host through the window's surface.
 *
 * Build against a built Wine tree $W (Winelib, like kbdprobe):
 *   $W/tools/winegcc/winegcc --wine-objdir $W -mconsole -o hostprobe.exe hostprobe.c \
 *     -L$W/libs/winecrt0 -L$W/dlls/kernel32 -L$W/dlls/ntdll -L$W/dlls/user32 -L$W/dlls/gdi32 \
 *     -L$W/dlls/opengl32 -lwinecrt0 -lkernel32 -lntdll -luser32 -lgdi32 -lopengl32
 * Run: $W/loader/wine hostprobe.exe.so | ./hostprobe_host | ./screencheck
 */

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include "wine/wgl.h"

#define HOSTED_W 200
#define HOSTED_H 150
#define NORMAL_X 600
#define NORMAL_Y 120

static HWND hosted, normal, popup, gl_child;
static HGLRC gl_context;
static BOOL repainted_blue;

static void out(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

static void fill(HDC hdc, int left, int top, int right, int bottom, COLORREF color)
{
    RECT rect = { left, top, right, bottom };
    HBRUSH brush = CreateSolidBrush(color);

    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        fill(hdc, 0, 0, 100, 40, RGB(255, 255, 0));
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Magenta, every time it is painted. */
static LRESULT CALLBACK gl_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        PIXELFORMATDESCRIPTOR pfd = {0};
        HDC hdc = GetDC(hwnd);
        int format;

        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        if (!(format = ChoosePixelFormat(hdc, &pfd)) || !SetPixelFormat(hdc, format, &pfd) ||
            !(gl_context = wglCreateContext(hdc)))
            out("FAIL no GL context for the hosted window's child\n");
        ReleaseDC(hwnd, hdc);
        SetTimer(hwnd, 3, 200, NULL);
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (gl_context && wglMakeCurrent(hdc, gl_context))
        {
            glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            SwapBuffers(hdc);
            wglMakeCurrent(NULL, NULL);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK hosted_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (repainted_blue) fill(hdc, 0, 0, HOSTED_W, HOSTED_H, RGB(0, 0, 255));
        else
        {
            fill(hdc, 0, 0, HOSTED_W, HOSTED_H / 2, RGB(0, 255, 0));
            fill(hdc, 0, HOSTED_H / 2, HOSTED_W, HOSTED_H, RGB(255, 0, 0));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_WINDOWPOSCHANGED:
    {
        RECT rect;
        GetWindowRect(hwnd, &rect);
        out("POS %ld %ld\n", rect.left, rect.top);
        break;
    }
    case WM_ACTIVATE:
        out("ACTIVATE %d\n", LOWORD(wp) != WA_INACTIVE);
        break;
    case WM_LBUTTONDOWN:
        out("LBUTTONDOWN %d %d\n", (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_RBUTTONDOWN:
    {
        /* As a drop-down list would: an owned popup just below the window. */
        POINT pt = { 0, HOSTED_H + 10 };
        ClientToScreen(hwnd, &pt);
        if (!popup)
            popup = CreateWindowExA(WS_EX_TOOLWINDOW, "hostprobe popup", "", WS_POPUP, pt.x, pt.y,
                                    100, 40, hwnd, NULL, GetModuleHandleA(NULL), NULL);
        ShowWindow(popup, SW_SHOWNOACTIVATE);
        UpdateWindow(popup);
        out("POPUP %ld %ld\n", pt.x, pt.y);
        return 0;
    }
    case WM_KEYDOWN:
        out("KEYDOWN %#x\n", (unsigned int)wp);
        if (wp == 'A')
        {
            repainted_blue = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (wp == VK_ESCAPE)
        {
            if (popup) DestroyWindow(popup);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CHAR:
        out("CHAR %#x\n", (unsigned int)wp);
        return 0;
    case WM_MOUSEWHEEL:
        out("WHEEL %d\n", (short)HIWORD(wp));
        return 0;
    case WM_DESTROY:
        out("DESTROYED\n");
        hosted = NULL;
        /* Long enough for the host to see the window go. */
        SetTimer(normal, 2, 1500, NULL);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK normal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rect;
        HDC hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rect);
        fill(hdc, 0, 0, rect.right, rect.bottom, RGB(0, 0, 255));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1)
        {
            /* An ordinary window still gets a BWindow of its own, on screen. */
            POINT pt = { 60, 40 };
            KillTimer(hwnd, 1);
            ClientToScreen(hwnd, &pt);
            out("SCREEN %ld %ld 0000ff an ordinary window is a BWindow of its own\n", pt.x, pt.y);
        }
        else PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void register_class(const char *name, WNDPROC proc)
{
    WNDCLASSA cls = {0};

    cls.lpfnWndProc = proc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    cls.lpszClassName = name;
    RegisterClassA(&cls);
}

int main(void)
{
    MSG msg;
    DWORD start = GetTickCount();
    RECT rect = { 0, 0, 120, 80 };
    HANDLE team;

    register_class("hostprobe hosted", hosted_proc);
    register_class("hostprobe normal", normal_proc);
    register_class("hostprobe popup", popup_proc);
    register_class("hostprobe gl", gl_proc);

    hosted = CreateWindowExA(0, "hostprobe hosted", "hosted", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                             HOSTED_W, HOSTED_H, NULL, NULL, GetModuleHandleA(NULL), NULL);
    /* Top right, clear of the points the host clicks and reads. */
    gl_child = CreateWindowExA(0, "hostprobe gl", "", WS_CHILD | WS_VISIBLE, HOSTED_W - 50, 0, 50,
                               50, hosted, NULL, GetModuleHandleA(NULL), NULL);
    /* Before the window is first shown: that is when the driver decides. */
    SetPropA(hosted, "__wine_renku_hosted", (HANDLE)1);
    ShowWindow(hosted, SW_SHOWNOACTIVATE);
    UpdateWindow(hosted);
    team = GetPropA(hosted, "__wine_renku_team");
    if (!team) out("FAIL the driver set no team property (not winerenku.drv, or no hosted mode)\n");
    out("HOSTPROBE %d %lu\n", (int)(INT_PTR)team, (unsigned long)(UINT_PTR)hosted);

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    normal = CreateWindowExA(0, "hostprobe normal", "hostprobe ordinary window",
                             WS_OVERLAPPEDWINDOW, NORMAL_X, NORMAL_Y, rect.right - rect.left,
                             rect.bottom - rect.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
    ShowWindow(normal, SW_SHOWNOACTIVATE);
    UpdateWindow(normal);
    SetTimer(normal, 1, 1000, NULL);

    /* Never hangs: gives up after 40 seconds whatever the host does. */
    while (GetTickCount() - start < 40000)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT) == WAIT_TIMEOUT) continue;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    out("FAIL hostprobe.exe timed out\n");
done:
    out("EXIT\n");
    return 0;
}
