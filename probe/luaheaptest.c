// luaheap test: the wrappers against a heap of our own standing in for the game's Lua heap, the
// way lua_Alloc calls them (flags 0; HeapSize before a free or a realloc). 4 threads do random
// allocations, reallocations and frees and check every block's size and contents. Built with
// luaheap.c -DLUAHEAP_TEST. Also: a block from the real heap (allocated before the hook) goes back
// to it, and another heap's calls pass straight through.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

void luaheap_test_setup(HANDLE *heapVar, void **a, void **r, void **f, void **s);
void log_event(const char *fmt, ...) { (void)fmt; }

typedef LPVOID(WINAPI *alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID(WINAPI *realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL(WINAPI *free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T(WINAPI *size_t_t)(HANDLE, DWORD, LPCVOID);
static alloc_t A;
static realloc_t R;
static free_t F;
static size_t_t S;
static HANDLE g_lua;
static volatile LONG g_errors, g_ops;

static unsigned rnd(unsigned *s) { *s = *s * 1103515245 + 12345; return *s >> 8; }

static size_t pick(unsigned *s)
{
    unsigned r = rnd(s) % 100;
    return r < 70 ? 1 + rnd(s) % 64 : r < 95 ? 64 + rnd(s) % 4096 : r < 99 ? 4096 + rnd(s) % 200000 : 200000 + rnd(s) % 3000000;
}

static void fill(BYTE *p, size_t n, BYTE v) { memset(p, v, n); }
static int check(const BYTE *p, size_t n, BYTE v)
{
    for (size_t i = 0; i < n; i += 1 + n / 64) if (p[i] != v) return 0;
    return !n || p[n - 1] == v;
}

static DWORD WINAPI worker(LPVOID arg)
{
    unsigned seed = (unsigned)(ULONG_PTR)arg * 7919 + 1;
    enum { N = 2048 };
    static __thread BYTE *blk[N];
    static __thread size_t len[N];
    static __thread BYTE val[N];
    for (int it = 0; it < 300000; it++) {
        int i = rnd(&seed) % N;
        if (!blk[i]) {
            size_t n = pick(&seed);
            blk[i] = A(g_lua, 0, n);
            if (!blk[i]) { g_errors++; continue; }
            len[i] = n;
            val[i] = (BYTE)rnd(&seed);
            fill(blk[i], n, val[i]);
        } else if (rnd(&seed) & 1) {
            if (S(g_lua, 0, blk[i]) != len[i] || !check(blk[i], len[i], val[i])) g_errors++;
            size_t n = pick(&seed);
            BYTE *q = R(g_lua, 0, blk[i], n);
            if (!q) { g_errors++; continue; }
            if (!check(q, n < len[i] ? n : len[i], val[i])) g_errors++;
            blk[i] = q;
            len[i] = n;
            fill(q, n, val[i]);
        } else {
            if (S(g_lua, 0, blk[i]) != len[i] || !check(blk[i], len[i], val[i])) g_errors++;
            if (!F(g_lua, 0, blk[i])) g_errors++;
            blk[i] = NULL;
        }
        InterlockedIncrement(&g_ops);
    }
    for (int i = 0; i < N; i++) if (blk[i]) F(g_lua, 0, blk[i]);
    return 0;
}

int main(void)
{
    HANDLE lua = HeapCreate(0, 0, 0), other = HeapCreate(0, 0, 0);
    void *before = HeapAlloc(lua, 0, 100);  // a block from before the hook
    void *a, *r, *f, *s;
    luaheap_test_setup(&g_lua, &a, &r, &f, &s);
    A = (alloc_t)a; R = (realloc_t)r; F = (free_t)f; S = (size_t_t)s;
    g_lua = lua;
    if (S(lua, 0, before) != 100 || !F(lua, 0, before)) { printf("pre-hook block mishandled\n"); return 1; }
    void *o = A(other, 0, 50);
    if (!o || HeapSize(other, 0, o) != 50 || !F(other, 0, o)) { printf("other heap mishandled\n"); return 1; }
    void *z = A(lua, HEAP_ZERO_MEMORY, 1000);
    for (int i = 0; i < 1000; i++) if (((BYTE *)z)[i]) { printf("zero flag ignored\n"); return 1; }
    F(lua, 0, z);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    PROCESS_HEAP_ENTRY e = {0};
    int left = 0;
    HeapLock(lua);
    while (HeapWalk(lua, &e)) if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) left++;
    HeapUnlock(lua);
    printf("%ld operations, %ld errors; busy blocks left in the Windows heap: %d\n", g_ops, g_errors, left);
    return g_errors != 0;
}
