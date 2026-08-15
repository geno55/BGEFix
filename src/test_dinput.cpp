/*
 * test_dinput - functional test for dinput8_xinput.
 *
 * Drives the proxy exactly the way BGE.exe does: DirectInput8Create, CreateDevice on
 * the system keyboard, SetDataFormat, Acquire, then GetDeviceState into a 256-byte
 * array. Anything the controller injects shows up as set scan codes.
 *
 * Builds the keyboard DIDATAFORMAT by hand so no DirectX SDK is required.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static const GUID kSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kGuidKey =
    { 0x55728220, 0xD33C, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kIID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

typedef struct { const GUID* pguid; DWORD dwOfs; DWORD dwType; DWORD dwFlags; } DIOBJDF;
typedef struct { DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; DIOBJDF* rgodf; } DIDF;

#define DIDFT_BUTTON    0x0000000C   /* PSHBUTTON | TGLBUTTON */
#define DIDFT_OPTIONAL  0x80000000   /* required: no keyboard has all 256 keys */
#define DIDF_RELAXIS    0x00000001
#define DISCL_NONEXCLUSIVE 0x00000002
#define DISCL_BACKGROUND   0x00000008
#define DIRECTINPUT_VERSION 0x0800

struct IDI; struct IDev;
typedef struct {
    void *qi, *ar; ULONG (STDMETHODCALLTYPE *Release)(IDI*);
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(IDI*, const GUID*, IDev**, void*);
} IDIVtbl;
struct IDI { IDIVtbl* v; };

typedef struct {
    void *qi, *ar; ULONG (STDMETHODCALLTYPE *Release)(IDev*);
    void *caps, *enumobj, *getprop, *setprop;
    HRESULT (STDMETHODCALLTYPE *Acquire)(IDev*);
    HRESULT (STDMETHODCALLTYPE *Unacquire)(IDev*);
    HRESULT (STDMETHODCALLTYPE *GetDeviceState)(IDev*, DWORD, void*);
    void *getdata;
    HRESULT (STDMETHODCALLTYPE *SetDataFormat)(IDev*, const DIDF*);
    void *seteventnotif;
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(IDev*, HWND, DWORD);
} IDevVtbl;
struct IDev { IDevVtbl* v; };

static const char* DikName(int c)
{
    switch (c) {
        case 0x11: return "W";     case 0x1E: return "A";
        case 0x1F: return "S";     case 0x20: return "D";
        case 0x39: return "SPACE"; case 0x1D: return "LCTRL";
        case 0x10: return "Q";     case 0x12: return "E";
        case 0x2A: return "LSHIFT";case 0x2E: return "C";
        case 0x01: return "ESC";   case 0x0F: return "TAB";
        case 0x03: return "2";     case 0x04: return "3";
        default:   return "?";
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

    check("DirectInput8Create exported", GetProcAddress(m, "DirectInput8Create") != NULL, "");

    typedef HRESULT (WINAPI *PFN)(HINSTANCE, DWORD, const GUID*, void**, void*);
    PFN create = (PFN)GetProcAddress(m, "DirectInput8Create");
    if (!create) return 2;

    IDI* di = NULL;
    HRESULT hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                        &kIID_IDirectInput8A, (void**)&di, NULL);
    char buf[160];
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("DirectInput8Create succeeded", SUCCEEDED(hr) && di, buf);
    if (!di) return 2;

    IDev* kb = NULL;
    hr = di->v->CreateDevice(di, &kSysKeyboard, &kb, NULL);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("keyboard device created", SUCCEEDED(hr) && kb, buf);
    if (!kb) return 2;

    /* Hand-built keyboard data format: 256 push buttons, one byte each. */
    static DIOBJDF odf[256];
    for (int i = 0; i < 256; ++i) {
        odf[i].pguid  = &kGuidKey;
        odf[i].dwOfs  = (DWORD)i;
        odf[i].dwType = DIDFT_BUTTON | (DWORD)(i << 8) | DIDFT_OPTIONAL;
        odf[i].dwFlags = 0;
    }
    DIDF df;
    df.dwSize = sizeof(DIDF); df.dwObjSize = sizeof(DIOBJDF);
    df.dwFlags = DIDF_RELAXIS; df.dwDataSize = 256; df.dwNumObjs = 256; df.rgodf = odf;

    hr = kb->v->SetDataFormat(kb, &df);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("SetDataFormat accepted", SUCCEEDED(hr), buf);

    HWND hwnd = GetConsoleWindow();
    if (!hwnd) hwnd = GetDesktopWindow();
    hr = kb->v->SetCooperativeLevel(kb, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("SetCooperativeLevel accepted", SUCCEEDED(hr), buf);

    hr = kb->v->Acquire(kb);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("Acquire succeeded", SUCCEEDED(hr), buf);

    BYTE state[256];
    memset(state, 0, sizeof(state));
    hr = kb->v->GetDeviceState(kb, 256, state);
    wsprintfA(buf, "hr=0x%08X", (unsigned)hr);
    check("GetDeviceState succeeded", SUCCEEDED(hr), buf);

    /* Is a pad connected at all? */
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput1_3.dll");
    int connected = 0;
    if (xi) {
        typedef DWORD (WINAPI *XG)(DWORD, void*);
        XG xg = (XG)GetProcAddress(xi, "XInputGetState");
        if (xg) { BYTE st[16]; memset(st, 0, sizeof(st)); connected = (xg(0, st) == ERROR_SUCCESS); }
    }
    check("XInput pad connected on port 0", connected, connected ? "" : "none detected");

    if (seconds > 0) {
        printf("\n  Live for %d seconds - move the sticks / press buttons:\n\n", seconds);
        DWORD end = GetTickCount() + (DWORD)seconds * 1000;
        char last[256] = {0};
        while (GetTickCount() < end) {
            memset(state, 0, sizeof(state));
            if (FAILED(kb->v->GetDeviceState(kb, 256, state))) { kb->v->Acquire(kb); Sleep(50); continue; }
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

    kb->v->Unacquire(kb);
    kb->v->Release(kb);
    di->v->Release(di);
    printf("\n  %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
