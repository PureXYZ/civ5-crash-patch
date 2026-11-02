// fluidpool test: FluidPools' changes in the installed exes mapped as images (imports not bound).
// fluidpool_check passes on the DX11 exe's code (the other two aren't mapped: their sites are
// CEG-checked); fluidpool_apply sends the seeding's first instruction and the step launch call to
// the stubs, decoded back here, and the check then fails.
// Every pool block comes from a stand-in malloc that ends it at a no-access page, so a write past
// any pool faults. Then, on a stand-in particle object (4 pools of the game's 24 MB, a 100 x 60
// grid):
// - The game's own seeding function, run from the mapped exe, writes 200,000 particles (more
//   than the game's 196,608): pool 0 grows past its end once, every particle arrives intact; the
//   first one swaps the other pools for 1 MB blocks that keep their stale entries.
// - The step stub with a stand-in launcher that puts every particle of the range into one pool
//   (the worst a job split can do): every pool is sized to 4 x the range's blocks first, the
//   seed-grown pool shrinks, a smaller range shrinks them only after 30 s with nothing larger,
//   a larger one grows them at once, a range past the grid is clamped, and pools still the
//   game's on another object are all replaced at its first step.
// - With no room for a bigger pool, the old one stays and the step's jobs are skipped (the
//   launcher, which would write past it, isn't called), logged once; with room again it grows.
// - The seed stub keeps every register but eax; the step stub keeps ebx, esi, edi and ebp.
// Skipped when the game isn't installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

#define ORIG 0x30000
#define ENTRY 128
#define NPOOLS 4
#define W 100
#define H 60

static char g_log[8192];
void log_event(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", line);
    strncat(g_log, line, sizeof g_log - strlen(g_log) - 2);
    strcat(g_log, "\n");
}

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// Blocks that end at a no-access page.
#define MAX_BLOCKS 64
static struct { BYTE *base, *p; size_t size; } g_blocks[MAX_BLOCKS];
static int g_live, g_allocs, g_frees, g_badFrees, g_noRoom;
static void *__cdecl test_alloc(size_t size)
{
    if (g_noRoom) return NULL;
    size_t span = (size + 4095) & ~(size_t)4095;
    BYTE *base = VirtualAlloc(NULL, span + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    DWORD old;
    VirtualProtect(base + span, 4096, PAGE_NOACCESS, &old);
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (!g_blocks[i].base) {
            g_blocks[i].base = base, g_blocks[i].p = base + span - size, g_blocks[i].size = size;
            g_live++, g_allocs++;
            return g_blocks[i].p;
        }
    return NULL;
}
static void __cdecl test_delete(void *p)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (g_blocks[i].p == p && g_blocks[i].base) {
            VirtualFree(g_blocks[i].base, 0, MEM_RELEASE);
            g_blocks[i].base = NULL;
            g_live--, g_frees++;
            return;
        }
    g_badFrees++;
}
static size_t block_size(const void *p)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (g_blocks[i].base && g_blocks[i].p == p) return g_blocks[i].size;
    return 0;
}

static DWORD g_now = 1000;
static DWORD WINAPI test_clock(void) { return g_now; }
extern DWORD(WINAPI *g_fluidClock)(void);

// The stand-in object and grid.
static BYTE *g_objs[2];
static DWORD g_blocksOf[W * H * 8];
static BYTE g_grid[0x44];
static BYTE **slots(BYTE *obj) { return (BYTE **)(obj + 0x40); }
static DWORD *counts(BYTE *obj) { return (DWORD *)(obj + 0x440); }
static BYTE *new_obj(void)
{
    BYTE *obj = calloc(1, 0x1000);
    obj[0x909] = 1;
    *(DWORD *)(obj + 0x90c) = NPOOLS;
    *(BYTE **)(obj + 0x904) = g_grid;
    for (int i = 0; i < NPOOLS; i++) slots(obj)[i] = test_alloc((size_t)ORIG * ENTRY);  // the game's
    return obj;
}

// The step launcher: every particle of the range's blocks into pool g_thread, as a job split that
// gives one thread all of it would (4 a block, the base read again for each block).
static int g_thread, g_launches;
static DWORD g_wrote;
static void __cdecl fake_launch(const LONG *range, BYTE *const *body, void *flag)
{
    (void)flag;
    BYTE *obj = body[0];
    g_launches++;
    g_wrote = 0;
    LONG y1 = range[0] > H ? H : range[0], x1 = range[3] > W ? W : range[3];
    for (LONG y = range[1]; y < y1; y++)
        for (LONG x = range[4]; x < x1; x++)
            for (DWORD b = 0; b < g_blocksOf[8 * (y * W + x)]; b++)
                for (int lane = 0; lane < 4; lane++) {
                    BYTE *e = slots(obj)[g_thread] + (size_t)counts(obj)[g_thread] * ENTRY;
                    *(DWORD *)e = (DWORD)(y * W + x);
                    *(DWORD *)(e + 0x7c) = lane;
                    counts(obj)[g_thread]++;
                    g_wrote++;
                }
}

// The update's call: counts zeroed, then call <launcher>(range, body, flag) - here the stub.
static void substep(BYTE *obj, LONG x0, LONG y0, LONG x1, LONG y1, int thread)
{
    for (int i = 0; i < NPOOLS; i++) counts(obj)[i] = 0;
    LONG range[6] = {y1, y0, 1, x1, x0, 1};
    BYTE *body[5] = {obj};
    BYTE flag = 0;
    g_thread = thread;
    typedef void(__cdecl * step_t)(const LONG *, BYTE *const *, void *);
    ((step_t)fluidpool_step_stub())(range, body, &flag);
}

static DWORD range_need(LONG x0, LONG y0, LONG x1, LONG y1)
{
    DWORD n = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (LONG y = y0; y < y1; y++)
        for (LONG x = x0; x < x1; x++) n += g_blocksOf[8 * (y * W + x)];
    return 4 * n;
}
static DWORD size_for(DWORD need) { return (need + need / 4 + 4096 + 8191) / 8192 * 8192; }
static DWORD cap(BYTE *obj, int i) { return (DWORD)(block_size(slots(obj)[i]) / ENTRY); }

void *g_args[4];  // the register test's (globals: it changes ebp and esp)
DWORD g_kept[5];

static const WCHAR *const kDir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V\\";

// fluidpool_check on an installed exe gives `want` (NULL: passes); -1 if it isn't there.
static int check_exe(const WCHAR *name, const char *want)
{
    WCHAR path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls%ls", kDir, name);
    HMODULE m = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) return -1;
    const GameExe *g = game_exe((BYTE *)m);
    const char *why = g ? fluidpool_check((BYTE *)m, g) : "not a known build";
    CHECK(want ? why && !strcmp(why, want) : !why, "%ls: fluidpool_check: %s", name, why ? why : "passes");
    FreeLibrary(m);
    return 1;
}

int main(void)
{
    if (check_exe(L"CivilizationV_DX11.exe", NULL) < 0) {
        printf("fluidpool test: skipped (game not installed here)\n");
        return 0;
    }
    check_exe(L"CivilizationV.exe", "not mapped for this exe");
    check_exe(L"CivilizationV_Tablet.exe", "not mapped for this exe");

    WCHAR path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsCivilizationV_DX11.exe", kDir);
    BYTE *exe = (BYTE *)LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    const GameExe *g = game_exe(exe);
    g_fluidClock = test_clock;
    const char *why = fluidpool_apply(exe, g, test_alloc, test_delete, (void *)fake_launch);
    CHECK(!why, "fluidpool_apply: %s", why ? why : "");
    if (why) return 1;
    CHECK(fluidpool_check(exe, g) != NULL, "the check still passes after the change");
    const BYTE *p = exe + g->fluidSeed;
    CHECK(p[0] == 0xE8 && p + 5 + *(const LONG *)(p + 1) == (BYTE *)fluidpool_seed_stub() && p[5] == 0x90,
          "seeding not a call to its stub");
    p = exe + g->fluidStepCall;
    CHECK(p[0] == 0xE8 && p + 5 + *(const LONG *)(p + 1) == (BYTE *)fluidpool_step_stub(), "step launch not a call to its stub");

    // The grid: 0-8 blocks a cell.
    *(LONG *)(g_grid + 0x10) = W;
    *(LONG *)(g_grid + 0x14) = H;
    *(DWORD **)(g_grid + 0x3c) = g_blocksOf;
    srand(7);
    for (int c = 0; c < W * H; c++) g_blocksOf[8 * c] = rand() % 9;
    DWORD all = range_need(0, 0, W, H);
    printf("grid: %lu particles at most\n", (unsigned long)all);

    // Seeding, with stale entries in pools 1 and 3.
    BYTE *obj = g_objs[0] = new_obj();
    BYTE *game[NPOOLS];
    memcpy(game, slots(obj), sizeof game);
    counts(obj)[1] = 5, counts(obj)[3] = 7;
    for (DWORD k = 0; k < 7; k++) {
        *(DWORD *)(slots(obj)[1] + k * ENTRY) = 0x1000 + k;
        *(DWORD *)(slots(obj)[3] + k * ENTRY) = 0x3000 + k;
    }
    typedef void(__attribute__((thiscall)) * seed_t)(BYTE *, const DWORD *, const DWORD *, float, float, DWORD);
    seed_t seed = (seed_t)(exe + g->fluidSeed);
    const DWORD SEEDS = 200000;
    for (DWORD k = 0; k < SEEDS; k++) {
        DWORD pos[3] = {k, k + 1, k + 2}, vel[2] = {~k, k ^ 0x5555};
        seed(obj, pos, vel, 1.0f, 2.0f, k * 3);
    }
    int bad = 0;
    BYTE *p0 = slots(obj)[0];
    for (DWORD k = 0; k < SEEDS && p0; k++) {
        const DWORD *e = (const DWORD *)(p0 + (size_t)k * ENTRY);
        bad += e[0] != k || e[1] != k + 1 || e[2] != k + 2 || e[3] != ~k || e[4] != (k ^ 0x5555) || e[9] != k * 3;
    }
    CHECK(counts(obj)[0] == SEEDS && !bad && cap(obj, 0) >= SEEDS && slots(obj)[0] != game[0],
          "seeding: count %lu, %d wrong, pool 0 holds %lu", (unsigned long)counts(obj)[0], bad, (unsigned long)cap(obj, 0));
    CHECK(cap(obj, 1) == 8192 && cap(obj, 2) == 8192 && cap(obj, 3) == 8192 && slots(obj)[1] != game[1] &&
          slots(obj)[3] != game[3] && *(DWORD *)(slots(obj)[1] + 4 * ENTRY) == 0x1004 &&
          *(DWORD *)(slots(obj)[3] + 6 * ENTRY) == 0x3006 && counts(obj)[1] == 5,
          "the other pools at the first seed: %lu %lu %lu", (unsigned long)cap(obj, 1), (unsigned long)cap(obj, 2),
          (unsigned long)cap(obj, 3));
    CHECK(g_live == NPOOLS && !g_badFrees, "after seeding: %d blocks live, %d bad frees", g_live, g_badFrees);

    // A whole-grid step, all of it into pool 2.
    substep(obj, 0, 0, W, H, 2);
    DWORD want = size_for(all);
    CHECK(g_launches == 1 && g_wrote == all && counts(obj)[2] == all, "whole grid: wrote %lu of %lu", (unsigned long)g_wrote,
          (unsigned long)all);
    for (int i = 0; i < NPOOLS; i++) CHECK(cap(obj, i) == want, "whole grid: pool %d holds %lu, not %lu", i, (unsigned long)cap(obj, i), (unsigned long)want);
    // A small range: kept for 30 s, then shrunk once nothing larger came.
    DWORD smallNeed = range_need(10, 10, 20, 20);
    substep(obj, 10, 10, 20, 20, 0);
    CHECK(cap(obj, 0) == want && g_wrote == smallNeed, "small range shrank at once");
    g_now += 31000;
    substep(obj, 10, 10, 20, 20, 1);
    CHECK(cap(obj, 1) == want, "shrank with the larger need still in the window");
    g_now += 31000;
    substep(obj, 10, 10, 20, 20, 3);
    for (int i = 0; i < NPOOLS; i++)
        CHECK(cap(obj, i) == size_for(smallNeed), "after 30 s of small steps: pool %d holds %lu, not %lu", i,
              (unsigned long)cap(obj, i), (unsigned long)size_for(smallNeed));
    // Larger again, into pool 3 (grows at once), and a range past the grid (clamped).
    substep(obj, 0, 0, W + 50, H + 9, 3);
    CHECK(counts(obj)[3] == all && cap(obj, 3) == want, "past the grid: wrote %lu, pool 3 holds %lu", (unsigned long)counts(obj)[3],
          (unsigned long)cap(obj, 3));

    // Another object whose pools are still the game's: all replaced at its first step.
    BYTE *obj2 = g_objs[1] = new_obj();
    int frees = g_frees;
    substep(obj2, 0, 0, 30, 30, 1);
    DWORD need2 = range_need(0, 0, 30, 30);
    for (int i = 0; i < NPOOLS; i++)
        CHECK(cap(obj2, i) == size_for(need2), "the game's pools at the first step: pool %d holds %lu", i, (unsigned long)cap(obj2, i));
    CHECK(g_frees - frees == NPOOLS && counts(obj2)[1] == need2, "the game's pools: %d freed", g_frees - frees);

    // No room for a bigger pool: the old one stays and the jobs don't run (the launcher would write
    // past it into the no-access page), logged once over two such steps.
    g_noRoom = 1;
    BYTE *before = slots(obj2)[0];
    int launches = g_launches;
    g_log[0] = 0;
    substep(obj2, 0, 0, W, H, 0);
    substep(obj2, 0, 0, W, H, 0);
    const char *first = strstr(g_log, "is skipped");
    CHECK(slots(obj2)[0] == before && g_launches == launches && counts(obj2)[0] == 0 && first && !strstr(first + 1, "is skipped"),
          "no room: %d launches, %s", g_launches - launches, g_log);
    g_noRoom = 0;
    substep(obj2, 0, 0, W, H, 0);
    CHECK(g_launches == launches + 1 && counts(obj2)[0] == all && cap(obj2, 0) == want, "room again: pool 0 holds %lu",
          (unsigned long)cap(obj2, 0));

    // Registers. The seed stub: everything but eax, which gets the count.
    counts(obj)[0] = 1234;
    DWORD a = 0x11111111, b = 0x22222222, c = (DWORD)(ULONG_PTR)obj, d = 0x44444444, si = 0x55555555, di = 0x66666666;
    void *stub = fluidpool_seed_stub();
    __asm__ volatile("call *%[f]" : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(si), "+D"(di) : [f] "m"(stub) : "cc", "memory");
    CHECK(a == 1234 && b == 0x22222222 && c == (DWORD)(ULONG_PTR)obj && d == 0x44444444 && si == 0x55555555 && di == 0x66666666,
          "seed stub registers: %08lx %08lx %08lx %08lx %08lx %08lx", a, b, c, d, si, di);
    // The step stub: ebx, esi, edi, ebp (and the stack) kept. The asm changes ebp and esp, so
    // everything it reads or writes is a global.
    for (int i = 0; i < NPOOLS; i++) counts(obj)[i] = 0;
    static LONG range[6] = {20, 10, 1, 20, 10, 1};
    static BYTE *body[5];
    static BYTE flag;
    body[0] = obj;
    g_args[0] = range, g_args[1] = body, g_args[2] = &flag, g_args[3] = fluidpool_step_stub();
    __asm__ volatile("pushl %%ebp\n\t"
                     "movl %%esp, %%eax\n\t"
                     "movl $0x22222222, %%ebx\n\t"
                     "movl $0x55555555, %%esi\n\t"
                     "movl $0x66666666, %%edi\n\t"
                     "movl $0x77777777, %%ebp\n\t"
                     "pushl %%eax\n\t"
                     "pushl %[a2]\n\t"
                     "pushl %[a1]\n\t"
                     "pushl %[a0]\n\t"
                     "call *%[f]\n\t"
                     "addl $12, %%esp\n\t"
                     "popl %%eax\n\t"
                     "subl %%esp, %%eax\n\t"
                     "movl %%ebp, %%ecx\n\t"
                     "popl %%ebp\n\t"
                     "movl %%ebx, %[b]\n\t"
                     "movl %%esi, %[s]\n\t"
                     "movl %%edi, %[d]\n\t"
                     "movl %%ecx, %[p]\n\t"
                     "movl %%eax, %[e]\n\t"
                     : [b] "=m"(g_kept[0]), [s] "=m"(g_kept[1]), [d] "=m"(g_kept[2]), [p] "=m"(g_kept[3]), [e] "=m"(g_kept[4])
                     : [f] "m"(g_args[3]), [a0] "m"(g_args[0]), [a1] "m"(g_args[1]), [a2] "m"(g_args[2])
                     : "eax", "ebx", "ecx", "edx", "esi", "edi", "cc", "memory");
    DWORD keptB = g_kept[0], keptS = g_kept[1], keptD = g_kept[2], keptP = g_kept[3], espDiff = g_kept[4];
    CHECK(keptB == 0x22222222 && keptS == 0x55555555 && keptD == 0x66666666 && keptP == 0x77777777 && !espDiff,
          "step stub registers: ebx %08lx esi %08lx edi %08lx ebp %08lx, esp off by %ld", keptB, keptS, keptD, keptP, (long)espDiff);

    CHECK(!g_badFrees && g_live == 2 * NPOOLS, "end: %d blocks live (want %d), %d bad frees", g_live, 2 * NPOOLS, g_badFrees);
    CHECK(!strstr(g_log, "more than its"), "a pool overflowed: %s", g_log);
    fluidpool_report();
    printf("fluidpool test: %s (%d allocations, %d frees)\n", g_fail ? "FAILED" : "passed", g_allocs, g_frees);
    return g_fail ? 1 : 0;
}
