// Small-block allocator fallback ([probe] SmallBlockFallback, default 1; 0 = off).
//
// The exe has a fixed-size block allocator (FSBA) for small objects: one 32 MB region, committed
// up front, handed out in 4 KB chunks to 16 size classes (32 to 512 bytes), each with an array of
// chunk descriptors. The game core DLL's operator new tries it first (FSBAAllocate, an exe
// export) and falls back to malloc when it returns NULL; operator delete gives a block back to it
// when the address is in its region, else frees it. When the current chunk of a size class is
// full, an allocation scans that class's whole descriptor array under a lock for a chunk with a
// free slot (exe+0x41bed0), and takes a new chunk from the region if none has one. Once the
// region has no chunk left, that scan runs on every allocation of a full class, finds nothing,
// and the allocation falls back to malloc anyway. In a late-game save (2026-10-29, turn ~400 of
// a Huge map) all 8,176 chunks were in use, classes such as 96 and 160 bytes had no free slot,
// and the futile scan was 27% of the game core's CPU time during AI turns.
//
// Once every chunk of the region has been in use for 30 s (loading a game fills it briefly and
// gives chunks back), this calls the exe's own FSBADisable(1) export: FSBAAllocate then returns
// NULL straight away and new small objects go to malloc (mimalloc with MiMalloc=1). Blocks
// already in the region are still freed and reallocated by address range (FSBADeallocate and the
// realloc path don't read the disabled flag). The cost: slots freed in the region afterwards
// aren't reused. The allocator is found through the exe's exports (the same in the DX11, DX9 and
// tablet exes; its code is checked).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

#define FULL_FOR_MS 30000

typedef void(__cdecl *FSBADisable_t)(BYTE disable);
typedef BYTE(__cdecl *FSBAIsDisabled_t)(void);
static FSBADisable_t pDisable;
static FSBAIsDisabled_t pIsDisabled;
static BYTE *const *g_allocVar;
static ULONGLONG g_fullSince;
static int g_done;

// GetSmallObjectAllocator's cached allocator variable (0/1 while not created), or NULL.
static BYTE *const *alloc_var(BYTE *exe)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    const BYTE *get = (const BYTE *)GetProcAddress((HMODULE)exe, "GetSmallObjectAllocator");
    // GetSmallObjectAllocator: push ebp; mov ebp,esp; and esp,-8; sub esp,3Ch; cmp byte [x],0;
    // push ebx; push esi; push edi; jne; mov eax,[allocator var]
    static const BYTE kGet[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x3C, 0x80, 0x3D};
    if (!get || memcmp(get, kGet, sizeof kGet) || get[0x15] != 0xA1) return NULL;
    BYTE *var = *(BYTE *const *)(get + 0x16);
    return var > exe && var + 4 <= exe + nt->OptionalHeader.SizeOfImage ? (BYTE *const *)var : NULL;
}

const char *smallblock_check(BYTE *exe, const GameExe *g)
{
    if (!GetProcAddress((HMODULE)exe, "FSBADisable") || !GetProcAddress((HMODULE)exe, "FSBAIsDisabled")) return "exports";
    return alloc_var(exe) ? NULL : "GetSmallObjectAllocator";
}

void smallblock_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"SmallBlockFallback", 1, ini)) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const char *why = smallblock_check(exe, NULL);
    if (why) {
        log_event("smallblock: the exe's small-block allocator isn't the known one (%s); left alone", why);
        return;
    }
    pDisable = (FSBADisable_t)GetProcAddress((HMODULE)exe, "FSBADisable");
    pIsDisabled = (FSBAIsDisabled_t)GetProcAddress((HMODULE)exe, "FSBAIsDisabled");
    g_allocVar = alloc_var(exe);
    log_event("smallblock: the game's small-block allocator will be switched off once its 32 MB region stays full for %d s",
              FULL_FOR_MS / 1000);
}

// Chunks in use / chunks in the region. The counts change under the allocator's locks; a torn
// read only delays the decision by a tick.
static int region_full(const BYTE *a, DWORD *used, DWORD *total)
{
    const BYTE *classes = *(BYTE *const *)a;
    DWORD n = *(const DWORD *)(a + 0xb4), shift = *(const DWORD *)(a + 0x70), size = *(const DWORD *)(a + 0x40);
    if (!classes || n > 64 || shift < 10 || shift > 20) return 0;
    *used = 0;
    for (DWORD i = 0; i < n; i++) *used += *(const DWORD *)(classes + 0x40 * i + 0xc);
    *total = size >> shift;
    return *total && *used + 16 >= *total;
}

// Sampler thread.
void smallblock_tick(void)
{
    if (!g_allocVar || g_done) return;
    const BYTE *a = *g_allocVar;
    if ((ULONG_PTR)a <= 1) return;  // not created yet
    DWORD used, total;
    ULONGLONG now = GetTickCount64();
    if (!region_full(a, &used, &total)) {
        g_fullSince = 0;
        return;
    }
    if (!g_fullSince) g_fullSince = now;
    if (now - g_fullSince < FULL_FOR_MS) return;
    g_done = 1;
    pDisable(1);
    log_event("smallblock: the game's small-block region has been full for %d s (%lu of %lu chunks); switched its allocator "
              "off (FSBADisable), new small objects now go to malloc (disabled=%d)",
              FULL_FOR_MS / 1000, used, total, pIsDisabled());
}
