// The game's event queue: overflow log and fix ([probe] MsgQueueGuard, default 1; 0 = log only).
//
// Threads post events for the main thread into a queue object at exe+0x160e400: two queues
// (the object's first dword picks one; only queue 0 seen in use), each a pair of 4 MB halves:
//   +0          used bytes of half 0, records from +0x80
//   +0x400080   used bytes of half 1, records from +0x400100
//   +0x800100   flip counter: posts go to half (counter & 1)
//   +0x800104   the game's own high-water mark of a half's used bytes
// with a handler table of 192 (fn, ctx) pairs per queue at +8 + queue * 0x600 and a lock at
// +0x1001000. A post (inlined at ~240 sites, e.g. exe+0xc230) reads the counter, reserves its record
// with InterlockedExchangeAdd(&half.used, size) through the exe's import, and writes
// [size][type][0][0] and the payload at half + 0x80 + the offset it got. Nothing checks the 4 MB.
// Once a frame the main loop calls exe+0x59d20: try-lock (InterlockedCompareExchange import), bump
// the counter, zero the new half's used bytes, call fn(payload, ctx) for each record of the other
// half (exe+0x2a5970), unlock.
//
// A Huge late-game save posts more than 4 MB on the main thread while it loads (in the game's Lua
// UI code; sessions 20261030_045941 and 051035: 5.07 MB, 6,585 posts past the end). The extra
// records run into the other half (or past the second half onto the counter). The flip then
// zeroes a dword inside them and new posts overwrite them while the main thread walks them: a
// record with a garbage type calls a NULL handler (exe+0x2a59a3, sessions 015612, 030248, 045643
// and 051035, all while loading) and a zero size would loop forever.
//
// The fix hooks those two import slots of the exe. A post that doesn't fit its half gets an offset
// that points into a spill buffer (8 MB per half, allocated the first time) instead, and so does every later
// post to that half until the next pass, to keep the order. At the next pass (its try-lock) the
// probe runs the game's own pass, then calls the handlers of the spilled records in order under
// the same lock, and makes the game's call return as if the lock were taken. The records the half
// held come first, then the spill: the order they were posted in. No game code changed. The
// DX9 and tablet exes have the same queue (addresses in exes.c; the code is checked).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

// GameExe: queueObj (DX11 exe+0x160e400), queuePass (the pass, thiscall, ecx = object;
// exe+0x59d20), queueFlip (exe+0x90df0), queuePost (one inlined post, type 0x49, 0x80 bytes;
// exe+0xc230), queueWalk (the pass's handler call; exe+0x2a5992), queueMainSite (main loop:
// mov ecx, object; call the pass; exe+0xb4062). The post reserves through the exe's
// InterlockedExchangeAdd import, the pass try-locks through its InterlockedCompareExchange import.
#define Q0 0xc80
#define QSIZE 0x800180
#define HALF 0x400080
#define CAP 0x400000
#define COUNTER 0x800100
#define HIGH 0x800104
#define TABLE 8
#define TABLE_SIZE 0x600
#define TYPES 192
#define LOCK 0x1001000
#define SPILL_CAP (8 << 20)

typedef void(__thiscall *pass_t)(BYTE *obj);
typedef void(__cdecl *handler_t)(void *payload, void *ctx);

static BYTE *g_obj;
static pass_t g_pass;
static DWORD g_mainTid;
static int g_guard;

static volatile LONG g_overMain, g_overOther;  // posts that found their half full
static volatile LONG g_spilled, g_spillFull, g_drains, g_delivered, g_noHandler, g_bad, g_spillMax;
#define SITES 8
static volatile LONG g_siteCount;
static void *volatile g_sites[SITES];
static DWORD g_siteTid[SITES];

// The spills, one per half (a key's half bit picks it), so posts that overflow the next half
// while the handlers of one spill run wait for their own pass. While a spill is active, posts to
// the half that overflowed (its key: counter, queue and half) go there too, so they stay behind
// the ones before them.
typedef struct {
    BYTE *volatile buf;
    volatile LONG used, active, key, queue;
    volatile LONG limit;  // offset of the first reservation that didn't fit
} Spill;
static Spill g_sp[2] = {{.limit = SPILL_CAP}, {.limit = SPILL_CAP}};
static volatile LONG g_drainTid;
static volatile LONG g_waits;  // records the drain found reserved but not yet written
#ifdef MSGQUEUE_TEST
static DWORD g_testMark;
#endif

static void note_site(void *ret)
{
    for (LONG i = 0; i < g_siteCount && i < SITES; i++)
        if (g_sites[i] == ret) return;
    LONG i = InterlockedIncrement(&g_siteCount) - 1;
    if (i >= SITES) return;
    g_siteTid[i] = GetCurrentThreadId();
    g_sites[i] = ret;
}

// The queue and half that `a` is the used-bytes word of, or 0.
static int half_of(LONG volatile *a, LONG *queue, LONG *half)
{
    ULONG_PTR off = (ULONG_PTR)a - (ULONG_PTR)(g_obj + Q0);
    if (off >= 2 * QSIZE) return 0;
    ULONG_PTR r = off % QSIZE;
    if (r != 0 && r != HALF) return 0;
    *queue = (LONG)(off / QSIZE);
    *half = r != 0;
    return 1;
}

static BYTE *spill_reserve(Spill *sp, LONG size, LONG key, LONG queue)
{
    if (!sp->buf) {
        BYTE *b = (BYTE *)VirtualAlloc(NULL, SPILL_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!b) return NULL;
        if (InterlockedCompareExchangePointer((PVOID volatile *)&sp->buf, b, NULL)) VirtualFree(b, 0, MEM_RELEASE);
    }
    LONG off = InterlockedExchangeAdd(&sp->used, size);
    if (off > SPILL_CAP - size) {  // full; every later reservation fails too, until the drain
        for (LONG m = sp->limit; off < m; m = sp->limit)
            InterlockedCompareExchange(&sp->limit, off, m);
        return NULL;
    }
    sp->queue = queue;
    sp->key = key;
    sp->active = 1;
    InterlockedIncrement(&g_spilled);
    for (LONG m = g_spillMax; off + size > m; m = g_spillMax)
        InterlockedCompareExchange(&g_spillMax, off + size, m);
    return sp->buf + off;
}

static LONG WINAPI hook_xadd(LONG volatile *a, LONG value)
{
    LONG q, h;
    if (!half_of(a, &q, &h) || value <= 0 || value > CAP) return InterlockedExchangeAdd(a, value);
    LONG key = (*(volatile LONG *)(g_obj + Q0 + q * QSIZE + COUNTER) << 2) | (q << 1) | h;
    Spill *sp = &g_sp[h];
    if (!(sp->active && sp->key == key)) {
        for (;;) {
            LONG cur = *a;
            if (cur < 0 || cur > CAP - value) break;
            if (InterlockedCompareExchange(a, cur + value, cur) == cur) return cur;
        }
        InterlockedIncrement(GetCurrentThreadId() == g_mainTid ? &g_overMain : &g_overOther);
        note_site(__builtin_return_address(0));
    }
    BYTE *rec = g_guard ? spill_reserve(sp, value, key, q) : NULL;
    if (!rec) {
        if (g_guard) InterlockedIncrement(&g_spillFull);
        return InterlockedExchangeAdd(a, value);  // what the game does
    }
    return (LONG)(rec - ((BYTE *)a + 0x80));  // the caller writes at half + 0x80 + this
}

// Whether a record's header has been written: a post reserves first and writes after, so a
// poster on another thread can still be between the two. Waits up to 20 ms.
static int record_ready(const volatile DWORD *r, LONG room)
{
    for (DWORD t0 = 0;;) {
        DWORD size = r[0], type = r[1];
        if (size >= 0x10 && !(size & 3) && size <= (DWORD)room && type < TYPES) return 1;
        if (!t0) {
            t0 = GetTickCount();
            InterlockedIncrement(&g_waits);
        } else if (GetTickCount() - t0 > 20) {
            return 0;
        }
        SwitchToThread();
    }
}

// Calls a spill's records' handlers, in order, as the game's pass does. Under the queue lock.
// A delivered record's size word is cleared, so a reservation not yet written never looks like an
// old record (the buffer is reused from the start after each drain).
static void drain_spill(Spill *sp)
{
    const BYTE *table = g_obj + TABLE + sp->queue * TABLE_SIZE;
    LONG off = 0;
    for (;;) {
        LONG used = sp->used, end = used < sp->limit ? used : sp->limit;
        while (off < end) {
            volatile DWORD *r = (volatile DWORD *)(sp->buf + off);
            if (!record_ready(r, end - off)) {
                InterlockedIncrement(&g_bad);
                memset(sp->buf + off, 0, end - off);
                off = end;  // drop the rest of this stretch
                break;
            }
            DWORD size = r[0], type = r[1];
            handler_t fn = *(handler_t const *)(table + type * 8);
            if (fn) {
                fn((void *)(r + 4), *(void *const *)(table + type * 8 + 4));
                InterlockedIncrement(&g_delivered);
            } else {
                InterlockedIncrement(&g_noHandler);
            }
#ifdef MSGQUEUE_TEST
            if (g_testMark) r[4] = g_testMark;  // the test can then tell a stale payload from a new record
#endif
            r[0] = 0;
            off += size;
        }
        // Done unless more records reached the spill while the handlers ran.
        if (InterlockedCompareExchange(&sp->used, 0, used) == used) break;
    }
    sp->active = 0;
    sp->limit = SPILL_CAP;
    InterlockedIncrement(&g_drains);
}

static LONG WINAPI hook_cmpx(LONG volatile *a, LONG exch, LONG cmp)
{
    DWORD tid = GetCurrentThreadId();
    if ((BYTE *)a != g_obj + LOCK || !(g_sp[0].active || g_sp[1].active) || g_drainTid == (LONG)tid ||
        InterlockedCompareExchange(&g_drainTid, (LONG)tid, 0) != 0)
        return InterlockedCompareExchange(a, exch, cmp);
    // The game's pass is starting and a spill holds records: run the pass here (its try-lock
    // comes back to this hook and goes through), then the spill of the half it walked, under the
    // same lock.
    LONG q = g_sp[0].active ? g_sp[0].queue : g_sp[1].queue;
    volatile LONG *counter = (volatile LONG *)(g_obj + Q0 + q * QSIZE + COUNTER);
    LONG before = *counter;
    g_pass(g_obj);
    Spill *sp = &g_sp[before & 1];
    if (*counter != before && sp->active && InterlockedCompareExchange(a, 1, 0) == 0) {
        drain_spill(sp);
        *a = 0;
    }
    g_drainTid = 0;
    return 1;  // "taken": the game's own call returns without a second pass
}

#ifdef MSGQUEUE_TEST
void msgqueue_test_setup(BYTE *obj, DWORD mainTid, int guard, void *pass, void **xadd, void **cmpx)
{
    g_obj = obj;
    g_mainTid = mainTid;
    g_guard = guard;
    g_pass = (pass_t)pass;
    *xadd = (void *)hook_xadd;
    *cmpx = (void *)hook_cmpx;
}

BYTE *msgqueue_test_spill(int i)
{
    return g_sp[i].buf;
}

LONG msgqueue_test_waits(void)
{
    return g_waits;
}

void msgqueue_test_mark(DWORD v)
{
    g_testMark = v;
}
#endif

static int patch_slot(void *volatile *slot, void *to)
{
    DWORD old;
    if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return 0;
    InterlockedExchange((volatile LONG *)slot, (LONG)(ULONG_PTR)to);
    VirtualProtect((void *)slot, 4, old, &old);
    return 1;
}

const char *msgqueue_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    static const BYTE kPass[] = {0x56, 0x57, 0x6A, 0x00, 0x8B, 0xF1, 0x6A, 0x01, 0x8D, 0xBE, 0x00, 0x10, 0x00, 0x01, 0x57, 0xFF, 0x15};
    static const BYTE kFlip[] = {0xFF, 0x81, 0x00, 0x01, 0x80, 0x00, 0x8B, 0x81, 0x00, 0x01, 0x80, 0x00, 0x83, 0xE0, 0x01,
                                 0x69, 0xC0, 0x80, 0x00, 0x40, 0x00, 0xC7, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00};
    static const BYTE kPost[] = {0x56, 0x8B, 0xB1, 0x00, 0x01, 0x80, 0x00, 0x83, 0xE6, 0x01, 0x69, 0xF6, 0x80, 0x00,
                                 0x40, 0x00, 0x57, 0x03, 0xF1, 0x68, 0x80, 0x00, 0x00, 0x00, 0x56, 0xFF, 0x15};
    // The pass after its try-lock: test eax,eax / jne out / ... and the walk's indirect call.
    static const BYTE kWalk[] = {0x8B, 0x46, 0x04, 0x8B, 0x4C, 0xC3, 0x04, 0x8D, 0x04, 0xC3, 0x8B, 0x00, 0x51, 0x8D, 0x56, 0x10, 0x52, 0xFF, 0xD0};
    BYTE *slotXadd = exe_import_slot(exe, "InterlockedExchangeAdd"), *slotCmpx = exe_import_slot(exe, "InterlockedCompareExchange");
    const BYTE *site = exe + g->queueMainSite;
    if (!g->queueObj || g->queueObj + LOCK + 4 >= nt->OptionalHeader.SizeOfImage || !slotXadd || !slotCmpx) return "addresses";
    // ... and its layout: imul eax,eax,800180h; lea ecx,[eax+esi+0C80h] (QSIZE, Q0)
    static const BYTE kLayout[] = {0x69, 0xC0, 0x80, 0x01, 0x80, 0x00, 0x8D, 0x8C, 0x30, 0x80, 0x0C, 0x00, 0x00};
    if (memcmp(exe + g->queuePass, kPass, sizeof kPass) || *(BYTE *const *)(exe + g->queuePass + sizeof kPass) != slotCmpx ||
        memcmp(exe + g->queuePass + 0x1b, kLayout, sizeof kLayout))
        return "pass";
    if (memcmp(exe + g->queueFlip, kFlip, sizeof kFlip)) return "flip";
    if (memcmp(exe + g->queuePost, kPost, sizeof kPost) || *(BYTE *const *)(exe + g->queuePost + sizeof kPost) != slotXadd)
        return "post";
    if (memcmp(exe + g->queueWalk, kWalk, sizeof kWalk)) return "handler call";
    if (site[0] != 0xB9 || *(BYTE *const *)(site + 1) != exe + g->queueObj || exe_branch_target(exe, site + 5) != exe + g->queuePass ||
        site[5] != 0xE8)
        return "main loop call";
    return NULL;
}

void msgqueue_init(const WCHAR *ini)
{
    g_guard = GetPrivateProfileIntW(L"probe", L"MsgQueueGuard", 1, ini);
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void *xadd = (void *)GetProcAddress(k32, "InterlockedExchangeAdd");
    void *cmpx = (void *)GetProcAddress(k32, "InterlockedCompareExchange");
    const char *why = g ? msgqueue_check(exe, g) : "not a known build of the game exe";
    BYTE *slotXadd = exe_import_slot(exe, "InterlockedExchangeAdd"), *slotCmpx = exe_import_slot(exe, "InterlockedCompareExchange");
    if (!why && (!xadd || !cmpx || *(void *const *)slotXadd != xadd || *(void *const *)slotCmpx != cmpx)) why = "import slots";
    if (why) {
        log_event("msgqueue: the game's event queue isn't the known code (%s); left alone", why);
        return;
    }
    g_obj = exe + g->queueObj;
    g_pass = (pass_t)(exe + g->queuePass);
    g_mainTid = GetCurrentThreadId();  // start_probe runs in DllMain, on the main thread
    // The post hook also logs; the pass hook only matters with the guard. The pass hook goes in
    // first: without it nothing may go to the spill. (With no spill active it only passes through.)
    int passHooked = g_guard && patch_slot((void *volatile *)slotCmpx, (void *)hook_cmpx);
    if (g_guard && !passHooked) {
        g_guard = 0;
        log_event("msgqueue: couldn't patch the pass's import slot; fix off");
    }
    if (!patch_slot((void *volatile *)slotXadd, (void *)hook_xadd)) {
        log_event("msgqueue: couldn't patch the post's import slot; left alone");
        g_guard = 0;
        if (!passHooked) g_obj = NULL;
        return;
    }
    if (g_guard)
        log_event("msgqueue: posts that don't fit the game's 4 MB event queue half go to a spill buffer, handled "
                  "right after the game's next pass over the queue; no game code changed");
    else
        log_event("msgqueue: logging the game's event queue (4 MB halves); fix off");
}

static LONG g_loggedHigh, g_loggedOver, g_loggedDrains, g_loggedSites;

static void log_counts(const char *when)
{
    log_event("msgqueue%s: posts that found their half full: %ld on the main thread, %ld on others; %ld spilled "
              "(most %.2f MB at once), handled after %ld passes: %ld delivered, %ld with no handler, %ld bad, %ld didn't fit the spill, "
              "%ld waited for",
              when, g_overMain, g_overOther, g_spilled, g_spillMax / 1048576.0, g_drains, g_delivered, g_noHandler, g_bad,
              g_spillFull, g_waits);
}

// Sampler thread.
void msgqueue_tick(void)
{
    if (!g_obj) return;
    LONG high = *(volatile LONG *)(g_obj + Q0 + HIGH);  // queue 0; the game's own high-water mark
    if (high > CAP && high >= g_loggedHigh + (256 << 10)) {
        g_loggedHigh = high;
        log_event("msgqueue: the game's event queue held %.2f MB in one half (room for 4 MB)", high / 1048576.0);
    }
    LONG idx = *(volatile LONG *)g_obj;
    static LONG loggedIdx;
    if (idx != loggedIdx) {
        loggedIdx = idx;
        log_event("msgqueue: the game switched to event queue %ld", idx);
    }
    LONG over = g_overMain + g_overOther;
    if (over != g_loggedOver || g_drains != g_loggedDrains) {
        g_loggedOver = over;
        g_loggedDrains = g_drains;
        log_counts("");
    }
    for (; g_loggedSites < g_siteCount && g_loggedSites < SITES && g_sites[g_loggedSites]; g_loggedSites++) {
        ULONG_PTR base = 0;
        const char *m = module_at(g_sites[g_loggedSites], &base);
        log_event("msgqueue: a post from %s+0x%lx (thread %lu%s) found its half full", m ? m : "?",
                  (unsigned long)((ULONG_PTR)g_sites[g_loggedSites] - base), g_siteTid[g_loggedSites],
                  g_siteTid[g_loggedSites] == g_mainTid ? ", main" : "");
    }
}

void msgqueue_report(void)
{
    if (!g_obj) return;
    LONG high = *(volatile LONG *)(g_obj + Q0 + HIGH);
    log_event("msgqueue: the most the game's event queue held in one half: %.2f MB (room for 4 MB)", high / 1048576.0);
    log_counts(" (total)");
}
