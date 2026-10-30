// mimalloc redirect test DLL (no CRT of its own: every allocation goes through its
// msvcr90.dll import slots, the ones mialloc.c rewrites). Loaded late by
// CivilizationV_mitest.exe, like the VP and DB DLLs in the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern void *(__cdecl *const imp_malloc)(size_t) __asm__("__imp__malloc");
extern void(__cdecl *const imp_free)(void *) __asm__("__imp__free");
extern void *(__cdecl *const imp_realloc)(void *, size_t) __asm__("__imp__realloc");
extern void *(__cdecl *const imp_calloc)(size_t, size_t) __asm__("__imp__calloc");
extern void *(__cdecl *const imp_amalloc)(size_t, size_t) __asm__("__imp___aligned_malloc");
extern void(__cdecl *const imp_afree)(void *) __asm__("__imp___aligned_free");
extern char *(__cdecl *const imp_strdup)(const char *) __asm__("__imp___strdup");
extern HANDLE(__cdecl *const imp_heap)(void) __asm__("__imp___get_heap_handle");
extern void *(__cdecl *const imp_new)(size_t) __asm__("__imp_??2@YAPAXI@Z");
extern void(__cdecl *const imp_delete)(void *) __asm__("__imp_??3@YAXPAX@Z");
extern void *(__cdecl *const imp_newa)(size_t) __asm__("__imp_??_U@YAPAXI@Z");
extern void(__cdecl *const imp_deletea)(void *) __asm__("__imp_??_V@YAXPAX@Z");

void *memset(void *d, int c, size_t n)
{
    volatile unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    volatile unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

typedef struct {
    int nFailed;
    char failed[24][64];
    int earlyIsMi, mallocIsMi, newIsMi, strdupIsMi, staleReadable, largeIsCrt;
    DWORD freeBeforeMb, freeFullMb, freeAfterMb;
} MiTestResult;

static MiTestResult g_r;
static void *g_early;

static void fail(const char *what)
{
    if (g_r.nFailed < 24) lstrcpynA(g_r.failed[g_r.nFailed], what, 64);
    g_r.nFailed++;
}
#define CHECK(c, what) do { if (!(c)) fail(what); } while (0)

// Asks the probe (mi_is_in_heap_region). An alignment heuristic was flaky: after big frees
// the CRT heap can grow into a freed 4 MB aligned segment slot; HeapValidate misjudged too.
static int is_mi(const void *p)
{
    static int (*q)(const void *);
    if (!q) q = (int (*)(const void *))GetProcAddress(GetModuleHandleA("msimg32.dll"), "CivProbe_IsMimalloc");
    return q && q(p);
}

static DWORD free_mb(void)
{
    DWORD total = 0;
    ULONG_PTR a = 0x10000;
    MEMORY_BASIC_INFORMATION m;
    while (a < 0xFFFE0000u && VirtualQuery((LPCVOID)a, &m, sizeof m)) {
        if (m.State == MEM_FREE) total += (DWORD)(m.RegionSize >> 10);
        if (!m.RegionSize) break;
        a = (ULONG_PTR)m.BaseAddress + m.RegionSize;
        if (a < (ULONG_PTR)m.BaseAddress) break;
    }
    return total >> 10;
}

static void fill(unsigned char *p, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 7); }
static int same(const unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(seed + i * 7)) return 0;
    return 1;
}

#define NPTR 20000
static void *g_ptr[NPTR];
static volatile LONG g_threadOk;

static DWORD WINAPI other_thread(LPVOID arg)
{
    (void)arg;
    int ok = 1;
    for (int i = 0; i < NPTR; i += 2) {  // free half of the main thread's blocks here
        if (!same(g_ptr[i], 16 + i % 300, i)) ok = 0;
        imp_free(g_ptr[i]);
        g_ptr[i] = imp_malloc(40 + i % 500);  // and leave new ones for the main thread to free
        fill(g_ptr[i], 40 + i % 500, i + 1);
    }
    g_threadOk = ok;
    return 0;
}

__declspec(dllexport) MiTestResult *__cdecl mitest_run(void)
{
    g_r.earlyIsMi = is_mi(g_early);
    imp_free(g_early);

    // malloc/free, many sizes, data intact
    for (int i = 0; i < NPTR; i++) {
        size_t n = 16 + i % 300;
        g_ptr[i] = imp_malloc(n);
        if (!g_ptr[i]) { fail("malloc returned NULL"); return &g_r; }
        fill(g_ptr[i], n, i);
    }
    g_r.mallocIsMi = is_mi(g_ptr[0]);
    HANDLE th = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CHECK(g_threadOk, "cross-thread data");
    for (int i = 0; i < NPTR; i++) {
        int ok = (i & 1) ? same(g_ptr[i], 16 + i % 300, i) : same(g_ptr[i], 40 + i % 500, i + 1);
        if (!ok) { fail("data after cross-thread frees"); break; }
    }
    for (int i = 0; i < NPTR; i++) imp_free(g_ptr[i]);

    // realloc semantics
    unsigned char *p = imp_realloc(NULL, 100);
    CHECK(p != NULL, "realloc(NULL)");
    fill(p, 100, 3);
    p = imp_realloc(p, 100000);
    CHECK(p && same(p, 100, 3), "realloc grow keeps data");
    p = imp_realloc(p, 50);
    CHECK(p && same(p, 50, 3), "realloc shrink keeps data");
    CHECK(imp_realloc(p, 0) == NULL, "realloc(p, 0) frees, returns NULL");

    // calloc zeroes; malloc(0) is unique and non-NULL
    unsigned char *z = imp_calloc(1000, 7);
    int zero = z != NULL;
    for (int i = 0; z && i < 7000; i++) if (z[i]) zero = 0;
    CHECK(zero, "calloc zeroed");
    imp_free(z);
    void *a0 = imp_malloc(0), *b0 = imp_malloc(0);
    CHECK(a0 && b0 && a0 != b0, "malloc(0) unique");
    imp_free(a0);
    imp_free(b0);
    imp_free(NULL);

    // aligned
    for (size_t al = 1; al <= 8192; al <<= 1) {
        void *q = imp_amalloc(333, al);
        CHECK(q && ((ULONG_PTR)q % (al < 4 ? 4 : al)) == 0, "_aligned_malloc alignment");
        if (q) { fill(q, 333, 9); CHECK(same(q, 333, 9), "_aligned_malloc data"); }
        imp_afree(q);
    }
    // (alignment 3 is not tested: msvcr90 treats it as a fatal invalid parameter)
    imp_afree(NULL);

    // C++ operators
    void *o = imp_new(48), *oa = imp_newa(4000);
    g_r.newIsMi = is_mi(o);
    CHECK(o && oa, "operator new");
    imp_delete(o);
    imp_deletea(oa);
    imp_delete(NULL);

    // Blocks the CRT made itself stay on its heap; free and realloc must accept them.
    char *s = imp_strdup("hello from the CRT heap");
    g_r.strdupIsMi = is_mi(s);
    imp_free(s);
    s = imp_strdup("moved into mimalloc by realloc");
    s = imp_realloc(s, 5000);
    CHECK(s && lstrcmpA(s, "moved into mimalloc by realloc") == 0, "realloc of a CRT block keeps data");
    CHECK(is_mi(s), "realloc moved the CRT block to mimalloc");
    imp_free(s);

    // Freed blocks stay readable while their segment lives (the game reads deleted objects).
    // Bit i of staleReadable: size 40 KB, 200 KB, 1 MB.
    static const size_t staleSizes[3] = {2000, 8000, 16000};  // the sizes mimalloc serves
    void *keep = imp_malloc(4000);  // small: keeps the mimalloc segment alive
    unsigned char *gone[3];
    for (int k = 0; k < 3; k++) { gone[k] = imp_malloc(staleSizes[k]); }
    void *keep2 = imp_malloc(12000);
    for (int k = 0; k < 3; k++) imp_free(gone[k]);
    for (int round = 0; round < 30; round++) {  // ~1.5 s of allocation traffic: mimalloc purges lazily
        Sleep(50);
        for (int i = 0; i < 500; i++) imp_free(imp_malloc(64 + (i % 50) * 700));
    }
    g_r.staleReadable = 0;
    for (int k = 0; k < 3; k++) {
        MEMORY_BASIC_INFORMATION sm;
        if (VirtualQuery(gone[k] + 0x4CC, &sm, sizeof sm) && sm.State == MEM_COMMIT) g_r.staleReadable |= 1 << k;
    }
    imp_free(keep);
    imp_free(keep2);

    // The size split: large blocks stay on the CRT heap, realloc moves blocks across it.
    unsigned char *big = imp_malloc(100000), *bign = imp_new(50000), *biga = imp_amalloc(40000, 64);
    // Large CRT-heap blocks carry slack: a 2 MB aligned block ending on a page must take a small overrun.
    for (int k = 0; k < 8; k++) {
        size_t n = (2u << 20) - 64 * k;
        unsigned char *a2 = imp_amalloc(n, 16), *m2 = imp_malloc(n);
        for (int j = 0; j < 160; j++) { a2[n + j] = 1; m2[n + j] = 1; }
        imp_afree(a2);
        imp_free(m2);
    }
    g_r.largeIsCrt = !is_mi(big) && !is_mi(bign) && !is_mi(biga);
    imp_free(big);
    imp_delete(bign);
    imp_afree(biga);
    unsigned char *x = imp_malloc(1000);
    fill(x, 1000, 5);
    x = imp_realloc(x, 50000);  // small -> large
    CHECK(x && same(x, 1000, 5) && !is_mi(x), "realloc past the split moves to the CRT heap");
    fill(x, 50000, 6);
    x = imp_realloc(x, 3000);   // large -> small
    CHECK(x && same(x, 3000, 6) && is_mi(x), "realloc below the split moves to mimalloc");
    imp_free(x);
    unsigned char *zc = imp_calloc(10000, 10);
    CHECK(zc && !is_mi(zc) && zc[99999] == 0, "large calloc on the CRT heap, zeroed");
    imp_free(zc);

    // huge block
    unsigned char *h = imp_malloc(64u << 20);
    CHECK(h != NULL, "64 MB malloc");
    if (h) { h[0] = 1; h[(64u << 20) - 1] = 2; imp_free(h); }

    // Address space comes back after a burst of small blocks is freed.
    g_r.freeBeforeMb = free_mb();
    static void *burst[200000];
    for (int i = 0; i < 200000; i++) burst[i] = imp_malloc(1000);
    g_r.freeFullMb = free_mb();
    for (int i = 0; i < 200000; i++) imp_free(burst[i]);
    Sleep(300);
    void *nudge = imp_malloc(16);  // let mimalloc run its delayed purges
    imp_free(nudge);
    Sleep(300);
    g_r.freeAfterMb = free_mb();

    CHECK(HeapValidate(imp_heap(), 0, NULL), "CRT heap valid");
    return &g_r;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g_early = imp_malloc(100);  // patched before our init runs?
    return TRUE;
}