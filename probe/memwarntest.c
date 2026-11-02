// Low address space warning test (memwarn.c). Needs a copy of the game's lua51_Win32.dll in
// build/ (skipped without it) and luagc_testdb.dll. The parent writes civprobe.ini so luagc wraps
// luagc_testdb.dll's functions, runs itself as the child and restores the ini. The child stands in
// for the game: a stand-in for its Lua lock (CivProbe_SetLuaLock), a Lua Events table whose
// GameplayAlertMessage records what it is called with, a count of the UI contexts listening in
// place of the exe's list (CivProbe_MemWarnTestScreen; each check of it is counted), and free
// address space fed in by CivProbe_MemWarnFeed. Each step calls the wrapped make() from Lua. As
// the test exe isn't the game, the probe must first log that the in-game warning is off.
// Expected: the warning is posted once per crossing of 200 MB, only on the main thread inside the
// lock, only once something listens (tried at most once a second until then), again only after
// free space climbs back to 300 MB, not at all if it climbs back first; a Lua error in the event
// or no Events table doesn't crash or disturb the database call. The parent checks the log lines.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-66s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

typedef struct lua_State lua_State;
static lua_State *(*newstate)(void);
static void (*openlibs)(lua_State *);
static int (*loadbuffer)(lua_State *, const char *, size_t, const char *);
static int (*pcall)(lua_State *, int, int, int);
static const char *(*tolstring)(lua_State *, int, size_t *);
static int (*gettop)(lua_State *);
static void (*settop)(lua_State *, int);
static int (*feed)(DWORD);
static lua_State *L;
static CRITICAL_SECTION g_cs;
static volatile DWORD g_listeners, g_checks;  // UI contexts listening (the exe's list in the game); checks of it

static DWORD listeners(void)
{
    g_checks++;
    return g_listeners;
}

// Runs a chunk; its one result (or the error) as a string in out.
static int run(const char *code, char *out, int cap)
{
    int err = loadbuffer(L, code, strlen(code), "=memwarntest.lua") || pcall(L, 0, 1, 0);
    const char *s = tolstring(L, -1, NULL);
    snprintf(out, cap, "%s", s ? s : "nil");
    settop(L, 0);
    if (err) printf("  lua error: %s\n", out);
    return !err;
}

static char g_res[1024];
static const char *lua(const char *code) { run(code, g_res, sizeof g_res); return g_res; }

// The database call under the stand-in lock, as the game's main thread makes it. make() must
// return its one userdata whatever happens after it.
static int db_locked(void)
{
    EnterCriticalSection(&g_cs);
    int ok = run("return db()", g_res, sizeof g_res) && !strcmp(g_res, "1");
    LeaveCriticalSection(&g_cs);
    return ok && gettop(L) == 0;
}

static int num(const char *expr)
{
    char code[128];
    snprintf(code, sizeof code, "return %s", expr);
    return atoi(lua(code));
}

static DWORD WINAPI other_thread(void *arg)
{
    *(int *)arg = db_locked();
    return 0;
}

static const char kSetup[] =
    "alerts, fail = {}, false\n"
    "function events()\n"
    "  return { GameplayAlertMessage = setmetatable({},\n"
    "    { __call = function(self, text) if fail then error('listener broke') end alerts[#alerts + 1] = text end }) }\n"
    "end\n"
    "Events = events()\n"
    // A full collection first: the warning waits while Lua runs finalizers (make()'s __gc), and
    // this small heap can sit in that phase between steps.
    "function db() collectgarbage() local u = make() assert(type(u) == 'userdata') return select('#', make()) end\n"
    "return 1";

static int child(const char *dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\lua51_Win32.dll", dir);
    HMODULE lua51 = LoadLibraryA(path), db = LoadLibraryA("luagc_testdb.dll"), probe = GetModuleHandleA("msimg32.dll");
    if (!lua51 || !db || !probe) { printf("cannot load lua51_Win32.dll, luagc_testdb.dll or the probe\n"); return 1; }
    newstate = (void *)GetProcAddress(lua51, "luaL_newstate");
    openlibs = (void *)GetProcAddress(lua51, "luaL_openlibs");
    loadbuffer = (void *)GetProcAddress(lua51, "luaL_loadbuffer");
    pcall = (void *)GetProcAddress(lua51, "lua_pcall");
    tolstring = (void *)GetProcAddress(lua51, "lua_tolstring");
    gettop = (void *)GetProcAddress(lua51, "lua_gettop");
    settop = (void *)GetProcAddress(lua51, "lua_settop");
    void (*reg)(lua_State *) = (void *)GetProcAddress(db, "testdb_register");
    void (*set_lock)(CRITICAL_SECTION *) = (void *)GetProcAddress(probe, "CivProbe_SetLuaLock");
    void (*set_screen)(DWORD (*)(void)) = (void *)GetProcAddress(probe, "CivProbe_MemWarnTestScreen");
    feed = (void *)GetProcAddress(probe, "CivProbe_MemWarnFeed");
    if (!newstate || !openlibs || !loadbuffer || !pcall || !tolstring || !gettop || !settop || !reg || !set_lock ||
        !set_screen || !feed) {
        printf("setup failed: a function is missing\n");
        return 1;
    }
    InitializeCriticalSection(&g_cs);
    set_lock(&g_cs);
    set_screen(listeners);
    L = newstate();
    openlibs(L);
    reg(L);
    if (!run(kSetup, g_res, sizeof g_res)) return 1;

    check(feed(300) == 0 && db_locked() && num("#alerts") == 0 && (int)g_checks == 0, "300 MB free: nothing due, nothing tried");
    check(feed(150) == 1, "150 MB: crossed");
    check(run("return db()", g_res, sizeof g_res) && (int)g_checks == 0 && num("#alerts") == 0,
          "outside the Lua lock: not tried");
    int otherOk = 0;
    HANDLE th = CreateThread(NULL, 0, other_thread, &otherOk, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    check(otherOk && (int)g_checks == 0 && num("#alerts") == 0, "inside the lock on another thread: not tried");
    check(db_locked() && (int)g_checks == 1 && num("#alerts") == 0, "nothing listens yet: tried, waits");
    int ok = 1;
    for (int i = 0; i < 100; i++) ok &= db_locked();
    check(ok && (int)g_checks == 1, "100 more calls within a second: not tried again");
    Sleep(1100);
    g_listeners = 2;
    check(db_locked() && num("#alerts") == 1, "a listener, a second later: shown");
    const char *a1 = lua("return alerts[1]");
    printf("  text: %s\n", a1);
    check(strstr(a1, "(150 MB left)") && strstr(a1, "[COLOR_NEGATIVE_TEXT]") && strstr(a1, "[NEWLINE]Save, quit"),
          "the text names the free space and what to do");
    for (int i = 0; i < 10; i++) ok &= db_locked();
    check(ok && feed(120) == 0 && num("#alerts") == 1, "more calls, 120 MB: still one warning");
    check(feed(250) == 0 && feed(140) == 0, "back to 250 MB and down to 140: no new crossing");
    Sleep(1100);
    check(db_locked() && num("#alerts") == 1 && (int)g_checks == 2, "and nothing shown");
    check(feed(300) == 0 && feed(180) == 1, "back to 300 MB, then 180: crossed again");
    check(db_locked() && num("#alerts") == 2 && strstr(lua("return alerts[2]"), "(180 MB left)"), "second warning shown");

    lua("fail = true");
    check(feed(320) == 0 && feed(100) == 1, "320, then 100 MB: crossed (the event will raise an error)");
    Sleep(1100);
    int c0 = (int)g_checks;
    check(db_locked() && num("#alerts") == 2 && (int)g_checks == c0 + 1, "the event's error: caught, make() unharmed");
    Sleep(1100);
    check(db_locked() && (int)g_checks == c0 + 1, "not tried again after an error");

    lua("fail = false");
    g_listeners = 0;
    check(feed(320) == 0 && feed(150) == 1 && feed(320) == 0, "crossed and back to 320 MB before a listener");
    g_listeners = 2;
    Sleep(1100);
    c0 = (int)g_checks;
    check(db_locked() && num("#alerts") == 2 && (int)g_checks == c0, "nothing due any more: not shown");

    lua("Events = nil");
    check(feed(150) == 1, "150 MB again, with no Events table");
    Sleep(1100);
    check(db_locked() && num("#alerts") == 2, "no Events: no crash, waits");
    lua("Events = events()");
    Sleep(1100);
    check(db_locked() && num("#alerts") == 3 && strstr(lua("return alerts[3]"), "(150 MB left)"),
          "Events back: shown");
    set_lock(NULL);
    return g_ok ? 0 : 1;
}

static int read_newest_log(const char *dir, char *buf, int cap)
{
    char pat[MAX_PATH], best[MAX_PATH] = "";
    snprintf(pat, sizeof pat, "%s\\civprobe\\events_*.log", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if (strcmp(fd.cFileName, best) > 0) strcpy(best, fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    snprintf(pat, sizeof pat, "%s\\civprobe\\%s", dir, best);
    FILE *f = fopen(pat, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    char exe[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    *strrchr(dir, '\\') = 0;
    if (argc > 1) return child(dir);

    snprintf(path, sizeof path, "%s\\lua51_Win32.dll", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        printf("skipped: no lua51_Win32.dll in %s\n", dir);
        return 0;
    }
    snprintf(ini, sizeof ini, "%s\\civprobe\\civprobe.ini", dir);
    static char saved[65536];
    FILE *f = fopen(ini, "rb");
    size_t n = f ? fread(saved, 1, sizeof saved, f) : 0;
    if (f) fclose(f);
    f = fopen(ini, "wb");
    if (!f) { printf("cannot write %s\n", ini); return 1; }
    fprintf(f, "[probe]\r\nAllocTracking=0\r\nLuaGcModule=luagc_testdb.dll\r\nLuaGcCrt=ucrtbase.dll\r\n");
    fclose(f);
    Sleep(1100);  // a later second than the previous launch: the child's events log sorts last
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    f = fopen(ini, "wb");  // put the shared test ini back
    if (f) { fwrite(saved, 1, n, f); fclose(f); }

    static char log[1 << 20];
    check(code == 0 && read_newest_log(dir, log, sizeof log), "child passed, log read");
    check(strstr(log, "] luagc: patched lua_pushcclosure import of luagc_testdb.dll") != NULL, "luagc wrapped the test DB");
    check(strstr(log, "] memwarn: the in-game warning needs the game's GameplayAlertMessage listener list "
                      "(not a known build of the game exe); log only") != NULL &&
              strstr(log, "] memwarn: warns under 200 MB of free address space (in the log only)") != NULL,
          "not the game exe: log only at start");
    check(strstr(log, "] memwarn: no in-game screen yet (nothing listens to GameplayAlertMessage)") != NULL, "waiting logged");
    check(strstr(log, "] memwarn: in-game warning shown: 150 MB free (due since 0 s; 2 listening)") != NULL &&
              strstr(log, "] memwarn: in-game warning shown: 180 MB free") != NULL, "both warnings logged");
    check(strstr(log, "] memwarn: a Lua error while showing the in-game warning (100 MB free)") != NULL, "the error logged");
    check(strstr(log, "] memwarn: free address space back at 320 MB before the in-game warning could be shown") != NULL,
          "the climb back logged");
    check(strstr(log, "] memwarn: no Events.GameplayAlertMessage in this Lua state; tried again later") != NULL, "no Events logged");
    check(strstr(log, "] memwarn: 5 crossing(s) under 200 MB, 3 in-game warning(s) shown") != NULL, "exit report");
    check(strstr(log, "] exception 0x") == NULL, "no exception logged");
    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
