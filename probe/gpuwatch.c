// Who makes the GPU driver add 32 MB regions ([probe] GpuWatch, default 0; 1 = on; diagnostic).
//
// The D3D driver's CPU-visible buffers are mapped into the game by the graphics kernel, in 32 MB
// regions it creates itself and never gives back (see walk_va in probe.c). In a late-game save the
// driver kept adding them during AI turns, while the mapped total stayed flat: the regions fill
// with holes. A buffer gets its CPU address from D3DKMTLock / D3DKMTLock2, which the D3D runtime
// calls for the driver. This hooks those two calls in win32u.dll (the WOW64 system-call stubs, as
// topdown.c does for ntdll; nothing in the game's own code or imports is touched) and, after each
// one:
//  - looks up the region of the returned address; the first time an address lands in a 32 MB
//    region not seen before, it records the stack (return addresses found by scanning it), so the
//    events log names the game code that made the driver open the region: `gpuwatch: ...`. A
//    region the driver opened some other way gets a line the first time a lock lands in it too;
//    the sampler's `gpu chunk:` line, logged within 2 s of a region appearing, tells which;
//  - every 16th lock, at most 20 a second, records the stack and the size of the mapped piece, and
//    each report lists the callers that map the most (`gpuwatch: maps by ...`).
// The hooks take no locks of other code and allocate nothing; records go through a ring that
// the sampler thread drains. D3DKMTCreateAllocation calls are counted, and every 8th (at most 10
// a second) records its stack too (`gpuwatch: allocations by ...`): in a late-game session the
// driver created ~30 allocations a second, about one per lock.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define STUB_LEN 16
#define FRAMES 48
#define RING 256  // power of two
#define KNOWN 1024
#define SIGS 256

typedef LONG(NTAPI *Stub1_t)(void *arg);
static Stub1_t real_lock, real_lock2, real_create;
static volatile LONG g_nLock, g_nLock2, g_nCreate, g_nSampled, g_nDropped, g_nChunks, g_nKnownFull;
static ULONGLONG g_t0;
static int g_on;

typedef struct {
    volatile DWORD seq;  // index + 1 once complete, 0 when free
    DWORD tMs;
    ULONG_PTR addr, chunk;  // chunk = 0 for a sampled lock
    DWORD pieceKb, tid;
    BYTE call;              // 1 Lock, 2 Lock2, 3 CreateAllocation
    void *frames[FRAMES];
} Rec;

static Rec *g_ring;
static volatile LONG g_head;
static LONG g_tail, g_stall = -1;

// Allocation bases already looked at (32 MB chunks and other regions alike), so a region is
// walked once. Lock-free insert; a duplicate after a race only costs one extra walk.
static volatile ULONG_PTR g_known[KNOWN];
static volatile LONG g_knownCount;

static int known(ULONG_PTR ab)
{
    LONG n = g_knownCount;
    if (n > KNOWN) n = KNOWN;
    for (LONG i = 0; i < n; i++)
        if (g_known[i] == ab) return 1;
    return 0;
}

static void add_known(ULONG_PTR ab)
{
    LONG i = InterlockedIncrement(&g_knownCount) - 1;
    if (i < KNOWN) g_known[i] = ab;
}

static int known_full(void)
{
    return g_knownCount >= KNOWN;
}

// As in topdown.c: a value on the stack that points into image code just after a call.
static int is_return_address(ULONG_PTR ra)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)ra, &mbi, sizeof mbi)) return 0;
    DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE || !(mbi.Protect & x) || (mbi.Protect & PAGE_GUARD)) return 0;
    if (ra - 6 < (ULONG_PTR)mbi.BaseAddress) return 0;
    BYTE b[6];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ra - 6), b, 6, &got) || got != 6) return 0;
    const BYTE *e = b + 6;
    if (e[-5] == 0xE8) return 1;
    if (e[-6] == 0xFF && (e[-5] & 0x38) == 0x10) return 1;
    if (e[-2] == 0xFF && (e[-1] & 0xF8) == 0xD0) return 1;
    if (e[-3] == 0xFF && (e[-2] & 0x38) == 0x10 && (e[-2] & 0xC0) == 0x40) return 1;
    if (e[-2] == 0xFF && (e[-1] & 0x38) == 0x10 && (e[-1] & 0xC0) == 0x00) return 1;
    return 0;
}

// The exe's image range: the game code that calls Direct3D is in the exe, which never unloads.
static ULONG_PTR g_exeLo, g_exeHi;

// frames[0..HEAD) = the first return addresses above the hook (runtime and driver); then up to
// FRAMES - HEAD return addresses inside the exe, from up to 64 KB of stack: the driver's frames
// are deep and full of stale code addresses, which used up a plain scan before the game's.
#define HEAD 16
static void capture(void **frames, void *ret, const DWORD *from)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    ULONG_PTR lo = (ULONG_PTR)tib->StackLimit, hi = (ULONG_PTR)tib->StackBase;
    const DWORD *limit = from + 16384;
    if ((ULONG_PTR)limit > hi) limit = (const DWORD *)hi;
    int n = 0, g = HEAD;
    frames[n++] = ret;
    for (const DWORD *p = from; p < limit && g < FRAMES; p++) {
        ULONG_PTR v = *p;
        if (v < 0x10000 || (v >= lo && v < hi)) continue;
        int inExe = v >= g_exeLo && v < g_exeHi;
        if (n < HEAD && v != (ULONG_PTR)frames[n - 1] && is_return_address(v)) {
            frames[n++] = (void *)v;
            if (!inExe) continue;
        } else if (!inExe || v == (ULONG_PTR)frames[g - 1] || !is_return_address(v)) {
            continue;
        }
        frames[g++] = (void *)v;
    }
    while (n < HEAD) frames[n++] = NULL;
    while (g < FRAMES) frames[g++] = NULL;
}

static void record(ULONG_PTR addr, ULONG_PTR chunk, DWORD pieceKb, BYTE call, void *ret, const DWORD *from)
{
    LONG h = InterlockedIncrement(&g_head) - 1;
    Rec *r = &g_ring[h & (RING - 1)];
    if (r->seq) { InterlockedIncrement(&g_nDropped); return; }
    r->tMs = (DWORD)(GetTickCount64() - g_t0);
    r->addr = addr;
    r->chunk = chunk;
    r->pieceKb = pieceKb;
    r->call = call;
    r->tid = GetCurrentThreadId();
    capture(r->frames, ret, from);
    MemoryBarrier();
    r->seq = (DWORD)h + 1;
}

// Rate limit for the sampled stacks: 20 per second.
static volatile LONG g_sampleSec, g_sampleInSec;

static void on_mapped(ULONG_PTR p, BYTE call, void *ret, const DWORD *from)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery((LPCVOID)p, &mbi, sizeof mbi) || mbi.Type != MEM_PRIVATE) return;
    ULONG_PTR ab = (ULONG_PTR)mbi.AllocationBase;
    DWORD pieceKb = (DWORD)(mbi.RegionSize >> 10);
    if (!known(ab) && known_full()) {
        InterlockedIncrement(&g_nKnownFull);  // no room to remember it: no first-map stack either
    } else if (!known(ab)) {
        // Add up the region's pieces.
        ULONGLONG total = 0;
        ULONG_PTR a = ab;
        for (int i = 0; i < 4096 && VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) && (ULONG_PTR)mbi.AllocationBase == ab; i++) {
            total += mbi.RegionSize;
            a += mbi.RegionSize;
        }
        add_known(ab);
        if (total == (32ull << 20)) {
            InterlockedIncrement(&g_nChunks);
            record(p, ab, pieceKb, call, ret, from);
            return;
        }
    }
    LONG n = InterlockedIncrement(&g_nSampled);
    if (n & 15) return;
    LONG sec = (LONG)(GetTickCount64() / 1000);
    if (sec != g_sampleSec) { g_sampleSec = sec; g_sampleInSec = 0; }
    if (InterlockedIncrement(&g_sampleInSec) > 20) return;
    record(p, 0, pieceKb, call, ret, from);
}

#define HOOK_FROM ((const DWORD *)__builtin_frame_address(0) + 2)

// D3DKMT_LOCK: pData at +0x14; D3DKMT_LOCK2: pData at +0x0c (32-bit layouts).
static LONG NTAPI hook_lock(void *arg)
{
    LONG st = real_lock(arg);
    InterlockedIncrement(&g_nLock);
    if (st >= 0 && arg) on_mapped(*(ULONG_PTR *)((BYTE *)arg + 0x14), 1, __builtin_return_address(0), HOOK_FROM);
    return st;
}

static LONG NTAPI hook_lock2(void *arg)
{
    LONG st = real_lock2(arg);
    InterlockedIncrement(&g_nLock2);
    if (st >= 0 && arg) on_mapped(*(ULONG_PTR *)((BYTE *)arg + 0x0c), 2, __builtin_return_address(0), HOOK_FROM);
    return st;
}

static volatile LONG g_createSec, g_createInSec;

static LONG NTAPI hook_create(void *arg)
{
    LONG st = real_create(arg);
    LONG n = InterlockedIncrement(&g_nCreate);
    if (st < 0 || (n & 7)) return st;
    LONG sec = (LONG)(GetTickCount64() / 1000);
    if (sec != g_createSec) { g_createSec = sec; g_createInSec = 0; }
    if (InterlockedIncrement(&g_createInSec) <= 10) record(0, 0, 0, 3, __builtin_return_address(0), HOOK_FROM);
    return st;
}

// win32u's WOW64 stubs: mov eax, <n>; mov edx, <Wow64SystemServiceCall>; call edx; ret 4 (16 bytes).
static int hook_stub(HMODULE dll, const char *name, void *hook, BYTE *tramp, void **real)
{
    BYTE *stub = (BYTE *)GetProcAddress(dll, name);
    if (!stub || ((ULONG_PTR)stub & 7) || stub[0] != 0xB8 || stub[5] != 0xBA || stub[10] != 0xFF || stub[11] != 0xD2 ||
        stub[12] != 0xC2 || stub[13] != 0x04 || stub[14] != 0x00) {
        log_event("gpuwatch: %s isn't the known stub; not hooked", name);
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
    if (!VirtualProtect(stub, 8, PAGE_EXECUTE_READWRITE, &prot)) return 0;
    LONGLONG was = InterlockedCompareExchange64((volatile LONGLONG *)stub, patched, old);
    VirtualProtect(stub, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), stub, 8);
    if (was != old) {
        log_event("gpuwatch: %s changed while being hooked; not hooked", name);
        return 0;
    }
    return 1;
}

void gpuwatch_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"GpuWatch", 0, ini)) return;
    HMODULE w = GetModuleHandleW(L"win32u.dll");
    if (!w) w = LoadLibraryW(L"win32u.dll");
    g_ring = (Rec *)VirtualAlloc(NULL, sizeof(Rec) * RING, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    BYTE *page = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_EXECUTE_READWRITE);
    if (!w || !g_ring || !page) { log_event("gpuwatch: no win32u.dll or no buffers; off"); return; }
    g_t0 = GetTickCount64();
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    g_exeLo = (ULONG_PTR)exe;
    g_exeHi = g_exeLo + ((IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew))->OptionalHeader.SizeOfImage;
    int n = hook_stub(w, "NtGdiDdDDILock", (void *)hook_lock, page, (void **)&real_lock);
    n += hook_stub(w, "NtGdiDdDDILock2", (void *)hook_lock2, page + STUB_LEN, (void **)&real_lock2);
    n += hook_stub(w, "NtGdiDdDDICreateAllocation", (void *)hook_create, page + 2 * STUB_LEN, (void **)&real_create);
    DWORD prot;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &prot);
    g_on = n > 0;
    log_event("gpuwatch: watching the driver's buffer mappings (%d of 3 win32u calls hooked): who makes it add 32 MB regions", n);
}

/* ------------------------------------------------------------------ */
/* Drain and report (sampler thread)                                   */
/* ------------------------------------------------------------------ */

// The record's exe frames (the game code that called Direct3D), up to n; the runtime's and the
// driver's frames if no exe frame was found.
static int frames_text(void *const *frames, int n, char *out, int cap)
{
    int i = frames[HEAD] ? HEAD : 0, k = 0, len = 0;
    out[0] = 0;
    for (; i < FRAMES && frames[i] && k < n; i++, k++) {
        ULONG_PTR base;
        const char *m = module_at(frames[i], &base);
        len += m ? snprintf(out + len, cap - len, "%s%s+0x%lx", k ? " < " : "", m, (unsigned long)((ULONG_PTR)frames[i] - base))
                 : snprintf(out + len, cap - len, "%s0x%08lx", k ? " < " : "", (unsigned long)(ULONG_PTR)frames[i]);
        if (len >= cap - 1) {
            len = cap - 1;  // truncated (snprintf returns the length it wanted)
            break;
        }
    }
    return len;
}

static struct { char key[200]; DWORD count, kb; BYTE create; } g_sigs[SIGS];
static int g_sigCount;

void gpuwatch_tick(void)
{
    if (!g_on) return;
    LONG head = g_head;
    while (g_tail != head) {
        Rec *r = &g_ring[g_tail & (RING - 1)];
        if (r->seq != (DWORD)g_tail + 1) {
            if (r->seq && (LONG)(r->seq - ((DWORD)g_tail + 1)) < 0) r->seq = 0;
            if (g_stall != g_tail) { g_stall = g_tail; break; }
            g_tail++;
            continue;
        }
        Rec rec = *r;
        MemoryBarrier();
        r->seq = 0;
        g_tail++;
        if (rec.chunk) {
            char text[1200];
            frames_text(rec.frames, 12, text, sizeof text);
            log_event("gpuwatch: %.1f s: first map into the 32 MB region 0x%08lx (%s, piece %lu KB, thread %lu) from %s",
                      rec.tMs / 1000.0, (unsigned long)rec.chunk, rec.call == 1 ? "Lock" : "Lock2", (unsigned long)rec.pieceKb,
                      (unsigned long)rec.tid, text);
            continue;
        }
        char key[200];
        int n = frames_text(rec.frames, 4, key, sizeof key);
        snprintf(key + n, sizeof key - n, " [thread %lu]", (unsigned long)rec.tid);
        int i = 0;
        BYTE create = rec.call == 3;
        while (i < g_sigCount && (g_sigs[i].create != create || strcmp(g_sigs[i].key, key))) i++;
        if (i == g_sigCount) {
            if (g_sigCount == SIGS) continue;
            memcpy(g_sigs[i].key, key, sizeof key);
            g_sigs[i].create = create;
            g_sigCount++;
        }
        g_sigs[i].count++;
        g_sigs[i].kb += rec.pieceKb;
    }
}

void gpuwatch_report(void)
{
    if (!g_on) return;
    gpuwatch_tick();
    log_event("gpuwatch: %ld Lock, %ld Lock2, %ld CreateAllocation calls; %ld regions of 32 MB first mapped by a lock; "
              "%ld records dropped", g_nLock, g_nLock2, g_nCreate, g_nChunks, g_nDropped);
    if (g_nKnownFull)
        log_event("gpuwatch: the table of %d known regions filled; %ld locks into regions after that weren't checked "
                  "for a first map", KNOWN, g_nKnownFull);
    // Top callers of the sampled locks, by count.
    int order[SIGS];
    for (int i = 0; i < g_sigCount; i++) order[i] = i;
    for (int i = 0; i < g_sigCount; i++)
        for (int j = i + 1; j < g_sigCount; j++)
            if (g_sigs[order[j]].count > g_sigs[order[i]].count) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int create = 0; create < 2; create++)
        for (int i = 0, shown = 0; i < g_sigCount && shown < 10; i++) {
            if (g_sigs[order[i]].create != create) continue;
            shown++;
            if (create)
                log_event("gpuwatch: allocations by %s: %lu sampled", g_sigs[order[i]].key, (unsigned long)g_sigs[order[i]].count);
            else
                log_event("gpuwatch: maps by %s: %lu sampled, pieces %.1f MB in all", g_sigs[order[i]].key,
                          (unsigned long)g_sigs[order[i]].count, g_sigs[order[i]].kb / 1024.0);
        }
}
