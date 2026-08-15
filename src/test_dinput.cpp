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
