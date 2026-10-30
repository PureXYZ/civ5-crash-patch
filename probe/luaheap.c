// The game's Lua heap on mimalloc ([probe] LuaHeapMimalloc, default 1; 0 = off).
//
// The exe's Lua allocator (lua_Alloc, exe+0x2ce440) keeps every Lua object in a heap of its own,
// created at startup with the low-fragmentation front end (exe+0x44b912, handle at exe+0x894b1c)
// and never destroyed. It calls HeapAlloc / HeapReAlloc / HeapFree with flags 0 and HeapSize (to
// count the bytes it frees; it counts the requested size when it allocates). An NT heap never
// gives segments back, so the heap keeps its peak and its holes: in a late-game Huge save it held
// ~190 MB of address space for ~110 MB of live Lua (peak 146-150 MB).
//
// With LuaHeapMimalloc=1 those four imports of the exe point at wrappers that, for this one heap
// handle, put the block in mimalloc behind an 8-byte header with the requested size (so HeapSize
// returns what the game expects and its Lua byte counter stays exact) and an owner tag. mimalloc
// gives empty segments back to Windows. Other heaps go straight through, as do blocks of this heap
// that aren't ours (none expected: the heap is created after the probe loads). The game's other
// uses of the handle, HeapCompact (exe+0x6217af) and a HeapWalk statistics pass (exe+0x63df20), see
// an almost empty heap. DX11 addresses here; the DX9 and tablet exes have the same allocator
// (exes.c: luaAlloc, luaHeapVar; code checked).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"
#include <mimalloc.h>

#define TAG 0x4D61754Cu  // "LuaM"

typedef LPVOID(WINAPI *alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID(WINAPI *realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL(WINAPI *free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T(WINAPI *size_t_t)(HANDLE, DWORD, LPCVOID);
static alloc_t real_alloc;
static realloc_t real_realloc;
static free_t real_free;
static size_t_t real_size;
static HANDLE *g_heapVar;
static volatile LONG g_live, g_peak, g_blocks, g_fallback, g_foreign;

#define OK_FLAGS (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY)

static int lua_heap(HANDLE h) { return h && h == *g_heapVar; }
static int ours(const void *p) { return p && mi_is_in_heap_region(p) && ((const DWORD *)p)[-1] == TAG; }

static void count(LONG delta)
{
    LONG now = InterlockedExchangeAdd(&g_live, delta) + delta;
    for (LONG m = g_peak; now > m; m = g_peak) InterlockedCompareExchange(&g_peak, now, m);
}

static LPVOID WINAPI h_alloc(HANDLE h, DWORD flags, SIZE_T n)
{
    if (!lua_heap(h) || (flags & ~OK_FLAGS) || n > 0x7FFF0000) return real_alloc(h, flags, n);
    DWORD *b = (DWORD *)mi_malloc(n + 8);
    if (!b) {
        InterlockedIncrement(&g_fallback);
        return real_alloc(h, flags, n);
    }
    b[0] = (DWORD)n;
    b[1] = TAG;
    if (flags & HEAP_ZERO_MEMORY) memset(b + 2, 0, n);
    InterlockedIncrement(&g_blocks);
    count((LONG)n);
    return b + 2;
}

static BOOL WINAPI h_free(HANDLE h, DWORD flags, LPVOID p)
{
    if (!lua_heap(h) || !ours(p)) {
        if (p && lua_heap(h)) InterlockedIncrement(&g_foreign);
        return real_free(h, flags, p);
    }
    DWORD *b = (DWORD *)p - 2;
    count(-(LONG)b[0]);
    InterlockedDecrement(&g_blocks);
    b[1] = 0;
    mi_free(b);
    return TRUE;
}

static SIZE_T WINAPI h_size(HANDLE h, DWORD flags, LPCVOID p)
{
    if (!lua_heap(h) || !ours(p)) return real_size(h, flags, p);
    return ((const DWORD *)p)[-2];
}

static LPVOID WINAPI h_realloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
    if (!lua_heap(h) || !ours(p)) return real_realloc(h, flags, p, n);
    DWORD *b = (DWORD *)p - 2, old = b[0];
    if (n > 0x7FFF0000) return NULL;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n + 8 > mi_usable_size(b)) return NULL;
    } else {
        b = (DWORD *)mi_realloc(b, n + 8);
        if (!b) return NULL;  // the old block stays valid, as with HeapReAlloc
    }
    b[0] = (DWORD)n;
    b[1] = TAG;
    if ((flags & HEAP_ZERO_MEMORY) && n > old) memset((BYTE *)(b + 2) + old, 0, n - old);
    count((LONG)n - (LONG)old);
    return b + 2;
}

#ifdef LUAHEAP_TEST
void luaheap_test_setup(HANDLE *heapVar, void **a, void **r, void **f, void **s)
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    real_alloc = (alloc_t)GetProcAddress(k, "HeapAlloc");
    real_realloc = (realloc_t)GetProcAddress(k, "HeapReAlloc");
    real_free = (free_t)GetProcAddress(k, "HeapFree");
    real_size = (size_t_t)GetProcAddress(k, "HeapSize");
    g_heapVar = heapVar;
    *a = (void *)h_alloc;
    *r = (void *)h_realloc;
    *f = (void *)h_free;
    *s = (void *)h_size;
}
#endif

static int slot_is(BYTE *exe, const char *name, void *fn)
{
    BYTE *slot = exe_import_slot(exe, name);
    return slot && *(void **)slot == fn;
}

static void set_slot(BYTE *exe, const char *name, void *to)
{
    void *volatile *slot = (void *volatile *)exe_import_slot(exe, name);
    DWORD old;
    if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return;
    InterlockedExchange((volatile LONG *)slot, (LONG)(ULONG_PTR)to);
    VirtualProtect((void *)slot, 4, old, &old);
}

const char *luaheap_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    const BYTE *la = exe + g->luaAlloc;
    // lua_Alloc: push esi; mov esi,[esp+14h]; push edi; xor edi,edi; test esi,esi; jne; mov esi,[esp+10h];
    // test esi,esi; je; mov eax,[heap var]; push esi; push edi; push eax; call [HeapSize]
    static const BYTE kHead[] = {0x56, 0x8B, 0x74, 0x24, 0x14, 0x57, 0x33, 0xFF, 0x85, 0xF6, 0x75};
    if (!g->luaAlloc || g->luaHeapVar + 4 >= nt->OptionalHeader.SizeOfImage) return "addresses";
    if (memcmp(la, kHead, sizeof kHead) || la[0x14] != 0xA1 || *(BYTE *const *)(la + 0x15) != exe + g->luaHeapVar ||
        la[0x1c] != 0xFF || la[0x1d] != 0x15 || *(BYTE *const *)(la + 0x1e) != exe_import_slot(exe, "HeapSize"))
        return "lua_Alloc";
    if (!exe_import_slot(exe, "HeapAlloc") || !exe_import_slot(exe, "HeapReAlloc") || !exe_import_slot(exe, "HeapFree"))
        return "imports";
    return NULL;
}

void luaheap_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"LuaHeapMimalloc", 1, ini)) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    real_alloc = (alloc_t)GetProcAddress(k, "HeapAlloc");
    real_realloc = (realloc_t)GetProcAddress(k, "HeapReAlloc");
    real_free = (free_t)GetProcAddress(k, "HeapFree");
    real_size = (size_t_t)GetProcAddress(k, "HeapSize");
    const char *why = g ? luaheap_check(exe, g) : "not a known build of the game exe";
    if (!why && !(slot_is(exe, "HeapAlloc", (void *)real_alloc) && slot_is(exe, "HeapReAlloc", (void *)real_realloc) &&
                  slot_is(exe, "HeapFree", (void *)real_free) && slot_is(exe, "HeapSize", (void *)real_size)))
        why = "import slots";
    if (why) {
        log_event("luaheap: the game's Lua allocator isn't the known code (%s); left alone", why);
        return;
    }
    void *probe = mi_malloc(16);
    if (!probe || !mi_is_in_heap_region(probe)) {
        log_event("luaheap: mimalloc self-test failed; left alone");
        return;
    }
    mi_free(probe);
    g_heapVar = (HANDLE *)(exe + g->luaHeapVar);
    set_slot(exe, "HeapSize", (void *)h_size);
    set_slot(exe, "HeapFree", (void *)h_free);
    set_slot(exe, "HeapReAlloc", (void *)h_realloc);
    set_slot(exe, "HeapAlloc", (void *)h_alloc);
    log_event("luaheap: the game's Lua objects go to mimalloc instead of its own Windows heap (which never gives space "
              "back); no game code changed");
}

void luaheap_report(void)
{
    if (!g_heapVar) return;
    log_event("luaheap: %ld Lua blocks, %.1f MB live in mimalloc (peak %.1f MB); %ld fell back to the Windows heap, %ld "
              "frees of blocks that weren't ours",
              g_blocks, g_live / 1048576.0, g_peak / 1048576.0, g_fallback, g_foreign);
}
