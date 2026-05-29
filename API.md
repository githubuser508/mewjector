# Mewjector API v3

Mewjector v3.0 adds exported functions to version.dll that mods resolve at runtime.
Three services: hook chaining, type ID allocation, namespace collision detection.

All existing chainloader behavior is unchanged. The API is opt-in. Mods that don't
use it work exactly as before. Third-party mods doing their own inline patching are
unaffected (but if they overwrite a managed hook site, `MJ_VerifyHooks` will catch it).


## Resolving the API

Mewjector IS version.dll, so `GetModuleHandleA("version.dll")` always works:

```c
HMODULE hMJ = GetModuleHandleA("version.dll");
typedef int (__cdecl *fn_install)(UINT_PTR, int, void*, void**, int, const char*);
fn_install install = (fn_install)GetProcAddress(hMJ, "MJ_InstallHook");
```

Or use `mewjector.h` (single header, no import lib needed):

```c
#include "mewjector.h"
static MewjectorAPI mj;

// In init:
if (!MJ_Require("MyMod") || !MJ_Resolve(&mj)) return;  // not present or too old
mj.InstallHook(...);
```

`MJ_Require`/`MJ_Resolve` will check if Mewjector is present, log to the user if
they need to update Mewjector, and retrieve all functions. They fail gracefully
(return 0) if version.dll is the real system DLL.


## Versioning

`MJ_GetVersion` returns 3. The number increments when exports are added.
Existing signatures never change.

`MJ_Require`/`MJ_Resolve` gate on `MJ_GetVersion() >= MJ_API_VERSION`.

`MJ_RequireVersion`/`MJ_ResolveVersion` are variants of `MJ_Require`/`MJ_Resolve`
that accept an explicit API version number. They gate on `MJ_GetVersion() >= apiVersion`,
where `apiVersion <= MJ_API_VERSION`.

---


## Hook Chaining

`MJ_InstallHook` manages a per-RVA linked list of hooks sorted by priority.
The game entry is patched once. Additional hooks for the same RVA chain through
trampolines without touching the game code again.

```
Game -> chain[0].hookFn (lowest pri)
          -> chain[1].hookFn
               -> ...
                    -> chain[N].hookFn (highest pri)
                         -> original game function
```

Each trampoline has the original function's signature. Call it to pass through.
Don't call it to short-circuit. Modify args before calling to intercept.


### Internals

First hook on an RVA:
1. Stolen bytes copied into an original trampoline (`VirtualAlloc` RWX):
   stolen bytes + `FF 25 00 00 00 00 [return addr]`
2. Game entry overwritten with `FF 25 00 00 00 00 [target]` (14 bytes).
   Extra stolen bytes beyond 14 get NOPed. Target = first hook's fn.
3. Original bytes backed up for integrity checks.

Subsequent hooks on the same RVA:
1. New entry gets a 14-byte JMP stub trampoline (RWX).
2. Inserted into the sorted list.
3. `MJ_RebuildChain`: updates the game entry target to the new chain head
   (`VirtualProtect` round-trip), then wires each stub to the next hook
   (or original trampoline for the last). Stubs are RWX so no protect needed.


### MJ_InstallHook

```c
int __cdecl MJ_InstallHook(
    UINT_PTR    rva,            // target function RVA
    int         stolenBytes,    // bytes to steal, 0 to auto-calculate; >= 14 for manual
    void*       hookFn,         // same signature as original
    void**      outTrampoline,  // receives call-next function pointer
    int         priority,       // lower = called first
    const char* owner           // your DLL name, for logs
);
```

Returns 1/0. Failure is logged.

`stolenBytes` must land on an instruction boundary. Pass 0 to calculate automatically,
or pass a manually calculated value. Caller ensures no RIP-relative instructions in
the stolen range (they get relocated to a different address in the trampoline). First
hook establishes the count; subsequent hooks for the same RVA warn if they disagree but
use the established value.

`outTrampoline` stays valid for process lifetime. Cast it to the original fn type.

Priority conventions:
- 0-9: framework-level (CSF status interception, etc.)
- 10-49: core mod hooks
- 50-99: secondary/dependent
- 100+: observers, logging

Mutex-protected. Concurrent calls are safe but serialized.

**Example:**

```c
typedef void* (__fastcall *fn_apply_status)(void*, const char*, int, void*);
static fn_apply_status g_orig;

static void* __fastcall Hook(void* rcx, const char* name, int stacks, void* src)
{
    if (should_block(name)) return NULL;
    return g_orig(rcx, name, stacks, src);
}

// In init:
void* tramp = NULL;
mj.InstallHook(0x47f6a0, 15, Hook, &tramp, 10, "MyMod");
g_orig = (fn_apply_status)tramp;
```


### MJ_QueryHook

```c
int __cdecl MJ_QueryHook(UINT_PTR rva);
```

Returns hook count at an RVA, or 0 if unmanaged.


---


## Type ID Allocation

### MJ_AllocTypeIdPair

```c
UINT_PTR __cdecl MJ_AllocTypeIdPair(const char* owner);
```

Returns a unique base index. You own `base` and `base + 1`.

Starts at `0x1000` (well above vanilla 0-0x318). Atomically increments by 2
(`InterlockedExchangeAdd`). Monotonic, no free. IDs persist for process lifetime.

```c
UINT_PTR t = mj.AllocTypeIdPair("CSF");
// t = 0x1000, t+1 = 0x1001. Next caller gets 0x1002/0x1003.
```


---


## Namespace Registry

### MJ_RegisterName

```c
int __cdecl MJ_RegisterName(const char* category, const char* name, const char* owner);
```

Returns 1 if registered, 0 on collision (already owned by someone else).
Same-owner re-registration is idempotent (returns 1). Collisions are logged
with both owners identified.

Categories are arbitrary strings. Current conventions:

| Category        | For                                |
|-----------------|------------------------------------|
| `"status"`      | Custom status names                |
| `"formula_var"` | Custom deval variables             |
| `"conditional"` | Conditional_ dispatch names        |
| `"x_is"`        | X_is enum bindings                 |

512 total registrations. Mutex-protected.

```c
if (!mj.RegisterName("status", "ModRegen", "CSF")) {
    mj.Log("CSF", "ModRegen name collision");
    return;
}
```


### MJ_LookupName

```c
const char* __cdecl MJ_LookupName(const char* category, const char* name);
```

Returns owner string or NULL. Returned pointer is internal storage, valid for
process lifetime, don't modify it.


---


## Utilities

### MJ_GetGameBase

```c
UINT_PTR __cdecl MJ_GetGameBase(void);
```

`GetModuleHandleA(NULL)` as `UINT_PTR`. Already resolved before any mod loads.


### MJ_Log

```c
void __cdecl MJ_Log(const char* owner, const char* fmt, ...);
```

Writes to `mod_logs/chainloader.log`:
```
[HH:MM:SS.mmm] [owner] your message
```
No-op if logging is disabled. Shares the chainloader's log mutex.


### MJ_VerifyHooks

```c
int __cdecl MJ_VerifyHooks(void);
```

Checks every managed site: entry still has `FF 25`, target matches chain head.
Returns corrupted count (0 = clean). Runs automatically after all mods load.
Call manually whenever.


### MJ_GetVersion

```c
int __cdecl MJ_GetVersion(void);
```

Returns 3.


---


## Lifecycle

```
DllMain (loader lock held)
  Base dir, log, mutexes, real version.dll, config
  Mod loading deferred

First version.dll proxy call
  LoadModsOnce():
    g_mjGameBase resolved, API is live
    Phase 1: [LoadOrder] DLLs (mods can call MJ_* in DllMain)
    Phase 2: ScanPath scan
    Phase 3: GameDir (if enabled)
    Phase 4: Mewtator manifest
    MJ_VerifyHooks() runs automatically
```

Mods load sequentially via `LoadLibraryA`. Each mod's DllMain can safely call
any MJ_* function; previous mods' hooks and registrations are already complete.


---


## Raw Patching

The API does not intercept or restrict `VirtualProtect` / `VirtualAlloc` /
direct byte writes. Mid-function patches, vtable writes, arbitrary memory
access all work exactly as before.

The only thing that will cause problems: manually patching the entry point of
an RVA that Mewjector also manages. That overwrites the `FF 25` and breaks
the chain. `MJ_VerifyHooks` detects this but can't prevent it.

`MJ_AllocTypeIdPair` and `MJ_RegisterName` are voluntary. Hardcoding IDs or
names bypasses collision detection but otherwise works fine.

---


## Exports

| Symbol              | What it does                              |
|---------------------|-------------------------------------------|
| `MJ_InstallHook`   | Chainable entry-point hook                |
| `MJ_QueryHook`     | Hook count at an RVA                      |
| `MJ_AllocTypeIdPair` | Unique type index pair                  |
| `MJ_RegisterName`  | Collision-detected name registration      |
| `MJ_LookupName`    | Name ownership query                      |
| `MJ_GetGameBase`   | Game image base                           |
| `MJ_Log`           | Shared log output                         |
| `MJ_VerifyHooks`   | Hook integrity scan                       |
| `MJ_GetVersion`    | API version (3)                           |


## Limits

| Resource         | Limit     | Notes                                  |
|------------------|-----------|----------------------------------------|
| Hook sites       | Unbounded | Heap-allocated                         |
| Hooks per site   | Unbounded | Heap-allocated                         |
| Name registrations | 512     | All categories combined                |
| Type ID pairs    | ~2048     | 0x1000 to 0x1FFF before LONG wraps    |
| Owner string     | 63 chars  | Truncated                              |
| Name string      | 127 chars | Truncated                              |
| Category string  | 31 chars  | Truncated                              |
