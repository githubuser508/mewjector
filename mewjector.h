/*
 * mewjector.h — Mewjector API header for mod developers
 *
 * Include this in your mod and call MJ_Require()/MJ_Resolve() during init
 * to get access to all Mewjector coordination services.  Everything is
 * resolved at runtime from version.dll — no import lib or build dependency
 * needed.
 *
 * Usage:
 *
 *   #include "mewjector.h"
 *
 *   static MewjectorAPI mj;
 *
 *   // In your DllMain or init function:
 *   if (!MJ_Require("MyMod") || !MJ_Resolve(&mj)) {
 *       // Mewjector not available (plain version.dll?) — fall back
 *       OutputDebugStringA("Mewjector API not found\n");
 *       return;
 *   }
 *
 *   // Install a chainable hook (priority 10, lower = called first)
 *   void* trampoline = NULL;
 *   mj.InstallHook(0x47f6a0, 15, MyHookFn, &trampoline, 10, "MyMod");
 *   g_originalFn = (fn_original)trampoline;
 *
 *   // Allocate a unique type ID pair for a custom game object
 *   UINT_PTR typeBase = mj.AllocTypeIdPair("MyMod");
 *   // Use typeBase and typeBase+1 — guaranteed unique
 *
 *   // Register names to detect collisions with other mods
 *   if (!mj.RegisterName("status", "MyCustomStatus", "MyMod"))
 *       mj.Log("MyMod", "Name collision on MyCustomStatus!");
 *
 *   // Log through the shared chainloader log
 *   mj.Log("MyMod", "Loaded with %d hooks", hookCount);
 *
 *
 * Hook chaining explained:
 *
 *   When multiple mods hook the same RVA, Mewjector chains them by
 *   priority (lower value = called first).  Each hook receives a
 *   trampoline that calls the NEXT hook in the chain, not the original
 *   game function.  The last hook's trampoline calls the real original.
 *
 *   Example: CSF hooks 0x47f6a0 at priority 10, ExposeCatData hooks
 *   it at priority 20.  Call flow:
 *
 *     Game code calls function at RVA 0x47f6a0
 *       → CSF's hook (priority 10)
 *         → CSF calls trampoline (if it wants to pass through)
 *           → ExposeCatData's hook (priority 20)
 *             → ExposeCatData calls trampoline
 *               → original game function
 *
 *   Each hook can inspect args, modify them, skip the chain (by not
 *   calling the trampoline), or replace the return value — exactly
 *   as with a standalone hook, just composable now.
 *
 *
 * Graceful degradation:
 *
 *   MJ_Resolve() returns 0 if Mewjector is not present (e.g. the real
 *   system version.dll is loaded, or an older chainloader without the
 *   API).  Mods should detect this and either fall back to standalone
 *   InstallHook() or refuse to load.
 *
 *
 * Versioning:
 *
 *   MJ_Require()/MJ_Resolve() check MJ_GetVersion() >= MJ_API_VERSION.
 *   If MJ_Resolve() requests a higher API version than a player's
 *   installed Mewjector, the resolve will fail gracefully.
 *   MJ_Require() prints log messages if a player needs to update
 *   Mewjector to support a mod.
 *
 *   Mewjector is forward-compatible: new exports are added, existing
 *   ones never change signature. To purposely target an older API
 *   version, MJ_RequireVersion()/ MJ_ResolveVersion() accept an
 *   explicit API version number.
 *
 */

#ifndef MEWJECTOR_H
#define MEWJECTOR_H

#include <windows.h>

#define MJ_API_VERSION 3

/* ===================================================================
 *  Function typedefs
 * =================================================================== */

typedef int (__cdecl *MJ_fn_InstallHook)(
    UINT_PTR    rva,
    int         stolenBytes,
    void*       hookFn,
    void**      outTrampoline,
    int         priority,
    const char* owner
);

typedef int (__cdecl *MJ_fn_QueryHook)(
    UINT_PTR rva
);

typedef UINT_PTR (__cdecl *MJ_fn_AllocTypeIdPair)(
    const char* owner
);

typedef int (__cdecl *MJ_fn_RegisterName)(
    const char* category,
    const char* name,
    const char* owner
);

typedef const char* (__cdecl *MJ_fn_LookupName)(
    const char* category,
    const char* name
);

typedef UINT_PTR (__cdecl *MJ_fn_GetGameBase)(void);

typedef void (__cdecl *MJ_fn_Log)(
    const char* owner,
    const char* fmt,
    ...
);

typedef int (__cdecl *MJ_fn_VerifyHooks)(void);

typedef int (__cdecl *MJ_fn_GetVersion)(void);


/* ===================================================================
 *  API struct — filled by MJ_Resolve()
 * =================================================================== */

typedef struct {
    MJ_fn_InstallHook     InstallHook;
    MJ_fn_QueryHook       QueryHook;
    MJ_fn_AllocTypeIdPair AllocTypeIdPair;
    MJ_fn_RegisterName    RegisterName;
    MJ_fn_LookupName      LookupName;
    MJ_fn_GetGameBase     GetGameBase;
    MJ_fn_Log             Log;
    MJ_fn_VerifyHooks     VerifyHooks;
    MJ_fn_GetVersion      GetVersion;
} MewjectorAPI;


/* ===================================================================
 *  MJ_RequireVersion — Check if the requested API version is available.
 *                      Log an error if not.
 *
 *  Returns 1 if Mewjector can provide the requested API version.
 *  Returns 0 if Mewjector cannot provide the requested API version.
 *
 *  This is a static inline so the header is self-contained — no
 *  separate .c file or import lib needed.
 * =================================================================== */
static inline int MJ_RequireVersion(int apiVersion, const char* owner)
{
    HMODULE hMJ = GetModuleHandleA("version.dll");
    if (!hMJ) return 0;

    /* Check version and log a mismatch if possible, using v3 API functions. */
    MJ_fn_GetVersion GetVersion = (MJ_fn_GetVersion)GetProcAddress(hMJ, "MJ_GetVersion");
    MJ_fn_Log Log = (MJ_fn_Log)GetProcAddress(hMJ, "MJ_Log");
    if (!GetVersion) return 0;
    if (apiVersion < 3 || apiVersion > MJ_API_VERSION)
    {
        if (Log)
        {
            Log(owner, "This mod tried to request a Mewjector API version unknown to its copy of mewjector.h");
            Log(owner, "(if you are a player please report this issue to the mod's developer)");
            Log(owner, "GetVersion(): %d, MJ_API_VERSION: %d, apiVersion: %d", GetVersion(), MJ_API_VERSION, apiVersion);
        }
        return 0;
    }
    else if (GetVersion() < apiVersion)
    {
        if(Log)
        {
            Log(owner, "This mod requires a newer version of Mewjector than currently installed.");
            Log(owner, "Available API version: %d, Requested API version: %d", GetVersion(), apiVersion);
            Log(owner, "Please check for a Mewjector update at:");
            Log(owner, "https://www.nexusmods.com/mewgenics/mods/218");
            Log(owner, "https://github.com/githubuser508/mewjector");
        }
        return 0;
    }

    return 1;
}

/* ===================================================================
 *  MJ_Require — Check if the maximum known API version is available.
 *               Log an error if not.
 *
 *  Returns 1 if Mewjector can provide the requested API version.
 *  Returns 0 if Mewjector cannot provide the requested API version.
 *
 *  This is a static inline so the header is self-contained — no
 *  separate .c file or import lib needed.
 * =================================================================== */
static inline int MJ_Require(const char* owner)
{
    return MJ_RequireVersion(MJ_API_VERSION, owner);
}

/* ===================================================================
 *  MJ_ResolveVersion — Resolve functions at a requested API version.
 *
 *  Returns 1 if all functions resolved successfully.
 *  Returns 0 if Mewjector is not available (safe to call always).
 *
 *  This is a static inline so the header is self-contained — no
 *  separate .c file or import lib needed.
 * =================================================================== */
static inline int MJ_ResolveVersion(int apiVersion, MewjectorAPI* api)
{
    if (!api) return 0;
    memset(api, 0, sizeof(MewjectorAPI));

    HMODULE hMJ = GetModuleHandleA("version.dll");
    if (!hMJ) return 0;

    /* Version gate — must be a known version and at least the requested version */
    api->GetVersion = (MJ_fn_GetVersion)GetProcAddress(hMJ, "MJ_GetVersion");
    if (apiVersion < 3 || apiVersion > MJ_API_VERSION) return 0;
    if (!api->GetVersion) return 0;
    if (api->GetVersion() < apiVersion) return 0;

    /* Resolve everything else */
    #define MJ__RESOLVE(field, name) \
        api->field = (MJ_fn_##field)GetProcAddress(hMJ, "MJ_" name); \
        if (!api->field) return 0;

    /* API version 3 (baseline) */
    MJ__RESOLVE(InstallHook,     "InstallHook");
    MJ__RESOLVE(QueryHook,       "QueryHook");
    MJ__RESOLVE(AllocTypeIdPair, "AllocTypeIdPair");
    MJ__RESOLVE(RegisterName,    "RegisterName");
    MJ__RESOLVE(LookupName,      "LookupName");
    MJ__RESOLVE(GetGameBase,     "GetGameBase");
    MJ__RESOLVE(Log,             "Log");
    MJ__RESOLVE(VerifyHooks,     "VerifyHooks");

    #undef MJ__RESOLVE
    return 1;
}

/* ===================================================================
 *  MJ_Resolve — Resolve all functions at the maximum known API version.
 *
 *  Returns 1 if all functions resolved successfully.
 *  Returns 0 if Mewjector is not available (safe to call always).
 *
 *  This is a static inline so the header is self-contained — no
 *  separate .c file or import lib needed.
 * =================================================================== */
static inline int MJ_Resolve(MewjectorAPI* api)
{
    return MJ_ResolveVersion(MJ_API_VERSION, api);
}

#endif /* MEWJECTOR_H */
