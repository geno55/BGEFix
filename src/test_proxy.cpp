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

/* ------------------------------------------------------------------ the launcher's mode list
 *
 * GOG's CheckApplication.exe and SettingsApplication.exe build their display-mode list
 * from D3DFMT_R5G6B5 alone. When the driver reports no modes in that format, the settings
 * app shows empty Resolution and Refresh rate dropdowns and the launcher stops starting
 * the game, so the proxy stands the 32-bit list in for it.
 *
 * Whether this machine's driver reports 16-bit modes is not something a test can decide -
 * on the machine this was written for the answer changes between runs of the same binary -
 * so nothing here asserts a number. Everything is asserted against the SYSTEM d3d9,
 * loaded alongside the proxy, as an invariant that holds either way:
 *
 *   the 32-bit list is passed through untouched, always
 *   X1R5G5B5 is passed through untouched, always - filling that one in as well crashed
 *     SettingsApplication.exe, because a healthy driver reports zero of them too
 *   a real 16-bit list is passed through untouched, including where it ends
 *   an empty one is replaced by the 32-bit list, relabelled, same order
 *   Fill16BitModeList=0 turns all of it off
 */
static void TestLegacyModeListFill(void)
{
    char ini[MAX_PATH], sys[MAX_PATH], detail[256];

    printf("\n  -- 16-bit mode list for GOG's launcher and settings app --\n");

    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    lstrcpyA(sys + n, "\\d3d9.dll");

    HMODULE mReal  = LoadLibraryA(sys);
    HMODULE mProxy = LoadLibraryA("d3d9.dll");
    check("system d3d9 loads for comparison", mReal != NULL, sys);
    if (!mReal || !mProxy) return;

    typedef IDirect3D9* (WINAPI *PFN)(UINT);
    PFN createReal  = (PFN)GetProcAddress(mReal,  "Direct3DCreate9");
    PFN createProxy = (PFN)GetProcAddress(mProxy, "Direct3DCreate9");
    if (!createReal || !createProxy) { check("both entry points resolve", 0, ""); return; }

    IDirect3D9* real = createReal(D3D_SDK_VERSION);
    if (!real) { check("system Direct3DCreate9 returns object", 0, ""); return; }

    const UINT r32 = real->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    const UINT r16 = real->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_R5G6B5);
    const UINT r15 = real->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X1R5G5B5);
    wsprintfA(detail, "driver reports X8R8G8B8=%u R5G6B5=%u X1R5G5B5=%u", r32, r16, r15);
    printf("       %s\n", detail);

    /* Default state: no ini beside the DLL means the fill is on, which is what the
     * installer writes explicitly. */
    PathBesideExe(ini, "d3d9_windowed.ini");
    DeleteFileA(ini);
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);   /* drop Windows' ini cache */

    IDirect3D9* proxy = createProxy(D3D_SDK_VERSION);
    if (!proxy) { check("proxy Direct3DCreate9 returns object", 0, ""); real->Release(); return; }

    const UINT p32 = proxy->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    const UINT p16 = proxy->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_R5G6B5);
    const UINT p15 = proxy->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X1R5G5B5);

    wsprintfA(detail, "proxy=%u driver=%u", p32, r32);
    check("32-bit mode list is passed through untouched", p32 == r32, detail);

    wsprintfA(detail, "proxy=%u driver=%u", p15, r15);
    check("X1R5G5B5 is left alone (filling it crashes the settings app)", p15 == r15, detail);

    if (r16 == 0) {
        wsprintfA(detail, "proxy=%u driver=0, X8R8G8B8=%u", p16, r32);
        check("empty 16-bit list is stood in for by the 32-bit one", p16 == r32, detail);

        /* Same resolutions in the same order, so a mode INDEX saved by the settings app
         * still means what it meant when the driver was reporting a real 16-bit list. */
        D3DDISPLAYMODE fromProxy, fromReal;
        ZeroMemory(&fromProxy, sizeof(fromProxy));
        ZeroMemory(&fromReal, sizeof(fromReal));
        HRESULT hp = proxy->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_R5G6B5, 0, &fromProxy);
        HRESULT hr = real->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, 0, &fromReal);
        check("the stand-in enumerates", SUCCEEDED(hp) && SUCCEEDED(hr), "");
        if (SUCCEEDED(hp) && SUCCEEDED(hr)) {
            wsprintfA(detail, "%ux%u@%u vs %ux%u@%u",
                      fromProxy.Width, fromProxy.Height, fromProxy.RefreshRate,
                      fromReal.Width, fromReal.Height, fromReal.RefreshRate);
            check("stand-in modes match the 32-bit list, index for index",
                  fromProxy.Width == fromReal.Width && fromProxy.Height == fromReal.Height &&
                  fromProxy.RefreshRate == fromReal.RefreshRate, detail);
            wsprintfA(detail, "format=%d", (int)fromProxy.Format);
            check("stand-in modes are labelled 16-bit, as asked for",
                  fromProxy.Format == D3DFMT_R5G6B5, detail);
        }
    }
    else {
        wsprintfA(detail, "proxy=%u driver=%u", p16, r16);
        check("a real 16-bit list is passed through untouched", p16 == r16, detail);
    }

    /* Whichever branch produced the list, it has to END somewhere: one past the count must
     * still fail, or the caller is handed modes the driver refused. */
    D3DDISPLAYMODE past;
    ZeroMemory(&past, sizeof(past));
    check("enumerating past the end still fails",
          FAILED(proxy->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_R5G6B5, p16, &past)), "");

    proxy->Release();

    /* The off switch. Config is read in Direct3DCreate9, so a second call picks it up. */
    WriteTextFile(ini, "[Display]\r\nMode=1\r\nFill16BitModeList=0\r\n\r\n[General]\r\nLog=0\r\n");
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);

    IDirect3D9* off = createProxy(D3D_SDK_VERSION);
    if (off) {
        UINT o16 = off->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_R5G6B5);
        wsprintfA(detail, "proxy=%u driver=%u", o16, r16);
        check("Fill16BitModeList=0 passes the driver's answer through", o16 == r16, detail);
        off->Release();
    }
    else check("proxy still creates with the fill disabled", 0, "");

    DeleteFileA(ini);
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);
    real->Release();
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

    /* A second window on the same thread, activated so the presentation window starts
     * INACTIVE. Without it, CreateWindowEx has usually already activated hwnd and the
     * activation check below would pass whether or not the proxy did anything - which is
     * the bug it exists to catch. SetActiveWindow is thread-local and never refused, so
     * setting this up cannot itself depend on what else is on the desktop. */
    HWND decoy = CreateWindowExA(0, "BGEProxyTest", "decoy",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 40, 40, 200, 120, NULL, NULL, wc.hInstance, NULL);
    if (decoy) SetActiveWindow(decoy);

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
     * the controller entirely until you alt-tab away and back.
     *
     * Two separate things get checked here, because only one of them is the proxy's to
     * guarantee:
     *
     *   Activation is. SetActiveWindow/SetFocus act on the window's own thread and the OS
     *   never refuses them, so "the proxy activated the presentation window" is a hard
     *   assertion. The decoy above is what gives it teeth: activation starts elsewhere,
     *   so a proxy that went back to SWP_NOACTIVATE and dropped the activation calls
     *   fails this line rather than coasting on CreateWindowEx having activated hwnd.
     *
     *   Foreground is NOT. SetForegroundWindow is refused outright for a process that
     *   neither owns the foreground nor received the last input event - which is exactly
     *   a test run launched from a script while the user is in another window. That made
     *   this check fail intermittently on a proxy that was behaving perfectly. So the
     *   foreground half asserts only when the OS was willing to grant this process the
     *   foreground at all, and reports SKIP with the reason when it was not. */
    HWND active = GetActiveWindow();
    wsprintfA(buf, "active=0x%08X ours=0x%08X", (unsigned)(UINT_PTR)active, (unsigned)(UINT_PTR)hwnd);
    check("window is ACTIVATED after CreateDevice", active == hwnd, buf);

    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0, ourPid = GetCurrentProcessId();
    if (fg) GetWindowThreadProcessId(fg, &fgPid);

    if (fgPid == ourPid) {
        /* The OS let this process hold the foreground, so the proxy's SetForegroundWindow
         * had to land on the presentation window and not, say, the decoy. */
        wsprintfA(buf, "foreground=0x%08X ours=0x%08X",
                  (unsigned)(UINT_PTR)fg, (unsigned)(UINT_PTR)hwnd);
        check("window is FOREGROUND after CreateDevice", fg == hwnd, buf);
    }
    else {
        printf("  SKIP window is FOREGROUND after CreateDevice -> another process owns "
               "the foreground, so Windows refuses this one the change; activation, "
               "checked above, is the part the proxy controls\n");
    }

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
    if (decoy) DestroyWindow(decoy);

    TestLegacyModeListFill();
    TestOomIsLoud();

    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
