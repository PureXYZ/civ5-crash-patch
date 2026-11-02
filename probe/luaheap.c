// The game's Lua heap on mimalloc ([probe] LuaHeapMimalloc, default 1; 0 = off), and the game
// database's SQLite heap on mimalloc ([probe] DbHeapMimalloc, default 1; 0 = off).
//
// The exe's Lua allocator (lua_Alloc, exe+0x2ce440) keeps every Lua object in a heap of its own,
// created at startup with the low-fragmentation front end (exe+0x44b912, handle at exe+0x894b1c)
// and never destroyed. It calls HeapAlloc / HeapReAlloc / HeapFree with flags 0 and HeapSize (to
// count the bytes it frees; it counts the requested size when it allocates). An NT heap never
// gives segments back, so the heap keeps its peak and its holes: in a late-game Huge save it held
// ~190 MB of address space for ~110 MB of live Lua (peak 146-150 MB).
//
// The game database DLL gives SQLite its own memory methods (the sqlite3_mem_methods table at
// +0x7849c: malloc +0x64c0, free +0x6500, realloc +0x6550, size +0x65a0, init +0x65d0, shutdown
// +0x6610). Init makes a growable heap (HeapCreate(0, 5 MB, 0), handle at +0x61b6c4) and the others
// call the DLL's HeapAlloc / HeapFree / HeapReAlloc / HeapSize imports on it with flags 0, keeping a
// byte count from the rounded requested size and HeapSize. Nothing else uses the handle except a
// HeapWalk statistics pass (+0x6630, Database.log). Loading a heavily modded game peaks SQLite at
// ~78 MB and leaves ~14 MB live, but the heap keeps its 16 MB segments: 94 MB held (2026-11-01).
//
// With either option, the module's four imports point at wrappers that, for that heap's handle,
// put the block in mimalloc behind an 8-byte header with the requested size (so HeapSize returns
// what the caller expects and its byte counter stays exact) and an owner tag per heap. mimalloc
// gives empty segments back to Windows. Other heaps go straight through, as do blocks of these
// heaps that aren't ours (none expected: both heaps are created after the probe loads). The heaps'
// other users (the exe's HeapCompact exe+0x6217af and HeapWalk exe+0x63df20, the database's HeapWalk)
// see an almost empty heap. DX11 addresses here; the DX9 and tablet exes have the same Lua allocator
// (exes.c: luaAlloc, luaHeapVar; code checked) and share the database DLL.
//
// The heaps themselves are made with room up front: the exe's HeapCreate(0, 20 MB, 0)
// (exe+0x4b903) and the database's HeapCreate(0, 5 MB, 0) commit it all at once, and with the
// redirect nothing ever uses it (read in the game 2026-11-02: both 100% free, 25 MB). Each module
// calls HeapCreate once, for that heap, so its HeapCreate import also gets a wrapper that asks for
// a growable heap with nothing up front; it still grows if a block ever falls back to it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"
#include <mimalloc.h>

typedef LPVOID(WINAPI *alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID(WINAPI *realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL(WINAPI *free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T(WINAPI *size_t_t)(HANDLE, DWORD, LPCVOID);
typedef HANDLE(WINAPI *create_t)(DWORD, SIZE_T, SIZE_T);
static alloc_t real_alloc;
static realloc_t real_realloc;
static free_t real_free;
static size_t_t real_size;
static create_t real_create;

typedef struct {
    HANDLE *var;  // where the module keeps the heap handle; NULL = not redirected
    DWORD tag;
    volatile LONG live, peak, blocks, fallback, foreign;
    create_t create;  // the module's HeapCreate import before the probe's wrapper (NULL: not hooked)
    SIZE_T upfront;   // what that HeapCreate asked to commit up front (0: not made by it yet)
} Heap;
static Heap g_lua = {.tag = 0x4D61754Cu};  // "LuaM"
static Heap g_db = {.tag = 0x4D626453u};   // "SdbM"

#define OK_FLAGS (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY)

static Heap *heap_of(HANDLE h)
{
    if (!h) return NULL;
    if (g_lua.var && h == *g_lua.var) return &g_lua;
    if (g_db.var && h == *g_db.var) return &g_db;
    return NULL;
}
static int ours(const Heap *H, const void *p) { return p && mi_is_in_heap_region(p) && ((const DWORD *)p)[-1] == H->tag; }

static void count(Heap *H, LONG delta)
{
    LONG now = InterlockedExchangeAdd(&H->live, delta) + delta;
    for (LONG m = H->peak; now > m; m = H->peak) InterlockedCompareExchange(&H->peak, now, m);
}

static LPVOID WINAPI h_alloc(HANDLE h, DWORD flags, SIZE_T n)
{
    Heap *H = heap_of(h);
    if (!H || (flags & ~OK_FLAGS) || n > 0x7FFF0000) return real_alloc(h, flags, n);
    DWORD *b = (DWORD *)mi_malloc(n + 8);
    if (!b) {
        InterlockedIncrement(&H->fallback);
        return real_alloc(h, flags, n);
    }
    b[0] = (DWORD)n;
    b[1] = H->tag;
    if (flags & HEAP_ZERO_MEMORY) memset(b + 2, 0, n);
    InterlockedIncrement(&H->blocks);
    count(H, (LONG)n);
    return b + 2;
}

static BOOL WINAPI h_free(HANDLE h, DWORD flags, LPVOID p)
{
    Heap *H = heap_of(h);
    if (!H || !ours(H, p)) {
        if (p && H) InterlockedIncrement(&H->foreign);
        return real_free(h, flags, p);
    }
    DWORD *b = (DWORD *)p - 2;
    count(H, -(LONG)b[0]);
    InterlockedDecrement(&H->blocks);
    b[1] = 0;
    mi_free(b);
    return TRUE;
}

static SIZE_T WINAPI h_size(HANDLE h, DWORD flags, LPCVOID p)
{
    Heap *H = heap_of(h);
    if (!H || !ours(H, p)) return real_size(h, flags, p);
    return ((const DWORD *)p)[-2];
}

static LPVOID WINAPI h_realloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
    Heap *H = heap_of(h);
    if (!H || !ours(H, p)) return real_realloc(h, flags, p, n);
    DWORD *b = (DWORD *)p - 2, old = b[0];
    if (n > 0x7FFF0000) return NULL;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n + 8 > mi_usable_size(b)) return NULL;
    } else {
        b = (DWORD *)mi_realloc(b, n + 8);
        if (!b) return NULL;  // the old block stays valid, as with HeapReAlloc
    }
    b[0] = (DWORD)n;
    b[1] = H->tag;
    if ((flags & HEAP_ZERO_MEMORY) && n > old) memset((BYTE *)(b + 2) + old, 0, n - old);
    count(H, (LONG)n - (LONG)old);
    return b + 2;
}

// A growable heap with nothing committed up front instead of the module's initial size.
static HANDLE create(Heap *H, DWORD flags, SIZE_T initial, SIZE_T max)
{
    if (max || !initial) return H->create(flags, initial, max);
    HANDLE h = H->create(flags, 0, 0);
    if (!h) return H->create(flags, initial, max);
    H->upfront = initial;
    return h;
}
static HANDLE WINAPI h_create_lua(DWORD flags, SIZE_T initial, SIZE_T max) { return create(&g_lua, flags, initial, max); }
static HANDLE WINAPI h_create_db(DWORD flags, SIZE_T initial, SIZE_T max) { return create(&g_db, flags, initial, max); }

static void real_heap_functions(void)
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    real_alloc = (alloc_t)GetProcAddress(k, "HeapAlloc");
    real_realloc = (realloc_t)GetProcAddress(k, "HeapReAlloc");
    real_free = (free_t)GetProcAddress(k, "HeapFree");
    real_size = (size_t_t)GetProcAddress(k, "HeapSize");
    real_create = (create_t)GetProcAddress(k, "HeapCreate");
}

#ifdef LUAHEAP_TEST
// The test drives both heaps: luaVar and dbVar stand in for the two modules' handle variables.
void luaheap_test_setup(HANDLE *luaVar, HANDLE *dbVar, void **a, void **r, void **f, void **s)
{
    real_heap_functions();
    g_lua.var = luaVar;
    g_db.var = dbVar;
    g_lua.create = g_db.create = real_create;
    *a = (void *)h_alloc;
    *r = (void *)h_realloc;
    *f = (void *)h_free;
    *s = (void *)h_size;
}
// The two modules' HeapCreate wrappers, and what each one's caller asked to commit up front.
void luaheap_test_create(void **lua, void **db, SIZE_T **luaUpfront, SIZE_T **dbUpfront)
{
    *lua = (void *)h_create_lua;
    *db = (void *)h_create_db;
    *luaUpfront = &g_lua.upfront;
    *dbUpfront = &g_db.upfront;
}
#endif

static int slot_is(BYTE *mod, const char *name, void *fn)
{
    BYTE *slot = exe_import_slot(mod, name);
    return slot && *(void **)slot == fn;
}

static void set_slot(BYTE *mod, const char *name, void *to)
{
    void *volatile *slot = (void *volatile *)exe_import_slot(mod, name);
    DWORD old;
    if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return;
    InterlockedExchange((volatile LONG *)slot, (LONG)(ULONG_PTR)to);
    VirtualProtect((void *)slot, 4, old, &old);
}

static int slots_unhooked(BYTE *mod)
{
    return slot_is(mod, "HeapAlloc", (void *)real_alloc) && slot_is(mod, "HeapReAlloc", (void *)real_realloc) &&
           slot_is(mod, "HeapFree", (void *)real_free) && slot_is(mod, "HeapSize", (void *)real_size);
}

// Redirects the module's four imports; the wrappers serve both heaps, so a second call for the
// other module only repoints that module's slots. Its HeapCreate goes to H's own wrapper unless
// something else already hooks it (then the heap keeps its room up front).
static void redirect(BYTE *mod, Heap *H, void *createWrapper)
{
    set_slot(mod, "HeapSize", (void *)h_size);
    set_slot(mod, "HeapFree", (void *)h_free);
    set_slot(mod, "HeapReAlloc", (void *)h_realloc);
    set_slot(mod, "HeapAlloc", (void *)h_alloc);
    if (slot_is(mod, "HeapCreate", (void *)real_create)) {
        H->create = real_create;
        set_slot(mod, "HeapCreate", createWrapper);
    }
}

static int mimalloc_ok(void)
{
    void *probe = mi_malloc(16);
    int ok = probe && mi_is_in_heap_region(probe);
    mi_free(probe);
    return ok;
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

// The database DLL (one file for all three exes): timestamp 0x546cbfb1, image 0x624000.
#define DB_HEAP_VAR 0x61b6c4
#define DB_METHODS 0x7849c

// `mov eax,[heap var]` at off, and `call [import]` at each call offset.
static int db_uses(BYTE *db, DWORD fn, DWORD movOff, const char *import, DWORD callOff)
{
    const BYTE *p = db + fn;
    return p[movOff] == 0xA1 && *(BYTE *const *)(p + movOff + 1) == db + DB_HEAP_VAR && p[callOff] == 0xFF &&
           p[callOff + 1] == 0x15 && *(BYTE *const *)(p + callOff + 2) == exe_import_slot(db, import);
}

const char *dbheap_check(BYTE *db)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(db + ((IMAGE_DOS_HEADER *)db)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x546cbfb1 || nt->OptionalHeader.SizeOfImage != 0x624000) return "build";
    // SQLite's memory methods: malloc, free, realloc, size, roundup, init, shutdown.
    static const DWORD kFns[] = {0x64c0, 0x6500, 0x6550, 0x65a0, 0x65c0, 0x65d0, 0x6610};
    BYTE *const *m = (BYTE *const *)(db + DB_METHODS);
    for (int i = 0; i < 7; i++)
        if (m[i] != db + kFns[i]) return "memory methods";
    // malloc: mov eax,[esp+4]; mov ecx,[count]; add eax,7; and eax,-8 ... mov eax,[heap]; push 0; push eax; call [HeapAlloc]
    static const BYTE kMalloc[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D};
    if (memcmp(db + 0x64c0, kMalloc, sizeof kMalloc) || !db_uses(db, 0x64c0, 0x27, "HeapAlloc", 0x2f))
        return "malloc";
    if (!db_uses(db, 0x6500, 0x00, "HeapSize", 0x0e) || db[0x6532] != 0x8B || db[0x6533] != 0x0D ||
        *(BYTE **)(db + 0x6534) != db + DB_HEAP_VAR || db[0x653c] != 0xFF ||
        *(BYTE **)(db + 0x653e) != exe_import_slot(db, "HeapFree"))
        return "free";
    if (!db_uses(db, 0x6550, 0x00, "HeapSize", 0x19) || db[0x6597] != 0xFF ||
        *(BYTE **)(db + 0x6599) != exe_import_slot(db, "HeapReAlloc"))
        return "realloc";
    if (!db_uses(db, 0x65a0, 0x09, "HeapSize", 0x11)) return "size";
    // init: push ecx; push 0; push 500000h; push 0; call [HeapCreate] ... mov [heap var],eax
    static const BYTE kInit[] = {0x51, 0x6A, 0x00, 0x68, 0x00, 0x00, 0x50, 0x00, 0x6A, 0x00, 0xFF, 0x15};
    if (memcmp(db + 0x65d0, kInit, sizeof kInit) || db[0x65ea] != 0xA3 || *(BYTE **)(db + 0x65eb) != db + DB_HEAP_VAR)
        return "init";
    return NULL;
}

static void dbheap_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"DbHeapMimalloc", 1, ini)) return;
    BYTE *db = (BYTE *)GetModuleHandleW(L"CvGameDatabaseWin32Final Release.dll");
    const char *why = !db ? "not loaded" : dbheap_check(db);
    if (!why && !slots_unhooked(db)) why = "import slots";
    if (!why && !mimalloc_ok()) why = "mimalloc self-test";
    if (why) {
        log_event("dbheap: the game database's SQLite allocator isn't the known code (%s); left alone", why);
        return;
    }
    if (*(HANDLE *)(db + DB_HEAP_VAR))
        log_event("dbheap: the SQLite heap already exists; its older blocks stay in it");
    g_db.var = (HANDLE *)(db + DB_HEAP_VAR);
    redirect(db, &g_db, (void *)h_create_db);
    log_event("dbheap: the game database's SQLite memory goes to mimalloc instead of its own Windows heap (which "
              "never gives space back); no game code changed");
}

void luaheap_init(const WCHAR *ini)
{
    real_heap_functions();
    dbheap_init(ini);
    if (!GetPrivateProfileIntW(L"probe", L"LuaHeapMimalloc", 1, ini)) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    const char *why = g ? luaheap_check(exe, g) : "not a known build of the game exe";
    if (!why && !slots_unhooked(exe)) why = "import slots";
    if (!why && !mimalloc_ok()) why = "mimalloc self-test";
    if (why) {
        log_event("luaheap: the game's Lua allocator isn't the known code (%s); left alone", why);
        return;
    }
    g_lua.var = (HANDLE *)(exe + g->luaHeapVar);
    redirect(exe, &g_lua, (void *)h_create_lua);
    log_event("luaheap: the game's Lua objects go to mimalloc instead of its own Windows heap (which never gives space "
              "back); no game code changed");
}

// How the module's Windows heap was made, for the report.
static const char *made(const Heap *H, char *buf, size_t cap)
{
    if (!H->create) return "its HeapCreate wasn't hooked";
    if (!H->upfront) return "its Windows heap not made yet";
    snprintf(buf, cap, "its Windows heap made with nothing up front (the game asked for %.0f MB)",
             H->upfront / 1048576.0);
    return buf;
}

void luaheap_report(void)
{
    char a[96];
    if (g_lua.var)
        log_event("luaheap: %ld Lua blocks, %.1f MB live in mimalloc (peak %.1f MB); %ld fell back to the Windows "
                  "heap, %ld frees of blocks that weren't ours; %s",
                  g_lua.blocks, g_lua.live / 1048576.0, g_lua.peak / 1048576.0, g_lua.fallback, g_lua.foreign,
                  made(&g_lua, a, sizeof a));
    if (g_db.var)
        log_event("dbheap: %ld SQLite blocks, %.1f MB live in mimalloc (peak %.1f MB); %ld fell back to the Windows "
                  "heap, %ld frees of blocks that weren't ours; %s",
                  g_db.blocks, g_db.live / 1048576.0, g_db.peak / 1048576.0, g_db.fallback, g_db.foreign,
                  made(&g_db, a, sizeof a));
}
