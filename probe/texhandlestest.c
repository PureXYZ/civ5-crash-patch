// texhandles test: the game's own texture handle pool code, grown by texhandles_apply to 16,384 and
// then 32,768 handles, in the installed DX11 exe mapped as an image (imports not bound). The few imports that code calls are
// bound to our CRT, and the exe's small-object allocator (FSBA) is sent to _aligned_malloc. Then:
// the constructor builds the pool, every handle is taken (1..N-2, then 0 when full), 768 handles
// across the old 0x3000 limit (and the last one) are released into one release queue (past its
// 0x200 inline entries), the frame flush and the recycle move them to the free list, they are
// taken back, AddRef / Release work past 0x3000, two of the game's record getters read the moved
// records, and the destructor frees what the pool allocated. Nothing past the grown pool may change,
// and the old records' space must be inaccessible. Skipped when the game isn't installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

#define OLD_N 0x3000
#define MAX_N 0x8000
#define REC 0x398

void log_event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int g_fail;
static DWORD N;  // this run's handle count
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

typedef void *(__thiscall *Ctor_t)(void *pool);
typedef void(__thiscall *Fn_t)(void *pool);
typedef DWORD(__thiscall *Alloc_t)(void *pool);
typedef DWORD(__thiscall *Ref_t)(void *pool, DWORD h);
typedef DWORD(__stdcall *Get_t)(DWORD h);

static void *__cdecl fsba_alloc(size_t n) { (void)n; return NULL; }
static char __cdecl fsba_free(void *p) { (void)p; return 0; }
static void *__cdecl aligned_alloc_any(size_t n, size_t align) { return _aligned_malloc(n, align < 8 ? 8 : align); }
static LONG g_allocs;
static void *__cdecl exe_alloc(size_t n, size_t align)  // exe+0x413a80 (size, align, file, line)
{
    InterlockedIncrement(&g_allocs);
    return aligned_alloc_any(n, align);
}

static int put4(BYTE *p, const void *v)
{
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, v, 4);
    VirtualProtect(p, 4, old, &old);
    return 1;
}

static int bind(BYTE *exe, const char *name, void *fn)
{
    BYTE *slot = exe_import_slot(exe, name);
    if (!slot) printf("no import %s\n", name);
    return slot && put4(slot, &fn);
}

static int jump(BYTE *exe, DWORD rva, const BYTE *want, size_t len, void *fn)
{
    BYTE *p = exe + rva;
    if (memcmp(p, want, len)) {
        printf("unexpected code at exe+0x%lx\n", (unsigned long)rva);
        return 0;
    }
    BYTE code[5] = {0xE9};
    LONG rel = (LONG)((BYTE *)fn - (p + 5));
    memcpy(code + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, code, 5);
    VirtualProtect(p, 5, old, &old);
    return 1;
}

// One run on a fresh mapping of the exe: -1 = skipped (no game here), else the failures.
static int run(DWORD n)
{
    const WCHAR *path = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V\\CivilizationV_DX11.exe";
    HMODULE m = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        printf("texhandles test: skipped (game not installed here)\n");
        return -1;
    }
    N = n;
    g_fail = 0;
    g_allocs = 0;
    BYTE *exe = (BYTE *)m;
    const GameExe *g = game_exe(exe);
    if (!g || !g->texPool) {
        printf("texhandles test: not the known DX11 exe\n");
        return 1;
    }
    const char *why = texhandles_check(exe, g);
    if (why) {
        printf("texhandles_check: %s\n", why);
        return 1;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    int ok = bind(exe, "InterlockedCompareExchange", (void *)GetProcAddress(k32, "InterlockedCompareExchange")) &&
             bind(exe, "memset", (void *)memset) && bind(exe, "memcpy", (void *)memcpy) &&
             bind(exe, "_aligned_malloc", (void *)aligned_alloc_any) && bind(exe, "_aligned_free", (void *)_aligned_free);
    // FSBAAllocate: push esi; push edi; push 1; call GetSmallObjectAllocator. FSBADeallocate:
    // push ebx; push ebp; push esi; push edi; push 1. The aligned allocator: mov eax,[esp+8]; test eax,eax
    static const BYTE kFsbaAlloc[] = {0x56, 0x57, 0x6A, 0x01, 0xE8}, kFsbaFree[] = {0x53, 0x55, 0x56, 0x57, 0x6A, 0x01},
                      kAlloc[] = {0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0};
    ok = ok && jump(exe, 0x413870, kFsbaAlloc, sizeof kFsbaAlloc, (void *)fsba_alloc) &&
         jump(exe, 0x4138b0, kFsbaFree, sizeof kFsbaFree, (void *)fsba_free) &&
         jump(exe, 0x413a80, kAlloc, sizeof kAlloc, (void *)exe_alloc);
    if (!ok) return 1;

    BYTE *rec = VirtualAlloc(NULL, N * REC, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    why = texhandles_apply(exe, g, N, rec);
    if (why) {
        printf("texhandles_apply: %s\n", why);
        return 1;
    }
    CHECK(texhandles_check(exe, g) != NULL, "the check still passes after the change");

    BYTE *pool = exe + g->texPool;
    DWORD extra = N - OLD_N, items = 0xe088 + 4 * extra, queues = 0xc048 + 4 * extra, frame = 0xc044 + 4 * extra;
    DWORD vec = 0x32088 + 16 * extra, freeCount = 0x328b8 + 16 * extra, end = 0x328c0 + 16 * extra;
    // A reference from the texture code (exe+0x4bd550: mov eax,[items+eax*4]) and one into the
    // records (exe+0x4bc0dc: add ebp, records).
    CHECK(*(DWORD *)(exe + 0x4bd553) == (DWORD)(ULONG_PTR)(pool + items), "item reference not moved");
    CHECK(*(DWORD *)(exe + 0x4bc0de) == (DWORD)(ULONG_PTR)rec, "record reference not moved");
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *trip = (BYTE *)(((ULONG_PTR)pool + end + 0xfff) & ~(ULONG_PTR)0xfff);
    CHECK(VirtualQuery(trip, &mbi, sizeof mbi) && mbi.Protect == PAGE_NOACCESS, "old records not inaccessible");

    ((Ctor_t)(exe + 0x4c7280))(pool);
    CHECK(*(DWORD *)(pool + 0x40) == 1, "next after the constructor: %lu", *(DWORD *)(pool + 0x40));
    CHECK(*(DWORD *)(pool + frame) == 0, "frame index");
    for (int q = 0; q < 4; q++) {
        DWORD *v = (DWORD *)(pool + queues + q * 0x810);
        CHECK(v[0] == (DWORD)(ULONG_PTR)(v + 3) && v[1] == 0 && v[2] == 0x200, "release queue %d", q);
    }
    DWORD *pv = (DWORD *)(pool + vec);
    CHECK(pv[0] == (DWORD)(ULONG_PTR)(pv + 3) && pv[2] == 0x200, "old-handle vector");

    Alloc_t alloc = (Alloc_t)(exe + 0x4c5cb0);
    Ref_t release = (Ref_t)(exe + 0x1e8df0), addref = (Ref_t)(exe + 0x306290);
    Fn_t flush = (Fn_t)(exe + 0x4c33d0), recycle = (Fn_t)(exe + 0x4c48d0);
    DWORD h;
    for (h = 1; h <= N - 2; h++) {
        DWORD r = alloc(pool);
        if (r != h) {
            CHECK(0, "alloc %lu returned %lu", (unsigned long)h, (unsigned long)r);
            break;
        }
    }
    CHECK(alloc(pool) == 0, "alloc past the end");
    DWORD *refs = (DWORD *)(pool + 0x44);
    int bad = 0;
    for (h = 1; h <= N - 2; h++) bad += refs[h] != 1;
    CHECK(!bad && !refs[0] && !refs[N - 1], "refcounts: %d not 1", bad);
    CHECK(*(DWORD *)(pool + frame) == 0, "frame index after the refcounts filled");

    static BYTE released[MAX_N];
    memset(released, 0, sizeof released);
    DWORD count = 0;
    for (h = 0x2e00; h < 0x3100; h++) released[h] = 1;
    released[N - 2] = 1;
    for (h = 0; h < N; h++)
        if (released[h]) {
            CHECK(release(pool, h) == 0, "release %lu", (unsigned long)h);
            count++;
        }
    CHECK(((DWORD *)(pool + queues))[1] == count, "queue 0 holds %lu", ((DWORD *)(pool + queues))[1]);
    for (int i = 0; i < 4; i++) flush(pool);
    CHECK(pv[1] == count && *(DWORD *)(pool + frame) == 0, "after the flushes: %lu in the vector", pv[1]);
    recycle(pool);
    CHECK(pv[1] == 0 && *(DWORD *)(pool + freeCount) == count, "free list holds %lu", *(DWORD *)(pool + freeCount));
    for (DWORD i = 0; i < count; i++) {
        h = alloc(pool);
        CHECK(h < N && released[h] == 1, "alloc from the free list returned %lu", (unsigned long)h);
        if (h < N) released[h] = 2;
    }
    CHECK(*(DWORD *)(pool + freeCount) == 0 && alloc(pool) == 0, "free list not empty after taking them back");
    CHECK(addref(pool, 0x3001) == 2 && release(pool, 0x3001) == 1, "AddRef / Release past 0x3000");

    Get_t record = (Get_t)(exe + 0x4bc840), field = (Get_t)(exe + 0x4bc860);
    CHECK(record(N - 2) == (DWORD)(ULONG_PTR)(rec + (N - 2) * REC), "record getter");
    *(DWORD *)(rec + 0x3001 * REC + 0x394) = 0x12345678;
    CHECK(field(0x3001) == 0x12345678, "record field getter");

    BYTE *p = pool + end;
    while (p < trip && !*p) p++;
    CHECK(p == trip, "a byte past the grown pool changed (+0x%lx)", (unsigned long)(p - pool));

    ((Fn_t)(exe + 0x4c5430))(pool);
    printf("texhandles test: %lu handles, %lu released and taken back, %ld exe allocations: %s\n", (unsigned long)N,
           (unsigned long)count, g_allocs, g_fail ? "FAILED" : "ok");
    VirtualFree(rec, 0, MEM_RELEASE);
    FreeLibrary(m);
    return g_fail;
}

int main(void)
{
    int r = run(0x4000);
    if (r < 0) return 0;
    if (r) return 1;
    return run(MAX_N) ? 1 : 0;
}
