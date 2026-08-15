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
 * Both places the field is set
 * ----------------------------
 * A D3D9 application sets Windowed in two places, and intercepting only the first is a
 * fix that quietly expires mid-session:
 *
 *   IDirect3D9::CreateDevice      - once, at startup
 *   IDirect3DDevice9::Reset       - on device-lost recovery, and on any resolution or
 *                                   display change made from the game's options
 *
 * If Reset is not intercepted, the game resets with Windowed = FALSE, goes back to
 * exclusive fullscreen, and the HUD corruption this proxy exists to prevent returns.
 * So the device returned by CreateDevice is wrapped too, and both call sites run the
 * same ForceWindowed() edit. IDirect3DDevice9::CreateAdditionalSwapChain is a third
 * entry point for present parameters and is covered for completeness.
 *
 * How the interfaces are modelled
 * -------------------------------
 * d3d9.h ships with the Windows SDK (under `shared\`, not `um\`), so the interfaces
 * come from the header and the proxies simply derive from IDirect3D9 and
 * IDirect3DDevice9. The compiler emits the vtables, which means slot order and every
 * method signature are checked at compile time: adding, reordering or mistyping a
 * method fails the build instead of calling through the wrong function pointer at
 * runtime. IDirect3DDevice9 has 119 methods, of which four are interesting; the other
 * 115 pass-through bodies were generated from the header rather than typed out.
 *
 * An earlier version of this file hand-declared the vtable to avoid the include. It
 * got IDirect3D9::GetAdapterMonitor wrong - one parameter too many - which silently
 * unbalanced the __stdcall stack on every call. Do not reintroduce hand-written
 * interface layouts here.
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
#include <d3d9.h>
#include <stdarg.h>
#include <new>

/* wvsprintf supports only c C d i s S u x X - no %p and no %f. Pointers are logged
 * through this cast, which is exact because the DLL is 32-bit by construction. */
#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise this DLL as one of ours across rebuilds. Identifying it
 * by file hash instead would make an upgraded build look third-party, and the installer
 * would chain the new proxy to the old one. Referenced from DllMain so it is not
 * optimised out of the binary. */
static const char kProxyMarker[] = "BGEFIX_PROXY_V1";

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

/* ------------------------------------------------------------------ the one edit */

/* Rewrites a D3DPRESENT_PARAMETERS in place and restyles the presentation window.
 * Returns the window that was targeted, so the caller can reassert afterwards.
 *
 * This must be applied at EVERY point a D3DPRESENT_PARAMETERS reaches the driver, not
 * just at device creation. IDirect3DDevice9::Reset takes one too, and it is what a game
 * calls on device-lost recovery and on any resolution change from an options menu. A
 * proxy that only intercepts CreateDevice goes back to exclusive fullscreen the first
 * time the game resets - which is exactly the state this DLL exists to prevent. */
static HWND ForceWindowed(D3DPRESENT_PARAMETERS* pp, HWND fallback, const char* where)
{
    HWND target = fallback;
    if (!pp) return target;

    Log("[%s] requested windowed=%d %ux%u refresh=%u", where, pp->Windowed,
        pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz);

    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        /* Must be zero for a windowed device, else the call returns D3DERR_INVALIDCALL. */
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (pp->hDeviceWindow) target = pp->hDeviceWindow;

    /* In stretch mode the backbuffer stays at the game's resolution and D3D scales it
     * to the client area on Present, so only the window changes. */
    ApplyWindowMode(target, pp->BackBufferWidth, pp->BackBufferHeight);
    return target;
}

/* ------------------------------------------------------------------ device proxy */

/* Wraps IDirect3DDevice9 for one reason: to intercept Reset. The other 118 methods are
 * pure pass-through and were generated directly from the SDK's d3d9.h rather than typed
 * out, then checked by the compiler - leaving one of them out, or getting a signature
 * wrong, leaves the class abstract and fails the build. */
class D3D9DeviceProxy final : public IDirect3DDevice9
{
public:
    D3D9DeviceProxy(IDirect3DDevice9* real, IDirect3D9* parentProxy,
                    IDirect3D9* parentReal, HWND focus)
        : real_(real), parentProxy_(parentProxy), parentReal_(parentReal), focus_(focus) {}

    /* --- IUnknown --- */

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        HRESULT hr = real_->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr) && ppv && *ppv == (void*)real_)
            *ppv = static_cast<IDirect3DDevice9*>(this);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = real_->Release();
        if (n == 0) {
            this->~D3D9DeviceProxy();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return n;
    }

    /* --- the second place Windowed gets set --- */

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override
    {
        HWND target = ForceWindowed(pp, focus_, "reset");

        HRESULT hr = real_->Reset(pp);
        Log("[reset] result=0x%08X", hr);

        /* Reset re-establishes the swap chain, and a driver may resize or restore the
         * window while doing so. Reassert, exactly as after CreateDevice. */
        if (SUCCEEDED(hr) && pp)
            ApplyWindowMode(target, pp->BackBufferWidth, pp->BackBufferHeight);

        return hr;
    }

    /* D3D9 only permits windowed additional swap chains, so this is normally a no-op -
     * but it is a third D3DPRESENT_PARAMETERS entry point, so the invariant is applied
     * here too. No restyle: a secondary swap chain is not the presentation window. */
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp,
                                                        IDirect3DSwapChain9** sc) override
    {
        if (pp && !pp->Windowed) {
            Log("[swapchain] forcing windowed on additional swap chain");
            pp->Windowed = TRUE;
            pp->FullScreen_RefreshRateInHz = 0;
        }
        return real_->CreateAdditionalSwapChain(pp, sc);
    }

    /* Hand back our factory wrapper, so a caller cannot reach the unwrapped IDirect3D9
     * through the device and create a fullscreen device behind our back. The device
     * holds a reference to its IDirect3D9, so parentProxy_ outlives this object. */
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override
    {
        HRESULT hr = real_->GetDirect3D(ppD3D9);
        if (SUCCEEDED(hr) && ppD3D9 && *ppD3D9 == parentReal_) *ppD3D9 = parentProxy_;
        return hr;
    }

    /* --- generated pass-through: see scripts note above --- */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override
    { return real_->TestCooperativeLevel(); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override
    { return real_->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override
    { return real_->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* a0) override
    { return real_->GetDeviceCaps(a0); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT a0, D3DDISPLAYMODE* a1) override
    { return real_->GetDisplayMode(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* a0) override
    { return real_->GetCreationParameters(a0); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT a0, UINT a1, IDirect3DSurface9* a2) override
    { return real_->SetCursorProperties(a0, a1, a2); }
    void STDMETHODCALLTYPE SetCursorPosition(int a0, int a1, DWORD a2) override
    { real_->SetCursorPosition(a0, a1, a2); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL a0) override
    { return real_->ShowCursor(a0); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT a0, IDirect3DSwapChain9** a1) override
    { return real_->GetSwapChain(a0, a1); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override
    { return real_->GetNumberOfSwapChains(); }
    HRESULT STDMETHODCALLTYPE Present(CONST RECT* a0, CONST RECT* a1, HWND a2,
                                      CONST RGNDATA* a3) override
    { return real_->Present(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT a0, UINT a1, D3DBACKBUFFER_TYPE a2,
                                            IDirect3DSurface9** a3) override
    { return real_->GetBackBuffer(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT a0, D3DRASTER_STATUS* a1) override
    { return real_->GetRasterStatus(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL a0) override
    { return real_->SetDialogBoxMode(a0); }
    void STDMETHODCALLTYPE SetGammaRamp(UINT a0, DWORD a1, CONST D3DGAMMARAMP* a2) override
    { real_->SetGammaRamp(a0, a1, a2); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT a0, D3DGAMMARAMP* a1) override
    { real_->GetGammaRamp(a0, a1); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT a0, UINT a1, UINT a2, DWORD a3, D3DFORMAT a4,
                                            D3DPOOL a5, IDirect3DTexture9** a6, HANDLE* a7) override
    { return real_->CreateTexture(a0, a1, a2, a3, a4, a5, a6, a7); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT a0, UINT a1, UINT a2, UINT a3, DWORD a4,
                                                  D3DFORMAT a5, D3DPOOL a6,
                                                  IDirect3DVolumeTexture9** a7, HANDLE* a8) override
    { return real_->CreateVolumeTexture(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT a0, UINT a1, DWORD a2, D3DFORMAT a3,
                                                D3DPOOL a4, IDirect3DCubeTexture9** a5,
                                                HANDLE* a6) override
    { return real_->CreateCubeTexture(a0, a1, a2, a3, a4, a5, a6); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT a0, DWORD a1, DWORD a2, D3DPOOL a3,
                                                 IDirect3DVertexBuffer9** a4, HANDLE* a5) override
    { return real_->CreateVertexBuffer(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT a0, DWORD a1, D3DFORMAT a2, D3DPOOL a3,
                                                IDirect3DIndexBuffer9** a4, HANDLE* a5) override
    { return real_->CreateIndexBuffer(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT a0, UINT a1, D3DFORMAT a2,
                                                 D3DMULTISAMPLE_TYPE a3, DWORD a4, BOOL a5,
                                                 IDirect3DSurface9** a6, HANDLE* a7) override
    { return real_->CreateRenderTarget(a0, a1, a2, a3, a4, a5, a6, a7); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT a0, UINT a1, D3DFORMAT a2,
                                                        D3DMULTISAMPLE_TYPE a3, DWORD a4,
                                                        BOOL a5, IDirect3DSurface9** a6,
                                                        HANDLE* a7) override
    { return real_->CreateDepthStencilSurface(a0, a1, a2, a3, a4, a5, a6, a7); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* a0, CONST RECT* a1,
                                            IDirect3DSurface9* a2, CONST POINT* a3) override
    { return real_->UpdateSurface(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* a0,
                                            IDirect3DBaseTexture9* a1) override
    { return real_->UpdateTexture(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* a0, IDirect3DSurface9* a1) override
    { return real_->GetRenderTargetData(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT a0, IDirect3DSurface9* a1) override
    { return real_->GetFrontBufferData(a0, a1); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* a0, CONST RECT* a1,
                                          IDirect3DSurface9* a2, CONST RECT* a3,
                                          D3DTEXTUREFILTERTYPE a4) override
    { return real_->StretchRect(a0, a1, a2, a3, a4); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* a0, CONST RECT* a1, D3DCOLOR a2) override
    { return real_->ColorFill(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT a0, UINT a1, D3DFORMAT a2,
                                                          D3DPOOL a3, IDirect3DSurface9** a4,
                                                          HANDLE* a5) override
    { return real_->CreateOffscreenPlainSurface(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD a0, IDirect3DSurface9* a1) override
    { return real_->SetRenderTarget(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD a0, IDirect3DSurface9** a1) override
    { return real_->GetRenderTarget(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* a0) override
    { return real_->SetDepthStencilSurface(a0); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** a0) override
    { return real_->GetDepthStencilSurface(a0); }
    HRESULT STDMETHODCALLTYPE BeginScene() override
    { return real_->BeginScene(); }
    HRESULT STDMETHODCALLTYPE EndScene() override
    { return real_->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD a0, CONST D3DRECT* a1, DWORD a2, D3DCOLOR a3,
                                    float a4, DWORD a5) override
    { return real_->Clear(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE a0, CONST D3DMATRIX* a1) override
    { return real_->SetTransform(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE a0, D3DMATRIX* a1) override
    { return real_->GetTransform(a0, a1); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE a0, CONST D3DMATRIX* a1) override
    { return real_->MultiplyTransform(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9* a0) override
    { return real_->SetViewport(a0); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* a0) override
    { return real_->GetViewport(a0); }
    HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9* a0) override
    { return real_->SetMaterial(a0); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* a0) override
    { return real_->GetMaterial(a0); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD a0, CONST D3DLIGHT9* a1) override
    { return real_->SetLight(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD a0, D3DLIGHT9* a1) override
    { return real_->GetLight(a0, a1); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD a0, BOOL a1) override
    { return real_->LightEnable(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD a0, BOOL* a1) override
    { return real_->GetLightEnable(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD a0, CONST float* a1) override
    { return real_->SetClipPlane(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD a0, float* a1) override
    { return real_->GetClipPlane(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE a0, DWORD a1) override
    { return real_->SetRenderState(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE a0, DWORD* a1) override
    { return real_->GetRenderState(a0, a1); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE a0, IDirect3DStateBlock9** a1) override
    { return real_->CreateStateBlock(a0, a1); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override
    { return real_->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** a0) override
    { return real_->EndStateBlock(a0); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9* a0) override
    { return real_->SetClipStatus(a0); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* a0) override
    { return real_->GetClipStatus(a0); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD a0, IDirect3DBaseTexture9** a1) override
    { return real_->GetTexture(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD a0, IDirect3DBaseTexture9* a1) override
    { return real_->SetTexture(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD a0, D3DTEXTURESTAGESTATETYPE a1,
                                                   DWORD* a2) override
    { return real_->GetTextureStageState(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD a0, D3DTEXTURESTAGESTATETYPE a1,
                                                   DWORD a2) override
    { return real_->SetTextureStageState(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD a0, D3DSAMPLERSTATETYPE a1, DWORD* a2) override
    { return real_->GetSamplerState(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD a0, D3DSAMPLERSTATETYPE a1, DWORD a2) override
    { return real_->SetSamplerState(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* a0) override
    { return real_->ValidateDevice(a0); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT a0, CONST PALETTEENTRY* a1) override
    { return real_->SetPaletteEntries(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT a0, PALETTEENTRY* a1) override
    { return real_->GetPaletteEntries(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT a0) override
    { return real_->SetCurrentTexturePalette(a0); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* a0) override
    { return real_->GetCurrentTexturePalette(a0); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT* a0) override
    { return real_->SetScissorRect(a0); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* a0) override
    { return real_->GetScissorRect(a0); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL a0) override
    { return real_->SetSoftwareVertexProcessing(a0); }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override
    { return real_->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float a0) override
    { return real_->SetNPatchMode(a0); }
    float STDMETHODCALLTYPE GetNPatchMode() override
    { return real_->GetNPatchMode(); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE a0, UINT a1, UINT a2) override
    { return real_->DrawPrimitive(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE a0, INT a1, UINT a2,
                                                   UINT a3, UINT a4, UINT a5) override
    { return real_->DrawIndexedPrimitive(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE a0, UINT a1, CONST void* a2,
                                              UINT a3) override
    { return real_->DrawPrimitiveUP(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE a0, UINT a1, UINT a2,
                                                     UINT a3, CONST void* a4, D3DFORMAT a5,
                                                     CONST void* a6, UINT a7) override
    { return real_->DrawIndexedPrimitiveUP(a0, a1, a2, a3, a4, a5, a6, a7); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT a0, UINT a1, UINT a2,
                                              IDirect3DVertexBuffer9* a3,
                                              IDirect3DVertexDeclaration9* a4, DWORD a5) override
    { return real_->ProcessVertices(a0, a1, a2, a3, a4, a5); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* a0,
                                                      IDirect3DVertexDeclaration9** a1) override
    { return real_->CreateVertexDeclaration(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* a0) override
    { return real_->SetVertexDeclaration(a0); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** a0) override
    { return real_->GetVertexDeclaration(a0); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD a0) override
    { return real_->SetFVF(a0); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* a0) override
    { return real_->GetFVF(a0); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* a0, IDirect3DVertexShader9** a1) override
    { return real_->CreateVertexShader(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* a0) override
    { return real_->SetVertexShader(a0); }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** a0) override
    { return real_->GetVertexShader(a0); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT a0, CONST float* a1, UINT a2) override
    { return real_->SetVertexShaderConstantF(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT a0, float* a1, UINT a2) override
    { return real_->GetVertexShaderConstantF(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT a0, CONST int* a1, UINT a2) override
    { return real_->SetVertexShaderConstantI(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT a0, int* a1, UINT a2) override
    { return real_->GetVertexShaderConstantI(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT a0, CONST BOOL* a1, UINT a2) override
    { return real_->SetVertexShaderConstantB(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT a0, BOOL* a1, UINT a2) override
    { return real_->GetVertexShaderConstantB(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT a0, IDirect3DVertexBuffer9* a1, UINT a2,
                                              UINT a3) override
    { return real_->SetStreamSource(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT a0, IDirect3DVertexBuffer9** a1, UINT* a2,
                                              UINT* a3) override
    { return real_->GetStreamSource(a0, a1, a2, a3); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT a0, UINT a1) override
    { return real_->SetStreamSourceFreq(a0, a1); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT a0, UINT* a1) override
    { return real_->GetStreamSourceFreq(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* a0) override
    { return real_->SetIndices(a0); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** a0) override
    { return real_->GetIndices(a0); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* a0, IDirect3DPixelShader9** a1) override
    { return real_->CreatePixelShader(a0, a1); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* a0) override
    { return real_->SetPixelShader(a0); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** a0) override
    { return real_->GetPixelShader(a0); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT a0, CONST float* a1, UINT a2) override
    { return real_->SetPixelShaderConstantF(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT a0, float* a1, UINT a2) override
    { return real_->GetPixelShaderConstantF(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT a0, CONST int* a1, UINT a2) override
    { return real_->SetPixelShaderConstantI(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT a0, int* a1, UINT a2) override
    { return real_->GetPixelShaderConstantI(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT a0, CONST BOOL* a1, UINT a2) override
    { return real_->SetPixelShaderConstantB(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT a0, BOOL* a1, UINT a2) override
    { return real_->GetPixelShaderConstantB(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT a0, CONST float* a1,
                                            CONST D3DRECTPATCH_INFO* a2) override
    { return real_->DrawRectPatch(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT a0, CONST float* a1,
                                           CONST D3DTRIPATCH_INFO* a2) override
    { return real_->DrawTriPatch(a0, a1, a2); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT a0) override
    { return real_->DeletePatch(a0); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE a0, IDirect3DQuery9** a1) override
    { return real_->CreateQuery(a0, a1); }

private:
    IDirect3DDevice9* real_;
    IDirect3D9*       parentProxy_;   /* our IDirect3D9 wrapper */
    IDirect3D9*       parentReal_;    /* the object it wraps */
    HWND              focus_;         /* focus window from CreateDevice, for Reset */
};

/* ------------------------------------------------------------------ the proxy */

/* Deriving from IDirect3D9 makes the compiler generate the vtable from the SDK
 * declaration. Every method below must match the header or the class stays abstract
 * and the build fails - which is the whole point. */
class D3D9Proxy final : public IDirect3D9
{
public:
    explicit D3D9Proxy(IDirect3D9* real) : real_(real) {}

    /* --- IUnknown --- */

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        HRESULT hr = real_->QueryInterface(riid, ppv);
        /* Hand back the wrapper rather than the object it wraps. */
        if (SUCCEEDED(hr) && ppv && *ppv == (void*)real_)
            *ppv = static_cast<IDirect3D9*>(this);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = real_->Release();
        if (n == 0) {
            this->~D3D9Proxy();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return n;
    }

    /* --- IDirect3D9 pass-through --- */

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInit) override
    { return real_->RegisterSoftwareDevice(pInit); }

    UINT STDMETHODCALLTYPE GetAdapterCount() override
    { return real_->GetAdapterCount(); }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f,
                                                   D3DADAPTER_IDENTIFIER9* id) override
    { return real_->GetAdapterIdentifier(a, f, id); }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT fmt) override
    { return real_->GetAdapterModeCount(a, fmt); }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT fmt, UINT mode,
                                               D3DDISPLAYMODE* out) override
    { return real_->EnumAdapterModes(a, fmt, mode, out); }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* out) override
    { return real_->GetAdapterDisplayMode(a, out); }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE devType,
                                              D3DFORMAT adapterFmt, D3DFORMAT bbFmt,
                                              BOOL windowed) override
    {
        /* The game asks whether its fullscreen combination is supported. Answer for the
         * windowed case we are actually going to create, or it may bail before CreateDevice. */
        UNREFERENCED_PARAMETER(windowed);
        return real_->CheckDeviceType(a, devType, adapterFmt, bbFmt, TRUE);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE devType,
                                                D3DFORMAT adapterFmt, DWORD usage,
                                                D3DRESOURCETYPE rtype,
                                                D3DFORMAT checkFmt) override
    { return real_->CheckDeviceFormat(a, devType, adapterFmt, usage, rtype, checkFmt); }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE devType,
                                                         D3DFORMAT surfFmt, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE mst,
                                                         DWORD* quality) override
    { return real_->CheckDeviceMultiSampleType(a, devType, surfFmt, windowed, mst, quality); }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE devType,
                                                     D3DFORMAT adapterFmt,
                                                     D3DFORMAT rtFmt,
                                                     D3DFORMAT dsFmt) override
    { return real_->CheckDepthStencilMatch(a, devType, adapterFmt, rtFmt, dsFmt); }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE devType,
                                                          D3DFORMAT srcFmt,
                                                          D3DFORMAT tgtFmt) override
    { return real_->CheckDeviceFormatConversion(a, devType, srcFmt, tgtFmt); }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE devType,
                                            D3DCAPS9* caps) override
    { return real_->GetDeviceCaps(a, devType, caps); }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override
    { return real_->GetAdapterMonitor(a); }

    /* The first of the two places Windowed is set. The other is Reset, which is why the
     * device that comes back here has to be wrapped rather than handed straight over. */
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE devType, HWND hFocus,
                                           DWORD behaviour, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** ppDevice) override
    {
        HWND target = ForceWindowed(pp, hFocus, "create");

        HRESULT hr = real_->CreateDevice(adapter, devType, hFocus, behaviour, pp, ppDevice);
        Log("[create] result=0x%08X", hr);
        if (FAILED(hr)) return hr;

        /* Some drivers resize the window during device creation; reassert afterwards. */
        if (pp) ApplyWindowMode(target, pp->BackBufferWidth, pp->BackBufferHeight);

        if (ppDevice && *ppDevice) {
            void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(D3D9DeviceProxy));
            if (mem) {
                D3D9DeviceProxy* d = new (mem) D3D9DeviceProxy(*ppDevice, this, real_, target);
                Log("[create] wrapped IDirect3DDevice9 0x%08X as 0x%08X", PTRV(*ppDevice), PTRV(d));
                *ppDevice = d;
            }
            else {
                /* Hand back the real device rather than failing the call: the game still
                 * runs, it just loses windowed enforcement across a Reset. */
                Log("[create] WARNING out of memory, device left unwrapped");
            }
        }
        return hr;
    }

private:
    IDirect3D9* real_;
};

/* ------------------------------------------------------------------ exports */

extern "C" {

typedef IDirect3D9* (WINAPI *PFN_Create9)(UINT);

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    LoadConfig();

    PFN_Create9 fn = (PFN_Create9)ChainProc("Direct3DCreate9");
    if (!fn) {
        Log("[init] FAILED to resolve Direct3DCreate9 from chain");
        return NULL;
    }

    IDirect3D9* real = fn(sdkVersion);
    if (!real) return NULL;

    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(D3D9Proxy));
    if (!mem) return real;   /* degrade to pass-through rather than fail the game */

    D3D9Proxy* w = new (mem) D3D9Proxy(real);
    Log("[init] wrapped IDirect3D9 0x%08X as 0x%08X (mode=%d)", PTRV(real), PTRV(w), g_mode);
    return w;
}

/* IDirect3D9Ex has an extended vtable this proxy does not model, so it is passed
 * through untouched. BGE uses Direct3DCreate9; nothing here depends on the Ex path. */
HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    typedef HRESULT (WINAPI *PFN)(UINT, IDirect3D9Ex**);
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
