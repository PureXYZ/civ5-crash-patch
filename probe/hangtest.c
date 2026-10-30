// Freeze watchdog test (hang.c). The parent writes civprobe.ini with short timings
// (HangSeconds=6, HangRepeatSeconds=4, IntervalMs=500), runs itself as the child and restores
// the ini. The child shows a small window off-screen, takes its messages, then stops taking
// them for 15 s inside freeze_here() and takes them again. The parent checks the child's
// events log: the freeze is logged with the thread, a stack that runs through freeze_here()'s
// caller, the memory line, repeated while it lasts, and its end.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        Sleep(20);
    }
}

static char g_expect[128];

__attribute__((noinline)) void freeze_here(void)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    snprintf(g_expect, sizeof g_expect, " %s+0x%lx ", strrchr(exe, '\\') + 1,
             (unsigned long)((ULONG_PTR)__builtin_return_address(0) - (ULONG_PTR)GetModuleHandleA(NULL)));
    Sleep(15000);
}

static int child(const char *dir)
{
    HWND w = CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "hangtest", WS_POPUP | WS_VISIBLE, -32000, -32000, 10, 10,
                             NULL, NULL, NULL, NULL);
    if (!w) { printf("no window (%lu)\n", GetLastError()); return 1; }
    pump(1500);
    freeze_here();
    pump(3000);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\hangtest_expect.txt", dir);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(g_expect, f); fclose(f); }
    return 0;
}

// Lua child: the freeze is a database call (stall, from luagc_testdb.dll, wrapped by luagc)
// made two Lua functions deep, so the watchdog should log the Lua stack too.
typedef struct lua_State lua_State;
static const char kLuaScript[] = "local function inner(ms) stall(ms) end\n"
                                 "function outer(ms) inner(ms) end\n"
                                 "outer(12000)\n";

// fault=1: the probe reads a corrupt lua_State instead (CivProbe_HangTestFault), as if it caught
// the thread changing its stack; the read must be given up without a crash.
static const char kFaultScript[] = "stall(9000)\n";

static int lua_child(const char *dir, int fault)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\lua51_Win32.dll", dir);
    HMODULE lua = LoadLibraryA(path), db = LoadLibraryA("luagc_testdb.dll");
    if (!lua || !db) { printf("cannot load lua51_Win32.dll or luagc_testdb.dll\n"); return 1; }
    lua_State *(*newstate)(void) = (void *)GetProcAddress(lua, "luaL_newstate");
    void (*openlibs)(lua_State *) = (void *)GetProcAddress(lua, "luaL_openlibs");
    int (*loadbuffer)(lua_State *, const char *, size_t, const char *) = (void *)GetProcAddress(lua, "luaL_loadbuffer");
    int (*pcall)(lua_State *, int, int, int) = (void *)GetProcAddress(lua, "lua_pcall");
    void (*reg)(lua_State *) = (void *)GetProcAddress(db, "testdb_register");
    HWND w = CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "hangtest", WS_POPUP | WS_VISIBLE, -32000, -32000, 10, 10,
                             NULL, NULL, NULL, NULL);
    if (!w || !newstate || !openlibs || !loadbuffer || !pcall || !reg) { printf("setup failed\n"); return 1; }
    pump(1500);
    lua_State *L = newstate();
    openlibs(L);
    reg(L);
    if (fault) ((void (*)(void))GetProcAddress(GetModuleHandleA("msimg32.dll"), "CivProbe_HangTestFault"))();
    const char *script = fault ? kFaultScript : kLuaScript;
    size_t len = fault ? sizeof kFaultScript - 1 : sizeof kLuaScript - 1;
    if (loadbuffer(L, script, len, "=hangtest.lua") || pcall(L, 0, 0, 0)) {
        printf("lua error\n");
        return 1;
    }
    pump(3000);
    return 0;
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

static int count(const char *s, const char *what)
{
    int n = 0;
    for (const char *p = s; (p = strstr(p, what)); p++) n++;
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    char exe[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    *strrchr(dir, '\\') = 0;
    if (argc > 1)
        return !strcmp(argv[1], "lua") ? lua_child(dir, 0) : !strcmp(argv[1], "luafault") ? lua_child(dir, 1) : child(dir);

    snprintf(ini, sizeof ini, "%s\\civprobe\\civprobe.ini", dir);
    static char saved[65536];
    FILE *f = fopen(ini, "rb");
    size_t n = f ? fread(saved, 1, sizeof saved, f) : 0;
    if (f) fclose(f);
    f = fopen(ini, "wb");
    if (!f) { printf("cannot write %s\n", ini); return 1; }
    fprintf(f, "[probe]\r\nIntervalMs=500\r\nHangSeconds=6\r\nHangRepeatSeconds=4\r\n");
    fclose(f);
    Sleep(1100);  // a later second than the previous launch: the child's events log sorts last
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    printf("  the child freezes for 15 s\n");
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    f = fopen(ini, "wb");  // put the shared test ini back
    if (f) { fwrite(saved, 1, n, f); fclose(f); }

    static char log[1 << 20], expect[128] = "";
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\hangtest_expect.txt", dir);
    f = fopen(path, "rb");
    if (f) { expect[fread(expect, 1, sizeof expect - 1, f)] = 0; fclose(f); DeleteFileA(path); }
    check(code == 0 && expect[0] && read_newest_log(dir, log, sizeof log), "child ran, log read");
    const char *p = strstr(log, "] freeze: the game window has not responded for ");
    check(p != NULL, "freeze logged");
    check(p && strstr(p, "] freeze: thread ") && strstr(p, "memory (in freeze)"), "with the thread's position and memory");
    const char *st = p ? strstr(p, "] stack: ") : NULL;
    char frames[2048] = "";
    if (st) {
        size_t len = strcspn(st + 8, "\r\n");
        if (len > sizeof frames - 2) len = sizeof frames - 2;
        memcpy(frames, st + 8, len);
        strcpy(frames + len, " ");
    }
    printf("  want%s\n  got %s\n", expect, frames);
    check(expect[0] && strstr(frames, expect) != NULL, "stack runs through freeze_here()'s caller");
    check(count(log, "] freeze: the game window has not responded for ") >= 2, "repeated while it lasts");
    check(strstr(log, "] freeze: the game responds again after ") != NULL, "its end logged");
    check(strstr(log, "] freeze: Lua stack") == NULL, "no Lua stack for a freeze outside Lua");

    // The same inside a Lua database call (needs a copy of the game's lua51_Win32.dll here).
    snprintf(path, sizeof path, "%s\\lua51_Win32.dll", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        printf("  Lua freeze: skipped (no lua51_Win32.dll in %s)\n", dir);
    } else {
        f = fopen(ini, "wb");
        if (!f) { printf("cannot write %s\n", ini); return 1; }
        fprintf(f, "[probe]\r\nIntervalMs=500\r\nHangSeconds=6\r\nHangRepeatSeconds=4\r\nAllocTracking=0\r\n"
                   "LuaGcModule=luagc_testdb.dll\r\nLuaGcCrt=ucrtbase.dll\r\n");
        fclose(f);
        Sleep(1100);
        snprintf(cmd, sizeof cmd, "\"%s\" lua", exe);
        printf("  the Lua child freezes for 12 s\n");
        code = 1;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        f = fopen(ini, "wb");
        if (f) { fwrite(saved, 1, n, f); fclose(f); }
        check(code == 0 && read_newest_log(dir, log, sizeof log), "Lua child ran, log read");
        const char *ls = strstr(log, "] freeze: Lua stack of state ");
        check(ls != NULL, "Lua stack logged");
        const char *want[] = {"] lua:   [C] in global 'stall'", "] lua:   hangtest.lua:1 in upvalue 'inner'",
                              "] lua:   hangtest.lua:2 in global 'outer'", "] lua:   hangtest.lua:3 (main chunk)"};
        const char *at = ls;
        for (int i = 0; i < 4; i++) {
            const char *q = at ? strstr(at, want[i]) : NULL;
            char what[96];
            snprintf(what, sizeof what, "frame %d: %s", i, want[i] + 8);
            check(q != NULL, what);
            at = q;
        }
        if (!at && ls) printf("%.600s\n", ls);

        f = fopen(ini, "wb");
        if (!f) { printf("cannot write %s\n", ini); return 1; }
        fprintf(f, "[probe]\r\nIntervalMs=500\r\nHangSeconds=6\r\nHangRepeatSeconds=60\r\nAllocTracking=0\r\n"
                   "LuaGcModule=luagc_testdb.dll\r\nLuaGcCrt=ucrtbase.dll\r\n");
        fclose(f);
        Sleep(1100);
        snprintf(cmd, sizeof cmd, "\"%s\" luafault", exe);
        printf("  the Lua child freezes for 9 s; its Lua state reads as corrupt\n");
        code = 1;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        f = fopen(ini, "wb");
        if (f) { fwrite(saved, 1, n, f); fclose(f); }
        check(code == 0 && read_newest_log(dir, log, sizeof log), "corrupt-Lua child ran to the end, log read");
        check(strstr(log, "] freeze: reading the Lua stack of state ") != NULL, "the faulting Lua stack read given up");
        check(strstr(log, "] freeze: the game responds again after ") != NULL, "and the freeze's end logged");
        check(strstr(log, "] exception 0x") == NULL, "no exception logged for it");
    }

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
