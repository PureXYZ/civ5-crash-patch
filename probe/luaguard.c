// Guard against the game's crash on quitting ([probe] LuaUnrefGuard=1, the default).
//
// At exit the exe (exe+0x50523d) releases a registry reference with luaL_unref on a
// lua_State whose memory has already been freed and unmapped; luaL_unref then
// faults reading L->l_G (lua51_Win32.dll+0x105b). Seen in three sessions, with and
// without luagc and mimalloc. The luaL_unref imports of every module are pointed at
// a wrapper that skips the call when the state has been closed, or its memory is no
// longer readable. Readability alone is not enough: after lua_close the small lua_State
// can still sit in mapped heap memory while the registry's array has been released,
// and luaL_unref then faults writing it (lua51_Win32.dll+0x3b05, seen 2026-10-28). So
// lua_close is wrapped too and remembers each closed global_State (from before the close: releases
// made while it closes, by __gc finalizers, are skipped too); lua_newstate forgets
// one again if a new state reuses its address. A live state is never skipped.
// Third form (2026-10-29): the exe's Lua-ref destructor (exe+0x505220) holds the coroutine
// (lua_newthread) it took the ref on, and that thread can already be garbage collected while
// the main state lives on. Its lua_State block is still mapped, but its stack is freed, so
// luaL_unref's push through L->top writes into released memory (lua51+0x3b05 again; under
// mimalloc the stack's segment may be unmapped). The registry is shared by every thread of a
// state, so a registry unref runs on the state's main thread instead, which is never collected.
// Fourth part (2026-10-29): the ref destructor doesn't take the exe's Lua lock, and the game-core
// thread runs GameEvents on that same main thread under the lock (a GameEvent crashed on a
// corrupt table in the first session with the reroute). So a registry unref now runs only under
// the exe's Lua lock (found from its lock wrapper's code bytes), taken with TryEnter so the guard
// never waits on it. When another thread holds it, the unref is queued and done by the next one
// that gets the lock. Without the lock (another exe), only a thread whose stack is gone is rerouted.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

typedef struct lua_State lua_State;
typedef void(__cdecl *luaL_unref_t)(lua_State *, int, int);
typedef void(__cdecl *lua_close_t)(lua_State *);
typedef lua_State *(__cdecl *lua_newstate_t)(void *, void *);

#define LSTATE_L_G 16      // lua_State.l_G (Lua 5.1, x86)
#define LSTATE_SIZE 0x48   // enough of lua_State for luaL_unref's reads
#define GSTATE_SIZE 0x80   // up to and past l_registry (+0x60)
#define GSTATE_MAINTHREAD 0x70  // global_State.mainthread (lua_newstate stores it there)
#define LSTATE_TOP 8       // lua_State.top
#define LUA_REGISTRYINDEX (-10000)
// The exe's Lua lock/unlock wrappers, `mov ecx, <CRITICAL_SECTION>; jmp <wrapper>` (GameExe
// luaLock / luaUnlock; DX11 exe+0x5042a0 / +0x5042b0).
#define MAX_DEFERRED 4096

static luaL_unref_t real_unref;
static lua_close_t real_close;
static lua_newstate_t real_newstate;

#define MAX_CLOSED 16
static void *volatile g_closed[MAX_CLOSED];  // global_States of closed Lua states
static HMODULE g_lua;
static volatile LONG g_skipped, g_slots, g_rerouted, g_dead;
static CRITICAL_SECTION *g_luaLock;  // the exe's, or NULL
static volatile LONG g_regUnrefs, g_inLock, g_outside, g_deferred, g_drained, g_dropped, g_noRef;
static struct { void *g; int ref; } g_queue[MAX_DEFERRED];
static volatile LONG g_queueLen, g_queueSpin;

static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m)) return 0;
    DWORD r = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
              PAGE_EXECUTE_WRITECOPY;
    if (m.State != MEM_COMMIT || !(m.Protect & r) || (m.Protect & PAGE_GUARD)) return 0;
    if ((const BYTE *)p + n > (const BYTE *)m.BaseAddress + m.RegionSize)
        return readable((const BYTE *)m.BaseAddress + m.RegionSize, (const BYTE *)p + n - ((const BYTE *)m.BaseAddress + m.RegionSize));
    return 1;
}

// The last global_State whose memory and main thread checked out. Both live until
// lua_close (guard_close clears this first), so the per-call checks can skip their two
// VirtualQuery calls for it: ~2,900 registry unrefs a second on the UI thread, and the
// queries were ~4% of that thread's time.
static void *volatile g_okG;

static void *global_of(lua_State *L) { return *(void *const *)((const BYTE *)L + LSTATE_L_G); }
static lua_State *mainthread_of(void *g) { return *(lua_State *const *)((const BYTE *)g + GSTATE_MAINTHREAD); }

static int is_closed(void *g)
{
    for (int i = 0; i < MAX_CLOSED; i++)
        if (g_closed[i] == g) return 1;
    return 0;
}

static void queue_lock(void);
static void queue_unlock(void);

// Remembers g as closed: a free slot, else the oldest entry (a state closed long ago).
static void mark_closed(void *g)
{
    static volatile LONG next;
    if (is_closed(g)) return;
    for (int i = 0; i < MAX_CLOSED; i++)
        if (!g_closed[i] && InterlockedCompareExchangePointer((void *volatile *)&g_closed[i], g, NULL) == NULL) return;
    LONG k = InterlockedIncrement(&next);
    g_closed[(DWORD)k % MAX_CLOSED] = g;
    if (k == 1) log_event("luaguard: more than %d closed Lua states; forgetting the oldest", MAX_CLOSED);
}

static void __cdecl guard_close(lua_State *L)
{
    void *g = L && readable(L, LSTATE_SIZE) ? global_of(L) : NULL;
    if (g) {
        // Closed from here on: unrefs made while it closes (C++ ref destructors run by __gc
        // finalizers) are skipped too, so none of them can put g back into g_okG.
        mark_closed(g);
        g_okG = NULL;
        queue_lock();  // its deferred unrefs go with it, also if a new state later reuses g
        LONG k = 0;
        for (LONG i = 0; i < g_queueLen; i++)
            if (g_queue[i].g != g) g_queue[k++] = g_queue[i];
        InterlockedExchangeAdd(&g_dropped, g_queueLen - k);
        g_queueLen = k;
        queue_unlock();
    }
    real_close(L);
    g_okG = NULL;
    if (g) log_event("luaguard: Lua state %p closed (global_State %p)", (void *)L, g);
}

static lua_State *__cdecl guard_newstate(void *f, void *ud)
{
    lua_State *L = real_newstate(f, ud);
    if (L) {
        void *g = global_of(L);
        for (int i = 0; i < MAX_CLOSED; i++)
            if (g_closed[i] == g) InterlockedCompareExchangePointer((void *volatile *)&g_closed[i], NULL, g);
    }
    return L;
}

// The main thread of g, or NULL when g has no valid one.
static lua_State *valid_main(void *g)
{
    lua_State *M = mainthread_of(g);
    return readable(M, LSTATE_SIZE) && global_of(M) == g ? M : NULL;
}

static lua_State *checked_main(void *g)
{
    if (g == g_okG) return mainthread_of(g);
    lua_State *M = !is_closed(g) && readable(g, GSTATE_SIZE) ? valid_main(g) : NULL;
    if (M) g_okG = g;
    return M;
}

static void queue_lock(void) { while (InterlockedExchange(&g_queueSpin, 1)) YieldProcessor(); }
static void queue_unlock(void) { InterlockedExchange(&g_queueSpin, 0); }

// Caller holds the Lua lock: runs the unrefs queued while another thread held it.
static void drain_queue(void)
{
    for (;;) {
        queue_lock();
        LONG n = g_queueLen;
        void *g = n ? g_queue[n - 1].g : NULL;
        int ref = n ? g_queue[n - 1].ref : 0;
        if (n) g_queueLen = n - 1;
        queue_unlock();
        if (!n) return;
        lua_State *M = !is_closed(g) && readable(g, GSTATE_SIZE) ? valid_main(g) : NULL;
        if (M) {
            real_unref(M, LUA_REGISTRYINDEX, ref);
            InterlockedIncrement(&g_drained);
        } else {
            InterlockedIncrement(&g_dropped);
        }
    }
}

static void __cdecl guard_unref(lua_State *L, int t, int ref)
{
    if (ref < 0) {  // LUA_NOREF / LUA_REFNIL: luaL_unref returns at once (lua51+0x3ac7), so no checks or lock
        InterlockedIncrement(&g_noRef);
        return;
    }
    void *okG = g_okG, *g = okG;
    int ok = okG && L == mainthread_of(okG);  // the checked main thread itself
    if (!ok && readable(L, LSTATE_SIZE)) {
        g = global_of(L);
        ok = g == okG || (!is_closed(g) && readable(g, GSTATE_SIZE));
    }
    if (!ok) {
        if (InterlockedIncrement(&g_skipped) <= 5)
            log_event("luaguard: skipped luaL_unref(%p, %d, %d) on a closed Lua state (caller %p)", (void *)L, t, ref,
                      __builtin_return_address(0));
        return;
    }
    if (t != LUA_REGISTRYINDEX) {
        real_unref(L, t, ref);
        return;
    }
    InterlockedIncrement(&g_regUnrefs);
    lua_State *M = checked_main(g);
    if (!M) {
        if (InterlockedIncrement(&g_skipped) <= 5)
            log_event("luaguard: skipped luaL_unref(%p, %d, %d): its global_State %p has no valid main thread (caller %p)",
                      (void *)L, t, ref, g, __builtin_return_address(0));
        return;
    }
    int stackGone = M != L && !readable(*(void *const *)((const BYTE *)L + LSTATE_TOP), 16);
    if (stackGone && InterlockedIncrement(&g_dead) <= 5)
        log_event("luaguard: luaL_unref(%p, %d, %d) on a collected Lua thread (stack gone), done on the main thread %p (caller %p)",
                  (void *)L, t, ref, (void *)M, __builtin_return_address(0));
    if (!g_luaLock) {  // an unknown exe: reroute only a thread whose stack is gone
        if (stackGone) {
            InterlockedIncrement(&g_rerouted);
            L = M;
        }
        real_unref(L, t, ref);
        return;
    }
    DWORD me = GetCurrentThreadId(), owner = (DWORD)(ULONG_PTR)g_luaLock->OwningThread;
    if (owner == me) {
        InterlockedIncrement(&g_inLock);
    } else if (TryEnterCriticalSection(g_luaLock)) {
        if (InterlockedIncrement(&g_outside) <= 5)
            log_event("luaguard: luaL_unref(%p, %d, %d) outside the game's Lua lock on thread %lu (caller %p); done under the lock",
                      (void *)L, t, ref, me, __builtin_return_address(0));
    } else {  // another thread is in Lua: no Lua call from here now (the queue is checked again when drained)
        queue_lock();
        // Checked again under the queue's lock: guard_close marks g closed before it clears g's
        // entries under this lock, so an entry added after that would outlive the state (and
        // reach a new state that reuses g's address).
        int closed = is_closed(g);
        LONG n = g_queueLen;
        if (!closed && n < MAX_DEFERRED) {
            g_queue[n].g = g;
            g_queue[n].ref = ref;
            g_queueLen = n + 1;
        }
        queue_unlock();
        if (closed || n >= MAX_DEFERRED) InterlockedIncrement(&g_dropped);
        if (InterlockedIncrement(&g_deferred) <= 5)
            log_event("luaguard: luaL_unref(%p, %d, %d) on thread %lu while thread %lu holds the game's Lua lock; deferred (caller %p)",
                      (void *)L, t, ref, me, owner, __builtin_return_address(0));
        return;
    }
    if (is_closed(g)) {  // closed on another thread since the checks above
        InterlockedIncrement(&g_skipped);
    } else {
        if (M != L) InterlockedIncrement(&g_rerouted);
        real_unref(M, t, ref);
    }
    drain_queue();
    if (owner != me) LeaveCriticalSection(g_luaLock);
}

// Test hook: a stand-in for the exe's Lua lock.
__declspec(dllexport) void CivProbe_SetLuaLock(CRITICAL_SECTION *cs) { g_luaLock = cs; }

// The CRITICAL_SECTION the exe's Lua lock/unlock wrappers use, after checking their code bytes.
static CRITICAL_SECTION *find_lua_lock(BYTE *exe, const GameExe *g)
{
    static const BYTE kWrap[] = {0x51, 0xFF, 0x15};  // push ecx; call [Enter/LeaveCriticalSection]
    if (!g || !g->luaLock) return NULL;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    BYTE *end = exe + nt->OptionalHeader.SizeOfImage, *lk = exe + g->luaLock, *un = exe + g->luaUnlock;
    if (un + 10 > end || lk[0] != 0xB9 || lk[5] != 0xE9 || un[0] != 0xB9 || un[5] != 0xE9 || memcmp(lk + 1, un + 1, 4))
        return NULL;
    BYTE *cs = *(BYTE **)(lk + 1), *enter = lk + 10 + *(LONG *)(lk + 6), *leave = un + 10 + *(LONG *)(un + 6);
    if (cs < exe || cs + sizeof(CRITICAL_SECTION) > end) return NULL;
    if (enter < exe || enter + 3 > end || leave < exe || leave + 3 > end) return NULL;
    if (memcmp(enter, kWrap, 3) || memcmp(leave, kWrap, 3)) return NULL;
    return (CRITICAL_SECTION *)cs;
}

const char *luaguard_check(BYTE *exe, const GameExe *g) { return find_lua_lock(exe, g) ? NULL : "Lua lock wrappers"; }

// For memwarn.c (main thread only): the lock the guard uses (or the test's stand-in), else the
// exe's, found the same way, also when the guard is off.
CRITICAL_SECTION *luaguard_lua_lock(void)
{
    static CRITICAL_SECTION *found;
    static int looked;
    if (g_luaLock) return g_luaLock;
    if (!looked) {
        BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
        found = find_lua_lock(exe, game_exe(exe));
        looked = 1;
    }
    return found;
}

static void patch_module(BYTE *base)
{
    if ((HMODULE)base == g_lua) return;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); dir.Size && d->Name; d++)
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk); t->u1.Function; t++) {
            void *to = t->u1.Function == (DWORD)(ULONG_PTR)real_unref      ? (void *)guard_unref
                       : t->u1.Function == (DWORD)(ULONG_PTR)real_close    ? (void *)guard_close
                       : t->u1.Function == (DWORD)(ULONG_PTR)real_newstate ? (void *)guard_newstate
                                                                           : NULL;
            if (!to) continue;
            DWORD old;
            if (!VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old)) continue;
            InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)to);
            VirtualProtect(&t->u1.Function, 4, old, &old);
            n++;
        }
    if (n) {
        WCHAR name[MAX_PATH];
        if (!GetModuleFileNameW((HMODULE)base, name, MAX_PATH)) name[0] = 0;
        const WCHAR *s = wcsrchr(name, 92);  // 92 = backslash
        log_event("luaguard: guarding %d Lua import(s) (luaL_unref/lua_close/lua_newstate) of %ls", n, s ? s + 1 : name);
        InterlockedExchangeAdd(&g_slots, n);
    }
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } LqUstr;
typedef struct { ULONG Flags; const LqUstr *FullDllName; const LqUstr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } LqNotifyData;
typedef VOID(CALLBACK *LqNotify_t)(ULONG reason, const LqNotifyData *data, PVOID ctx);
typedef LONG(NTAPI *LqRegister_t)(ULONG, LqNotify_t, PVOID, PVOID *);

static VOID CALLBACK on_dll_notify(ULONG reason, const LqNotifyData *data, PVOID ctx)
{
    (void)ctx;
    if (reason == 1 /* loaded */) patch_module((BYTE *)data->DllBase);
}

void luaguard_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"LuaUnrefGuard", 1, ini)) return;
    g_lua = GetModuleHandleW(L"lua51_Win32.dll");
    real_unref = g_lua ? (luaL_unref_t)GetProcAddress(g_lua, "luaL_unref") : NULL;
    real_close = g_lua ? (lua_close_t)GetProcAddress(g_lua, "lua_close") : NULL;
    real_newstate = g_lua ? (lua_newstate_t)GetProcAddress(g_lua, "lua_newstate") : NULL;
    if (!real_unref || !real_close || !real_newstate) { log_event("luaguard: lua51_Win32.dll or its luaL_unref/lua_close/lua_newstate not found; off"); return; }
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    if (!g_luaLock) g_luaLock = find_lua_lock(exe, game_exe(exe));
    if (g_luaLock)
        log_event("luaguard: registry unrefs run under the game's Lua lock (exe+0x%lx)",
                  (unsigned long)((BYTE *)g_luaLock - (BYTE *)GetModuleHandleW(NULL)));
    else
        log_event("luaguard: the game's Lua lock wasn't found (not a known build of the game exe?); only unrefs on collected Lua threads go to the main thread");
    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; i++) patch_module((BYTE *)mods[i]);
    }
    LqRegister_t reg = (LqRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (reg) reg(0, on_dll_notify, NULL, &cookie);  // the VP DLL loads later
}

void luaguard_report(void)
{
    if (real_unref && g_skipped) log_event("luaguard: skipped %ld luaL_unref calls on closed Lua states", g_skipped);
    if (real_unref && g_regUnrefs)
        log_event("luaguard: %ld registry luaL_unref calls: %ld inside the game's Lua lock, %ld outside it (lock taken), "
                  "%ld deferred while another thread held it (%ld done later, %ld dropped, %ld waiting); "
                  "%ld from Lua threads done on the main thread (%ld whose stack was gone)",
                  g_regUnrefs, g_inLock, g_outside, g_deferred, g_drained, g_dropped, g_queueLen, g_rerouted, g_dead);
    if (real_unref && g_noRef) log_event("luaguard: %ld luaL_unref calls with no ref (< 0) returned at once", g_noRef);
}