/*
 * dinput8_xinput - adds XInput (Xbox) controller support to Beyond Good & Evil.
 *
 * Why this exists
 * ---------------
 * The GOG/retail build of BGE.exe has no gamepad code at all. It imports exactly one
 * symbol from dinput8.dll (DirectInput8Create) and contains no joystick vocabulary.
 * Controller support was only ever added in the later Steam/Uplay builds.
 *
 * What it does still have is a DirectInput keyboard and mouse, and its key bindings are
 * stored in the registry as DirectInput scan codes:
 *
 *     Up/Down/Left/Right = DIK_W / DIK_S / DIK_A / DIK_D   (0x11 / 0x1F / 0x1E / 0x20)
 *     Run/Accelerate     = DIK_SPACE (0x39)
 *     Crouch             = DIK_LCONTROL (0x1D)
 *     Primary/Secondary  = mouse buttons 0 and 1
 *
 * So the game reads a 256-byte DirectInput keyboard state array indexed by scan code.
 * That makes the reliable approach a dinput8 proxy that writes controller state directly
 * into the buffer the game reads - rather than synthesising OS-level input with SendInput
 * and hoping an exclusive-mode DirectInput device picks it up.
 *
 * We wrap IDirectInput8::CreateDevice, and for the system keyboard and mouse we return a
 * device wrapper whose GetDeviceState (immediate mode) and GetDeviceData (buffered mode)
 * merge in state derived from XInput. Everything else forwards untouched.
 *
 * How the interfaces are modelled
 * -------------------------------
 * dinput.h and xinput.h both ship with the Windows SDK, so the interfaces, the device
 * GUIDs, the DIMOUSESTATE offsets and the XInput button bits all come from the headers.
 * The wrappers derive from IDirectInput8A / IDirectInputDevice8A, which means the
 * compiler emits the vtables: a wrong slot order or a mistyped parameter is a build
 * failure, not a call through the wrong function pointer. See the note in
 * d3d9_windowed.cpp about the bug hand-written vtables actually caused here.
 *
 * ANSI vs Unicode: DirectInput8Create hands back IDirectInput8A or IDirectInput8W
 * depending on the IID the caller asks for. The two have identical vtable layouts and
 * differ only in the string types behind pointer parameters we forward untouched, so
 * one wrapper serves both. BGE requests the ANSI interface.
 *
 * Chaining
 * --------
 * As with the d3d9 proxy, only one file can be called dinput8.dll. If another wrapper is
 * already present, rename it to dinput8_chain.dll and we forward to it; otherwise we load
 * the system copy. A 32-bit process gets SysWOW64 via file system redirection.
 *
 * License: MIT.
 */

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <stdarg.h>
#include <new>

#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise this DLL as one of ours across rebuilds. See the note in
 * d3d9_windowed.cpp. Referenced from DllMain so it is not optimised out. */
static const char kProxyMarker[] = "BGEFIX_PROXY_V1";

/* A mapping target: 0 = unmapped, MOUSE_BASE+n = mouse button n, else a DIK scan code. */
#define MOUSE_BASE 0x1000

/* Mouse button n lives at this byte offset in DIMOUSESTATE/DIMOUSESTATE2. */
#define MOUSE_BTN_OFS(n) (FIELD_OFFSET(DIMOUSESTATE2, rgbButtons) + (n))

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);

/* ------------------------------------------------------------------ state */

static HMODULE g_self = NULL, g_chain = NULL, g_xinput = NULL;
static PFN_XInputGetState g_XInputGetState = NULL;
static CRITICAL_SECTION g_lock;
static BOOL g_lockReady = FALSE;
static BOOL g_log = FALSE;
static wchar_t g_ini[MAX_PATH] = {0}, g_logPath[MAX_PATH] = {0};

/* mapping */
static int  g_mA, g_mB, g_mX, g_mY, g_mLB, g_mRB, g_mLT, g_mRT;
static int  g_mStart, g_mBack, g_mLS, g_mRS;
static int  g_mDU, g_mDD, g_mDL, g_mDR;
static int  g_sUp, g_sDown, g_sLeft, g_sRight;
static int  g_deadzone = 8000, g_lookDeadzone = 8000, g_lookSens = 30, g_invertLook = 0;
static int  g_triggerThreshold = 60;

/* injected state, rebuilt each poll */
static BYTE g_keys[256];
static BYTE g_prevKeys[256];
static BYTE g_mouseBtn[8];
static BYTE g_prevMouseBtn[8];
static LONG g_lookX = 0, g_lookY = 0;
static DWORD g_seq = 1;

/* ------------------------------------------------------------------ logging */

static void Log(const char* fmt, ...)
{
    if (!g_log || !g_logPath[0]) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ config */

static int ParseTarget(const wchar_t* s, int fallback)
{
    if (!s || !s[0]) return fallback;
    if (s[0] == L'M' || s[0] == L'm') {           /* MOUSE1 / MOUSE2 / ... */
        const wchar_t* p = s;
        while (*p && (*p < L'0' || *p > L'9')) p++;
        if (!*p) return fallback;
        int n = 0;
        while (*p >= L'0' && *p <= L'9') { n = n * 10 + (*p - L'0'); p++; }
        if (n < 1) n = 1;
        return MOUSE_BASE + (n - 1);
    }
    int base = 10, i = 0, v = 0;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) { base = 16; i = 2; }
    for (; s[i]; ++i) {
        int d;
        if      (s[i] >= L'0' && s[i] <= L'9') d = s[i] - L'0';
        else if (s[i] >= L'a' && s[i] <= L'f') d = s[i] - L'a' + 10;
        else if (s[i] >= L'A' && s[i] <= L'F') d = s[i] - L'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    return v;
}

static int ReadMap(const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Buttons", key, def, buf, 64, g_ini);
    return ParseTarget(buf, 0);
}

static int ReadStick(const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Sticks", key, def, buf, 64, g_ini);
    return ParseTarget(buf, 0);
}

static void BuildPaths(void)
{
    wchar_t p[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_self, p, MAX_PATH);
    if (!len || len >= MAX_PATH) return;
    for (DWORD i = len; i > 0; --i)
        if (p[i-1] == L'\\' || p[i-1] == L'/') { p[i] = 0; break; }
    lstrcpynW(g_ini, p, MAX_PATH);     lstrcatW(g_ini, L"dinput8_xinput.ini");
    lstrcpynW(g_logPath, p, MAX_PATH); lstrcatW(g_logPath, L"dinput8_xinput.log");
}

static void LoadConfig(void)
{
    if (!g_ini[0]) return;

    /* Defaults follow the game's own bindings, decoded from
     * HKCU\Software\Ubisoft\Beyond Good & Evil\...\Key bindings. */
    g_mA     = ReadMap(L"A",         L"MOUSE1"); /* primary action   */
    g_mB     = ReadMap(L"B",         L"MOUSE2"); /* secondary action */
    g_mX     = ReadMap(L"X",         L"0x10");   /* Q - use object   */
    g_mY     = ReadMap(L"Y",         L"0x12");   /* E - buddy        */
    g_mLB    = ReadMap(L"LB",        L"0x2A");   /* LShift - look    */
    g_mRB    = ReadMap(L"RB",        L"0x2E");   /* C - center view  */
    g_mLT    = ReadMap(L"LT",        L"0x1D");   /* LCtrl - crouch   */
    g_mRT    = ReadMap(L"RT",        L"0x39");   /* Space - run      */
    g_mStart = ReadMap(L"Start",     L"0x01");   /* Esc - menu       */
    g_mBack  = ReadMap(L"Back",      L"0x0F");   /* Tab - map        */
    g_mLS    = ReadMap(L"LS",        L"0");
    g_mRS    = ReadMap(L"RS",        L"0x2E");   /* C - center view  */
    g_mDU    = ReadMap(L"DPadUp",    L"0");
    g_mDD    = ReadMap(L"DPadDown",  L"0");
    g_mDL    = ReadMap(L"DPadLeft",  L"0x03");   /* 2 - inventory prev */
    g_mDR    = ReadMap(L"DPadRight", L"0x04");   /* 3 - inventory next */

    g_sUp    = ReadStick(L"LeftUp",    L"0x11"); /* W */
    g_sDown  = ReadStick(L"LeftDown",  L"0x1F"); /* S */
    g_sLeft  = ReadStick(L"LeftLeft",  L"0x1E"); /* A */
    g_sRight = ReadStick(L"LeftRight", L"0x20"); /* D */

    g_deadzone         = (int)GetPrivateProfileIntW(L"Sticks", L"Deadzone",        8000, g_ini);
    g_lookDeadzone     = (int)GetPrivateProfileIntW(L"Sticks", L"LookDeadzone",    8000, g_ini);
    g_lookSens         = (int)GetPrivateProfileIntW(L"Sticks", L"LookSensitivity",   30, g_ini);
    g_invertLook       = (int)GetPrivateProfileIntW(L"Sticks", L"InvertLook",         0, g_ini);
    g_triggerThreshold = (int)GetPrivateProfileIntW(L"Buttons", L"TriggerThreshold",  60, g_ini);
    g_log              = GetPrivateProfileIntW(L"General", L"Log", 0, g_ini) != 0;
}

/* ------------------------------------------------------------------ chain */

static HMODULE GetChain(void)
{
    if (g_chain) return g_chain;
    if (!g_lockReady) return NULL;

    EnterCriticalSection(&g_lock);
    if (!g_chain) {
        wchar_t self[MAX_PATH], dir[MAX_PATH], cand[MAX_PATH], name[64];
        DWORD len = GetModuleFileNameW(g_self, self, MAX_PATH);
        lstrcpynW(dir, self, MAX_PATH);
        for (DWORD i = len; i > 0; --i)
            if (dir[i-1] == L'\\' || dir[i-1] == L'/') { dir[i] = 0; break; }

        GetPrivateProfileStringW(L"General", L"Chain", L"dinput8_chain.dll", name, 64, g_ini);
        lstrcpynW(cand, dir, MAX_PATH); lstrcatW(cand, name);
        if (lstrcmpiW(cand, self) != 0 && GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            g_chain = LoadLibraryW(cand);
            Log("[chain] %S -> 0x%08X", cand, PTRV(g_chain));
        }
        if (!g_chain) {
            /* A 32-bit process is redirected to SysWOW64, which is what we want. */
            UINT n = GetSystemDirectoryW(cand, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                lstrcatW(cand, L"\\dinput8.dll");
                if (lstrcmpiW(cand, self) != 0) {
                    g_chain = LoadLibraryW(cand);
                    Log("[chain] %S -> 0x%08X", cand, PTRV(g_chain));
                }
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return g_chain;
}

static void InitXInput(void)
{
    if (g_XInputGetState) return;
    const wchar_t* names[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3; ++i) {
        g_xinput = LoadLibraryW(names[i]);
        if (g_xinput) {
            g_XInputGetState = (PFN_XInputGetState)GetProcAddress(g_xinput, "XInputGetState");
            if (g_XInputGetState) { Log("[xinput] using %S", names[i]); return; }
            FreeLibrary(g_xinput); g_xinput = NULL;
        }
    }
    Log("[xinput] no XInput runtime found");
}

/* ------------------------------------------------------------------ polling */

static void Press(int target)
{
    if (target <= 0) return;
    if (target >= MOUSE_BASE) {
        int b = target - MOUSE_BASE;
        if (b >= 0 && b < 8) g_mouseBtn[b] = 0x80;
    }
    else if (target < 256) {
        g_keys[target] = 0x80;
    }
}

/* Rebuilds the injected key/mouse state from the first connected pad. */
static void PollPad(void)
{
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_mouseBtn, 0, sizeof(g_mouseBtn));
    g_lookX = g_lookY = 0;

    InitXInput();
    if (!g_XInputGetState) return;

    XINPUT_STATE st;
    DWORD ok = ERROR_DEVICE_NOT_CONNECTED;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        memset(&st, 0, sizeof(st));
        if (g_XInputGetState(i, &st) == ERROR_SUCCESS) { ok = ERROR_SUCCESS; break; }
    }
    if (ok != ERROR_SUCCESS) return;

    WORD b = st.Gamepad.wButtons;
    if (b & XINPUT_GAMEPAD_A)              Press(g_mA);
    if (b & XINPUT_GAMEPAD_B)              Press(g_mB);
    if (b & XINPUT_GAMEPAD_X)              Press(g_mX);
    if (b & XINPUT_GAMEPAD_Y)              Press(g_mY);
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  Press(g_mLB);
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) Press(g_mRB);
    if (b & XINPUT_GAMEPAD_START)          Press(g_mStart);
    if (b & XINPUT_GAMEPAD_BACK)           Press(g_mBack);
    if (b & XINPUT_GAMEPAD_LEFT_THUMB)     Press(g_mLS);
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    Press(g_mRS);
    if (b & XINPUT_GAMEPAD_DPAD_UP)        Press(g_mDU);
    if (b & XINPUT_GAMEPAD_DPAD_DOWN)      Press(g_mDD);
    if (b & XINPUT_GAMEPAD_DPAD_LEFT)      Press(g_mDL);
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     Press(g_mDR);

    if (st.Gamepad.bLeftTrigger  > g_triggerThreshold) Press(g_mLT);
    if (st.Gamepad.bRightTrigger > g_triggerThreshold) Press(g_mRT);

    /* Left stick -> digital movement keys. The game has no analog movement path. */
    int lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
    if (lx >  g_deadzone) Press(g_sRight);
    if (lx < -g_deadzone) Press(g_sLeft);
    if (ly >  g_deadzone) Press(g_sUp);
    if (ly < -g_deadzone) Press(g_sDown);

    /* D-pad doubles as movement when not otherwise mapped. */
    if ((b & XINPUT_GAMEPAD_DPAD_UP)   && g_mDU == 0) Press(g_sUp);
    if ((b & XINPUT_GAMEPAD_DPAD_DOWN) && g_mDD == 0) Press(g_sDown);

    /* Right stick -> relative mouse motion, consumed by the mouse device wrapper. */
    int rx = st.Gamepad.sThumbRX, ry = st.Gamepad.sThumbRY;
    if (rx > g_lookDeadzone || rx < -g_lookDeadzone)
        g_lookX = (LONG)((__int64)rx * g_lookSens / 32767);
    if (ry > g_lookDeadzone || ry < -g_lookDeadzone) {
        LONG dy = (LONG)((__int64)ry * g_lookSens / 32767);
        g_lookY = g_invertLook ? dy : -dy;   /* stick up should look up */
    }
}

/* ------------------------------------------------------------------ device wrapper */

class DIDeviceProxy final : public IDirectInputDevice8A
{
public:
    DIDeviceProxy(IDirectInputDevice8A* real, bool isKeyboard)
        : real_(real), isKeyboard_(isKeyboard) {}

    /* --- IUnknown --- */

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        HRESULT hr = real_->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr) && ppv && *ppv == (void*)real_)
            *ppv = static_cast<IDirectInputDevice8A*>(this);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = real_->Release();
        if (n == 0) {
            this->~DIDeviceProxy();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return n;
    }

    /* --- the two methods that inject controller state --- */

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override
    {
        HRESULT hr = real_->GetDeviceState(cb, data);
        if (FAILED(hr) || !data) return hr;

        PollPad();

        if (isKeyboard_) {
            if (cb >= 256) {
                BYTE* k = (BYTE*)data;
                for (int i = 0; i < 256; ++i) if (g_keys[i]) k[i] = 0x80;
            }
        }
        else {
            /* DIMOUSESTATE: LONG lX, lY, lZ followed by rgbButtons[4]; DIMOUSESTATE2
             * is identical with rgbButtons[8]. Accept either. */
            const DWORD btnOfs = (DWORD)FIELD_OFFSET(DIMOUSESTATE2, rgbButtons);
            if (cb >= btnOfs) {
                LONG* ax = (LONG*)data;
                ax[0] += g_lookX;
                ax[1] += g_lookY;
                DWORD nb = cb - btnOfs;
                if (nb > sizeof(g_mouseBtn)) nb = sizeof(g_mouseBtn);
                BYTE* btn = (BYTE*)data + btnOfs;
                for (DWORD i = 0; i < nb; ++i) if (g_mouseBtn[i]) btn[i] = 0x80;
            }
        }
        return hr;
    }

    /* Buffered mode: synthesise press/release events for anything that changed since the
     * last poll, and append them after whatever the real device returned. */
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod,
                                            LPDWORD pdwInOut, DWORD flags) override
    {
        HRESULT hr = real_->GetDeviceData(cbObj, rgdod, pdwInOut, flags);
        if (FAILED(hr) || !pdwInOut) return hr;

        /* Peek must not consume our synthetic events either. */
        const BOOL peek = (flags & DIGDD_PEEK) != 0;
        if (!rgdod) return hr;                      /* caller only wants a count */

        /* A DX3-era caller passes the 16-byte struct; DX8 adds uAppData. */
        if (cbObj < sizeof(DIDEVICEOBJECTDATA_DX3)) return hr;

        PollPad();

        DWORD count = *pdwInOut;
        DWORD cap   = count;                        /* on entry: buffer capacity */
        DWORD used  = count;                        /* real device already filled this many */
        if (used > cap) return hr;

        BYTE* base = (BYTE*)rgdod;
        BYTE  curB[8];
        BYTE* cur;
        BYTE* prev;
        int   n;

        if (isKeyboard_) { cur = g_keys; prev = g_prevKeys; n = 256; }
        else {
            memcpy(curB, g_mouseBtn, sizeof(curB));
            cur = curB; prev = g_prevMouseBtn; n = (int)sizeof(curB);
        }

        for (int i = 0; i < n && used < cap; ++i) {
            if (cur[i] == prev[i]) continue;
            DIDEVICEOBJECTDATA_DX3* o =
                (DIDEVICEOBJECTDATA_DX3*)(base + (size_t)used * cbObj);
            o->dwOfs       = isKeyboard_ ? (DWORD)i : (DWORD)MOUSE_BTN_OFS(i);
            o->dwData      = cur[i] ? 0x80 : 0x00;
            o->dwTimeStamp = GetTickCount();
            o->dwSequence  = g_seq++;
            if (cbObj >= sizeof(DIDEVICEOBJECTDATA))
                ((DIDEVICEOBJECTDATA*)o)->uAppData = 0;
            used++;
        }

        if (!peek) memcpy(prev, cur, (size_t)n);
        *pdwInOut = used;
        return hr;
    }

    /* --- everything else forwards untouched --- */

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS a) override
    { return real_->GetCapabilities(a); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) override
    { return real_->EnumObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID a, LPDIPROPHEADER b) override
    { return real_->GetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID a, LPCDIPROPHEADER b) override
    { return real_->SetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE Acquire() override
    { return real_->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override
    { return real_->Unacquire(); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT a) override
    { return real_->SetDataFormat(a); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE a) override
    { return real_->SetEventNotification(a); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a, DWORD b) override
    { return real_->SetCooperativeLevel(a, b); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) override
    { return real_->GetObjectInfo(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA a) override
    { return real_->GetDeviceInfo(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override
    { return real_->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override
    { return real_->Initialize(a, b, c); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID a, LPCDIEFFECT b,
                                           LPDIRECTINPUTEFFECT* c, LPUNKNOWN d) override
    { return real_->CreateEffect(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) override
    { return real_->EnumEffects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA a, REFGUID b) override
    { return real_->GetEffectInfo(a, b); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD a) override
    { return real_->GetForceFeedbackState(a); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD a) override
    { return real_->SendForceFeedbackCommand(a); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a,
                                                       LPVOID b, DWORD c) override
    { return real_->EnumCreatedEffectObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override
    { return real_->Escape(a); }
    HRESULT STDMETHODCALLTYPE Poll() override
    { return real_->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b,
                                             LPDWORD c, DWORD d) override
    { return real_->SendDeviceData(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b,
                                                LPVOID c, DWORD d) override
    { return real_->EnumEffectsInFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override
    { return real_->WriteEffectToFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override
    { return real_->BuildActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override
    { return real_->SetActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA a) override
    { return real_->GetImageInfo(a); }

private:
    IDirectInputDevice8A* real_;
    bool                  isKeyboard_;
};

/* ------------------------------------------------------------------ factory wrapper */

class DI8Proxy final : public IDirectInput8A
{
public:
    explicit DI8Proxy(IDirectInput8A* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        HRESULT hr = real_->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr) && ppv && *ppv == (void*)real_)
            *ppv = static_cast<IDirectInput8A*>(this);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = real_->Release();
        if (n == 0) {
            this->~DI8Proxy();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return n;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A* out,
                                           LPUNKNOWN outer) override
    {
        HRESULT hr = real_->CreateDevice(rguid, out, outer);
        if (FAILED(hr) || !out || !*out) return hr;

        const bool isKb = (IsEqualGUID(rguid, GUID_SysKeyboard) != 0);
        const bool isMs = (IsEqualGUID(rguid, GUID_SysMouse) != 0);
        if (!isKb && !isMs) return hr;               /* leave other devices alone */

        void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(DIDeviceProxy));
        if (!mem) return hr;
        DIDeviceProxy* w = new (mem) DIDeviceProxy(*out, isKb);

        Log("[device] wrapped %s 0x%08X as 0x%08X", isKb ? "keyboard" : "mouse",
            PTRV(*out), PTRV(w));
        *out = w;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a, LPDIENUMDEVICESCALLBACKA b,
                                          LPVOID c, DWORD d) override
    { return real_->EnumDevices(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID a) override
    { return real_->GetDeviceStatus(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override
    { return real_->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b) override
    { return real_->Initialize(a, b); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, LPCSTR b, LPGUID c) override
    { return real_->FindDevice(a, b, c); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR a, LPDIACTIONFORMATA b,
                                                     LPDIENUMDEVICESBYSEMANTICSCBA c,
                                                     LPVOID d, DWORD e) override
    { return real_->EnumDevicesBySemantics(a, b, c, d, e); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a,
                                               LPDICONFIGUREDEVICESPARAMSA b,
                                               DWORD c, LPVOID d) override
    { return real_->ConfigureDevices(a, b, c, d); }

private:
    IDirectInput8A* real_;
};

/* ------------------------------------------------------------------ exports */

extern "C" {

typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                  LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    LoadConfig();

    HMODULE m = GetChain();
    PFN_DI8Create fn = m ? (PFN_DI8Create)GetProcAddress(m, "DirectInput8Create") : NULL;
    if (!fn) { Log("[init] cannot resolve DirectInput8Create"); return E_FAIL; }

    HRESULT hr = fn(hinst, ver, riid, ppvOut, punkOuter);
    if (FAILED(hr) || !ppvOut || !*ppvOut) return hr;

    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(DI8Proxy));
    if (!mem) return hr;

    /* IDirectInput8W has the same vtable layout, so the ANSI wrapper serves both. */
    DI8Proxy* w = new (mem) DI8Proxy((IDirectInput8A*)*ppvOut);
    Log("[init] wrapped IDirectInput8 0x%08X as 0x%08X", PTRV(*ppvOut), PTRV(w));
    *ppvOut = w;
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    typedef HRESULT (WINAPI *PFN)(void);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "DllCanUnloadNow") : NULL);
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c)
{
    typedef HRESULT (WINAPI *PFN)(REFCLSID, REFIID, LPVOID*);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "DllGetClassObject") : NULL);
    return fn ? fn(a, b, c) : E_NOTIMPL;
}

HRESULT WINAPI DllRegisterServer(void)
{
    typedef HRESULT (WINAPI *PFN)(void);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "DllRegisterServer") : NULL);
    return fn ? fn() : E_NOTIMPL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    typedef HRESULT (WINAPI *PFN)(void);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "DllUnregisterServer") : NULL);
    return fn ? fn() : E_NOTIMPL;
}

LPCDIDATAFORMAT WINAPI GetdfDIJoystick(void)
{
    typedef LPCDIDATAFORMAT (WINAPI *PFN)(void);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "GetdfDIJoystick") : NULL);
    return fn ? fn() : NULL;
}

} /* extern "C" */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (kProxyMarker[0] == 0) return FALSE;   /* keeps the marker in the binary */
        g_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        g_lockReady = TRUE;
        memset(g_keys, 0, sizeof(g_keys));
        memset(g_prevKeys, 0, sizeof(g_prevKeys));
        memset(g_mouseBtn, 0, sizeof(g_mouseBtn));
        memset(g_prevMouseBtn, 0, sizeof(g_prevMouseBtn));
        BuildPaths();
        LoadConfig();
        /* No LoadLibrary here - the loader lock would deadlock. See GetChain(). */
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        if (g_lockReady) { DeleteCriticalSection(&g_lock); g_lockReady = FALSE; }
    }
    return TRUE;
}
