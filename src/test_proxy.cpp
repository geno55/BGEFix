/*
 * test_proxy - functional test for d3d9_windowed.
 *
 * Loads the built d3d9.dll, asks for an EXCLUSIVE FULLSCREEN device exactly the way
 * BGE does, and asserts that the proxy rewrote the request into a windowed one and
 * restyled the window. Run it from the folder containing the built d3d9.dll.
 *
 * The interface, D3DPRESENT_PARAMETERS and the D3D constants all come from the SDK's
 * d3d9.h (Include\<ver>\shared) so this test cannot drift from the real ABI. The DLL is
 * still loaded with LoadLibrary rather than an import, so we exercise the proxy the same
 * way the game does.
 *
 * Build: see build_test.cmd (32-bit).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static int g_fail = 0;
static void check(const char* what, int ok, const char* detail)
{
    printf("  %-4s %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail && *detail ? " -> " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

int main(void)
{
    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "BGEProxyTest";
    RegisterClassA(&wc);

    /* A conventional fullscreen-style window, like the game creates. */
    HWND hwnd = CreateWindowExA(0, "BGEProxyTest", "proxy test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("could not create window\n"); return 2; }

    HMODULE m = LoadLibraryA("d3d9.dll");
    check("proxy d3d9.dll loads", m != NULL, "");
    if (!m) return 2;

    char loaded[MAX_PATH] = {0};
    GetModuleFileNameA(m, loaded, MAX_PATH);
    printf("       loaded: %s\n", loaded);

    typedef IDirect3D9* (WINAPI *PFN)(UINT);
    PFN create = (PFN)GetProcAddress(m, "Direct3DCreate9");
    check("Direct3DCreate9 exported", create != NULL, "");
    check("DebugSetMute exported", GetProcAddress(m, "DebugSetMute") != NULL, "");
    if (!create) return 2;

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    check("Direct3DCreate9 returns object", d3d != NULL, "");
    if (!d3d) return 2;

    /* A slot far down the vtable: catches any layout drift between the proxy and the
     * real interface, which a forwarding-only test would otherwise never touch. */
    HMONITOR mon = d3d->GetAdapterMonitor(D3DADAPTER_DEFAULT);
    check("GetAdapterMonitor forwards", mon != NULL, "");

    UINT adapters = d3d->GetAdapterCount();
    char buf[128];
    wsprintfA(buf, "count=%u", adapters);
    check("GetAdapterCount forwards", adapters > 0, buf);

    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth            = 800;
    pp.BackBufferHeight           = 600;
    pp.BackBufferFormat           = D3DFMT_X8R8G8B8;
    pp.BackBufferCount            = 1;
    pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow              = hwnd;
    pp.Windowed                   = FALSE;   /* <-- exclusive fullscreen, as BGE asks */
    pp.FullScreen_RefreshRateInHz = 60;      /* <-- illegal once windowed */

    printf("\n  request: Windowed=%d refresh=%u\n", pp.Windowed, pp.FullScreen_RefreshRateInHz);

    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);

    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("CreateDevice succeeded", SUCCEEDED(hr), buf);

    wsprintfA(buf, "Windowed=%d", pp.Windowed);
    check("proxy forced Windowed=TRUE", pp.Windowed == TRUE, buf);

    wsprintfA(buf, "refresh=%u", pp.FullScreen_RefreshRateInHz);
    check("refresh rate zeroed for windowed", pp.FullScreen_RefreshRateInHz == 0, buf);

    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    wsprintfA(buf, "style=0x%08X", (unsigned)style);
    check("window restyled to WS_POPUP", (style & WS_POPUP) != 0, buf);
    check("caption removed", (style & WS_CAPTION) == 0, buf);

    RECT rc; GetClientRect(hwnd, &rc);
    wsprintfA(buf, "client=%dx%d", rc.right, rc.bottom);
    check("client area matches backbuffer", rc.right == 800 && rc.bottom == 600, buf);

    /* An exclusive-fullscreen device activates the window as part of taking the display.
     * Windowed, nothing does that for us - and BGE treats an inactive window as "not
     * playing": it stops polling DirectInput and silences audio. The game then ignores
     * the controller entirely until you alt-tab away and back. */
    HWND fg = GetForegroundWindow();
    wsprintfA(buf, "foreground=0x%08X ours=0x%08X", (unsigned)(UINT_PTR)fg, (unsigned)(UINT_PTR)hwnd);
    check("window is ACTIVE after CreateDevice", fg == hwnd, buf);

    /* ---------------------------------------------------------------- Reset --------
     * The other place a D3D9 app sets Windowed. This is what the game does on
     * device-lost recovery and on a resolution change from its options menu. A proxy
     * that only rewrites CreateDevice passes every check above and still drops the
     * game back into exclusive fullscreen here. */
    if (dev) {
        printf("\n  -- device reset (resolution change / device-lost recovery) --\n");

        IDirect3D9* fromDevice = NULL;
        hr = dev->GetDirect3D(&fromDevice);
        check("device is wrapped (GetDirect3D returns the proxy)",
              SUCCEEDED(hr) && fromDevice == d3d, "");
        if (fromDevice) fromDevice->Release();

        D3DPRESENT_PARAMETERS rp; ZeroMemory(&rp, sizeof(rp));
        rp.BackBufferWidth            = 640;
        rp.BackBufferHeight           = 480;
        rp.BackBufferFormat           = D3DFMT_X8R8G8B8;
        rp.BackBufferCount            = 1;
        rp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
        rp.hDeviceWindow              = hwnd;
        rp.Windowed                   = FALSE;   /* <-- back to fullscreen, as BGE asks */
        rp.FullScreen_RefreshRateInHz = 60;

        printf("  request: Windowed=%d refresh=%u %ux%u\n",
               rp.Windowed, rp.FullScreen_RefreshRateInHz,
               rp.BackBufferWidth, rp.BackBufferHeight);

        hr = dev->Reset(&rp);
        wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
        check("Reset succeeded", SUCCEEDED(hr), buf);

        wsprintfA(buf, "Windowed=%d", rp.Windowed);
        check("proxy forced Windowed=TRUE on Reset", rp.Windowed == TRUE, buf);

        wsprintfA(buf, "refresh=%u", rp.FullScreen_RefreshRateInHz);
        check("refresh rate zeroed on Reset", rp.FullScreen_RefreshRateInHz == 0, buf);

        style = GetWindowLongA(hwnd, GWL_STYLE);
        wsprintfA(buf, "style=0x%08X", (unsigned)style);
        check("window still borderless after Reset",
              (style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0, buf);

        GetClientRect(hwnd, &rc);
        wsprintfA(buf, "client=%dx%d", rc.right, rc.bottom);
        check("window resized to the new backbuffer",
              rc.right == 640 && rc.bottom == 480, buf);
    }

    if (dev) dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);

    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
