/* d3dprobe: Direct3D through wined3d on the driver's OpenGL, gate 3 of OSMESA-GL-PLAN.md section 5.
 *
 * A Windows program:
 *   x86_64-w64-mingw32-gcc -O1 -Wall -o d3dprobe.exe d3dprobe.c -ld3d9 -ld3d11 -ldxgi -lgdi32 -luser32
 * Run under Wine, on RenkuOS with screencheck (screencheck.cpp) reading what it prints:
 *   wine d3dprobe.exe --screen | ./screencheck
 *
 * Creates a D3D9 device and a D3D11 device with a swap chain, each on its own window, clears to
 * a colour and presents. With --screen it prints the screen point and the colour expected there
 * ("SCREEN x y rrggbb what") for screencheck to read: winerenku.drv shows an unclipped GL
 * window's frames in a view of its own, which nothing in Win32 can read back. The failure this
 * gate was about is wined3d's "Failed to find a suitable pixel format", after which device
 * creation fails. Prints PASS/FAIL per check; the exit status is the number of failures. */

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

static int failures, screen;

static void check(int ok, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
    fflush(stdout);
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

/* What the screen shows, checked by screencheck. want is 0xRRGGBB. Lets the frame reach the
 * screen first, and gives screencheck time to read it after. */
static void expect_screen(HWND hwnd, int x, int y, unsigned int want, const char *what)
{
    POINT pt = { x, y };
    pump(300);
    if (!screen) return;
    ClientToScreen(hwnd, &pt);
    printf("SCREEN %ld %ld %06x %s\n", pt.x, pt.y, want, what);
    fflush(stdout);
    pump(700);
}

static HWND make_window(const char *title, int x)
{
    HWND hwnd = CreateWindowA("d3dprobe", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, 80, 336, 279,
                              NULL, NULL, GetModuleHandleA(NULL), NULL);
    pump(300);
    return hwnd;
}

static void test_d3d9(void)
{
    D3DPRESENT_PARAMETERS pp = {0};
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d;
    char detail[256];
    HRESULT hr;
    HWND hwnd;
    RECT rect;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    check(d3d != NULL, "Direct3DCreate9", NULL);
    if (!d3d) return;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id)))
        printf("      D3D9 adapter: %s (%s)\n", id.Description, id.Driver);

    hwnd = make_window("d3dprobe D3D9", 40);
    GetClientRect(hwnd, &rect);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = rect.right;
    pp.BackBufferHeight = rect.bottom;
    pp.hDeviceWindow = hwnd;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    snprintf(detail, sizeof(detail), "hr %#lx", (unsigned long)hr);
    check(SUCCEEDED(hr) && device, "IDirect3D9::CreateDevice (HAL, windowed)", detail);
    if (device)
    {
        /* orange: red and blue swapped would show as azure, which the D3D11 test uses */
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 128, 0), 1.0f, 0);
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        snprintf(detail, sizeof(detail), "hr %#lx", (unsigned long)hr);
        check(SUCCEEDED(hr), "IDirect3DDevice9::Present", detail);
        expect_screen(hwnd, rect.right / 2, rect.bottom / 2, 0xff8000, "D3D9 window is orange");
        IDirect3DDevice9_Release(device);
    }
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
}

static void test_d3d11(void)
{
    static const float colour[4] = { 0.0f, 128.0f / 255.0f, 1.0f, 1.0f };  /* azure */
    DXGI_SWAP_CHAIN_DESC sd = {0};
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL level = 0;
    char detail[256];
    HRESULT hr;
    HWND hwnd;
    RECT rect;

    hwnd = make_window("d3dprobe D3D11", 400);
    GetClientRect(hwnd, &rect);
    sd.BufferDesc.Width = rect.right;
    sd.BufferDesc.Height = rect.bottom;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                       &sd, &swapchain, &device, &level, &context);
    snprintf(detail, sizeof(detail), "hr %#lx, feature level %#x", (unsigned long)hr, level);
    check(SUCCEEDED(hr) && device && swapchain, "D3D11CreateDeviceAndSwapChain (hardware)", detail);
    if (SUCCEEDED(hr))
    {
        hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &rtv);
        snprintf(detail, sizeof(detail), "hr %#lx", (unsigned long)hr);
        check(SUCCEEDED(hr), "back buffer render target view", detail);
        if (rtv)
        {
            ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colour);
            hr = IDXGISwapChain_Present(swapchain, 0, 0);
            snprintf(detail, sizeof(detail), "hr %#lx", (unsigned long)hr);
            check(SUCCEEDED(hr), "IDXGISwapChain::Present", detail);
            expect_screen(hwnd, rect.right / 2, rect.bottom / 2, 0x0080ff, "D3D11 window is azure");
            ID3D11RenderTargetView_Release(rtv);
        }
        if (back) ID3D11Texture2D_Release(back);
        ID3D11DeviceContext_Release(context);
        IDXGISwapChain_Release(swapchain);
        ID3D11Device_Release(device);
    }
    DestroyWindow(hwnd);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc = {0};

    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "d3dprobe";
    RegisterClassA(&wc);

    screen = argc > 1 && !strcmp(argv[1], "--screen");
    test_d3d9();
    test_d3d11();
    printf("%d failure(s)\n", failures);
    return failures;
}
