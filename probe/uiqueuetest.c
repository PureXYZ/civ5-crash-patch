// uiqueue handler-table test: a stand-in exe image with the queue's handler tables and processing
// conditions at the game's offsets. Records are dispatched like exe+0x23aba0 does (fn(payload,
// context) from the table). Built with uiqueue.c -DUIQUEUE_TEST.
// Checks: wrapped handlers get their own arguments; rewrapping doesn't wrap twice and picks up a
// re-registered handler; other threads pass through; data-dirty and TeamMet records within their
// own windows are merged and the last one runs late (with its payload and the table's context)
// only when the game's conditions and the queue lock allow; a long frame is counted.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

int uiqueue_test_setup(BYTE *exe, int stats, int dirtyMs, int teamMetMs, void (**frame)(LONGLONG), void (**late)(LONGLONG));
void uiqueue_test_rewrap(void);
LONG uiqueue_test_counts(int teamMet, LONG *skipped, LONG *late, LONG *longFrames);
LONG uiqueue_test_type_calls(int type);
void log_event(const char *fmt, ...) { (void)fmt; }

#define RVA_OWNER 0x2610200
#define RVA_TABLES 0x2610208
#define RVA_LOCKED 0x29e0447
#define RVA_GAME 0x8c8e74
#define RVA_MODE 0x2610f80
#define IMAGE 0x29e1000

typedef void(__cdecl *handler_t)(void *payload, void *ctx);
static BYTE *g_exe;
static int g_fail;
static BYTE g_dontProcess;
static LONG g_aCalls, g_bCalls, g_dirtyCalls, g_metCalls;
static void *g_lastPayload, *g_lastCtx;
static BYTE g_lastBytes[8];
static int g_ctxA, g_ctxD, g_ctxM;
static BYTE g_metBytes[8];
static void *g_metCtx;

#define CHECK(c, ...)                                                                                                          \
    do {                                                                                                                       \
        if (!(c)) {                                                                                                            \
            printf("FAIL line %d: ", __LINE__);                                                                                \
            printf(__VA_ARGS__);                                                                                               \
            printf("\n");                                                                                                      \
            g_fail = 1;                                                                                                        \
        }                                                                                                                      \
    } while (0)

static BYTE __fastcall check_fn(void *self, void *edx) { return g_dontProcess; }  // thiscall stand-in
static void *g_vtable[0x3a4 / 4 + 1];
static void *g_checkObj[1] = {g_vtable};
static BYTE g_game[0x1464];

static void __cdecl handler_a(void *payload, void *ctx)
{
    g_aCalls++;
    g_lastPayload = payload;
    g_lastCtx = ctx;
}
static void __cdecl handler_b(void *payload, void *ctx) { g_bCalls++; }
static void __cdecl handler_dirty(void *payload, void *ctx)
{
    g_dirtyCalls++;
    g_lastPayload = payload;
    g_lastCtx = ctx;
    memcpy(g_lastBytes, payload, 8);
}
static void __cdecl handler_met(void *payload, void *ctx)
{
    g_metCalls++;
    g_metCtx = ctx;
    memcpy(g_metBytes, payload, 8);
}
static void __cdecl handler_slow(void *payload, void *ctx) { Sleep(45); }

static handler_t *slot(int table, int type) { return (handler_t *)(g_exe + RVA_TABLES + table * 0x600 + type * 8); }
static void dispatch(int type, BYTE *rec)
{
    handler_t *e = slot(0, type);
    e[0](rec + 0x44, ((void **)e)[1]);
}
static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static DWORD WINAPI other_thread(void *p)
{
    dispatch(0x10, p);
    return 0;
}

int main(void)
{
    g_exe = VirtualAlloc(NULL, IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_exe) return 2;
    g_vtable[0x3a4 / 4] = (void *)check_fn;
    *(void **)(g_game + 0x1460) = g_checkObj;
    *(void **)(g_exe + RVA_GAME) = g_game;
    *slot(0, 0x10) = handler_a;
    ((void **)slot(0, 0x10))[1] = &g_ctxA;
    *slot(0, 0x66) = handler_dirty;
    ((void **)slot(0, 0x66))[1] = &g_ctxD;
    *slot(0, 0x20) = handler_slow;
    *slot(0, 0x77) = handler_met;
    ((void **)slot(0, 0x77))[1] = &g_ctxM;

    void (*frame)(LONGLONG), (*late)(LONGLONG);
    if (!uiqueue_test_setup(g_exe, 1, 50, 120, &frame, &late)) {
        printf("FAIL setup\n");
        return 1;
    }
    CHECK(*slot(0, 0x10) != handler_a, "slot not wrapped");
    CHECK(*slot(1, 0x10) == NULL, "empty slot wrapped");

    static BYTE rec[0x44 + 8];
    *(DWORD *)rec = sizeof rec;
    *(DWORD *)(rec + 4) = 0x10;
    dispatch(0x10, rec);
    CHECK(g_aCalls == 1 && g_lastPayload == rec + 0x44 && g_lastCtx == &g_ctxA, "arguments: calls %ld", g_aCalls);
    CHECK(uiqueue_test_type_calls(0x10) == 1, "stats count %ld", uiqueue_test_type_calls(0x10));

    // rewrap: no double wrap; a re-registered handler is picked up
    uiqueue_test_rewrap();
    dispatch(0x10, rec);
    CHECK(g_aCalls == 2, "after rewrap: calls %ld", g_aCalls);
    *slot(0, 0x10) = handler_b;
    uiqueue_test_rewrap();
    dispatch(0x10, rec);
    CHECK(g_bCalls == 1 && g_aCalls == 2, "re-registered: a %ld b %ld", g_aCalls, g_bCalls);

    // other threads pass through, uncounted
    LONG before = uiqueue_test_type_calls(0x10);
    HANDLE th = CreateThread(NULL, 0, other_thread, rec, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CHECK(g_bCalls == 2 && uiqueue_test_type_calls(0x10) == before, "other thread: b %ld, counted %ld", g_bCalls,
          uiqueue_test_type_calls(0x10) - before);

    // TeamMet records (window 120 ms) alongside: the first runs, the next is merged
    static BYTE m1[0x44 + 8], m2[0x44 + 8];
    *(DWORD *)m1 = *(DWORD *)m2 = sizeof m1;
    memcpy(m1 + 0x44, "met 1/2.", 8);
    memcpy(m2 + 0x44, "met 3/4.", 8);
    dispatch(0x77, m1);
    dispatch(0x77, m2);
    CHECK(g_metCalls == 1 && !memcmp(g_metBytes, "met 1/2.", 8), "first TeamMet: %ld", g_metCalls);

    // data-dirty records: the first runs, the next ones within 50 ms are merged
    static BYTE d1[0x44 + 8], d2[0x44 + 8];
    *(DWORD *)d1 = *(DWORD *)d2 = sizeof d1;
    memcpy(d1 + 0x44, "first...", 8);
    memcpy(d2 + 0x44, "second..", 8);
    dispatch(0x66, d1);
    CHECK(g_dirtyCalls == 1 && !memcmp(g_lastBytes, "first...", 8), "first dirty: %ld", g_dirtyCalls);
    dispatch(0x66, d1);
    dispatch(0x66, d2);
    LONG skipped, lateN, longFrames;
    CHECK(g_dirtyCalls == 1, "merged ran: %ld", g_dirtyCalls);
    late(qpc());
    CHECK(g_dirtyCalls == 1, "late ran too early");
    Sleep(60);
    volatile LONG *lock = (volatile LONG *)(g_exe + RVA_OWNER + 0xd00);
    *lock = 1;  // the game's pass holds the queue
    late(qpc());
    CHECK(g_dirtyCalls == 1, "late ran under the queue lock");
    *lock = 0;
    g_exe[RVA_LOCKED] = 1;  // a core update is running
    late(qpc());
    CHECK(g_dirtyCalls == 1, "late ran during a core update");
    g_exe[RVA_LOCKED] = 0;
    g_dontProcess = 1;  // the game's per-frame check says no
    late(qpc());
    CHECK(g_dirtyCalls == 1, "late ran against the game's check");
    g_dontProcess = 0;
    late(qpc());
    CHECK(g_dirtyCalls == 2 && !memcmp(g_lastBytes, "second..", 8) && g_lastCtx == &g_ctxD && *lock == 0,
          "late run: calls %ld, ctx ok %d, lock %ld", g_dirtyCalls, g_lastCtx == &g_ctxD, *lock);
    CHECK(g_metCalls == 1, "TeamMet ran late before its own window (%ld)", g_metCalls);
    Sleep(80);
    late(qpc());
    CHECK(g_metCalls == 2 && !memcmp(g_metBytes, "met 3/4.", 8) && g_metCtx == &g_ctxM && *lock == 0, "TeamMet late run: %ld",
          g_metCalls);
    LONG mruns = uiqueue_test_counts(1, &skipped, &lateN, &longFrames);
    CHECK(mruns == 2 && skipped == 1 && lateN == 1, "TeamMet counts: runs %ld skipped %ld late %ld", mruns, skipped, lateN);
    late(qpc() + 1000000000);
    CHECK(g_dirtyCalls == 2 && g_metCalls == 2, "late ran twice");
    LONG runs = uiqueue_test_counts(0, &skipped, &lateN, &longFrames);
    CHECK(runs == 2 && skipped == 2 && lateN == 1, "counts: runs %ld skipped %ld late %ld", runs, skipped, lateN);
    Sleep(60);
    dispatch(0x66, d1);  // after the window: runs at once
    CHECK(g_dirtyCalls == 3, "after the window: %ld", g_dirtyCalls);

    // a long frame
    frame(qpc());
    dispatch(0x20, rec);
    frame(qpc());
    uiqueue_test_counts(0, &skipped, &lateN, &longFrames);
    CHECK(longFrames == 1, "long frames %ld", longFrames);

    printf(g_fail ? "uiqueuetest: FAILED\n" : "uiqueuetest: all checks passed\n");
    return g_fail;
}
