// Stand-in for CvGameDatabase in the luagc test. It registers its Lua functions
// through an imported lua_pushcclosure, like the real DLL. make() returns a
// userdata that owns BLOCK bytes of C memory, freed by its __gc.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *);
__declspec(dllimport) void lua_pushcclosure(lua_State *, lua_CFunction, int);
__declspec(dllimport) void *lua_newuserdata(lua_State *, size_t);
__declspec(dllimport) void *lua_touserdata(lua_State *, int);
__declspec(dllimport) int lua_setmetatable(lua_State *, int);
__declspec(dllimport) void lua_createtable(lua_State *, int, int);
__declspec(dllimport) void lua_setfield(lua_State *, int, const char *);
__declspec(dllimport) void lua_pushvalue(lua_State *, int);
__declspec(dllimport) ptrdiff_t lua_tointeger(lua_State *, int);
__declspec(dllimport) void lua_pushstring(lua_State *, const char *);
__declspec(dllimport) int lua_error(lua_State *);

#define LUA_GLOBALSINDEX (-10002)
#define UPVALUE(i) (LUA_GLOBALSINDEX - (i))
#define BLOCK (32 * 1024)

static volatile LONG g_live, g_peak, g_gcs;
static LONG g_gcErrorEvery;  // testdb_gc_errors: every Nth __gc raises a Lua error

static int ud_gc(lua_State *L)
{
    void **u = (void **)lua_touserdata(L, 1);
    if (u && *u) { free(*u); *u = NULL; InterlockedExchangeAdd(&g_live, -BLOCK); }
    if (g_gcErrorEvery && InterlockedIncrement(&g_gcs) % g_gcErrorEvery == 0) {
        lua_pushstring(L, "error in __gc");
        return lua_error(L);
    }
    return 0;
}

static int make(lua_State *L)
{
    void **u = (void **)lua_newuserdata(L, sizeof(void *));
    *u = malloc(BLOCK);
    if (*u) memset(*u, 0, 64);
    LONG live = InterlockedExchangeAdd(&g_live, BLOCK) + BLOCK;
    if (live > g_peak) g_peak = live;
    lua_pushvalue(L, UPVALUE(1));
    lua_setmetatable(L, -2);
    return 1;
}

// stall(ms): a database call that takes ms milliseconds (the freeze watchdog test).
static int stall(lua_State *L)
{
    Sleep((DWORD)lua_tointeger(L, 1));
    return 0;
}

__declspec(dllexport) void testdb_register(lua_State *L)
{
    lua_pushcclosure(L, stall, 0);
    lua_setfield(L, LUA_GLOBALSINDEX, "stall");
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, ud_gc, 0);
    lua_setfield(L, -2, "__gc");
    lua_pushcclosure(L, make, 1);  // upvalue: the metatable
    lua_setfield(L, LUA_GLOBALSINDEX, "make");
}

__declspec(dllexport) void testdb_gc_errors(long every) { g_gcErrorEvery = every; }
__declspec(dllexport) long testdb_peak_mb(void) { return g_peak >> 20; }
__declspec(dllexport) long testdb_live_mb(void) { return g_live >> 20; }
