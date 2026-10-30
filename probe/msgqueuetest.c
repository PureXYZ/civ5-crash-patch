// msgqueue fix test: a stand-in for the game's event queue and its once-a-frame pass, with the
// posts and the pass going through the hooks as they go through the exe's imports. The main thread
// posts every frame and now and then "loads": 5+ MB of posts with no pass, past a half's 4 MB.
// Two workers post all along, and some handlers post too. Built with msgqueue.c -DMSGQUEUE_TEST.
// Checks: the main thread's records all arrive, once each, in order; nothing lands past a half.
// Workers' records can still be lost: a post reads the counter before it reserves, so a flip in
// between puts it in a half that was already walked. That is the game's own protocol, not checked.
// Nor is its other race: the pass can read a worker's record after the header is written and
// before the payload is, and get the payload of an older record there (counted as "stale reads";
// the stand-in pass and the spill's drain mark each record they delivered, so they can't pass as
// main-thread records; seq 0 is memory never written). The CI run on 2026-10-30 failed on one
// such read before they were told apart.
// Usage: msgqueuetest [guard=1] [workers=2] [slow=0]. Guard 0 shows the game's behaviour
// (overruns) and always exits 0. Slow 1 has workers yield between header and payload, which makes
// that race common: the spill must still deliver each main-thread record once.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

void msgqueue_test_setup(BYTE *obj, DWORD mainTid, int guard, void *pass, void **xadd, void **cmpx);
BYTE *msgqueue_test_spill(int i);
LONG msgqueue_test_waits(void);
void msgqueue_test_mark(DWORD v);
void log_event(const char *fmt, ...) { (void)fmt; }
const char *module_at(const void *addr, ULONG_PTR *base) { (void)addr; (void)base; return NULL; }

#define Q0 0xc80
#define HALF 0x400080
#define CAP 0x400000
#define COUNTER 0x800100
#define LOCK 0x1001000
#define REC 0x80

typedef LONG(WINAPI *xadd_t)(LONG volatile *, LONG);
typedef LONG(WINAPI *cmpx_t)(LONG volatile *, LONG, LONG);
static xadd_t g_xadd;
static cmpx_t g_cmpx;
static BYTE *g_obj, *g_q;
static volatile LONG g_stop, g_overruns, g_bad;
static LONG g_posted[4], g_got[4], g_last[4], g_outOfOrder[4], g_dup[4];
static int g_slowWriters;
static LONG g_stale;
#define WALKED 5  // the poster id the stand-in pass leaves in a record it walked

static void post(LONG who, LONG seq, DWORD type)
{
    LONG h = *(volatile LONG *)(g_q + COUNTER) & 1;  // as the game's posts: pick the half first
    BYTE *half = g_q + h * HALF;
    LONG off = g_xadd((LONG volatile *)half, REC);
    DWORD *r = (DWORD *)(half + 0x80 + off);
    int inSpill = 0;
    for (int i = 0; i < 2; i++) {
        BYTE *spill = msgqueue_test_spill(i);
        inSpill |= spill && (BYTE *)r >= spill && (BYTE *)r < spill + (8 << 20);
    }
    if (!inSpill && ((BYTE *)r < half + 0x80 || (BYTE *)r > half + 0x80 + CAP - REC)) {
        InterlockedIncrement(&g_overruns);  // would have run past the half
        return;
    }
    volatile DWORD *v = r;  // header first, then the payload, in the game's order (exe+0xc25c)
    v[0] = REC;
    v[1] = type;
    v[2] = v[3] = 0;
    if (g_slowWriters && who != 0 && who != 3) SwitchToThread();  // a poster preempted in between
    v[4] = who;
    v[5] = seq;
    InterlockedIncrement(&g_posted[who]);
}

static void __cdecl on_event(void *payload, void *ctx)
{
    const LONG *p = payload;
    LONG who = p[0], seq = p[1];
    if (who == WALKED || (who == 0 && seq == 0)) { g_stale++; return; }  // seq 0: memory never written
    if (who < 0 || who > 3) { g_bad++; return; }
    g_got[who]++;
    if (seq == g_last[who]) g_dup[who]++;
    else if (seq != g_last[who] + 1) g_outOfOrder[who]++;
    g_last[who] = seq;
    static LONG replySeq;
    if (!ctx && who == 0 && seq % 97 == 0) post(3, ++replySeq, 7);  // a handler posting
}

// The game's pass (exe+0x59d20), thiscall: try-lock through the import, flip, walk, unlock. Unlike
// the game's, it waits for a record a worker has reserved but not yet written, so that race of the
// game's own protocol doesn't show up as lost main-thread records (the walk stops at a bad one).
static void __thiscall pass(BYTE *obj)
{
    LONG volatile *lock = (LONG volatile *)(obj + LOCK);
    if (g_cmpx(lock, 1, 0)) return;
    LONG c = InterlockedIncrement((volatile LONG *)(g_q + COUNTER));
    *(volatile LONG *)(g_q + (c & 1) * HALF) = 0;
    BYTE *old = g_q + ((c - 1) & 1) * HALF;
    LONG used = *(volatile LONG *)old;
    if (used > CAP) used = CAP;
    void **table = (void **)(obj + 8);
    for (BYTE *p = old + 0x80, *end = old + 0x80 + used; p < end;) {
        DWORD *r = (DWORD *)p;
        for (DWORD t0 = GetTickCount(); (r[0] != REC || r[1] >= 192) && GetTickCount() - t0 < 50;) SwitchToThread();
        if (r[0] != REC || r[1] >= 192 || !table[r[1] * 2]) { g_bad++; break; }
        ((void(__cdecl *)(void *, void *))table[r[1] * 2])(r + 4, table[r[1] * 2 + 1]);
        r[1] = 999;  // a stale record walked again would show as bad
        r[4] = WALKED;
        p += REC;
    }
    *lock = 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    LONG who = (LONG)(ULONG_PTR)arg, seq = 0;
    while (!g_stop) {
        post(who, ++seq, 7);
        if (seq % 64 == 0) Sleep(1);  // ~8 MB/s each at most
    }
    return 0;
}

int main(int argc, char **argv)
{
    int guard = argc > 1 ? atoi(argv[1]) : 1;
    g_obj = VirtualAlloc(NULL, LOCK + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_q = g_obj + Q0;
    void **table = (void **)(g_obj + 8);
    table[7 * 2] = (void *)on_event;
    table[8 * 2] = (void *)on_event;
    table[8 * 2 + 1] = (void *)1;  // same check, no posting
    void *x, *c;
    msgqueue_test_setup(g_obj, GetCurrentThreadId(), guard, (void *)pass, &x, &c);
    msgqueue_test_mark(WALKED);  // the spill's drain marks what it delivered too
    g_xadd = (xadd_t)x;
    g_cmpx = (cmpx_t)c;
    int nw = argc > 2 ? atoi(argv[2]) : 2;
    g_slowWriters = argc > 3 && atoi(argv[3]);
    HANDLE th[2];
    for (int i = 0; i < nw; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
    LONG seq = 0;
    int frames = 0, loads = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 6000) {
        int n = ++frames % 25 ? 500 : (loads++, 42000);  // 42000 x 128 bytes = 5.1 MB in one "load"
        for (int i = 0; i < n; i++) post(0, ++seq, i % 50 ? 7 : 8);
        pass(g_obj);  // the main loop's call (exe+0xb4067)
        Sleep(2);
    }
    g_stop = 1;
    if (nw) WaitForMultipleObjects(nw, th, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) pass(g_obj);
    printf("guard=%d frames=%d loads=%d overruns=%ld bad=%ld spill waits=%ld stale reads=%ld\n", guard, frames, loads,
           g_overruns, g_bad, msgqueue_test_waits(), g_stale);
    for (int w = 0; w < 4; w++)  // 0 = main, 3 = handlers
        printf("poster %d: posted %ld got %ld outOfOrder %ld dup %ld\n", w, g_posted[w], g_got[w], g_outOfOrder[w], g_dup[w]);
    return guard && (g_overruns || g_bad || g_outOfOrder[0] || g_dup[0] || g_got[0] != g_posted[0] || g_outOfOrder[3]);
}
