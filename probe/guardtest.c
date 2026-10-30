// luaL_unref guard test: run as CivilizationV_guardtest.exe next to the probe's
// msimg32.dll and a copy of the game's lua51_Win32.dll (it needs msvcr90, bound by
// the same manifest as the mimalloc test). Expect a live state's unref to really run
// (the freed ref number is handed out again) and calls on unmapped state memory to
// be skipped instead of faulting like the game's exit crash, and an unref through a
// collected coroutine (stack released) to run on the main thread instead. With a stand-in for
// the game's Lua lock, an unref made while another thread holds it waits in a queue until the
// next unref that gets the lock.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lua_State lua_State;
__declspec(dllimport) lua_State *luaL_newstate(void);
__declspec(dllimport) int luaL_ref(lua_State *, int);
__declspec(dllimport) void luaL_unref(lua_State *, int, int);
__declspec(dllimport) void lua_pushboolean(lua_State *, int);
__declspec(dllimport) void lua_close(lua_State *);
__declspec(dllimport) lua_State *lua_newstate(void *(*)(void *, void *, size_t, size_t), void *);
__declspec(dllimport) lua_State *lua_newthread(lua_State *);
__declspec(dllimport) void lua_settop(lua_State *, int);
__declspec(dllimport) int lua_gc(lua_State *, int, int);
__declspec(dllimport) void *lua_newuserdata(lua_State *, size_t);
__declspec(dllimport) void lua_createtable(lua_State *, int, int);
__declspec(dllimport) void lua_pushcclosure(lua_State *, int (*)(lua_State *), int);
__declspec(dllimport) void lua_setfield(lua_State *, int, const char *);
__declspec(dllimport) int lua_setmetatable(lua_State *, int);
#define LUA_REGISTRYINDEX (-10000)
#define LUA_GCCOLLECT 2

static void *l_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (!nsize) { free(p); return NULL; }
    return realloc(p, nsize);
}

// Blocks of 512 bytes or more (Lua stacks) get their own pages, released when freed; smaller
// ones (a thread's lua_State) are never really freed, like heap memory not yet reused. A
// collected thread then looks like the game's: its lua_State intact, its stack unmapped.
static void *split_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    (void)ud;
    int was_big = p && osize >= 512, big = nsize >= 512;
    void *q = NULL;
    if (nsize) {
        q = big ? VirtualAlloc(NULL, nsize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : malloc(nsize);
        if (!q) return NULL;
        if (p) memcpy(q, p, osize < nsize ? osize : nsize);
    }
    if (was_big) VirtualFree(p, 0, MEM_RELEASE);
    return q;
}

// A C++ object's destructor run by a __gc finalizer during lua_close, releasing its ref.
static int g_closingRef;
static int unref_in_gc(lua_State *L)
{
    luaL_unref(L, LUA_REGISTRYINDEX, g_closingRef);
    return 0;
}

static CRITICAL_SECTION g_cs;
static HANDLE g_held, g_release;

// Another thread inside the game's Lua calls.
static DWORD WINAPI hold_lock(void *arg)
{
    (void)arg;
    EnterCriticalSection(&g_cs);
    SetEvent(g_held);
    WaitForSingleObject(g_release, INFINITE);
    LeaveCriticalSection(&g_cs);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    lua_State *L = luaL_newstate();
    lua_pushboolean(L, 1);
    int r1 = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_unref(L, LUA_REGISTRYINDEX, r1);
    lua_pushboolean(L, 1);
    int r2 = luaL_ref(L, LUA_REGISTRYINDEX);
    printf("live state: ref %d released and handed out again as %d (%s)\n", r1, r2, r1 == r2 ? "ok" : "WRONG");
    lua_close(L);
    luaL_unref(L, LUA_REGISTRYINDEX, r2);  // the game's exit: L closed, its memory possibly still mapped
    printf("closed state: skipped, no crash\n");
    lua_State *L2 = lua_newstate(l_alloc, NULL);  // may reuse the closed state's address
    lua_pushboolean(L2, 1);
    int r3 = luaL_ref(L2, LUA_REGISTRYINDEX);
    luaL_unref(L2, LUA_REGISTRYINDEX, r3);
    lua_pushboolean(L2, 1);
    int r4 = luaL_ref(L2, LUA_REGISTRYINDEX);
    printf("new state after a close: ref %d released and reused as %d (%s)\n", r3, r4, r3 == r4 ? "ok" : "WRONG");
    lua_close(L2);

    lua_State *L3 = lua_newstate(split_alloc, NULL);
    lua_State *T = lua_newthread(L3);  // a coroutine, as the game's UI contexts use
    lua_pushboolean(T, 1);
    int r5 = luaL_ref(T, LUA_REGISTRYINDEX);
    lua_settop(L3, 0);  // drop the only reference to the thread
    lua_gc(L3, LUA_GCCOLLECT, 0);  // collects it; its stack is released
    luaL_unref(T, LUA_REGISTRYINDEX, r5);  // the game's exit crash, third form (lua51+0x3b05)
    lua_pushboolean(L3, 1);
    int r6 = luaL_ref(L3, LUA_REGISTRYINDEX);
    printf("collected thread: ref %d released on the main thread and reused as %d (%s)\n", r5, r6, r5 == r6 ? "ok" : "WRONG");
    lua_close(L3);
    int ok = r1 == r2 && r3 == r4 && r5 == r6;

    // An unref made while the state closes must not mark it as good again: an unref after the
    // close then wrote the registry's released array (its 200 refs put it on pages of its own).
    lua_State *L6 = lua_newstate(split_alloc, NULL);
    for (int i = 0; i < 200; i++) { lua_pushboolean(L6, 1); luaL_ref(L6, LUA_REGISTRYINDEX); }
    lua_pushboolean(L6, 1);
    g_closingRef = luaL_ref(L6, LUA_REGISTRYINDEX);
    lua_newuserdata(L6, 4);
    lua_createtable(L6, 0, 1);
    lua_pushcclosure(L6, unref_in_gc, 0);
    lua_setfield(L6, -2, "__gc");
    lua_setmetatable(L6, -2);
    lua_settop(L6, 0);
    lua_close(L6);  // runs unref_in_gc
    luaL_unref(L6, LUA_REGISTRYINDEX, 5);
    printf("unref during lua_close, then one after it: skipped, no crash\n");

    // With the game's Lua lock (a stand-in here): unrefs run under it, and wait in a queue
    // while another thread holds it.
    void (*set_lock)(CRITICAL_SECTION *) =
        (void (*)(CRITICAL_SECTION *))GetProcAddress(GetModuleHandleA("msimg32.dll"), "CivProbe_SetLuaLock");
    if (!set_lock) {
        printf("CivProbe_SetLuaLock not exported: WRONG\n");
        return 1;
    }
    InitializeCriticalSection(&g_cs);
    set_lock(&g_cs);
    lua_State *L4 = luaL_newstate();
    lua_pushboolean(L4, 1);
    int r7 = luaL_ref(L4, LUA_REGISTRYINDEX);
    luaL_unref(L4, LUA_REGISTRYINDEX, r7);  // lock free: taken for the call
    lua_pushboolean(L4, 1);
    int r8 = luaL_ref(L4, LUA_REGISTRYINDEX);
    printf("lock free: ref %d released and reused as %d (%s)\n", r7, r8, r7 == r8 ? "ok" : "WRONG");
    ok &= r7 == r8;

    g_held = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, hold_lock, NULL, 0, NULL);
    WaitForSingleObject(g_held, INFINITE);
    luaL_unref(L4, LUA_REGISTRYINDEX, r8);  // another thread holds the lock: deferred
    lua_pushboolean(L4, 1);
    int r9 = luaL_ref(L4, LUA_REGISTRYINDEX);
    printf("lock held elsewhere: ref %d not released yet, new ref %d (%s)\n", r8, r9, r8 != r9 ? "ok" : "WRONG");
    ok &= r8 != r9;
    SetEvent(g_release);
    WaitForSingleObject(th, INFINITE);
    luaL_unref(L4, LUA_REGISTRYINDEX, r9);  // lock free again: r9, then the queued r8
    lua_pushboolean(L4, 1);
    int r10 = luaL_ref(L4, LUA_REGISTRYINDEX);
    lua_pushboolean(L4, 1);
    int r11 = luaL_ref(L4, LUA_REGISTRYINDEX);
    printf("lock released: queued ref %d done, refs %d and %d handed out again (%s)\n", r8, r10, r11,
           r10 == r8 && r11 == r9 ? "ok" : "WRONG");
    ok &= r10 == r8 && r11 == r9;
    luaL_unref(L4, LUA_REGISTRYINDEX, -1);  // LUA_REFNIL: nothing to release
    lua_pushboolean(L4, 1);
    int r12n = luaL_ref(L4, LUA_REGISTRYINDEX);
    printf("no ref (-1): nothing released, new ref %d (%s)\n", r12n, r12n != r10 && r12n != r11 ? "ok" : "WRONG");
    ok &= r12n != r10 && r12n != r11;
    lua_close(L4);

    lua_State *L5 = lua_newstate(split_alloc, NULL);
    lua_State *T5 = lua_newthread(L5);
    lua_pushboolean(T5, 1);
    int r12 = luaL_ref(T5, LUA_REGISTRYINDEX);
    lua_settop(L5, 0);
    lua_gc(L5, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_cs);  // as inside the game's Lua calls
    luaL_unref(T5, LUA_REGISTRYINDEX, r12);
    LeaveCriticalSection(&g_cs);
    lua_pushboolean(L5, 1);
    int r13 = luaL_ref(L5, LUA_REGISTRYINDEX);
    printf("collected thread, lock held here: ref %d released on the main thread and reused as %d (%s)\n", r12, r13,
           r12 == r13 ? "ok" : "WRONG");
    ok &= r12 == r13;
    lua_close(L5);
    set_lock(NULL);

    BYTE *half = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);  // before `gone`, so they differ
    BYTE *gone = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VirtualFree(gone, 0, MEM_RELEASE);
    luaL_unref((lua_State *)gone, LUA_REGISTRYINDEX, 1);
    printf("unmapped state: skipped, no crash\n");

    *(BYTE **)(half + 16) = gone;  // l_G points at released memory
    luaL_unref((lua_State *)half, LUA_REGISTRYINDEX, 1);
    printf("state with unmapped global_State: skipped, no crash\n");
    printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
    Sleep(1500);
    return ok ? 0 : 1;
}