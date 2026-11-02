// terrain test: TerrainBaseSlots' changes in the installed DX11 exe mapped as an image (imports not
// bound). terrain_check passes on the game's code; terrain_apply sends the hand-over's count read and
// the release's array to the stubs (`call stub; nop`) and the hand-over's array to ours (`mov edx,
// array; nop`), decoded back here, and points the qsort import at its wrapper; the check then fails
// (it would refuse to change the code twice). Then, on the mapped exe's terrain object with a
// stand-in tile manager, texture maker, free-list reserve and qsort: the hand-over copies the
// game's handles and makes nothing; the off-screen sort tops the free list up to 2 per tile that
// isn't deep water + 64 once the tile lists are filled, once, again when the map grows, up to the
// ceiling, and only for the off-screen comparator; a new manager gets everything made so far; the
// release returns our array once and then the object's own; a small map and base textures off make
// nothing. The stubs change only ecx (hand-over) or ebx (release). Skipped when the game isn't
// installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

#define OFF_ARRAY 0x6d888
#define OFF_POOL 0x62a04
#define OFF_MGR 0x75fa4
#define MGR_OTHERS 0x231c
#define MGR_PRERENDER 0x2324
#define MGR_FREELIST 0x21e8c
#define GAME 6144
#define CAP 20000

void log_event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
// terrain_init and the real texture maker aren't run here.
DWORD texhandles_count(void) { return 0x3000; }
DWORD texhandles_room(void) { return 0; }

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static int g_makes, g_reserves, g_sorts;
static DWORD g_next = 50000;  // the stand-in maker's next handle
static DWORD fake_make(DWORD *out, DWORD n)
{
    g_makes++;
    for (DWORD i = 0; i < n; i++) out[i] = g_next++;
    return n;
}
// The game's FFastVector reserve: capacity doubles until past n, contents move, the old heap
// buffer (not the inline one at +0xc) is freed.
static void __attribute__((thiscall)) fake_reserve(DWORD *vec, DWORD n)
{
    g_reserves++;
    DWORD cap = vec[2] ? vec[2] : 1;
    while (n >= cap) cap *= 2;
    DWORD *d = malloc(cap * 4);
    memcpy(d, (DWORD *)(ULONG_PTR)vec[0], vec[1] * 4);
    if ((DWORD *)(ULONG_PTR)vec[0] != vec + 3) free((DWORD *)(ULONG_PTR)vec[0]);
    vec[0] = (DWORD)(ULONG_PTR)d;
    vec[2] = cap;
}
static void __cdecl fake_qsort(void *b, size_t n, size_t w, int(__cdecl *c)(const void *, const void *))
{
    (void)b, (void)n, (void)w, (void)c;
    g_sorts++;
}

// Calls a stub with esi = obj and known values in the other registers; returns ecx and ebx, and
// whether esi and edi survived.
static DWORD call_stub(void *stub, void *obj, DWORD *ebx, int *kept)
{
    DWORD a = 0x11111111, b = 0x22222222, c = 0x33333333, d = 0x44444444, di = 0x66666666;
    void *si = obj;
    __asm__ volatile("call *%[f]"
                     : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(si), "+D"(di)
                     : [f] "m"(stub)
                     : "cc", "memory");
    *ebx = b;
    *kept = si == obj && di == 0x66666666;
    return c;
}

int main(void)
{
    const WCHAR *path = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V\\CivilizationV_DX11.exe";
    HMODULE m = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        printf("terrain test: skipped (game not installed here)\n");
        return 0;
    }
    BYTE *exe = (BYTE *)m;
    const GameExe *g = game_exe(exe);
    if (!g || !g->terrainObj) {
        printf("terrain test: not the known DX11 exe\n");
        return 1;
    }
    const char *why = terrain_check(exe, g);
    if (why) {
        printf("terrain_check: %s\n", why);
        return 1;
    }
    BYTE after[10];
    memcpy(after, exe + g->terrainRelease + 6, sizeof after);
    void **slot = (void **)exe_import_slot(exe, "qsort");
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (void *)fake_qsort;
    VirtualProtect(slot, 4, old, &old);
    typedef void(__cdecl * qsort_t)(void *, size_t, size_t, void *);
    void *offscreen = *(void **)(exe + g->terrainSort + 1);

    DWORD *array = VirtualAlloc(NULL, CAP * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    why = terrain_apply(exe, g, CAP, array, fake_make, (void *)fake_reserve);
    if (why) {
        printf("terrain_apply: %s\n", why);
        return 1;
    }
    CHECK(terrain_check(exe, g) != NULL, "the check still passes after the change");
    const BYTE *p = exe + g->terrainHandCount;
    CHECK(p[0] == 0xE8 && p + 5 + *(const LONG *)(p + 1) == (BYTE *)terrain_handover_stub() && p[5] == 0x90,
          "hand-over count not a call to its stub");
    p = exe + g->terrainHandArray;
    CHECK(p[0] == 0xBA && *(DWORD *const *)(p + 1) == array && p[5] == 0x90, "hand-over array not mov edx, array");
    p = exe + g->terrainRelease;
    CHECK(p[0] == 0xE8 && p + 5 + *(const LONG *)(p + 1) == (BYTE *)terrain_release_stub() && p[5] == 0x90 &&
          !memcmp(p + 6, after, sizeof after), "release not a call to its stub, or the loop after it changed");
    CHECK(*slot != (void *)fake_qsort, "qsort import not hooked");
    qsort_t sort = (qsort_t)*slot;

    // The mapped exe's terrain object; base textures on.
    BYTE *obj = exe + g->terrainObj;
    DWORD *own = (DWORD *)(obj + OFF_ARRAY), *count = (DWORD *)(obj + OFF_POOL);
    BYTE **mgrp = (BYTE **)(obj + OFF_MGR);
    for (DWORD i = 0; i < GAME; i++) own[i] = 100 + i;
    *count = GAME;
    *mgrp = NULL;
    DWORD *on = *(DWORD **)(exe + g->terrainInit + 2);
    *on = 1;

    DWORD ebx, ecx;
    int kept, bad = 0;
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == GAME && *count == GAME && !g_makes && kept && ebx == 0x22222222,
          "first hand-over: %lu (object %lu), %d makes, registers %s", ecx, *count, g_makes, kept ? "kept" : "changed");
    for (DWORD i = 0; i < GAME; i++) bad += array[i] != 100 + i;
    CHECK(!bad, "our array: %d of the game's wrong", bad);

    // A tile manager as the setup leaves it: the game's handles in the free list's inline storage.
    BYTE *mgr = VirtualAlloc(NULL, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD *vec = (DWORD *)(mgr + MGR_FREELIST), *pre = (DWORD *)(mgr + MGR_PRERENDER), *others = (DWORD *)(mgr + MGR_OTHERS);
    vec[0] = (DWORD)(ULONG_PTR)(vec + 3);
    vec[1] = GAME;
    vec[2] = 0x1fa0;
    memcpy(vec + 3, array, GAME * 4);
    *mgrp = mgr;
    int t = 0;
    sort(&t, 0, 4, offscreen);
    CHECK(!g_makes && g_sorts == 1, "lists empty: %d makes, %d sorts", g_makes, g_sorts);

    *pre = 8288;
    *others = 9;  // 8,297 tiles: 16,658 textures
    sort(&t, 0, 4, offscreen);
    DWORD *data = (DWORD *)(ULONG_PTR)vec[0];
    bad = 0;
    for (DWORD i = 0; i < vec[1] && i < 30000; i++) bad += data[i] != (i < GAME ? 100 + i : 50000 + i - GAME);
    CHECK(g_makes == 1 && g_reserves == 1 && vec[1] == 16658 && *count == 16658 && g_sorts == 2 && !bad &&
          data != vec + 3, "top-up: %d makes, %d reserves, free list %lu, object %lu, %d wrong", g_makes, g_reserves,
          vec[1], *count, bad);
    sort(&t, 0, 4, offscreen);
    CHECK(g_makes == 1 && vec[1] == 16658 && g_sorts == 3, "same lists again: %d makes", g_makes);

    *pre += 100;  // the map grew
    sort(&t, 0, 4, (void *)fake_qsort);
    CHECK(g_makes == 1 && g_sorts == 4, "another comparator topped up");
    sort(&t, 0, 4, offscreen);
    CHECK(g_makes == 2 && vec[1] == 16858 && *count == 16858, "grown map: free list %lu, object %lu", vec[1], *count);

    *pre = 12000;  // more than the ceiling holds
    sort(&t, 0, 4, offscreen);
    CHECK(g_makes == 3 && vec[1] == CAP && *count == CAP, "ceiling: free list %lu, object %lu", vec[1], *count);

    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == CAP && *count == CAP && kept, "a second map's hand-over: %lu", ecx);

    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    CHECK(ebx == (DWORD)(ULONG_PTR)array && kept, "release: %08lx, not our array", ebx);
    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    CHECK(ebx == (DWORD)(ULONG_PTR)own && kept, "second release: %08lx, not the object's array", ebx);

    *count = GAME;  // a new pool and a small map
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    vec[0] = (DWORD)(ULONG_PTR)(vec + 3), vec[1] = GAME, vec[2] = 0x1fa0;
    *pre = 2000;
    *others = 0;
    sort(&t, 0, 4, offscreen);
    CHECK(ecx == GAME && g_makes == 3 && vec[1] == GAME && *count == GAME, "small map: %d makes, free list %lu", g_makes,
          vec[1]);

    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    *count = GAME;
    *on = 0;
    call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    *pre = 8297;
    sort(&t, 0, 4, offscreen);
    CHECK(g_makes == 3 && *count == GAME, "base textures off: %d makes, object %lu", g_makes, *count);

    printf("terrain test: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
