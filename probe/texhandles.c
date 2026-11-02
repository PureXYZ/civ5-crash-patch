// More texture handles than the game's 12,288 ([probe] TextureHandles, default 16384; 0 = the game's).
//
// Every texture the engine makes (UI atlases, icons, fonts, terrain tiles, leader scenes) takes a
// handle from one static pool (DX11 exe+0x3d3e2c0, 0x328c0 bytes):
//   +0x0      spinlock (InterlockedCompareExchange)
//   +0x40     the next new handle (handle 0 is the null texture made at exe+0x4c7940)
//   +0x44     refcount[0x3000]
//   +0xc044   frame index (0..3); +0xc048 four release queues (0x810 bytes each: FFastVector of
//             handles with 0x200 inline)
//   +0xe088   item[0x3000], 12 bytes: the resource, its shader view and one more pointer
//   +0x32088  handles past their four frames (another such vector); +0x32898 the free list (a deque)
// and each handle has a 0x398-byte record (description, size, render-target views) in a static
// array right after the pool (exe+0x3d70b80). Alloc (exe+0x4c5cb0) hands out handles up to 0x2ffe,
// then 0 while the free list is empty, and a texture made with handle 0 is the null texture: black
// or magenta icons and "Error loading font texture". The game frees few textures (it keeps every
// icon atlas it has loaded), so a big mod set on a Huge map fills the table: 12,287 at turn ~132
// (Sapiens + Gaia, TerrainDetailLevel 3, where terrain alone takes ~6,480 handles).
//
// With TextureHandles=N (up to 16,384) the table holds N handles:
// - The records move to a block of our own (N x 0x398 bytes, 14.4 MB at 16,384), and the pool
//   grows in place into the space they leave. The refcounts stay at +0x44 and get N entries;
//   what follows them moves up by 4 x (N - 0x3000) (frame index, release queues, items) or
//   16 x (N - 0x3000) (the vector and the free list after the items).
// - Every absolute reference to those parts is found through the exe's base relocations: 50 into
//   the pool past the refcounts and 52 into the records, all in the texture code; each moves by
//   the same amount. The pool's own functions (alloc, release, the per-frame flush, recycle,
//   constructor, destructor; exes.c) reach it through `this`: their 42 offsets, the 0x3000 cap and
//   the constructor's two memset sizes are rewritten. The refcounts don't move, so the AddRef
//   shared with four other pools (exe+0x306290) and the inline refcount updates elsewhere in the
//   exe are unchanged.
// - The old records' space past the grown pool is made inaccessible, so a reference this missed
//   faults at once (the exception is logged with its address) instead of using stale memory.
// It all happens in DllMain, before the exe's static constructors build the pool; if the pool is
// already built, or any value or count differs, nothing is changed. 16,384 is the most: handles
// are stored as 16-bit values (the bind lists, exe+0x4bd160), and up to 0x3fff they also stay in
// the 14 bits the game's own 0x3000 needs, in case anything packs them. DX11 only so far. In game
// (Sapiens + Gaia, Huge, TerrainDetailLevel 3, GPU texture decode, HD strategic view) a save
// loaded with 12,991 handles, 703 past the game's table, and the icons were right.
//
// With or without the option, the sampler logs when the table passes 90% and when it is full, and
// texhandles_report() logs how many handles are in use.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define OLD_N 0x3000        // the game's handle count
#define MAX_N 0x4000
#define REC 0x398           // bytes per record
#define NEXT 0x40           // pool offsets: the next new handle,
#define REFS_END 0xc044     // the end of the refcounts,
#define ITEMS_END 0x32088   // the end of the items,
#define FREE_COUNT 0x328b8  // the free list's size,
#define POOL_SIZE 0x328c0   // the end of the pool
#define MAX_SITES 256

static BYTE *g_pool;
static DWORD g_n = OLD_N, g_freeCount = FREE_COUNT;
static int g_said90, g_saidFull;

// The offset of pool offset off (REFS_END and up) in a pool with `extra` more handles.
static DWORD moved(DWORD off, DWORD extra) { return off + (off < ITEMS_END ? 4 : 16) * extra; }

// The base relocations whose target is in the pool past its refcounts or in the records: how
// many are in .text (*pool, *rec, and their addresses if sites) and how many elsewhere (*other).
static void find_fixups(BYTE *exe, const GameExe *g, BYTE **sites, DWORD *pool, DWORD *rec, DWORD *other)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    DWORD lo = 0, hi = 0, base = (DWORD)(ULONG_PTR)exe, n = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".text", 6)) lo = s[i].VirtualAddress, hi = lo + s[i].Misc.VirtualSize;
    *pool = *rec = *other = 0;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for (DWORD at = 0; at + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size;) {
        const IMAGE_BASE_RELOCATION *b = (const IMAGE_BASE_RELOCATION *)(exe + dir.VirtualAddress + at);
        if (b->SizeOfBlock < sizeof *b || at + b->SizeOfBlock > dir.Size) break;
        const WORD *e = (const WORD *)(b + 1);
        for (DWORD i = 0; i < (b->SizeOfBlock - sizeof *b) / 2; i++) {
            if (e[i] >> 12 != IMAGE_REL_BASED_HIGHLOW) continue;
            DWORD rva = b->VirtualAddress + (e[i] & 0xfff);
            if (rva + 4 > nt->OptionalHeader.SizeOfImage) continue;
            DWORD t = *(const DWORD *)(exe + rva) - base;
            int inPool = t >= g->texPool + REFS_END && t < g->texRecords;
            if (!inPool && (t < g->texRecords || t >= g->texRecords + OLD_N * REC)) continue;
            if (rva < lo || rva + 4 > hi) {
                (*other)++;
                continue;
            }
            if (sites && n < MAX_SITES) sites[n] = exe + rva;
            n++;
            if (inPool) (*pool)++;
            else (*rec)++;
        }
        at += b->SizeOfBlock;
    }
}

const char *texhandles_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (!g->texPool || !g->texSites) return "not mapped for this exe";
    if (g->texRecords != g->texPool + POOL_SIZE || g->texRecords + OLD_N * REC > nt->OptionalHeader.SizeOfImage)
        return "addresses";
    for (DWORD i = 0; i < g->texSiteCount; i++) {
        const TexSite *s = &g->texSites[i];
        if (s->rva + 4 > nt->OptionalHeader.SizeOfImage || *(const DWORD *)(exe + s->rva) != s->value ||
            (s->kind == TH_OFF && (s->value < REFS_END || s->value >= POOL_SIZE)))
            return "pool functions";
    }
    DWORD pool, rec, other;
    find_fixups(exe, g, NULL, &pool, &rec, &other);
    if (pool != g->texPoolFixups || rec != g->texRecFixups || other) return "references";
    return NULL;
}

const char *texhandles_apply(BYTE *exe, const GameExe *g, DWORD n, BYTE *records)
{
    static BYTE *fix[MAX_SITES], *at[MAX_SITES + 64];
    static DWORD to[MAX_SITES + 64], prot[MAX_SITES + 64];
    DWORD extra = n - OLD_N, base = (DWORD)(ULONG_PTR)exe, pool, rec, other;
    if (n <= OLD_N || n > MAX_N) return "handle count";
    find_fixups(exe, g, fix, &pool, &rec, &other);
    if (pool != g->texPoolFixups || rec != g->texRecFixups || other || pool + rec > MAX_SITES ||
        g->texSiteCount > 64)
        return "references";
    int k = 0;
    for (DWORD i = 0; i < pool + rec; i++) {
        DWORD t = *(const DWORD *)fix[i] - base;
        at[k] = fix[i];
        to[k++] = t < g->texRecords ? base + g->texPool + moved(t - g->texPool, extra)
                                    : (DWORD)(ULONG_PTR)records + (t - g->texRecords);
    }
    for (DWORD i = 0; i < g->texSiteCount; i++) {
        const TexSite *s = &g->texSites[i];
        if (*(const DWORD *)(exe + s->rva) != s->value) return "pool functions";
        at[k] = exe + s->rva;
        to[k++] = s->kind == TH_OFF ? moved(s->value, extra) : s->kind == TH_COUNT ? n
                : s->kind == TH_REFBYTES ? 4 * n : 12 * n;
    }
    // All or nothing: every place writable first. Restoring in reverse order undoes the stack of
    // protections where two places share a page.
    int w = 0;
    while (w < k && VirtualProtect(at[w], 4, PAGE_EXECUTE_READWRITE, &prot[w])) w++;
    if (w == k)
        for (int i = 0; i < k; i++) memcpy(at[i], &to[i], 4);
    for (int i = w - 1; i >= 0; i--) VirtualProtect(at[i], 4, prot[i], &prot[i]);
    if (w < k) return "VirtualProtect";
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    // The old records past the grown pool: a missed reference faults there.
    ULONG_PTR lo = ((ULONG_PTR)exe + g->texPool + moved(POOL_SIZE, extra) + 0xfff) & ~(ULONG_PTR)0xfff;
    ULONG_PTR hi = ((ULONG_PTR)exe + g->texRecords + OLD_N * REC) & ~(ULONG_PTR)0xfff;
    DWORD old;
    if (hi > lo) VirtualProtect((void *)lo, hi - lo, PAGE_NOACCESS, &old);
    return NULL;
}

void texhandles_init(const WCHAR *ini)
{
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    DWORD n = GetPrivateProfileIntW(L"probe", L"TextureHandles", MAX_N, ini);
    if (!g || !g->texPool) {
        if (n) log_event("texhandles: not mapped for this exe; the game's 12,288 texture handles");
        return;
    }
    g_pool = exe + g->texPool;  // reported either way
    if (!n || n == OLD_N) return;
    if (n < OLD_N) {
        log_event("texhandles: TextureHandles=%lu is less than the game's 12,288; left alone", (unsigned long)n);
        return;
    }
    if (n > MAX_N) n = MAX_N;
    const char *why = texhandles_check(exe, g);
    if (!why && (*(volatile DWORD *)g_pool || *(volatile DWORD *)(g_pool + NEXT))) why = "the pool is already built";
    BYTE *rec = NULL;
    if (!why && !(rec = VirtualAlloc(NULL, n * REC, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
        why = "no room for the records";
    if (!why) why = texhandles_apply(exe, g, n, rec);
    if (why) {
        if (rec) VirtualFree(rec, 0, MEM_RELEASE);
        log_event("texhandles: the texture handle pool isn't the known code (%s); the game's 12,288 handles", why);
        return;
    }
    g_n = n;
    g_freeCount = moved(FREE_COUNT, n - OLD_N);
    log_event("texhandles: the texture table holds %lu handles (the game's 12,288); records moved to %p (%.1f MB), "
              "%lu references and %lu pool offsets changed", (unsigned long)n, (void *)rec, n * (double)REC / 1048576,
              (unsigned long)(g->texPoolFixups + g->texRecFixups), (unsigned long)g->texSiteCount);
}

// Handles handed out and not on the free list (handle 0 included); 0 before the pool is built.
static DWORD in_use(DWORD *next, DWORD *free)
{
    *next = *(volatile DWORD *)(g_pool + NEXT);
    *free = *(volatile DWORD *)(g_pool + g_freeCount);
    if (*next > g_n || *free > *next) return 0;
    return *next - *free;
}

// Sampler thread.
void texhandles_tick(void)
{
    if (!g_pool) return;
    DWORD next, free, used = in_use(&next, &free);
    if (!g_said90 && used >= g_n / 10 * 9) {
        g_said90 = 1;
        log_event("texhandles: %lu of %lu texture handles in use (90%%)", (unsigned long)used, (unsigned long)g_n);
    }
    // Alloc stops when the counter would reach the count: handles 1..count-2.
    if (!g_saidFull && next >= g_n - 1 && !free) {
        g_saidFull = 1;
        log_event("texhandles: the texture table is full (%lu handles): textures made from now on are blank (black or "
                  "magenta icons, \"Error loading font texture\")%s", (unsigned long)g_n,
                  g_n < MAX_N ? "; TextureHandles=16384 makes room for more" : "");
    }
}

void texhandles_report(void)
{
    if (!g_pool) return;
    DWORD next, free, used = in_use(&next, &free);
    log_event("texhandles: %lu of %lu texture handles in use (%lu freed for reuse)", (unsigned long)used,
              (unsigned long)g_n, (unsigned long)free);
}
