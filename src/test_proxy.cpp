/*
 * test_proxy - functional test for d3d9_windowed.
 *
 * Loads the built d3d9.dll, asks for an EXCLUSIVE FULLSCREEN device exactly the way
 * BGE does, and asserts that the proxy rewrote the request into a windowed one and
 * restyled the window. Run it from the folder containing the built d3d9.dll.
 *
 * Build: see build_test.cmd (32-bit, no DirectX headers required).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef struct {
    UINT  BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat;
    UINT  BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT  FullScreen_RefreshRateInHz, PresentationInterval;
} D3DPP;

struct ID3D9;
typedef struct {
    void* QueryInterface; void* AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(ID3D9*);
    void* RegisterSoftwareDevice; void* GetAdapterCount; void* GetAdapterIdentifier;
    void* GetAdapterModeCount;    void* EnumAdapterModes; void* GetAdapterDisplayMode;
    void* CheckDeviceType;        void* CheckDeviceFormat; void* CheckDeviceMultiSampleType;
    void* CheckDepthStencilMatch; void* CheckDeviceFormatConversion; void* GetDeviceCaps;
    void* GetAdapterMonitor;
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(ID3D9*, UINT, DWORD, HWND, DWORD, D3DPP*, void**);
} ID3D9Vtbl;
struct ID3D9 { ID3D9Vtbl* lpVtbl; };

#define D3D_SDK_VERSION 32
#define D3DDEVTYPE_HAL  1
#define D3DFMT_X8R8G8B8 22
#define D3DSWAPEFFECT_DISCARD 1
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020

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

    typedef ID3D9* (WINAPI *PFN)(UINT);
    PFN create = (PFN)GetProcAddress(m, "Direct3DCreate9");
    check("Direct3DCreate9 exported", create != NULL, "");
    check("DebugSetMute exported", GetProcAddress(m, "DebugSetMute") != NULL, "");
    if (!create) return 2;

    ID3D9* d3d = create(D3D_SDK_VERSION);
    check("Direct3DCreate9 returns object", d3d != NULL, "");
    if (!d3d) return 2;

    D3DPP pp; ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth            = 800;
    pp.BackBufferHeight           = 600;
    pp.BackBufferFormat           = D3DFMT_X8R8G8B8;
    pp.BackBufferCount            = 1;
    pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow              = hwnd;
    pp.Windowed                   = FALSE;   /* <-- exclusive fullscreen, as BGE asks */
    pp.FullScreen_RefreshRateInHz = 60;      /* <-- illegal once windowed */

    printf("\n  request: Windowed=%d refresh=%u\n", pp.Windowed, pp.FullScreen_RefreshRateInHz);

    void* dev = NULL;
    HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    char buf[128];

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

    if (dev) { /* release device via its own vtable slot 2 */
        typedef ULONG (STDMETHODCALLTYPE *REL)(void*);
        REL rel = (REL)(*(void***)dev)[2];
        rel(dev);
    }
    d3d->lpVtbl->Release(d3d);
    DestroyWindow(hwnd);

    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
