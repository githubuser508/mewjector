/*
 * version.dll — DLL proxy chainloader
 *
 * Drop this as version.dll next to the game executable along with
 * chainloader.ini. On load it:
 *   1. Forwards all 17 version.dll exports to the real system version.dll
 *   2. Reads chainloader.ini for configuration
 *   3. Scans configured directories for .dll mod files
 *   4. Optionally reads a Mewtator manifest for managed mod DLLs
 *   5. Logs everything to mod_logs/chainloader.log
 *
 * Build (MSVC):
 *   cl /LD /O2 /GS- version.c /Fe:version.dll /link /DEF:version.def
 *
 * The .def file controls the export table so the game finds our forwarded
 * functions by name, exactly matching the real version.dll.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>      /* EnumProcessModules, GetModuleInformation for the crash handler */
#include <stdio.h>
#include <io.h>      /* _fileno, _get_osfhandle for raw HANDLE logging */
#include <string.h>
#include "mj_lde.h"

/* Mewjector API version (used by LoadModsOnce and the API itself) */
#define MJ_API_VERSION 3

/* Forward declarations and early globals for MJ API
 * (full API defined further below, but LoadModsOnce needs these) */
__declspec(dllexport) int __cdecl MJ_VerifyHooks(void);
static UINT_PTR g_mjGameBase    = 0;
static int      g_mjHasHooks    = 0;  /* set by MJ_InstallHook, checked post-load */

/* ===================================================================
 *  Game base directory (resolved once, used everywhere)
 * =================================================================== */

static char g_baseDir[MAX_PATH];  /* Ends with backslash */

static void ResolveBaseDir(void)
{
    GetModuleFileNameA(NULL, g_baseDir, MAX_PATH);
    char* slash = strrchr(g_baseDir, '\\');
    if (slash) *(slash + 1) = '\0';
    else strcpy(g_baseDir, ".\\");
}

/* ===================================================================
 *  Logging — writes to mod_logs/chainloader.log
 * =================================================================== */

static FILE*  g_logFile   = NULL;
static HANDLE g_logMutex  = NULL;
static HANDLE g_logHandle = INVALID_HANDLE_VALUE; /* raw HANDLE for WriteFile; avoids CRT FILE-stream lock */

static void LogOpen(void)
{
    char logDir[MAX_PATH];
    char logPath[MAX_PATH];

    snprintf(logDir, MAX_PATH, "%smod_logs", g_baseDir);
    CreateDirectoryA(logDir, NULL);

    snprintf(logPath, MAX_PATH, "%s\\chainloader.log", logDir);

    g_logFile  = fopen(logPath, "w");
    g_logMutex = CreateMutexA(NULL, FALSE, NULL);
    /* Cache the underlying Win32 HANDLE so CLog can use WriteFile directly,
     * bypassing the CRT FILE-stream lock.  This prevents a deadlock when a
     * background thread is suspended while it holds the FILE-stream lock. */
    if (g_logFile)
        g_logHandle = (HANDLE)(intptr_t)_get_osfhandle(_fileno(g_logFile));
}

static void LogClose(void)
{
    /* Clear g_logHandle BEFORE fclose so that CLog calls from other
     * threads see INVALID_HANDLE_VALUE and bail out.  Do NOT CloseHandle
     * it — the same kernel handle is owned by g_logFile and will be
     * released by fclose. */
    g_logHandle = INVALID_HANDLE_VALUE;
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
    if (g_logMutex) { CloseHandle(g_logMutex); g_logMutex = NULL; }
}

static void CLog(const char* fmt, ...)
{
    if (g_logHandle == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n > 0 && n < (int)sizeof(buf)) {
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
        va_end(ap);
        if (m > 0) n += m;
    }
    if (n > 0 && n + 1 < (int)sizeof(buf))
        buf[n++] = '\n';

    /* Try-acquire the mutex (0ms timeout).  If it is held by a suspended
     * thread, skip the wait and fall through to an unguarded WriteFile —
     * two concurrent unguarded writes may produce a garbled log line in
     * extreme cases, but the process will never deadlock.
     * WriteFile bypasses the CRT FILE-stream lock entirely, so it succeeds
     * even when a thread is frozen mid-fprintf holding that lock. */
    BOOL held = (g_logMutex &&
                 WaitForSingleObject(g_logMutex, 0) == WAIT_OBJECT_0);
    DWORD written;
    WriteFile(g_logHandle, buf, (DWORD)n, &written, NULL);
    if (held) ReleaseMutex(g_logMutex);
}

/* ===================================================================
 *  INI configuration
 * =================================================================== */

#define MAX_LOAD_ORDER 64

typedef struct {
    int  enabled;            /* Master switch                            */
    int  logging;            /* Whether to log (already open by now)     */
    char scanPath[MAX_PATH]; /* Directory to scan for DLLs (relative)    */
    int  scanGameDir;        /* Also scan the exe's own directory        */
    char manifest[MAX_PATH]; /* Mewtator manifest path (abs or relative) */

    /* [LoadOrder] — priority DLLs loaded first, in listed order */
    char loadOrder[MAX_LOAD_ORDER][MAX_PATH];
    int  loadOrderCount;
} ChainloaderConfig;

static ChainloaderConfig g_config;

/* Trim leading/trailing whitespace + quotes in-place */
static void TrimInPlace(char* s)
{
    /* Leading */
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '"') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* Trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
           s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == '"'))
        s[--len] = '\0';
}

static void LoadConfig(void)
{
    char iniPath[MAX_PATH];
    char buf[MAX_PATH];

    /* Defaults — these apply if no ini exists */
    g_config.enabled        = 1;
    g_config.logging        = 1;
    g_config.scanGameDir    = 0;
    g_config.loadOrderCount = 0;
    strcpy(g_config.scanPath, "mods");
    g_config.manifest[0] = '\0';

    snprintf(iniPath, MAX_PATH, "%schainloader.ini", g_baseDir);

    /* Check if ini exists */
    DWORD attr = GetFileAttributesA(iniPath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        CLog("No chainloader.ini found — using defaults");
        CLog("  ScanPath=%s, ScanGameDir=%d",
             g_config.scanPath, g_config.scanGameDir);
        return;
    }

    CLog("Loading config from %s", iniPath);

    /* Enabled */
    GetPrivateProfileStringA("Chainloader", "Enabled", "1",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.enabled = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* Logging */
    GetPrivateProfileStringA("Chainloader", "Logging", "1",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.logging = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* ScanPath */
    GetPrivateProfileStringA("Chainloader", "ScanPath", "mods",
                             g_config.scanPath, sizeof(g_config.scanPath),
                             iniPath);
    TrimInPlace(g_config.scanPath);

    /* ScanGameDir */
    GetPrivateProfileStringA("Chainloader", "ScanGameDir", "0",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.scanGameDir = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* MewtatorManifest */
    GetPrivateProfileStringA("Chainloader", "MewtatorManifest", "",
                             g_config.manifest, sizeof(g_config.manifest),
                             iniPath);
    TrimInPlace(g_config.manifest);

    CLog("  Enabled=%d, Logging=%d, ScanPath=%s, ScanGameDir=%d",
         g_config.enabled, g_config.logging,
         g_config.scanPath, g_config.scanGameDir);
    if (g_config.manifest[0])
        CLog("  MewtatorManifest=%s", g_config.manifest);
    else
        CLog("  MewtatorManifest=(none)");

    /* [LoadOrder] — numbered entries: Mod1=foo.dll, Mod2=bar.dll, ... */
    g_config.loadOrderCount = 0;
    for (int i = 1; i <= MAX_LOAD_ORDER; i++) {
        char key[16];
        snprintf(key, sizeof(key), "Mod%d", i);
        GetPrivateProfileStringA("LoadOrder", key, "",
                                 buf, sizeof(buf), iniPath);
        TrimInPlace(buf);
        if (buf[0] == '\0') break;  /* Stop at first missing entry */
        strncpy(g_config.loadOrder[g_config.loadOrderCount], buf, MAX_PATH - 1);
        g_config.loadOrder[g_config.loadOrderCount][MAX_PATH - 1] = '\0';
        g_config.loadOrderCount++;
    }

    if (g_config.loadOrderCount > 0) {
        CLog("  LoadOrder: %d entries", g_config.loadOrderCount);
        for (int i = 0; i < g_config.loadOrderCount; i++)
            CLog("    Mod%d=%s", i + 1, g_config.loadOrder[i]);
    } else {
        CLog("  LoadOrder: (none)");
    }
}

/* ===================================================================
 *  Real version.dll forwarding
 * =================================================================== */

static HMODULE g_realVersionDll = NULL;

/* Function pointers for all 17 exports */
static FARPROC pfn_GetFileVersionInfoA        = NULL;
static FARPROC pfn_GetFileVersionInfoByHandle  = NULL;
static FARPROC pfn_GetFileVersionInfoExA       = NULL;
static FARPROC pfn_GetFileVersionInfoExW       = NULL;
static FARPROC pfn_GetFileVersionInfoSizeA     = NULL;
static FARPROC pfn_GetFileVersionInfoSizeExA   = NULL;
static FARPROC pfn_GetFileVersionInfoSizeExW   = NULL;
static FARPROC pfn_GetFileVersionInfoSizeW     = NULL;
static FARPROC pfn_GetFileVersionInfoW         = NULL;
static FARPROC pfn_VerFindFileA                = NULL;
static FARPROC pfn_VerFindFileW                = NULL;
static FARPROC pfn_VerInstallFileA             = NULL;
static FARPROC pfn_VerInstallFileW             = NULL;
static FARPROC pfn_VerLanguageNameA            = NULL;
static FARPROC pfn_VerLanguageNameW            = NULL;
static FARPROC pfn_VerQueryValueA              = NULL;
static FARPROC pfn_VerQueryValueW              = NULL;

static int LoadRealVersionDll(void)
{
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    strcat(sysDir, "\\version.dll");

    g_realVersionDll = LoadLibraryA(sysDir);
    if (!g_realVersionDll) {
        CLog("FATAL: Could not load real version.dll from %s (error %lu)",
             sysDir, GetLastError());
        return 0;
    }

    CLog("Loaded real version.dll from %s", sysDir);

    #define RESOLVE(name) \
        pfn_##name = GetProcAddress(g_realVersionDll, #name); \
        if (!pfn_##name) CLog("  WARNING: Could not resolve " #name);

    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoByHandle);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerLanguageNameA);
    RESOLVE(VerLanguageNameW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);

    #undef RESOLVE

    return 1;
}

/* ===================================================================
 *  Deferred mod loading — triggered on first proxy call
 *
 *  We cannot load mod DLLs inside DllMain because the Windows loader
 *  lock is held, which prevents any threads those DLLs spawn from
 *  starting.  Instead, we defer mod loading until the game makes its
 *  first call to any version.dll export.  At that point the loader
 *  lock is released, we're in normal execution context, and it's
 *  early enough that hooks install before anything important runs.
 * =================================================================== */

/* Forward declarations for functions/data defined further below */
static void ScanAndLoadDir(const char* dirPath, const char* label);
static void LoadMewtatorManifest(const char* manifestPath);
static int  AlreadyLoaded(const char* fullPath);
static void RecordLoaded(const char* fullPath);
static int  g_loadedCount;

static volatile LONG g_modsLoaded  = 0;
static HANDLE        g_modsLoadDone = NULL;  /* manual-reset event; signalled when LoadModsOnce completes */
static void LoadModsOnce(void);   /* forward decl for entry-point fallback */
static void StartCrashWatchdog(void);  /* forward decl; body in crash-handler block */

/* --- Entry-point trampoline fallback ---
 *
 * PURELY A SAFETY NET.  On most builds, a version.dll proxy call fires
 * early and triggers LoadModsOnce() long before the CRT entry point
 * runs.  This fallback exists for the rare case where NO proxy export
 * is ever called (some builds never hit a version.dll export).
 *
 * Strategy: during DLL_PROCESS_ATTACH we patch the game's PE entry
 * point (AddressOfEntryPoint, i.e. mainCRTStartup) with a small
 * trampoline that calls a handler, then jumps back to the *original*
 * entry point.  The handler calls LoadModsOnce(), then restores the
 * original entry-point bytes before returning.  This way the
 * trampoline never executes stolen prologue bytes at a relocated
 * address, avoiding RIP-relative fixup issues entirely.
 *
 * Because the entry point has not been called yet at DllMain time,
 * and the trampoline only executes once the loader lock is fully
 * released, this avoids the deadlock issues that plague
 * CreateThread-based fallbacks.
 *
 * If a proxy call already loaded mods, the InterlockedCompareExchange
 * guard in LoadModsOnce() makes the handler a near-no-op (it still
 * restores the entry-point bytes, which is harmless).
 *
 * TODO: The restore-and-jump approach avoids the RIP-relative problem
 * entirely but means we can't chain hooks on the entry point.  A more
 * complete fix would be to detect RIP-relative operands in the stolen
 * bytes and rewrite their displacements in the trampoline, matching
 * the approach MJ_CreateSite already uses for E8/E9 instructions.
 *
 * The trampoline layout (x86-64):
 *
 *   sub  rsp, 0x28                     ; shadow space + alignment
 *   mov  rax, <EntryPointHandler>      ; absolute 64-bit address
 *   call rax                           ; load mods + restore bytes
 *   add  rsp, 0x28
 *   jmp  [rip+0]                       ; jump to restored entry point
 *   <8-byte entry point address>
 */

/* Saved state for restoring the entry point after the trampoline fires */
static uint8_t* g_epFallbackAddr       = NULL;
static uint8_t  g_epFallbackOrigBytes[24];
static int      g_epFallbackStolenCount = 0;

/* Handles of threads suspended in EntryPointHandler to prevent the
 * CRT-init race.  EpResumeWatcher resumes them after mainCRTStartup has
 * had time to initialize the CRT (heap, critical sections, atexit). */
/* Hardcoded RVA of the CRT critical section that TLS-callback threads
 * try to acquire before mainCRTStartup initialises it.  Identified from
 * VEH null-rgn diagnostics (RCX = game_base + 0x13A8268).
 * Breaks if the game binary updates and the CS moves. */
#define MJ_GAME_CRTCS_RVA  0x13A8268UL

/* First CALL rel32 found in the stolen entry-point bytes, resolved to an
 * absolute function pointer.  In MSVC mainCRTStartup this is always
 * __security_init_cookie, which must run before any mod thread is created
 * (see EntryPointHandler).  NULL if no CALL was found in the prologue. */
static void (__cdecl *g_epCrtInitFn)(void) = NULL;

/* TLS slot used as a per-thread re-entrancy counter for MjVectoredFilter.
 * Win32 explicit TLS slots 0-63 are stored directly in the TEB (no heap
 * allocation), so TlsGetValue/TlsSetValue are safe to call from inside a
 * VEH or while handling a nested exception.  TLS_OUT_OF_INDEXES means the
 * slot was not allocated (should never happen in practice). */
static DWORD g_vehTlsSlot = TLS_OUT_OF_INDEXES;

/* Calls LoadModsOnce() on a dedicated thread so EntryPointHandler can
 * return immediately and let mainCRTStartup initialise the game's CRT
 * (including the CS at game+MJ_GAME_CRTCS_RVA that TLS-callback threads
 * spin on).  Without this, the full mod-load time (~40 ms) delays
 * mainCRTStartup long enough for those threads to exhaust their 1 MB
 * stacks -- each CS retry uses ~0x1100 bytes of stack, ~230 retries
 * fill 1 MB. */
static DWORD WINAPI EpDeferredLoader(LPVOID param)
{
    (void)param;
    LoadModsOnce();
    return 0;
}

static void EntryPointHandler(void)
{
    /* Log whether LoadModsOnce already ran via a proxy export.  If
     * g_modsLoaded is non-zero here, a proxy call beat the entry
     * point and the fallback trampoline is just cleaning up. */
    CLog("[EP-fallback] Handler invoked (g_modsLoaded=%ld).",
         (long)g_modsLoaded);

    /* __security_init_cookie was already called under the loader lock in
     * PatchEntryPointFallback (see comment there for the full explanation).
     * This call is now a no-op: __security_init_cookie returns immediately
     * when the cookie is already set.  Kept as a safety net for the case
     * where PatchEntryPointFallback found no CALL rel32 in the prologue. */
    if (g_epCrtInitFn)
        g_epCrtInitFn(); /* safety net: __security_init_cookie (already called under loader lock) */

    /* Spawn a deferred loader so this handler returns immediately and lets
     * mainCRTStartup initialise the game's CRT (especially the CS at
     * game+MJ_GAME_CRTCS_RVA).  Proxy calls that fire before loading is
     * complete will block inside LoadModsOnce() on g_modsLoadDone. */
    {
        HANDLE hLoader = CreateThread(NULL, 0, EpDeferredLoader, NULL, 0, NULL);
        if (hLoader) {
            CloseHandle(hLoader);
            CLog("[EP-fallback] Async mod-loader spawned.");
        } else {
            CLog("[EP-fallback] WARNING: CreateThread failed (err=%lu); "
                 "loading mods synchronously.", GetLastError());
            LoadModsOnce();
        }
    }

    /* Restore the original entry-point bytes so the jmp back executes
     * the real prologue in place -- no RIP-relative relocation needed. */
    if (g_epFallbackAddr && g_epFallbackStolenCount > 0) {
        DWORD oldProt;
        VirtualProtect(g_epFallbackAddr, g_epFallbackStolenCount,
                       PAGE_EXECUTE_READWRITE, &oldProt);
        memcpy(g_epFallbackAddr, g_epFallbackOrigBytes,
               g_epFallbackStolenCount);
        VirtualProtect(g_epFallbackAddr, g_epFallbackStolenCount,
                       oldProt, &oldProt);
    }
    CLog("[EP-fallback] Entry point restored; returning to trampoline.");
}

static int PatchEntryPointFallback(void)
{
    /* Locate the game's PE entry point */
    UINT_PTR imageBase = (UINT_PTR)GetModuleHandleA(NULL);
    if (!imageBase) return 0;

    const uint8_t* dosHeader = (const uint8_t*)imageBase;
    DWORD peOffset = *(const DWORD*)(dosHeader + 0x3C);
    const uint8_t* peHeader = dosHeader + peOffset;

    /* Skip PE signature (4) + COFF header (20) to reach OptionalHeader */
    const uint8_t* optHeader = peHeader + 4 + 20;
    WORD optMagic = *(const WORD*)optHeader;
    if (optMagic != 0x20B) {
        CLog("  Fallback: not PE32+ (magic 0x%04X), skipping.", optMagic);
        return 0;
    }

    DWORD entryRVA = *(const DWORD*)(optHeader + 16);  /* AddressOfEntryPoint */
    if (entryRVA == 0) {
        CLog("  Fallback: no entry point, skipping.");
        return 0;
    }

    uint8_t* entryAddr = (uint8_t*)(imageBase + entryRVA);

    /* Walk instructions at the entry point to find a clean cut >= 14 bytes */
    int stolenBytes = 0;
    const uint8_t* walk = entryAddr;
    while (stolenBytes < 14) {
        int len = mj_lde(walk);
        if (len == 0) {
            CLog("  Fallback: LDE failed at entry+%d, cannot patch.", stolenBytes);
            return 0;
        }
        walk        += len;
        stolenBytes += len;
    }

    /* Save the original bytes so EntryPointHandler can restore them */
    g_epFallbackAddr        = entryAddr;
    g_epFallbackStolenCount = stolenBytes;
    memcpy(g_epFallbackOrigBytes, entryAddr, stolenBytes);

    /* Locate the first CALL rel32 (E8) in the stolen bytes and save its
     * absolute target.  In MSVC mainCRTStartup this is always the call to
     * __security_init_cookie, which must be invoked before mod DLLs are
     * loaded (see EntryPointHandler for the full explanation). */
    g_epCrtInitFn = NULL;
    for (int i = 0; i + 4 < stolenBytes; i++) {
        if (g_epFallbackOrigBytes[i] == 0xE8) {
            int32_t rel32;
            memcpy(&rel32, g_epFallbackOrigBytes + i + 1, 4);
            /* Instruction is at entryAddr+i; next insn is entryAddr+i+5 */
            g_epCrtInitFn = (void (__cdecl *)(void))
                ((uint8_t*)entryAddr + i + 5 + rel32);
            CLog("  Fallback: CALL at prologue+%d -> CRT init fn 0x%p",
                 i, (void*)(UINT_PTR)g_epCrtInitFn);
            /* Call immediately while still inside DllMain (loader lock held).
             * The loader lock prevents background threads that run game code
             * (e.g. TID 18900 observed in crash logs) from entering
             * GS-protected game functions before __security_cookie is set.
             * Race that caused the no-mods __fastfail crash:
             *   - cookie init deferred to EntryPointHandler (~10 us after
             *     lock release) → background thread enters GS-protected fn
             *     with old placeholder cookie → epilogue check fails →
             *     __report_gsfailure → __fastfail (invisible to VEH/SEH).
             *   - cookie init here (under lock) → no background thread can
             *     execute game code yet → no race. */
            g_epCrtInitFn();
            CLog("  Fallback: CRT init fn called under loader lock.");
            break;
        }
    }
    if (!g_epCrtInitFn)
        CLog("  Fallback: no CALL rel32 in stolen bytes; CRT init pre-call skipped.");

    /* Trampoline: 20 (call wrapper) + 14 (jmp to entry start)
     * No stolen bytes -- the handler restores them before we jump back. */
    int tramSize = 20 + 14;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, tramSize,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        CLog("  Fallback: VirtualAlloc failed, cannot patch.");
        g_epFallbackAddr = NULL;
        return 0;
    }

    int off = 0;

    /* sub rsp, 0x28 */
    tramp[off++] = 0x48; tramp[off++] = 0x83;
    tramp[off++] = 0xEC; tramp[off++] = 0x28;

    /* mov rax, <EntryPointHandler> */
    tramp[off++] = 0x48; tramp[off++] = 0xB8;
    UINT_PTR fnAddr = (UINT_PTR)&EntryPointHandler;
    memcpy(tramp + off, &fnAddr, 8);
    off += 8;

    /* call rax */
    tramp[off++] = 0xFF; tramp[off++] = 0xD0;

    /* add rsp, 0x28 */
    tramp[off++] = 0x48; tramp[off++] = 0x83;
    tramp[off++] = 0xC4; tramp[off++] = 0x28;

    /* jmp [rip+0] back to entry point start (now restored to original) */
    tramp[off++] = 0xFF; tramp[off++] = 0x25;
    tramp[off++] = 0x00; tramp[off++] = 0x00;
    tramp[off++] = 0x00; tramp[off++] = 0x00;
    UINT_PTR retAddr = (UINT_PTR)entryAddr;  /* start, not +stolenBytes */
    memcpy(tramp + off, &retAddr, 8);

    /* Overwrite the entry point: jmp [rip+0] to trampoline, NOP remainder */
    DWORD oldProt;
    VirtualProtect(entryAddr, stolenBytes, PAGE_EXECUTE_READWRITE, &oldProt);

    entryAddr[0] = 0xFF;
    entryAddr[1] = 0x25;
    entryAddr[2] = 0x00;
    entryAddr[3] = 0x00;
    entryAddr[4] = 0x00;
    entryAddr[5] = 0x00;
    memcpy(entryAddr + 6, &tramp, 8);
    for (int i = 14; i < stolenBytes; i++)
        entryAddr[i] = 0x90;

    VirtualProtect(entryAddr, stolenBytes, oldProt, &oldProt);

    CLog("  Fallback: entry point patched at RVA=0x%X stolen=%d trampoline=0x%p.",
         entryRVA, stolenBytes, (void*)tramp);

    return 1;
}

static void LoadModsOnce(void)
{
    if (InterlockedCompareExchange(&g_modsLoaded, 1, 0) != 0) {
        /* Another thread won the race and is loading (or already done).
         * Wait for the done event so proxy callers don't proceed before
         * mods are fully initialised. */
        if (g_modsLoadDone)
            WaitForSingleObject(g_modsLoadDone, 15000);
        return;
    }

    /* Actually disable when Enabled=0 says we're disabled. */
    if (!g_config.enabled) {
        if (g_modsLoadDone) SetEvent(g_modsLoadDone);
        return;
    }

    /* Resolve game image base for RVA calculations */
    g_mjGameBase = (UINT_PTR)GetModuleHandleA(NULL);

    CLog("First proxy call — loading mods (loader lock released)");
    CLog("Game image base: 0x%p", (void*)g_mjGameBase);
    CLog("Mewjector API v%d available (MJ_InstallHook, MJ_RegisterName, ...)",
         MJ_API_VERSION);
    CLog("");

    /* Start the crash-handler watchdog now that the loader lock is
     * released. Idempotent. See StartCrashWatchdog for why it cannot
     * run from DllMain. */
    StartCrashWatchdog();

    /* Phase 1: [LoadOrder] priority DLLs — loaded first, in listed order */
    if (g_config.loadOrderCount > 0) {
        CLog("[LoadOrder] Loading %d priority DLL(s)...", g_config.loadOrderCount);

        /* Resolve the scan directory once for relative paths */
        char modDir[MAX_PATH];
        if (g_config.scanPath[0]) {
            if (g_config.scanPath[0] != '\\' &&
                !(g_config.scanPath[1] == ':')) {
                snprintf(modDir, MAX_PATH, "%s%s", g_baseDir, g_config.scanPath);
            } else {
                strncpy(modDir, g_config.scanPath, MAX_PATH - 1);
                modDir[MAX_PATH - 1] = '\0';
            }
        } else {
            strncpy(modDir, g_baseDir, MAX_PATH - 1);
            modDir[MAX_PATH - 1] = '\0';
        }

        for (int i = 0; i < g_config.loadOrderCount; i++) {
            const char* entry = g_config.loadOrder[i];
            char fullPath[MAX_PATH];

            /* Absolute or relative path */
            if (entry[0] == '\\' || (entry[1] == ':')) {
                strncpy(fullPath, entry, MAX_PATH - 1);
                fullPath[MAX_PATH - 1] = '\0';
            } else {
                snprintf(fullPath, MAX_PATH, "%s\\%s", modDir, entry);
            }

            if (AlreadyLoaded(fullPath)) {
                CLog("[LoadOrder]   %d. SKIP (already loaded): %s", i + 1, entry);
                continue;
            }

            CLog("[LoadOrder]   %d. Loading: %s", i + 1, entry);
            HMODULE hMod = LoadLibraryA(fullPath);
            if (hMod) {
                CLog("[LoadOrder]      OK at 0x%p", (void*)hMod);
                RecordLoaded(fullPath);
            } else {
                DWORD err = GetLastError();
                CLog("[LoadOrder]      FAILED — error %lu (0x%08lX)", err, err);
            }
        }
        CLog("");
    }

    /* Phase 2: Scan configured mod directory */
    if (g_config.scanPath[0]) {
        char scanDir[MAX_PATH];
        if (g_config.scanPath[0] != '\\' &&
            !(g_config.scanPath[1] == ':')) {
            snprintf(scanDir, MAX_PATH, "%s%s", g_baseDir, g_config.scanPath);
        } else {
            strncpy(scanDir, g_config.scanPath, MAX_PATH - 1);
            scanDir[MAX_PATH - 1] = '\0';
        }
        ScanAndLoadDir(scanDir, "ScanPath");
        CLog("");
    }

    /* Phase 3: Optionally scan game exe directory */
    if (g_config.scanGameDir) {
        char gameDir[MAX_PATH];
        strncpy(gameDir, g_baseDir, MAX_PATH - 1);
        gameDir[MAX_PATH - 1] = '\0';
        size_t gdLen = strlen(gameDir);
        if (gdLen > 0 && gameDir[gdLen-1] == '\\')
            gameDir[gdLen-1] = '\0';
        ScanAndLoadDir(gameDir, "GameDir");
        CLog("");
    }

    /* Phase 4: Mewtator manifest */
    if (g_config.manifest[0]) {
        LoadMewtatorManifest(g_config.manifest);
        CLog("");
    }

    CLog("=== Mod loading complete: %d DLL(s) loaded ===",
         g_loadedCount);

    /* Post-load hook integrity verification */
    if (g_mjHasHooks) {
        CLog("");
        CLog("[MJ] Running post-load integrity check...");
        int corrupted = MJ_VerifyHooks();
        if (corrupted > 0)
            CLog("[MJ] WARNING: %d hook site(s) may have been "
                 "overwritten by non-Mewjector mods!", corrupted);
    }

    /* Signal the done event so any thread waiting in LoadModsOnce()
     * (e.g. a proxy call that fired while we were loading) can proceed. */
    if (g_modsLoadDone) SetEvent(g_modsLoadDone);
}


/* ===================================================================
 *  Exported forwarding stubs
 *
 *  Each is a thin wrapper that tail-calls the real function.
 *  The .def file maps the real export names to these Proxy_ functions.
 *  Every stub calls LoadModsOnce() to trigger deferred mod loading
 *  on the first call.
 *
 * =================================================================== */

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoA(
    LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(LPCSTR, DWORD, DWORD, LPVOID);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoA) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoA)(lptstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) int WINAPI Proxy_GetFileVersionInfoByHandle(
    int hMem, LPCWSTR lpFileName, HANDLE handle, LPVOID lpData, DWORD cbData)
{
    typedef int (WINAPI *fn_t)(int, LPCWSTR, HANDLE, LPVOID, DWORD);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoByHandle) return 0;
    return ((fn_t)pfn_GetFileVersionInfoByHandle)(hMem, lpFileName, handle, lpData, cbData);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoExA(
    DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoExA) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoExA)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoExW(
    DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoExW) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoExW)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeA(
    LPCSTR lptstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(LPCSTR, LPDWORD);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoSizeA) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeA)(lptstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(
    DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPDWORD);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoSizeExA) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeExA)(dwFlags, lpwstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(
    DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPDWORD);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoSizeExW) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeExW)(dwFlags, lpwstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeW(
    LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(LPCWSTR, LPDWORD);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoSizeW) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeW)(lptstrFilename, lpdwHandle);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoW(
    LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(LPCWSTR, DWORD, DWORD, LPVOID);
    LoadModsOnce();
    if (!pfn_GetFileVersionInfoW) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoW)(lptstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerFindFileA(
    DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir,
    LPSTR szCurDir, PUINT puCurDirLen, LPSTR szDestDir, PUINT puDestDirLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    LoadModsOnce();
    if (!pfn_VerFindFileA) return 0;
    return ((fn_t)pfn_VerFindFileA)(uFlags, szFileName, szWinDir, szAppDir,
                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerFindFileW(
    DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir,
    LPWSTR szCurDir, PUINT puCurDirLen, LPWSTR szDestDir, PUINT puDestDirLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    LoadModsOnce();
    if (!pfn_VerFindFileW) return 0;
    return ((fn_t)pfn_VerFindFileW)(uFlags, szFileName, szWinDir, szAppDir,
                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerInstallFileA(
    DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir,
    LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT puTmpFileLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    LoadModsOnce();
    if (!pfn_VerInstallFileA) return 0;
    return ((fn_t)pfn_VerInstallFileA)(uFlags, szSrcFileName, szDestFileName, szSrcDir,
                                        szDestDir, szCurDir, szTmpFile, puTmpFileLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerInstallFileW(
    DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir,
    LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT puTmpFileLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    LoadModsOnce();
    if (!pfn_VerInstallFileW) return 0;
    return ((fn_t)pfn_VerInstallFileW)(uFlags, szSrcFileName, szDestFileName, szSrcDir,
                                        szDestDir, szCurDir, szTmpFile, puTmpFileLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPSTR, DWORD);
    LoadModsOnce();
    if (!pfn_VerLanguageNameA) return 0;
    return ((fn_t)pfn_VerLanguageNameA)(wLang, szLang, cchLang);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPWSTR, DWORD);
    LoadModsOnce();
    if (!pfn_VerLanguageNameW) return 0;
    return ((fn_t)pfn_VerLanguageNameW)(wLang, szLang, cchLang);
}

__declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueA(
    LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    typedef BOOL (WINAPI *fn_t)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    LoadModsOnce();
    if (!pfn_VerQueryValueA) return FALSE;
    return ((fn_t)pfn_VerQueryValueA)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

__declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueW(
    LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    typedef BOOL (WINAPI *fn_t)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    LoadModsOnce();
    if (!pfn_VerQueryValueW) return FALSE;
    return ((fn_t)pfn_VerQueryValueW)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

/* ===================================================================
 *  Mod DLL scanning and loading
 * =================================================================== */

/* DLLs to skip — case-insensitive comparison */
static const char* g_excludeList[] = {
    "version.dll",            /* ourselves                          */
    "steam_api.dll",          /* Steam                              */
    "steam_api64.dll",        /* Steam 64-bit                       */
    "steamclient.dll",        /* Steam client                       */
    "steamclient64.dll",      /* Steam client 64-bit                */
    "tier0_s.dll",            /* Source/Steam tier                   */
    "tier0_s64.dll",
    "vstdlib_s.dll",          /* Valve stdlib                       */
    "vstdlib_s64.dll",
    "gameoverlayrenderer.dll",
    "gameoverlayrenderer64.dll",
    "d3dcompiler_47.dll",     /* DirectX runtime                    */
    "xaudio2_9.dll",
    "xinput1_3.dll",
    "xinput1_4.dll",
    "xinput9_1_0.dll",
    NULL
};

static int IsExcluded(const char* filename)
{
    for (int i = 0; g_excludeList[i] != NULL; i++) {
        if (_stricmp(filename, g_excludeList[i]) == 0)
            return 1;
    }
    return 0;
}

/* Track loaded DLLs to avoid double-loading from multiple sources */
#define MAX_LOADED 256
static char g_loadedDlls[MAX_LOADED][MAX_PATH];
static int  g_loadedCount = 0;

static int AlreadyLoaded(const char* fullPath)
{
    for (int i = 0; i < g_loadedCount; i++) {
        if (_stricmp(g_loadedDlls[i], fullPath) == 0)
            return 1;
    }
    return 0;
}

static void RecordLoaded(const char* fullPath)
{
    if (g_loadedCount < MAX_LOADED) {
        strncpy(g_loadedDlls[g_loadedCount], fullPath, MAX_PATH - 1);
        g_loadedDlls[g_loadedCount][MAX_PATH - 1] = '\0';
        g_loadedCount++;
    }
}

/* Scan a directory for .dll files and load them */
static void ScanAndLoadDir(const char* dirPath, const char* label)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*.dll", dirPath);

    CLog("[%s] Scanning: %s", label, dirPath);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            CLog("[%s]   No .dll files found", label);
        else if (err == ERROR_PATH_NOT_FOUND)
            CLog("[%s]   Directory does not exist", label);
        else
            CLog("[%s]   FindFirstFile error %lu", label, err);
        return;
    }

    int loaded = 0, skipped = 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (IsExcluded(fd.cFileName)) {
            CLog("[%s]   SKIP (excluded): %s", label, fd.cFileName);
            skipped++;
            continue;
        }

        char fullPath[MAX_PATH];
        snprintf(fullPath, MAX_PATH, "%s\\%s", dirPath, fd.cFileName);

        if (AlreadyLoaded(fullPath)) {
            CLog("[%s]   SKIP (already loaded): %s", label, fd.cFileName);
            skipped++;
            continue;
        }

        CLog("[%s]   Loading: %s", label, fd.cFileName);

        HMODULE hMod = LoadLibraryA(fullPath);
        if (hMod) {
            CLog("[%s]     OK at 0x%p", label, (void*)hMod);
            RecordLoaded(fullPath);
            loaded++;
        } else {
            DWORD err = GetLastError();
            CLog("[%s]     FAILED — error %lu (0x%08lX)", label, err, err);
        }

    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    CLog("[%s]   Done: %d loaded, %d skipped", label, loaded, skipped);
}

/* ===================================================================
 *  Mewtator manifest loading
 * =================================================================== */

static void LoadMewtatorManifest(const char* manifestPath)
{
    char resolvedPath[MAX_PATH];

    /* Resolve relative paths against game directory */
    if (manifestPath[0] != '\\' && !(manifestPath[1] == ':')) {
        snprintf(resolvedPath, MAX_PATH, "%s%s", g_baseDir, manifestPath);
    } else {
        strncpy(resolvedPath, manifestPath, MAX_PATH - 1);
        resolvedPath[MAX_PATH - 1] = '\0';
    }

    CLog("[Mewtator] Reading manifest: %s", resolvedPath);

    FILE* f = fopen(resolvedPath, "r");
    if (!f) {
        CLog("[Mewtator]   File not found or unreadable — skipping");
        return;
    }

    char line[MAX_PATH];
    int loaded = 0, skipped = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#')
            continue;

        TrimInPlace(line);
        len = strlen(line);

        /* Validate: must end in .dll */
        if (len < 5 || _stricmp(line + len - 4, ".dll") != 0) {
            CLog("[Mewtator]   SKIP (not .dll): %s", line);
            skipped++;
            continue;
        }

        /* Check file exists */
        DWORD attr = GetFileAttributesA(line);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            CLog("[Mewtator]   SKIP (not found): %s", line);
            skipped++;
            continue;
        }

        if (AlreadyLoaded(line)) {
            CLog("[Mewtator]   SKIP (already loaded): %s", line);
            skipped++;
            continue;
        }

        CLog("[Mewtator]   Loading: %s", line);

        HMODULE hMod = LoadLibraryA(line);
        if (hMod) {
            CLog("[Mewtator]     OK at 0x%p", (void*)hMod);
            RecordLoaded(line);
            loaded++;
        } else {
            DWORD err = GetLastError();
            CLog("[Mewtator]     FAILED — error %lu (0x%08lX)", err, err);
        }
    }

    fclose(f);
    CLog("[Mewtator]   Done: %d loaded, %d skipped", loaded, skipped);
}

/* ===================================================================
 *  Mewjector API v3: Exported services for mod coordination
 *
 *  Mods resolve these at runtime via:
 *    HMODULE hMJ = GetModuleHandleA("version.dll");
 *    fn = GetProcAddress(hMJ, "MJ_InstallHook");
 *
 *  Or include mewjector.h for convenience wrappers.
 *
 *  Services:
 *    Hook chaining:   multiple mods can hook the same RVA safely
 *    Type ID alloc:   unique custom type index pairs, no collisions
 *    Name registry:   detect namespace collisions at init time
 *    Integrity check: verify no third party overwrote managed hooks
 *    Shared logging:  mods can log through the chainloader's log
 * =================================================================== */

/* MJ_API_VERSION is defined near the top of the file (forward decl block) */
#define MJ_MAX_NAMES    512
#define MJ_TYPE_ID_BASE 0x1000  /* Well above vanilla range (0–0x318) */

/* --- Hook chaining structures --- */

typedef struct MJ_HookEntry {
    void*  hookFn;              /* Mod's replacement function               */
    BYTE*  trampoline;          /* Executable JMP stub -> next hook/original*/
    int    priority;            /* Lower = called first                     */
    char   owner[64];           /* DLL name for diagnostics                 */
    struct MJ_HookEntry* next;  /* Next in priority-sorted chain            */
} MJ_HookEntry;

typedef struct MJ_HookSite {
    UINT_PTR         rva;            /* Game function RVA                   */
    int              stolenBytes;    /* Bytes replaced at entry             */
    BYTE*            patchAddr;      /* Absolute address of patched entry   */
    BYTE*            origTrampoline; /* Stolen bytes + JMP back to original */
    BYTE             origBytes[24];  /* Backup of original bytes (for verify) */
    MJ_HookEntry*    chain;          /* Head of priority-sorted hook list   */
    struct MJ_HookSite* nextSite;    /* Next in global site list            */
} MJ_HookSite;

/* --- Namespace registry --- */

typedef struct {
    char category[32];   /* e.g. "status", "formula_var", "conditional" */
    char name[128];      /* e.g. "ModRegen", "inbreeding"              */
    char owner[64];      /* e.g. "CustomStatusFramework"               */
} MJ_NameEntry;

/* --- Mewjector API globals --- */
/* g_mjGameBase is defined early (line ~30) so LoadModsOnce can use it */

static MJ_HookSite*  g_mjHookSites  = NULL;
static HANDLE        g_mjHookMutex  = NULL;

static MJ_NameEntry  g_mjNames[MJ_MAX_NAMES];
static int           g_mjNameCount  = 0;
static HANDLE        g_mjNameMutex  = NULL;

static volatile LONG g_mjNextTypeId = MJ_TYPE_ID_BASE;

/* Crash handler. SEH top-level filter (MjCrashFilter) catches unhandled
 * exceptions. VEH (MjVectoredFilter) at priority 0 catches fatal codes
 * that other filters might absorb. Watchdog re-installs the SEH filter
 * every 2s. Reports written to mod_logs\crashes\<pid>-<timestamp>.{txt,dmp}.
 * Write helpers use no heap and no CRT I/O, only WriteFile on stack
 * buffers. Each subsection wrapped in __try so handler faults do not
 * recurse. */

static void CrashWriteHookList(HANDLE hFile);

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = NULL;
static PVOID                        g_vectoredHandlerReg  = NULL;
static volatile LONG                g_crashHandlerEntered = 0;
/* Caps the near-null AV extended-diagnostics block (registers + stack
 * walk) to at most 8 dumps per run so it never floods the log. */
static volatile LONG                g_nullAvDiagCount     = 0;

/* MiniDumpWriteDump signature for dbghelp.dll, loaded dynamically. */
typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(
    HANDLE hProcess, DWORD pid, HANDLE hFile,
    DWORD dumpType, PVOID exceptionParam,
    PVOID userStreamParam, PVOID callbackParam);

/* MINIDUMP_EXCEPTION_INFORMATION layout, defined inline to avoid pulling in dbghelp.h. */
typedef struct {
    DWORD            ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL             ClientPointers;
} MJ_MINIDUMP_EXCEPTION_INFORMATION;

/* Minimal MiniDumpType flags we care about. */
#define MJ_MiniDumpWithDataSegs              0x00000001
#define MJ_MiniDumpWithFullMemory            0x00000002
#define MJ_MiniDumpWithHandleData            0x00000004
#define MJ_MiniDumpWithThreadInfo            0x00001000
#define MJ_MiniDumpWithUnloadedModules       0x00000020
#define MJ_MiniDumpWithIndirectlyReferencedMemory 0x00000040

static void CrashWriteStr(HANDLE hFile, const char* s)
{
    if (!s) return;
    DWORD len = (DWORD)strlen(s);
    DWORD written = 0;
    WriteFile(hFile, s, len, &written, NULL);
}

/* snprintf-to-buffer-and-WriteFile helper. Buffer is stack-local so we
 * never hit the crashing heap. */
#define CRASHF(hFile, ...) do { \
    char _cbuf[1024]; \
    int _cn = _snprintf(_cbuf, sizeof(_cbuf), __VA_ARGS__); \
    if (_cn < 0 || _cn >= (int)sizeof(_cbuf)) _cn = (int)sizeof(_cbuf) - 1; \
    DWORD _cw = 0; \
    WriteFile((hFile), _cbuf, (DWORD)_cn, &_cw, NULL); \
} while (0)

static const char* CrashExceptionName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        case 0xE0434352:                          return "CLR_MANAGED_EXCEPTION";
        case 0xE06D7363:                          return "CPP_EH_EXCEPTION";
        default:                                  return "UNKNOWN";
    }
}

/* Resolve an instruction address to "modulename.dll + 0xRVA". Returns TRUE
 * if the address falls inside a loaded module, FALSE if it's stray. */
static BOOL CrashResolveAddr(UINT_PTR addr, char* outModule, size_t outSize,
                             UINT_PTR* outRva)
{
    HMODULE mods[512];
    DWORD   needed = 0;
    HANDLE  hProc  = GetCurrentProcess();

    if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed))
        return FALSE;

    int count = (int)(needed / sizeof(HMODULE));
    if (count > 512) count = 512;

    for (int i = 0; i < count; i++) {
        MODULEINFO mi;
        if (!GetModuleInformation(hProc, mods[i], &mi, sizeof(mi)))
            continue;
        UINT_PTR base = (UINT_PTR)mi.lpBaseOfDll;
        UINT_PTR end  = base + mi.SizeOfImage;
        if (addr < base || addr >= end) continue;

        char path[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], path, MAX_PATH)) {
            strncpy(outModule, "?", outSize);
            outModule[outSize - 1] = '\0';
        } else {
            const char* leaf = strrchr(path, '\\');
            leaf = leaf ? (leaf + 1) : path;
            strncpy(outModule, leaf, outSize);
            outModule[outSize - 1] = '\0';
        }
        *outRva = addr - base;
        return TRUE;
    }
    strncpy(outModule, "?", outSize);
    outModule[outSize - 1] = '\0';
    *outRva = 0;
    return FALSE;
}

static void CrashWriteHeader(HANDLE hFile, EXCEPTION_POINTERS* ep, SYSTEMTIME* stamp)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;

    CrashWriteStr(hFile, "=== Mewjector crash report ===\r\n");
    CRASHF(hFile, "PID:            %lu\r\n",
           (unsigned long)GetCurrentProcessId());
    CRASHF(hFile, "Thread ID:      %lu\r\n",
           (unsigned long)GetCurrentThreadId());
    CRASHF(hFile, "Timestamp:      %04d-%02d-%02d %02d:%02d:%02d.%03d (local)\r\n",
           stamp->wYear, stamp->wMonth, stamp->wDay,
           stamp->wHour, stamp->wMinute, stamp->wSecond, stamp->wMilliseconds);
    CRASHF(hFile, "Exception code: 0x%08lX (%s)\r\n",
           (unsigned long)er->ExceptionCode, CrashExceptionName(er->ExceptionCode));
    CRASHF(hFile, "Exception flag: 0x%08lX\r\n",
           (unsigned long)er->ExceptionFlags);
    CRASHF(hFile, "Fault address:  0x%p\r\n", er->ExceptionAddress);

    char mod[128]; UINT_PTR rva = 0;
    if (CrashResolveAddr((UINT_PTR)er->ExceptionAddress, mod, sizeof(mod), &rva))
        CRASHF(hFile, "Fault location: %s + 0x%IX\r\n", mod, rva);
    else
        CrashWriteStr(hFile, "Fault location: (unresolved address)\r\n");

    /* AV/In-page-error details: the NumberParameters[] describes the op. */
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
        && er->NumberParameters >= 2) {
        const char* kind;
        switch (er->ExceptionInformation[0]) {
            case 0: kind = "read";    break;
            case 1: kind = "write";   break;
            case 8: kind = "execute"; break;
            default: kind = "?";       break;
        }
        CRASHF(hFile, "AV detail:      %s at 0x%p\r\n",
               kind, (void*)er->ExceptionInformation[1]);
    }
    CrashWriteStr(hFile, "\r\n");
}

static void CrashWriteRegisters(HANDLE hFile, CONTEXT* ctx)
{
#ifdef _M_X64
    CrashWriteStr(hFile, "--- Registers (x64) ---\r\n");
    CRASHF(hFile, "RIP=%016llX  RSP=%016llX  RBP=%016llX\r\n",
           (unsigned long long)ctx->Rip,
           (unsigned long long)ctx->Rsp,
           (unsigned long long)ctx->Rbp);
    CRASHF(hFile, "RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX\r\n",
           (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
           (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    CRASHF(hFile, "RSI=%016llX  RDI=%016llX  R8 =%016llX  R9 =%016llX\r\n",
           (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
           (unsigned long long)ctx->R8,  (unsigned long long)ctx->R9);
    CRASHF(hFile, "R10=%016llX  R11=%016llX  R12=%016llX  R13=%016llX\r\n",
           (unsigned long long)ctx->R10, (unsigned long long)ctx->R11,
           (unsigned long long)ctx->R12, (unsigned long long)ctx->R13);
    CRASHF(hFile, "R14=%016llX  R15=%016llX  EFL=%08lX\r\n",
           (unsigned long long)ctx->R14, (unsigned long long)ctx->R15,
           (unsigned long)ctx->EFlags);
#else
    CrashWriteStr(hFile, "--- Registers (x86) ---\r\n");
    CRASHF(hFile, "EIP=%08lX  ESP=%08lX  EBP=%08lX  EFL=%08lX\r\n",
           (unsigned long)ctx->Eip, (unsigned long)ctx->Esp,
           (unsigned long)ctx->Ebp, (unsigned long)ctx->EFlags);
    CRASHF(hFile, "EAX=%08lX  EBX=%08lX  ECX=%08lX  EDX=%08lX\r\n",
           (unsigned long)ctx->Eax, (unsigned long)ctx->Ebx,
           (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx);
    CRASHF(hFile, "ESI=%08lX  EDI=%08lX\r\n",
           (unsigned long)ctx->Esi, (unsigned long)ctx->Edi);
#endif
    CrashWriteStr(hFile, "\r\n");
}

/* Quick stack backtrace: dump the first N pointer-sized words at RSP
 * and resolve each one that looks like a code address. No StackWalk64
 * is used here. The backtrace path avoids pulling in dbghelp. dbghelp
 * is loaded dynamically only for the minidump. This will not perfectly
 * reconstruct unwind but gives useful return-address breadcrumbs. */
static void CrashWriteBacktrace(HANDLE hFile, CONTEXT* ctx)
{
    CrashWriteStr(hFile, "--- Approximate backtrace (raw stack walk) ---\r\n");
#ifdef _M_X64
    UINT_PTR rsp = (UINT_PTR)ctx->Rsp;
#else
    UINT_PTR rsp = (UINT_PTR)ctx->Esp;
#endif
    /* Probe up to 256 slots looking for pointers into loaded modules. */
    __try {
        int printed = 0;
        for (int i = 0; i < 256 && printed < 32; i++) {
            UINT_PTR slot = rsp + i * sizeof(UINT_PTR);
            UINT_PTR val  = *(UINT_PTR*)slot;

            /* Coarse filter: must look like a user-mode code pointer. */
            if (val < 0x10000 || val > 0x00007FFFFFFFFFFFULL) continue;

            char mod[128]; UINT_PTR rva = 0;
            if (!CrashResolveAddr(val, mod, sizeof(mod), &rva)) continue;

            CRASHF(hFile, "  [%02d] sp+%04X  0x%016llX  %s + 0x%IX\r\n",
                   printed, (unsigned)(i * sizeof(UINT_PTR)),
                   (unsigned long long)val, mod, rva);
            printed++;
        }
        if (printed == 0)
            CrashWriteStr(hFile, "  (no resolvable return addresses in 256-slot window)\r\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CrashWriteStr(hFile, "  (stack probe faulted; state is unrecoverable)\r\n");
    }
    CrashWriteStr(hFile, "\r\n");
}

static void CrashWriteModuleList(HANDLE hFile)
{
    CrashWriteStr(hFile, "--- Loaded modules ---\r\n");
    HMODULE mods[512];
    DWORD   needed = 0;
    HANDLE  hProc  = GetCurrentProcess();
    if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed)) {
        CrashWriteStr(hFile, "  (EnumProcessModules failed)\r\n\r\n");
        return;
    }
    int count = (int)(needed / sizeof(HMODULE));
    if (count > 512) count = 512;

    for (int i = 0; i < count; i++) {
        MODULEINFO mi; memset(&mi, 0, sizeof(mi));
        GetModuleInformation(hProc, mods[i], &mi, sizeof(mi));
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        CRASHF(hFile, "  0x%016llX  size=0x%08lX  %s\r\n",
               (unsigned long long)(UINT_PTR)mi.lpBaseOfDll,
               (unsigned long)mi.SizeOfImage,
               path);
    }
    CrashWriteStr(hFile, "\r\n");
}

/* Walk the Mewjector hook chain and dump every site + entry. Called from
 * the top-level exception filter. No mutex acquisition. The process may
 * be mid-crash with the mutex forever held, so a best-effort walk inside
 * __try/__except is preferred over deadlocking. */
static void CrashWriteHookList(HANDLE hFile)
{
    CrashWriteStr(hFile, "--- Mewjector installed hooks ---\r\n");
    MJ_HookSite* s = g_mjHookSites;
    if (!s) {
        CrashWriteStr(hFile, "  (no hooks installed)\r\n\r\n");
        return;
    }
    __try {
        int siteIdx = 0;
        while (s && siteIdx < 256) {
            CRASHF(hFile, "  site[%d] RVA=0x%IX  patchAddr=0x%p  stolen=%d\r\n",
                   siteIdx, s->rva, s->patchAddr, s->stolenBytes);
            MJ_HookEntry* e = s->chain;
            int entIdx = 0;
            while (e && entIdx < 32) {
                CRASHF(hFile, "    [%d] fn=0x%p  pri=%d  owner=%s\r\n",
                       entIdx, e->hookFn, e->priority,
                       e->owner[0] ? e->owner : "(anon)");
                e = e->next;
                entIdx++;
            }
            s = s->nextSite;
            siteIdx++;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CrashWriteStr(hFile, "  (hook-chain walk faulted)\r\n");
    }
    CrashWriteStr(hFile, "\r\n");
}

/* Context passed to the minidump helper thread. The faulting thread is
 * NOT the helper. It is the thread WriteCrashReport ran on. The helper
 * suspends and captures the faulting thread per the MiniDumpWriteDump
 * contract. */
typedef struct {
    EXCEPTION_POINTERS* ep;
    DWORD               crashedThreadId;   /* the thread that took the AV */
    const char*         dumpPath;
    BOOL                success;           /* set by helper */
    DWORD               winErr;            /* GetLastError() if !success */
    DWORD               bytesWritten;      /* GetFileSize after write */
} MJ_MiniDumpHelperCtx;

static DWORD WINAPI MiniDumpHelperThread(LPVOID lpParam)
{
    MJ_MiniDumpHelperCtx* ctx = (MJ_MiniDumpHelperCtx*)lpParam;
    ctx->success = FALSE;
    ctx->winErr  = 0;
    ctx->bytesWritten = 0;

    HMODULE hDbg = LoadLibraryA("dbghelp.dll");
    if (!hDbg) { ctx->winErr = GetLastError(); return 0; }

    PFN_MiniDumpWriteDump pWrite = (PFN_MiniDumpWriteDump)
        GetProcAddress(hDbg, "MiniDumpWriteDump");
    if (!pWrite) { ctx->winErr = GetLastError(); FreeLibrary(hDbg); return 0; }

    /* GENERIC_READ | GENERIC_WRITE: dbghelp internally re-reads its own
     * writes to compute stream offsets. WRITE-only file handles caused
     * 0-byte dumps in the 25628-20260425-224319 incident. */
    HANDLE hDump = CreateFileA(ctx->dumpPath,
                               GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDump == INVALID_HANDLE_VALUE) {
        ctx->winErr = GetLastError();
        FreeLibrary(hDbg);
        return 0;
    }

    MJ_MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId          = ctx->crashedThreadId;  /* NOT GetCurrentThreadId */
    mei.ExceptionPointers = ctx->ep;
    mei.ClientPointers    = FALSE;

    DWORD type = MJ_MiniDumpWithDataSegs
               | MJ_MiniDumpWithHandleData
               | MJ_MiniDumpWithThreadInfo
               | MJ_MiniDumpWithUnloadedModules
               | MJ_MiniDumpWithIndirectlyReferencedMemory;

    BOOL ok = pWrite(GetCurrentProcess(), GetCurrentProcessId(),
                     hDump, type, &mei, NULL, NULL);
    if (!ok) ctx->winErr = GetLastError();

    /* Capture file size BEFORE closing so we can audit empty-write bugs. */
    DWORD sizeHi = 0;
    DWORD sizeLo = GetFileSize(hDump, &sizeHi);
    if (sizeLo != INVALID_FILE_SIZE) ctx->bytesWritten = sizeLo;

    CloseHandle(hDump);
    FreeLibrary(hDbg);

    ctx->success = ok && (ctx->bytesWritten > 0);
    return 0;
}

static void CrashWriteMinidump(EXCEPTION_POINTERS* ep,
                               const char* crashDir, const char* stamp)
{
    /* MiniDumpWriteDump must be called from a separate thread. Called
     * inline on the crashing thread it cannot capture the faulting
     * thread's context, and silently produces a 0-byte file. The helper
     * thread pattern is the documented Microsoft recommendation:
     *   https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
     */

    char dumpPath[MAX_PATH];
    _snprintf(dumpPath, MAX_PATH, "%s\\%lu-%s.dmp",
              crashDir, (unsigned long)GetCurrentProcessId(), stamp);

    MJ_MiniDumpHelperCtx ctx;
    ctx.ep              = ep;
    ctx.crashedThreadId = GetCurrentThreadId();
    ctx.dumpPath        = dumpPath;
    ctx.success         = FALSE;
    ctx.winErr          = 0;
    ctx.bytesWritten    = 0;

    HANDLE hThread = CreateThread(NULL, 0, MiniDumpHelperThread, &ctx,
                                  0, NULL);
    if (hThread == NULL) {
        if (g_logFile) {
            CLog("[crash] CreateThread for minidump helper failed: err=%lu",
                 (unsigned long)GetLastError());
            fflush(g_logFile);
        }
        return;
    }

    /* 30s budget: a full memory snapshot of a 21 MB exe + game heap is
     * typically < 1s, but disk pressure or AV scanners can stall. */
    DWORD waitRes = WaitForSingleObject(hThread, 30000);
    if (waitRes == WAIT_TIMEOUT) {
        if (g_logFile) {
            CLog("[crash] minidump helper timed out after 30s; abandoning");
            fflush(g_logFile);
        }
        /* Do not kill the thread. It is holding dbghelp internal locks.
         * Leak the handle and let the OS clean up at process exit. */
        return;
    }
    CloseHandle(hThread);

    if (g_logFile) {
        CLog("[crash] minidump: success=%d bytes=%lu winErr=%lu path=%s",
             ctx.success ? 1 : 0,
             (unsigned long)ctx.bytesWritten,
             (unsigned long)ctx.winErr,
             dumpPath);
        fflush(g_logFile);
    }
}

/* Shared writer: called from both the SEH top-level filter and from the
 * VEH fallback. 'source' is a short tag that goes into the report and
 * the chainloader log to identify which path fired. Returns TRUE if a
 * report was written, FALSE if re-entered (another handler is already
 * writing). */
static BOOL WriteCrashReport(EXCEPTION_POINTERS* ep, const char* source)
{
    /* Re-entrancy guard: if the filter itself faults and re-enters, or
     * if both VEH and SEH fire for the same event, get out of the way. */
    if (InterlockedCompareExchange(&g_crashHandlerEntered, 1, 0) != 0)
        return FALSE;

    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32];
    _snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    /* Ensure the crash directory exists. Both mod_logs and its crashes
     * child may need to be created; CreateDirectoryA is idempotent. */
    {
        char parent[MAX_PATH];
        _snprintf(parent, MAX_PATH, "%smod_logs", g_baseDir);
        CreateDirectoryA(parent, NULL);
    }
    char crashDir[MAX_PATH];
    _snprintf(crashDir, MAX_PATH, "%smod_logs\\crashes", g_baseDir);
    CreateDirectoryA(crashDir, NULL);

    char txtPath[MAX_PATH];
    _snprintf(txtPath, MAX_PATH, "%s\\%lu-%s.txt",
              crashDir, (unsigned long)GetCurrentProcessId(), stamp);

    HANDLE hFile = CreateFileA(txtPath,
                               GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile != INVALID_HANDLE_VALUE) {
        CRASHF(hFile, "Source:         %s\r\n", source ? source : "(?)");
        __try { CrashWriteHeader   (hFile, ep, &st); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try { CrashWriteRegisters(hFile, ep->ContextRecord); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try { CrashWriteBacktrace(hFile, ep->ContextRecord); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try { CrashWriteModuleList(hFile); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try { CrashWriteHookList (hFile); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        CrashWriteStr(hFile, "=== end of crash report ===\r\n");
        CloseHandle(hFile);
    } else {
        /* Explain missing crash files in chainloader.log. */
        if (g_logFile) {
            __try {
                CLog("[crash] WriteCrashReport: CreateFileA failed GLE=%lu path=%s",
                     (unsigned long)GetLastError(), txtPath);
                fflush(g_logFile);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    /* Minidump: separate __try because MiniDumpWriteDump can itself
     * fault on very broken processes. */
    __try { CrashWriteMinidump(ep, crashDir, stamp); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    /* Flush the normal chainloader log so whatever CLog output preceded
     * the crash is on disk. Also log an audit entry so the chainloader
     * log carries a breadcrumb. Useful when the crash report file itself
     * fails to reach disk (bad path, locked handle, etc.). */
    if (g_logFile) {
        __try {
            CLog("[crash] WriteCrashReport fired: source=%s code=0x%08lX @ %p -> %s",
                 source ? source : "(?)",
                 (unsigned long)ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress,
                 (hFile != INVALID_HANDLE_VALUE) ? txtPath : "(file write failed)");
            fflush(g_logFile);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    return TRUE;
}

static LONG WINAPI MjCrashFilter(EXCEPTION_POINTERS* ep)
{
    /* Log entry before WriteCrashReport so the breadcrumb survives even
     * if the crash-file write itself fails (bad path, permissions, etc.). */
    if (g_logFile) {
        __try {
            CLog("[crash] MjCrashFilter: code=0x%08lX addr=%p",
                 (unsigned long)ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress);
            fflush(g_logFile);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    WriteCrashReport(ep, "SEH-top-level");

    /* Chain to the previous filter (WER, debugger, anything the game
     * may have installed itself). Returning EXCEPTION_CONTINUE_SEARCH
     * if there's no previous filter lets the default OS handling
     * proceed (WER pops up, process exits, etc.). */
    return g_prevUnhandledFilter
        ? g_prevUnhandledFilter(ep)
        : EXCEPTION_CONTINUE_SEARCH;
}

/* VEH: fires before SEH unwinding. Filtered to fatal-only codes plus
 * NONCONTINUABLE to avoid spamming reports for routine throw/catch.
 * Always returns CONTINUE_SEARCH so other handlers still run. */
static LONG WINAPI MjVectoredFilter(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code  = ep->ExceptionRecord->ExceptionCode;
    DWORD flags = ep->ExceptionRecord->ExceptionFlags;

    /* Determine fatality BEFORE the re-entrancy check so that a fatal
     * exception that fires while we are already inside CLog on this thread
     * (guard==1) is never silently dropped -- it still gets a crash report,
     * just without the log line (to avoid the recursive-AV chain). */
    BOOL fatal = FALSE;
    switch (code) {
        case EXCEPTION_STACK_OVERFLOW:      /* always fatal */
        case 0xC0000409:                    /* STATUS_STACK_BUFFER_OVERRUN / __fastfail */
        case 0xC000041D:                    /* STATUS_FATAL_USER_CALLBACK_EXCEPTION */
            fatal = TRUE;
            break;
        default:
            break;
    }
    /* A handler that declared itself non-continuable is by definition
     * unrecoverable -- EXCEPT for C++ EH exceptions (0xE06D7363): the MSVC
     * runtime always sets EXCEPTION_NONCONTINUABLE on C++ throws to signal
     * ownership of the dispatch frame, not that the process is dying.
     * Steam (and other DLLs) throw and catch their own C++ exceptions
     * internally; treating those as fatal produces spurious crash reports. */
    if ((flags & EXCEPTION_NONCONTINUABLE) && code != 0xE06D7363) fatal = TRUE;

    /* Re-entrancy guard: if CLog (or any other code below) causes an
     * exception on this thread the VEH fires again.  Without this guard
     * the chain MjVectoredFilter->CLog->AV->MjVectoredFilter->... unwinds
     * the stack (STACK_OVERFLOW, 0xC00000FD).
     * TlsGetValue/TlsSetValue are safe here: explicit TLS slots 0-63 live
     * directly in the TEB and never allocate or throw.
     * For NON-fatal re-entrant exceptions we return immediately (they are
     * routine continuable AVs from ntdll TLS initialization).
     * For FATAL re-entrant exceptions we skip the CLog but still write the
     * crash report so the event is never silently discarded. */
    BOOL reentrant = FALSE;
    if (g_vehTlsSlot != TLS_OUT_OF_INDEXES) {
        if ((ULONG_PTR)TlsGetValue(g_vehTlsSlot) != 0) {
            reentrant = TRUE;
            if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
            /* fatal + reentrant: fall through to WriteCrashReport, skip log */
        } else {
            TlsSetValue(g_vehTlsSlot, (PVOID)1);
        }
    }

    /* Log every exception that isn't routine control-flow noise:
     * guard-page probe (0x80000001), breakpoint (0x80000003),
     * single-step (0x80000004).  C++ EH (0xE06D7363) is logged so
     * unhandled C++ throws are visible, but they are never marked fatal.
     * Skip logging when re-entrant to avoid the recursive-AV chain. */
    if (!reentrant &&
        code != 0x80000001 &&
        code != 0x80000003 &&
        code != 0x80000004) {
        /* For access violations also log the access type (0=read 1=write 8=DEP)
         * and the faulting VA -- distinguishes stack probes from real faults. */
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            CLog("[VEH] tid=%lu code=0x%08lX flags=0x%lX addr=%p type=%lu va=%p fatal=%d",
                 (unsigned long)GetCurrentThreadId(),
                 (unsigned long)code, (unsigned long)flags,
                 ep->ExceptionRecord->ExceptionAddress,
                 (unsigned long)ep->ExceptionRecord->ExceptionInformation[0],
                 (void*)ep->ExceptionRecord->ExceptionInformation[1],
                 (int)fatal);
        } else {
            CLog("[VEH] tid=%lu code=0x%08lX flags=0x%lX addr=%p fatal=%d",
                 (unsigned long)GetCurrentThreadId(),
                 (unsigned long)code, (unsigned long)flags,
                 ep->ExceptionRecord->ExceptionAddress, (int)fatal);
        }

        /* Extended diagnostics for near-null-pointer AVs.
         * va < 0x10000 is the signature of the CRT-init race: a background
         * thread reads/writes through a CRT pointer that mainCRTStartup has
         * not yet initialised.  Dump registers + fault-module + raw stack
         * walk so the call chain is visible without a minidump.  Capped at
         * g_nullAvDiagCount <= 8 to avoid flooding the log. */
        if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->NumberParameters >= 2
            && ep->ExceptionRecord->ExceptionInformation[1] < 0x10000
            && ep->ContextRecord
            && InterlockedIncrement(&g_nullAvDiagCount) <= 8) {
            __try {
                CONTEXT* ctx = ep->ContextRecord;
                CLog("[VEH] null-rgn diag: RIP=%016llX RCX=%016llX RDX=%016llX",
                     (unsigned long long)ctx->Rip,
                     (unsigned long long)ctx->Rcx,
                     (unsigned long long)ctx->Rdx);
                CLog("[VEH] null-rgn diag: RAX=%016llX RSP=%016llX RBP=%016llX",
                     (unsigned long long)ctx->Rax,
                     (unsigned long long)ctx->Rsp,
                     (unsigned long long)ctx->Rbp);
                /* Resolve faulting instruction to module + RVA so we
                 * know which system DLL (ucrtbase, ntdll, etc.) faulted. */
                {
                    char fmod[128]; UINT_PTR frva = 0;
                    if (CrashResolveAddr(
                            (UINT_PTR)ep->ExceptionRecord->ExceptionAddress,
                            fmod, sizeof(fmod), &frva))
                        CLog("[VEH] null-rgn diag: fault in %s+0x%IX",
                             fmod, frva);
                }
                /* Raw stack walk: first 10 resolvable return addresses.
                 * Map raw addresses to functions offline via IDA or WinDbg
                 * if needed; module+RVA resolution is done here for
                 * immediate readability. */
                {
                    UINT_PTR rsp = ctx->Rsp;
                    char smod[128]; UINT_PTR srva = 0;
                    int n = 0;
                    for (int si = 0; si < 96 && n < 10; si++) {
                        UINT_PTR val = *(UINT_PTR*)(rsp + (UINT_PTR)si * 8);
                        if (val >= 0x10000 && val <= 0x7FFFFFFFFFFFULL
                            && CrashResolveAddr(val, smod, sizeof(smod), &srva))
                            CLog("[VEH] null-rgn stk[%02d] %s+0x%IX",
                                 n++, smod, srva);
                    }
                    if (n == 0)
                        CLog("[VEH] null-rgn diag: no resolvable frames in "
                             "96-slot window");
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                CLog("[VEH] null-rgn diag: faulted while collecting "
                     "diagnostics");
            }
        }
    }

    /* Release guard before WriteCrashReport so any exception inside it
     * (DbgHelp, file I/O) can be logged normally rather than silently
     * dropped as re-entrant. */
    if (!reentrant && g_vehTlsSlot != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_vehTlsSlot, (PVOID)0);

    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;

    WriteCrashReport(ep, "VEH-fatal");
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Watchdog thread. The game (or the CLR, or any other late-loaded DLL)
 * can call SetUnhandledExceptionFilter() after MjCrashFilter is
 * installed, displacing it from the top of the chain. The watchdog
 * periodically re-installs MjCrashFilter and captures whatever was
 * displaced, so MjCrashFilter chains into it and the prior filter's
 * behaviour (WER, game's own dialog, etc.) is preserved. */
static DWORD WINAPI CrashFilterWatchdog(LPVOID param)
{
    (void)param;
    ULONGLONG tick = 0;
    for (;;) {
        Sleep(2000);
        tick++;

        LPTOP_LEVEL_EXCEPTION_FILTER prev =
            SetUnhandledExceptionFilter(&MjCrashFilter);
        if (prev != &MjCrashFilter && prev != NULL) {
            /* A different filter was installed between the previous
             * watchdog tick and now. Capture it so MjCrashFilter chains
             * into it. */
            g_prevUnhandledFilter = prev;
            CLog("[crash] Watchdog: re-claimed top-level filter; " \
                 "chained prev=%p", (void*)prev);
        }

        /* Heartbeat every 5 ticks (~10s). Brackets time-of-death for
         * crashes that bypass SEH/VEH entirely (__fastfail, ExitProcess,
         * TerminateProcess). Cadence kept coarse so the log does not
         * turn into noise for healthy runs. */
        if ((tick % 5) == 0) {
            CLog("[crash] heartbeat: process alive at tick %llu",
                 (unsigned long long)tick);
            if (g_logFile) fflush(g_logFile);
        }
    }
    /* Unreachable, but keep /W3 happy on older toolchains that don't
     * recognise an infinite loop as a no-return tail. */
    return 0;
}

/* DllMain-time install. Watchdog thread is deferred to StartCrashWatchdog
 * because CreateThread from DllMain deadlocks against the loader lock. */
static void InstallCrashHandler(void)
{
    /* Layer 1: top-level SEH filter. Fires on unhandled exceptions
     * that have escaped every __except frame in the process. */
    g_prevUnhandledFilter = SetUnhandledExceptionFilter(&MjCrashFilter);

    /* Layer 2: vectored handler at FirstHandler=0 (called last). Avoids
     * doubling up with another priority-1 VEH (e.g. CSF's). */
    g_vectoredHandlerReg = AddVectoredExceptionHandler(0, &MjVectoredFilter);

    /* Force some CRT initialization the filter might rely on (path
     * resolution, etc.) by doing a tiny warmup. Avoids surprises inside
     * the filter if it hasn't been touched yet. */
    SYSTEMTIME dummy; GetLocalTime(&dummy); (void)dummy;

    CLog("[crash] Baseline crash handler installed "
         "(SEH+VEH). Reports -> mod_logs\\crashes\\");
}

/* Called from LoadModsOnce after the loader lock has been released.
 * Creates the watchdog thread that periodically re-installs MjCrashFilter
 * if the game (or CLR, or any late-loaded DLL) displaces it.
 * Idempotent: guards against being called more than once.
 *
 * Disabled by default. Too much of a headache. */
static const BOOL g_crashWatchdogEnabled = FALSE;
static LONG g_watchdogStarted = 0;
static void StartCrashWatchdog(void)
{
    if (!g_crashWatchdogEnabled) {
        CLog("[crash] Watchdog disabled (g_crashWatchdogEnabled=FALSE). "
             "SEH+VEH crash handlers remain active.");
        return;
    }

    if (InterlockedCompareExchange(&g_watchdogStarted, 1, 0) != 0) return;

    HANDLE hThread = CreateThread(NULL, 0, &CrashFilterWatchdog, NULL, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
        CLog("[crash] Watchdog thread started "
             "(re-installs top-level filter every 2s).");
    } else {
        CLog("[crash] Watchdog thread creation failed: GetLastError=%lu. "
             "Crash handler still active via SEH+VEH.",
             (unsigned long)GetLastError());
    }
}

/* --- Internal: find existing hook site by RVA --- */

static MJ_HookSite* MJ_FindSite(UINT_PTR rva)
{
    for (MJ_HookSite* s = g_mjHookSites; s; s = s->nextSite)
        if (s->rva == rva) return s;
    return NULL;
}

/* Diagnostic for VirtualAlloc returning NX or refusing PAGE_EXECUTE_*
 * (ACG, EDR hook, Smart App Control, HVCI, etc.). Verbose triage is
 * logged once per process. A one-line summary fires on every subsequent
 * failure. */

static volatile LONG g_mjDynCodeDiagLogged = 0;

static void MJ_DiagnoseDynamicCodeBlocked(const char* purpose,
                                          void* page,
                                          SIZE_T size,
                                          DWORD lastError,
                                          DWORD observedProtect)
{
    /* One-line summary, always emitted. */
    CLog("[MJ] DYNAMIC CODE BLOCKED while allocating %s "
         "(page=%p size=0x%IX protect=0x%X GetLastError=0x%X). "
         "The trampoline cannot be made executable on this machine.",
         purpose ? purpose : "executable memory",
         page, (size_t)size,
         (unsigned)observedProtect, (unsigned)lastError);

    /* Verbose triage block, emitted once per process. */
    if (InterlockedExchange(&g_mjDynCodeDiagLogged, 1) != 0)
        return;

    CLog("[MJ] First occurrence in this process. Likely root causes "
         "(per-machine, not per-build):");
    CLog("[MJ]   1. Arbitrary Code Guard (ACG) enabled for the host EXE "
         "in Windows Exploit Protection.");
    CLog("[MJ]   2. EDR/AV filtering NtAllocateVirtualMemory or "
         "NtProtectVirtualMemory and silently stripping EXECUTE bits "
         "(CrowdStrike, SentinelOne, ESET, Trend Micro, Sophos "
         "Intercept X, corporate Bitdefender, etc.).");
    CLog("[MJ]   3. Windows 11 Smart App Control on an unsigned "
         "process tree.");
    CLog("[MJ]   4. HVCI / Memory Integrity with VBS-enforced ACG "
         "(Windows 11 22H2+).");
    CLog("[MJ]   5. Group-policy mitigation on a managed machine "
         "(corporate / AAD-joined). OneDrive-redirected Documents/"
         "Desktop is a soft signal here.");
    CLog("[MJ]   6. Another loaded module having called "
         "SetProcessMitigationPolicy(ProcessDynamicCodePolicy, "
         "ProhibitDynamicCode=1) earlier in the load order.");
    CLog("[MJ] User triage:");
    CLog("[MJ]   PowerShell: Get-ProcessMitigation -Name <host>.exe");
    CLog("[MJ]   GUI: Settings -> Windows Security -> App & browser "
         "control -> Exploit protection -> Program settings -> "
         "<host>.exe -> uncheck 'Arbitrary code guard (ACG)' and "
         "'Block low integrity images'.");
    CLog("[MJ]   Also check: Smart App Control state, Core isolation "
         "-> Memory integrity, and any third-party EDR/AV configured "
         "to block dynamic code in unsigned processes.");
    CLog("[MJ] Effect: this hook installation will fail. Mods that "
         "depend on Mewjector hooks will report install failure and "
         "may fall back to raw mode or refuse to load. Disable the "
         "offending mitigation for the host EXE and relaunch.");
}

/* --- Internal: verify a page is committed and has executable protection.
 * Returns 1 if executable, 0 otherwise. */

static int MJ_PageIsExecutable(void* page)
{
    if (!page) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)page, &mbi, sizeof(mbi)) != sizeof(mbi))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) ? 1 : 0;
}

/* Probe a freshly returned VirtualAlloc result and decide whether it
 * is a usable executable page. If yes, return the pointer. If no
 * (allocation failed with ERROR_DYNAMIC_CODE_BLOCKED, or the page came
 * back without EXECUTE bits), emit the dynamic-code-blocked diagnostic,
 * free the page when present, and return NULL. Other allocation
 * failures (e.g. region conflicts) are returned silently as NULL so
 * the caller can keep searching. */

static BYTE* MJ_FinalizeExecAlloc(BYTE* page, SIZE_T size, const char* purpose)
{
    if (!page) {
        DWORD le = GetLastError();
        if (le == ERROR_DYNAMIC_CODE_BLOCKED || le == ERROR_ACCESS_DENIED)
            MJ_DiagnoseDynamicCodeBlocked(purpose, NULL, size, le, 0);
        return NULL;
    }
    if (!MJ_PageIsExecutable(page)) {
        MEMORY_BASIC_INFORMATION mbi;
        DWORD observed = 0;
        if (VirtualQuery((LPCVOID)page, &mbi, sizeof(mbi)) == sizeof(mbi))
            observed = mbi.Protect;
        MJ_DiagnoseDynamicCodeBlocked(purpose, page, size,
                                      GetLastError(), observed);
        VirtualFree(page, 0, MEM_RELEASE);
        return NULL;
    }
    return page;
}

/* --- Internal: allocate RWX memory within +/-2 GB of 'nearAddr' ---
 *
 * Trampolines that contain E8/E9 rel32 instructions need to sit close
 * enough to the original code that the 32-bit displacement can still
 * reach the same target.  Scans outward from 'nearAddr' in 64 KB
 * steps (Windows allocation granularity) looking for a free region.
 * Falls back to an unconstrained allocation if nothing in range is
 * found; the fixup pass will log a warning in that case. */

static BYTE* MJ_AllocNear(void* nearAddr, SIZE_T size)
{
    const SIZE_T kGranularity = 0x10000;          /* 64 KB */
    const intptr_t kMaxDelta  = 0x7FFF0000LL;     /* just under 2 GB */

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    UINT_PTR lo = (UINT_PTR)si.lpMinimumApplicationAddress;
    UINT_PTR hi = (UINT_PTR)si.lpMaximumApplicationAddress;

    UINT_PTR origin = (UINT_PTR)nearAddr;
    UINT_PTR minAddr = (origin > (UINT_PTR)kMaxDelta)
                     ? (origin - kMaxDelta) : lo;
    UINT_PTR maxAddr = (origin < (UINT_PTR)(hi - kMaxDelta))
                     ? (origin + kMaxDelta) : hi;
    if (minAddr < lo) minAddr = lo;
    if (maxAddr > hi) maxAddr = hi;

    /* Round to allocation granularity */
    minAddr = (minAddr + kGranularity - 1) & ~(kGranularity - 1);
    maxAddr = maxAddr & ~(kGranularity - 1);

    /* Search outward from origin, alternating below/above */
    for (UINT_PTR delta = kGranularity;
         delta <= (UINT_PTR)kMaxDelta;
         delta += kGranularity)
    {
        UINT_PTR candidates[2];
        int nCandidates = 0;
        if (origin >= delta && (origin - delta) >= minAddr)
            candidates[nCandidates++] = (origin - delta) & ~(kGranularity - 1);
        if ((origin + delta) <= maxAddr)
            candidates[nCandidates++] = (origin + delta) & ~(kGranularity - 1);

        for (int i = 0; i < nCandidates; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)candidates[i], &mbi, sizeof(mbi)) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= size)
            {
                BYTE* p = (BYTE*)VirtualAlloc(
                    (LPVOID)candidates[i], size,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) {
                    if (MJ_PageIsExecutable(p)) return p;
                    /* Allocation succeeded but the page came back
                     * without EXECUTE. This indicates a process-wide
                     * dynamic-code block, so other candidates would
                     * fail the same way. Diagnose, free, and bail. */
                    {
                        DWORD observed = 0;
                        MEMORY_BASIC_INFORMATION m;
                        if (VirtualQuery((LPCVOID)p, &m, sizeof(m)) == sizeof(m))
                            observed = m.Protect;
                        MJ_DiagnoseDynamicCodeBlocked(
                            "near-trampoline", p, size,
                            GetLastError(), observed);
                    }
                    VirtualFree(p, 0, MEM_RELEASE);
                    return NULL;
                }
                /* VirtualAlloc returned NULL. If the system explicitly
                 * refused executable memory, no other candidate will
                 * succeed either. Diagnose and bail. */
                if (GetLastError() == ERROR_DYNAMIC_CODE_BLOCKED) {
                    MJ_DiagnoseDynamicCodeBlocked(
                        "near-trampoline", NULL, size,
                        ERROR_DYNAMIC_CODE_BLOCKED, 0);
                    return NULL;
                }
                /* Otherwise: probably a region conflict, keep searching. */
            }
        }
    }

    /* Last resort: unconstrained allocation (fixup will warn if needed) */
    CLog("[MJ]   WARN: could not allocate within +/-2 GB of %p, "
         "falling back to arbitrary address.", nearAddr);
    {
        BYTE* p = (BYTE*)VirtualAlloc(NULL, size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        return MJ_FinalizeExecAlloc(p, size, "near-trampoline-fallback");
    }
}

/* --- Internal: allocate a 14-byte executable JMP stub --- */

static BYTE* MJ_AllocJmpStub(void* target)
{
    BYTE* stub = (BYTE*)VirtualAlloc(NULL, 14,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    stub = MJ_FinalizeExecAlloc(stub, 14, "hook-jmp-stub");
    if (!stub) return NULL;

    /*  FF 25 00 00 00 00   jmp qword ptr [rip+0]
     *  XX XX XX XX XX XX XX XX   absolute target address   */
    stub[0] = 0xFF;
    stub[1] = 0x25;
    stub[2] = 0x00;
    stub[3] = 0x00;
    stub[4] = 0x00;
    stub[5] = 0x00;
    memcpy(stub + 6, &target, 8);
    return stub;
}

/* --- Internal: update a JMP stub's target (RWX memory, no protect needed) --- */

static void MJ_UpdateStub(BYTE* stub, void* newTarget)
{
    memcpy(stub + 6, &newTarget, 8);
}

/* --- Internal: create a new hook site and patch the game entry --- */

static MJ_HookSite* MJ_CreateSite(UINT_PTR rva, int stolenBytes)
{
    MJ_HookSite* site = (MJ_HookSite*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MJ_HookSite));
    if (!site) return NULL;

    BYTE* patchAddr = (BYTE*)(g_mjGameBase + rva);

    /* If stolenBytes == 0, calculate the stolen byte count ourselves using
     * mj_lde (see mj_lde.h). mj_lde returns 0 on any opcode it does not
     * recognize, in which case we abort and require the caller to supply
     * an explicit stolenBytes value for this hook site. */
    if (stolenBytes == 0)
    {
        BYTE* cursor = patchAddr;
        while (cursor - patchAddr < 14)
        {
            int len = mj_lde(cursor);
            if (len <= 0)
            {
                CLog("[Mewjector] MJ_CreateSite: mj_lde failed to decode "
                     "instruction at rva=0x%llX (offset %lld). Caller must "
                     "supply explicit stolenBytes for this site.\n",
                     (unsigned long long)rva,
                     (long long)(cursor - patchAddr));
                HeapFree(GetProcessHeap(), 0, site);
                return NULL;
            }
            cursor += len;
        }
        stolenBytes = (int)(cursor - patchAddr);
    }

    site->rva         = rva;
    site->stolenBytes = stolenBytes;
    site->patchAddr   = patchAddr;
    site->chain       = NULL;

    /* Back up original bytes for integrity verification */
    memcpy(site->origBytes, site->patchAddr,
           stolenBytes < 24 ? stolenBytes : 24);

    /* Build original trampoline: stolen bytes + JMP back to original+N.
     * Allocate within +/-2 GB of the patch site so that any E8/E9 rel32
     * instructions in the stolen bytes can be fixed up in-place. */
    int tramSz = stolenBytes + 14;
    BYTE* tramp = MJ_AllocNear(patchAddr, tramSz);
    if (!tramp) {
        HeapFree(GetProcessHeap(), 0, site);
        return NULL;
    }

    memcpy(tramp, site->patchAddr, stolenBytes);   /* stolen prologue  */

    /* --- RIP-relative fixup pass ---
     * Scan the stolen bytes for any instruction whose encoding references
     * RIP, and rewrite its 4-byte displacement so it resolves to the same
     * absolute target from the trampoline's location.  Two classes:
     *
     *   1. E8 / E9 rel32 (call rel32 / jmp rel32).  Common in prologues
     *      that call __chkstk for large stack frames.
     *
     *   2. ModR/M-encoded [rip+disp32] operands (mod==00, rm==101).
     *      Common in prologues that touch globals or call indirect:
     *      MOV [rip+d], reg / MOV reg,[rip+d] / LEA reg,[rip+d] /
     *      FF 15 [rip+d] (call indirect) / FF 25 [rip+d] (jmp indirect).
     *      Length-disassembled via mj_lde_ex; the disp32 offset within
     *      the instruction comes back as info.rip_rel_disp_offset.
     *
     * Both classes share the same abs-target math: take the original
     * instruction's end address (the PC value used to resolve the
     * disp32), add the original disp32, then subtract the new
     * instruction's end address to get the new disp32. Because
     * MJ_AllocNear allocates the trampoline within +/-2 GB of the
     * patch site, the difference always fits in int32 and the fixup
     * succeeds. Out-of-range cases are logged and flagged. */
    {
        int pos = 0;
        while (pos < stolenBytes) {
            mj_lde_info ldi = mj_lde_ex(tramp + pos);
            int len = ldi.length;
            if (len <= 0) break;   /* shouldn't happen -- already validated */

            BYTE opcode = tramp[pos];

            /* Class 1: E8 / E9 rel32 ------------------------------------ */
            if ((opcode == 0xE8 || opcode == 0xE9) && len == 5
                && pos + 5 <= stolenBytes)
            {
                /* Read the original displacement */
                int32_t origDisp;
                memcpy(&origDisp, tramp + pos + 1, 4);

                /* Compute the absolute target as it was at the original site */
                UINT_PTR origInstrAddr = (UINT_PTR)site->patchAddr + pos;
                UINT_PTR absTarget     = origInstrAddr + 5 + (int64_t)origDisp;

                /* Compute the new displacement from the trampoline location */
                UINT_PTR newInstrAddr  = (UINT_PTR)tramp + pos;
                int64_t  newDisp64     = (int64_t)(absTarget - (newInstrAddr + 5));

                if (newDisp64 >= -2147483648LL && newDisp64 <= 2147483647LL) {
                    int32_t newDisp = (int32_t)newDisp64;
                    memcpy(tramp + pos + 1, &newDisp, 4);
                    CLog("[MJ]   Fixup: %s at stolen+%d -> abs 0x%llX (disp %+d -> %+d)",
                         opcode == 0xE8 ? "call" : "jmp",
                         pos, (unsigned long long)absTarget, origDisp, newDisp);
                } else {
                    CLog("[MJ]   WARNING: %s at stolen+%d target 0x%llX out of "
                         "rel32 range from trampoline -- cannot fixup!",
                         opcode == 0xE8 ? "call" : "jmp",
                         pos, (unsigned long long)absTarget);
                }
            }
            /* Class 2: ModR/M [rip+disp32] ------------------------------ */
            else if (ldi.rip_rel_disp_offset >= 0
                  && pos + ldi.rip_rel_disp_offset + 4 <= stolenBytes)
            {
                int      dispPos       = pos + ldi.rip_rel_disp_offset;
                int32_t  origDisp;
                memcpy(&origDisp, tramp + dispPos, 4);

                UINT_PTR origInstrAddr = (UINT_PTR)site->patchAddr + pos;
                UINT_PTR origInstrEnd  = origInstrAddr + len;
                UINT_PTR absTarget     = origInstrEnd + (int64_t)origDisp;

                UINT_PTR newInstrAddr  = (UINT_PTR)tramp + pos;
                UINT_PTR newInstrEnd   = newInstrAddr + len;
                int64_t  newDisp64     = (int64_t)(absTarget - newInstrEnd);

                if (newDisp64 >= -2147483648LL && newDisp64 <= 2147483647LL) {
                    int32_t newDisp = (int32_t)newDisp64;
                    memcpy(tramp + dispPos, &newDisp, 4);
                    CLog("[MJ]   Fixup: ModR/M [rip+disp32] at stolen+%d "
                         "-> abs 0x%llX (disp %+d -> %+d, len=%d)",
                         pos, (unsigned long long)absTarget,
                         origDisp, newDisp, len);
                } else {
                    CLog("[MJ]   WARNING: ModR/M [rip+disp32] at stolen+%d "
                         "target 0x%llX out of rel32 range from "
                         "trampoline -- cannot fixup!",
                         pos, (unsigned long long)absTarget);
                }
            }

            pos += len;
        }
    }

    tramp[stolenBytes + 0] = 0xFF;                 /* jmp qword [rip+0]*/
    tramp[stolenBytes + 1] = 0x25;
    tramp[stolenBytes + 2] = 0x00;
    tramp[stolenBytes + 3] = 0x00;
    tramp[stolenBytes + 4] = 0x00;
    tramp[stolenBytes + 5] = 0x00;
    UINT_PTR retAddr = (UINT_PTR)site->patchAddr + stolenBytes;
    memcpy(tramp + stolenBytes + 6, &retAddr, 8);  /* return address   */

    site->origTrampoline = tramp;

    /* Patch the game entry point:
     *   FF 25 00 00 00 00 [8-byte target]  = 14 bytes
     *   Remaining stolen bytes filled with 0x90 (NOP), unreachable   */
    DWORD oldProt;
    VirtualProtect(site->patchAddr, stolenBytes,
                   PAGE_EXECUTE_READWRITE, &oldProt);

    site->patchAddr[0] = 0xFF;
    site->patchAddr[1] = 0x25;
    site->patchAddr[2] = 0x00;
    site->patchAddr[3] = 0x00;
    site->patchAddr[4] = 0x00;
    site->patchAddr[5] = 0x00;
    /* Target address at [6..13], filled by RebuildChain */
    memset(site->patchAddr + 6, 0, 8);
    for (int i = 14; i < stolenBytes; i++)
        site->patchAddr[i] = 0x90;

    VirtualProtect(site->patchAddr, stolenBytes, oldProt, &oldProt);

    /* Prepend to global site list */
    site->nextSite = g_mjHookSites;
    g_mjHookSites  = site;

    return site;
}

/* --- Internal: rebuild the hook chain for a site ---
 *
 *  Game entry -> chain[0].hookFn
 *  chain[0].trampoline -> chain[1].hookFn
 *  chain[1].trampoline -> chain[2].hookFn
 *  ...
 *  chain[N].trampoline -> origTrampoline (real game code)            */

static void MJ_RebuildChain(MJ_HookSite* site)
{
    if (!site->chain) return;

    /* Update the 8-byte target in the patched game entry */
    DWORD oldProt;
    VirtualProtect(site->patchAddr + 6, 8,
                   PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site->patchAddr + 6, &site->chain->hookFn, 8);
    VirtualProtect(site->patchAddr + 6, 8, oldProt, &oldProt);

    /* Wire each hook's trampoline to the next hook (or original) */
    for (MJ_HookEntry* e = site->chain; e; e = e->next) {
        void* target = e->next
            ? e->next->hookFn
            : (void*)site->origTrampoline;
        MJ_UpdateStub(e->trampoline, target);
    }
}

/* ===================================================================
 *  MJ_InstallHook: Install a chainable hook at a game RVA
 *
 *  Multiple hooks on the same RVA are chained by priority
 *  (lower priority value = called first in the chain).
 *
 *  outTrampoline receives a callable function pointer with the
 *  same signature as the hooked function.  Calling it invokes
 *  the next hook in the chain, or the original game function
 *  if this is the last (lowest-priority) hook.
 *
 *  Returns 1 on success, 0 on failure.
 * =================================================================== */

__declspec(dllexport) int __cdecl MJ_InstallHook(
    UINT_PTR    rva,
    int         stolenBytes,
    void*       hookFn,
    void**      outTrampoline,
    int         priority,
    const char* owner)
{
    if (!hookFn || !outTrampoline || (stolenBytes < 14 && stolenBytes != 0)) {
        CLog("[MJ] InstallHook REJECTED: bad args "
             "(rva=0x%llX stolen=%d hook=%p owner=%s)",
             (unsigned long long)rva, stolenBytes, hookFn,
             owner ? owner : "(null)");
        return 0;
    }

    /* Lazy init game base (should already be set by LoadModsOnce) */
    if (!g_mjGameBase)
        g_mjGameBase = (UINT_PTR)GetModuleHandleA(NULL);

    WaitForSingleObject(g_mjHookMutex, INFINITE);

    /* Find or create the hook site */
    MJ_HookSite* site = MJ_FindSite(rva);

    if (!site) {
        site = MJ_CreateSite(rva, stolenBytes);
        if (!site) {
            CLog("[MJ] InstallHook FAILED: could not create site "
                 "for RVA 0x%llX (%s)",
                 (unsigned long long)rva, owner ? owner : "?");
            ReleaseMutex(g_mjHookMutex);
            return 0;
        }
        CLog("[MJ] New hook site: RVA 0x%llX (%d stolen bytes)",
             (unsigned long long)rva, site->stolenBytes);
    } else if (stolenBytes != 0 && site->stolenBytes != stolenBytes) {
        /* Note that if the site has already been created with a manual stolenBytes value,
         * subsequent hooking attempts with stolenBytes==0 do not check whether the
         * established size agrees with an automatic calculation */
        CLog("[MJ] WARNING: %s requests %d stolen bytes at "
             "RVA 0x%llX but site already has %d, using %d",
             owner ? owner : "?", stolenBytes,
             (unsigned long long)rva, site->stolenBytes,
             site->stolenBytes);
    }

    /* Allocate the hook entry */
    MJ_HookEntry* entry = (MJ_HookEntry*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MJ_HookEntry));
    if (!entry) {
        CLog("[MJ] InstallHook FAILED: entry alloc (%s)",
             owner ? owner : "?");
        ReleaseMutex(g_mjHookMutex);
        return 0;
    }

    entry->hookFn    = hookFn;
    entry->priority  = priority;
    entry->trampoline = MJ_AllocJmpStub(NULL);  /* target set by rebuild */
    if (owner) { strncpy(entry->owner, owner, 63); entry->owner[63] = '\0'; }

    if (!entry->trampoline) {
        CLog("[MJ] InstallHook FAILED: trampoline alloc (%s)",
             owner ? owner : "?");
        HeapFree(GetProcessHeap(), 0, entry);
        ReleaseMutex(g_mjHookMutex);
        return 0;
    }

    /* Insert into chain sorted by priority (lower = earlier) */
    MJ_HookEntry** pp = &site->chain;
    while (*pp && (*pp)->priority <= priority)
        pp = &(*pp)->next;
    entry->next = *pp;
    *pp = entry;

    /* Rebuild the entire chain for this site */
    MJ_RebuildChain(site);

    *outTrampoline = (void*)entry->trampoline;
    g_mjHasHooks = 1;

    CLog("[MJ] Hook installed: RVA 0x%llX  pri=%d  owner=%s  fn=%p",
         (unsigned long long)rva, priority,
         owner ? owner : "?", hookFn);

    /* Log the full chain state */
    int pos = 0;
    for (MJ_HookEntry* e = site->chain; e; e = e->next, pos++) {
        void* nextTarget = e->next
            ? e->next->hookFn
            : (void*)site->origTrampoline;
        CLog("[MJ]   chain[%d]: %-24s pri=%-4d fn=%p -> %p",
             pos, e->owner, e->priority, e->hookFn, nextTarget);
    }

    ReleaseMutex(g_mjHookMutex);
    return 1;
}

/* ===================================================================
 *  MJ_QueryHook: Count hooks installed at an RVA
 * =================================================================== */

__declspec(dllexport) int __cdecl MJ_QueryHook(UINT_PTR rva)
{
    WaitForSingleObject(g_mjHookMutex, INFINITE);
    int count = 0;
    MJ_HookSite* site = MJ_FindSite(rva);
    if (site)
        for (MJ_HookEntry* e = site->chain; e; e = e->next)
            count++;
    ReleaseMutex(g_mjHookMutex);
    return count;
}

/* ===================================================================
 *  MJ_AllocTypeIdPair: Allocate a unique type ID pair
 *
 *  Returns base index.  Caller gets base and base+1.
 *  Thread-safe via InterlockedExchangeAdd.
 * =================================================================== */

__declspec(dllexport) UINT_PTR __cdecl MJ_AllocTypeIdPair(const char* owner)
{
    LONG base = InterlockedExchangeAdd(&g_mjNextTypeId, 2);
    CLog("[MJ] TypeIdPair: 0x%04X / 0x%04X  owner=%s",
         (unsigned)base, (unsigned)(base + 1),
         owner ? owner : "?");
    return (UINT_PTR)base;
}

/* ===================================================================
 *  MJ_RegisterName: Register a name in a collision namespace
 *
 *  Categories: "status", "formula_var", "conditional", "x_is",
 *  or any string your mods agree on.
 *
 *  Returns 1 if registered.  Returns 0 if the name was already
 *  taken by a different owner (collision detected).
 * =================================================================== */

__declspec(dllexport) int __cdecl MJ_RegisterName(
    const char* category,
    const char* name,
    const char* owner)
{
    if (!category || !name) return 0;

    WaitForSingleObject(g_mjNameMutex, INFINITE);

    /* Check for collision */
    for (int i = 0; i < g_mjNameCount; i++) {
        if (_stricmp(g_mjNames[i].category, category) == 0 &&
            _stricmp(g_mjNames[i].name, name) == 0)
        {
            /* Same owner re-registering is OK (idempotent) */
            if (owner && _stricmp(g_mjNames[i].owner, owner) == 0) {
                ReleaseMutex(g_mjNameMutex);
                return 1;
            }
            CLog("[MJ] NAME COLLISION: %s/%s owned by [%s], "
                 "rejected for [%s]",
                 category, name, g_mjNames[i].owner,
                 owner ? owner : "?");
            ReleaseMutex(g_mjNameMutex);
            return 0;
        }
    }

    if (g_mjNameCount >= MJ_MAX_NAMES) {
        CLog("[MJ] Name registry full (%d)! Cannot register %s/%s",
             MJ_MAX_NAMES, category, name);
        ReleaseMutex(g_mjNameMutex);
        return 0;
    }

    MJ_NameEntry* e = &g_mjNames[g_mjNameCount++];
    strncpy(e->category, category, 31);          e->category[31] = '\0';
    strncpy(e->name, name, 127);                 e->name[127]    = '\0';
    strncpy(e->owner, owner ? owner : "", 63);   e->owner[63]    = '\0';

    CLog("[MJ] Name registered: %s/%s  owner=%s", category, name, e->owner);

    ReleaseMutex(g_mjNameMutex);
    return 1;
}

/* ===================================================================
 *  MJ_LookupName: Check who owns a name (or NULL if unregistered)
 * =================================================================== */

__declspec(dllexport) const char* __cdecl MJ_LookupName(
    const char* category,
    const char* name)
{
    if (!category || !name) return NULL;

    WaitForSingleObject(g_mjNameMutex, INFINITE);
    for (int i = 0; i < g_mjNameCount; i++) {
        if (_stricmp(g_mjNames[i].category, category) == 0 &&
            _stricmp(g_mjNames[i].name, name) == 0)
        {
            const char* result = g_mjNames[i].owner;
            ReleaseMutex(g_mjNameMutex);
            return result;
        }
    }
    ReleaseMutex(g_mjNameMutex);
    return NULL;
}

/* ===================================================================
 *  MJ_GetGameBase: Return the game's image base address
 * =================================================================== */

__declspec(dllexport) UINT_PTR __cdecl MJ_GetGameBase(void)
{
    if (!g_mjGameBase)
        g_mjGameBase = (UINT_PTR)GetModuleHandleA(NULL);
    return g_mjGameBase;
}

/* ===================================================================
 *  MJ_Log: Log through the chainloader's timestamped log file
 *
 *  Mods can use this instead of opening their own log files.
 *  Output goes to mod_logs/chainloader.log with an [owner] tag.
 * =================================================================== */

__declspec(dllexport) void __cdecl MJ_Log(
    const char* owner,
    const char* fmt, ...)
{
    if (!g_logFile) return;
    WaitForSingleObject(g_logMutex, INFINITE);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] [%s] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            owner ? owner : "?");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);

    ReleaseMutex(g_logMutex);
}

/* ===================================================================
 *  MJ_VerifyHooks: Post-load integrity check
 *
 *  Reads each managed hook site's entry bytes and verifies they
 *  still contain our FF 25 patch pointing at the chain head.
 *  Call after all mods are loaded to detect third-party overwrites.
 *
 *  Returns the number of corrupted sites (0 = all good).
 * =================================================================== */

__declspec(dllexport) int __cdecl MJ_VerifyHooks(void)
{
    int corrupted = 0;

    WaitForSingleObject(g_mjHookMutex, INFINITE);

    for (MJ_HookSite* site = g_mjHookSites; site; site = site->nextSite) {
        /* Check the entry still has our FF 25 opcode */
        if (site->patchAddr[0] != 0xFF || site->patchAddr[1] != 0x25) {
            CLog("[MJ] INTEGRITY FAIL: RVA 0x%llX entry overwritten! "
                 "Expected FF 25, found %02X %02X",
                 (unsigned long long)site->rva,
                 site->patchAddr[0], site->patchAddr[1]);
            corrupted++;
            continue;
        }

        /* Check the target matches the current chain head */
        if (site->chain) {
            void* currentTarget;
            memcpy(&currentTarget, site->patchAddr + 6, 8);
            if (currentTarget != site->chain->hookFn) {
                CLog("[MJ] INTEGRITY FAIL: RVA 0x%llX target mismatch! "
                     "Expected %p (%s), found %p",
                     (unsigned long long)site->rva,
                     site->chain->hookFn, site->chain->owner,
                     currentTarget);
                corrupted++;
            }
        }
    }

    ReleaseMutex(g_mjHookMutex);

    if (corrupted == 0 && g_mjHookSites)
        CLog("[MJ] Integrity check: ALL OK");
    else if (corrupted > 0)
        CLog("[MJ] Integrity check: %d site(s) CORRUPTED!", corrupted);

    return corrupted;
}

/* ===================================================================
 *  MJ_GetVersion: Return API version for compatibility checks
 * =================================================================== */

__declspec(dllexport) int __cdecl MJ_GetVersion(void)
{
    return MJ_API_VERSION;
}


/* ===================================================================
 *  DLL entry point
 *
 *  DllMain only handles lightweight setup that is safe under the
 *  loader lock: base dir resolution, logging, version.dll forwarding,
 *  and config parsing.  Actual mod loading is deferred to the first
 *  proxy call via LoadModsOnce() — see above.
 * =================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        /* Allocate VEH re-entrancy TLS slot before any handler is installed.
         * Must happen before InstallCrashHandler() and before any thread is
         * created, so the slot is valid whenever the VEH first fires. */
        g_vehTlsSlot = TlsAlloc();

        ResolveBaseDir();
        LogOpen();

        /* Init Mewjector API mutexes (safe under loader lock) */
        g_mjHookMutex  = CreateMutexA(NULL, FALSE, NULL);
        g_mjNameMutex  = CreateMutexA(NULL, FALSE, NULL);
        g_modsLoadDone = CreateEventA(NULL, TRUE, FALSE, NULL); /* manual-reset */

        /* Install the SEH+VEH crash handlers as early as possible so
         * any AV or __fastfail that fires during mod load (or during
         * the patched init wrapper) gets captured to mod_logs\crashes\.
         * Watchdog thread is deferred to LoadModsOnce because
         * CreateThread from DllMain risks loader-lock deadlock. */
        InstallCrashHandler();

#ifdef MJ_DEBUG_BUILD
        CLog("=== Mewjector DEBUGv3.x (build %s, RIP-rel-fixup-v2) ===",
             __DATE__);
#else
        CLog("=== Mewjector v3.0 ===");
#endif
        CLog("Process: PID %lu  TID %lu", GetCurrentProcessId(), GetCurrentThreadId());
        CLog("Game directory: %s", g_baseDir);
        CLog("");

        if (!LoadRealVersionDll()) {
            CLog("WARNING: Could not load real version.dll. Forwarded API calls");
            CLog("  will return failures, but mod loading will proceed.");
        }
        CLog("");

        LoadConfig();
        CLog("");

        if (!g_config.enabled) {
            CLog("Chainloader DISABLED via config. No mods will be loaded.");
            CLog("=== Chainloader init complete (disabled) ===");
            return TRUE;
        }

        CLog("Mod loading deferred to first proxy call.");

        /* Fallback: patch the game's entry point so that LoadModsOnce()
         * is guaranteed to run even if no version.dll proxy export is
         * ever called.  Safe to do here -- the entry point hasn't been
         * called yet, and the trampoline executes after the loader lock
         * is released.  If a proxy call fires first, the atomic guard
         * in LoadModsOnce() makes this a no-op. */
        if (!PatchEntryPointFallback())
            CLog("WARNING: entry-point fallback failed; mods depend on "
                 "a version.dll proxy call.");
        CLog("");
    }
    else if (reason == DLL_PROCESS_DETACH) {
        CLog("=== Mewjector detaching ===");

        if (g_vehTlsSlot != TLS_OUT_OF_INDEXES) {
            TlsFree(g_vehTlsSlot);
            g_vehTlsSlot = TLS_OUT_OF_INDEXES;
        }

        if (g_mjHookMutex) { CloseHandle(g_mjHookMutex); g_mjHookMutex = NULL; }
        if (g_mjNameMutex) { CloseHandle(g_mjNameMutex); g_mjNameMutex = NULL; }

        LogClose();

        if (g_realVersionDll) {
            FreeLibrary(g_realVersionDll);
            g_realVersionDll = NULL;
        }
    }

    return TRUE;
}
