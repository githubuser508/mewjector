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
#include <stdio.h>
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

static FILE*  g_logFile  = NULL;
static HANDLE g_logMutex = NULL;

static void LogOpen(void)
{
    char logDir[MAX_PATH];
    char logPath[MAX_PATH];

    snprintf(logDir, MAX_PATH, "%smod_logs", g_baseDir);
    CreateDirectoryA(logDir, NULL);

    snprintf(logPath, MAX_PATH, "%s\\chainloader.log", logDir);

    g_logFile  = fopen(logPath, "w");
    g_logMutex = CreateMutexA(NULL, FALSE, NULL);
}

static void LogClose(void)
{
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
    if (g_logMutex) { CloseHandle(g_logMutex); g_logMutex = NULL; }
}

static void CLog(const char* fmt, ...)
{
    if (!g_logFile) return;
    WaitForSingleObject(g_logMutex, INFINITE);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);

    ReleaseMutex(g_logMutex);
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

static volatile LONG g_modsLoaded = 0;
static void LoadModsOnce(void);   /* forward decl for entry-point fallback */

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

static void EntryPointHandler(void)
{
    LoadModsOnce();

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

    CLog("  Fallback: entry point patched at RVA 0x%X (%d stolen bytes).",
         entryRVA, stolenBytes);

    return 1;
}

static void LoadModsOnce(void)
{
    if (InterlockedCompareExchange(&g_modsLoaded, 1, 0) != 0) return;

    /* Actually disable when Enabled=0 says we're disabled. */
    if (!g_config.enabled) return;

    /* Resolve game image base for RVA calculations */
    g_mjGameBase = (UINT_PTR)GetModuleHandleA(NULL);

    CLog("First proxy call — loading mods (loader lock released)");
    CLog("Game image base: 0x%p", (void*)g_mjGameBase);
    CLog("Mewjector API v%d available (MJ_InstallHook, MJ_RegisterName, ...)",
         MJ_API_VERSION);
    CLog("");

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

/* --- Internal: find existing hook site by RVA --- */

static MJ_HookSite* MJ_FindSite(UINT_PTR rva)
{
    for (MJ_HookSite* s = g_mjHookSites; s; s = s->nextSite)
        if (s->rva == rva) return s;
    return NULL;
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
                if (p) return p;
            }
        }
    }

    /* Last resort: unconstrained allocation (fixup will warn if needed) */
    CLog("[MJ]   WARN: could not allocate within +/-2 GB of %p, "
         "falling back to arbitrary address.", nearAddr);
    return (BYTE*)VirtualAlloc(NULL, size,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

/* --- Internal: allocate a 14-byte executable JMP stub --- */

static BYTE* MJ_AllocJmpStub(void* target)
{
    BYTE* stub = (BYTE*)VirtualAlloc(NULL, 14,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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
     * Scan the stolen bytes for E8 (call rel32) and E9 (jmp rel32)
     * instructions.  Their 4-byte displacement is relative to the
     * instruction's own address, so after relocation into the
     * trampoline they point to the wrong place.  We rewrite each
     * displacement so it resolves to the same absolute target as it
     * did at the original location.
     *
     * This fixes prologues that contain a call to __chkstk (common
     * for functions with large stack frames under MSVC x64).
     *
     * NOTE: This does NOT handle ModR/M-encoded RIP-relative
     * instructions (e.g. LEA reg,[rip+disp], MOV reg,[rip+disp]).
     * Those require full ModR/M decoding -- not needed for any
     * current hook site but worth adding if new sites require it. */
    {
        int pos = 0;
        while (pos < stolenBytes) {
            int len = mj_lde(tramp + pos);
            if (len <= 0) break;   /* shouldn't happen -- already validated */

            BYTE opcode = tramp[pos];
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

        ResolveBaseDir();
        LogOpen();

        /* Init Mewjector API mutexes (safe under loader lock) */
        g_mjHookMutex = CreateMutexA(NULL, FALSE, NULL);
        g_mjNameMutex = CreateMutexA(NULL, FALSE, NULL);

        CLog("=== Mewjector v3.0 ===");
        CLog("Process: PID %lu", GetCurrentProcessId());
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
