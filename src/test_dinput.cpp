/*
 * test_dinput - functional test for dinput8_xinput.
 *
 * Drives the proxy exactly the way BGE.exe does: DirectInput8Create, CreateDevice on
 * the system keyboard, SetDataFormat, Acquire, then GetDeviceState into a 256-byte
 * array. Anything the controller injects shows up as set scan codes.
 *
 * Interfaces, structures and device GUIDs come from the SDK's dinput.h and dxguid.lib,
 * so this test cannot drift from the real ABI. The keyboard data format is still built
 * here rather than taken from dinput8.lib's c_dfDIKeyboard, because linking that would
 * turn dinput8.dll into a load-time import and stop us loading the proxy explicitly.
 */

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad_support.h"

/* The one thing the Windows SDK's dinput.h does not carry: DIDFT_OPTIONAL existed only
 * in the legacy DirectX SDK header. It is a single documented flag bit, not a struct
 * layout, so defining it here costs nothing that a wrong vtable slot would. */
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000u
#endif

static const char* DikName(int c)
{
    switch (c) {
        case DIK_W:        return "W";     case DIK_A:      return "A";
        case DIK_S:        return "S";     case DIK_D:      return "D";
        case DIK_SPACE:    return "SPACE"; case DIK_LCONTROL: return "LCTRL";
        case DIK_Q:        return "Q";     case DIK_E:      return "E";
        case DIK_LSHIFT:   return "LSHIFT";case DIK_C:      return "C";
        case DIK_ESCAPE:   return "ESC";   case DIK_TAB:    return "TAB";
        case DIK_2:        return "2";     case DIK_3:      return "3";
        default:           return "?";
    }
}

static int g_fail = 0;
static void check(const char* what, int ok, const char* detail)
{
    printf("  %-4s %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail && *detail ? " -> " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* ------------------------------------------------------------------ stick maths
 *
 * No controller required: these assert on the shaping functions the proxy compiles, so
 * a regression in deadzone geometry or the look time base fails here rather than being
 * something a player has to notice by feel. */

static double Mag(double x, double y) { return sqrt(x * x + y * y); }

static void TestStickMaths(void)
{
    const int dz = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;   /* 7849 */
    double x, y;
    char buf[160];

    printf("\n  -- stick shaping (no controller needed) --\n");

    /* Centre and inside the deadzone stay dead. */
    PadApplyDeadzone(0, 0, dz, &x, &y);
    check("centre is dead", x == 0.0 && y == 0.0, "");
    PadApplyDeadzone(dz - 100, 0, dz, &x, &y);
    check("inside the deadzone is dead", x == 0.0 && y == 0.0, "");

    /* The cross-shaped dead region. A diagonal push equal to the deadzone on each axis
     * is 1.41x the deadzone from centre and must be live; a per-axis test rejects it. */
    PadApplyDeadzone(dz, dz, dz, &x, &y);
    wsprintfA(buf, "len=%d/1000 at raw(%d,%d)", (int)(Mag(x, y) * 1000), dz, dz);
    check("diagonal at the per-axis threshold is live", Mag(x, y) > 0.0, buf);

    /* Circular, not square: the same push in any direction gives the same magnitude. */
    double m0, m45, m90;
    PadApplyDeadzone(20000, 0, dz, &x, &y);      m0  = Mag(x, y);
    PadApplyDeadzone(14142, 14142, dz, &x, &y);  m45 = Mag(x, y);
    PadApplyDeadzone(0, 20000, dz, &x, &y);      m90 = Mag(x, y);
    wsprintfA(buf, "0deg=%d 45deg=%d 90deg=%d (/1000)",
              (int)(m0 * 1000), (int)(m45 * 1000), (int)(m90 * 1000));
    check("dead region is circular, not cross-shaped",
          Mag(m0 - m45, 0) < 0.01 && Mag(m0 - m90, 0) < 0.01, buf);

    /* Continuity: just past the edge the output must be near zero, not a step up to
     * the raw axis value (which would be ~0.24 of full scale at this deadzone). */
    PadApplyDeadzone(dz + 20, 0, dz, &x, &y);
    wsprintfA(buf, "out=%d/1000 just past edge", (int)(Mag(x, y) * 1000));
    check("response ramps from the deadzone edge", Mag(x, y) < 0.01, buf);

    /* Full deflection reaches exactly 1, including on a diagonal (which raw would
     * overshoot, since the negative axis reaches -32768). */
    PadApplyDeadzone(32767, 0, dz, &x, &y);
    check("full deflection is magnitude 1", Mag(x, y) > 0.999 && Mag(x, y) <= 1.0, "");
    PadApplyDeadzone(-32768, -32768, dz, &x, &y);
    wsprintfA(buf, "len=%d/1000", (int)(Mag(x, y) * 1000));
    check("full diagonal is clamped to 1", Mag(x, y) <= 1.0 + 1e-9, buf);

    /* Frame-rate independence: the same second of full-deflection stick must travel
     * the same distance however finely it is sliced. */
    const int speed = 1800;
    long at60 = 0, at144 = 0, at30 = 0;
    double c = 0.0;
    for (int i = 0; i < 60; ++i)  at60  += PadAccumulate(1.0, speed, 1.0 / 60.0, &c);
    c = 0.0;
    for (int i = 0; i < 144; ++i) at144 += PadAccumulate(1.0, speed, 1.0 / 144.0, &c);
    c = 0.0;
    for (int i = 0; i < 30; ++i)  at30  += PadAccumulate(1.0, speed, 1.0 / 30.0, &c);
    wsprintfA(buf, "30Hz=%d 60Hz=%d 144Hz=%d counts/sec", at30, at60, at144);
    check("look speed is frame-rate independent",
          at60 == speed && at144 == speed && at30 == speed, buf);

    /* The carry: a slow stick must still move, not truncate to zero every step. */
    c = 0.0;
    long slow = 0;
    for (int i = 0; i < 144; ++i) slow += PadAccumulate(0.02, speed, 1.0 / 144.0, &c);
    wsprintfA(buf, "%d counts/sec at 2%% deflection", slow);
    check("sub-count motion is carried, not truncated", slow > 0, buf);

    /* Releasing the stick must not leave a queued nudge. */
    c = 0.9;
    long idle = PadAccumulate(0.0, speed, 1.0 / 60.0, &c);
    wsprintfA(buf, "delta=%d carry=%d/1000", idle, (int)(c * 1000));
    check("centred stick emits nothing and drops its carry",
          idle == 0 && c == 0.0, buf);

    /* Sign symmetry: negative travel must match positive. */
    c = 0.0;
    long neg = 0;
    for (int i = 0; i < 60; ++i) neg += PadAccumulate(-1.0, speed, 1.0 / 60.0, &c);
    wsprintfA(buf, "%d vs %d", neg, -at60);
    check("negative direction travels the same distance", neg == -at60, buf);
}

/* ------------------------------------------------------------------ scan throttle
 *
 * XInputGetState on an empty slot is a documented slow path. These assert on the policy
 * that decides when the proxy is allowed to sweep all four user indices. */
static void TestScanPolicy(void)
{
    const double rescan = 2.0;
    char buf[160];

    printf("\n  -- XInput slot scanning --\n");

    /* Mid-game on a live pad: never scan, however long it has been. */
    check("connected and in use never triggers a scan",
          PadShouldScan(1, 0, 0.1, rescan) == 0 &&
          PadShouldScan(1, 0, 999.0, rescan) == 0, "");

    /* Idle pad: allowed, but only on the throttle - that is what lets someone put down
     * pad 0 and pick up pad 1. */
    check("idle pad rescans only on the throttle",
          PadShouldScan(1, 1, 0.5, rescan) == 0 &&
          PadShouldScan(1, 1, 2.5, rescan) == 1, "");

    /* Nothing connected: same throttle, and the very first poll must scan. */
    check("unplugged rescans only on the throttle",
          PadShouldScan(0, 1, 0.5, rescan) == 0 &&
          PadShouldScan(0, 1, 2.5, rescan) == 1, "");
    check("first poll always scans", PadShouldScan(0, 1, 1e9, rescan) == 1, "");

    /* The cost, on the machine this project actually targets: no controller, 144 Hz.
     * The old code walked 0..3 on every GetDeviceState of both wrapped devices. */
    double since = 1e9;
    int scans = 0;
    for (int f = 0; f < 144; ++f) {
        if (PadShouldScan(0, 1, since, rescan)) { scans++; since = 0.0; }
        since += 1.0 / 144.0;
    }
    int now  = scans * XUSER_MAX_COUNT;
    int then = 144 * 2 * XUSER_MAX_COUNT;   /* per frame, per wrapped device, per slot */
    wsprintfA(buf, "%d calls/sec unplugged at 144Hz, was %d", now, then);
    check("unplugged cost is bounded", now <= XUSER_MAX_COUNT, buf);

    /* And while playing, a poll costs exactly one call on the remembered index. */
    scans = 0;
    for (int f = 0; f < 144; ++f) if (PadShouldScan(1, 0, 1e9, rescan)) scans++;
    wsprintfA(buf, "%d scans in a second of play", scans);
    check("no scans at all while playing", scans == 0, buf);
}

/* ------------------------------------------------------------------ event sequencing
 *
 * Synthetic buffered events share a numbering namespace with DirectInput's own, so an
 * application comparing them with DISEQUENCE_COMPARE gets the right order. */
static void TestSequencing(void)
{
    char buf[160];

    printf("\n  -- buffered event sequence numbers --\n");

    /* The comparison itself must be a signed difference so it survives wraparound. */
    check("DISEQUENCE_COMPARE semantics",
          PadSeqAfter(5, 3) && !PadSeqAfter(3, 5) && !PadSeqAfter(4, 4), "");
    check("sequence comparison survives wraparound",
          PadSeqAfter(2, 0xFFFFFFFEuL) && !PadSeqAfter(0xFFFFFFFEuL, 2), "");

    /* Anchoring: a synthetic event must land above every real event already delivered.
     * A private counter starting at 1 - the old behaviour - sorts before all of them. */
    unsigned long real = 500000;
    unsigned long next = PadSeqNext(real, 0);
    wsprintfA(buf, "real=%lu synthetic=%lu", real, next);
    check("synthetic event sorts after a delivered real event",
          PadSeqAfter(next, real), buf);

    /* Our own events stay ordered among themselves. */
    unsigned long last = 0, prev = 0;
    int monotonic = 1;
    for (int i = 0; i < 100; ++i) {
        last = PadSeqNext(real, last);
        if (i && !PadSeqAfter(last, prev)) monotonic = 0;
        prev = last;
    }
    wsprintfA(buf, "%lu after 100 events from anchor %lu", last, real);
    check("synthetic events are monotonic", monotonic, buf);

    /* Re-anchoring: when real input resumes with a much higher number, we jump above it
     * rather than continuing to count from our own stale value. */
    unsigned long newer = 900000;
    unsigned long after = PadSeqNext(newer, last);
    wsprintfA(buf, "last=%lu real=%lu -> %lu", last, newer, after);
    check("re-anchors to a newer real sequence", PadSeqAfter(after, newer), buf);

    /* But a stale anchor must not drag our counter backwards. */
    unsigned long stale = PadSeqNext(1, after);
    wsprintfA(buf, "%lu -> %lu with stale anchor 1", after, stale);
    check("a stale anchor never rewinds the counter", PadSeqAfter(stale, after), buf);
}

/* ------------------------------------------------------------------ failure path
 *
 * The proxy's out-of-memory branch hands the game the real DirectInput and stops adding
 * controller support. That is the right call - failing the call outright would leave the
 * game with no input at all - but from the player's chair it is indistinguishable from
 * the controller component never having been installed, so it has to be announced.
 *
 * dist\oom\dinput8.dll is this same source built with BGEFIX_TEST_OOM, so the wrapper
 * allocation fails on demand. Log=0 here is deliberate: it is the default the installer
 * writes, and a report that only appears once the user has already enabled logging is not
 * a report. BGEFIX_NO_UI suppresses only the message box, which would otherwise hang an
 * unattended run.
 */
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

static void TestOomIsLoud(void)
{
    char dll[MAX_PATH], ini[MAX_PATH], log[MAX_PATH], buf[8192], detail[160];

    printf("\n  -- out of memory: degradation is announced, not silent --\n");

    PathBesideExe(dll, "oom\\dinput8.dll");
    PathBesideExe(ini, "oom\\dinput8_xinput.ini");
    PathBesideExe(log, "oom\\dinput8_xinput.log");

    WriteTextFile(ini, "[General]\r\nLog=0\r\n");
    DeleteFileA(log);
    SetEnvironmentVariableA("BGEFIX_NO_UI", "1");
    SetEnvironmentVariableA("BGEFIX_TEST_OOM_AFTER", "0");   /* fail the first wrapper */

    HMODULE m = LoadLibraryA(dll);
    check("forced-OOM build loads", m != NULL, dll);
    if (!m) return;

    typedef HRESULT (WINAPI *PFN)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    PFN create = (PFN)GetProcAddress(m, "DirectInput8Create");
    IDirectInput8A* di = NULL;
    HRESULT hr = create ? create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                                 IID_IDirectInput8A, (LPVOID*)&di, NULL) : E_FAIL;
    wsprintfA(detail, "hr=0x%08X", (unsigned)hr);
    check("the game still gets a usable IDirectInput8", SUCCEEDED(hr) && di, detail);

    /* Pass-through has to be real pass-through: the game's keyboard must still work. */
    IDirectInputDevice8A* kb = NULL;
    if (di) {
        hr = di->CreateDevice(GUID_SysKeyboard, &kb, NULL);
        wsprintfA(detail, "hr=0x%08X", (unsigned)hr);
        check("the game still gets its keyboard", SUCCEEDED(hr) && kb, detail);
    }

    ReadTextFile(log, buf, sizeof(buf));
    check("the failure is logged even with Log=0",
          strstr(buf, "[degraded]") != NULL, buf[0] ? "" : "no log written at all");
    check("the log says what the user loses",
          strstr(buf, "Controller support is off") != NULL, "");

    if (kb) kb->Release();
    if (di) di->Release();
    FreeLibrary(m);

    SetEnvironmentVariableA("BGEFIX_TEST_OOM_AFTER", NULL);
    SetEnvironmentVariableA("BGEFIX_NO_UI", NULL);
}

int main(int argc, char** argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 0;

    HMODULE m = LoadLibraryA("dinput8.dll");
    check("proxy dinput8.dll loads", m != NULL, "");
    if (!m) return 2;
    char path[MAX_PATH] = {0}; GetModuleFileNameA(m, path, MAX_PATH);
    printf("       loaded: %s\n", path);

    typedef HRESULT (WINAPI *PFN)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    PFN create = (PFN)GetProcAddress(m, "DirectInput8Create");
    check("DirectInput8Create exported", create != NULL, "");
    if (!create) return 2;

    IDirectInput8A* di = NULL;
    HRESULT hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                        IID_IDirectInput8A, (LPVOID*)&di, NULL);
    char buf[160];
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("DirectInput8Create succeeded", SUCCEEDED(hr) && di, buf);
    if (!di) return 2;

    IDirectInputDevice8A* kb = NULL;
    hr = di->CreateDevice(GUID_SysKeyboard, &kb, NULL);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("keyboard device created", SUCCEEDED(hr) && kb, buf);
    if (!kb) return 2;

    /* Keyboard data format: 256 push buttons, one byte each. DIDFT_OPTIONAL is
     * required - no keyboard actually reports all 256 keys. */
    static DIOBJECTDATAFORMAT odf[256];
    for (int i = 0; i < 256; ++i) {
        odf[i].pguid   = &GUID_Key;
        odf[i].dwOfs   = (DWORD)i;
        odf[i].dwType  = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
        odf[i].dwFlags = 0;
    }
    DIDATAFORMAT df;
    df.dwSize     = sizeof(df);
    df.dwObjSize  = sizeof(DIOBJECTDATAFORMAT);
    df.dwFlags    = DIDF_RELAXIS;
    df.dwDataSize = 256;
    df.dwNumObjs  = 256;
    df.rgodf      = odf;

    hr = kb->SetDataFormat(&df);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("SetDataFormat accepted", SUCCEEDED(hr), buf);

    HWND hwnd = GetConsoleWindow();
    if (!hwnd) hwnd = GetDesktopWindow();
    hr = kb->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("SetCooperativeLevel accepted", SUCCEEDED(hr), buf);

    /* Slots far down the device vtable. A forwarding-only test that touched just the
     * first few slots would not notice the wrapper's layout drifting from the real
     * interface, so exercise the deep end deliberately. */
    DIDEVCAPS caps; ZeroMemory(&caps, sizeof(caps)); caps.dwSize = sizeof(caps);
    hr = kb->GetCapabilities(&caps);
    wsprintfA(buf, "hr=0x%08X buttons=%u", (unsigned)hr, caps.dwButtons);
    check("GetCapabilities forwards", SUCCEEDED(hr) && caps.dwButtons > 0, buf);

    DIDEVICEINSTANCEA inst; ZeroMemory(&inst, sizeof(inst)); inst.dwSize = sizeof(inst);
    hr = kb->GetDeviceInfo(&inst);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("GetDeviceInfo forwards", SUCCEEDED(hr), buf);

    hr = kb->Acquire();
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("Acquire succeeded", SUCCEEDED(hr), buf);

    /* Poll is slot 25 of 32 - deep enough that a mis-ordered vtable would land
     * somewhere else entirely. A keyboard returns DI_NOEFFECT rather than DI_OK. */
    hr = kb->Poll();
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("Poll forwards (vtable slot 25)", SUCCEEDED(hr), buf);

    BYTE state[256];
    memset(state, 0, sizeof(state));
    hr = kb->GetDeviceState(sizeof(state), state);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("GetDeviceState succeeded", SUCCEEDED(hr), buf);

    /* Is a pad connected at all? */
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput1_3.dll");
    int connected = 0;
    if (xi) {
        typedef DWORD (WINAPI *XG)(DWORD, XINPUT_STATE*);
        XG xg = (XG)GetProcAddress(xi, "XInputGetState");
        if (xg) {
            XINPUT_STATE st; ZeroMemory(&st, sizeof(st));
            connected = (xg(0, &st) == ERROR_SUCCESS);
        }
    }
    /* Informational, never a failure: a pad is not needed to validate the proxy - the
     * checks above already exercise wrapping, forwarding and vtable layout. Requiring
     * one would make the suite red on any machine with nothing plugged in. */
    printf("  %-4s XInput pad connected on port 0 -> %s\n",
           connected ? "INFO" : "SKIP",
           connected ? "yes" : "none detected, injection not exercised");

    TestStickMaths();
    TestScanPolicy();
    TestSequencing();
    TestOomIsLoud();

    if (seconds > 0) {
        printf("\n  Live for %d seconds - move the sticks / press buttons:\n\n", seconds);
        DWORD end = GetTickCount() + (DWORD)seconds * 1000;
        char last[256] = {0};
        while (GetTickCount() < end) {
            memset(state, 0, sizeof(state));
            if (FAILED(kb->GetDeviceState(sizeof(state), state))) { kb->Acquire(); Sleep(50); continue; }
            char line[256] = {0};
            for (int i = 0; i < 256; ++i) {
                if (!(state[i] & 0x80)) continue;
                char one[24];
                wsprintfA(one, "%s(0x%02X) ", DikName(i), i);
                if (lstrlenA(line) + lstrlenA(one) < 200) lstrcatA(line, one);
            }
            if (lstrcmpA(line, last) != 0) {
                printf("    keys: %s\n", line[0] ? line : "(none)");
                lstrcpyA(last, line);
            }
            Sleep(40);
        }
    }

    kb->Unacquire();
    kb->Release();
    di->Release();
    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
