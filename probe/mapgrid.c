// Map detail grids sized for the loaded map ([probe] MapGridReserve, default 1; 0 = off).
//
// At startup the exe reserves room for two tiled grids that cover the largest map it supports
// (exe+0x76018 -> exe+0x24c620 on the object at exe+0x991270): 16 x X x (Y + 4) tiles of 512
// bytes and as many of 256 bytes, with X x Y = 152 x 96 (hard-coded fallbacks: the Defines rows
// MAX_SUPPORTED_MAP_X / _Y it asks for don't exist): 118.75 + 59.4 = 178 MB in two blocks,
// whatever map is played later. The grids are 16 x 16 fine cells per tile (uint16 and byte
// values) behind a tile directory; a tile whose cells are all equal lives in the directory. When
// a map loads (exe+0x47e4e9 / +0x47e628 -> Init), Init gets that map's tile count and grows the
// reserve itself if it is larger (exe+0x5d73a0 / +0x6cfe10, the only other callers), then gives
// every tile a block. So the startup reserve only matters for maps up to 152 x 96, and a smaller
// map leaves the rest unused: a 97 x 66 Huge Communitu map uses 108,640 of the 243,200 tiles
// (measured in the game, 2026-10-30), ~99 MB never touched.
//
// With MapGridReserve=1 the sampler thread waits for that startup reserve (both vectors with a
// capacity and no tiles, unchanged for STABLE_MS, 20 s: the engine's startup is over and the
// main menu is up) and gives both blocks back with the game's own
// aligned delete (exe+0x413cf0, as Init does with the old block), leaving the vectors empty, so
// the map load reserves exactly what the map needs. Nothing reads the vectors before a map has
// tiles. If a map already has tiles, it leaves them alone. The vectors are emptied with
// compare-exchanges (see take()), so a map load that starts at that moment reserves anew; an Init
// that read the old capacity a few instructions before is the window left. No game code is
// changed and no import
// is hooked: hooking the exe's Database::Results imports to change the reserve (an earlier
// version) went with a crash in the game's message dispatcher while loading a save, the same
// crash as an earlier GetProcAddress import hook. The DX9 and tablet exes have the same grids
// (exes.c; code checked).
//
// mapgrid_report() logs both grids' tiles in use and reserved, with or without the option.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

// GameExe: gridReserve (DX11 exe+0x76018: mov ecx, grids; call the reserve), gridReserveFn
// (exe+0x24c620: reserves both vectors), grids (exe+0x991270), gridFree (exe+0x413cf0: the
// exe's aligned delete, FSBADeallocate or else _aligned_free; the DX9 exe's has FSBADeallocate
// inlined). The free is checked to be the one the vectors' own reserve frees their old block with.

typedef void(__cdecl *Free_t)(void *p);
static BYTE *g_grids;
static Free_t g_free;
static int g_state;              // 0 off, 1 waiting for the startup reserve, 2 done
static DWORD g_seen[4];          // the previous tick's data and capacity of both vectors
static DWORD g_since;            // GetTickCount when they last changed (0: not yet seen)
#define STABLE_MS 20000          // wall time, whatever IntervalMs is

const char *mapgrid_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (!g->grids || g->grids + 0x100 > nt->OptionalHeader.SizeOfImage) return "addresses";
    const BYTE *call = exe + g->gridReserve;  // B9 grids; E8 rel32
    if (call[0] != 0xB9 || *(BYTE *const *)(call + 1) != exe + g->grids || call[5] != 0xE8 ||
        exe_branch_target(exe, call + 5) != exe + g->gridReserveFn)
        return "reserve call";
    // The reserve: push esi; mov esi,[esp+8]; push edi; mov edi,ecx; lea ecx,[edi+28h]; mov [edi+9Ch],esi;
    // cmp esi,[ecx+8]; jbe; push esi; call (512-byte tiles) ... cmp esi,[edi+70h]; lea ecx,[edi+68h]; jbe; push esi; call
    static const BYTE kReserve[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9, 0x8D, 0x4F, 0x28, 0x89, 0xB7,
                                    0x9C, 0x00, 0x00, 0x00, 0x3B, 0x71, 0x08, 0x76, 0x06, 0x56, 0xE8};
    static const BYTE kReserve2[] = {0x3B, 0x77, 0x70, 0x8D, 0x4F, 0x68, 0x76, 0x06, 0x56, 0xE8};
    const BYTE *r = exe + g->gridReserveFn;
    if (memcmp(r, kReserve, sizeof kReserve) || memcmp(r + 0x1c, kReserve2, sizeof kReserve2)) return "reserve";
    // The vector's reserve frees its old block with the exe's aligned delete (cdecl).
    const BYTE *v = exe_branch_target(exe, r + 0x17);
    if (!v || v[0x5e] != 0xE8 || exe_branch_target(exe, v + 0x5e) != exe + g->gridFree || v[0x63] != 0x83 || v[0x64] != 0xC4 ||
        v[0x65] != 0x04)
        return "free";
    return NULL;
}

void mapgrid_init(const WCHAR *ini)
{
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    const char *why = g ? mapgrid_check(exe, g) : "not a known build of the game exe";
    if (why) {
        log_event("mapgrid: the map detail grids aren't the known code (%s); left alone", why);
        return;
    }
    g_grids = exe + g->grids;  // reported either way
    if (!GetPrivateProfileIntW(L"probe", L"MapGridReserve", 1, ini)) return;
    g_free = (Free_t)(exe + g->gridFree);
    g_state = 1;
    log_event("mapgrid: the startup reserve of the map detail grids (178 MB, for a 152 x 96 map) will be given back "
              "before a map loads, which then reserves what it needs; no game code changed");
}

// FFastVector: data, size, capacity (elements).
#define VEC1 (g_grids + 0x28)  // 512-byte tiles
#define VEC2 (g_grids + 0x68)  // 256-byte tiles
static DWORD *vec(BYTE *v) { return (DWORD *)v; }

// Empties a vector holding the startup reserve; 1 if its block is then ours to free. The capacity
// goes first: a map load that starts now finds no room and reserves anew, and a reserve of its own
// that replaced the block (and freed it) makes the data compare-exchange fail.
static int take(DWORD *v, DWORD data, DWORD cap)
{
    volatile LONG *w = (volatile LONG *)v;
    if (InterlockedCompareExchange(&w[2], 0, (LONG)cap) != (LONG)cap) return 0;
    return InterlockedCompareExchange(&w[0], 0, (LONG)data) == (LONG)data;
}

// Sampler thread.
void mapgrid_tick(void)
{
    if (g_state != 1) return;
    DWORD *a = vec(VEC1), *b = vec(VEC2);
    DWORD now[4] = {a[0], a[2], b[0], b[2]};
    if (a[1] || b[1]) {
        g_state = 2;
        log_event("mapgrid: the map detail grids already hold %lu + %lu tiles; their reserve is left alone",
                  (unsigned long)a[1], (unsigned long)b[1]);
        return;
    }
    // Wait for both blocks, then for STABLE_MS with nothing changed (a reserve in progress sets
    // the capacity before the data pointer).
    int same = now[0] && now[1] && now[2] && now[3] && !memcmp(now, g_seen, sizeof now);
    memcpy(g_seen, now, sizeof now);
    DWORD t = GetTickCount() | 1;
    if (!same) g_since = t;
    if (!same || t - g_since < STABLE_MS) return;
    g_state = 2;
    double mb = (now[1] * 512.0 + now[3] * 256.0) / 1048576;
    int ta = take(a, now[0], now[1]), tb = take(b, now[2], now[3]);
    if (ta) g_free((void *)(ULONG_PTR)now[0]);
    if (tb) g_free((void *)(ULONG_PTR)now[2]);
    if (ta && tb)
        log_event("mapgrid: gave back the startup reserve of the map detail grids (%lu + %lu tiles, %.1f MB); the map "
                  "load reserves what it needs", (unsigned long)now[1], (unsigned long)now[3], mb);
    else
        log_event("mapgrid: a map load started as the startup reserve was being given back; gave back %s",
                  ta ? "the first grid's" : tb ? "the second grid's" : "neither");
}

static void grid_line(const BYTE *v, DWORD bytes, char *out, int cap)
{
    DWORD n = *(const DWORD *)(v + 4), c = *(const DWORD *)(v + 8);
    snprintf(out, cap, "%lu of %lu %lu-byte tiles (%.1f of %.1f MB)", (unsigned long)n, (unsigned long)c,
             (unsigned long)bytes, n * (double)bytes / 1048576, c * (double)bytes / 1048576);
}

void mapgrid_report(void)
{
    if (!g_grids) return;
    char a[96], b[96];
    grid_line(VEC1, 512, a, sizeof a);
    grid_line(VEC2, 256, b, sizeof b);
    log_event("mapgrid: %s, %s in use", a, b);
}
