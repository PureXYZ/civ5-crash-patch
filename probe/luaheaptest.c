// luaheap test: the wrappers against two heaps of our own standing in for the game's Lua heap and
// the database's SQLite heap, the way lua_Alloc and the SQLite methods call them (flags 0; HeapSize
// before a free or a realloc). 4 threads (two per heap) do random allocations, reallocations and
// frees and check every block's size and contents. Built with luaheap.c -DLUAHEAP_TEST. Also: a
// block from a real heap (allocated before the hook) goes back to it, another heap's calls pass
// straight through, the two HeapCreate wrappers make growable heaps with almost nothing committed
// (and note the size asked for) that still work, and, when the game is installed, the database
// DLL's SQLite allocator matches dbheap_check (else that part is skipped).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

void luaheap_test_setup(HANDLE *luaVar, HANDLE *dbVar, void **a, void **r, void **f, void **s);
void luaheap_test_create(void **lua, void **db, SIZE_T **luaUpfront, SIZE_T **dbUpfront);
const char *dbheap_check(BYTE *db);
void log_event(const char *fmt, ...) { (void)fmt; }

typedef LPVOID(WINAPI *alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID(WINAPI *realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL(WINAPI *free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T(WINAPI *size_t_t)(HANDLE, DWORD, LPCVOID);
static alloc_t A;
static realloc_t R;
static free_t F;
static size_t_t S;
static HANDLE g_lua, g_db;
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
    HANDLE h = ((ULONG_PTR)arg & 1) ? g_lua : g_db;
    enum { N = 2048 };
    static __thread BYTE *blk[N];
    static __thread size_t len[N];
    static __thread BYTE val[N];
    for (int it = 0; it < 300000; it++) {
        int i = rnd(&seed) % N;
        if (!blk[i]) {
            size_t n = pick(&seed);
            blk[i] = A(h, 0, n);
            if (!blk[i]) { g_errors++; continue; }
            len[i] = n;
            val[i] = (BYTE)rnd(&seed);
            fill(blk[i], n, val[i]);
        } else if (rnd(&seed) & 1) {
            if (S(h, 0, blk[i]) != len[i] || !check(blk[i], len[i], val[i])) g_errors++;
            size_t n = pick(&seed);
            BYTE *q = R(h, 0, blk[i], n);
            if (!q) { g_errors++; continue; }
            if (!check(q, n < len[i] ? n : len[i], val[i])) g_errors++;
            blk[i] = q;
            len[i] = n;
            fill(q, n, val[i]);
        } else {
            if (S(h, 0, blk[i]) != len[i] || !check(blk[i], len[i], val[i])) g_errors++;
            if (!F(h, 0, blk[i])) g_errors++;
            blk[i] = NULL;
        }
        InterlockedIncrement(&g_ops);
    }
    for (int i = 0; i < N; i++) if (blk[i]) F(h, 0, blk[i]);
    return 0;
}

// Bytes committed in the heap's first segment (the region its handle starts).
static SIZE_T committed(HANDLE h)
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T n = 0;
    for (BYTE *p = (BYTE *)h; VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase == (void *)h; p += mbi.RegionSize)
        if (mbi.State == MEM_COMMIT) n += mbi.RegionSize;
    return n;
}

// Each module's HeapCreate wrapper, called the way the game calls it.
static int check_create(HANDLE(WINAPI *create)(DWORD, SIZE_T, SIZE_T), SIZE_T *upfront, SIZE_T asked, const char *what)
{
    HANDLE h = create(0, asked, 0);
    SIZE_T c = h ? committed(h) : 0;
    void *p = h ? HeapAlloc(h, 0, 3 << 20) : NULL;  // grows past what it started with
    int ok = h && *upfront == asked && c < 256 * 1024 && p && HeapFree(h, 0, p);
    printf("%s HeapCreate(0, %lu MB, 0): %lu KB committed, asked-for size noted %s, grows %s\n", what,
           (unsigned long)(asked >> 20), (unsigned long)(c >> 10), *upfront == asked ? "yes" : "NO", p ? "yes" : "NO");
    HANDLE fixed = create(0, 0x10000, 0x100000);  // a heap with a maximum size is made as asked
    if (!fixed || committed(fixed) < 0x10000) ok = 0;
    if (h) HeapDestroy(h);
    if (fixed) HeapDestroy(fixed);
    return ok;
}

static int busy_left(HANDLE h)
{
    PROCESS_HEAP_ENTRY e = {0};
    int left = 0;
    HeapLock(h);
    while (HeapWalk(h, &e)) if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) left++;
    HeapUnlock(h);
    return left;
}

int main(void)
{
    HANDLE lua = HeapCreate(0, 0, 0), db = HeapCreate(0, 0x500000, 0), other = HeapCreate(0, 0, 0);
    void *before = HeapAlloc(lua, 0, 100), *dbBefore = HeapAlloc(db, 0, 200);  // blocks from before the hook
    void *a, *r, *f, *s;
    luaheap_test_setup(&g_lua, &g_db, &a, &r, &f, &s);
    A = (alloc_t)a; R = (realloc_t)r; F = (free_t)f; S = (size_t_t)s;
    g_lua = lua;
    g_db = db;
    if (S(lua, 0, before) != 100 || !F(lua, 0, before) || S(db, 0, dbBefore) != 200 || !F(db, 0, dbBefore)) {
        printf("pre-hook block mishandled\n");
        return 1;
    }
    void *o = A(other, 0, 50);
    if (!o || HeapSize(other, 0, o) != 50 || !F(other, 0, o)) { printf("other heap mishandled\n"); return 1; }
    void *z = A(db, HEAP_ZERO_MEMORY, 1000);
    for (int i = 0; i < 1000; i++) if (((BYTE *)z)[i]) { printf("zero flag ignored\n"); return 1; }
    F(db, 0, z);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    printf("%ld operations, %ld errors; busy blocks left in the Windows heaps: Lua %d, SQLite %d\n", g_ops, g_errors,
           busy_left(lua), busy_left(db));

    void *cl, *cd;
    SIZE_T *ul, *ud;
    luaheap_test_create(&cl, &cd, &ul, &ud);
    if (!check_create((HANDLE(WINAPI *)(DWORD, SIZE_T, SIZE_T))cl, ul, 0x1400000, "Lua") ||
        !check_create((HANDLE(WINAPI *)(DWORD, SIZE_T, SIZE_T))cd, ud, 0x500000, "SQLite"))
        g_errors++;

    const char *dll = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V\\"
                      "CvGameDatabaseWin32Final Release.dll";
    HMODULE m = LoadLibraryExA(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        printf("database DLL check: skipped (game not installed here)\n");
    } else {
        const char *why = dbheap_check((BYTE *)m);
        printf("database DLL check: %s\n", why ? why : "the known SQLite allocator");
        if (why) g_errors++;
        FreeLibrary(m);
    }
    return g_errors != 0;
}
