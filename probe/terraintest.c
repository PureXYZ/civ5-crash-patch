// terrain test: TerrainBaseSlots' changes in the installed DX11 exe mapped as an image (imports not
// bound). terrain_check passes on the game's code; terrain_apply sends the hand-over's count read and
// the release's array to the stubs (`call stub; nop`) and the hand-over's array to ours (`mov edx,
// array; nop`), decoded back here, and the check then fails (it would refuse to change the code
// twice). Then the stubs run on a stand-in terrain object with a stand-in texture maker: the first
// hand-over copies the game's handles, adds the made ones and stores the total in the object, a
// second hand-over makes nothing, the release returns our array once and then the object's own, a
// new pool (the game's count again) makes them again, a short maker gives a smaller total, and with
// base textures off nothing is made. The stubs change only ecx (hand-over) or ebx (release).
// Skipped when the game isn't installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define OFF_ARRAY 0x6d888
#define OFF_POOL 0x62a04
#define GAME 6144
#define SLOTS 16640

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

static int g_calls;
static DWORD g_short;  // 0 = make all asked
static DWORD fake_make(DWORD *out, DWORD n)
{
    g_calls++;
    if (g_short && g_short < n) n = g_short;
    for (DWORD i = 0; i < n; i++) out[i] = 50000 + i;
    return n;
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

    DWORD *array = VirtualAlloc(NULL, SLOTS * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    why = terrain_apply(exe, g, SLOTS, array, fake_make);
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

    // A stand-in terrain object; base textures on.
    BYTE *obj = VirtualAlloc(NULL, OFF_ARRAY + 8096 * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD *own = (DWORD *)(obj + OFF_ARRAY), *count = (DWORD *)(obj + OFF_POOL);
    for (DWORD i = 0; i < GAME; i++) own[i] = 100 + i;
    *count = GAME;
    DWORD *on = *(DWORD **)(exe + g->terrainInit + 2);
    *on = 1;

    DWORD ebx, ecx;
    int kept, bad = 0;
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == SLOTS && *count == SLOTS && g_calls == 1 && kept && ebx == 0x22222222,
          "first hand-over: %lu (object %lu), %d makes, registers %s", ecx, *count, g_calls, kept ? "kept" : "changed");
    for (DWORD i = 0; i < SLOTS; i++) bad += array[i] != (i < GAME ? 100 + i : 50000 + i - GAME);
    CHECK(!bad, "our array: %d wrong", bad);
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == SLOTS && g_calls == 1 && kept, "second hand-over: %lu, %d makes", ecx, g_calls);

    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    CHECK(ebx == (DWORD)(ULONG_PTR)array && kept, "release: %08lx, not our array", ebx);
    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    CHECK(ebx == (DWORD)(ULONG_PTR)own && kept, "second release: %08lx, not the object's array", ebx);

    *count = GAME;  // a new pool
    g_short = 100;
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == GAME + 100 && *count == GAME + 100 && g_calls == 2, "short maker: %lu, %d makes", ecx, g_calls);

    call_stub(terrain_release_stub(), obj, &ebx, &kept);
    *count = GAME;
    *on = 0;
    ecx = call_stub(terrain_handover_stub(), obj, &ebx, &kept);
    CHECK(ecx == GAME && *count == GAME && g_calls == 2, "base textures off: %lu, %d makes", ecx, g_calls);

    printf("terrain test: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
