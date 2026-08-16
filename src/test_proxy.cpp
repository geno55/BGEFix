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
#include <string.h>

static int g_fail = 0;
static void check(const char* what, int ok, const char* detail)
{
    printf("  %-4s %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail && *detail ? " -> " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* ------------------------------------------------------------------ helpers */

/* Paths are built from the test executable's own folder rather than the working
 * directory, so the fixtures are found however the test is launched. */
static void PathBesideExe(char* out, const char* leaf)
{
    DWORD n = GetModuleFileNameA(NULL, out, MAX_PATH);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') --n;
    out[n] = 0;
    lstrcatA(out, leaf);
}

static void WriteTextFile(const char* path, const char* text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, lstrlenA(text), &w, NULL);
    CloseHandle(h);
}

/* Whole file into buf; empty string if it is not there. */
static void ReadTextFile(const char* path, char* buf, DWORD cap)
{
    buf[0] = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    ReadFile(h, buf, cap - 1, &got, NULL);
    buf[got] = 0;
    CloseHandle(h);
}

/* ------------------------------------------------------------------ failure path
 *
 * What the proxy does when it cannot allocate a wrapper. That branch decides whether the
 * game runs in exclusive fullscreen - the exact condition this DLL is installed to
 * prevent - and no ordinary run reaches it, so it is walked here deliberately, against
 * dist\oom\d3d9.dll: the same source built with BGEFIX_TEST_OOM.
 *
 * The two things being asserted are the ones a user actually experiences: the game keeps
 * running, and the loss is REPORTED. Reported specifically with Log=0, the default - a
 * warning written only when the user has already turned logging on is a warning nobody
 * receives.
 *
 * BGEFIX_NO_UI suppresses the message box (a modal dialog would hang an unattended test
 * run); it does not suppress the log line, which is the thing under test.
 */
static void TestOomIsLoud(void)
{
    char dll[MAX_PATH], ini[MAX_PATH], log[MAX_PATH], buf[8192], detail[256];

    printf("\n  -- out of memory: degradation is announced, not silent --\n");

    PathBesideExe(dll, "oom\\d3d9.dll");
    PathBesideExe(ini, "oom\\d3d9_windowed.ini");
    PathBesideExe(log, "oom\\d3d9_windowed.log");

    /* Log=0 is the default the installer writes; the report must survive it. */
    WriteTextFile(ini, "[Display]\r\nMode=1\r\n\r\n[General]\r\nLog=0\r\n");
    DeleteFileA(log);
    SetEnvironmentVariableA("BGEFIX_NO_UI", "1");

    /* Let the IDirect3D9 wrapper through and fail the DEVICE wrapper: that is the
     * interesting half, where the game is windowed right now but loses it at the next
     * Reset. */
    SetEnvironmentVariableA("BGEFIX_TEST_OOM_AFTER", "1");

    HMODULE m = LoadLibraryA(dll);
    check("forced-OOM build loads", m != NULL, dll);
    if (!m) return;

    typedef IDirect3D9* (WINAPI *PFN)(UINT);
    PFN create = (PFN)GetProcAddress(m, "Direct3DCreate9");
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : NULL;
    check("the game still gets a usable IDirect3D9", d3d != NULL, "");
    if (!d3d) { FreeLibrary(m); return; }

    HWND hwnd = CreateWindowExA(0, "BGEProxyTest", "oom test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                120, 120, 640, 480, NULL, NULL, GetModuleHandleA(NULL), NULL);

    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth  = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = hwnd;
    pp.Windowed         = FALSE;
    pp.FullScreen_RefreshRateInHz = 60;

    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    wsprintfA(detail, "hr=0x%08X", (unsigned)hr);
    check("the game still gets a working device", SUCCEEDED(hr) && dev != NULL, detail);

    /* Degraded means "loses windowed mode at the next Reset", not "does nothing now". */
    check("this session is still windowed", pp.Windowed == TRUE, "");

    ReadTextFile(log, buf, sizeof(buf));
    check("the failure is logged even with Log=0",
          strstr(buf, "[degraded]") != NULL, buf[0] ? "" : "no log written at all");
    check("the log says what the user loses",
          strstr(buf, "exclusive fullscreen") != NULL, "");

    if (dev) dev->Release();
    d3d->Release();
    if (hwnd) DestroyWindow(hwnd);
    FreeLibrary(m);

    SetEnvironmentVariableA("BGEFIX_TEST_OOM_AFTER", NULL);
    SetEnvironmentVariableA("BGEFIX_NO_UI", NULL);
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

    TestOomIsLoud();

    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
