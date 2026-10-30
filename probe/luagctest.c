// Host for the luagc test (named CivilizationV_* so the probe activates). Loads the
// game's lua51_Win32.dll, then luagc_testdb.dll, fills a ~50 MB Lua heap so Lua's own
// collector stays idle (as in the game), and creates many make() userdata.
// Usage: CivilizationV_luagctest.exe <path to lua51_Win32.dll> [count] [us of Lua work between calls]
// (a pace such as 900 gives ~34 MB/s of DB allocations, the game's busiest rate)
// [every Nth __gc raises a Lua error]: the collections must go on (exit code 1 if the C memory
// peak reaches half of all make() memory, as when the first error stopped them). Unpaced only:
// paced, Lua's own steps meet the errors too, and an error in Lua 5.1's GCTM leaves its own
// pacing far off (the Lua heap then grows to GBs before a collection, with or without the probe).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct lua_State lua_State;
typedef lua_State *(*newstate_t)(void);
typedef void (*openlibs_t)(lua_State *);
typedef int (*loadstring_t)(lua_State *, const char *);
typedef int (*pcall_t)(lua_State *, int, int, int);
typedef const char *(*tolstring_t)(lua_State *, int, size_t *);
typedef void (*reg_t)(lua_State *);
typedef long (*mb_t)(void);
typedef void (*errs_t)(long);

int wmain(int argc, WCHAR **argv)
{
    HDC dc = CreateCompatibleDC(NULL);  // keep the msimg32 import
    TRIVERTEX v[2] = {{0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
    GRADIENT_RECT gr = {0, 1};
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    if (argc < 2) { printf("usage: luagctest <lua51_Win32.dll> [count]\n"); return 2; }
    int count = argc > 2 ? _wtoi(argv[2]) : 20000;
    int paceUs = argc > 3 ? _wtoi(argv[3]) : 0;
    int gcErrors = argc > 4 ? _wtoi(argv[4]) : 0;

    HMODULE lua = LoadLibraryW(argv[1]);
    if (!lua) { printf("cannot load lua51_Win32.dll (%lu)\n", GetLastError()); return 1; }
    HMODULE db = LoadLibraryW(L"luagc_testdb.dll");
    if (!db) { printf("cannot load luagc_testdb.dll (%lu)\n", GetLastError()); return 1; }
    newstate_t newstate = (newstate_t)GetProcAddress(lua, "luaL_newstate");
    openlibs_t openlibs = (openlibs_t)GetProcAddress(lua, "luaL_openlibs");
    loadstring_t loadstring = (loadstring_t)GetProcAddress(lua, "luaL_loadstring");
    pcall_t pcall = (pcall_t)GetProcAddress(lua, "lua_pcall");
    tolstring_t tolstring = (tolstring_t)GetProcAddress(lua, "lua_tolstring");
    reg_t reg = (reg_t)GetProcAddress(db, "testdb_register");
    mb_t peak = (mb_t)GetProcAddress(db, "testdb_peak_mb"), live = (mb_t)GetProcAddress(db, "testdb_live_mb");
    ((errs_t)GetProcAddress(db, "testdb_gc_errors"))(gcErrors);

    lua_State *L = newstate();
    openlibs(L);
    reg(L);
    char script[768];
    snprintf(script, sizeof script,
             "big = {} for i = 1, 400000 do big[i] = string.rep('x', 100) .. i end "
             "collectgarbage() "
             "local pace = %d / 1e6 "
             "local function one() local u = make() "  // pcall'd: Lua's own collector may hit a __gc error too
             "  if pace > 0 then local t = os.clock() + pace while os.clock() < t do local s = {} end end end "
             "for i = 1, %d do pcall(one) end "
             "return collectgarbage('count')", paceUs, count);
    DWORD t0 = GetTickCount();
    if (loadstring(L, script) || pcall(L, 0, 1, 0)) { printf("lua error: %s\n", tolstring(L, -1, NULL)); return 1; }
    printf("%d make() calls in %lu ms; Lua heap %s KB; C memory peak %ld MB, live at end %ld MB\n", count,
           GetTickCount() - t0, tolstring(L, -1, NULL), peak(), live());
    if (gcErrors && peak() >= (long)((long long)count * 32 * 1024 / 2 >> 20)) {
        printf("FAILED: collections stopped after a __gc error\n");
        return 1;
    }
    return 0;
}
