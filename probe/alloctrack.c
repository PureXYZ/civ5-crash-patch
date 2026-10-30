// Attribution of C-runtime heap allocations to their callers.
//
// Patches the import tables of every module that imports the target CRT
// (msvcr90.dll in Civ V: the exe, the VP game core DLL, CvGameDatabase, ...)
// so malloc/calloc/realloc/free and friends go through hooks here.
//
// Two tables come out of it:
//  - call sites: every allocation, keyed by its immediate return address
//    (calls and cumulative bytes, i.e. churn);
//  - stack sites: live bytes, keyed by the return address plus up to 4 more
//    found by scanning the stack (allocations usually go through wrappers such
//    as operator new, so the immediate caller alone says little).
// Live bytes are exact for blocks >= BigBytes and estimated from a 1-in-2^k
// address sample for smaller ones, which keeps the bookkeeping small enough
// (~22 MB, reserved top-down) for a process that is short on address space.
#include "probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);  // kernel32, Win7+

enum { K_MALLOC, K_CALLOC, K_REALLOC, K_RECALLOC, K_ALIGNED, K_ALIGNED_REALLOC, K_NEW, K_NEWA, K_COUNT };
static const char *const kKindName[K_COUNT] = {"malloc", "calloc", "realloc", "_recalloc",
                                              "_aligned_malloc", "_aligned_realloc", "new", "new[]"};

#define STACK_FRAMES 5
#define CALLSITE_BITS 15
#define STACKSITE_BITS 16
#define BUCKET_BITS 19
#define MAX_NODES (1u << 20)
#define LOCK_STRIPES 1024
#define MAX_MODULES 1024
#define MAX_SCAN_MODULES 32

typedef struct {
    volatile LONG state;  // 0 empty, 1 being written, 2 ready
    void *ret;
    LONG kind;
    volatile LONGLONG calls, bytes;
} CallSite;

typedef struct {
    volatile LONG state;
    LONG kind;
    void *frames[STACK_FRAMES];
    volatile LONG liveBig, liveBigCount;          // exact, blocks >= g_bigBytes
    volatile LONG liveSampled, liveSampledCount;  // sampled small blocks
    volatile LONG trackedAllocs;
} StackSite;

typedef struct {
    void *p;
    DWORD size;
    WORD site;
    WORD big;
    DWORD next;
} Node;

typedef struct {
    ULONG_PTR base, end, textStart, textEnd;
    char name[48];
    volatile LONG active;
} Module;

static int g_enabled;
static DWORD g_bigBytes = 4096, g_sampleShift = 6, g_reportSeconds = 30;

static CallSite *g_callSites;
static StackSite *g_stackSites;
static volatile DWORD *g_buckets;
static Node *g_nodes;
static volatile LONG g_nodeTop = 1;  // node 0 means "none"
static DWORD g_nodeFree;
static volatile LONG g_poolLock;
static volatile LONG g_stripes[LOCK_STRIPES];
static volatile LONG g_dropped, g_stackSitesFull, g_callSitesFull;

static Module g_modules[MAX_MODULES];
static volatile LONG g_moduleCount;
static Module *g_scan[MAX_SCAN_MODULES];  // modules whose code we accept as stack frames
static volatile LONG g_scanCount;
static HMODULE g_self, g_crt;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

// 0 only while the process exits and the lock stays taken: ExitProcess may have killed its owner,
// and the static destructors that free blocks at DLL detach must not wait forever. Tracking never
// changes a block, so the callers then just leave the table alone.
static BOOLEAN(NTAPI *p_shutdown)(void);  // ntdll!RtlDllShutdownInProgress
static int spin_lock(volatile LONG *l)
{
    for (int i = 0; InterlockedExchange(l, 1); i++) {
        if (i < 64) {
            YieldProcessor();
        } else {
            SwitchToThread();
            if (!(i & 1023) && p_shutdown && p_shutdown()) return 0;
        }
    }
    return 1;
}
static void spin_unlock(volatile LONG *l) { InterlockedExchange(l, 0); }

static DWORD hash32(DWORD x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static DWORD bucket_of(void *p) { return hash32((DWORD)(ULONG_PTR)p) >> (32 - BUCKET_BITS); }
static int sampled(void *p) { return (((DWORD)(ULONG_PTR)p >> 3) * 2654435761u) >> (32 - g_sampleShift) == 0; }

/* ------------------------------------------------------------------ */
/* Modules                                                             */
/* ------------------------------------------------------------------ */

static void add_module(BYTE *base, const WCHAR *name, int nameLen, int scan)
{
    LONG n = g_moduleCount;
    if (n >= MAX_MODULES) return;
    Module *m = &g_modules[n];
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    m->base = (ULONG_PTR)base;
    m->end = m->base + nt->OptionalHeader.SizeOfImage;
    m->textStart = m->textEnd = 0;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        ULONG_PTR s = m->base + sec->VirtualAddress, e = s + sec->Misc.VirtualSize;
        if (!m->textStart || s < m->textStart) m->textStart = s;
        if (e > m->textEnd) m->textEnd = e;
    }
    int start = 0;
    for (int i = 0; i < nameLen; i++)
        if (name[i] == 92 || name[i] == '/') start = i + 1;  // 92 = backslash
    int k = 0;
    for (int i = start; i < nameLen && k < (int)sizeof m->name - 1; i++)
        m->name[k++] = (name[i] < 128 && name[i] != ',') ? (char)name[i] : '_';
    m->name[k] = 0;
    m->active = 1;
    InterlockedExchange(&g_moduleCount, n + 1);
    if (scan && g_scanCount < MAX_SCAN_MODULES) {
        g_scan[g_scanCount] = m;
        InterlockedIncrement(&g_scanCount);
    }
}

static void deactivate_module(ULONG_PTR base)
{
    for (LONG i = 0; i < g_moduleCount; i++)
        if (g_modules[i].base == base && g_modules[i].active) g_modules[i].active = 0;
}

// Most recent module (loaded or since unloaded) covering the address.
static const Module *module_of(ULONG_PTR a)
{
    for (LONG i = g_moduleCount - 1; i >= 0; i--)
        if (a >= g_modules[i].base && a < g_modules[i].end) return &g_modules[i];
    return NULL;
}

static void format_addr(void *a, char *out, int cap)
{
    if (!a) { out[0] = 0; return; }
    const Module *m = module_of((ULONG_PTR)a);
    if (m) snprintf(out, cap, "%s+0x%lx", m->name, (unsigned long)((ULONG_PTR)a - m->base));
    else snprintf(out, cap, "0x%08lx", (unsigned long)(ULONG_PTR)a);
}

/* ------------------------------------------------------------------ */
/* Stack scanning                                                      */
/* ------------------------------------------------------------------ */

static int in_scanned_code(ULONG_PTR a)
{
    LONG n = g_scanCount;
    for (LONG i = 0; i < n; i++) {
        Module *m = g_scan[i];
        if (m->active && a >= m->textStart + 8 && a < m->textEnd) return 1;
    }
    return 0;
}

// True if the bytes before `ra` look like a call instruction.
static int follows_call(const BYTE *ra)
{
    if (ra[-5] == 0xE8) return 1;                                                        // call rel32
    if (ra[-6] == 0xFF && (ra[-5] & 0x38) == 0x10) return 1;                             // call [disp32] / [reg+disp32]
    if (ra[-2] == 0xFF && (ra[-1] & 0xF8) == 0xD0) return 1;                             // call reg
    if (ra[-3] == 0xFF && (ra[-2] & 0x38) == 0x10 && (ra[-2] & 0xC0) == 0x40) return 1;  // call [reg+disp8]
    if (ra[-2] == 0xFF && (ra[-1] & 0x38) == 0x10 && (ra[-1] & 0xC0) == 0x00) return 1;  // call [reg]
    return 0;
}

// Fills frames[1..] with return addresses found above `from` on this thread's
// stack. Release builds omit frame pointers, so this scans instead of walking.
static void scan_stack(void **frames, const BYTE *from)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    const DWORD *p = (const DWORD *)(((ULONG_PTR)from + 3) & ~(ULONG_PTR)3);
    const DWORD *top = (const DWORD *)tib->StackBase;
    const DWORD *limit = p + 256;
    if (limit > top) limit = top;
    int n = 1;
    for (; p < limit && n < STACK_FRAMES; p++) {
        ULONG_PTR v = *p;
        if (v != (ULONG_PTR)frames[n - 1] && in_scanned_code(v) && follows_call((const BYTE *)v))
            frames[n++] = (void *)v;
    }
}

/* ------------------------------------------------------------------ */
/* Site tables (lock-free insert, entries never removed)               */
/* ------------------------------------------------------------------ */

static CallSite *callsite_get(void *ret, int kind)
{
    DWORD mask = (1u << CALLSITE_BITS) - 1;
    DWORD h = hash32((DWORD)(ULONG_PTR)ret ^ (kind * 0x9e3779b9u)) & mask;
    for (DWORD i = 0; i < 64; i++) {
        CallSite *c = &g_callSites[(h + i) & mask];
        if (c->state == 0 && InterlockedCompareExchange(&c->state, 1, 0) == 0) {
            c->ret = ret;
            c->kind = kind;
            InterlockedExchange(&c->state, 2);
            return c;
        }
        while (c->state == 1) YieldProcessor();
        if (c->ret == ret && c->kind == kind) return c;
    }
    InterlockedIncrement(&g_callSitesFull);
    return NULL;
}

static int stacksite_get(void **frames, int kind)
{
    DWORD mask = (1u << STACKSITE_BITS) - 1;
    DWORD h = kind * 0x9e3779b9u;
    for (int i = 0; i < STACK_FRAMES; i++) h = hash32(h ^ (DWORD)(ULONG_PTR)frames[i]);
    for (DWORD i = 0; i < 128; i++) {
        DWORD idx = (h + i) & mask;
        if (idx == mask) continue;  // 0xFFFF marks "no site" in Node.site
        StackSite *s = &g_stackSites[idx];
        if (s->state == 0 && InterlockedCompareExchange(&s->state, 1, 0) == 0) {
            memcpy(s->frames, frames, sizeof s->frames);
            s->kind = kind;
            InterlockedExchange(&s->state, 2);
            return (int)idx;
        }
        while (s->state == 1) YieldProcessor();
        if (s->kind == kind && memcmp(s->frames, frames, sizeof s->frames) == 0) return (int)idx;
    }
    InterlockedIncrement(&g_stackSitesFull);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Live-block table                                                    */
/* ------------------------------------------------------------------ */

static DWORD node_alloc(void)
{
    if (!spin_lock(&g_poolLock)) return 0;
    DWORD n = g_nodeFree;
    if (n) g_nodeFree = g_nodes[n].next;
    else if ((DWORD)g_nodeTop < MAX_NODES) n = (DWORD)g_nodeTop++;
    spin_unlock(&g_poolLock);
    return n;
}

static void node_release(DWORD n)
{
    if (!spin_lock(&g_poolLock)) return;  // exiting: the node is lost
    g_nodes[n].next = g_nodeFree;
    g_nodeFree = n;
    spin_unlock(&g_poolLock);
}

static int node_site(const Node *n) { return n->site == 0xFFFF ? -1 : n->site; }

// Totals over all sites, for alloc_est_live_mb (luagc.c's trigger).
static volatile LONGLONG g_totBig;  // can pass 2 GB in a large-address-aware process
static volatile LONG g_totSampled;

static void site_add(int site, DWORD size, int big, LONG sign)
{
    if (site < 0) return;
    StackSite *s = &g_stackSites[site];
    if (big) InterlockedExchangeAdd64(&g_totBig, sign * (LONGLONG)size);
    else InterlockedExchangeAdd(&g_totSampled, sign * (LONG)size);
    if (big) {
        InterlockedExchangeAdd(&s->liveBig, sign * (LONG)size);
        InterlockedExchangeAdd(&s->liveBigCount, sign);
    } else {
        InterlockedExchangeAdd(&s->liveSampled, sign * (LONG)size);
        InterlockedExchangeAdd(&s->liveSampledCount, sign);
    }
}

static void table_insert(void *p, DWORD size, int site, int big)
{
    DWORD n = node_alloc();
    if (!n) { InterlockedIncrement(&g_dropped); return; }
    DWORD b = bucket_of(p);
    volatile LONG *lock = &g_stripes[b % LOCK_STRIPES];
    Node stale = {0};
    int hadStale = 0;
    if (!spin_lock(lock)) return;  // exiting
    // An entry for the same address means that block was freed behind our back.
    for (DWORD i = g_buckets[b], prev = 0; i; prev = i, i = g_nodes[i].next) {
        if (g_nodes[i].p != p) continue;
        stale = g_nodes[i];
        hadStale = 1;
        if (prev) g_nodes[prev].next = g_nodes[i].next;
        else g_buckets[b] = g_nodes[i].next;
        node_release(i);
        break;
    }
    g_nodes[n].p = p;
    g_nodes[n].size = size;
    g_nodes[n].site = site < 0 ? 0xFFFF : (WORD)site;
    g_nodes[n].big = (WORD)big;
    g_nodes[n].next = g_buckets[b];
    g_buckets[b] = n;
    spin_unlock(lock);
    if (hadStale) site_add(node_site(&stale), stale.size, stale.big, -1);
    site_add(site, size, big, +1);
}

// Removes `p` if tracked; returns 1 and its record if it was.
static int table_take(void *p, Node *out)
{
    DWORD b = bucket_of(p);
    if (!g_buckets[b]) return 0;
    volatile LONG *lock = &g_stripes[b % LOCK_STRIPES];
    DWORD found = 0;
    if (!spin_lock(lock)) return 0;  // exiting
    for (DWORD i = g_buckets[b], prev = 0; i; prev = i, i = g_nodes[i].next) {
        if (g_nodes[i].p != p) continue;
        if (prev) g_nodes[prev].next = g_nodes[i].next;
        else g_buckets[b] = g_nodes[i].next;
        found = i;
        *out = g_nodes[i];
        break;
    }
    spin_unlock(lock);
    if (!found) return 0;
    node_release(found);
    site_add(node_site(out), out->size, out->big, -1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

__attribute__((noinline)) static void on_alloc(void *p, size_t size, int kind, void *ret, const BYTE *frame)
{
    CallSite *c = callsite_get(ret, kind);
    if (c) {
        InterlockedIncrement64(&c->calls);
        InterlockedExchangeAdd64(&c->bytes, (LONGLONG)size);
    }
    int big = size >= g_bigBytes;
    if (!big && !sampled(p)) return;
    void *frames[STACK_FRAMES] = {ret};
    scan_stack(frames, frame + 8);  // skip saved ebp and return address
    int site = stacksite_get(frames, kind);
    if (site >= 0) InterlockedIncrement(&g_stackSites[site].trackedAllocs);
    table_insert(p, (DWORD)size, site, big);
}

static void on_free(void *p)
{
    Node n;
    table_take(p, &n);
}

// Re-tracks a block whose realloc failed (the original stays valid).
static void restore(void *p, int had, const Node *old)
{
    if (had) table_insert(p, old->size, node_site(old), old->big);
}

#define FRAME() ((const BYTE *)__builtin_frame_address(0))
#define RET() __builtin_return_address(0)

static void *(__cdecl *r_malloc)(size_t);
static void *(__cdecl *r_calloc)(size_t, size_t);
static void *(__cdecl *r_realloc)(void *, size_t);
static void *(__cdecl *r_recalloc)(void *, size_t, size_t);
static void(__cdecl *r_free)(void *);
static void *(__cdecl *r_aligned_malloc)(size_t, size_t);
static void *(__cdecl *r_aligned_realloc)(void *, size_t, size_t);
static void(__cdecl *r_aligned_free)(void *);
static void *(__cdecl *r_new)(size_t);
static void *(__cdecl *r_newa)(size_t);
static void(__cdecl *r_delete)(void *);
static void(__cdecl *r_deletea)(void *);

static void *__cdecl h_malloc(size_t n)
{
    void *p = r_malloc(n);
    if (p) on_alloc(p, n, K_MALLOC, RET(), FRAME());
    return p;
}

static void *__cdecl h_calloc(size_t n, size_t m)
{
    void *p = r_calloc(n, m);
    if (p) on_alloc(p, n * m, K_CALLOC, RET(), FRAME());
    return p;
}

// Untrack before the real call: once the block is released, another thread can
// get the same address back and track it.
static void *__cdecl h_realloc(void *p, size_t n)
{
    Node old;
    int had = p ? table_take(p, &old) : 0;
    void *q = r_realloc(p, n);
    if (q && n) on_alloc(q, n, K_REALLOC, RET(), FRAME());
    else if (!q && n) restore(p, had, &old);
    return q;
}

static void *__cdecl h_recalloc(void *p, size_t n, size_t m)
{
    Node old;
    int had = p ? table_take(p, &old) : 0;
    void *q = r_recalloc(p, n, m);
    if (q && n * m) on_alloc(q, n * m, K_RECALLOC, RET(), FRAME());
    else if (!q && n * m) restore(p, had, &old);
    return q;
}

static void __cdecl h_free(void *p)
{
    if (p) on_free(p);
    r_free(p);
}

static void *__cdecl h_aligned_malloc(size_t n, size_t a)
{
    void *p = r_aligned_malloc(n, a);
    if (p) on_alloc(p, n, K_ALIGNED, RET(), FRAME());
    return p;
}

static void *__cdecl h_aligned_realloc(void *p, size_t n, size_t a)
{
    Node old;
    int had = p ? table_take(p, &old) : 0;
    void *q = r_aligned_realloc(p, n, a);
    if (q && n) on_alloc(q, n, K_ALIGNED_REALLOC, RET(), FRAME());
    else if (!q && n) restore(p, had, &old);
    return q;
}

static void __cdecl h_aligned_free(void *p)
{
    if (p) on_free(p);
    r_aligned_free(p);
}

static void *__cdecl h_new(size_t n)
{
    void *p = r_new(n);
    if (p) on_alloc(p, n, K_NEW, RET(), FRAME());
    return p;
}

static void *__cdecl h_newa(size_t n)
{
    void *p = r_newa(n);
    if (p) on_alloc(p, n, K_NEWA, RET(), FRAME());
    return p;
}

static void __cdecl h_delete(void *p)
{
    if (p) on_free(p);
    r_delete(p);
}

static void __cdecl h_deletea(void *p)
{
    if (p) on_free(p);
    r_deletea(p);
}

typedef struct { const char *name; void **real; void *hook; } Hook;
static Hook g_hooks[] = {
    {"malloc", (void **)&r_malloc, (void *)h_malloc},
    {"calloc", (void **)&r_calloc, (void *)h_calloc},
    {"realloc", (void **)&r_realloc, (void *)h_realloc},
    {"_recalloc", (void **)&r_recalloc, (void *)h_recalloc},
    {"free", (void **)&r_free, (void *)h_free},
    {"_aligned_malloc", (void **)&r_aligned_malloc, (void *)h_aligned_malloc},
    {"_aligned_realloc", (void **)&r_aligned_realloc, (void *)h_aligned_realloc},
    {"_aligned_free", (void **)&r_aligned_free, (void *)h_aligned_free},
    {"??2@YAPAXI@Z", (void **)&r_new, (void *)h_new},
    {"??_U@YAPAXI@Z", (void **)&r_newa, (void *)h_newa},
    {"??3@YAXPAX@Z", (void **)&r_delete, (void *)h_delete},
    {"??_V@YAXPAX@Z", (void **)&r_deletea, (void *)h_deletea},
};
#define HOOK_COUNT (int)(sizeof g_hooks / sizeof g_hooks[0])

/* ------------------------------------------------------------------ */
/* Import-table patching                                               */
/* ------------------------------------------------------------------ */

// Replaces every IAT slot that holds a real allocator address. Matching on the
// address (not the import name) also catches API-set imports.
static int patch_module(BYTE *base)
{
    if ((HMODULE)base == g_self || (HMODULE)base == g_crt) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk); t->u1.Function; t++) {
            for (int k = 0; k < HOOK_COUNT; k++) {
                if (!*g_hooks[k].real || t->u1.Function != (DWORD)(ULONG_PTR)*g_hooks[k].real) continue;
                DWORD old;
                if (VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old)) {
                    InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)g_hooks[k].hook);
                    VirtualProtect(&t->u1.Function, 4, old, &old);
                    patched++;
                }
                break;
            }
        }
    }
    return patched;
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
typedef struct { ULONG Flags; const USTR *FullDllName; const USTR *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DllNotifyData;
typedef VOID(CALLBACK *DllNotify_t)(ULONG reason, const DllNotifyData *data, PVOID ctx);
typedef LONG(NTAPI *LdrRegisterDllNotification_t)(ULONG, DllNotify_t, PVOID, PVOID *);

static void track_module(BYTE *base, const WCHAR *name, int nameLen)
{
    int n = patch_module(base);
    int isExe = (HMODULE)base == GetModuleHandleW(NULL);
    add_module(base, name, nameLen, n > 0 || isExe);
    if (n) log_event("alloc hooks: patched %d imports in %.*ls", n, nameLen, name);
}

static VOID CALLBACK on_dll_notify(ULONG reason, const DllNotifyData *data, PVOID ctx)
{
    (void)ctx;
    if (reason == 1) track_module((BYTE *)data->DllBase, data->FullDllName->Buffer, data->FullDllName->Length / 2);
    else if (reason == 2) deactivate_module((ULONG_PTR)data->DllBase);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

int alloc_enabled(void) { return g_enabled; }
HMODULE alloc_crt_module(void) { return g_crt; }
LONG alloc_est_live_mb(void)
{
    LONGLONG big = InterlockedCompareExchange64(&g_totBig, 0, 0);  // one 64-bit read
    return (LONG)((big + ((LONGLONG)g_totSampled << g_sampleShift)) >> 20);
}
DWORD alloc_report_seconds(void) { return g_reportSeconds; }

void alloc_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"AllocTracking", 0, ini)) return;
    p_shutdown = (BOOLEAN(NTAPI *)(void))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress");
    WCHAR crtName[64];
    GetPrivateProfileStringW(L"probe", L"AllocCrt", L"msvcr90.dll", crtName, 64, ini);
    g_bigBytes = GetPrivateProfileIntW(L"probe", L"AllocBigBytes", 4096, ini);
    g_sampleShift = GetPrivateProfileIntW(L"probe", L"AllocSampleShift", 6, ini);
    if (g_sampleShift < 1 || g_sampleShift > 12) g_sampleShift = 6;
    g_reportSeconds = GetPrivateProfileIntW(L"probe", L"AllocReportSeconds", 30, ini);

    g_crt = GetModuleHandleW(crtName);
    if (!g_crt) { log_event("alloc tracking off: %ls not loaded", crtName); return; }
    int found = 0;
    for (int k = 0; k < HOOK_COUNT; k++) {
        *g_hooks[k].real = (void *)GetProcAddress(g_crt, g_hooks[k].name);
        found += *g_hooks[k].real != NULL;
    }
    if (!r_malloc || !r_free) { log_event("alloc tracking off: malloc/free not found in %ls", crtName); return; }

    SIZE_T callBytes = sizeof(CallSite) << CALLSITE_BITS;
    SIZE_T stackBytes = sizeof(StackSite) << STACKSITE_BITS;
    SIZE_T bucketBytes = sizeof(DWORD) << BUCKET_BITS;
    SIZE_T nodeBytes = sizeof(Node) * (SIZE_T)MAX_NODES;
    SIZE_T totalBytes = callBytes + stackBytes + bucketBytes + nodeBytes;
    BYTE *mem = (BYTE *)VirtualAlloc(NULL, totalBytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!mem) { log_event("alloc tracking off: no memory for tables"); return; }
    g_callSites = (CallSite *)mem;
    g_stackSites = (StackSite *)(mem + callBytes);
    g_buckets = (volatile DWORD *)(mem + callBytes + stackBytes);
    g_nodes = (Node *)(mem + callBytes + stackBytes + bucketBytes);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)alloc_init, &g_self);
    log_event("alloc tracking: %ls, %d/%d functions, blocks >= %lu B exact, smaller sampled 1/%u, "
              "tables %lu MB at %p",
              crtName, found, HOOK_COUNT, g_bigBytes, 1u << g_sampleShift,
              (unsigned long)(totalBytes >> 20), (void *)mem);
    g_enabled = 1;

    // Modules already loaded now; the rest as they load (the VP DLL is loaded and
    // reloaded when mods are activated).
    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; i++) {
            WCHAR name[MAX_PATH];
            int len = (int)GetModuleFileNameW(mods[i], name, MAX_PATH);
            track_module((BYTE *)mods[i], name, len);
        }
    }
    LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (!reg || reg(0, on_dll_notify, NULL, &cookie) != 0) log_event("alloc tracking: no DLL load notifications");
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static LONGLONG est_live(const StackSite *s)
{
    return (LONGLONG)s->liveBig + ((LONGLONG)s->liveSampled << g_sampleShift);
}

static DWORD g_order[1u << STACKSITE_BITS];

static int cmp_stack(const void *a, const void *b)
{
    LONGLONG x = est_live(&g_stackSites[*(const DWORD *)a]), y = est_live(&g_stackSites[*(const DWORD *)b]);
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_call(const void *a, const void *b)
{
    LONGLONG x = g_callSites[*(const DWORD *)a].bytes, y = g_callSites[*(const DWORD *)b].bytes;
    return x < y ? 1 : x > y ? -1 : 0;
}

static char g_repBuf[64 * 1024];

static void flush_if_full(HANDLE h, int *n)
{
    if (*n > (int)sizeof g_repBuf - 2048) {
        write_all(h, g_repBuf, *n);
        *n = 0;
    }
}

// Writes allocsites_<session>_<label>.csv (live bytes by stack) and
// alloccalls_<session>_<label>.csv (cumulative bytes by immediate call site).
// Takes no locks and allocates nothing, but opens its files (CreateFileW allocates), so it must
// not run on the exception path.
static void write_reports(const char *label)
{
    WCHAR name[128];
    char a[STACK_FRAMES][80];

    DWORD n = 0;
    LONGLONG total = 0, totalBig = 0;
    for (DWORD i = 0; i < (1u << STACKSITE_BITS); i++) {
        StackSite *s = &g_stackSites[i];
        if (s->state != 2) continue;
        LONGLONG e = est_live(s);
        if (e <= 0) continue;
        total += e;
        totalBig += s->liveBig;
        g_order[n++] = i;
    }
    qsort(g_order, n, sizeof g_order[0], cmp_stack);
    _snwprintf(name, 128, L"allocsites_%s_%S.csv", g_session, label);
    name[127] = 0;
    HANDLE h = open_out(name);
    if (h != INVALID_HANDLE_VALUE) {
        int k = snprintf(g_repBuf, sizeof g_repBuf,
                         "est_live_mb,big_mb,small_est_mb,est_blocks,tracked_allocs,kind,"
                         "frame0,frame1,frame2,frame3,frame4\r\n");
        for (DWORD j = 0; j < n && j < 3000; j++) {
            StackSite *s = &g_stackSites[g_order[j]];
            for (int f = 0; f < STACK_FRAMES; f++) format_addr(s->frames[f], a[f], sizeof a[f]);
            k += snprintf(g_repBuf + k, sizeof g_repBuf - k, "%.3f,%.3f,%.3f,%ld,%ld,%s,%s,%s,%s,%s,%s\r\n",
                          est_live(s) / 1048576.0, s->liveBig / 1048576.0,
                          ((LONGLONG)s->liveSampled << g_sampleShift) / 1048576.0,
                          s->liveBigCount + (s->liveSampledCount << g_sampleShift), s->trackedAllocs,
                          kKindName[s->kind], a[0], a[1], a[2], a[3], a[4]);
            flush_if_full(h, &k);
        }
        write_all(h, g_repBuf, k);
        CloseHandle(h);
    }

    DWORD m = 0;
    for (DWORD i = 0; i < (1u << CALLSITE_BITS); i++)
        if (g_callSites[i].state == 2) g_order[m++] = i;
    qsort(g_order, m, sizeof g_order[0], cmp_call);
    _snwprintf(name, 128, L"alloccalls_%s_%S.csv", g_session, label);
    name[127] = 0;
    h = open_out(name);
    if (h != INVALID_HANDLE_VALUE) {
        int k = snprintf(g_repBuf, sizeof g_repBuf, "total_mb,calls,kind,site\r\n");
        for (DWORD j = 0; j < m && j < 3000; j++) {
            CallSite *c = &g_callSites[g_order[j]];
            format_addr(c->ret, a[0], sizeof a[0]);
            k += snprintf(g_repBuf + k, sizeof g_repBuf - k, "%.3f,%lld,%s,%s\r\n",
                          c->bytes / 1048576.0, c->calls, kKindName[c->kind], a[0]);
            flush_if_full(h, &k);
        }
        write_all(h, g_repBuf, k);
        CloseHandle(h);
    }
    log_event("alloc report %s: est live %.1f MB (%.1f MB in exact big blocks), %lu stack sites, "
              "%lu call sites, nodes %ld/%u, dropped %ld, table-full %ld/%ld",
              label, total / 1048576.0, totalBig / 1048576.0, n, m, g_nodeTop, MAX_NODES, g_dropped,
              g_stackSitesFull, g_callSitesFull);
}

void alloc_report(const char *label)
{
    static volatile LONG busy;
    if (!g_enabled || InterlockedExchange(&busy, 1)) return;  // the buffers are shared
    write_reports(label);
    InterlockedExchange(&busy, 0);
}
