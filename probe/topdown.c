// Hooks on the WOW64 ntdll virtual-memory stubs, for two opt-in modes.
//
// Top-down mode ([probe] TopDown=1 in civprobe.ini): every new reservation and
// mapped view in the process asks for the highest free address, like the
// system-wide AllocationPreference=0x100000 registry value but for the game only.
// Almost everything then lands above 2 GB from the first minute, so code that
// mishandles pointers >= 0x80000000 (signed compares, int round-trips, sentinel
// checks) fails early and repeatably instead of hours in. Image sections (DLL
// loads) are left alone.
//
// Reservation log ([probe] ReserveLog=1): every new private region of at least
// ReserveLogMinKB (default 1024) is written to reserves_<session>.csv with the
// caller's stack, and so is its release. That attributes address-space growth the
// heap columns can't: new heap segments (the stack shows which malloc caller grew
// the heap) and direct VirtualAlloc users. Reservations at an address the caller
// chose are logged too, with PLACED (0x40000000) added to the type, in case that is
// why the unattributed 32 MB regions above 2 GB never showed up (2026-10-30).
// The hooks can run with a heap lock held,
// so they take no locks, allocate nothing and look up no modules: they copy raw
// addresses into a ring buffer that the sampler thread drains (topdown_drain),
// and they never dereference code addresses directly (see is_return_address).
//
// How: the stubs of NtAllocateVirtualMemory(Ex), NtFreeVirtualMemory and
// NtMapViewOfSection are `mov eax, <syscall#>; mov edx, <abs>; call edx; ret N`
// (16 bytes, position independent). The stub is copied to a trampoline, then its
// first instruction (5 bytes) is replaced by a `jmp` to our hook with one 8-byte
// atomic write.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define STUB_LEN 16
#define SEC_IMAGE_ATTR 0x1000000
#define CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)

typedef LONG(NTAPI *NtAlloc_t)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef LONG(NTAPI *NtAllocEx_t)(HANDLE, PVOID *, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
typedef LONG(NTAPI *NtFree_t)(HANDLE, PVOID *, PSIZE_T, ULONG);
typedef LONG(NTAPI *NtMap_t)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD,
                             ULONG, ULONG);
typedef LONG(NTAPI *NtQuerySection_t)(HANDLE, DWORD, PVOID, SIZE_T, PSIZE_T);

typedef struct {
    PVOID BaseAddress;
    ULONG AllocationAttributes;
    LARGE_INTEGER MaximumSize;
} SectionBasicInfo;

static NtAlloc_t real_alloc;
static NtAllocEx_t real_allocEx;
static NtFree_t real_free;
static NtMap_t real_map;
static NtQuerySection_t pNtQuerySection;
static volatile LONG g_nAlloc, g_nAllocEx, g_nView, g_nRetry;
static int g_on;      // top-down placement
static int g_resLog;  // reservation log

/* ------------------------------------------------------------------ */
/* Reservation log: capture side (runs inside the hooks)               */
/* ------------------------------------------------------------------ */

#define RES_FRAMES 16
#define RES_RING 8192  // power of two
#define LIVE_SLOTS 16384
#define LIVE_PROBES 64
#define TOMB ((ULONG_PTR)1)

typedef struct {
    volatile DWORD seq;  // index + 1 once the record is complete, 0 when free
    DWORD tMs;
    ULONG_PTR base;
    DWORD size, type;  // type 0 = release
    void *frames[RES_FRAMES];
} ResRec;

static ResRec *g_ring;
static volatile LONG g_ringHead;    // next index to write
static LONG g_ringTail;             // next index to drain (sampler thread only)
static LONG g_stallIdx = -1;        // an index that was not ready at the previous drain
static volatile ULONG_PTR *g_live;  // bases of logged regions not yet released
static volatile LONG g_nRes, g_nRel, g_nDropped, g_nLiveFull;
static DWORD g_resMin = 1u << 20;
static ULONGLONG g_resT0;
static HANDLE g_resOut = INVALID_HANDLE_VALUE;

// True if `ra` is in executable image code and the bytes before it look like a
// call instruction. Nothing is cached: the game loads and unloads DLLs (e.g. GPU
// driver probing at startup), so a remembered code range can go away. The bytes
// are read with ReadProcessMemory, which fails instead of faulting if the module
// is unloaded between the query and the read.
static int is_return_address(ULONG_PTR ra)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)ra, &mbi, sizeof mbi)) return 0;
    DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE || !(mbi.Protect & x) || (mbi.Protect & PAGE_GUARD))
        return 0;
    if (ra - 6 < (ULONG_PTR)mbi.BaseAddress) return 0;
    BYTE b[6];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ra - 6), b, 6, &got) || got != 6) return 0;
    const BYTE *e = b + 6;                                                           // e[-k] = byte at ra - k
    if (e[-5] == 0xE8) return 1;                                                     // call rel32
    if (e[-6] == 0xFF && (e[-5] & 0x38) == 0x10) return 1;                           // call [disp32] / [reg+disp32]
    if (e[-2] == 0xFF && (e[-1] & 0xF8) == 0xD0) return 1;                           // call reg
    if (e[-3] == 0xFF && (e[-2] & 0x38) == 0x10 && (e[-2] & 0xC0) == 0x40) return 1; // call [reg+disp8]
    if (e[-2] == 0xFF && (e[-1] & 0x38) == 0x10 && (e[-1] & 0xC0) == 0x00) return 1; // call [reg]
    return 0;
}

// frames[0] = the hook's return address; the rest are return addresses found by
// scanning the stack above the hook's arguments (release builds omit frame pointers).
static void capture(void **frames, void *ret, const DWORD *from)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    ULONG_PTR lo = (ULONG_PTR)tib->StackLimit, hi = (ULONG_PTR)tib->StackBase;
    const DWORD *limit = from + 512;
    if ((ULONG_PTR)limit > hi) limit = (const DWORD *)hi;
    int n = 0;
    frames[n++] = ret;
    for (const DWORD *p = from; p < limit && n < RES_FRAMES; p++) {
        ULONG_PTR v = *p;
        if (v < 0x10000 || (v >= lo && v < hi) || v == (ULONG_PTR)frames[n - 1]) continue;
        if (is_return_address(v)) frames[n++] = (void *)v;
    }
    while (n < RES_FRAMES) frames[n++] = NULL;
}

static DWORD hash_base(ULONG_PTR b) { return (DWORD)((b >> 16) * 2654435761u) >> (32 - 14); }

static int live_insert(ULONG_PTR b)
{
    DWORD i = hash_base(b);
    for (int k = 0; k < LIVE_PROBES; k++, i = (i + 1) & (LIVE_SLOTS - 1)) {
        ULONG_PTR cur = g_live[i];
        if ((cur == 0 || cur == TOMB) &&
            (ULONG_PTR)InterlockedCompareExchange((volatile LONG *)&g_live[i], (LONG)b, (LONG)cur) == cur)
            return 1;
    }
    InterlockedIncrement(&g_nLiveFull);
    return 0;
}

static int live_remove(ULONG_PTR b)
{
    DWORD i = hash_base(b);
    for (int k = 0; k < LIVE_PROBES; k++, i = (i + 1) & (LIVE_SLOTS - 1)) {
        ULONG_PTR cur = g_live[i];
        if (cur == 0) return 0;
        if (cur == b) return InterlockedCompareExchange((volatile LONG *)&g_live[i], (LONG)TOMB, (LONG)b) == (LONG)b;
    }
    return 0;
}

// Two writers could only meet in one slot if the drain fell a full ring behind;
// a slot that is still occupied drops the new record instead.
static void res_record(ULONG_PTR base, DWORD size, DWORD type, void *ret, const DWORD *from)
{
    LONG h = InterlockedIncrement(&g_ringHead) - 1;
    ResRec *r = &g_ring[h & (RES_RING - 1)];
    if (r->seq) { InterlockedIncrement(&g_nDropped); return; }
    r->tMs = (DWORD)(GetTickCount64() - g_resT0);
    r->base = base;
    r->size = size;
    r->type = type;
    capture(r->frames, ret, from);
    MemoryBarrier();
    r->seq = (DWORD)h + 1;
}

static void note_reserve(PVOID base, SIZE_T size, ULONG type, void *ret, const DWORD *from)
{
    if (size < g_resMin) return;
    InterlockedIncrement(&g_nRes);
    live_insert((ULONG_PTR)base);
    res_record((ULONG_PTR)base, (DWORD)size, type, ret, from);
}
/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

// The hook's arguments start two slots above its frame (saved ebp, return address).
#define HOOK_FROM ((const DWORD *)__builtin_frame_address(0) + 2)

// A new region with no address requirements.
static int fresh_region(HANDLE proc, PVOID *base, ULONG type)
{
    return proc == CURRENT_PROCESS && base && !*base && (type & (MEM_RESERVE | MEM_COMMIT));
}

// A new region at an address the caller chose (a commit without MEM_RESERVE is inside an
// existing one).
#define PLACED 0x40000000u  // not a MEM_* flag
static int placed_region(HANDLE proc, PVOID *base, ULONG type)
{
    return proc == CURRENT_PROCESS && base && *base && (type & MEM_RESERVE);
}

static LONG NTAPI hook_alloc(HANDLE proc, PVOID *base, ULONG_PTR zeroBits, PSIZE_T size, ULONG type, ULONG prot)
{
    int fresh = fresh_region(proc, base, type), placed = g_resLog && placed_region(proc, base, type);
    LONG st;
    if (g_on && fresh && !zeroBits && !(type & (MEM_TOP_DOWN | MEM_PHYSICAL))) {
        st = real_alloc(proc, base, zeroBits, size, type | MEM_TOP_DOWN, prot);
        if (st >= 0) {
            InterlockedIncrement(&g_nAlloc);
        } else {
            InterlockedIncrement(&g_nRetry);
            *base = NULL;
            st = real_alloc(proc, base, zeroBits, size, type, prot);
        }
    } else {
        st = real_alloc(proc, base, zeroBits, size, type, prot);
    }
    if (g_resLog && (fresh || placed) && st >= 0)
        note_reserve(*base, *size, placed ? type | PLACED : type, __builtin_return_address(0), HOOK_FROM);
    return st;
}

static LONG NTAPI hook_allocEx(HANDLE proc, PVOID *base, PSIZE_T size, ULONG type, ULONG prot, PVOID params,
                               ULONG count)
{
    int fresh = fresh_region(proc, base, type), placed = g_resLog && placed_region(proc, base, type);
    LONG st;
    if (g_on && fresh && !count && !(type & (MEM_TOP_DOWN | MEM_PHYSICAL))) {
        st = real_allocEx(proc, base, size, type | MEM_TOP_DOWN, prot, params, count);
        if (st >= 0) {
            InterlockedIncrement(&g_nAllocEx);
        } else {
            InterlockedIncrement(&g_nRetry);
            *base = NULL;
            st = real_allocEx(proc, base, size, type, prot, params, count);
        }
    } else {
        st = real_allocEx(proc, base, size, type, prot, params, count);
    }
    if (g_resLog && (fresh || placed) && st >= 0)
        note_reserve(*base, *size, placed ? type | PLACED : type, __builtin_return_address(0), HOOK_FROM);
    return st;
}

// Only releases of regions the log recorded are written.
static LONG NTAPI hook_free(HANDLE proc, PVOID *base, PSIZE_T size, ULONG type)
{
    LONG st = real_free(proc, base, size, type);
    if (st >= 0 && (type & MEM_RELEASE) && proc == CURRENT_PROCESS && base) {
        ULONG_PTR b = (ULONG_PTR)*base;
        if (live_remove(b)) {
            InterlockedIncrement(&g_nRel);
            res_record(b, 0, 0, __builtin_return_address(0), HOOK_FROM);
        }
    }
    return st;
}

static int is_image_section(HANDLE sec)
{
    SectionBasicInfo sbi;
    if (!pNtQuerySection || pNtQuerySection(sec, 0, &sbi, sizeof sbi, NULL) < 0) return 1;  // unknown: leave it
    return (sbi.AllocationAttributes & SEC_IMAGE_ATTR) != 0;
}

static LONG NTAPI hook_map(HANDLE sec, HANDLE proc, PVOID *base, ULONG_PTR zeroBits, SIZE_T commit,
                           PLARGE_INTEGER off, PSIZE_T viewSize, DWORD inherit, ULONG type, ULONG prot)
{
    if (!zeroBits && proc == CURRENT_PROCESS && base && !*base && !(type & (MEM_TOP_DOWN | MEM_PHYSICAL)) &&
        !is_image_section(sec)) {
        LONG st = real_map(sec, proc, base, zeroBits, commit, off, viewSize, inherit, type | MEM_TOP_DOWN, prot);
        if (st >= 0) { InterlockedIncrement(&g_nView); return st; }
        InterlockedIncrement(&g_nRetry);
        *base = NULL;
    }
    return real_map(sec, proc, base, zeroBits, commit, off, viewSize, inherit, type, prot);
}

// Copies the stub to `tramp`, publishes it through *real, then redirects the stub.
static int hook_stub(HMODULE ntdll, const char *name, void *hook, BYTE *tramp, void **real)
{
    BYTE *stub = (BYTE *)GetProcAddress(ntdll, name);
    if (!stub) { log_event("topdown: %s not exported; not hooked", name); return 0; }
    if (((ULONG_PTR)stub & 7) || stub[0] != 0xB8 || stub[5] != 0xBA || stub[10] != 0xFF || stub[11] != 0xD2 ||
        stub[12] != 0xC2) {
        log_event("topdown: %s at %p has an unexpected stub (%02x %02x %02x %02x %02x); not hooked", name,
                  (void *)stub, stub[0], stub[5], stub[10], stub[11], stub[12]);
        return 0;
    }
    memcpy(tramp, stub, STUB_LEN);
    *real = tramp;

    LONGLONG old = *(volatile LONGLONG *)stub, patched;
    BYTE bytes[8];
    memcpy(bytes, &old, 8);
    bytes[0] = 0xE9;
    LONG rel = (LONG)((BYTE *)hook - (stub + 5));
    memcpy(bytes + 1, &rel, 4);
    memcpy(&patched, bytes, 8);

    DWORD prot;
    if (!VirtualProtect(stub, 8, PAGE_EXECUTE_READWRITE, &prot)) {
        log_event("topdown: VirtualProtect on %s failed (%lu); not hooked", name, GetLastError());
        return 0;
    }
    InterlockedCompareExchange64((volatile LONGLONG *)stub, patched, old);
    VirtualProtect(stub, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), stub, 8);
    log_event("topdown: hooked %s at %p", name, (void *)stub);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reservation log: drain side (sampler thread)                        */
/* ------------------------------------------------------------------ */

#define MAX_MOD_NAMES 256
static struct { HMODULE h; char name[48]; } g_modNames[MAX_MOD_NAMES];
static int g_modNameCount;

// "module+0xoffset", or the raw address if no loaded module covers it any more.
static void frame_name(void *a, char *out, int cap)
{
    HMODULE hm;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)a, &hm)) {
        snprintf(out, cap, "0x%08lx", (unsigned long)(ULONG_PTR)a);
        return;
    }
    int i = 0;
    while (i < g_modNameCount && g_modNames[i].h != hm) i++;
    if (i == g_modNameCount) {
        WCHAR full[MAX_PATH];
        DWORD len = GetModuleFileNameW(hm, full, MAX_PATH);
        const WCHAR *s = full;
        for (DWORD k = 0; k < len; k++)
            if (full[k] == 92) s = full + k + 1;  // 92 = backslash
        char name[48];
        int k = 0;
        for (; s < full + len && k < (int)sizeof name - 1; s++)
            name[k++] = (*s < 128 && *s != ' ' && *s != ',') ? (char)*s : '_';
        name[k] = 0;
        if (g_modNameCount == MAX_MOD_NAMES) {
            snprintf(out, cap, "%s+0x%lx", name, (unsigned long)((ULONG_PTR)a - (ULONG_PTR)hm));
            return;
        }
        g_modNames[i].h = hm;
        memcpy(g_modNames[i].name, name, sizeof name);
        g_modNameCount++;
    }
    snprintf(out, cap, "%s+0x%lx", g_modNames[i].name, (unsigned long)((ULONG_PTR)a - (ULONG_PTR)hm));
}

static char g_drainBuf[32 * 1024];

void topdown_drain(void)
{
    if (!g_resLog) return;
    LONG head = g_ringHead;
    int n = 0;
    while (g_ringTail != head) {
        ResRec *r = &g_ring[g_ringTail & (RES_RING - 1)];
        if (r->seq != (DWORD)g_ringTail + 1) {
            // A record from an earlier lap that the drain had skipped, finished late: free the
            // slot (else every later writer to it drops its record).
            if (r->seq && (LONG)(r->seq - ((DWORD)g_ringTail + 1)) < 0) r->seq = 0;
            // Still being written, or its writer dropped it. A writer takes microseconds,
            // so a record that is still incomplete at the next drain was dropped.
            if (g_stallIdx != g_ringTail) { g_stallIdx = g_ringTail; break; }
            g_ringTail++;
            continue;
        }
        ResRec rec = *r;
        MemoryBarrier();
        r->seq = 0;
        g_ringTail++;
        n += snprintf(g_drainBuf + n, sizeof g_drainBuf - n, "%.3f,%s,0x%08lx,%lu,0x%lx,", rec.tMs / 1000.0,
                      rec.type ? "R" : "F", (unsigned long)rec.base, (unsigned long)(rec.size >> 10),
                      (unsigned long)rec.type);
        for (int i = 0; i < RES_FRAMES && rec.frames[i]; i++) {
            char f[80];
            frame_name(rec.frames[i], f, sizeof f);
            n += snprintf(g_drainBuf + n, sizeof g_drainBuf - n, "%s%s", i ? " " : "", f);
        }
        n += snprintf(g_drainBuf + n, sizeof g_drainBuf - n, "\r\n");
        if (n > (int)sizeof g_drainBuf - 2048) { write_all(g_resOut, g_drainBuf, n); n = 0; }
    }
    if (n) write_all(g_resOut, g_drainBuf, n);
}

/* ------------------------------------------------------------------ */
/* Setup and report                                                    */
/* ------------------------------------------------------------------ */

static int reslog_setup(const WCHAR *ini)
{
    g_resMin = GetPrivateProfileIntW(L"probe", L"ReserveLogMinKB", 1024, ini) << 10;
    g_ring = (ResRec *)VirtualAlloc(NULL, sizeof(ResRec) * RES_RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_live = (volatile ULONG_PTR *)VirtualAlloc(NULL, sizeof(ULONG_PTR) * LIVE_SLOTS, MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
    WCHAR name[64];
    _snwprintf(name, 64, L"reserves_%s.csv", g_session);
    name[63] = 0;
    if (g_ring && g_live) g_resOut = open_out(name);
    if (g_resOut == INVALID_HANDLE_VALUE) {
        log_event("reserve log: no buffers or output file; disabled");
        return 0;
    }
    static const char hdr[] = "t_s,op,base,size_kb,type,frames\r\n";
    write_all(g_resOut, hdr, sizeof hdr - 1);
    g_resT0 = GetTickCount64();
    return 1;
}

void topdown_init(const WCHAR *ini)
{
    int topDown = GetPrivateProfileIntW(L"probe", L"TopDown", 0, ini);
    int resLog = GetPrivateProfileIntW(L"probe", L"ReserveLog", 0, ini);
    if (resLog) resLog = reslog_setup(ini);
    if (!topDown && !resLog) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtQuerySection = (NtQuerySection_t)GetProcAddress(ntdll, "NtQuerySection");
    int views = topDown && GetPrivateProfileIntW(L"probe", L"TopDownViews", 1, ini);

    // The trampolines and the mode flags must be in place before any stub is patched.
    BYTE *page = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_EXECUTE_READWRITE);
    if (!page) { log_event("topdown: no trampoline page (%lu); disabled", GetLastError()); return; }
    g_on = topDown;
    g_resLog = resLog;
    int n = 0;
    n += hook_stub(ntdll, "NtAllocateVirtualMemory", (void *)hook_alloc, page, (void **)&real_alloc);
    n += hook_stub(ntdll, "NtAllocateVirtualMemoryEx", (void *)hook_allocEx, page + STUB_LEN,
                   (void **)&real_allocEx);
    if (views && pNtQuerySection)
        n += hook_stub(ntdll, "NtMapViewOfSection", (void *)hook_map, page + 2 * STUB_LEN, (void **)&real_map);
    if (resLog) n += hook_stub(ntdll, "NtFreeVirtualMemory", (void *)hook_free, page + 3 * STUB_LEN, (void **)&real_free);
    DWORD prot;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &prot);
    if (topDown)
        log_event("topdown: %s (%d hooks, mapped views %s)",
                  n ? "ON, new memory goes to the highest free addresses" : "FAILED", n,
                  views ? "included" : "excluded");
    if (resLog)
        log_event("reserve log: ON, regions >= %lu KB with stacks -> reserves_%ls.csv (%d hooks)",
                  (unsigned long)(g_resMin >> 10), g_session, n);
}

void topdown_report(void)
{
    if (g_on)
        log_event("topdown: redirected %ld allocations, %ld Ex allocations, %ld views; %ld fell back to default",
                  g_nAlloc, g_nAllocEx, g_nView, g_nRetry);
    if (g_resLog)
        log_event("reserve log: %ld reservations, %ld releases logged; %ld dropped, %ld not tracked (table full)",
                  g_nRes, g_nRel, g_nDropped, g_nLiveFull);
}