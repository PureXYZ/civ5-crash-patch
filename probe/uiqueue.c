// Extra passes over the core-to-UI queue ([probe] UiQueuePasses, default 4; 1 = the game's own pass).
//
// The game core posts records (unit moved, plot changed, animation, ...) to a queue that the main
// thread works through once per frame (exe+0x385af0 on the queue owner at exe+0x2610200). A
// record about an object that an earlier record of the same frame, or a still-running animation,
// is about waits for a later frame, and records marked to keep their order wait behind it. So a
// unit's records go one per frame. The main loop hands the core its next update only once the
// queue is empty, and VP runs one AI player per update: in a late-game save these one-per-frame
// chains were ~20 s of a ~47 s AI turn (2026-10-30), with the main thread just rendering.
//
// N > 1 runs the same call up to N-1 more times per frame, right before the frame, from the run
// loop's PeekMessageW (an import of the exe; recognised by its return address). Each pass takes
// the next record of every chain once the previous one is done; records that start an animation
// stay pending as before. Only under the conditions of the game's own call site (exe+0xb3fe1):
// the game's per-frame check returns false and the queue is in normal mode; and only while the
// core isn't running an update (its lock byte) and records are waiting. Stops when a pass moves
// nothing. No game code is changed. DX11 addresses here; the DX9 and tablet exes have the same
// code elsewhere (exes.c; the code bytes are checked).
//
// Paced ([probe] UiQueueShare, default 50 percent; UiQueueFrameMs, default 20; 0 = unpaced).
// The records a pass takes in fire the UI's handlers (mod Lua included) in that pass and later in
// the same frame, so frames with extra passes run long. Unpaced, the passes ran in every frame of
// an AI turn that had records waiting: 18% of those frames took over 33 ms (0.3% of the others),
// a third of the AI turn's time was in such frames, and panning the map stuttered (2026-10-30,
// live A/B in a late-game save). Now extra passes run only while a bucket of time is positive:
// it refills at UiQueueShare percent of wall time, up to UiQueueFrameMs, and every frame longer
// than UiQueueFrameMs takes its overrun out, whatever made it long. After a long frame the next
// frames go without extra passes until the overrun is paid back. The debt is capped at 5 targets:
// uncapped, the save's load (and time with the game in the background) left so much that most
// sessions never ran an extra pass again (2026-10-30: 0 extra passes, thousands paced). The
// default was 25 until the redraw merges below; with them, one share per AI turn while panning
// gave the same time in long frames for no extra passes, 10, 25 and 50 (18-20%), unpaced 29%;
// AI turns 47, 46, 40 and 37 s (2026-10-30, live A/B, three turns each).
//
// Handler times ([probe] UiQueueStats=1, diagnostic). The records' handlers sit in a table of
// (function, context) pairs in .data, one pair per record type (owner + 8 + index * 0x600, called
// by exe+0x23aba0 as fn(payload, context)). The probe puts a timing thunk in each pair's function
// slot (a data write; the game's code is unchanged) and reports per type: calls, time, the
// longest call, and how much of the frames over 33 ms each type took.
//
// Merged records ([probe] UiDirtyCoalesceMs, default 250; UiTeamMetCoalesceMs, default 30000; 0 = off). Both
// types' handler is the game's generic one (exe+0x23e530) that calls every Lua listener:
// - 0x66 SerialEventGameDataDirty: CP's TechPanel rebuilds its unlock buttons from the database
//   each time, EUI panels, mods; ~43 ms a run in a late Huge game.
// - 0x77 TeamMet: in the user's setup only Global Relations listens, and both its listeners redraw
//   a whole view (hidden or not; it redraws on opening anyway); ~300 ms a run, 348 runs in 10
//   minutes of AI turns, the biggest hitch (2026-10-30). A 2 s window cut the runs ~80% in AI
//   turns but each run left is still a ~300 ms frame, hence 30 s for that mod (up to 60 s).
//   Listeners that use the event's teams would see only the last meeting of a burst; for mods
//   like that, UiTeamMetCoalesceMs=0.
// A record that comes within N ms of the end of that type's last run is skipped and remembered;
// the last one skipped runs N ms after the last run, from the run loop, under the game's
// conditions for processing records and the queue's own lock. So the listeners run at most about
// every N ms, and never later than N ms after the last record. A skipped TeamMet that still can't
// run 10 s after its time (record processing stopped: a save loading, the menu) is dropped, so it
// can't reach the listeners of the next game with the last game's teams.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

// GameExe (DX11 addresses):
//   uiProcess    exe+0x385af0   thiscall, ecx = queue owner
//   uiOwner      exe+0x2610200  [owner] = index; queue = owner + 0xc08 + index * 0x4c
//   uiLocked     exe+0x29e0447  byte: the main loop locked record processing for a core update
//   uiGame       exe+0x8c8e74   [game] + 0x1460 -> object whose vtable +0x3a4 says "don't process"
//   uiMode       exe+0x2610f80  [mode] + 0x2b0 & 2 -> another processing mode
//   uiSite       exe+0xb3fe1    the game's own call site, checked byte for byte
//   uiRet1/2     exe+0x41db0d / +0x41dc8d   run loop: call esi (PeekMessageW) at loop start / after the messages
//   uiDispatch   exe+0x23aba0   the dispatcher: call [table + type * 8] with (payload, context)
//   uiListeners  exe+0x23e530   the generic handler 0x66 and 0x77 are registered with: calls every listener
//   uiRegDirty / uiRegTeamMet   exe+0xc583c / +0x4a4ec, their registrations
// The handler tables are at owner + 8: per [owner] index, 0xc0 (fn, context) pairs, 0x600 apart.
#define NTYPES 0xc0
#define TYPE_DIRTY 0x66         // SerialEventGameDataDirty
#define TYPE_TEAMMET 0x77       // TeamMet
#define THUNK 10                // mov eax, slot; jmp common

static const BYTE kProcess[] = {0x51, 0x53, 0x56, 0x57, 0x6A, 0x00, 0x8B, 0xF1, 0x6A, 0x01, 0x8D, 0x9E, 0x00, 0x0D, 0x00, 0x00};
// mov eax,[game]; mov ecx,[eax+1460h]; mov edx,[ecx]; mov eax,[edx+3A4h]; call eax; test al,al; jne;
// mov eax,[mode]; cmp eax,ebx (DX9: test eax,eax); je; mov eax,[eax+2B0h]; test al,2; je; ...; mov ecx,owner; call process
static const BYTE kSite[] = {0xA1, 0, 0, 0, 0, 0x8B, 0x88, 0x60, 0x14, 0x00, 0x00, 0x8B, 0x11, 0x8B, 0x82, 0xA4, 0x03, 0x00,
                             0x00, 0xFF, 0xD0, 0x84, 0xC0, 0x75, 0x34, 0xA1, 0, 0, 0, 0, 0x3B, 0xC3, 0x74, 0x21, 0x8B, 0x80,
                             0xB0, 0x02, 0x00, 0x00, 0xA8, 0x02};
static const BYTE kSiteMask[] = {1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
                                 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
_Static_assert(sizeof kSite == sizeof kSiteMask, "site mask");
#define SITE_CALL 0x43          // offset of "mov ecx, owner; call process" in the site (exe+0xb4024)

typedef BOOL(WINAPI *PeekMessageW_t)(MSG *, HWND, UINT, UINT, UINT);
static PeekMessageW_t real_peek;
static BYTE *g_exe;
static const GameExe *g_g;
#define OWNER (g_exe + g_g->uiOwner)
#define TABLES (g_exe + g_g->uiOwner + 8)
static int g_passes;
static volatile LONG g_extra, g_moved, g_frames, g_paced;
static LONGLONG g_target, g_share;  // frame time (QPC ticks; also the bucket's size), refill percent (0 = unpaced)
static LONGLONG g_bucket, g_lastFrame, g_spent, g_freq;  // main thread only
static volatile LONG g_long[3];     // frames (run loop iterations) over 33 / 50 / 100 ms

// handler wrapping (main thread only, except the pass-through for other threads)
typedef void(__cdecl *handler_t)(void *payload, void *ctx);
static handler_t g_orig[2 * NTYPES];
static BYTE *g_thunks;              // 2 * NTYPES slots, then the common stub
static int g_stats;
static DWORD g_mainTid;
static int g_depth;
static LONGLONG g_lastWrap;
struct tstat {
    LONG n, longN;                  // calls; frames over 33 ms with a call
    LONGLONG ticks, max, longTicks;
};
static struct tstat g_ts[NTYPES];
static LONGLONG g_frameTicks[NTYPES], g_frameIn;  // this frame: per type (inclusive), top-level total
static BYTE g_touched[NTYPES], g_touchList[NTYPES];
static int g_nTouched;
static LONG g_longFrames, g_hugeFrames;           // over 33 / 100 ms
static LONGLONG g_longTicks, g_longIn, g_hugeTicks, g_hugeIn;
enum { M_DIRTY, M_TEAMMET, NMERGE };
struct merge {
    LONGLONG window, last;          // ticks; window 0 = off (written from outside for live A/B tests)
    DWORD type;
    const char *name;
    int pending, slot;
    BYTE payload[64];
    LONG run, skipped, late;
};
_Static_assert(sizeof(struct merge) == 112, "merge layout (ab.py writes window at +0 of each entry)");
static struct merge g_merge[NMERGE] = {[M_DIRTY] = {.type = TYPE_DIRTY, .name = "SerialEventGameDataDirty"},
                                       [M_TEAMMET] = {.type = TYPE_TEAMMET, .name = "TeamMet"}};
static int g_merging;               // any window set at launch
static LONG g_mergeDropped[NMERGE]; // pending records dropped (outside struct merge: ab.py's layout)
#define DROP_AFTER_S 10

static struct merge *merge_of(DWORD type)
{
    for (int i = 0; i < NMERGE; i++)
        if (g_merge[i].type == type) return &g_merge[i];
    return NULL;
}

static DWORD queue_bytes(void)
{
    BYTE *owner = OWNER;
    BYTE *q = owner + 0xc08 + *(DWORD *)owner * 0x4c;
    DWORD side = (*(DWORD *)(q + 0x20) & 1) * 16;  // write side
    return *(DWORD *)(q + side + 8) - *(DWORD *)(q + side + 4);
}

static int may_process(void)
{
    if (g_exe[g_g->uiLocked]) return 0;
    BYTE *game = *(BYTE **)(g_exe + g_g->uiGame);
    if (!game) return 0;
    void *obj = *(void **)(game + 0x1460);
    if (!obj) return 0;
    typedef BYTE(__thiscall * check_t)(void *);
    if ((*(check_t **)obj)[0x3a4 / 4](obj)) return 0;
    BYTE *mode = *(BYTE **)(g_exe + g_g->uiMode);
    return !mode || !(*(DWORD *)(mode + 0x2b0) & 2);
}

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Once per run loop iteration (the call at its start): the last frame's time goes into the
// bucket and the report's counts.
static void note_frame(LONGLONG now)
{
    if (g_lastFrame) {
        LONGLONG f = now - g_lastFrame, ms = f * 1000 / g_freq;
        if (ms > 33) g_long[0]++;
        if (ms > 50) g_long[1]++;
        if (ms > 100) g_long[2]++;
        if (g_share) {
            g_bucket += f * g_share / 100;
            if (g_bucket > g_target) g_bucket = g_target;
            if (f > g_target) g_bucket -= f - g_target;
            if (g_bucket < -5 * g_target) g_bucket = -5 * g_target;
        }
        if (ms > 33) {
            g_longFrames++;
            g_longTicks += f;
            g_longIn += g_frameIn;
            if (ms > 100) {
                g_hugeFrames++;
                g_hugeTicks += f;
                g_hugeIn += g_frameIn;
            }
            for (int i = 0; i < g_nTouched; i++) {
                struct tstat *s = &g_ts[g_touchList[i]];
                s->longN++;
                s->longTicks += g_frameTicks[g_touchList[i]];
            }
        }
    }
    for (int i = 0; i < g_nTouched; i++) {
        g_frameTicks[g_touchList[i]] = 0;
        g_touched[g_touchList[i]] = 0;
    }
    g_nTouched = 0;
    g_frameIn = 0;
    g_lastFrame = now;
}

static void account(DWORD type, LONGLONG dt)
{
    struct tstat *s = &g_ts[type];
    s->n++;
    s->ticks += dt;
    if (dt > s->max) s->max = dt;
    g_frameTicks[type] += dt;
    if (!g_touched[type]) {
        g_touched[type] = 1;
        g_touchList[g_nTouched++] = (BYTE)type;
    }
    if (!g_depth) g_frameIn += dt;
}

static void run_handler(DWORD slot, void *payload, void *ctx)
{
    DWORD type = slot % NTYPES;
    LONGLONG t0 = qpc();
    g_depth++;
    g_orig[slot](payload, ctx);
    g_depth--;
    LONGLONG t1 = qpc();
    account(type, t1 - t0);
    struct merge *m = merge_of(type);
    if (m) {
        m->last = t1;  // from the end: N ms of other work between runs
        m->run++;
    }
}

// Called by the thunks with the dispatcher's arguments (cdecl).
void __cdecl uiqueue_wrap(DWORD slot, void *ret, void *payload, void *ctx)
{
    if (GetCurrentThreadId() != g_mainTid) {
        g_orig[slot](payload, ctx);
        return;
    }
    struct merge *m = g_merging && !g_depth ? merge_of(slot % NTYPES) : NULL;
    if (m && m->window) {
        if (qpc() - m->last < m->window) {
            // the record: [size][type]... with the payload at +0x44
            DWORD n = *(DWORD *)((BYTE *)payload - 0x44) - 0x44;
            if (n > sizeof m->payload) n = sizeof m->payload;
            memcpy(m->payload, payload, n);
            m->pending = 1;
            m->slot = slot;
            m->skipped++;
            return;
        }
        m->pending = 0;
    }
    run_handler(slot, payload, ctx);
}

// The last skipped record of each merged type, once N ms have passed: under the conditions of
// the game's own pass and the queue's lock (owner + 0xd00, taken the same way as exe+0x385af0 does).
static void merge_late(LONGLONG now)
{
    for (int i = 0; i < NMERGE; i++) {
        struct merge *m = &g_merge[i];
        if (!m->pending || now - m->last < m->window) continue;
        if (i == M_TEAMMET && now - m->last > m->window + DROP_AFTER_S * g_freq) {
            m->pending = 0;  // its payload names the teams of a game that may be gone
            g_mergeDropped[i]++;
            continue;
        }
        if (!may_process()) continue;
        volatile LONG *lock = (volatile LONG *)(OWNER + 0xd00);
        if (InterlockedCompareExchange(lock, 1, 0)) return;
        m->pending = 0;
        m->late++;
        void *ctx = *(void **)(TABLES + m->slot / NTYPES * 0x600 + m->type * 8 + 4);
        run_handler(m->slot, m->payload, ctx);
        InterlockedExchange(lock, 0);
        now = qpc();
    }
}

// Puts a thunk in every non-empty function slot of both handler tables (again if the game
// registers a handler anew). Main thread, so the dispatcher never sees a half-done slot.
static void wrap_tables(void)
{
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < NTYPES; i++) {
            handler_t *e = (handler_t *)(TABLES + t * 0x600 + i * 8);
            BYTE *th = g_thunks + (t * NTYPES + i) * THUNK;
            if (!*e || (BYTE *)*e == th) continue;
            g_orig[t * NTYPES + i] = *e;
            *e = (handler_t)th;
        }
}

static int make_thunks(void)
{
    BYTE *p = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return 0;
    BYTE *common = p + 2 * NTYPES * THUNK;  // push eax; call uiqueue_wrap; add esp,4; ret
    common[0] = 0x50;
    common[1] = 0xE8;
    *(LONG *)(common + 2) = (LONG)((BYTE *)uiqueue_wrap - (common + 6));
    common[6] = 0x83, common[7] = 0xC4, common[8] = 0x04, common[9] = 0xC3;
    for (int s = 0; s < 2 * NTYPES; s++) {
        BYTE *t = p + s * THUNK;
        t[0] = 0xB8;
        *(DWORD *)(t + 1) = s;
        t[5] = 0xE9;
        *(LONG *)(t + 6) = (LONG)(common - (t + 10));
    }
    DWORD old;
    if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), p, 4096);
    g_thunks = p;
    return 1;
}

static BOOL WINAPI hook_peek(MSG *m, HWND w, UINT a, UINT b, UINT r)
{
    BOOL got = real_peek(m, w, a, b, r);
    BYTE *ret = (BYTE *)__builtin_return_address(0);
    if (ret != g_exe + g_g->uiRet1 && ret != g_exe + g_g->uiRet2) return got;
    LONGLONG now = qpc();
    if (ret == g_exe + g_g->uiRet1) {
        note_frame(now);
        if (g_thunks && now - g_lastWrap > g_freq) {
            g_mainTid = GetCurrentThreadId();
            wrap_tables();
            g_lastWrap = now;
        }
    }
    if (g_thunks && g_merging) merge_late(now);
    if (got || g_passes <= 1 || !queue_bytes()) return got;
    typedef void(__thiscall * process_t)(void *);
    process_t process = (process_t)(g_exe + g_g->uiProcess);
    int moved = 0;
    for (int i = 1; i < g_passes && may_process(); i++) {
        if (g_share && g_bucket <= 0) {
            g_paced++;
            break;
        }
        DWORD before = queue_bytes();
        LONGLONG t0 = qpc();
        process(OWNER);
        g_spent += qpc() - t0;
        DWORD after = queue_bytes();
        g_extra++;
        if (after >= before) break;
        g_moved += before - after;
        moved = 1;
        if (!after) break;
    }
    if (moved) g_frames++;
    return got;
}

const char *uiqueue_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!g->uiProcess || g->uiLocked >= size || g->uiOwner + 0x1000 >= size || g->uiSite + SITE_CALL + 10 >= size) return "addresses";
    if (memcmp(exe + g->uiProcess, kProcess, sizeof kProcess)) return "processing";
    for (DWORD i = 0; i < sizeof kSite; i++)
        if (kSiteMask[i] && exe[g->uiSite + i] != kSite[i]) return "call site";
    const BYTE *test = exe + g->uiSite + 30;  // the mode pointer's NULL check
    if (!(test[0] == 0x3B && test[1] == 0xC3) && !(test[0] == 0x85 && test[1] == 0xC0)) return "call site";
    const BYTE *call = exe + g->uiSite + SITE_CALL;  // B9 owner / E8 rel32 to process
    if (*(BYTE **)(exe + g->uiSite + 1) != exe + g->uiGame || *(BYTE **)(exe + g->uiSite + 26) != exe + g->uiMode ||
        call[0] != 0xB9 || *(BYTE *const *)(call + 1) != exe + g->uiOwner || call[5] != 0xE8 ||
        exe_branch_target(exe, call + 5) != exe + g->uiProcess)
        return "call site";
    // the run loop's two PeekMessageW calls: mov esi,[__imp_PeekMessageW] ... call esi
    if (exe[g->uiRet1 - 2] != 0xFF || exe[g->uiRet1 - 1] != 0xD6 || exe[g->uiRet2 - 2] != 0xFF || exe[g->uiRet2 - 1] != 0xD6)
        return "run loop";
    return NULL;
}

// The handler tables: the dispatcher, its table argument in the processing function, and the
// registrations of types 0x66 and 0x77 (mov [ecx + table slot], generic listener handler).
const char *uiqueue_tables_check(BYTE *exe, const GameExe *g)
{
    static const BYTE kDispatch[] = {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x41, 0x08, 0x2B, 0x41, 0x04, 0x74, 0x3B, 0x56, 0xE8};
    static const BYTE kCall[] = {0x8B, 0x48, 0x04, 0x8B, 0x54, 0xCF, 0x04, 0x8D, 0x0C, 0xCF, 0x83, 0xC0, 0x44, 0x52, 0x50, 0x8B, 0x01, 0xFF, 0xD0};
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    BYTE *tables = exe + g->uiOwner + 8;
    const BYTE *p = exe + g->uiProcess, *rd = exe + g->uiRegDirty, *rt = exe + g->uiRegTeamMet;
    if (!g->uiDispatch || g->uiOwner + 8 + 2 * 0x600 >= nt->OptionalHeader.SizeOfImage || !g->uiRegDirty || !g->uiRegTeamMet)
        return "addresses";
    if (memcmp(exe + g->uiDispatch, kDispatch, sizeof kDispatch) || memcmp(exe + g->uiDispatch + 0x21, kCall, sizeof kCall))
        return "dispatcher";
    if (p[0x9d] != 0x8D || p[0x9e] != 0x54 || p[0x9f] != 0x31 || p[0xa0] != 0x08 || exe_branch_target(exe, p + 0xb2) != exe + g->uiDispatch ||
        p[0xb2] != 0xE8)
        return "dispatcher call";
    if (rd[0] != 0xC7 || rd[1] != 0x81 || *(BYTE *const *)(rd + 2) != tables + TYPE_DIRTY * 8 || *(BYTE *const *)(rd + 6) != exe + g->uiListeners ||
        rt[0] != 0xC7 || rt[1] != 0x81 || *(BYTE *const *)(rt + 2) != tables + TYPE_TEAMMET * 8 || *(BYTE *const *)(rt + 6) != exe + g->uiListeners)
        return "registrations";
    return NULL;
}

void uiqueue_init(const WCHAR *ini)
{
    g_passes = GetPrivateProfileIntW(L"probe", L"UiQueuePasses", 4, ini);
    g_stats = GetPrivateProfileIntW(L"probe", L"UiQueueStats", 0, ini) == 1;
    int mergeMs[NMERGE] = {GetPrivateProfileIntW(L"probe", L"UiDirtyCoalesceMs", 250, ini),
                           GetPrivateProfileIntW(L"probe", L"UiTeamMetCoalesceMs", 30000, ini)};
    for (int i = 0; i < NMERGE; i++) {
        if (mergeMs[i] < 0) mergeMs[i] = 0;
        if (mergeMs[i] > 60000) mergeMs[i] = 60000;
        if (mergeMs[i]) g_merging = 1;
    }
    if (g_passes <= 1 && !g_stats && !g_merging) return;
    if (g_passes > 8) g_passes = 8;
    int share = GetPrivateProfileIntW(L"probe", L"UiQueueShare", 50, ini);
    int frameMs = GetPrivateProfileIntW(L"probe", L"UiQueueFrameMs", 20, ini);
    if (share > 100) share = 100;
    if (frameMs > 1000) frameMs = 1000;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    if (share > 0 && share < 100 && frameMs > 0) {
        g_target = g_bucket = g_freq * frameMs / 1000;
        g_share = share;
    }
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    const GameExe *g = game_exe(exe);
    const char *why = g ? uiqueue_check(exe, g) : "not a known build of the game exe";
    if (why) {
        log_event("uiqueue: the game's queue processing isn't the known code (%s); one pass per frame as usual", why);
        return;
    }
    const char *tablesWhy = g_stats || g_merging ? uiqueue_tables_check(exe, g) : "not needed";
    int tables = !tablesWhy;
    if ((g_stats || g_merging) && !tables)
        log_event("uiqueue: the game's record handler tables aren't the known code (%s); no handler times or coalescing", tablesWhy);
    g_g = g;
    g_exe = exe;
    real_peek = (PeekMessageW_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PeekMessageW");
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir.VirtualAddress); real_peek && dir.Size && d->Name; d++)
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(exe + d->FirstThunk); t->u1.Function; t++) {
            if (t->u1.Function != (DWORD)(ULONG_PTR)real_peek) continue;
            DWORD old;
            if (!VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old)) continue;
            InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)hook_peek);
            VirtualProtect(&t->u1.Function, 4, old, &old);
            n++;
        }
    if (!n) {
        log_event("uiqueue: PeekMessageW import not found; one pass per frame as usual");
        return;
    }
    if (tables && make_thunks()) {
        if (g_stats) log_event("uiqueue: timing the queue's record handlers (UiQueueStats=1)");
        for (int i = 0; i < NMERGE; i++) {
            g_merge[i].window = g_freq * mergeMs[i] / 1000;
            if (mergeMs[i])
                log_event("uiqueue: %s records within %d ms of the last run are merged into one later run", g_merge[i].name,
                          mergeMs[i]);
        }
    } else
        g_merging = 0;
    if (g_passes > 1 && g_share)
        log_event("uiqueue: up to %d passes over the core-to-UI queue per frame (the game's own: 1), paced (after a frame over "
                  "%d ms, none until %d%% of the time has paid it back); no game code changed", g_passes, frameMs, share);
    else if (g_passes > 1)
        log_event("uiqueue: up to %d passes over the core-to-UI queue per frame (the game's own: 1), unpaced; no game code changed",
                  g_passes);
}

void uiqueue_report(void)
{
    if (!g_exe || !real_peek) return;
    if (g_passes > 1)
        log_event("uiqueue: %ld extra passes in %ld frames moved %ld KB of records in %ld ms; %ld times paced; "
                  "frames over 33/50/100 ms: %ld/%ld/%ld", g_extra, g_frames, g_moved >> 10, (long)(g_spent * 1000 / g_freq),
                  g_paced, g_long[0], g_long[1], g_long[2]);
    if (!g_thunks) return;
    for (int i = 0; i < NMERGE; i++)
        if (g_merge[i].window)
            log_event("uiqueue: %s: %ld runs (%ld of them late), %ld records merged, %ld dropped", g_merge[i].name,
                      g_merge[i].run, g_merge[i].late, g_merge[i].skipped, g_mergeDropped[i]);
    if (!g_stats) return;
#define MS(t) ((long)((t) * 1000 / g_freq))
    log_event("uiqueue: %ld frames over 33 ms (%ld ms, %ld ms of it in record handlers); %ld over 100 ms (%ld ms, %ld in handlers)",
              g_longFrames, MS(g_longTicks), MS(g_longIn), g_hugeFrames, MS(g_hugeTicks), MS(g_hugeIn));
    // the 12 types with the most time in frames over 33 ms
    BYTE done[NTYPES] = {0};
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < NTYPES; i++)
            if (!done[i] && g_ts[i].n && (best < 0 || g_ts[i].longTicks > g_ts[best].longTicks)) best = i;
        if (best < 0) break;
        done[best] = 1;
        struct tstat *t = &g_ts[best];
        log_event("uiqueue:   type 0x%02x: %ld calls, %ld ms (longest %ld ms); in %ld long frames, %ld ms", best, t->n, MS(t->ticks),
                  MS(t->max), t->longN, MS(t->longTicks));
    }
#undef MS
}

#ifdef UIQUEUE_TEST
// A stand-in exe image (the DX11 exe's tables and processing conditions at their offsets): thunks
// made and tables wrapped as in the game; *note_frame / *late are the run loop's per-frame steps.
int uiqueue_test_setup(BYTE *exe, int stats, int dirtyMs, int teamMetMs, void (**frame)(LONGLONG), void (**late)(LONGLONG))
{
    g_g = game_exe_dx11();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_exe = exe;
    g_stats = stats;
    g_merge[M_DIRTY].window = g_freq * dirtyMs / 1000;
    g_merge[M_TEAMMET].window = g_freq * teamMetMs / 1000;
    g_merging = dirtyMs || teamMetMs;
    g_mainTid = GetCurrentThreadId();
    if (!make_thunks()) return 0;
    wrap_tables();
    *frame = note_frame;
    *late = merge_late;
    return 1;
}

void uiqueue_test_rewrap(void) { wrap_tables(); }
LONG uiqueue_test_counts(int teamMet, LONG *skipped, LONG *late, LONG *longFrames)
{
    struct merge *m = &g_merge[teamMet ? M_TEAMMET : M_DIRTY];
    *skipped = m->skipped;
    *late = m->late;
    *longFrames = g_longFrames;
    return m->run;
}
LONG uiqueue_test_type_calls(int type) { return g_ts[type].n; }
#endif
