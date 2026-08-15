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
 * Input model
 * -----------
 * The keyboard and mouse wrappers are two objects that must agree about one controller,
 * so the pad is read into a single immutable PadSample:
 *
 *   Shared, under g_padLock   one PadSample, copied out by value. A sample is reused for
 *                             2 ms, so both devices polled in the same frame see the same
 *                             instant instead of each re-reading XInput separately.
 *   Per device wrapper        edge-detection buffer, look carry, last-look timestamp.
 *                             Nothing else is shared, so nothing else can race.
 *
 * Look is a velocity, not a delta. RebuildSample stores the right stick as -1..1 and the
 * mouse wrapper multiplies by LookSpeed (counts per second) and by the time actually
 * elapsed since it last reported motion, measured with QueryPerformanceCounter. Adding a
 * fixed delta per poll instead makes the camera's speed depend on how often the game
 * polls: the same setting turned 2.4x faster at 144 Hz than at 60 Hz.
 *
 * Finding the pad is throttled. XInputGetState on an empty slot is a documented slow
 * path, so the connected user index is remembered and a poll costs one call; all four
 * slots are only swept when nothing is connected or the current pad is idle, and then at
 * most every two seconds. Sweeping 0..3 on every poll cost an unplugged machine around
 * 1150 slow calls a second at 144 Hz, in a game pinned to one core for timing.
 *
 * Stick shaping and the scan policy live in pad_support.h so the tests can assert on
 * them without a controller attached. See that file for why the deadzone is radial and
 * rescaled.
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
#include "pad_support.h"
#include <new>

#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise this DLL as one of ours across rebuilds. The GUID is the
 * identity, the trailing number is a version the installer parses rather than compares.
 * See the fuller note in d3d9_windowed.cpp. Referenced from DllMain so it is not
 * optimised out. */
static const char kProxyMarker[] = "BGEFIX_PROXY{502eb6b9-f979-4627-b242-8e146e0fc1de}v2";

/* A mapping target: 0 = unmapped, MOUSE_BASE+n = mouse button n, else a DIK scan code. */
#define MOUSE_BASE 0x1000

/* Mouse button n lives at this byte offset in DIMOUSESTATE/DIMOUSESTATE2. */
#define MOUSE_BTN_OFS(n) (FIELD_OFFSET(DIMOUSESTATE2, rgbButtons) + (n))

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);

/* ------------------------------------------------------------------ state */

static HMODULE g_self = NULL, g_chain = NULL, g_xinput = NULL;
static PFN_XInputGetState g_XInputGetState = NULL;

/* Two locks, each with one job. g_modLock covers lazy module loading (the chain DLL and
 * the XInput runtime); g_padLock covers the pad sample and the settings it is built
 * from. Lock order is pad -> module; nothing ever takes them the other way round. */
static CRITICAL_SECTION g_modLock;
static CRITICAL_SECTION g_padLock;
static BOOL g_locksReady = FALSE;

static BOOL g_log = FALSE;
static wchar_t g_ini[MAX_PATH] = {0}, g_logPath[MAX_PATH] = {0};

/* Settings. Written by LoadConfig and read by RebuildSample, both under g_padLock. */
static int  g_mA, g_mB, g_mX, g_mY, g_mLB, g_mRB, g_mLT, g_mRT;
static int  g_mStart, g_mBack, g_mLS, g_mRS;
static int  g_mDU, g_mDD, g_mDL, g_mDR;
static int  g_sUp, g_sDown, g_sLeft, g_sRight;
static int  g_deadzone          = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
static int  g_lookDeadzone      = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
static int  g_triggerThreshold  = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
static int  g_lookSpeed         = 1800;   /* mouse counts per second at full deflection */
static int  g_invertLook        = 0;

/* One sample of the pad at one instant.
 *
 * This is the ONLY mutable state shared between the keyboard and mouse wrappers, and
 * every read and write of it happens under g_padLock. It is copied out by value, so a
 * caller works on a private snapshot and cannot see another device's rebuild in
 * progress. The previous design had both wrappers memset and refill a set of globals
 * with no lock at all: one device's clear was another device's read.
 *
 * lookX/lookY are a VELOCITY in -1..1, not a per-poll delta. Turning that into mouse
 * counts is the mouse wrapper's job, because only it knows how much time has passed
 * since it last reported motion. */
typedef struct {
    BYTE     keys[256];      /* DIK scan codes to force down */
    BYTE     mouseBtn[8];
    double   lookX, lookY;
    LONGLONG taken;          /* QPC stamp; 0 = never sampled */
} PadSample;

static PadSample g_pad;
static LONGLONG  g_qpcFreq = 0;
/* Buffered-event sequence numbering, under g_padLock. g_seqAnchor is the highest real
 * DirectInput sequence number seen on either wrapped device; g_seqLast is the last
 * synthetic number we issued. See PadSeqNext in pad_support.h. */
static DWORD     g_seqAnchor = 0;
static DWORD     g_seqLast   = 0;

/* How long a sample stays fresh. Any device polled inside this window sees the SAME
 * instant of controller state, so the keyboard events and the mouse events the game
 * collects in one frame agree with each other. Well under a frame at any refresh rate,
 * so this never shows up as input lag. */
static const double kSampleMaxAgeSec = 0.002;

/* An unusually long gap between mouse reads - a menu, an alt-tab, a debugger - must not
 * be converted into one enormous camera jump. */
static const double kMaxLookStepSec = 0.25;

/* How often we may go looking for a controller. Long enough that an unplugged machine
 * pays almost nothing, short enough that plugging a pad in feels immediate. */
static const double kRescanSec = 2.0;

/* The XInput user index currently being read, or -1 if none is known connected, and when
 * we last swept all four slots. Both live under g_padLock. */
static int      g_padIndex = -1;
static LONGLONG g_lastScan = 0;

/* ------------------------------------------------------------------ time base */

static LONGLONG Now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double SecondsSince(LONGLONG then, LONGLONG now)
{
    if (g_qpcFreq <= 0 || then <= 0 || now <= then) return 0.0;
    return (double)(now - then) / (double)g_qpcFreq;
}

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

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Takes g_padLock: these values are read while building a sample. */
static void LoadConfig(void)
{
    if (!g_ini[0]) return;
    if (g_locksReady) EnterCriticalSection(&g_padLock);

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

    /* Deadzone and trigger defaults are XInput's own recommended values from xinput.h
     * rather than numbers picked here. Clamped so a hand-edited ini cannot reach the
     * deadzone == full-scale division in ApplyDeadzone. */
    g_deadzone     = ClampInt((int)GetPrivateProfileIntW(L"Sticks", L"Deadzone",
                              XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, g_ini), 0, 32000);
    g_lookDeadzone = ClampInt((int)GetPrivateProfileIntW(L"Sticks", L"LookDeadzone",
                              XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, g_ini), 0, 32000);
    g_triggerThreshold = ClampInt((int)GetPrivateProfileIntW(L"Buttons", L"TriggerThreshold",
                              XINPUT_GAMEPAD_TRIGGER_THRESHOLD, g_ini), 0, 254);
    g_invertLook   = (int)GetPrivateProfileIntW(L"Sticks", L"InvertLook", 0, g_ini);

    /* LookSpeed is mouse counts per second at full stick deflection - a real unit, the
     * same on every machine. It replaces LookSensitivity, which was counts per *poll*:
     * motion per second was polls-per-second x value, so the camera swung 2.4x faster
     * at 144 Hz than at 60 Hz and the documented "30" meant nothing on its own. An
     * ini still carrying the old key just gets the default. */
    g_lookSpeed = ClampInt((int)GetPrivateProfileIntW(L"Sticks", L"LookSpeed",
                           1800, g_ini), 1, 20000);

    g_log = GetPrivateProfileIntW(L"General", L"Log", 0, g_ini) != 0;

    if (g_locksReady) LeaveCriticalSection(&g_padLock);
}

/* ------------------------------------------------------------------ chain */

static HMODULE GetChain(void)
{
    if (g_chain) return g_chain;
    if (!g_locksReady) return NULL;

    EnterCriticalSection(&g_modLock);
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
    LeaveCriticalSection(&g_modLock);
    return g_chain;
}

/* Module loading, so it belongs to g_modLock rather than g_padLock. Callers may already
 * hold g_padLock; that ordering is fine and never reversed. */
static void InitXInput(void)
{
    if (g_XInputGetState) return;
    if (!g_locksReady) return;

    EnterCriticalSection(&g_modLock);
    if (!g_XInputGetState) {
        const wchar_t* names[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
        for (int i = 0; i < 3; ++i) {
            g_xinput = LoadLibraryW(names[i]);
            if (g_xinput) {
                g_XInputGetState = (PFN_XInputGetState)GetProcAddress(g_xinput, "XInputGetState");
                if (g_XInputGetState) { Log("[xinput] using %S", names[i]); break; }
                FreeLibrary(g_xinput); g_xinput = NULL;
            }
        }
        if (!g_XInputGetState) Log("[xinput] no XInput runtime found");
    }
    LeaveCriticalSection(&g_modLock);
}

/* ------------------------------------------------------------------ sampling */

static void PressInto(PadSample* s, int target)
{
    if (target <= 0) return;
    if (target >= MOUSE_BASE) {
        int b = target - MOUSE_BASE;
        if (b >= 0 && b < 8) s->mouseBtn[b] = 0x80;
    }
    else if (target < 256) {
        s->keys[target] = 0x80;
    }
}

/* Is nobody touching this pad? Uses the configured deadzones, so a stick resting slightly
 * off centre still counts as idle. */
static BOOL PadIsIdle(const XINPUT_STATE* st)
{
    const XINPUT_GAMEPAD* g = &st->Gamepad;
    double x, y;

    if (g->wButtons) return FALSE;
    if (g->bLeftTrigger  > g_triggerThreshold) return FALSE;
    if (g->bRightTrigger > g_triggerThreshold) return FALSE;

    PadApplyDeadzone(g->sThumbLX, g->sThumbLY, g_deadzone, &x, &y);
    if (x != 0.0 || y != 0.0) return FALSE;
    PadApplyDeadzone(g->sThumbRX, g->sThumbRY, g_lookDeadzone, &x, &y);
    if (x != 0.0 || y != 0.0) return FALSE;

    return TRUE;
}

/* Reads the pad we should be listening to. Returns FALSE if there is nothing to read.
 * Caller must hold g_padLock.
 *
 * The cost model, which is the whole point of this function (see PadShouldScan):
 *
 *   playing on a pad      one XInputGetState, on the remembered index
 *   nothing connected     one four-slot scan every kRescanSec, nothing in between
 *   pad connected, idle   same, so swapping controllers mid-session still works
 *
 * Previously every poll walked slots 0..3 until one answered, from both wrapped devices,
 * so an unplugged machine paid 8 slow calls a frame - about 1150 a second at 144 Hz. */
static BOOL ReadPad(XINPUT_STATE* st, LONGLONG now)
{
    BOOL haveCurrent = FALSE;
    memset(st, 0, sizeof(*st));

    /* Fast path: a pad we already know about costs exactly one call. */
    if (g_padIndex >= 0) {
        if (g_XInputGetState((DWORD)g_padIndex, st) == ERROR_SUCCESS) {
            haveCurrent = TRUE;
        } else {
            Log("[xinput] pad %d disconnected", g_padIndex);
            g_padIndex = -1;
            memset(st, 0, sizeof(*st));
        }
    }

    double sinceScan = (g_lastScan == 0) ? 1e9 : SecondsSince(g_lastScan, now);
    int    idle      = haveCurrent ? (PadIsIdle(st) ? 1 : 0) : 1;

    if (!PadShouldScan(haveCurrent ? 1 : 0, idle, sinceScan, kRescanSec))
        return haveCurrent;

    g_lastScan = now;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (haveCurrent && (int)i == g_padIndex) continue;

        XINPUT_STATE probe;
        memset(&probe, 0, sizeof(probe));
        if (g_XInputGetState(i, &probe) != ERROR_SUCCESS) continue;

        /* Prefer a pad somebody is actually holding - that is how pad 1 gets picked up
         * while pad 0 sits connected but untouched. With nothing current, take the
         * first connected slot so a single idle pad still works. */
        if (!haveCurrent || !PadIsIdle(&probe)) {
            Log("[xinput] using pad %u", i);
            g_padIndex = (int)i;
            *st = probe;
            return TRUE;
        }
    }

    return haveCurrent;
}

/* ------------------------------------------------------------------ event sequencing */

/* Learn DirectInput's current numbering from the events it just handed the caller.
 * Caller must hold g_padLock. */
static void SeqObserveLocked(const BYTE* base, DWORD count, DWORD cbObj)
{
    for (DWORD i = 0; i < count; ++i) {
        const DIDEVICEOBJECTDATA_DX3* o =
            (const DIDEVICEOBJECTDATA_DX3*)(base + (size_t)i * cbObj);
        if (PadSeqAfter(o->dwSequence, g_seqAnchor)) g_seqAnchor = o->dwSequence;
    }
}

/* Caller must hold g_padLock. */
static DWORD SeqNextLocked(void)
{
    g_seqLast = (DWORD)PadSeqNext(g_seqAnchor, g_seqLast);
    return g_seqLast;
}

/* Rebuilds g_pad from the active pad. Caller must hold g_padLock. */
static void RebuildSample(LONGLONG now)
{
    memset(&g_pad, 0, sizeof(g_pad));
    g_pad.taken = now;

    InitXInput();
    if (!g_XInputGetState) return;

    XINPUT_STATE st;
    if (!ReadPad(&st, now)) return;

    WORD b = st.Gamepad.wButtons;
    if (b & XINPUT_GAMEPAD_A)              PressInto(&g_pad, g_mA);
    if (b & XINPUT_GAMEPAD_B)              PressInto(&g_pad, g_mB);
    if (b & XINPUT_GAMEPAD_X)              PressInto(&g_pad, g_mX);
    if (b & XINPUT_GAMEPAD_Y)              PressInto(&g_pad, g_mY);
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  PressInto(&g_pad, g_mLB);
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) PressInto(&g_pad, g_mRB);
    if (b & XINPUT_GAMEPAD_START)          PressInto(&g_pad, g_mStart);
    if (b & XINPUT_GAMEPAD_BACK)           PressInto(&g_pad, g_mBack);
    if (b & XINPUT_GAMEPAD_LEFT_THUMB)     PressInto(&g_pad, g_mLS);
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    PressInto(&g_pad, g_mRS);
    if (b & XINPUT_GAMEPAD_DPAD_UP)        PressInto(&g_pad, g_mDU);
    if (b & XINPUT_GAMEPAD_DPAD_DOWN)      PressInto(&g_pad, g_mDD);
    if (b & XINPUT_GAMEPAD_DPAD_LEFT)      PressInto(&g_pad, g_mDL);
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     PressInto(&g_pad, g_mDR);

    if (st.Gamepad.bLeftTrigger  > g_triggerThreshold) PressInto(&g_pad, g_mLT);
    if (st.Gamepad.bRightTrigger > g_triggerThreshold) PressInto(&g_pad, g_mRT);

    /* Left stick -> digital movement keys. The game has no analog movement path, so
     * the shaped vector is quantised into 8 directions: a key goes down once the stick
     * is more than 22.5 degrees onto its axis, which is the standard 8-way split. */
    double lx, ly;
    PadApplyDeadzone(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, g_deadzone, &lx, &ly);
        if (lx >  PAD_OCTANT) PressInto(&g_pad, g_sRight);
    if (lx < -PAD_OCTANT) PressInto(&g_pad, g_sLeft);
    if (ly >  PAD_OCTANT) PressInto(&g_pad, g_sUp);
    if (ly < -PAD_OCTANT) PressInto(&g_pad, g_sDown);

    /* D-pad doubles as movement when not otherwise mapped. */
    if ((b & XINPUT_GAMEPAD_DPAD_UP)   && g_mDU == 0) PressInto(&g_pad, g_sUp);
    if ((b & XINPUT_GAMEPAD_DPAD_DOWN) && g_mDD == 0) PressInto(&g_pad, g_sDown);

    /* Right stick -> look velocity in -1..1. The mouse wrapper multiplies by LookSpeed
     * and by elapsed time; nothing here depends on how often we are polled. */
    double rx, ry;
    PadApplyDeadzone(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, g_lookDeadzone, &rx, &ry);
    g_pad.lookX = rx;
    g_pad.lookY = g_invertLook ? ry : -ry;   /* stick up should look up */
}

/* Copies out the current sample, refreshing it only if the last one has aged out. Two
 * devices polled in the same frame therefore see one identical instant. */
static void SamplePad(PadSample* out)
{
    memset(out, 0, sizeof(*out));
    if (!g_locksReady) return;

    EnterCriticalSection(&g_padLock);

    LONGLONG now = Now();
    if (g_pad.taken == 0 || SecondsSince(g_pad.taken, now) >= kSampleMaxAgeSec)
        RebuildSample(now);

    *out = g_pad;
    LeaveCriticalSection(&g_padLock);
}

/* ------------------------------------------------------------------ device wrapper */

class DIDeviceProxy final : public IDirectInputDevice8A
{
public:
    DIDeviceProxy(IDirectInputDevice8A* real, bool isKeyboard)
        : real_(real), isKeyboard_(isKeyboard), lastLook_(0), remX_(0.0), remY_(0.0)
    {
        memset(prev_, 0, sizeof(prev_));
    }

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

        PadSample s;
        SamplePad(&s);

        if (isKeyboard_) {
            if (cb >= 256) {
                BYTE* k = (BYTE*)data;
                for (int i = 0; i < 256; ++i) if (s.keys[i]) k[i] = 0x80;
            }
        }
        else {
            /* DIMOUSESTATE: LONG lX, lY, lZ followed by rgbButtons[4]; DIMOUSESTATE2
             * is identical with rgbButtons[8]. Accept either. */
            const DWORD btnOfs = (DWORD)FIELD_OFFSET(DIMOUSESTATE2, rgbButtons);
            if (cb >= btnOfs) {
                LONG dx = 0, dy = 0;
                ConsumeLook(s, &dx, &dy);

                LONG* ax = (LONG*)data;
                ax[0] += dx;
                ax[1] += dy;

                DWORD nb = cb - btnOfs;
                if (nb > sizeof(s.mouseBtn)) nb = sizeof(s.mouseBtn);
                BYTE* btn = (BYTE*)data + btnOfs;
                for (DWORD i = 0; i < nb; ++i) if (s.mouseBtn[i]) btn[i] = 0x80;
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

        PadSample s;
        SamplePad(&s);

        DWORD count = *pdwInOut;
        DWORD cap   = count;                        /* on entry: buffer capacity */
        DWORD used  = count;                        /* real device already filled this many */
        if (used > cap) return hr;

        BYTE* base = (BYTE*)rgdod;

        /* prev_ is per-instance: the keyboard and mouse wrappers no longer share an
         * edge-detection buffer, and s is already a private copy of the sample. */
        const BYTE* cur = isKeyboard_ ? s.keys : s.mouseBtn;
        const int   n   = isKeyboard_ ? 256 : (int)sizeof(s.mouseBtn);

        /* One acquisition for the whole batch. The sequence anchor is shared with the
         * other wrapped device because DirectInput's numbering is process-wide. */
        if (g_locksReady) EnterCriticalSection(&g_padLock);

        /* Read DirectInput's numbering off the events it just returned, then issue ours
         * from just above it, so both sets compare correctly under DISEQUENCE_COMPARE. */
        SeqObserveLocked(base, used, cbObj);

        for (int i = 0; i < n && used < cap; ++i) {
            if (cur[i] == prev_[i]) continue;
            DIDEVICEOBJECTDATA_DX3* o =
                (DIDEVICEOBJECTDATA_DX3*)(base + (size_t)used * cbObj);
            o->dwOfs       = isKeyboard_ ? (DWORD)i : (DWORD)MOUSE_BTN_OFS(i);
            o->dwData      = cur[i] ? 0x80 : 0x00;
            o->dwTimeStamp = GetTickCount();
            o->dwSequence  = SeqNextLocked();
            if (cbObj >= sizeof(DIDEVICEOBJECTDATA))
                ((DIDEVICEOBJECTDATA*)o)->uAppData = 0;
            used++;
        }

        if (g_locksReady) LeaveCriticalSection(&g_padLock);

        if (!peek) memcpy(prev_, cur, (size_t)n);
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
    /* Turns the right stick's velocity into a mouse delta for the time that has actually
     * elapsed since this device last reported one.
     *
     * This is what makes look speed frame-rate independent. The old code added a fixed
     * delta per poll, so motion per second was polls-per-second x delta and the camera
     * ran 2.4x faster at 144 Hz than at 60 Hz. LookSpeed is now counts per second and
     * means the same thing on every machine.
     *
     * The fractional part is carried over between calls, so a stick held just past the
     * deadzone still moves the camera instead of truncating to zero every frame. */
    void ConsumeLook(const PadSample& s, LONG* dx, LONG* dy)
    {
        *dx = 0;
        *dy = 0;

        LONGLONG now = Now();
        double dt = SecondsSince(lastLook_, now);
        lastLook_ = now;

        /* First call, or a long gap - a menu, an alt-tab, a breakpoint. Start clean
         * rather than applying the whole pause as one camera jump. */
        if (dt <= 0.0 || dt > kMaxLookStepSec) { remX_ = remY_ = 0.0; return; }

        *dx = (LONG)PadAccumulate(s.lookX, g_lookSpeed, dt, &remX_);
        *dy = (LONG)PadAccumulate(s.lookY, g_lookSpeed, dt, &remY_);
    }

    IDirectInputDevice8A* real_;
    bool                  isKeyboard_;

    /* Per-instance, so the two device wrappers share nothing but the pad sample. */
    BYTE                  prev_[256];   /* edge detection; mouse uses the first 8 */
    LONGLONG              lastLook_;    /* QPC stamp of the last look delta reported */
    double                remX_, remY_; /* sub-count carry */
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

        LARGE_INTEGER f;
        g_qpcFreq = QueryPerformanceFrequency(&f) ? f.QuadPart : 0;

        InitializeCriticalSection(&g_modLock);
        InitializeCriticalSection(&g_padLock);
        g_locksReady = TRUE;

        memset(&g_pad, 0, sizeof(g_pad));
        BuildPaths();
        LoadConfig();
        /* No LoadLibrary here - the loader lock would deadlock. See GetChain(). */
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        if (g_locksReady) {
            g_locksReady = FALSE;
            DeleteCriticalSection(&g_padLock);
            DeleteCriticalSection(&g_modLock);
        }
    }
    return TRUE;
}
