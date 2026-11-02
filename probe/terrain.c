// More terrain base textures ([probe] TerrainBaseSlots, default 0 = the game's own 6,144).
//
// The ground is drawn from terrain tiles (10,752 on a Sapiens + Gaia Huge map, 2,455 of them deep
// water, which is never textured). A tile is grey until it has its two base textures (tile +0x198 /
// +0x19c: BC1, 256 x 256 with 6 mips) and has been rendered into them. The textures come from a
// pool made once at startup (exe+0x2b8370 on the static terrain object exe+0x9165e0; handles in an
// array in the object at +0x6d888, room for 8,096, count at +0x62a04) and handed to each map's tile
// manager (exe+0x1bd079), which copies them into its free list (exe+0x23bcb0; an FFastVector that
// moves to the heap past 8,096). The settings code (exe+0x53f80) sets the count: 6,144 at
// TerrainDetailLevel 3, 4,096 below, so at most 3,072 tiles have textures. A tile that leaves the
// view keeps them (state 1: its mesh goes, the base textures stay valid) and is back within a frame
// or two. But the off-screen pass (exe+0x174f40) keeps 8 slots free by taking them from the end of
// its sorted list of off-screen tiles, and a tile without textures waits for the page-in: at most
// TerrainPageinSpeedStill (8) a frame, about a second for a screen of 500 tiles after a long scroll
// (measured live, session 20261101_215143, where 29% of the tiles had textures).
//
// The pool init can't be changed: it lies in code that Steam's CEG checksums (26 checks, each a
// `mov eax, fn; call eax; ret` stub whose function CRCs a table of code ranges; exe+0x2d00f0 covers
// exe+0x2b7000..0x2b9800), and the result of that check sizes per-thread arrays at map load
// (exe+0x25297 -> [exe+0x799474]). With the init patched the game crashed loading a save: job
// threads ran past those arrays (session 20261101_224159). So the extra textures are made later,
// at the first hand-over, by the same calls the init uses, and only code no check covers changes:
// - exe+0x1bd079 (`mov ecx,[esi+62A04h]`, the hand-over's count) calls a stub that, the first time,
//   copies the game's handles into our array, makes TerrainBaseSlots - count more textures with the
//   init's own description (exe+0x4c8fa0: BC1, the base resolution, its mips) and create call
//   (exe+0x4c6450 on the texture manager), stores the total in the object's count, and returns it.
// - exe+0x1bd086 (`lea edx,[esi+6D888h]`, the hand-over's array) gets `mov edx, <our array>`.
// - exe+0x2ccbfc (`lea ebx,[esi+6D888h]`, the release at exit) calls a stub that returns our array
//   (all of them are released) and starts over at the next hand-over, or the object's own array
//   if no map was loaded.
// Every texture takes a handle, so the texture table (texhandles.c) grows by TerrainBaseSlots -
// 6,144 too, which keeps the room the game's other textures have today. When the whole map fits (2
// slots per tile that isn't deep water), the game's own idle pre-render (one tile a frame when
// nothing else is being built) gives every tile its textures a few minutes after a load, and
// scrolling no longer shows grey. The cost per slot past 6,144 is address space: ~6 KB in the
// D3D11 runtime and driver (a test program's 15,000 such textures with views took 91 MB) plus its
// 920-byte handle record; in game, with 16,700 slots, 480 MB were free at 10 minutes instead of 589
// MB on the same save (session 20261101_225853 against 215143), ~10 KB per slot. And ~43 KB of
// video memory each (~730 MB for 16,700).
//
// terrain_report() logs the pool, how many tiles have base textures and the free count, and how
// many slots would hold the whole map, with or without the option.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

#define GAME_SLOTS 6144     // the game's count at TerrainDetailLevel 3, the most it sets
#define OFF_ARRAY 0x6d888   // terrain object: the handle array,
#define OFF_POOL 0x62a04    // the pool's count,
#define OFF_MGR 0x75fa4     // the tile manager
#define MGR_TILES 0x1ae0    // tile manager: the tiles,
#define MGR_COUNT 0x2318    // how many,
#define MGR_FREE 0x21e90    // base textures on the free list
#define TILE_BASE 0x198
#define TILE_HEIGHT 0x328   // 1 = deep water (never textured), 2 = shallow, 3 = land
#define HANDLE_RESERVE 512  // never take the texture table closer to full than this
// The base texture part of the pool init (its start is the GameExe's terrainInit):
#define INIT_RES 0x0d       // A1 <res>          mov eax,[base resolution]
#define INIT_FORMAT 0x54    // 6A 47             push 47h (BC1)
#define INIT_CTOR 0x5a      // E8 <ctor>         the description's constructor (9 arguments)
#define INIT_MGR 0x70       // 8B 0D <manager>   mov ecx,[texture manager]
#define INIT_CREATE 0x7b    // E8 <create>       create a texture from the description
#define INIT_DTOR 0x92      // E8 <dtor>         the description's destructor

typedef void(__thiscall *desc_ctor_t)(void *desc, DWORD format, DWORD a2, DWORD w, DWORD h, DWORD a5, DWORD mips,
                                      DWORD flags, DWORD a8, DWORD a9);
typedef DWORD(__thiscall *tex_create_t)(void *manager, void *desc);
typedef void(__thiscall *desc_dtor_t)(void *desc);

static BYTE *g_exe, *g_obj;
static const GameExe *g_g;
static DWORD g_want;            // TerrainBaseSlots (0 = off)
static DWORD *g_array;          // the game's handles, then ours
static volatile LONG g_made;    // our textures exist (since the last release)
static DWORD g_total;           // the total at the last hand-over
static DWORD (*g_makeExtra)(DWORD *out, DWORD n);

static const BYTE *call_target(const BYTE *p) { return p[0] == 0xE8 ? p + 5 + *(const LONG *)(p + 1) : NULL; }

// n more base textures exactly as the pool init makes them; how many were made.
static DWORD make_extra(DWORD *out, DWORD n)
{
    const BYTE *init = g_exe + g_g->terrainInit;
    DWORD res = **(DWORD *const *)(init + INIT_RES + 1);
    DWORD mips = res == 0x40 ? 4 : res == 0x80 ? 5 : res == 0x100 ? 6 : res == 0x200 ? 7 : 4;
    desc_ctor_t ctor = (desc_ctor_t)call_target(init + INIT_CTOR);
    tex_create_t create = (tex_create_t)call_target(init + INIT_CREATE);
    desc_dtor_t dtor = (desc_dtor_t)call_target(init + INIT_DTOR);
    void *manager = **(void **const *)(init + INIT_MGR + 2);
    __attribute__((aligned(16))) BYTE desc[0x400];  // the init's is 0x328 bytes
    memset(desc, 0, sizeof desc);
    ctor(desc, 0x47, 0, res, res, 0, mips, 0x80, 0, 0);
    DWORD made = 0;
    while (made < n && texhandles_room() > HANDLE_RESERVE) {
        DWORD h = create(manager, desc);
        if (!h) break;
        out[made++] = h;
    }
    dtor(desc);
    return made;
}

// exe+0x1bd079 (through handover_stub, ecx = the result): the count of base textures to hand to
// the new tile manager.
DWORD __cdecl terrain_handover(BYTE *obj) __asm__("terrain_handover");
DWORD __cdecl terrain_handover(BYTE *obj)
{
    if (g_made) return g_total;
    DWORD game = *(volatile DWORD *)(obj + OFF_POOL), total = game;
    if (game > g_want) game = total = g_want;  // can't happen (the game sets at most 6,144); never overrun
    memcpy(g_array, obj + OFF_ARRAY, game * 4);
    int on = **(DWORD *const *)(g_exe + g_g->terrainInit + 2) && game;  // base textures on (the init made them)
    if (on) total += g_makeExtra(g_array + game, g_want - game);
    g_total = total;
    *(volatile DWORD *)(obj + OFF_POOL) = total;  // what the release at exit loops over
    g_made = 1;
    log_event("terrain: %lu base textures for the map (%lu the game's, %lu more%s)", (unsigned long)total,
              (unsigned long)game, (unsigned long)(total - game),
              !on ? "; base textures are off" : total < g_want ? "; fewer than asked: the texture table is nearly full" : "");
    return total;
}

// exe+0x2ccbfc (through release_stub, ebx = the result): the array the release at exit walks.
DWORD *__cdecl terrain_release(BYTE *obj) __asm__("terrain_release");
DWORD *__cdecl terrain_release(BYTE *obj)
{
    if (!g_made) return (DWORD *)(obj + OFF_ARRAY);
    g_made = 0;  // all of them go now; a new pool starts over at its first hand-over
    return g_array;
}

// At both sites eax, ecx and edx are written before they are read again and the flags are dead;
// esi is the terrain object.
__attribute__((naked)) static void handover_stub(void)
{
    __asm__("pushl %esi\n\tcall terrain_handover\n\taddl $4, %esp\n\tmovl %eax, %ecx\n\tret");
}
__attribute__((naked)) static void release_stub(void)
{
    __asm__("pushl %esi\n\tcall terrain_release\n\taddl $4, %esp\n\tmovl %eax, %ebx\n\tret");
}

DWORD terrain_extra_handles(const WCHAR *ini)
{
    DWORD n = GetPrivateProfileIntW(L"probe", L"TerrainBaseSlots", 0, ini);
    if (n > TERRAIN_MAX_SLOTS) n = TERRAIN_MAX_SLOTS;
    return n > GAME_SLOTS ? n - GAME_SLOTS : 0;
}

static int bytes_at(BYTE *exe, DWORD size, DWORD rva, const BYTE *want, DWORD len)
{
    return rva && rva + len <= size && !memcmp(exe + rva, want, len);
}

const char *terrain_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!g->terrainObj) return "not mapped for this exe";
    if (g->terrainObj + OFF_MGR + 4 > size || g->terrainPoolCall + 10 > size || g->terrainInit + INIT_DTOR + 5 > size)
        return "addresses";
    // The pool init on the terrain object: mov ecx, obj; call init.
    const BYTE *c = exe + g->terrainPoolCall;
    if (c[0] != 0xB9 || *(BYTE *const *)(c + 1) != exe + g->terrainObj || c[5] != 0xE8) return "pool init call";
    // Its base texture part: cmp [on],0 / mov eax,[res] / push 47h / call ctor / mov ecx,[manager] /
    // call create / call dtor (only read, never changed: it's checksummed).
    const BYTE *i = exe + g->terrainInit;
    if (i[0] != 0x83 || i[1] != 0x3D || i[6] != 0 || i[INIT_RES] != 0xA1 || i[INIT_FORMAT] != 0x6A ||
        i[INIT_FORMAT + 1] != 0x47 || i[INIT_MGR] != 0x8B || i[INIT_MGR + 1] != 0x0D)
        return "pool init";
    const BYTE *t[3] = {call_target(i + INIT_CTOR), call_target(i + INIT_CREATE), call_target(i + INIT_DTOR)};
    for (int k = 0; k < 3; k++)
        if (!t[k] || t[k] < exe || t[k] >= exe + size) return "pool init calls";
    BYTE *on = *(BYTE *const *)(i + 2), *res = *(BYTE *const *)(i + INIT_RES + 1);
    if (on < exe || on + 4 > exe + size || res < exe || res + 4 > exe + size) return "pool init variables";
    // The hand-over: mov ecx,[esi+62A04h] / lea edx,[esi+6D888h]. The release at exit:
    // lea ebx,[esi+6D888h] / mov ecx,[ebx]; push ecx; mov ecx, <texture handle pool>; call.
    static const BYTE kCount[] = {0x8B, 0x8E, 0x04, 0x2A, 0x06, 0x00}, kArrayEdx[] = {0x8D, 0x96, 0x88, 0xD8, 0x06, 0x00},
                      kArrayEbx[] = {0x8D, 0x9E, 0x88, 0xD8, 0x06, 0x00, 0x8B, 0x0B, 0x51, 0xB9};
    if (!bytes_at(exe, size, g->terrainHandCount, kCount, sizeof kCount) ||
        !bytes_at(exe, size, g->terrainHandArray, kArrayEdx, sizeof kArrayEdx))
        return "hand-over";
    if (!bytes_at(exe, size, g->terrainRelease, kArrayEbx, sizeof kArrayEbx) ||
        *(BYTE *const *)(exe + g->terrainRelease + 10) != exe + g->texPool || exe[g->terrainRelease + 14] != 0xE8)
        return "release";
    // What the report reads: mov ecx,[esi+75FA4h] (the tile manager); the base texture allocator's
    // xor eax,eax; cmp [ecx+21E90h],eax (the free count).
    static const BYTE kMgr[] = {0x8B, 0x8E, 0xA4, 0x5F, 0x07, 0x00};
    static const BYTE kAlloc[] = {0x33, 0xC0, 0x39, 0x81, 0x90, 0x1E, 0x02, 0x00};
    if (!bytes_at(exe, size, g->terrainMgrLoad, kMgr, sizeof kMgr) || !bytes_at(exe, size, g->terrainAlloc, kAlloc, sizeof kAlloc))
        return "tile manager";
    return NULL;
}

const char *terrain_apply(BYTE *exe, const GameExe *g, DWORD slots, DWORD *array, DWORD (*makeExtra)(DWORD *, DWORD))
{
    BYTE *at[3] = {exe + g->terrainHandCount, exe + g->terrainHandArray, exe + g->terrainRelease};
    BYTE code[3][6];
    void *to[3] = {(void *)handover_stub, array, (void *)release_stub};
    for (int i = 0; i < 3; i++) {
        if (i == 1) {
            code[i][0] = 0xBA;  // mov edx, array
            memcpy(code[i] + 1, &to[i], 4);
        } else {
            LONG rel = (LONG)((BYTE *)to[i] - (at[i] + 5));
            code[i][0] = 0xE8;  // call stub
            memcpy(code[i] + 1, &rel, 4);
        }
        code[i][5] = 0x90;
    }
    g_exe = exe;
    g_g = g;
    g_want = slots;
    g_array = array;
    g_made = 0;
    g_makeExtra = makeExtra ? makeExtra : make_extra;
    // All or nothing, as in texhandles_apply.
    DWORD prot[3];
    int w = 0;
    while (w < 3 && VirtualProtect(at[w], 6, PAGE_EXECUTE_READWRITE, &prot[w])) w++;
    if (w == 3)
        for (int i = 0; i < 3; i++) memcpy(at[i], code[i], 6);
    for (int i = w - 1; i >= 0; i--) VirtualProtect(at[i], 6, prot[i], &prot[i]);
    if (w < 3) return "VirtualProtect";
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return NULL;
}

void *terrain_handover_stub(void) { return (void *)handover_stub; }  // for the test
void *terrain_release_stub(void) { return (void *)release_stub; }

void terrain_init(const WCHAR *ini)
{
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    DWORD want = GetPrivateProfileIntW(L"probe", L"TerrainBaseSlots", 0, ini);
    if (!g || !g->terrainObj) {
        if (want) log_event("terrain: not mapped for this exe; the game's own terrain textures");
        return;
    }
    const char *why = terrain_check(exe, g);
    if (why) {
        log_event("terrain: the terrain texture pool isn't the known code (%s); left alone", why);
        return;
    }
    g_obj = exe + g->terrainObj;  // reported either way
    if (!want) return;
    if (want <= GAME_SLOTS) {
        log_event("terrain: TerrainBaseSlots=%lu is not more than the game's 6,144; left alone", (unsigned long)want);
        return;
    }
    if (want > TERRAIN_MAX_SLOTS) want = TERRAIN_MAX_SLOTS;
    // The game's other textures keep the room they have with 6,144 slots in 16,384 handles.
    DWORD handles = texhandles_count(), room = handles > 16384 - GAME_SLOTS ? handles - (16384 - GAME_SLOTS) : 0;
    if (room <= GAME_SLOTS) {
        log_event("terrain: the texture table holds %lu handles, too few for more terrain textures (TextureHandles "
                  "must be on); left alone", (unsigned long)handles);
        return;
    }
    DWORD slots = want < room ? want : room;
    DWORD *array = VirtualAlloc(NULL, slots * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!array) {
        log_event("terrain: no room for the base texture array; left alone");
        return;
    }
    why = terrain_apply(exe, g, slots, array, NULL);
    if (why) {
        VirtualFree(array, 0, MEM_RELEASE);
        log_event("terrain: couldn't change the base texture hand-over (%s); left alone", why);
        return;
    }
    log_event("terrain: each map gets %lu base textures (%lu tiles; the game's own: 6,144 at TerrainDetailLevel 3), "
              "the rest made at the first map load%s", (unsigned long)slots, (unsigned long)slots / 2,
              slots < want ? "; fewer than asked: the texture table is too small" : "");
}

static int rd(const void *p, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

void terrain_report(void)
{
    if (!g_obj) return;
    LONG pool;
    BYTE *mgr = NULL, **tiles = NULL;
    DWORD n = 0, free = 0, based = 0, able = 0;
    if (!rd(g_obj + OFF_POOL, &pool, sizeof pool) || !pool) return;
    if (!rd(g_obj + OFF_MGR, &mgr, sizeof mgr) || !mgr || !rd(mgr + MGR_COUNT, &n, 4) || !rd(mgr + MGR_TILES, &tiles, 4) ||
        !rd(mgr + MGR_FREE, &free, 4) || n > 1000000) {
        log_event("terrain: base texture pool %ld slots, no map", pool);
        return;
    }
    for (DWORD i = 0; i < n; i++) {
        BYTE *tile;
        BYTE t[TILE_HEIGHT + 4 - TILE_BASE];
        if (!rd(tiles + i, &tile, 4) || !tile || !rd(tile + TILE_BASE, t, sizeof t)) continue;
        if (*(DWORD *)t) based++;
        if (*(DWORD *)(t + TILE_HEIGHT - TILE_BASE) != 1) able++;
    }
    log_event("terrain: %lu tiles, %lu not deep water, %lu with base textures (%.0f%%); pool %ld slots, %lu free%s",
              (unsigned long)n, (unsigned long)able, (unsigned long)based, able ? 100.0 * based / able : 0.0, pool,
              (unsigned long)free, (DWORD)pool < 2 * able ? "" : " (the whole map fits)");
    if ((DWORD)pool < 2 * able)
        log_event("terrain: TerrainBaseSlots=%lu would hold every tile of this map%s", (unsigned long)(2 * able + 64),
                  2 * able + 64 > TERRAIN_MAX_SLOTS ? " (more than the most, 22,528)" : "");
}
