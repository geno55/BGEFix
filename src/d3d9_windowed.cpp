/*
 * d3d9_windowed - a minimal Direct3D 9 proxy that forces windowed presentation.
 *
 * Why this exists
 * ---------------
 * Beyond Good & Evil (2003) creates an exclusive-fullscreen D3D9 device. Losing that
 * device on focus change is what corrupts the HUD or crashes the game on return, and
 * it is why the GOG release ships an `IgnoreAltTab` compatibility shim to disable
 * Alt+Tab outright. Remove the shim and you need the device to stop being exclusive.
 *
 * The only thing required to achieve that is D3DPRESENT_PARAMETERS::Windowed = TRUE.
 * This DLL intercepts exactly that one field and restyles the game window. It does
 * not touch aspect ratio, resolution, FOV or shaders. Anything that does belongs in
 * a separate DLL chained behind this one (see below).
 *
 * No DirectX headers required
 * ---------------------------
 * We never call a D3D method by name, we only forward vtable slots. So instead of
 * pulling in d3d9.h (which is not part of a default Windows SDK install) we declare
 * the IDirect3D9 vtable layout and D3DPRESENT_PARAMETERS directly. Every parameter
 * on x86 is 4 bytes, so pointer-typed placeholders forward correctly under __stdcall.
 *
 * Chaining
 * --------
 * A game loads `d3d9.dll` from its own folder before System32, which is how this proxy
 * gets loaded at all. Only one file can hold that name, so to run alongside another
 * d3d9 wrapper (dgVoodoo, DXVK, ReShade): rename theirs to d3d9_chain.dll and drop this
 * one in as d3d9.dll. We load the chain target if present, else System32\d3d9.dll.
 *
 * Note on ordering: this proxy is outermost (game -> us -> chained DLL -> system), so a
 * chained DLL sees our modified parameters and could override Windowed back to FALSE.
 *
 * License: MIT.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

/* wvsprintf supports only c C d i s S u x X - no %p and no %f. Pointers are logged
 * through this cast, which is exact because the DLL is 32-bit by construction. */
#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise this DLL as one of ours across rebuilds. Identifying it
 * by file hash instead would make an upgraded build look third-party, and the installer
 * would chain the new proxy to the old one. Referenced from DllMain so it is not
 * optimised out of the binary. */
static const char kProxyMarker[] = "BGEFIX_PROXY_V1";

/* ------------------------------------------------------------------ D3D types */

/* Exact layout of D3DPRESENT_PARAMETERS: 14 fields, all 4 bytes on x86. */
typedef struct {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    DWORD BackBufferFormat;
    UINT  BackBufferCount;
    DWORD MultiSampleType;
    DWORD MultiSampleQuality;
    DWORD SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  PresentationInterval;
} D3DPP;

struct ID3D9;

/* IDirect3D9 vtable. Order is fixed by the interface and must not change.
 * Types we never inspect are declared as 4-byte placeholders. */
typedef struct {
    HRESULT  (STDMETHODCALLTYPE *QueryInterface)(ID3D9*, const GUID*, void**);
    ULONG    (STDMETHODCALLTYPE *AddRef)(ID3D9*);
    ULONG    (STDMETHODCALLTYPE *Release)(ID3D9*);
    HRESULT  (STDMETHODCALLTYPE *RegisterSoftwareDevice)(ID3D9*, void*);
    UINT     (STDMETHODCALLTYPE *GetAdapterCount)(ID3D9*);
    HRESULT  (STDMETHODCALLTYPE *GetAdapterIdentifier)(ID3D9*, UINT, DWORD, void*);
    UINT     (STDMETHODCALLTYPE *GetAdapterModeCount)(ID3D9*, UINT, DWORD);
    HRESULT  (STDMETHODCALLTYPE *EnumAdapterModes)(ID3D9*, UINT, DWORD, UINT, void*);
    HRESULT  (STDMETHODCALLTYPE *GetAdapterDisplayMode)(ID3D9*, UINT, void*);
    HRESULT  (STDMETHODCALLTYPE *CheckDeviceType)(ID3D9*, UINT, DWORD, DWORD, DWORD, BOOL);
    HRESULT  (STDMETHODCALLTYPE *CheckDeviceFormat)(ID3D9*, UINT, DWORD, DWORD, DWORD, DWORD, DWORD);
    HRESULT  (STDMETHODCALLTYPE *CheckDeviceMultiSampleType)(ID3D9*, UINT, DWORD, DWORD, BOOL, DWORD, DWORD*);
    HRESULT  (STDMETHODCALLTYPE *CheckDepthStencilMatch)(ID3D9*, UINT, DWORD, DWORD, DWORD, DWORD);
    HRESULT  (STDMETHODCALLTYPE *CheckDeviceFormatConversion)(ID3D9*, UINT, DWORD, DWORD, DWORD);
    HRESULT  (STDMETHODCALLTYPE *GetDeviceCaps)(ID3D9*, UINT, DWORD, void*);
    HMONITOR (STDMETHODCALLTYPE *GetAdapterMonitor)(ID3D9*, UINT, DWORD);
    HRESULT  (STDMETHODCALLTYPE *CreateDevice)(ID3D9*, UINT, DWORD, HWND, DWORD, D3DPP*, void**);
} ID3D9Vtbl;

struct ID3D9 { ID3D9Vtbl* lpVtbl; };

/* Our wrapper. lpVtbl must stay first so it is layout-compatible with ID3D9. */
typedef struct {
    ID3D9Vtbl* lpVtbl;
    ID3D9*     real;
} Wrapper;

/* ------------------------------------------------------------------ state */

enum { MODE_WINDOWED = 0, MODE_BORDERLESS = 1, MODE_STRETCH = 2 };

static HMODULE  g_self  = NULL;
static HMODULE  g_chain = NULL;
static CRITICAL_SECTION g_lock;
static BOOL     g_lockReady = FALSE;
static int      g_mode  = MODE_BORDERLESS;
static BOOL     g_log   = FALSE;
static wchar_t  g_iniPath[MAX_PATH]  = {0};
static wchar_t  g_logPath[MAX_PATH]  = {0};

/* ------------------------------------------------------------------ logging */

static void Log(const char* fmt, ...)
{
    if (!g_log || !g_logPath[0]) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, buf, (DWORD)n, &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ config */

static void BuildPaths(void)
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    /* strip to directory */
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; break; }
    }
    lstrcpynW(g_iniPath, path, MAX_PATH);
    lstrcatW(g_iniPath, L"d3d9_windowed.ini");
    lstrcpynW(g_logPath, path, MAX_PATH);
    lstrcatW(g_logPath, L"d3d9_windowed.log");
}

static void LoadConfig(void)
{
    if (!g_iniPath[0]) return;
    g_mode = (int)GetPrivateProfileIntW(L"Display", L"Mode", MODE_BORDERLESS, g_iniPath);
    if (g_mode < MODE_WINDOWED || g_mode > MODE_STRETCH) g_mode = MODE_BORDERLESS;
    g_log  = GetPrivateProfileIntW(L"Display", L"Log", 0, g_iniPath) != 0;
}

/* ------------------------------------------------------------------ chain loading */

/* Never call LoadLibrary from DllMain - this runs lazily on first export use. */
static HMODULE GetChain(void)
{
    if (g_chain) return g_chain;
    if (!g_lockReady) return NULL;

    EnterCriticalSection(&g_lock);
    if (!g_chain) {
        wchar_t self[MAX_PATH], dir[MAX_PATH], cand[MAX_PATH], name[64];

        DWORD len = GetModuleFileNameW(g_self, self, MAX_PATH);
        lstrcpynW(dir, self, MAX_PATH);
        for (DWORD i = len; i > 0; --i) {
            if (dir[i - 1] == L'\\' || dir[i - 1] == L'/') { dir[i] = 0; break; }
        }

        GetPrivateProfileStringW(L"Display", L"Chain", L"d3d9_chain.dll",
                                 name, 64, g_iniPath);

        /* 1. chain target beside us (a renamed DXVK, dgVoodoo, ReShade, ...) */
        lstrcpynW(cand, dir, MAX_PATH);
        lstrcatW(cand, name);
        if (lstrcmpiW(cand, self) != 0 &&
            GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            g_chain = LoadLibraryW(cand);
            Log("[chain] loaded %S -> 0x%08X", cand, PTRV(g_chain));
        }

        /* 2. the real system implementation */
        if (!g_chain) {
            UINT n = GetSystemDirectoryW(cand, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                lstrcatW(cand, L"\\d3d9.dll");
                if (lstrcmpiW(cand, self) != 0) {
                    g_chain = LoadLibraryW(cand);
                    Log("[chain] loaded %S -> 0x%08X", cand, PTRV(g_chain));
                }
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return g_chain;
}

static FARPROC ChainProc(const char* name)
{
    HMODULE m = GetChain();
    return m ? GetProcAddress(m, name) : NULL;
}

/* ------------------------------------------------------------------ window restyle */

static void ApplyWindowMode(HWND hwnd, UINT bbW, UINT bbH)
{
    if (!hwnd || !IsWindow(hwnd)) return;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcMonitor.left = mi.rcMonitor.top = 0;
        mi.rcMonitor.right  = GetSystemMetrics(SM_CXSCREEN);
        mi.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    LONG monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    LONG monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                 WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    LONG style;
    RECT want;

    if (g_mode == MODE_WINDOWED) {
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        want.left = 0; want.top = 0;
        want.right = (LONG)bbW; want.bottom = (LONG)bbH;
        AdjustWindowRectEx(&want, style & ~WS_OVERLAPPED, FALSE, exStyle);
    }
    else {
        style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        if (g_mode == MODE_STRETCH) {
            want.left = 0; want.top = 0; want.right = monW; want.bottom = monH;
        } else {
            want.left = 0; want.top = 0;
            want.right = (LONG)bbW; want.bottom = (LONG)bbH;
        }
    }

    LONG w = want.right - want.left;
    LONG h = want.bottom - want.top;
    if (w <= 0 || h <= 0) return;

    /* never present a window larger than the monitor in centred modes */
    if (g_mode != MODE_STRETCH) {
        if (w > monW) w = monW;
        if (h > monH) h = monH;
    }

    LONG x = mi.rcMonitor.left + (monW - w) / 2;
    LONG y = mi.rcMonitor.top  + (monH - h) / 2;

    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    Log("[window] hwnd=0x%08X mode=%d pos=%d,%d size=%dx%d (backbuffer %ux%u)",
        PTRV(hwnd), g_mode, x, y, w, h, bbW, bbH);
}

/* ------------------------------------------------------------------ vtable thunks */

#define REAL(p) (((Wrapper*)(p))->real)
#define VT(p)   (REAL(p)->lpVtbl)

static HRESULT STDMETHODCALLTYPE W_QueryInterface(ID3D9* self, const GUID* riid, void** ppv)
{
    HRESULT hr = VT(self)->QueryInterface(REAL(self), riid, ppv);
    /* Hand back the wrapper rather than the object it wraps. */
    if (SUCCEEDED(hr) && ppv && *ppv == (void*)REAL(self)) *ppv = self;
    return hr;
}

static ULONG STDMETHODCALLTYPE W_AddRef(ID3D9* self)
{
    return VT(self)->AddRef(REAL(self));
}

static ULONG STDMETHODCALLTYPE W_Release(ID3D9* self)
{
    ID3D9* real = REAL(self);
    ULONG n = real->lpVtbl->Release(real);
    if (n == 0) HeapFree(GetProcessHeap(), 0, self);
    return n;
}

static HRESULT STDMETHODCALLTYPE W_RegisterSoftwareDevice(ID3D9* s, void* a)
{ return VT(s)->RegisterSoftwareDevice(REAL(s), a); }

static UINT STDMETHODCALLTYPE W_GetAdapterCount(ID3D9* s)
{ return VT(s)->GetAdapterCount(REAL(s)); }

static HRESULT STDMETHODCALLTYPE W_GetAdapterIdentifier(ID3D9* s, UINT a, DWORD f, void* p)
{ return VT(s)->GetAdapterIdentifier(REAL(s), a, f, p); }

static UINT STDMETHODCALLTYPE W_GetAdapterModeCount(ID3D9* s, UINT a, DWORD f)
{ return VT(s)->GetAdapterModeCount(REAL(s), a, f); }

static HRESULT STDMETHODCALLTYPE W_EnumAdapterModes(ID3D9* s, UINT a, DWORD f, UINT m, void* p)
{ return VT(s)->EnumAdapterModes(REAL(s), a, f, m, p); }

static HRESULT STDMETHODCALLTYPE W_GetAdapterDisplayMode(ID3D9* s, UINT a, void* p)
{ return VT(s)->GetAdapterDisplayMode(REAL(s), a, p); }

static HRESULT STDMETHODCALLTYPE W_CheckDeviceType(ID3D9* s, UINT a, DWORD d, DWORD af, DWORD bf, BOOL w)
{
    /* The game asks whether its fullscreen combination is supported. Answer for the
     * windowed case we are actually going to create, or it may bail before CreateDevice. */
    UNREFERENCED_PARAMETER(w);
    return VT(s)->CheckDeviceType(REAL(s), a, d, af, bf, TRUE);
}

static HRESULT STDMETHODCALLTYPE W_CheckDeviceFormat(ID3D9* s, UINT a, DWORD d, DWORD af, DWORD u, DWORD rt, DWORD cf)
{ return VT(s)->CheckDeviceFormat(REAL(s), a, d, af, u, rt, cf); }

static HRESULT STDMETHODCALLTYPE W_CheckDeviceMultiSampleType(ID3D9* s, UINT a, DWORD d, DWORD sf, BOOL w, DWORD mst, DWORD* q)
{ return VT(s)->CheckDeviceMultiSampleType(REAL(s), a, d, sf, w, mst, q); }

static HRESULT STDMETHODCALLTYPE W_CheckDepthStencilMatch(ID3D9* s, UINT a, DWORD d, DWORD af, DWORD rf, DWORD df)
{ return VT(s)->CheckDepthStencilMatch(REAL(s), a, d, af, rf, df); }

static HRESULT STDMETHODCALLTYPE W_CheckDeviceFormatConversion(ID3D9* s, UINT a, DWORD d, DWORD sf, DWORD tf)
{ return VT(s)->CheckDeviceFormatConversion(REAL(s), a, d, sf, tf); }

static HRESULT STDMETHODCALLTYPE W_GetDeviceCaps(ID3D9* s, UINT a, DWORD d, void* c)
{ return VT(s)->GetDeviceCaps(REAL(s), a, d, c); }

static HMONITOR STDMETHODCALLTYPE W_GetAdapterMonitor(ID3D9* s, UINT a, DWORD d)
{ return VT(s)->GetAdapterMonitor(REAL(s), a, d); }

/* The entire point of this DLL. */
static HRESULT STDMETHODCALLTYPE W_CreateDevice(ID3D9* s, UINT adapter, DWORD devType,
                                                HWND hFocus, DWORD behaviour,
                                                D3DPP* pp, void** ppDevice)
{
    HWND target = hFocus;

    if (pp) {
        Log("[create] requested windowed=%d %ux%u refresh=%u",
            pp->Windowed, pp->BackBufferWidth, pp->BackBufferHeight,
            pp->FullScreen_RefreshRateInHz);

        if (!pp->Windowed) {
            pp->Windowed = TRUE;
            /* Must be zero for a windowed device, else CreateDevice returns D3DERR_INVALIDCALL. */
            pp->FullScreen_RefreshRateInHz = 0;
        }
        if (pp->hDeviceWindow) target = pp->hDeviceWindow;

        /* In stretch mode the backbuffer stays at the game's resolution and D3D
         * scales it to the client area on Present, so only the window changes. */
        ApplyWindowMode(target, pp->BackBufferWidth, pp->BackBufferHeight);
    }

    HRESULT hr = VT(s)->CreateDevice(REAL(s), adapter, devType, hFocus, behaviour, pp, ppDevice);
    Log("[create] result=0x%08X", hr);

    /* Some drivers resize the window during device creation; reassert afterwards. */
    if (SUCCEEDED(hr) && pp)
        ApplyWindowMode(target, pp->BackBufferWidth, pp->BackBufferHeight);

    return hr;
}

static ID3D9Vtbl g_vtbl = {
    W_QueryInterface, W_AddRef, W_Release,
    W_RegisterSoftwareDevice, W_GetAdapterCount, W_GetAdapterIdentifier,
    W_GetAdapterModeCount, W_EnumAdapterModes, W_GetAdapterDisplayMode,
    W_CheckDeviceType, W_CheckDeviceFormat, W_CheckDeviceMultiSampleType,
    W_CheckDepthStencilMatch, W_CheckDeviceFormatConversion, W_GetDeviceCaps,
    W_GetAdapterMonitor, W_CreateDevice
};

/* ------------------------------------------------------------------ exports */

extern "C" {

typedef ID3D9* (WINAPI *PFN_Create9)(UINT);

ID3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    LoadConfig();

    PFN_Create9 fn = (PFN_Create9)ChainProc("Direct3DCreate9");
    if (!fn) {
        Log("[init] FAILED to resolve Direct3DCreate9 from chain");
        return NULL;
    }

    ID3D9* real = fn(sdkVersion);
    if (!real) return NULL;

    Wrapper* w = (Wrapper*)HeapAlloc(GetProcessHeap(), 0, sizeof(Wrapper));
    if (!w) return real;   /* degrade to pass-through rather than fail the game */

    w->lpVtbl = &g_vtbl;
    w->real   = real;
    Log("[init] wrapped IDirect3D9 0x%08X as 0x%08X (mode=%d)", PTRV(real), PTRV(w), g_mode);
    return (ID3D9*)w;
}

/* IDirect3D9Ex has an extended vtable this proxy does not model, so it is passed
 * through untouched. BGE uses Direct3DCreate9; nothing here depends on the Ex path. */
HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, void** out)
{
    typedef HRESULT (WINAPI *PFN)(UINT, void**);
    PFN fn = (PFN)ChainProc("Direct3DCreate9Ex");
    if (!fn) return E_NOTIMPL;
    return fn(sdkVersion, out);
}

void WINAPI DebugSetMute(void)
{
    typedef void (WINAPI *PFN)(void);
    PFN fn = (PFN)ChainProc("DebugSetMute");
    if (fn) fn();
}

int WINAPI D3DPERF_BeginEvent(DWORD col, LPCWSTR name)
{
    typedef int (WINAPI *PFN)(DWORD, LPCWSTR);
    PFN fn = (PFN)ChainProc("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}

int WINAPI D3DPERF_EndEvent(void)
{
    typedef int (WINAPI *PFN)(void);
    PFN fn = (PFN)ChainProc("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetMarker(DWORD col, LPCWSTR name)
{
    typedef void (WINAPI *PFN)(DWORD, LPCWSTR);
    PFN fn = (PFN)ChainProc("D3DPERF_SetMarker");
    if (fn) fn(col, name);
}

void WINAPI D3DPERF_SetRegion(DWORD col, LPCWSTR name)
{
    typedef void (WINAPI *PFN)(DWORD, LPCWSTR);
    PFN fn = (PFN)ChainProc("D3DPERF_SetRegion");
    if (fn) fn(col, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    typedef BOOL (WINAPI *PFN)(void);
    PFN fn = (PFN)ChainProc("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD opts)
{
    typedef void (WINAPI *PFN)(DWORD);
    PFN fn = (PFN)ChainProc("D3DPERF_SetOptions");
    if (fn) fn(opts);
}

DWORD WINAPI D3DPERF_GetStatus(void)
{
    typedef DWORD (WINAPI *PFN)(void);
    PFN fn = (PFN)ChainProc("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

} /* extern "C" */

/* ------------------------------------------------------------------ entry point */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (kProxyMarker[0] == 0) return FALSE;   /* keeps the marker in the binary */
        g_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        g_lockReady = TRUE;
        BuildPaths();
        LoadConfig();
        /* No LoadLibrary here - that would deadlock the loader. See GetChain(). */
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        if (g_lockReady) { DeleteCriticalSection(&g_lock); g_lockReady = FALSE; }
    }
    return TRUE;
}
