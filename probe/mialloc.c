// Opt-in: serve the game's CRT heap allocations from mimalloc ([probe] MiMalloc=1).
//
// Why: in long sessions the msvcr90 heap (an NT heap with LFH) keeps adding 16 MB
// segments while free space piles up inside it in holes too small to reuse (32-63 B
// holes went 91k -> 744k in 40 minutes), so free address space shrinks ~5-9 MB/min
// until the 4 GB process runs out. mimalloc keeps each size class on its own pages,
// reuses freed slots for the same size, and releases a 4 MB segment to Windows as
// soon as it is empty. Arenas (large reservations that are never released) are off
// by default; MiArenaMB > 0 turns them back on.
//
// How: the import slots of malloc, calloc, realloc, free, _aligned_malloc,
// _aligned_free and operator new/new[]/delete/delete[] that point into msvcr90 are
// redirected, in every loaded module and in modules loaded later. Blocks that came
// from the CRT heap (allocated before the patch, or inside msvcr90 itself, e.g. by
// _strdup or _malloc_crt) are recognised with mi_is_in_heap_region and handed back
// to msvcr90; realloc moves them into mimalloc. If mimalloc fails, the call falls
// back to msvcr90, which also keeps the CRT's new-handler and bad_alloc behaviour.
//
// Size split (MiMaxKB, default 16): only blocks up to that size go to mimalloc. The NT heap's
// fragmentation was all in small holes (8-63 B and 1-8 KB); its backend fits large blocks
// exactly and releases the >508 KB ones, while mimalloc rounds large blocks up to a size
// class and 32 KB slices (~335 MB over 1.7 GB live when it took every size). realloc moves
// a block across the split when its size crosses it. MiMaxKB=0 sends every size to mimalloc.
//
// Large blocks sent to the CRT heap get MiLargePadBytes (default 256) of slack at the end.
// The game overruns at least one of its own buffers: exe+0x1e0e63 writes a record 0x20+
// bytes past a 2 MB _aligned_malloc block (via exe+0x413b02), which faults when the NT
// heap's page-rounded big block happens to end right there (crash 2026-10-28 21:29). With
// mimalloc taking every size the size-class rounding hid it.
//
// Freed pages are never decommitted (MiPurgeMs=-1, the default). The CRT heap kept freed
// memory mapped and readable, and the game relies on that: a VP Lua binding (VP+0x21e40,
// the table's __instance pointer) read a field of an already deleted object and
// crashed on a page mimalloc had purged (2026-10-28, VP+0xd91a9). Purging only saves
// commit charge, not address space; empty 4 MB segments are still released.
//
// The CRT's own _malloc_crt/_calloc_crt/_realloc_crt are left alone so their blocks
// stay consistent. No game module imports _msize, _expand or _recalloc (checked
// 2026-10-28), so mimalloc's larger usable sizes are never observable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"
#include <mimalloc.h>
#include <mimalloc-stats.h>

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

typedef void *(__cdecl *malloc_t)(size_t);
typedef void *(__cdecl *calloc_t)(size_t, size_t);
typedef void *(__cdecl *realloc_t)(void *, size_t);
typedef void(__cdecl *free_t)(void *);
typedef size_t(__cdecl *msize_t)(void *);
typedef void *(__cdecl *amalloc_t)(size_t, size_t);

static malloc_t c_malloc, c_new, c_newa;
static calloc_t c_calloc;
static realloc_t c_realloc;
static free_t c_free, c_delete, c_deletea, c_afree;
static msize_t c_msize;
static amalloc_t c_amalloc;
static HMODULE g_crt, g_self;
static int g_on;
static size_t g_maxSmall = (size_t)-1;  // largest size served by mimalloc
static size_t g_pad;                    // slack added to CRT-heap blocks past the split
static volatile LONG g_nFallback, g_nCrtFree, g_nMoved, g_nSlots, g_nModules;

static int ours(const void *p) { return mi_is_in_heap_region(p); }
static int small(size_t n) { return n <= g_maxSmall; }
static size_t padded(size_t n) { return n <= (size_t)-1 - g_pad ? n + g_pad : n; }

// mimalloc keeps statistics per thread (MI_STAT=1 build); fold them into the process
// totals now and then so mialloc_report sees current live bytes.
static __thread unsigned t_allocs;
static void tick(void)
{
    if (!(++t_allocs & 0x3FFF)) mi_stats_merge();
}

static void *__cdecl m_malloc(size_t n)
{
    if (!small(n)) return c_malloc(padded(n));
    tick();
    void *p = mi_malloc(n);
    if (p) return p;
    InterlockedIncrement(&g_nFallback);
    return c_malloc(n);
}

static void *__cdecl m_calloc(size_t k, size_t n)
{
    if (k && n > (size_t)-1 / k) return c_calloc(k, n);  // overflow: let the CRT fail it
    if (!small(k * n)) return c_calloc(1, padded(k * n));
    void *p = mi_calloc(k, n);
    if (p) return p;
    InterlockedIncrement(&g_nFallback);
    return c_calloc(k, n);
}

static void __cdecl m_free(void *p)
{
    if (!p) return;
    if (ours(p)) { mi_free(p); return; }
    InterlockedIncrement(&g_nCrtFree);
    c_free(p);
}

// msvcr90 semantics: realloc(NULL, n) allocates, realloc(p, 0) frees and returns NULL,
// and a failure returns NULL leaving p intact.
static void *__cdecl m_realloc(void *p, size_t n)
{
    if (!p) return m_malloc(n);
    if (!n) { m_free(p); return NULL; }
    if (ours(p)) {
        void *q = small(n) ? mi_realloc(p, n) : NULL;
        if (q) return q;
        if (small(n)) InterlockedIncrement(&g_nFallback);
        q = c_malloc(padded(n));  // grew past the split (or mimalloc failed): move it to the CRT heap
        if (q) {
            size_t old = mi_usable_size(p);
            memcpy(q, p, old < n ? old : n);
            mi_free(p);
        }
        return q;
    }
    // A CRT heap block: move it into mimalloc if it is small now.
    if (!small(n)) return c_realloc(p, padded(n));
    void *q = mi_malloc(n);
    if (!q) { InterlockedIncrement(&g_nFallback); return c_realloc(p, n); }
    size_t old = c_msize(p);
    memcpy(q, p, old < n ? old : n);
    c_free(p);
    InterlockedIncrement(&g_nMoved);
    return q;
}

static void *__cdecl m_aligned_malloc(size_t n, size_t align)
{
    if (small(n) && align && !(align & (align - 1))) {  // msvcr90 rejects other alignments with EINVAL
        void *p = mi_malloc_aligned(n, align < sizeof(void *) ? sizeof(void *) : align);
        if (p) return p;
        InterlockedIncrement(&g_nFallback);
    }
    return c_amalloc(small(n) ? n : padded(n), align);
}

static void __cdecl m_aligned_free(void *p)
{
    if (!p) return;
    if (ours(p)) { mi_free(p); return; }
    InterlockedIncrement(&g_nCrtFree);
    c_afree(p);
}

// On failure the CRT's operator new runs the new handler and throws std::bad_alloc.
static void *__cdecl m_new(size_t n)
{
    if (!small(n)) return c_new(padded(n));
    tick();
    void *p = mi_malloc(n);
    if (p) return p;
    InterlockedIncrement(&g_nFallback);
    return c_new(n);
}

static void *__cdecl m_newa(size_t n)
{
    if (!small(n)) return c_newa(padded(n));
    tick();
    void *p = mi_malloc(n);
    if (p) return p;
    InterlockedIncrement(&g_nFallback);
    return c_newa(n);
}

static void __cdecl m_delete(void *p)
{
    if (!p) return;
    if (ours(p)) { mi_free(p); return; }
    InterlockedIncrement(&g_nCrtFree);
    c_delete(p);
}

static void __cdecl m_deletea(void *p)
{
    if (!p) return;
    if (ours(p)) { mi_free(p); return; }
    InterlockedIncrement(&g_nCrtFree);
    c_deletea(p);
}

typedef struct {
    const char *name;
    void **crt;  // the msvcr90 export
    void *mine;
} Redirect;

static Redirect g_redirects[] = {
    {"malloc", (void **)&c_malloc, (void *)m_malloc},
    {"calloc", (void **)&c_calloc, (void *)m_calloc},
    {"realloc", (void **)&c_realloc, (void *)m_realloc},
    {"free", (void **)&c_free, (void *)m_free},
    {"_aligned_malloc", (void **)&c_amalloc, (void *)m_aligned_malloc},
    {"_aligned_free", (void **)&c_afree, (void *)m_aligned_free},
    {"??2@YAPAXI@Z", (void **)&c_new, (void *)m_new},
    {"??_U@YAPAXI@Z", (void **)&c_newa, (void *)m_newa},
    {"??3@YAXPAX@Z", (void **)&c_delete, (void *)m_delete},
    {"??_V@YAXPAX@Z", (void **)&c_deletea, (void *)m_deletea},
};
#define N_REDIRECTS (int)(sizeof g_redirects / sizeof g_redirects[0])

// For the tests: does this block belong to mimalloc?
__declspec(dllexport) int CivProbe_IsMimalloc(const void *p) { return g_on && p && ours(p); }

void *mialloc_replacement(void *crtFn)
{
    if (!g_on || !crtFn) return crtFn;
    for (int i = 0; i < N_REDIRECTS; i++)
        if (*g_redirects[i].crt == crtFn) return g_redirects[i].mine;
    return crtFn;
}

// Redirects every import slot of the module that points at one of the CRT exports.
static int patch_module(BYTE *base)
{
    if ((HMODULE)base == g_crt || (HMODULE)base == g_self) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); dir.Size && d->Name; d++)
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk); t->u1.Function; t++)
            for (int i = 0; i < N_REDIRECTS; i++) {
                if (t->u1.Function != (DWORD)(ULONG_PTR)*g_redirects[i].crt) continue;
                DWORD old;
                if (!VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old)) break;
                InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)g_redirects[i].mine);
                VirtualProtect(&t->u1.Function, 4, old, &old);
                n++;
                break;
            }
    if (n) {
        WCHAR name[MAX_PATH];
        if (!GetModuleFileNameW((HMODULE)base, name, MAX_PATH)) name[0] = 0;
        const WCHAR *s = wcsrchr(name, 92);  // 92 = backslash
        log_event("mimalloc: redirected %d allocation imports of %ls", n, s ? s + 1 : name);
        InterlockedExchangeAdd(&g_nSlots, n);
        InterlockedIncrement(&g_nModules);
    }
    return n;
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } MiUstr;
typedef struct { ULONG Flags; const MiUstr *FullDllName; const MiUstr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } MiNotifyData;
typedef VOID(CALLBACK *MiNotify_t)(ULONG reason, const MiNotifyData *data, PVOID ctx);
typedef LONG(NTAPI *MiRegister_t)(ULONG, MiNotify_t, PVOID, PVOID *);

static VOID CALLBACK on_dll_notify(ULONG reason, const MiNotifyData *data, PVOID ctx)
{
    (void)ctx;
    if (reason == 1 /* loaded */) patch_module((BYTE *)data->DllBase);
}

static void on_mi_output(const char *msg, void *arg)
{
    (void)arg;
    char buf[512];
    int n = 0;
    for (; msg[n] && n < (int)sizeof buf - 1; n++) buf[n] = msg[n] == 10 || msg[n] == 13 ? ' ' : msg[n];
    buf[n] = 0;
    while (n && buf[n - 1] == ' ') buf[--n] = 0;
    if (n) log_event("mimalloc: %s", buf);
}

static void on_mi_error(int err, void *arg)
{
    (void)arg;
    log_event("mimalloc: error %d", err);
}

void mialloc_init(const WCHAR *ini)
{
    // mimalloc's options first, whatever happens below: the Lua heap (luaheap.c) uses mimalloc
    // even when the CRT redirect is off, and it needs freed pages kept mapped too.
    int purgeMs = (int)GetPrivateProfileIntW(L"probe", L"MiPurgeMs", (UINT)-1, ini);
    mi_option_set(mi_option_purge_delay, purgeMs);  // -1 = never decommit freed pages
    int arenaMb = GetPrivateProfileIntW(L"probe", L"MiArenaMB", 0, ini);
    if (arenaMb > 0) {
        mi_option_set(mi_option_disallow_arena_alloc, 0);
        mi_option_set(mi_option_arena_reserve, (long)arenaMb * 1024);  // KiB, per arena reservation
    } else {
        mi_option_set(mi_option_disallow_arena_alloc, 1);
    }
    mi_register_output(on_mi_output, NULL);
    mi_register_error(on_mi_error, NULL);
    if (!GetPrivateProfileIntW(L"probe", L"MiMalloc", 1, ini)) return;
    if (alloc_enabled()) { log_event("mimalloc: not used while AllocTracking=1"); return; }
    g_crt = GetModuleHandleW(L"msvcr90.dll");
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)mialloc_init, &g_self);
    if (!g_crt) { log_event("mimalloc: msvcr90.dll not loaded; off"); return; }
    for (int i = 0; i < N_REDIRECTS; i++) {
        *g_redirects[i].crt = (void *)GetProcAddress(g_crt, g_redirects[i].name);
        if (!*g_redirects[i].crt) { log_event("mimalloc: msvcr90.dll lacks %s; off", g_redirects[i].name); return; }
    }
    c_msize = (msize_t)GetProcAddress(g_crt, "_msize");
    if (!c_msize) { log_event("mimalloc: msvcr90.dll lacks _msize; off"); return; }

    int maxKb = (int)GetPrivateProfileIntW(L"probe", L"MiMaxKB", 16, ini);
    if (maxKb > 0) g_maxSmall = (size_t)(maxKb > 1048576 ? 1048576 : maxKb) << 10;  // at most 1 GB: no 32-bit wrap
    g_pad = GetPrivateProfileIntW(L"probe", L"MiLargePadBytes", 256, ini);
    void *probeBlock = mi_malloc(64);  // make sure mimalloc works before anything is redirected
    if (!probeBlock || !ours(probeBlock)) { log_event("mimalloc: self-test failed; off"); return; }
    mi_free(probeBlock);
    g_on = 1;

    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; i++) patch_module((BYTE *)mods[i]);
    }
    MiRegister_t reg = (MiRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (!reg || reg(0, on_dll_notify, NULL, &cookie) < 0) log_event("mimalloc: no DLL load notifications; later DLLs keep the CRT heap");
    char split[48];
    if (maxKb > 0) snprintf(split, sizeof split, "blocks up to %d KB (larger +%u B slack)", maxKb, (unsigned)g_pad);
    else snprintf(split, sizeof split, "blocks of every size");
    log_event("mimalloc: ON (v%d) for %s, %s, freed pages %s; %ld modules redirected so far", mi_version(), split,
              arenaMb > 0 ? "arenas on" : "no arenas (empty segments go back to Windows)",
              purgeMs < 0 ? "stay committed" : "decommitted after a delay", g_nModules);
}

void mialloc_report(void)
{
    if (!g_on) return;
    mi_stats_merge();
    mi_stats_t_decl(st);
    if (!mi_stats_get(&st)) return;
    double live = (double)(st.malloc_normal.current + st.malloc_huge.current);
    log_event("mimalloc: reserved %.1f MB (peak %.1f), committed %.1f MB (peak %.1f), live blocks ~%.1f MB "
              "(%.1f MB small, %.1f MB large); %lld segments (%lld abandoned), %lld pages (%lld abandoned, %.1f MB "
              "committed in pages), %lld threads",
              st.reserved.current / 1048576.0, st.reserved.peak / 1048576.0, st.committed.current / 1048576.0,
              st.committed.peak / 1048576.0, live / 1048576.0, st.malloc_normal.current / 1048576.0,
              st.malloc_huge.current / 1048576.0, (long long)st.segments.current,
              (long long)st.segments_abandoned.current, (long long)st.pages.current,
              (long long)st.pages_abandoned.current, st.page_committed.current / 1048576.0,
              (long long)st.threads.current);
    log_event("mimalloc: %ld modules, %ld slots; %ld CRT-heap frees passed on, %ld blocks moved by realloc, "
              "%ld fell back to the CRT heap", g_nModules, g_nSlots, g_nCrtFree, g_nMoved, g_nFallback);
}
