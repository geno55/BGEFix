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
 * Chaining
 * --------
 * As with the d3d9 proxy, only one file can be called dinput8.dll. If another wrapper is
 * already present, rename it to dinput8_chain.dll and we forward to it; otherwise we load
 * the system copy. A 32-bit process gets SysWOW64 via file system redirection.
 *
 * License: MIT.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise this DLL as one of ours across rebuilds. See the note in
 * d3d9_windowed.cpp. Referenced from DllMain so it is not optimised out. */
static const char kProxyMarker[] = "BGEFIX_PROXY_V1";

/* ------------------------------------------------------------------ DirectInput types */

/* Declared locally so no DirectX SDK headers are required. */
static const GUID kSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kSysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef struct {
    DWORD dwOfs;
    DWORD dwData;
    DWORD dwTimeStamp;
    DWORD dwSequence;
    /* uAppData follows in DX8 (cbObjectData tells us whether it is present) */
} DIOBJDATA;

struct IDI8;
struct IDIDev8;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDI8*, const GUID*, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDI8*);
    ULONG   (STDMETHODCALLTYPE *Release)(IDI8*);
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(IDI8*, const GUID*, IDIDev8**, void*);
    HRESULT (STDMETHODCALLTYPE *EnumDevices)(IDI8*, DWORD, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetDeviceStatus)(IDI8*, const GUID*);
    HRESULT (STDMETHODCALLTYPE *RunControlPanel)(IDI8*, HWND, DWORD);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IDI8*, HINSTANCE, DWORD);
    HRESULT (STDMETHODCALLTYPE *FindDevice)(IDI8*, const GUID*, const void*, GUID*);
    HRESULT (STDMETHODCALLTYPE *EnumDevicesBySemantics)(IDI8*, const void*, void*, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *ConfigureDevices)(IDI8*, void*, void*, DWORD, void*);
} IDI8Vtbl;
struct IDI8 { IDI8Vtbl* lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDIDev8*, const GUID*, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDIDev8*);
    ULONG   (STDMETHODCALLTYPE *Release)(IDIDev8*);
    HRESULT (STDMETHODCALLTYPE *GetCapabilities)(IDIDev8*, void*);
    HRESULT (STDMETHODCALLTYPE *EnumObjects)(IDIDev8*, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetProperty)(IDIDev8*, const GUID*, void*);
    HRESULT (STDMETHODCALLTYPE *SetProperty)(IDIDev8*, const GUID*, const void*);
    HRESULT (STDMETHODCALLTYPE *Acquire)(IDIDev8*);
    HRESULT (STDMETHODCALLTYPE *Unacquire)(IDIDev8*);
    HRESULT (STDMETHODCALLTYPE *GetDeviceState)(IDIDev8*, DWORD, void*);
    HRESULT (STDMETHODCALLTYPE *GetDeviceData)(IDIDev8*, DWORD, DIOBJDATA*, DWORD*, DWORD);
    HRESULT (STDMETHODCALLTYPE *SetDataFormat)(IDIDev8*, const void*);
    HRESULT (STDMETHODCALLTYPE *SetEventNotification)(IDIDev8*, HANDLE);
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(IDIDev8*, HWND, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetObjectInfo)(IDIDev8*, void*, DWORD, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetDeviceInfo)(IDIDev8*, void*);
    HRESULT (STDMETHODCALLTYPE *RunControlPanel)(IDIDev8*, HWND, DWORD);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IDIDev8*, HINSTANCE, DWORD, const GUID*);
    HRESULT (STDMETHODCALLTYPE *CreateEffect)(IDIDev8*, const GUID*, const void*, void**, void*);
    HRESULT (STDMETHODCALLTYPE *EnumEffects)(IDIDev8*, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetEffectInfo)(IDIDev8*, void*, const GUID*);
    HRESULT (STDMETHODCALLTYPE *GetForceFeedbackState)(IDIDev8*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SendForceFeedbackCommand)(IDIDev8*, DWORD);
    HRESULT (STDMETHODCALLTYPE *EnumCreatedEffectObjects)(IDIDev8*, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *Escape)(IDIDev8*, void*);
    HRESULT (STDMETHODCALLTYPE *Poll)(IDIDev8*);
    HRESULT (STDMETHODCALLTYPE *SendDeviceData)(IDIDev8*, DWORD, const void*, DWORD*, DWORD);
    HRESULT (STDMETHODCALLTYPE *EnumEffectsInFile)(IDIDev8*, const void*, void*, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *WriteEffectToFile)(IDIDev8*, const void*, DWORD, void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *BuildActionMap)(IDIDev8*, void*, const void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *SetActionMap)(IDIDev8*, void*, const void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetImageInfo)(IDIDev8*, void*);
} IDIDev8Vtbl;
struct IDIDev8 { IDIDev8Vtbl* lpVtbl; };

typedef struct { IDI8Vtbl*    lpVtbl; IDI8*    real; } WrapDI;
typedef struct { IDIDev8Vtbl* lpVtbl; IDIDev8* real; int isKeyboard; } WrapDev;

/* ------------------------------------------------------------------ XInput */

typedef struct {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XPAD;
typedef struct { DWORD dwPacketNumber; XPAD Gamepad; } XSTATE;

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XSTATE*);

#define XB_DPAD_UP    0x0001
#define XB_DPAD_DOWN  0x0002
#define XB_DPAD_LEFT  0x0004
#define XB_DPAD_RIGHT 0x0008
#define XB_START      0x0010
#define XB_BACK       0x0020
#define XB_LTHUMB     0x0040
#define XB_RTHUMB     0x0080
#define XB_LB         0x0100
#define XB_RB         0x0200
#define XB_A          0x1000
#define XB_B          0x2000
#define XB_X          0x4000
#define XB_Y          0x8000

/* A mapping target: 0 = unmapped, MOUSE_BASE+n = mouse button n, else a DIK scan code. */
#define MOUSE_BASE 0x1000

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

    XSTATE st;
    DWORD ok = ERROR_DEVICE_NOT_CONNECTED;
    for (DWORD i = 0; i < 4; ++i) {
        memset(&st, 0, sizeof(st));
        if (g_XInputGetState(i, &st) == ERROR_SUCCESS) { ok = ERROR_SUCCESS; break; }
    }
    if (ok != ERROR_SUCCESS) return;

    WORD b = st.Gamepad.wButtons;
    if (b & XB_A)         Press(g_mA);
    if (b & XB_B)         Press(g_mB);
    if (b & XB_X)         Press(g_mX);
    if (b & XB_Y)         Press(g_mY);
    if (b & XB_LB)        Press(g_mLB);
    if (b & XB_RB)        Press(g_mRB);
    if (b & XB_START)     Press(g_mStart);
    if (b & XB_BACK)      Press(g_mBack);
    if (b & XB_LTHUMB)    Press(g_mLS);
    if (b & XB_RTHUMB)    Press(g_mRS);
    if (b & XB_DPAD_UP)   Press(g_mDU);
    if (b & XB_DPAD_DOWN) Press(g_mDD);
    if (b & XB_DPAD_LEFT) Press(g_mDL);
    if (b & XB_DPAD_RIGHT)Press(g_mDR);

    if (st.Gamepad.bLeftTrigger  > g_triggerThreshold) Press(g_mLT);
    if (st.Gamepad.bRightTrigger > g_triggerThreshold) Press(g_mRT);

    /* Left stick -> digital movement keys. The game has no analog movement path. */
    int lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
    if (lx >  g_deadzone) Press(g_sRight);
    if (lx < -g_deadzone) Press(g_sLeft);
    if (ly >  g_deadzone) Press(g_sUp);
    if (ly < -g_deadzone) Press(g_sDown);

    /* D-pad doubles as movement when not otherwise mapped. */
    if ((b & XB_DPAD_UP)    && g_mDU == 0) Press(g_sUp);
    if ((b & XB_DPAD_DOWN)  && g_mDD == 0) Press(g_sDown);

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

#define DREAL(p) (((WrapDev*)(p))->real)
#define DVT(p)   (DREAL(p)->lpVtbl)

static HRESULT STDMETHODCALLTYPE D_QueryInterface(IDIDev8* s, const GUID* r, void** o)
{
    HRESULT hr = DVT(s)->QueryInterface(DREAL(s), r, o);
    if (SUCCEEDED(hr) && o && *o == (void*)DREAL(s)) *o = s;
    return hr;
}
static ULONG STDMETHODCALLTYPE D_AddRef(IDIDev8* s) { return DVT(s)->AddRef(DREAL(s)); }
static ULONG STDMETHODCALLTYPE D_Release(IDIDev8* s)
{
    IDIDev8* r = DREAL(s);
    ULONG n = r->lpVtbl->Release(r);
    if (n == 0) HeapFree(GetProcessHeap(), 0, s);
    return n;
}

static HRESULT STDMETHODCALLTYPE D_GetDeviceState(IDIDev8* s, DWORD cb, void* data)
{
    HRESULT hr = DVT(s)->GetDeviceState(DREAL(s), cb, data);
    if (FAILED(hr) || !data) return hr;

    PollPad();

    if (((WrapDev*)s)->isKeyboard) {
        if (cb >= 256) {
            BYTE* k = (BYTE*)data;
            for (int i = 0; i < 256; ++i) if (g_keys[i]) k[i] = 0x80;
        }
    }
    else {
        /* DIMOUSESTATE: LONG lX, lY, lZ; BYTE rgbButtons[4 or 8] */
        if (cb >= 12) {
            LONG* ax = (LONG*)data;
            ax[0] += g_lookX;
            ax[1] += g_lookY;
            DWORD nb = cb - 12;
            if (nb > 8) nb = 8;
            BYTE* btn = (BYTE*)data + 12;
            for (DWORD i = 0; i < nb; ++i) if (g_mouseBtn[i]) btn[i] = 0x80;
        }
    }
    return hr;
}

/* Buffered mode: synthesise press/release events for anything that changed since the
 * last poll, and append them after whatever the real device returned. */
static HRESULT STDMETHODCALLTYPE D_GetDeviceData(IDIDev8* s, DWORD cbObj, DIOBJDATA* rgdod,
                                                 DWORD* pdwInOut, DWORD flags)
{
    HRESULT hr = DVT(s)->GetDeviceData(DREAL(s), cbObj, rgdod, pdwInOut, flags);
    if (FAILED(hr) || !pdwInOut) return hr;

    /* Peek must not consume our synthetic events either. */
    const BOOL peek = (flags & 1 /* DIGDD_PEEK */) != 0;
    if (!rgdod) return hr;                      /* caller only wants a count */
    if (cbObj < sizeof(DIOBJDATA)) return hr;

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

    if (((WrapDev*)s)->isKeyboard) { cur = g_keys; prev = g_prevKeys; n = 256; }
    else {
        memcpy(curB, g_mouseBtn, 8);
        cur = curB; prev = g_prevMouseBtn; n = 8;
    }

    for (int i = 0; i < n && used < cap; ++i) {
        if (cur[i] == prev[i]) continue;
        DIOBJDATA* o = (DIOBJDATA*)(base + (size_t)used * cbObj);
        /* Mouse button offsets in DIMOUSESTATE start at byte 12. */
        o->dwOfs       = ((WrapDev*)s)->isKeyboard ? (DWORD)i : (DWORD)(12 + i);
        o->dwData      = cur[i] ? 0x80 : 0x00;
        o->dwTimeStamp = GetTickCount();
        o->dwSequence  = g_seq++;
        if (cbObj >= sizeof(DIOBJDATA) + sizeof(UINT_PTR))
            *(UINT_PTR*)((BYTE*)o + sizeof(DIOBJDATA)) = 0;
        used++;
    }

    if (!peek) memcpy(prev, cur, n);
    *pdwInOut = used;
    return hr;
}

static HRESULT STDMETHODCALLTYPE D_GetCapabilities(IDIDev8* s, void* a) { return DVT(s)->GetCapabilities(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_EnumObjects(IDIDev8* s, void* a, void* b, DWORD c) { return DVT(s)->EnumObjects(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_GetProperty(IDIDev8* s, const GUID* a, void* b) { return DVT(s)->GetProperty(DREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE D_SetProperty(IDIDev8* s, const GUID* a, const void* b) { return DVT(s)->SetProperty(DREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE D_Acquire(IDIDev8* s) { return DVT(s)->Acquire(DREAL(s)); }
static HRESULT STDMETHODCALLTYPE D_Unacquire(IDIDev8* s) { return DVT(s)->Unacquire(DREAL(s)); }
static HRESULT STDMETHODCALLTYPE D_SetDataFormat(IDIDev8* s, const void* a) { return DVT(s)->SetDataFormat(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_SetEventNotification(IDIDev8* s, HANDLE a) { return DVT(s)->SetEventNotification(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_SetCooperativeLevel(IDIDev8* s, HWND a, DWORD b) { return DVT(s)->SetCooperativeLevel(DREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE D_GetObjectInfo(IDIDev8* s, void* a, DWORD b, DWORD c) { return DVT(s)->GetObjectInfo(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_GetDeviceInfo(IDIDev8* s, void* a) { return DVT(s)->GetDeviceInfo(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_RunControlPanel(IDIDev8* s, HWND a, DWORD b) { return DVT(s)->RunControlPanel(DREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE D_Initialize(IDIDev8* s, HINSTANCE a, DWORD b, const GUID* c) { return DVT(s)->Initialize(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_CreateEffect(IDIDev8* s, const GUID* a, const void* b, void** c, void* d) { return DVT(s)->CreateEffect(DREAL(s), a, b, c, d); }
static HRESULT STDMETHODCALLTYPE D_EnumEffects(IDIDev8* s, void* a, void* b, DWORD c) { return DVT(s)->EnumEffects(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_GetEffectInfo(IDIDev8* s, void* a, const GUID* b) { return DVT(s)->GetEffectInfo(DREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE D_GetForceFeedbackState(IDIDev8* s, DWORD* a) { return DVT(s)->GetForceFeedbackState(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_SendForceFeedbackCommand(IDIDev8* s, DWORD a) { return DVT(s)->SendForceFeedbackCommand(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_EnumCreatedEffectObjects(IDIDev8* s, void* a, void* b, DWORD c) { return DVT(s)->EnumCreatedEffectObjects(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_Escape(IDIDev8* s, void* a) { return DVT(s)->Escape(DREAL(s), a); }
static HRESULT STDMETHODCALLTYPE D_Poll(IDIDev8* s) { return DVT(s)->Poll(DREAL(s)); }
static HRESULT STDMETHODCALLTYPE D_SendDeviceData(IDIDev8* s, DWORD a, const void* b, DWORD* c, DWORD d) { return DVT(s)->SendDeviceData(DREAL(s), a, b, c, d); }
static HRESULT STDMETHODCALLTYPE D_EnumEffectsInFile(IDIDev8* s, const void* a, void* b, void* c, DWORD d) { return DVT(s)->EnumEffectsInFile(DREAL(s), a, b, c, d); }
static HRESULT STDMETHODCALLTYPE D_WriteEffectToFile(IDIDev8* s, const void* a, DWORD b, void* c, DWORD d) { return DVT(s)->WriteEffectToFile(DREAL(s), a, b, c, d); }
static HRESULT STDMETHODCALLTYPE D_BuildActionMap(IDIDev8* s, void* a, const void* b, DWORD c) { return DVT(s)->BuildActionMap(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_SetActionMap(IDIDev8* s, void* a, const void* b, DWORD c) { return DVT(s)->SetActionMap(DREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE D_GetImageInfo(IDIDev8* s, void* a) { return DVT(s)->GetImageInfo(DREAL(s), a); }

static IDIDev8Vtbl g_devVtbl = {
    D_QueryInterface, D_AddRef, D_Release,
    D_GetCapabilities, D_EnumObjects, D_GetProperty, D_SetProperty,
    D_Acquire, D_Unacquire, D_GetDeviceState, D_GetDeviceData,
    D_SetDataFormat, D_SetEventNotification, D_SetCooperativeLevel,
    D_GetObjectInfo, D_GetDeviceInfo, D_RunControlPanel, D_Initialize,
    D_CreateEffect, D_EnumEffects, D_GetEffectInfo, D_GetForceFeedbackState,
    D_SendForceFeedbackCommand, D_EnumCreatedEffectObjects, D_Escape, D_Poll,
    D_SendDeviceData, D_EnumEffectsInFile, D_WriteEffectToFile,
    D_BuildActionMap, D_SetActionMap, D_GetImageInfo
};

/* ------------------------------------------------------------------ factory wrapper */

#define IREAL(p) (((WrapDI*)(p))->real)
#define IVT(p)   (IREAL(p)->lpVtbl)

static HRESULT STDMETHODCALLTYPE I_QueryInterface(IDI8* s, const GUID* r, void** o)
{
    HRESULT hr = IVT(s)->QueryInterface(IREAL(s), r, o);
    if (SUCCEEDED(hr) && o && *o == (void*)IREAL(s)) *o = s;
    return hr;
}
static ULONG STDMETHODCALLTYPE I_AddRef(IDI8* s) { return IVT(s)->AddRef(IREAL(s)); }
static ULONG STDMETHODCALLTYPE I_Release(IDI8* s)
{
    IDI8* r = IREAL(s);
    ULONG n = r->lpVtbl->Release(r);
    if (n == 0) HeapFree(GetProcessHeap(), 0, s);
    return n;
}

static HRESULT STDMETHODCALLTYPE I_CreateDevice(IDI8* s, const GUID* rguid, IDIDev8** out, void* outer)
{
    HRESULT hr = IVT(s)->CreateDevice(IREAL(s), rguid, out, outer);
    if (FAILED(hr) || !out || !*out || !rguid) return hr;

    int isKb  = (memcmp(rguid, &kSysKeyboard, sizeof(GUID)) == 0);
    int isMs  = (memcmp(rguid, &kSysMouse,    sizeof(GUID)) == 0);
    if (!isKb && !isMs) return hr;               /* leave other devices alone */

    WrapDev* w = (WrapDev*)HeapAlloc(GetProcessHeap(), 0, sizeof(WrapDev));
    if (!w) return hr;
    w->lpVtbl     = &g_devVtbl;
    w->real       = *out;
    w->isKeyboard = isKb;
    *out = (IDIDev8*)w;

    Log("[device] wrapped %s 0x%08X as 0x%08X", isKb ? "keyboard" : "mouse",
        PTRV(w->real), PTRV(w));
    return hr;
}

static HRESULT STDMETHODCALLTYPE I_EnumDevices(IDI8* s, DWORD a, void* b, void* c, DWORD d) { return IVT(s)->EnumDevices(IREAL(s), a, b, c, d); }
static HRESULT STDMETHODCALLTYPE I_GetDeviceStatus(IDI8* s, const GUID* a) { return IVT(s)->GetDeviceStatus(IREAL(s), a); }
static HRESULT STDMETHODCALLTYPE I_RunControlPanel(IDI8* s, HWND a, DWORD b) { return IVT(s)->RunControlPanel(IREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE I_Initialize(IDI8* s, HINSTANCE a, DWORD b) { return IVT(s)->Initialize(IREAL(s), a, b); }
static HRESULT STDMETHODCALLTYPE I_FindDevice(IDI8* s, const GUID* a, const void* b, GUID* c) { return IVT(s)->FindDevice(IREAL(s), a, b, c); }
static HRESULT STDMETHODCALLTYPE I_EnumDevicesBySemantics(IDI8* s, const void* a, void* b, void* c, void* d, DWORD e) { return IVT(s)->EnumDevicesBySemantics(IREAL(s), a, b, c, d, e); }
static HRESULT STDMETHODCALLTYPE I_ConfigureDevices(IDI8* s, void* a, void* b, DWORD c, void* d) { return IVT(s)->ConfigureDevices(IREAL(s), a, b, c, d); }

static IDI8Vtbl g_diVtbl = {
    I_QueryInterface, I_AddRef, I_Release,
    I_CreateDevice, I_EnumDevices, I_GetDeviceStatus, I_RunControlPanel,
    I_Initialize, I_FindDevice, I_EnumDevicesBySemantics, I_ConfigureDevices
};

/* ------------------------------------------------------------------ exports */

extern "C" {

typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, const GUID*, void**, void*);

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, const GUID* riid,
                                  void** ppvOut, void* punkOuter)
{
    LoadConfig();

    HMODULE m = GetChain();
    PFN_DI8Create fn = m ? (PFN_DI8Create)GetProcAddress(m, "DirectInput8Create") : NULL;
    if (!fn) { Log("[init] cannot resolve DirectInput8Create"); return E_FAIL; }

    HRESULT hr = fn(hinst, ver, riid, ppvOut, punkOuter);
    if (FAILED(hr) || !ppvOut || !*ppvOut) return hr;

    WrapDI* w = (WrapDI*)HeapAlloc(GetProcessHeap(), 0, sizeof(WrapDI));
    if (!w) return hr;
    w->lpVtbl = &g_diVtbl;
    w->real   = (IDI8*)*ppvOut;
    *ppvOut   = w;

    InitXInput();
    Log("[init] wrapped IDirectInput8 0x%08X as 0x%08X", PTRV(w->real), PTRV(w));
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    typedef HRESULT (WINAPI *PFN)(void);
    PFN fn = (PFN)(GetChain() ? GetProcAddress(GetChain(), "DllCanUnloadNow") : NULL);
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(const GUID* a, const GUID* b, void** c)
{
    typedef HRESULT (WINAPI *PFN)(const GUID*, const GUID*, void**);
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

LPCVOID WINAPI GetdfDIJoystick(void)
{
    typedef LPCVOID (WINAPI *PFN)(void);
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
