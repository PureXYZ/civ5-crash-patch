// Game-core pacing ([probe] GameCoreIntervalMs, default 10; 0 = the game's own 100 ms).
//
// With EnableGameCoreThreading the game core (AI turns included) runs on its own thread, but
// only one update at a time, handed over by the main thread. Each frame the main loop adds the
// frame time to an accumulator (a float at exe+0x8cf91c, in .data) and compares it with a
// virtual getter (vtable +0x198, exe+0x304b60: `fld [0.1f]; ret`); only when the accumulator
// passes it does it call exe+0x13af10, which resumes the core thread for one update
// (exe+0x4d8bf0), after which the core pauses itself again. The accumulator then restarts at 0,
// and a hand-over that finds the core still busy is skipped. Measured in a late-game Huge save
// (2026-10-29): the core ran 8-20 ms every 100-200 ms and was paused 91% of each AI turn, so the
// 0.1 s interval, not AI work, set the length of AI turns.
//
// No game code is changed: Steam's CEG wrapper checks the main loop's code, and a 6-byte patch of
// this call site made the game quit by itself 30 s into every launch (PostQuitMessage from the
// main loop, behind a CEG stub). Instead a thread of ours sets the accumulator to 0.1 every
// GameCoreIntervalMs, so the next frame's own check passes. Only the main loop block reads or
// writes the accumulator (four references, checked with the code bytes around them). A write
// that races the main thread's add is simply lost. DX11 addresses here; the DX9 and tablet exes
// have the same loop (exes.c: coreSite, coreAcc; code checked).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

static const BYTE kSite[] = {0x8B, 0x90, 0x98, 0x01, 0x00, 0x00,  // mov edx, [eax+198h]
                             0xD9, 0x1D, 0, 0, 0, 0,              // fstp dword [acc]
                             0xFF, 0xD2,                          // call edx
                             0xD9, 0x05, 0, 0, 0, 0,              // fld dword [acc]
                             0xDF, 0xF1, 0xDD, 0xD8, 0x76};       // fcomip; fstp st0; jbe
static const BYTE kMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1};
#define GAME_INTERVAL 0.1f

static volatile float *g_acc;
static LONG g_ms;
static volatile LONG g_writes;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static DWORD WINAPI pacer(void *arg)
{
    (void)arg;
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)g_ms * 10000;
    for (;;) {
        if (t && SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(t, INFINITE);
        else Sleep((DWORD)g_ms);
        if (*g_acc < GAME_INTERVAL) {
            *g_acc = GAME_INTERVAL;
            g_writes++;
        }
    }
    return 0;
}

const char *coretick_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    const BYTE *f = exe + g->coreSite;
    BYTE *acc = exe + g->coreAcc;
    if (!g->coreSite || g->coreSite + sizeof kSite > nt->OptionalHeader.SizeOfImage || g->coreAcc + 4 > nt->OptionalHeader.SizeOfImage)
        return "addresses";
    for (DWORD i = 0; i < sizeof kSite; i++)
        if (kMask[i] && f[i] != kSite[i]) return "hand-over code";
    if (*(BYTE *const *)(f + 8) != acc || *(BYTE *const *)(f + 16) != acc) return "accumulator";
    return NULL;
}

void coretick_init(const WCHAR *ini)
{
    g_ms = GetPrivateProfileIntW(L"probe", L"GameCoreIntervalMs", 10, ini);
    if (g_ms <= 0 || g_ms >= 100) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    const char *why = g ? coretick_check(exe, g) : "not a known build of the game exe";
    if (why) {
        log_event("coretick: game-core hand-over code not found (%s); pacing unchanged", why);
        return;
    }
    g_acc = (volatile float *)(exe + g->coreAcc);
    HANDLE th = CreateThread(NULL, 64 * 1024, pacer, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { log_event("coretick: no thread; pacing unchanged"); return; }
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(th);
    log_event("coretick: the game core gets an update up to every %ld ms (the game's own: 100 ms); no game code changed", g_ms);
}

void coretick_report(void)
{
    if (g_acc) log_event("coretick: %ld early hand-overs", g_writes);
}
