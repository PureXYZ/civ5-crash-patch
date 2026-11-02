// The fluid fog's particle pools sized to what the fog needs ([probe] FluidPools, default 1; 0 = the
// game's own 24 MB per job thread).
//
// The fluid fog of war (App\Graphics\FluidParticles.cpp) is a particle simulation on a grid of
// cells. Its object (the fog object exe+0x9a94d8 + 8) gets one particle pool per job thread at
// startup (exe+0x28f780, MaxSimultaneousThreads of them, the slots at +0x40, the counts at
// +0x440): 196,608 particles of 128 bytes, 24 MB each, zeroed. Nothing in the game stores that
// capacity or checks against it. The particles live in the grid (exe+0x329dd4: per cell up to 8
// blocks of 4, the block counts at [grid+0x3c] + 32 x cell); the pools only carry them through a
// step:
// - Each update (exe+0x109990) runs 3 substeps over the cells in view plus a margin. A substep
//   zeroes the counts (exe+0x109b2a), runs the step jobs (the call at exe+0x109c5d, range = the
//   cells), and merges the pools back into the grid (exe+0x0d07d0: the cells cleared, every pool's
//   [0, count) inserted, at most 32 a cell, the rest dropped). A job (exe+0x1eb5b0) takes each
//   block of a cell to the step (exe+0x2b9f60) or, at the edge, the pass-through (exe+0x381a20);
//   each writes at most one particle a lane, 4 a block, into the pool of the thread it runs on
//   (pool[thread][count[thread]++]). So in one substep a pool gets at most 4 x the blocks in the
//   range, however the jobs are spread over the threads.
// - Loading a map seeds the whole map at once, on a lattice, into pool 0 (exe+0x1b6af0 ->
//   exe+0x17eeb0, one call a particle), then merges them (exe+0x1bfc50): 66,788 particles on a
//   map of 6,790 tiles. That is what pool 0 is sized for, and a seed of more than 196,608 writes
//   past it.
// After seeding nothing makes particles, so the grid only loses them (the merge's cap).
//
// Measured (2026-11-02, a late Huge-size VP save, 4 threads): seed 66,788; the largest step 10,660
// particles at most; the busiest pool 8,731 in a step, the others under 3,000. Of 4 x 24 MB.
//
// The step and the job are in code Steam's CEG checksums (exe+0x2d00f0, exe+0x173d0; cegall.py),
// which can't change. Two places outside every range do:
// - The step launch call (exe+0x109c5d) goes to step_stub, which first counts the blocks in the
//   range and makes every pool hold 4 x that (the counts were just zeroed and no job is running,
//   so a pool can be swapped for a new block without copying), then calls the launcher, then
//   notes how full each pool got. A pool is the game's own 24 MB block until the first step, then
//   one of ours: need + 25% + 4,096, in whole MB, at least 1 MB; it grows at once when the need
//   is larger, and shrinks to what the last 30 s needed when it holds more than twice that.
// - The seeding's first instruction (exe+0x17eeb0, `mov eax,[ecx+440h]`) calls seed_stub, which
//   grows pool 0 (copying what's in it) before a particle would go past its end, so a map of any
//   size fits. Its first call per load also swaps the other pools for small blocks (copying their
//   stale entries, which the merge after seeding still reads).
// Blocks come from the exe's malloc import, as the game's own pools do (its new: GetSmallObject-
// Allocator, then malloc for anything big), and old ones go to the game's delete (FSBA or free),
// as its destructor (exe+0x3658c3) frees them. The particles and what the fog does with them are
// the same; only where the pools are changes. The DX9 and tablet exes have the same code (exes.c).
//
// fluidpool_report() logs the pools now, the most one pool held, the largest need and the seed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define ORIG 0x30000        // the game's pool: 196,608 particles
#define ENTRY 128           // bytes a particle
#define MB_ENTRIES 8192     // particles a MB
#define MAX_POOLS 256       // pool and count slots in the object
#define OFF_POOLS 0x40
#define OFF_COUNTS 0x440
#define OFF_GRID 0x904
#define OFF_THREADED 0x909  // the pools exist (jobs on)
#define OFF_NPOOLS 0x90c
#define GRID_W 0x10         // grid: cells a row,
#define GRID_H 0x14         //   rows,
#define GRID_BLOCKS 0x3c    //   blocks a cell, at dword 8 x cell
#define SHRINK_MS 30000

typedef void *(__cdecl *malloc_t)(size_t);
typedef void(__cdecl *delete_t)(void *);
static void *volatile *g_mallocSlot;   // the exe's malloc import, read at each use
static delete_t g_delete;
void *g_fluidStepFn;                   // the step launcher (step_stub calls it)
static volatile LONG g_lock;

typedef struct { BYTE *p; DWORD cap, most; BYTE seedGrown; } Pool;
static Pool g_pool[MAX_POOLS];
static BYTE *g_obj;
static DWORD g_n;
static int g_seeding;
static DWORD g_winMost, g_winStart;
static DWORD g_maxNeed, g_maxSeed, g_resizes, g_fails, g_overflows, g_steps;
DWORD(WINAPI *g_fluidClock)(void) = GetTickCount;  // the test's clock

static malloc_t exe_malloc(void) { return (malloc_t)*g_mallocSlot; }

static DWORD *counts(BYTE *obj) { return (DWORD *)(obj + OFF_COUNTS); }
static BYTE **slots(BYTE *obj) { return (BYTE **)(obj + OFF_POOLS); }

// Pools in use: jobs on, 1..256 pools, a grid.
static DWORD pools_of(BYTE *obj)
{
    if (!obj || !obj[OFF_THREADED] || !*(BYTE **)(obj + OFF_GRID)) return 0;
    DWORD n = *(DWORD *)(obj + OFF_NPOOLS);
    return n <= MAX_POOLS ? n : 0;
}

static int mine(BYTE *obj, DWORD i) { return slots(obj)[i] && slots(obj)[i] == g_pool[i].p; }
static DWORD cap_of(BYTE *obj, DWORD i) { return mine(obj, i) ? g_pool[i].cap : ORIG; }

// need + 25% + 4,096, in whole MB, at least 1 MB.
static DWORD size_for(DWORD need)
{
    DWORD n = need + need / 4 + 4096;
    return (n + MB_ENTRIES - 1) / MB_ENTRIES * MB_ENTRIES;
}

static void lock(void) { while (InterlockedExchange(&g_lock, 1)) YieldProcessor(); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

// What the pools hold now, in MB.
static double held_mb(BYTE *obj, DWORD n)
{
    double mb = 0;
    for (DWORD i = 0; i < n; i++)
        if (slots(obj)[i]) mb += cap_of(obj, i) / (double)MB_ENTRIES;
    return mb;
}

static DWORD g_gave;  // the game's own pools given back
static void log_gave(BYTE *obj, DWORD n, DWORD before, const char *when)
{
    if (g_gave != before)
        log_event("fluidpool: gave back %lu of the game's 24 MB pools at %s; the %lu pools hold %.1f MB",
                  (unsigned long)(g_gave - before), when, (unsigned long)n, held_mb(obj, n));
}

// Pool i becomes a new block of `entries` (at least its count), holding its [0, count). 0 if there
// was no room: the old one stays.
static int resize(BYTE *obj, DWORD i, DWORD entries)
{
    BYTE *old = slots(obj)[i];
    int game = old && !mine(obj, i);
    DWORD n = counts(obj)[i], have = cap_of(obj, i);
    if (n > have) n = have;  // past its end it holds nothing of the game's
    if (entries < n) entries = n;
    BYTE *p = exe_malloc()((size_t)entries * ENTRY);
    if (!p) {
        if (!g_fails++)
            log_event("fluidpool: no room for a fog particle pool of %.1f MB; pool %lu keeps its %.1f MB",
                      entries / (double)MB_ENTRIES, (unsigned long)i, have / (double)MB_ENTRIES);
        return 0;
    }
    if (n) memcpy(p, old, (size_t)n * ENTRY);
    slots(obj)[i] = p;
    if (old) g_delete(old);
    DWORD most = g_pool[i].most;
    g_pool[i] = (Pool){p, entries, most, 0};
    g_resizes++;
    g_gave += game;
    return 1;
}

// Before the step jobs of a substep: every pool holds 4 particles for each block in the range
// (rows [1]..[0], columns [4]..[3], as the job reads it).
void __cdecl fluid_step_pre(const LONG *range, BYTE *const *body)
{
    BYTE *obj = body[0];
    DWORD n = pools_of(obj);
    if (!n) return;
    BYTE *grid = *(BYTE **)(obj + OFF_GRID);
    LONG w = *(LONG *)(grid + GRID_W), h = *(LONG *)(grid + GRID_H);
    const DWORD *blocks = *(const DWORD *const *)(grid + GRID_BLOCKS);
    LONG y0 = range[1] < 0 ? 0 : range[1], y1 = range[0] > h ? h : range[0];
    LONG x0 = range[4] < 0 ? 0 : range[4], x1 = range[3] > w ? w : range[3];
    DWORD need = 0;
    if (blocks)
        for (LONG y = y0; y < y1; y++)
            for (LONG x = x0; x < x1; x++) need += blocks[8 * (y * w + x)];
    need *= 4;

    lock();
    g_obj = obj;
    g_n = n;
    g_seeding = 0;
    g_steps++;
    if (need > g_maxNeed) g_maxNeed = need;
    DWORD now = g_fluidClock();
    if (need > g_winMost) g_winMost = need;
    int shrink = now - g_winStart >= SHRINK_MS;
    DWORD want = size_for(need), small = size_for(g_winMost), gave = g_gave;
    for (DWORD i = 0; i < n; i++) {
        if (!slots(obj)[i]) continue;
        DWORD cap = cap_of(obj, i);
        if (need > cap || !mine(obj, i) || g_pool[i].seedGrown) {
            if (!resize(obj, i, want) && need > cap && !g_overflows)
                log_event("fluidpool: pool %lu holds %lu particles, this step may need %lu", (unsigned long)i,
                          (unsigned long)cap, (unsigned long)need);
        } else if (shrink && cap > 2 * small) {
            resize(obj, i, small);
        }
    }
    if (shrink) {
        g_winStart = now;
        g_winMost = need;
    }
    log_gave(obj, n, gave, "a fog step");
    unlock();
}

// After them: how full each pool got (more than it holds would mean the bound is wrong).
void __cdecl fluid_step_post(BYTE *const *body)
{
    BYTE *obj = body[0];
    DWORD n = pools_of(obj);
    for (DWORD i = 0; i < n; i++) {
        DWORD c = counts(obj)[i];
        if (mine(obj, i) && c > g_pool[i].most) g_pool[i].most = c;
        if (c > cap_of(obj, i) && !g_overflows++)
            log_event("fluidpool: pool %lu got %lu particles, more than its %lu", (unsigned long)i, (unsigned long)c,
                      (unsigned long)cap_of(obj, i));
    }
}

// Before each seeded particle: pool 0 has room for it. The first call per load also swaps the
// other pools, if still the game's, for small blocks.
void __cdecl fluid_seed_pre(BYTE *obj)
{
    DWORD n = pools_of(obj);
    if (!n || !slots(obj)[0]) return;
    DWORD c = counts(obj)[0];
    if (c >= g_maxSeed) g_maxSeed = c + 1;
    if (g_seeding && c < cap_of(obj, 0)) return;
    lock();
    if (!g_seeding) {
        g_seeding = 1;
        g_obj = obj;
        g_n = n;
        DWORD gave = g_gave;
        for (DWORD i = 1; i < n; i++)
            if (slots(obj)[i] && !mine(obj, i)) resize(obj, i, size_for(counts(obj)[i]));
        log_gave(obj, n, gave, "a map's seeding");
    }
    DWORD cap = cap_of(obj, 0);
    if (c >= cap && resize(obj, 0, cap < ORIG ? ORIG : cap + cap / 2)) g_pool[0].seedGrown = 1;
    unlock();
}

// exe+0x17eeb0 (`mov eax,[ecx+440h]`) -> `call seed_stub; nop`, ecx = the particle object. Keeps
// every register but eax, which gets what the instruction loads.
void seed_stub(void);
// exe+0x109c5d (`call <step launcher>`, cdecl: range, body, flag) -> step_stub.
void step_stub(void);
__asm__(".text\n"
        ".globl _seed_stub\n_seed_stub:\n"
        "\tpushl %ecx\n"
        "\tpushl %edx\n"
        "\tpushl %ecx\n"
        "\tcall _fluid_seed_pre\n"
        "\taddl $4, %esp\n"
        "\tpopl %edx\n"
        "\tpopl %ecx\n"
        "\tmovl 0x440(%ecx), %eax\n"
        "\tret\n"
        ".globl _step_stub\n_step_stub:\n"
        "\tpushl 8(%esp)\n"
        "\tpushl 8(%esp)\n"
        "\tcall _fluid_step_pre\n"
        "\taddl $8, %esp\n"
        "\tpushl 0xc(%esp)\n"
        "\tpushl 0xc(%esp)\n"
        "\tpushl 0xc(%esp)\n"
        "\tcall *_g_fluidStepFn\n"
        "\taddl $0xc, %esp\n"
        "\tpushl 8(%esp)\n"
        "\tcall _fluid_step_post\n"
        "\taddl $4, %esp\n"
        "\tret\n");

void *fluidpool_seed_stub(void) { return (void *)seed_stub; }  // for the test
void *fluidpool_step_stub(void) { return (void *)step_stub; }

static int bytes_at(const BYTE *exe, DWORD size, DWORD rva, const BYTE *want, DWORD n)
{
    return rva && rva + n <= size && !memcmp(exe + rva, want, n);
}

// The rel32 call target at rva, as an RVA (0 if not a call into the image).
static DWORD call_rva(BYTE *exe, DWORD rva)
{
    BYTE *t = exe_branch_target(exe, exe + rva);
    return t ? (DWORD)(t - exe) : 0;
}

// `call [slot]` (FF 15) within n bytes of fn.
static int calls_slot(const BYTE *exe, DWORD size, DWORD fn, const BYTE *slot, DWORD n)
{
    BYTE want[6] = {0xFF, 0x15};
    DWORD va = (DWORD)(ULONG_PTR)slot;
    memcpy(want + 2, &va, 4);
    for (DWORD i = 0; fn && i < n && fn + i + 6 <= size; i++)
        if (!memcmp(exe + fn + i, want, 6)) return 1;
    return 0;
}

// The game's delete: the destructor's call, through one jmp (the DX9 exe's).
static DWORD delete_rva(BYTE *exe, const GameExe *g)
{
    DWORD d = call_rva(exe, g->fluidFree);
    if (d && exe[d] == 0xE9) d = (DWORD)(exe_branch_target(exe, exe + d) - exe);
    return d;
}

static DWORD count_in(const BYTE *exe, DWORD fn, DWORD len, const BYTE *pat, DWORD n)
{
    DWORD hits = 0;
    for (DWORD i = 0; i + n <= len; i++) hits += !memcmp(exe + fn + i, pat, n);
    return hits;
}

const char *fluidpool_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!g->fluidSeed) return "not mapped for this exe";
    if (!g->fluidInit || !g->fluidReset || !g->fluidStepCall || !g->fluidJob || !g->fluidStepFn ||
        !g->fluidPassFn || !g->fluidMerge || !g->fluidFree)
        return "addresses";
    BYTE *mallocSlot = exe_import_slot(exe, "malloc"), *freeSlot = exe_import_slot(exe, "free");
    if (!mallocSlot || !freeSlot) return "imports";
    // The pools: lea ebp,[esi+440h] (the counts; a 3-byte nop after it, but not in the DX9 exe) /
    // push 0; push 39h; push 26Fh; push file; push 1; push 1800000h; call new / ... / push ctor;
    // push 30000h; push 80h; push edi; call (196,608 of 128 bytes) / mov [ebp-400h],edi (the slot) /
    // push 1800000h; push 0; push edi; call memset / mov [ebp],0 (the count) / inc ebx ...
    // cmp ebx,[esi+90Ch] (one a pool).
    static const BYTE kLea[] = {0x8D, 0xAE, 0x40, 0x04, 0x00, 0x00};
    static const BYTE kAlloc[] = {0x6A, 0x00, 0x6A, 0x39, 0x68, 0x6F, 0x02, 0x00, 0x00};
    static const BYTE kSize[] = {0x6A, 0x01, 0x68, 0x00, 0x00, 0x80, 0x01, 0xE8};
    static const BYTE kCtor[] = {0x68, 0x00, 0x00, 0x03, 0x00, 0x68, 0x80, 0x00, 0x00, 0x00, 0x57, 0xE8};
    static const BYTE kSlot[] = {0x89, 0xBD, 0x00, 0xFC, 0xFF, 0xFF};
    static const BYTE kZero[] = {0x68, 0x00, 0x00, 0x80, 0x01, 0x6A, 0x00, 0x57, 0xE8};
    static const BYTE kCount[] = {0xC7, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x83, 0xC4, 0x0C, 0x83, 0xC5, 0x04,
                                  0x3B, 0x9E, 0x0C, 0x09, 0x00, 0x00};
    DWORD in = g->fluidInit;
    if (in < 9 || !(bytes_at(exe, size, in - 6, kLea, sizeof kLea) || bytes_at(exe, size, in - 9, kLea, sizeof kLea)) ||
        !bytes_at(exe, size, in, kAlloc, sizeof kAlloc) ||
        !bytes_at(exe, size, in + 0xe, kSize, sizeof kSize) || !bytes_at(exe, size, in + 0x28, kCtor, sizeof kCtor) ||
        !bytes_at(exe, size, in + 0x3c, kSlot, sizeof kSlot) || !bytes_at(exe, size, in + 0x4a, kZero, sizeof kZero) ||
        !bytes_at(exe, size, in + 0x57, kCount, sizeof kCount))
        return "pool init";
    if (!calls_slot(exe, size, call_rva(exe, in + 0x15), mallocSlot, 0x60)) return "pool new";
    // The seeding's particle: count, base, the fields written through [ecx+40h] each time, inc, ret 14h.
    static const BYTE kSeed[] = {0x8B, 0x81, 0x40, 0x04, 0x00, 0x00, 0x8B, 0x54, 0x24, 0x04, 0xF3, 0x0F, 0x10, 0x44,
                                 0x24, 0x0C, 0xC1, 0xE0, 0x07, 0x03, 0x41, 0x40, 0x56, 0x8B, 0x32, 0x89, 0x30};
    static const BYTE kSeedEnd[] = {0xFF, 0x81, 0x40, 0x04, 0x00, 0x00, 0x5F, 0x5E, 0xC2, 0x14, 0x00};
    if (!bytes_at(exe, size, g->fluidSeed, kSeed, sizeof kSeed) || !bytes_at(exe, size, g->fluidSeed + 0x83, kSeedEnd, sizeof kSeedEnd))
        return "seed";
    // The substep: the counts zeroed (xor eax,eax; cmp [esi+90Ch],eax; jbe; lea ecx,[esi+440h]; ...
    // mov [ecx],0; add eax,edi; add ecx,4; cmp eax,[esi+90Ch]; jb), then the step launch, then the merge.
    static const BYTE kReset[] = {0x33, 0xC0, 0x39, 0x86, 0x0C, 0x09, 0x00, 0x00, 0x76, 0x1F, 0x8D, 0x8E, 0x40,
                                  0x04, 0x00, 0x00, 0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x01, 0x00, 0x00,
                                  0x00, 0x00, 0x03, 0xC7, 0x83, 0xC1, 0x04, 0x3B, 0x86, 0x0C, 0x09, 0x00, 0x00, 0x72, 0xED};
    static const BYTE kAfter[] = {0x8B, 0x84, 0x24, 0xAC, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x18, 0x6A, 0x00, 0x50, 0x8D,
                                  0x4C, 0x24, 0x4C, 0x51, 0x8D, 0x54, 0x24, 0x48, 0x52, 0x8B, 0xCE, 0xE8};
    if (!bytes_at(exe, size, g->fluidReset, kReset, sizeof kReset) || g->fluidStepCall < g->fluidReset ||
        g->fluidStepCall - g->fluidReset > 0x180)
        return "substep";
    DWORD launcher = call_rva(exe, g->fluidStepCall);
    if (!launcher || !bytes_at(exe, size, g->fluidStepCall + 5, kAfter, sizeof kAfter) ||
        call_rva(exe, g->fluidStepCall + 5 + sizeof kAfter - 1) != g->fluidMerge)
        return "step call";
    // The launcher: cdecl, the range in its first argument (rows [4]..[0], columns [10h]..[0Ch]).
    static const BYTE kLaunch[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x56, 0x8B, 0xB4, 0x24, 0x0C, 0x01, 0x00, 0x00,
                                   0x8B, 0x46, 0x04, 0x3B, 0x06, 0x7D, 0x6C, 0x8B, 0x4E, 0x10, 0x3B, 0x4E, 0x0C, 0x7D};
    if (!bytes_at(exe, size, launcher, kLaunch, sizeof kLaunch)) return "step launcher";
    // The job: cell = row x [grid+10h] + column, times 8; blocks at [grid+38h] + 100h each, their
    // count [grid+3Ch][cell]; the edge to the pass-through, the rest to the step; the count read
    // again after each block.
    static const BYTE kCell[] = {0x8B, 0x4D, 0x00, 0x8B, 0x91, 0x04, 0x09, 0x00, 0x00, 0x8B, 0x72, 0x10, 0x0F,
                                 0xAF, 0xF7, 0x03, 0xF0, 0x03, 0xF6, 0x03, 0xF6, 0x03, 0xF6};
    static const BYTE kBlocks[] = {0x8B, 0xFE, 0xC1, 0xE7, 0x08, 0x03, 0x7A, 0x38, 0x8B, 0x52, 0x3C, 0x33, 0xDB, 0x39, 0x1C, 0xB2, 0x76};
    static const BYTE kNext[] = {0x8B, 0x4D, 0x00, 0x8B, 0x81, 0x04, 0x09, 0x00, 0x00, 0x8B, 0x50, 0x3C, 0x43,
                                 0x81, 0xC7, 0x00, 0x01, 0x00, 0x00, 0x3B, 0x1C, 0xB2, 0x72};
    DWORD j = g->fluidJob;
    if (!bytes_at(exe, size, j, kCell, sizeof kCell) || !bytes_at(exe, size, j + 0x39, kBlocks, sizeof kBlocks) ||
        !bytes_at(exe, size, j + 0x77, kNext, sizeof kNext) || call_rva(exe, j + 0x5d) != g->fluidPassFn ||
        call_rva(exe, j + 0x72) != g->fluidStepFn)
        return "job";
    // The step (to its ret 10h): pool = [esi+4*thread+40h], 4 lanes, the count read 9 times and
    // incremented once, at the end of a lane.
    static const BYTE kBase[] = {0x8B, 0x45, 0x0C, 0x8B, 0x4C, 0x86, 0x40};
    static const BYTE kLanes[] = {0xC7, 0x44, 0x24, 0x20, 0x04, 0x00, 0x00, 0x00};
    static const BYTE kInc[] = {0xFF, 0x84, 0x86, 0x40, 0x04, 0x00, 0x00, 0x83, 0xC7, 0x04, 0x83, 0x6C, 0x24, 0x20, 0x01, 0x0F, 0x85};
    static const BYTE kCountRef[] = {0x86, 0x40, 0x04, 0x00, 0x00};
    static const BYTE kStepEnd[] = {0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x10, 0x00};
    DWORD s = g->fluidStepFn;
    if (!bytes_at(exe, size, s + 0x4c, kBase, sizeof kBase) || !bytes_at(exe, size, s + 0x78, kLanes, sizeof kLanes) ||
        !bytes_at(exe, size, s + 0x4ed, kInc, sizeof kInc) || !bytes_at(exe, size, s + 0x502, kStepEnd, sizeof kStepEnd) ||
        count_in(exe, s, 0x50b, kInc, 3) != 1 || count_in(exe, s, 0x50b, kCountRef, sizeof kCountRef) != 10)
        return "step";
    // The pass-through: pool = [ecx+4*thread+40h], ebx = 1, four lanes unrolled, each adding ebx to
    // the count, ret 8.
    static const BYTE kPass[] = {0x8B, 0x7C, 0xB1, 0x40, 0xBB, 0x01, 0x00, 0x00, 0x00};
    static const BYTE kAdd[] = {0x01, 0x9C, 0xB1, 0x40, 0x04, 0x00, 0x00};
    static const BYTE kPassEnd[] = {0x5F, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x14, 0xC2, 0x08, 0x00};
    DWORD p = g->fluidPassFn;
    if (!bytes_at(exe, size, p + 0x1b, kPass, sizeof kPass) || !bytes_at(exe, size, p + 0x3ba, kPassEnd, sizeof kPassEnd) ||
        count_in(exe, p, 0x3ba, kAdd, sizeof kAdd) != 4 || !bytes_at(exe, size, p + 0xfb, kAdd, sizeof kAdd) ||
        !bytes_at(exe, size, p + 0x1e3, kAdd, sizeof kAdd) || !bytes_at(exe, size, p + 0x2cb, kAdd, sizeof kAdd) ||
        !bytes_at(exe, size, p + 0x3b3, kAdd, sizeof kAdd))
        return "pass-through";
    // The merge reads each pool and its count through the slots: lea edi,[esi+440h] ... mov ecx,[edi-400h]; mov edx,[edi].
    static const BYTE kMerge[] = {0x8D, 0xBE, 0x40, 0x04, 0x00, 0x00, 0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x86, 0x04,
                                  0x09, 0x00, 0x00, 0x8B, 0x8F, 0x00, 0xFC, 0xFF, 0xFF, 0x8B, 0x17};
    if (!bytes_at(exe, size, g->fluidMerge + 0x34, kMerge, sizeof kMerge)) return "merge";
    // The destructor frees each pool with the game's delete (FSBA, else the free import):
    // call delete; mov [edi],ebx; inc ebp; add esp,4; add edi,4; cmp ebp,[esi+90Ch]; jb.
    static const BYTE kFree[] = {0x89, 0x1F, 0x45, 0x83, 0xC4, 0x04, 0x83, 0xC7, 0x04, 0x3B, 0xAE, 0x0C, 0x09, 0x00, 0x00, 0x72};
    if (exe[g->fluidFree] != 0xE8 || !bytes_at(exe, size, g->fluidFree + 5, kFree, sizeof kFree)) return "destructor";
    if (!calls_slot(exe, size, delete_rva(exe, g), freeSlot, 0x100)) return "delete";
    return NULL;
}

// Sends the seeding and the step launch to the stubs. fluidpool_check must have passed. alloc and
// del stand in for the exe's malloc import and the game's delete (the test's; NULL = the game's),
// launcher for the step launcher. NULL when done, else why nothing changed.
const char *fluidpool_apply(BYTE *exe, const GameExe *g, void *(__cdecl *alloc)(size_t), void(__cdecl *del)(void *),
                            void *launcher)
{
    static void *s_alloc;
    s_alloc = (void *)alloc;
    g_mallocSlot = alloc ? (void *volatile *)&s_alloc : (void *volatile *)exe_import_slot(exe, "malloc");
    g_delete = del ? del : (delete_t)(exe + delete_rva(exe, g));
    g_fluidStepFn = launcher ? launcher : exe + call_rva(exe, g->fluidStepCall);
    g_winStart = g_fluidClock();
    BYTE *at[2] = {exe + g->fluidSeed, exe + g->fluidStepCall};
    BYTE code[2][6];
    LONG rel = (LONG)((BYTE *)seed_stub - (at[0] + 5));
    code[0][0] = 0xE8;
    memcpy(code[0] + 1, &rel, 4);
    code[0][5] = 0x90;
    rel = (LONG)((BYTE *)step_stub - (at[1] + 5));
    code[1][0] = 0xE8;
    memcpy(code[1] + 1, &rel, 4);
    DWORD len[2] = {6, 5}, prot[2];
    int w = 0;
    while (w < 2 && VirtualProtect(at[w], len[w], PAGE_EXECUTE_READWRITE, &prot[w])) w++;
    if (w == 2)
        for (int i = 0; i < 2; i++) memcpy(at[i], code[i], len[i]);
    for (int i = w - 1; i >= 0; i--) VirtualProtect(at[i], len[i], prot[i], &prot[i]);
    if (w < 2) return "VirtualProtect";
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return NULL;
}

void fluidpool_init(const WCHAR *ini)
{
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    if (!GetPrivateProfileIntW(L"probe", L"FluidPools", 1, ini)) return;
    if (!g || !g->fluidSeed) {
        log_event("fluidpool: not mapped for this exe; the fog keeps the game's particle pools");
        return;
    }
    const char *why = fluidpool_check(exe, g);
    if (!why) why = fluidpool_apply(exe, g, NULL, NULL, NULL);
    if (why) {
        log_event("fluidpool: the fog's particle pools aren't the known code (%s); left alone", why);
        return;
    }
    log_event("fluidpool: the fog's particle pools hold what each step can write (the game's: 24 MB per job thread); "
              "from the first map load on");
}

void fluidpool_report(void)
{
    if (!g_obj) return;
    char line[512];
    int o = 0, freed = 1;
    lock();
    DWORD n = g_n < 8 ? g_n : 8;
    for (DWORD i = 0; i < g_n; i++) freed &= !slots(g_obj)[i];
    // At exit the game's destructor has freed them (slots cleared): then the sizes they last had.
    double held = freed ? 0 : held_mb(g_obj, g_n);
    for (DWORD i = 0; i < g_n && freed; i++) held += g_pool[i].cap / (double)MB_ENTRIES;
    for (DWORD i = 0; i < n && o < (int)sizeof line - 48; i++) {
        if (!freed && !mine(g_obj, i))
            o += snprintf(line + o, sizeof line - o, "%s%s", i ? ", " : "", slots(g_obj)[i] ? "the game's 24 MB" : "none");
        else
            o += snprintf(line + o, sizeof line - o, "%s%.1f MB (most %lu)", i ? ", " : "", g_pool[i].cap / (double)MB_ENTRIES,
                          (unsigned long)g_pool[i].most);
    }
    unlock();
    log_event("fluidpool: %lu pools, %s %.1f MB (the game's: %lu MB): %s%s; largest step need %lu particles, seed %lu, "
              "%lu steps, %lu resizes%s%s", (unsigned long)g_n, freed ? "freed by the game now; they held" : "holding", held,
              (unsigned long)g_n * 24, line, g_n > n ? ", ..." : "", (unsigned long)g_maxNeed, (unsigned long)g_maxSeed,
              (unsigned long)g_steps, (unsigned long)g_resizes, g_fails ? ", allocation failures" : "",
              g_overflows ? ", AN OVERFLOW" : "");
}
