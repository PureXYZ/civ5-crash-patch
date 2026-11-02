// Opt-in Lua garbage collection under C++ memory pressure ([probe] LuaGcStepMB=64).
//
// Each Lua database query (GameInfo..., DB.Query) is a small Lua userdata that owns
// C++ memory inside CvGameDatabase: a hash of column names per Database::Results,
// several KB each. Lua's collector only counts the userdata, so dead queries pile
// up until a cycle happens to finish: 420 MB of them while a Huge game was created,
// and the CRT heap never returns the address space it grew into.
//
// The DB DLL registers its Lua C functions through its import of lua_pushcclosure.
// That import is patched to register a 10-byte thunk per function instead. The thunk
// calls the real function. On return (the same point where a script's own
// collectgarbage() would run) it does a full collection if the CRT live estimate has
// grown LuaGcStepMB since the last one.
//
// Trigger: with AllocTracking on, its CRT live estimate. With it off (much cheaper), a
// counter of the bytes the DB DLL and MSVCP90 allocate while the thread is inside a
// wrapped call: the Results objects, their column hash and its strings.
//
// Guards: no collection while one is running (the DB's __gc finalizers are wrapped
// too), and none while Lua is in its finalizer phase (global_State.gcstate, read at
// a Lua 5.1 x86 offset that is checked against lua_getallocf first).
//
// Incremental mode (LuaGcSliceUs > 0): a full collection of the game's ~100 MB Lua heap
// holds the calling thread for 30-80 ms, a visible stutter. Instead, once due, the
// collection is spread over the following DB calls with lua_gc(LUA_GCSTEP): at most
// LuaGcSliceUs of collector work per 10 ms of wall time, until the cycle has passed its
// atomic step (which decides what is garbage) and swept. Lua's own steps then run the
// finalizers that free the DB's C++ memory. A drive that started after the atomic step
// first goes round to the next cycle. If the DB allocates another step's worth before
// the drive ends, the rest is done as one full collection.
//
// The thunks' return is also where memwarn.c shows its low address space warning in game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

typedef struct lua_State lua_State;
typedef int(__cdecl *lua_CFunction)(lua_State *);
typedef void(__cdecl *lua_pushcclosure_t)(lua_State *, lua_CFunction, int);
typedef int(__cdecl *lua_gc_t)(lua_State *, int, int);
typedef void *(__cdecl *lua_Alloc_t)(void *, void *, size_t, size_t);
typedef lua_Alloc_t(__cdecl *lua_getallocf_t)(lua_State *, void **);
typedef int(__cdecl *lua_cpcall_t)(lua_State *, lua_CFunction, void *);
typedef void(__cdecl *lua_settop_t)(lua_State *, int);

#define LUA_GCCOLLECT 2
#define LUA_GCCOUNT 3
#define LUA_GCSTEP 5
#define GCS_SWEEPSTRING 2  // the first state after the atomic step
#define GCS_PAUSE 0
#define GCS_FINALIZE 4
#define LSTATE_L_G 16      // lua_State.l_G
#define GSTATE_FREALLOC 12  // global_State.frealloc, after the 12-byte stringtable
#define GSTATE_UD 16
#define GSTATE_GCSTATE 21   // after ud (4) and currentwhite (1)
#define MAX_THUNKS 512
#define THUNK_LEN 10

static lua_pushcclosure_t real_pushcclosure;
static lua_gc_t p_lua_gc;
static lua_getallocf_t p_lua_getallocf;
static lua_cpcall_t p_lua_cpcall;
static lua_settop_t p_lua_settop;
static BYTE *g_thunks;
static lua_CFunction g_fns[MAX_THUNKS];
static volatile LONG g_nThunks, g_thunkLock;
static WCHAR g_target[64];
static LONG g_stepMb;
static volatile LONG g_nextGcMb, g_inGc;
static int g_layout;  // 0 unchecked, 1 ok, -1 unexpected (collections disabled)
static volatile LONG g_nGc, g_nDeferred, g_gcMs, g_freedMb, g_patched, g_nGcErrors;
static int g_disabled;

// Light trigger (AllocTracking=0). TLS holds the frame address of the outermost wrapped
// call on the thread; a Lua error that longjmps out leaves a stale (deeper) address,
// which the next shallower call replaces.
static int g_light;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static volatile LONG g_luaBytes, g_gcMark;
// MB counted at past collections: g_luaBytes wraps every 4 GB, so the report adds these up instead.
static LONG g_countedMb;
static WCHAR g_crtName[64];
typedef void *(__cdecl *malloc_t)(size_t);
typedef void *(__cdecl *calloc_t)(size_t, size_t);
typedef void *(__cdecl *realloc_t)(void *, size_t);
// What the wrappers call: the CRT exports, or their mimalloc replacements when MiMalloc=1.
static malloc_t c_malloc, c_new, c_newa;
static calloc_t c_calloc;
static realloc_t c_realloc;
static void *o_malloc, *o_new, *o_newa, *o_calloc, *o_realloc;  // the CRT exports

// Incremental mode. g_phase while driving: 1 = go round to the next cycle (started after
// the atomic step), 2 = wait for the atomic step, 3 = sweeping until finalize or pause.
static LONG g_sliceUs;
static int g_phase, g_driveKb;
static LONG g_driveCalls, g_nFallback;
static DWORD g_driveT0, g_driveMark;
static LONGLONG g_qpf, g_window, g_windowUsed, g_driveTicks, g_maxSliceTicks;
#define WINDOW_MS 10

static void count(size_t n)
{
    DWORD err = GetLastError();
    BYTE *frame = (BYTE *)TlsGetValue(g_tls), here;
    if (frame && &here < frame) InterlockedExchangeAdd(&g_luaBytes, (LONG)n);
    SetLastError(err);
}
static void *__cdecl l_malloc(size_t n) { count(n); return c_malloc(n); }
static void *__cdecl l_new(size_t n) { count(n); return c_new(n); }
static void *__cdecl l_newa(size_t n) { count(n); return c_newa(n); }
static void *__cdecl l_calloc(size_t n, size_t m) { count(n * m); return c_calloc(n, m); }
static void *__cdecl l_realloc(void *p, size_t n) { count(n); return c_realloc(p, n); }

static int due(void)
{
    if (g_disabled) return 0;
    if (g_light) return ((DWORD)g_luaBytes - (DWORD)g_gcMark) >= ((DWORD)g_stepMb << 20);
    return alloc_est_live_mb() >= g_nextGcMb;
}

void luagc_entry(void);
// thunk: mov eax, <fn>; jmp luagc_entry. Same stack as a direct call: [esp+4] = L.
__asm__(".text\n.globl _luagc_entry\n_luagc_entry:\n"
        "\tpushl 4(%esp)\n"
        "\tpushl %eax\n"
        "\tcall _luagc_call\n"
        ".globl _luagc_entry_ret\n_luagc_entry_ret:\n"
        "\taddl $8, %esp\n"
        "\tret\n");
extern const BYTE luagc_entry_ret[];

// Which Lua state each thread's wrapped call runs on, for the freeze watchdog (hang.c): `arg`
// is the address of luagc_call's first argument, so the word below it on that thread's stack
// is the return address into luagc_entry while the call is running. A Lua error that longjmps
// out leaves the slot set, but then that word no longer matches. Written arg first (to NULL),
// then L, then arg, so a reader that sees a matching arg also sees its L.
#define MAX_CALL_SLOTS 32
static struct { volatile DWORD tid; lua_State *volatile L; void *volatile arg; } g_calls[MAX_CALL_SLOTS];
static __thread int t_slot = -1;

static int call_slot(void)
{
    if (t_slot >= 0) return t_slot;
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < MAX_CALL_SLOTS; i++)
        if (g_calls[i].tid == me || InterlockedCompareExchange((volatile LONG *)&g_calls[i].tid, (LONG)me, 0) == 0)
            return t_slot = i;
    return t_slot = MAX_CALL_SLOTS;  // table full: this thread isn't tracked
}

lua_State *luagc_thread_call(DWORD tid, void **arg, const void **ret)
{
    *ret = luagc_entry_ret;
    *arg = NULL;
    for (int i = 0; i < MAX_CALL_SLOTS; i++)
        if (g_calls[i].tid == tid) {
            *arg = g_calls[i].arg;
            return *arg ? g_calls[i].L : NULL;
        }
    return NULL;
}

static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

static BYTE *gcstate_of(lua_State *L)
{
    BYTE *g = *(BYTE **)((BYTE *)L + LSTATE_L_G);
    if (g_layout == 0) {
        void *ud = NULL;
        lua_Alloc_t f = p_lua_getallocf(L, &ud);
        int ok = readable(g, GSTATE_GCSTATE + 1) && *(lua_Alloc_t *)(g + GSTATE_FREALLOC) == f &&
                 *(void **)(g + GSTATE_UD) == ud && g[GSTATE_GCSTATE] <= GCS_FINALIZE;
        g_layout = ok ? 1 : -1;
        log_event("luagc: Lua 5.1 global_State layout %s (L=%p g=%p frealloc=%p)",
                  ok ? "confirmed" : "UNEXPECTED, collections disabled", (void *)L, (void *)g, (void *)f);
    }
    return g_layout > 0 ? g + GSTATE_GCSTATE : NULL;
}

// A collection or step can raise a Lua error: a __gc metamethod's (Lua 5.1 runs them
// unprotected) or out of memory. Unprotected, it would longjmp out past the g_inGc reset and
// stop all later collections, so they run inside lua_cpcall (g_inGc held: one at a time).
static int g_gcWhat, g_gcResult;
static int __cdecl gc_protected(lua_State *L)
{
    g_gcResult = p_lua_gc(L, g_gcWhat, 0);
    return 0;
}

static int gc_call(lua_State *L, int what)
{
    g_gcWhat = what;
    g_gcResult = 0;
    if (p_lua_cpcall(L, gc_protected, NULL) == 0) return 1;
    p_lua_settop(L, -2);  // the error object
    if (InterlockedIncrement(&g_nGcErrors) <= 5) log_event("luagc: a Lua error during a collection (a __gc metamethod, or out of memory); skipped");
    return 0;
}

// A finalizer's error leaves the collector in its finalize phase, which collect() takes for
// "a finalizer is running" and waits on, and Lua's own steps may not come for a long time
// (luagctest: no further collection at all). Each step takes the failing finalizer off the list
// before calling it, so these steps get through the rest.
static void finish_finalize(lua_State *L, const BYTE *gcstate)
{
    for (int i = 0; i < 100000 && *gcstate == GCS_FINALIZE; i++) gc_call(L, LUA_GCSTEP);
}

// lua_gc(L, what, 0), or -1 after a Lua error (popped; the collector state stays consistent).
static int safe_gc(lua_State *L, int what)
{
    if (gc_call(L, what)) return g_gcResult;
    BYTE *gcstate = gcstate_of(L);
    if (gcstate) finish_finalize(L, gcstate);
    return -1;
}

static void collect(lua_State *L)
{
    if (InterlockedCompareExchange(&g_inGc, 1, 0)) return;  // a finalizer of our own collection
    BYTE *gcstate = gcstate_of(L);
    if (!gcstate) {
        g_disabled = 1;
    } else if (*gcstate == GCS_FINALIZE) {
        InterlockedIncrement(&g_nDeferred);  // Lua is running finalizers: try again on the next call
    } else {
        LONG before = g_light ? 0 : alloc_est_live_mb();
        int kbBefore = p_lua_gc(L, LUA_GCCOUNT, 0);
        DWORD t0 = GetTickCount();
        safe_gc(L, LUA_GCCOLLECT);
        DWORD ms = GetTickCount() - t0;
        LONG n = InterlockedIncrement(&g_nGc);
        InterlockedExchangeAdd(&g_gcMs, (LONG)ms);
        int log = n <= 100 || n % 100 == 0;
        if (g_light) {
            LONG counted = (LONG)(((DWORD)g_luaBytes - (DWORD)g_gcMark) >> 20);
            g_gcMark = g_luaBytes;
            g_countedMb += counted;
            if (log)
                log_event("luagc: collection %ld: %ld MB allocated in DB calls since the last, Lua %d -> %d KB, %lu ms",
                          n, counted, kbBefore, p_lua_gc(L, LUA_GCCOUNT, 0), ms);
        } else {
            LONG after = alloc_est_live_mb();
            g_nextGcMb = after + g_stepMb;
            if (before > after) InterlockedExchangeAdd(&g_freedMb, before - after);
            if (log)
                log_event("luagc: collection %ld: CRT live %ld -> %ld MB, Lua %d -> %d KB, %lu ms", n, before, after,
                          kbBefore, p_lua_gc(L, LUA_GCCOUNT, 0), ms);
        }
    }
    InterlockedExchange(&g_inGc, 0);
}

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void drive_end(lua_State *L, const char *how)
{
    LONG n = InterlockedIncrement(&g_nGc);
    LONG ms = (LONG)(g_driveTicks * 1000 / g_qpf);
    InterlockedExchangeAdd(&g_gcMs, ms);
    g_countedMb += (LONG)(((DWORD)g_driveMark - (DWORD)g_gcMark) >> 20);
    if (n <= 100 || n % 100 == 0)
        log_event("luagc: collection %ld: %ld MB allocated in DB calls since the last, Lua %d -> %d KB, %s over %ld DB calls in %lu ms (%ld ms of work, longest slice %ld us)",
                  n, (LONG)(((DWORD)g_driveMark - (DWORD)g_gcMark) >> 20), g_driveKb, p_lua_gc(L, LUA_GCCOUNT, 0), how,
                  g_driveCalls, GetTickCount() - g_driveT0, ms, (LONG)(g_maxSliceTicks * 1000000 / g_qpf));
    g_gcMark = g_driveMark;
    g_phase = 0;
}

// One slice of an incremental drive (g_inGc held). Returns with the drive still on
// (g_phase != 0) until the cycle that started after the trigger has swept.
static void drive(lua_State *L, BYTE *gcstate)
{
    if (!g_phase) {
        g_phase = *gcstate >= GCS_SWEEPSTRING ? 1 : 2;
        g_driveKb = p_lua_gc(L, LUA_GCCOUNT, 0);
        g_driveT0 = GetTickCount();
        g_driveMark = g_luaBytes;  // what this collection is for; counting goes on for the next
        g_driveCalls = 0;
        g_driveTicks = g_maxSliceTicks = 0;
    }
    if (((DWORD)g_luaBytes - (DWORD)g_driveMark) >= ((DWORD)g_stepMb << 20)) {  // fell behind: finish at once
        LONGLONG t0 = qpc();
        safe_gc(L, LUA_GCCOLLECT);
        g_driveTicks += qpc() - t0;
        g_driveCalls++;
        InterlockedIncrement(&g_nFallback);
        drive_end(L, "finished by a full collection");
        return;
    }
    LONGLONG now = qpc(), win = now / (g_qpf * WINDOW_MS / 1000);
    if (win != g_window) g_window = win, g_windowUsed = 0;
    LONGLONG budget = g_qpf * g_sliceUs / 1000000 - g_windowUsed;
    if (budget <= 0) return;
    g_driveCalls++;
    LONGLONG t0 = now, t;
    do {
        int cycleDone = safe_gc(L, LUA_GCSTEP);
        if (cycleDone < 0) { t = qpc(); break; }  // try the next slice again later
        BYTE s = *gcstate;
        if (g_phase == 1 && (cycleDone || s < GCS_SWEEPSTRING)) g_phase = 2;
        else if (g_phase == 2 && (cycleDone || s >= GCS_SWEEPSTRING)) g_phase = 3;
        if (g_phase == 3 && (cycleDone || s == GCS_FINALIZE || s == GCS_PAUSE)) g_phase = 4;
        t = qpc();
    } while (g_phase != 4 && t - t0 < budget);
    g_windowUsed += t - t0;
    g_driveTicks += t - t0;
    if (t - t0 > g_maxSliceTicks) g_maxSliceTicks = t - t0;
    if (g_phase == 4) drive_end(L, "incremental");
}

static void collect_slice(lua_State *L)
{
    if (InterlockedCompareExchange(&g_inGc, 1, 0)) return;
    BYTE *gcstate = gcstate_of(L);
    if (!gcstate) g_disabled = 1, g_phase = 0;
    else if (*gcstate == GCS_FINALIZE) {
        // Maybe inside a finalizer, so no step here: Lua's own steps run them. If the
        // atomic step came after the drive started (phases 2 and 3), that's the end.
        if (g_phase >= 2) drive_end(L, "incremental");
        else if (!g_phase) InterlockedIncrement(&g_nDeferred);
    } else drive(L, gcstate);
    InterlockedExchange(&g_inGc, 0);
}

__attribute__((used)) int __cdecl luagc_call(lua_CFunction fn, lua_State *L)
{
    BYTE mark, *outer = NULL;
    int slot = call_slot();
    lua_State *prevL = NULL;
    void *prevArg = NULL;
    if (slot < MAX_CALL_SLOTS) {
        prevL = g_calls[slot].L, prevArg = g_calls[slot].arg;
        g_calls[slot].arg = NULL;
        g_calls[slot].L = L;
        g_calls[slot].arg = (void *)&fn;
    }
    if (g_light) {
        BYTE *f = (BYTE *)TlsGetValue(g_tls);
        if (!f || &mark >= f) TlsSetValue(g_tls, outer = &mark);  // outermost (or the recorded one is stale)
    }
    int r = fn(L);
    if (slot < MAX_CALL_SLOTS) {
        g_calls[slot].arg = NULL;
        g_calls[slot].L = prevL;
        g_calls[slot].arg = prevArg;
    }
    if (outer) TlsSetValue(g_tls, NULL);
    if (!g_inGc && (g_phase || due())) {
        if (g_sliceUs > 0) collect_slice(L);
        else collect(L);
    }
    if (!g_inGc && memwarn_due()) {  // the low address space warning (memwarn.c), not while finalizers run
        BYTE *gcstate = gcstate_of(L);
        if (gcstate && *gcstate != GCS_FINALIZE) memwarn_show(L);
    }
    return r;
}

static lua_CFunction thunk_for(lua_CFunction fn)
{
    LONG n = g_nThunks;
    for (LONG i = 0; i < n; i++)
        if (g_fns[i] == fn) return (lua_CFunction)(g_thunks + i * THUNK_LEN);
    while (InterlockedExchange(&g_thunkLock, 1)) YieldProcessor();
    lua_CFunction result = fn;  // table full: register the function unwrapped
    n = g_nThunks;
    for (LONG i = 0; i < n && result == fn; i++)
        if (g_fns[i] == fn) result = (lua_CFunction)(g_thunks + i * THUNK_LEN);
    if (result == fn && n < MAX_THUNKS) {
        BYTE *t = g_thunks + n * THUNK_LEN;
        LONG rel = (LONG)((BYTE *)luagc_entry - (t + THUNK_LEN));
        t[0] = 0xB8;
        memcpy(t + 1, &fn, 4);
        t[5] = 0xE9;
        memcpy(t + 6, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), t, THUNK_LEN);
        g_fns[n] = fn;
        InterlockedExchange(&g_nThunks, n + 1);
        result = (lua_CFunction)t;
    }
    InterlockedExchange(&g_thunkLock, 0);
    return result;
}

static void __cdecl hook_pushcclosure(lua_State *L, lua_CFunction fn, int n)
{
    real_pushcclosure(L, fn ? thunk_for(fn) : fn, n);
}

// Points every IAT slot of the module that holds `from` at `to`.
static int patch_iat(BYTE *base, const void *from, void *to)
{
    if (!from) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); dir.Size && d->Name; d++)
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk); t->u1.Function; t++) {
            if (t->u1.Function != (DWORD)(ULONG_PTR)from) continue;
            DWORD old;
            if (!VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old)) continue;
            InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)to);
            VirtualProtect(&t->u1.Function, 4, old, &old);
            n++;
        }
    return n;
}

// Light trigger: count the module's CRT allocations made inside wrapped calls. A slot
// may hold the CRT export or, if mialloc.c got there first, its replacement.
static int patch_pair(BYTE *base, void *crt, void *now, void *to)
{
    return patch_iat(base, crt, to) + (now != crt ? patch_iat(base, now, to) : 0);
}

static int patch_counting(BYTE *base)
{
    return patch_pair(base, o_malloc, (void *)c_malloc, l_malloc) + patch_pair(base, o_calloc, (void *)c_calloc, l_calloc) +
           patch_pair(base, o_realloc, (void *)c_realloc, l_realloc) + patch_pair(base, o_new, (void *)c_new, l_new) +
           patch_pair(base, o_newa, (void *)c_newa, l_newa);
}

static void patch_target(BYTE *base)
{
    if (!real_pushcclosure) {
        HMODULE lua = GetModuleHandleW(L"lua51_Win32.dll");
        if (!lua) { log_event("luagc: lua51_Win32.dll not loaded; not hooked"); return; }
        real_pushcclosure = (lua_pushcclosure_t)GetProcAddress(lua, "lua_pushcclosure");
        p_lua_gc = (lua_gc_t)GetProcAddress(lua, "lua_gc");
        p_lua_getallocf = (lua_getallocf_t)GetProcAddress(lua, "lua_getallocf");
        p_lua_cpcall = (lua_cpcall_t)GetProcAddress(lua, "lua_cpcall");
        p_lua_settop = (lua_settop_t)GetProcAddress(lua, "lua_settop");
        if (!real_pushcclosure || !p_lua_gc || !p_lua_getallocf || !p_lua_cpcall || !p_lua_settop) {
            log_event("luagc: lua51_Win32.dll lacks lua_pushcclosure/lua_gc/lua_getallocf/lua_cpcall/lua_settop; not hooked");
            real_pushcclosure = NULL;
            return;
        }
    }
    int n = patch_iat(base, real_pushcclosure, hook_pushcclosure);
    InterlockedExchangeAdd(&g_patched, n);
    int a = g_light ? patch_counting(base) : 0;
    log_event("luagc: %s lua_pushcclosure import of %ls at %p (%d allocation imports counted)",
              n ? "patched" : "found no", g_target, (void *)base, a);
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } LgUstr;
typedef struct { ULONG Flags; const LgUstr *FullDllName; const LgUstr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } LgNotifyData;
typedef VOID(CALLBACK *LgNotify_t)(ULONG reason, const LgNotifyData *data, PVOID ctx);
typedef LONG(NTAPI *LgRegister_t)(ULONG, LgNotify_t, PVOID, PVOID *);

static VOID CALLBACK on_dll_notify(ULONG reason, const LgNotifyData *data, PVOID ctx)
{
    (void)ctx;
    if (reason != 1 /* loaded */ || !data->BaseDllName) return;
    int len = data->BaseDllName->Length / 2;
    if (len == lstrlenW(g_target) && _wcsnicmp(data->BaseDllName->Buffer, g_target, len) == 0)
        patch_target((BYTE *)data->DllBase);
}

void luagc_init(const WCHAR *ini)
{
    g_stepMb = GetPrivateProfileIntW(L"probe", L"LuaGcStepMB", 64, ini);
    if (g_stepMb <= 0) return;
    if (g_stepMb > 2048) g_stepMb = 2048;  // MB << 20 must fit the 32-bit byte counters
    g_sliceUs = GetPrivateProfileIntW(L"probe", L"LuaGcSliceUs", 1000, ini);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    GetPrivateProfileStringW(L"probe", L"LuaGcModule", L"CvGameDatabaseWin32Final Release.dll", g_target, 64, ini);
    g_light = !alloc_enabled();
    if (!g_light) g_sliceUs = 0;  // incremental mode uses the light trigger's counter
    if (g_light) {
        GetPrivateProfileStringW(L"probe", L"LuaGcCrt", L"msvcr90.dll", g_crtName, 64, ini);
        HMODULE crt = GetModuleHandleW(g_crtName);
        g_tls = TlsAlloc();
        if (crt) {
            o_malloc = (void *)GetProcAddress(crt, "malloc");
            o_calloc = (void *)GetProcAddress(crt, "calloc");
            o_realloc = (void *)GetProcAddress(crt, "realloc");
            o_new = (void *)GetProcAddress(crt, "??2@YAPAXI@Z");
            o_newa = (void *)GetProcAddress(crt, "??_U@YAPAXI@Z");
            c_malloc = (malloc_t)mialloc_replacement(o_malloc);
            c_calloc = (calloc_t)mialloc_replacement(o_calloc);
            c_realloc = (realloc_t)mialloc_replacement(o_realloc);
            c_new = (malloc_t)mialloc_replacement(o_new);
            c_newa = (malloc_t)mialloc_replacement(o_newa);
        }
        if (!c_malloc || g_tls == TLS_OUT_OF_INDEXES) { log_event("luagc: %ls not usable for the light trigger; off", g_crtName); return; }
    }
    g_thunks = (BYTE *)VirtualAlloc(NULL, MAX_THUNKS * THUNK_LEN, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                    PAGE_EXECUTE_READWRITE);
    if (!g_thunks) { log_event("luagc: no thunk page; off"); return; }
    if (g_light) {
        if (g_sliceUs > 0)
            log_event("luagc: on, a Lua collection every %ld MB allocated inside DB calls (light trigger, %ls), incremental: at most %ld us of work per %d ms, target %ls",
                      g_stepMb, g_crtName, g_sliceUs, WINDOW_MS, g_target);
        else
            log_event("luagc: on, full Lua collection every %ld MB allocated inside DB calls (light trigger, %ls), target %ls",
                      g_stepMb, g_crtName, g_target);
        HMODULE cpp = GetModuleHandleW(L"MSVCP90.dll");  // the std::string keys of the column hash
        if (cpp) log_event("luagc: counting %d allocation imports in MSVCP90.dll", patch_counting((BYTE *)cpp));
    } else {
        g_nextGcMb = alloc_est_live_mb() + g_stepMb;
        log_event("luagc: on, full Lua collection every %ld MB of CRT growth, target %ls", g_stepMb, g_target);
    }

    HMODULE target = GetModuleHandleW(g_target);
    if (target) patch_target((BYTE *)target);
    LgRegister_t reg = (LgRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (reg) reg(0, on_dll_notify, NULL, &cookie);  // a reload, or a target loaded later (tests)
}

void luagc_report(void)
{
    if (!g_stepMb || !g_thunks) return;
    if (g_nGcErrors) log_event("luagc: %ld Lua errors during collections (skipped)", g_nGcErrors);
    if (g_light)
        log_event("luagc: %ld collections (%ld ms total%s%ld finished by a full collection), %ld MB allocated in DB calls, %ld deferred, %ld functions wrapped, %ld imports patched",
                  g_nGc, g_gcMs, g_sliceUs > 0 ? ", incremental, " : ", ", g_nFallback,
                  g_countedMb + (LONG)(((DWORD)g_luaBytes - (DWORD)g_gcMark) >> 20), g_nDeferred, g_nThunks, g_patched);
    else
        log_event("luagc: %ld collections (%ld ms total, ~%ld MB freed), %ld deferred, %ld functions wrapped, %ld imports patched",
                  g_nGc, g_gcMs, g_freedMb, g_nDeferred, g_nThunks, g_patched);
}
