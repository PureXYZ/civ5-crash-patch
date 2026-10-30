// Freeze watchdog ([probe] HangSeconds=30, the default; 0 = off). A frozen game raises no
// exception, and if the player then ends it the log just stops. Every sampler tick asks
// Windows whether the game's main window is hung (IsHungAppWindow: its thread has not taken a
// message for 5 s). Once that has lasted HangSeconds, the window's thread is suspended just
// long enough to copy its registers and stack, and the copy is scanned and logged after it
// resumes: while suspended it may hold a lock (the heap's, the loader's) that logging needs.
// While the freeze lasts this repeats every HangRepeatSeconds (60), up to 5 times, and its end
// is logged too. Long loads that stop the message loop show up as well; the "responding
// again" line tells them apart from a real hang.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

typedef struct {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    PVOID UniqueProcess, UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority, BasePriority;
} ThreadBasicInfo;
typedef LONG(NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef BOOL(WINAPI *IsHungAppWindow_t)(HWND);

static DWORD g_hangMs, g_repeatMs;
static IsHungAppWindow_t pIsHung;
static NtQueryInformationThread_t pQueryThread;
static HWND g_wnd;
static ULONGLONG g_hungSince, g_lastStack;
static int g_stacksThisFreeze, g_freezes;
#define MAX_STACKS_PER_FREEZE 5
#define MAX_FREEZES 10
#define STACK_COPY_BYTES (64 * 1024)
static BYTE g_stackCopy[STACK_COPY_BYTES];

// The Lua call stack of the frozen thread, when it is inside a Lua database call that luagc
// wraps (the busy loops seen so far were). Read with lua_getstack/lua_getinfo while the thread
// is suspended: no allocation and no locks with "nSl". currentline would store L->savedpc into
// a running Lua frame, so the innermost frame gets "nS" unless it is a C function.
typedef struct {
    int event;
    const char *name, *namewhat, *what, *source;
    int currentline, nups, linedefined, lastlinedefined;
    char short_src[60];  // LUA_IDSIZE
    int i_ci;
} lua_Debug51;
typedef int(__cdecl *lua_getstack_t)(struct lua_State *, int, lua_Debug51 *);
typedef int(__cdecl *lua_getinfo_t)(struct lua_State *, const char *, lua_Debug51 *);
static lua_getstack_t pGetStack;
static lua_getinfo_t pGetInfo;
#define MAX_LUA_FRAMES 16
static struct { char src[60], name[48], namewhat[12]; int line; char c; } g_luaFrames[MAX_LUA_FRAMES];
static int g_nLuaFrames;

// The suspended thread may be in the middle of changing its Lua state (growing its stack, a GC
// step), so the reads above can fault. The probe's exception handler sends such a fault on this
// thread back to read_lua_stack (hang_catch_fault), which then gives up on the Lua stack.
static void *g_walkJmp[5];  // __builtin_setjmp buffer
static void *g_walkSeh;     // the thread's SEH chain at the start of the walk
static volatile DWORD g_walkTid;
static int g_walkFaulted, g_testFault;

int hang_catch_fault(void)
{
    if (!g_walkTid || g_walkTid != GetCurrentThreadId()) return 0;
    g_walkTid = 0;
    ((NT_TIB *)NtCurrentTeb())->ExceptionList = g_walkSeh;  // drop SEH frames of the abandoned calls
    __builtin_longjmp(g_walkJmp, 1);
}

// Test hook: the next Lua stack read uses a corrupt lua_State, as if caught mid-update.
__declspec(dllexport) void CivProbe_HangTestFault(void) { g_testFault = 1; }
static struct lua_State *g_luaState;

static void copy_str(char *to, int cap, const char *from)
{
    int i = 0;
    if (from)
        for (; i < cap - 1 && from[i]; i++) to[i] = from[i];
    to[i] = 0;
}

static int readable_range(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (const BYTE *)p + n <= (const BYTE *)m.BaseAddress + m.RegionSize;
}

// Thread tid is suspended; sp..top is its stack.
static void read_lua_stack(DWORD tid, ULONG_PTR sp, ULONG_PTR top)
{
    g_nLuaFrames = 0;
    g_luaState = NULL;
    if (!pGetStack || !pGetInfo) return;  // resolved before the suspend: the loader lock may be held
    void *arg;
    const void *ret;
    struct lua_State *L = luagc_thread_call(tid, &arg, &ret);
    if (!L || (ULONG_PTR)arg < sp + 4 || (ULONG_PTR)arg >= top || ((const void *const *)arg)[-1] != ret) return;
    if (!readable_range(L, 0x48)) return;
    g_luaState = L;
    static void *fake[0x48 / 4];
    if (g_testFault) {  // ci (+0x14) far past base_ci (+0x28): lua_getinfo reads an unmapped CallInfo
        g_testFault = 0;
        fake[2] = fake[3] = (void *)0x1000;  // top, base
        fake[5] = (void *)0x10;
        fake[10] = NULL;
        L = (struct lua_State *)fake;
    }
    g_walkFaulted = 0;
    g_walkSeh = ((NT_TIB *)NtCurrentTeb())->ExceptionList;
    if (__builtin_setjmp(g_walkJmp)) {
        g_walkFaulted = 1;
        return;
    }
    g_walkTid = GetCurrentThreadId();
    lua_Debug51 ar;
    for (int level = 0; g_nLuaFrames < MAX_LUA_FRAMES && pGetStack(L, level, &ar); level++) {
        if (!pGetInfo(L, "S", &ar)) break;
        int isC = ar.what && ar.what[0] == 'C';
        if (!pGetInfo(L, level || isC ? "nSl" : "nS", &ar)) break;
        if (!(level || isC)) ar.currentline = -1;
        copy_str(g_luaFrames[g_nLuaFrames].src, sizeof g_luaFrames[0].src, ar.short_src);
        copy_str(g_luaFrames[g_nLuaFrames].name, sizeof g_luaFrames[0].name, ar.name);
        copy_str(g_luaFrames[g_nLuaFrames].namewhat, sizeof g_luaFrames[0].namewhat, ar.namewhat);
        g_luaFrames[g_nLuaFrames].line = ar.currentline;
        g_luaFrames[g_nLuaFrames].c = isC ? 'C' : ar.what && ar.what[0] == 'm' ? 'm' : 'L';
        g_nLuaFrames++;
    }
    g_walkTid = 0;
}

static void log_lua_stack(void)
{
    if (!g_luaState) return;
    if (g_walkFaulted)
        log_event("freeze: reading the Lua stack of state %p faulted after %d frames (it was being changed); skipped",
                  (void *)g_luaState, g_nLuaFrames);
    if (!g_nLuaFrames) return;
    log_event("freeze: Lua stack of state %p, innermost first:", (void *)g_luaState);
    for (int i = 0; i < g_nLuaFrames; i++) {
        char where[96];
        if (g_luaFrames[i].c == 'C') snprintf(where, sizeof where, "[C]");
        else if (g_luaFrames[i].line >= 0) snprintf(where, sizeof where, "%s:%d", g_luaFrames[i].src, g_luaFrames[i].line);
        else snprintf(where, sizeof where, "%s", g_luaFrames[i].src);
        if (g_luaFrames[i].name[0])
            log_event("lua:   %s in %s '%s'", where, g_luaFrames[i].namewhat[0] ? g_luaFrames[i].namewhat : "function",
                      g_luaFrames[i].name);
        else
            log_event("lua:   %s%s", where, g_luaFrames[i].c == 'm' ? " (main chunk)" : "");
    }
}

static BOOL CALLBACK find_window(HWND w, LPARAM out)
{
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

// Logs where the window's thread is: its instruction pointer and a scan of its stack copy.
static void log_thread(DWORD tid)
{
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!t) { log_event("freeze: cannot open thread %lu (%lu)", tid, GetLastError()); return; }
    ThreadBasicInfo tbi;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SIZE_T copied = 0;
    int ok = 0;
    if (pQueryThread && pQueryThread(t, 0 /* ThreadBasicInformation */, &tbi, sizeof tbi, NULL) == 0 &&
        SuspendThread(t) != (DWORD)-1) {
        // Only copying while it is suspended: no calls that could wait on a lock it holds.
        if (GetThreadContext(t, &ctx)) {
            const NT_TIB *tib = (const NT_TIB *)tbi.TebBaseAddress;
            ULONG_PTR sp = ctx.Esp & ~(ULONG_PTR)3, top = (ULONG_PTR)tib->StackBase;
            if (sp >= (ULONG_PTR)tib->StackLimit && sp < top) {
                copied = top - sp > STACK_COPY_BYTES ? STACK_COPY_BYTES : top - sp;
                memcpy(g_stackCopy, (const void *)sp, copied);
                read_lua_stack(tid, sp, top);
            }
            ok = 1;
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    if (!ok) { log_event("freeze: cannot read thread %lu", tid); return; }
    ULONG_PTR mb;
    const char *m = module_at((const void *)ctx.Eip, &mb);
    if (m) log_event("freeze: thread %lu is at %s+0x%lx", tid, m, (unsigned long)(ctx.Eip - mb));
    else log_event("freeze: thread %lu is at %p", tid, (void *)ctx.Eip);
    if (copied) modules_log_stack_copy(g_stackCopy, copied);
    else log_event("stack: esp=%p is outside the thread's stack", (void *)ctx.Esp);
    log_lua_stack();
}

// Sampler thread.
void hang_tick(void)
{
    if (!g_hangMs) return;
    if (!pIsHung) {  // resolved here, not in DllMain
        pIsHung = (IsHungAppWindow_t)GetProcAddress(LoadLibraryW(L"user32.dll"), "IsHungAppWindow");
        if (!pIsHung) { log_event("freeze: IsHungAppWindow not found; off"); g_hangMs = 0; return; }
    }
    if (!g_wnd || !IsWindow(g_wnd) || !IsWindowVisible(g_wnd)) {
        g_wnd = NULL;
        EnumWindows(find_window, (LPARAM)&g_wnd);
        if (!g_wnd) return;
    }
    ULONGLONG now = GetTickCount64();
    if (!pIsHung(g_wnd)) {
        if (g_hungSince && g_stacksThisFreeze)
            log_event("freeze: the game responds again after %.0f s", (now - g_hungSince) / 1000.0 + 5);
        g_hungSince = 0;
        return;
    }
    if (!g_hungSince) { g_hungSince = now; g_stacksThisFreeze = 0; }
    // IsHungAppWindow turns true after 5 s without messages; the times below include them.
    ULONGLONG hung = now - g_hungSince + 5000;
    int first = !g_stacksThisFreeze && hung >= g_hangMs;
    int again = g_stacksThisFreeze && g_stacksThisFreeze < MAX_STACKS_PER_FREEZE && now - g_lastStack >= g_repeatMs;
    if (!first && !again) return;
    if (first && ++g_freezes > MAX_FREEZES) return;
    if (g_freezes > MAX_FREEZES) return;
    g_stacksThisFreeze++;
    g_lastStack = now;
    DWORD tid = GetWindowThreadProcessId(g_wnd, NULL);
    HMODULE lua = GetModuleHandleW(L"lua51_Win32.dll");
    if (lua && !pGetInfo) {
        pGetStack = (lua_getstack_t)GetProcAddress(lua, "lua_getstack");
        pGetInfo = (lua_getinfo_t)GetProcAddress(lua, "lua_getinfo");
    }
    log_event("freeze: the game window has not responded for %.0f s (thread %lu)", hung / 1000.0, tid);
    log_thread(tid);
    log_memory_now("in freeze");
}

void hang_init(const WCHAR *ini)
{
    UINT hangS = GetPrivateProfileIntW(L"probe", L"HangSeconds", 30, ini);
    UINT repeatS = GetPrivateProfileIntW(L"probe", L"HangRepeatSeconds", 60, ini);
    if ((int)hangS <= 0) return;
    g_hangMs = (hangS > 3600 ? 3600 : hangS) * 1000;  // at most an hour (and no 32-bit wrap)
    g_repeatMs = ((int)repeatS <= 0 ? 60 : repeatS > 3600 ? 3600 : repeatS) * 1000;
    if (g_hangMs < 6000) g_hangMs = 6000;  // IsHungAppWindow needs 5 s
    pQueryThread = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
}
