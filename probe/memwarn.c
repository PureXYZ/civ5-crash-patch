// In-game warning when address space runs low ([probe] LowMemoryWarnMB=200, the default; 0 = off;
// LowMemoryWarnInGame=1, the default; 0 = only the events log line).
//
// The sampler has logged "under 200 MB free: save and restart the game soon" since 1.0.0, but
// players only read the log after a crash. Now the game shows it too, through its own
// Events.GameplayAlertMessage(text): the line of text at the top of the screen that the in-game
// UI (InGame.lua, also Community Patch's copy) shows for 8 seconds; EUI's notification panel
// keeps it in a tooltip too.
//
// How the exe runs an Event called from Lua (DX11): __call (exe+0x33b5c0) calls the event's
// Call, which converts the arguments and posts a record instead of calling the listeners (one
// LuaEvent type's Call, exe+0x28e8e0, appends it to a double-buffered queue without a lock,
// exe+0x26f940). GameplayAlertMessage is record type 0x73 of the core-to-UI queue (uiqueue.c):
// the function the game calls with its name (exe+0x24c900, from exe+0x2bd9b9) registers the
// generic handler exe+0x23e530 for it, which calls a list of delegates, one per UI context that
// listens (each context has its own Events table, and Events.X.Count() counts only that
// context's listeners). So the text reaches the listeners later, on the main thread, in its pass
// over the queue. In the game only the main thread calls Events from Lua (UI scripts), inside the
// exe's Lua lock. So the warning is posted only there:
// - on the main thread (the one that loads this DLL: the game's UI thread, thread 0 in dumps);
// - while that thread holds the exe's Lua lock (luaguard.c finds it from its wrappers' code);
// - at the return of a database call that luagc.c wraps (a C function called from Lua, the same
//   point where a script's own call of an Event would run; with luagc off, log only), and not
//   while Lua is running finalizers or luagc is collecting (luagc.c checks both before calling
//   memwarn_show);
// - inside lua_cpcall, so a Lua error is caught and the stack is left as it was;
// - only when the engine's delegate list for type 0x73 isn't empty: in the main menu and before
//   the in-game screen loads nothing listens (in game: 2, InGame.lua and EUI's panel), and the
//   warning waits. It is checked at most once a second while it waits.
// One warning per crossing: it is armed again only after free space has climbed back to
// LowMemoryWarnMB + 100 MB. No game code is changed, and nothing reaches saves or game state:
// the listeners only put the text on screen.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

typedef struct lua_State lua_State;
typedef int(__cdecl *lua_CFunction)(lua_State *);
typedef int(__cdecl *lua_cpcall_t)(lua_State *, lua_CFunction, void *);
typedef void(__cdecl *lua_settop_t)(lua_State *, int);
typedef void(__cdecl *lua_getfield_t)(lua_State *, int, const char *);
typedef int(__cdecl *lua_type_t)(lua_State *, int);
typedef void(__cdecl *lua_pushstring_t)(lua_State *, const char *);
typedef void(__cdecl *lua_call_t)(lua_State *, int, int);
typedef void *(__cdecl *lua_touserdata_t)(lua_State *, int);

#define LUA_GLOBALSINDEX (-10002)
#define LUA_TTABLE 5
#define LUA_TUSERDATA 7
#define TYPE_ALERT 0x73  // GameplayAlertMessage's record type in the core-to-UI queue
#define REARM_MB 100
#define RETRY_MS 1000

static lua_cpcall_t p_cpcall;
static lua_settop_t p_settop;
static lua_getfield_t p_getfield;
static lua_type_t p_type;
static lua_pushstring_t p_pushstring;
static lua_call_t p_call;
static lua_touserdata_t p_touserdata;
static int g_resolved;  // 0 not yet, 1 ok, -1 lua51_Win32.dll lacks something

enum { ARMED, PENDING, SHOWING, DONE };
static volatile LONG g_state = ARMED;
static DWORD g_thresholdMb, g_inGame, g_mainTid;
static BYTE *g_exe;
static const GameExe *g_g;  // set when memwarn_check passed (the in-game warning is possible)
static DWORD (*g_testListeners)(void);  // CivProbe_MemWarnTestScreen: stands in for the engine's list
static volatile LONG g_freeMb;    // the latest sample's free address space
static volatile LONG g_testFeed;  // CivProbe_MemWarnFeed took over from the sampler
static double g_dueT;
static DWORD g_lastTry;
static LONG g_crossings, g_shown, g_noLock, g_noUi, g_errors;
static int g_loggedNoLock, g_loggedNoUi, g_loggedNoEvents;

DWORD memwarn_threshold_mb(void) { return g_thresholdMb; }

static int feed(ULONGLONG freeBytes)
{
    if (!g_thresholdMb) return 0;
    LONG mb = (LONG)(freeBytes >> 20);
    InterlockedExchange(&g_freeMb, mb);
    if ((DWORD)mb < g_thresholdMb) {
        if (InterlockedCompareExchange(&g_state, g_inGame ? PENDING : DONE, ARMED) != ARMED) return 0;
        g_dueT = log_time_s();
        g_loggedNoLock = g_loggedNoUi = g_loggedNoEvents = 0;
        InterlockedIncrement(&g_crossings);
        return 1;
    }
    if ((DWORD)mb >= g_thresholdMb + REARM_MB) {
        // Climbed back: armed for the next crossing; a warning not shown yet is no longer due.
        if (InterlockedCompareExchange(&g_state, ARMED, DONE) != DONE &&
            InterlockedCompareExchange(&g_state, ARMED, PENDING) == PENDING)
            log_event("memwarn: free address space back at %ld MB before the in-game warning could be shown; not shown", mb);
    }
    return 0;
}

int memwarn_feed(ULONGLONG freeBytes) { return g_testFeed ? 0 : feed(freeBytes); }

// Test hook: free address space in MB in place of the sampler's (which is ignored from then on).
// 1 when this crossed the threshold.
__declspec(dllexport) int CivProbe_MemWarnFeed(DWORD freeMb)
{
    InterlockedExchange(&g_testFeed, 1);
    return feed((ULONGLONG)freeMb << 20);
}

// Test hook: the in-game warning on in a test exe (memwarn_check can't pass there), with fn
// standing in for the game exe's delegate list. Before any feed.
__declspec(dllexport) void CivProbe_MemWarnTestScreen(DWORD (*fn)(void))
{
    g_testListeners = fn;
    g_inGame = 1;
}

int memwarn_due(void) { return g_state == PENDING; }

// The registration of type 0x73 with the generic listener handler (mov dword [ecx + table slot],
// handler) in the function the game calls with the name "GameplayAlertMessage" (push; call).
const char *memwarn_check(BYTE *exe, const GameExe *g)
{
    static const char kName[] = "GameplayAlertMessage";
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!g->uiRegAlert || !g->uiAlertCall || !g->uiOwner || !g->uiListeners || g->uiOwner + 8 + 2 * 0x600 > size ||
        g->uiRegAlert + 10 > size || g->uiAlertCall + 10 > size)
        return "addresses";
    const BYTE *rg = exe + g->uiRegAlert, *call = exe + g->uiAlertCall;
    if (rg[0] != 0xC7 || rg[1] != 0x81 || *(BYTE *const *)(rg + 2) != exe + g->uiOwner + 8 + TYPE_ALERT * 8 ||
        *(BYTE *const *)(rg + 6) != exe + g->uiListeners)
        return "registration";
    const BYTE *name = call[0] == 0x68 ? *(const BYTE *const *)(call + 1) : NULL;
    const BYTE *fn = call[5] == 0xE8 ? exe_branch_target(exe, call + 5) : NULL;
    if (!name || name < exe || name + sizeof kName > exe + size || memcmp(name, kName, sizeof kName) || !fn || fn >= rg ||
        rg - fn > 0x100)
        return "name";
    return NULL;
}

static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const BYTE *)p + n <= (const BYTE *)m.BaseAddress + m.RegionSize;
}

// How many UI contexts listen to GameplayAlertMessage: the delegate list of the generic handler's
// context for type 0x73 (count at +0x14). Main thread only, where the UI adds and removes them.
static DWORD listeners(void)
{
    if (g_testListeners) return g_testListeners();
    BYTE *owner = g_exe + g_g->uiOwner;
    DWORD index = *(DWORD *)owner;  // which of the two handler tables is in use
    if (index > 1) return 0;
    BYTE *ctx = *(BYTE **)(owner + 8 + index * 0x600 + TYPE_ALERT * 8 + 4);
    return ctx && readable(ctx, 0x18) ? *(DWORD *)(ctx + 0x14) : 0;
}

static int resolve(void)
{
    if (g_resolved) return g_resolved > 0;
    HMODULE lua = GetModuleHandleW(L"lua51_Win32.dll");
    if (lua) {
        p_cpcall = (lua_cpcall_t)GetProcAddress(lua, "lua_cpcall");
        p_settop = (lua_settop_t)GetProcAddress(lua, "lua_settop");
        p_getfield = (lua_getfield_t)GetProcAddress(lua, "lua_getfield");
        p_type = (lua_type_t)GetProcAddress(lua, "lua_type");
        p_pushstring = (lua_pushstring_t)GetProcAddress(lua, "lua_pushstring");
        p_call = (lua_call_t)GetProcAddress(lua, "lua_call");
        p_touserdata = (lua_touserdata_t)GetProcAddress(lua, "lua_touserdata");
    }
    g_resolved = p_cpcall && p_settop && p_getfield && p_type && p_pushstring && p_call && p_touserdata ? 1 : -1;
    if (g_resolved < 0) log_event("memwarn: lua51_Win32.dll or one of its Lua functions not found; the warning stays in the log");
    return g_resolved > 0;
}

typedef struct { const char *text; int posted; } Post;

// Runs inside lua_cpcall: stack [1] = the Post. Events.GameplayAlertMessage(text), through the
// event's __call, as a UI script would call it.
static int __cdecl post_protected(lua_State *L)
{
    Post *p = (Post *)p_touserdata(L, 1);
    p_getfield(L, LUA_GLOBALSINDEX, "Events");  // 2
    int t = p_type(L, 2);
    if (t != LUA_TTABLE && t != LUA_TUSERDATA) return 0;
    p_getfield(L, 2, "GameplayAlertMessage");  // 3
    t = p_type(L, 3);
    if (t != LUA_TTABLE && t != LUA_TUSERDATA) return 0;
    p_pushstring(L, p->text);
    p_call(L, 1, 0);
    p->posted = 1;
    return 0;
}

// luagc.c, at the return of a wrapped database call while memwarn_due(), outside its own
// collections and Lua's finalizer phase.
void memwarn_show(lua_State *L)
{
    DWORD me = GetCurrentThreadId();
    if (me != g_mainTid) return;
    DWORD now = GetTickCount();
    if (g_lastTry && now - g_lastTry < RETRY_MS) return;
    CRITICAL_SECTION *lock = luaguard_lua_lock();
    if (!lock || (DWORD)(ULONG_PTR)lock->OwningThread != me) {
        g_noLock++;
        if (!lock && !g_loggedNoLock) {
            g_loggedNoLock = 1;
            log_event("memwarn: the game's Lua lock wasn't found (not a known build of the game exe?); the warning stays in the log");
        }
        return;
    }
    g_lastTry = now;
    DWORD n = listeners();
    if (!n) {
        g_noUi++;
        if (!g_loggedNoUi) {
            g_loggedNoUi = 1;
            log_event("memwarn: no in-game screen yet (nothing listens to GameplayAlertMessage); the warning waits for it");
        }
        return;
    }
    if (!resolve() || InterlockedCompareExchange(&g_state, SHOWING, PENDING) != PENDING) return;
    LONG mb = g_freeMb;
    char text[320];
    snprintf(text, sizeof text,
             "[COLOR_NEGATIVE_TEXT]Civ V Crash Patch: the game is running out of memory (%ld MB left).[ENDCOLOR]"
             "[NEWLINE]Save, quit to the desktop and load the save again to avoid a crash.", mb);
    Post p = {text, 0};
    if (p_cpcall(L, post_protected, &p) != 0) {
        p_settop(L, -2);  // the error object
        g_errors++;
        InterlockedExchange(&g_state, DONE);  // an error now would be an error next time too
        log_event("memwarn: a Lua error while showing the in-game warning (%ld MB free); it stays in the log", mb);
    } else if (p.posted) {
        g_shown++;
        InterlockedExchange(&g_state, DONE);
        log_event("memwarn: in-game warning shown: %ld MB free (due since %.0f s; %lu listening)", mb, g_dueT, n);
    } else {
        g_noUi++;
        InterlockedExchange(&g_state, PENDING);
        if (!g_loggedNoEvents) {
            g_loggedNoEvents = 1;
            log_event("memwarn: no Events.GameplayAlertMessage in this Lua state; tried again later");
        }
    }
}

void memwarn_init(const WCHAR *ini)
{
    g_mainTid = GetCurrentThreadId();  // DllMain: the game's main thread
    g_thresholdMb = GetPrivateProfileIntW(L"probe", L"LowMemoryWarnMB", 200, ini);
    if (g_thresholdMb > 3000) g_thresholdMb = 3000;
    g_inGame = GetPrivateProfileIntW(L"probe", L"LowMemoryWarnInGame", 1, ini) != 0;
    if (!g_thresholdMb) { log_event("memwarn: off (LowMemoryWarnMB=0)"); return; }
    g_exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(g_exe);
    const char *why = g ? memwarn_check(g_exe, g) : "not a known build of the game exe";
    if (g_inGame && why) {
        g_inGame = 0;
        log_event("memwarn: the in-game warning needs the game's GameplayAlertMessage listener list (%s); log only", why);
    } else if (g_inGame && !luagc_on()) {  // luagc_init ran first
        g_inGame = 0;
        log_event("memwarn: the in-game warning is shown from the Lua cleanup's database call hook, which is off "
                  "(LuaGcStepMB=0, or see its luagc: line); log only");
    }
    if (!why) g_g = g;
    log_event("memwarn: warns under %lu MB of free address space (%s), again after it climbs back to %lu MB",
              g_thresholdMb, g_inGame ? "in the log and in game" : "in the log only", g_thresholdMb + REARM_MB);
}

void memwarn_report(void)
{
    if (!g_crossings) return;
    log_event("memwarn: %ld crossing(s) under %lu MB, %ld in-game warning(s) shown; %ld checks found nothing listening, "
              "%ld database calls on the main thread outside the Lua lock, %ld Lua errors",
              g_crossings, g_thresholdMb, g_shown, g_noUi, g_noLock, g_errors);
    if (g_state == PENDING)
        log_event("memwarn: the in-game warning due since %.0f s hasn't been shown yet", g_dueT);
}
