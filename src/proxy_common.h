/*
 * proxy_common - the plumbing both DLLs in this project need, in one copy.
 *
 * d3d9.dll and dinput8.dll intercept completely different APIs, but they are the same
 * KIND of thing: a DLL that sits in the game folder under a system DLL's name, reads an
 * ini beside itself, writes a log beside itself, carries a marker so the installer can
 * recognise it across rebuilds, and forwards everything it does not intercept to a
 * chained wrapper or to the real implementation in System32.
 *
 * That plumbing was copy-pasted into both .cpp files, and the copies had already started
 * to drift - different log wording for the same event, a comment about SysWOW64
 * redirection in one and not the other, and the shared keys read from a different ini
 * section in each. Two copies means the next fix lands in one of them. It now lives here,
 * and each proxy supplies only the names that must differ, through a ProxyConfig:
 *
 *   baseName      d3d9_windowed        vs  dinput8_xinput      (ini and log file names)
 *   chainDefault  d3d9_chain.dll       vs  dinput8_chain.dll
 *   systemDll     d3d9.dll             vs  dinput8.dll
 *
 * Those three are forced by what the files are called. The ini SECTION is not: the keys
 * defined here are read from [General] in every proxy, so someone who has seen one ini
 * knows where Log and Chain live in the other. A proxy's own settings go in a section
 * named for what they do ([Display], [Buttons], [Sticks]) - never in [General].
 *
 * Header-only and `static`, like pad_support.h: each DLL is one translation unit, so
 * there is nothing to link and no build-system change. The two proxies still get
 * separate copies of the state below - they are separate DLLs and never share a process
 * image.
 *
 * License: MIT.
 */

#ifndef BGEFIX_PROXY_COMMON_H
#define BGEFIX_PROXY_COMMON_H

#include <windows.h>
#include <stdarg.h>

/* wvsprintf supports only c C d i s S u x X - no %p and no %f. Pointers are logged
 * through this cast, which is exact because the DLLs are 32-bit by construction. */
#define PTRV(p) ((unsigned)(UINT_PTR)(p))

/* Lets the installer recognise either DLL as one of ours across rebuilds. Identifying
 * them by file hash instead would make an upgraded build look third-party, and the
 * installer would chain the new proxy to the old one.
 *
 * The GUID is the identity and never changes. The trailing number is a version, and the
 * installer matches the prefix and PARSES it rather than comparing the whole string - if
 * it compared, then bumping this number would make the next installer treat this build as
 * a third-party wrapper and chain to it, which is the very thing the marker prevents.
 *
 * Referenced from ProxyStartup so it is not optimised out of the binary. */
static const char kProxyMarker[] = "BGEFIX_PROXY{502eb6b9-f979-4627-b242-8e146e0fc1de}v2";

/* What one proxy is called. Each .cpp defines one of these as a file-scope constant and
 * hands it to ProxyStartup. */
typedef struct {
    const wchar_t* baseName;      /* L"d3d9_windowed" -> d3d9_windowed.ini / .log */
    const wchar_t* chainDefault;  /* L"d3d9_chain.dll" - a renamed third-party wrapper */
    const wchar_t* systemDll;     /* L"d3d9.dll" - the real implementation to fall back to */
} ProxyConfig;

/* Where the keys this header owns live, in every proxy's ini. */
#define PROXY_SECTION L"General"

/* ------------------------------------------------------------------ shared state */

static const ProxyConfig* g_cfg = NULL;
static HMODULE  g_self  = NULL;
static HMODULE  g_chain = NULL;

/* Covers lazy module loading only. A proxy with more to protect adds its own lock rather
 * than overloading this one; see g_padLock in dinput8_xinput.cpp. */
static CRITICAL_SECTION g_modLock;
static BOOL     g_modLockReady = FALSE;

static BOOL     g_log = FALSE;
static wchar_t  g_iniPath[MAX_PATH] = {0};
static wchar_t  g_logPath[MAX_PATH] = {0};

/* ------------------------------------------------------------------ logging */

/* Appends one line. Unconditional - the Log=0 check belongs to the caller, because a
 * failure report has to be written whether or not the user turned logging on. */
static void ProxyLogWrite(const char* text, int len)
{
    if (!g_logPath[0] || len <= 0) return;

    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, text, (DWORD)len, &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    CloseHandle(h);
}

/* Tracing, for someone who has turned Log=1 on to diagnose something. Silent by default.
 * A failure the user needs to know about is NOT this - see ProxyDegraded. */
static void Log(const char* fmt, ...)
{
    if (!g_log || !g_logPath[0]) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    ProxyLogWrite(buf, n);
}

/* ------------------------------------------------------------------ degradation */

static LONG g_degradedShown = 0;

/*
 * The proxy could not do the thing it was installed to do, but the process can carry on
 * without it. Every such site MUST report through here rather than just returning.
 *
 * Carrying on is usually the right call - a game that starts and misses one feature beats
 * a game that refuses to start - but a behaviour change the user cannot see is not. If
 * the d3d9 proxy quietly gives up, the game goes back to exclusive fullscreen and the HUD
 * corruption this whole tool exists to prevent comes back, looking exactly like the fix
 * simply not working. So both halves of this are load-bearing:
 *
 *   The log line is written even when Log=0. Log defaults to 0, so the old
 *   Log("WARNING ...") at these sites reached nobody; the report has to survive the
 *   default settings or it is not a report.
 *
 *   The first failure per process is also shown as a message box, because nobody reads a
 *   log they do not know exists. Once per process, not once per site: a machine failing
 *   these allocations will fail them repeatedly, and a dialog storm helps no one. Set
 *   BGEFIX_NO_UI=1 in the environment to suppress the box - the log line is still
 *   written, and the tests use it to assert on this path.
 *
 * Never call this from DllMain: MessageBox inside the loader lock can deadlock. Every
 * caller is an export or an interface method, which is well clear of it.
 */
static void ProxyDegraded(const char* fmt, ...)
{
    /* Every buffer here is 1024 bytes because that is wsprintf's documented maximum
     * output - it cannot write more, so none of these can overflow. */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(msg, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    char line[1024];
    int m = wsprintfA(line, "[degraded] %s", msg);
    ProxyLogWrite(line, m);

    if (InterlockedCompareExchange(&g_degradedShown, 1, 0) != 0) return;
    if (GetEnvironmentVariableA("BGEFIX_NO_UI", NULL, 0) != 0) return;

    /* TOPMOST and SETFOREGROUND because a game may already own the screen by the time
     * this fires - the d3d9 sites both run before any fullscreen device exists, but the
     * dinput8 ones can fire whenever the game gets round to creating its input. If the
     * box is still missed, the log line above it is the durable record. */
    char box[1024];
    wsprintfA(box, "%s\r\n\r\nThe game will keep running. Details were written to:\r\n%S",
              msg, g_logPath);
    MessageBoxA(NULL, box, "Beyond Good & Evil fix",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
}

/* ------------------------------------------------------------------ allocation */

/* Every wrapper object goes through these. Two reasons they are not bare HeapAlloc calls:
 * the out-of-memory branch below each of them is the one path that changes what the game
 * does, and it is the one path no ordinary run will ever take - so build_test.cmd builds a
 * second pair of DLLs with BGEFIX_TEST_OOM defined and the tests walk it for real.
 *
 * The shipping build compiles to a plain HeapAlloc; nothing below survives /DNDEBUG
 * without the test define. */
#ifdef BGEFIX_TEST_OOM
static LONG g_allocCount = 0;

/* Allocations to let through before failing everything after them, read from
 * BGEFIX_TEST_OOM_AFTER (default 0 = fail the very first). It takes a number rather than
 * a flag so a test can reach the SECOND allocation site in a call chain: the d3d9 device
 * wrapper is only attempted once the IDirect3D9 wrapper exists. */
static long ProxyTestOomAfter(void)
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("BGEFIX_TEST_OOM_AFTER", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return 0;

    long v = 0;
    for (DWORD i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; ++i)
        v = v * 10 + (buf[i] - '0');
    return v;
}
#endif

static void* ProxyAlloc(SIZE_T bytes)
{
#ifdef BGEFIX_TEST_OOM
    if (InterlockedIncrement(&g_allocCount) > ProxyTestOomAfter()) return NULL;
#endif
    return HeapAlloc(GetProcessHeap(), 0, bytes);
}

static void ProxyFree(void* p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

/* ------------------------------------------------------------------ config */

/* The ini and the log sit beside the DLL, not beside the executable and not in %TEMP%:
 * the installer writes the ini into the game folder and a user looking for the log
 * expects it where they dropped the DLL. */
static void ProxyBuildPaths(void)
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    /* strip to directory */
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; break; }
    }

    lstrcpynW(g_iniPath, path, MAX_PATH);
    lstrcatW(g_iniPath, g_cfg->baseName);
    lstrcatW(g_iniPath, L".ini");

    lstrcpynW(g_logPath, path, MAX_PATH);
    lstrcatW(g_logPath, g_cfg->baseName);
    lstrcatW(g_logPath, L".log");
}

/* The settings every proxy has: [General] Log. Each one's LoadConfig calls this and then
 * reads its own keys from its own sections. */
static void ProxyLoadCommonConfig(void)
{
    if (!g_iniPath[0]) return;
    g_log = GetPrivateProfileIntW(PROXY_SECTION, L"Log", 0, g_iniPath) != 0;
}

/* ------------------------------------------------------------------ chain loading */

/* Never call LoadLibrary from DllMain - this runs lazily on first export use. */
static HMODULE GetChain(void)
{
    if (g_chain) return g_chain;
    if (!g_modLockReady || !g_cfg) return NULL;

    EnterCriticalSection(&g_modLock);
    if (!g_chain) {
        wchar_t self[MAX_PATH], dir[MAX_PATH], cand[MAX_PATH], name[64];

        DWORD len = GetModuleFileNameW(g_self, self, MAX_PATH);
        lstrcpynW(dir, self, MAX_PATH);
        for (DWORD i = len; i > 0; --i) {
            if (dir[i - 1] == L'\\' || dir[i - 1] == L'/') { dir[i] = 0; break; }
        }

        GetPrivateProfileStringW(PROXY_SECTION, L"Chain", g_cfg->chainDefault,
                                 name, 64, g_iniPath);

        /* 1. chain target beside us (a renamed DXVK, dgVoodoo, ReShade, ...) */
        lstrcpynW(cand, dir, MAX_PATH);
        lstrcatW(cand, name);
        if (lstrcmpiW(cand, self) != 0 &&
            GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            g_chain = LoadLibraryW(cand);
            Log("[chain] loaded %S -> 0x%08X", cand, PTRV(g_chain));
        }

        /* 2. the real system implementation. A 32-bit process is redirected to
         * SysWOW64, which is what we want. */
        if (!g_chain) {
            UINT n = GetSystemDirectoryW(cand, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                lstrcatW(cand, L"\\");
                lstrcatW(cand, g_cfg->systemDll);
                if (lstrcmpiW(cand, self) != 0) {
                    g_chain = LoadLibraryW(cand);
                    Log("[chain] loaded %S -> 0x%08X", cand, PTRV(g_chain));
                }
            }
        }
    }
    LeaveCriticalSection(&g_modLock);
    return g_chain;
}

static FARPROC ChainProc(const char* name)
{
    HMODULE m = GetChain();
    return m ? GetProcAddress(m, name) : NULL;
}

/* For a proxy that loads a module of its own (the XInput runtime) and wants it under the
 * same lock as the chain. Returns FALSE if the lock does not exist yet, in which case the
 * caller must do nothing rather than proceed unlocked.
 *
 * `inline` rather than plain `static`: only the dinput8 proxy loads a second module, and
 * an unreferenced static function in a header is C4505, which this build promotes to an
 * error with /WX. */
static inline BOOL ProxyEnterModuleLock(void)
{
    if (!g_modLockReady) return FALSE;
    EnterCriticalSection(&g_modLock);
    return TRUE;
}

static inline void ProxyLeaveModuleLock(void)
{
    LeaveCriticalSection(&g_modLock);
}

/* ------------------------------------------------------------------ entry point */

/* Call first thing from DLL_PROCESS_ATTACH, before any proxy-specific setup: it is what
 * makes g_iniPath usable, so a LoadConfig must follow it, not precede it. Returns FALSE
 * to refuse the load, which DllMain should propagate.
 *
 * Deliberately does no LoadLibrary - that would run inside the loader lock and deadlock.
 * See GetChain(). */
static BOOL ProxyStartup(HINSTANCE inst, const ProxyConfig* cfg)
{
    if (kProxyMarker[0] == 0) return FALSE;   /* keeps the marker in the binary */
    if (!cfg) return FALSE;

    g_cfg  = cfg;
    g_self = (HMODULE)inst;
    DisableThreadLibraryCalls(inst);

    InitializeCriticalSection(&g_modLock);
    g_modLockReady = TRUE;

    ProxyBuildPaths();
    return TRUE;
}

/* Call from DLL_PROCESS_DETACH, and only when the process is not exiting (lpReserved
 * NULL) - on process exit the loader has already torn things down. */
static void ProxyShutdown(void)
{
    if (!g_modLockReady) return;
    g_modLockReady = FALSE;
    DeleteCriticalSection(&g_modLock);
}

#endif /* BGEFIX_PROXY_COMMON_H */
