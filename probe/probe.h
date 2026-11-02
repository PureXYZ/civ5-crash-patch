// Shared between probe.c and alloctrack.c.
#pragma once
#include <windows.h>

void log_event(const char *fmt, ...);
double log_time_s(void);  // the events log's clock: seconds since the probe started
HANDLE open_out(const WCHAR *name);
void write_all(HANDLE h, const char *buf, int len);
extern WCHAR g_session[32];

// texhandles.c: a 4-byte operand in the texture handle pool's own functions, and what it holds.
enum { TH_OFF, TH_COUNT, TH_REFBYTES, TH_ITEMBYTES };  // pool offset past the refcounts, 0x3000, 4 x / 12 x 0x3000
typedef struct { DWORD rva, value, kind; } TexSite;

// exes.c: the RVAs the parts that change exact places use, per build of the game exe. 0 = that
// build has no such place (the part stays off there).
typedef struct {
    const char *name;  // "DX11", "tablet", "DX9"
    DWORD timestamp, imageSize;
    DWORD gameFault;   // the game's own NULL write at every start (probe.c)
    // cmdstream.c
    BYTE streamHalves; // 2: double-buffered streams (halves swap at the flip); 1: one set of arrays (DX9)
    BYTE recordRound;  // the record allocator rounds sizes up to 256 bytes (tablet)
    DWORD streamCtor, ctorRets[3], streamSplit, streamHalvesSite, streamBump, bounds[8];
    DWORD flipCall, flip, streamReset, spareCallers[3], pairPush[2];
    // msgqueue.c
    DWORD queueObj, queuePass, queueFlip, queuePost, queueWalk, queueMainSite;
    // uiqueue.c
    DWORD uiProcess, uiOwner, uiLocked, uiGame, uiMode, uiSite, uiRet1, uiRet2, uiDispatch, uiListeners;
    DWORD uiRegDirty, uiRegTeamMet;
    // memwarn.c: the registration of record type 0x73 (GameplayAlertMessage) with the generic
    // listener handler, and the call that names it (push "GameplayAlertMessage"; call)
    DWORD uiRegAlert, uiAlertCall;
    // mapgrid.c
    DWORD gridReserve, gridReserveFn, grids, gridFree;
    // luaheap.c
    DWORD luaHeapVar, luaAlloc;
    // coretick.c
    DWORD coreSite, coreAcc;
    // luaguard.c
    DWORD luaLock, luaUnlock;
    // d3dstats.c
    DWORD d3dContext, d3dFlushUse, d3dFlushVB, d3dFlushIB;
    // texhandles.c: the texture handle pool, the records right after it, how many .text
    // relocations point into the pool past its refcounts and into the records, and the pool
    // functions' own offsets and sizes
    DWORD texPool, texRecords, texPoolFixups, texRecFixups, texSiteCount;
    const TexSite *texSites;
    // terrain.c: the static terrain object, the pool init's call on it, the start of the init's
    // base texture part (read, never changed: it's checksummed), the hand-over to a map's tile
    // manager (count read, array lea), the release at exit (array lea), the tile update's
    // off-screen sort (push comparator ... call [qsort]), the tile lists' builder, and the tile
    // manager load and free-count compare the report relies on
    DWORD terrainObj, terrainPoolCall, terrainInit, terrainHandCount, terrainHandArray, terrainRelease;
    DWORD terrainSort, terrainLists, terrainMgrLoad, terrainAlloc;
    // intro.c: the intro player's CoCreateInstance(CLSID_FilterGraph) call, the owner's copy of
    // the SkipIntroVideo setting into the player, and that setting
    DWORD introCreate, introSkipCopy, skipIntro;
    // fluidpool.c: the fluid fog's pool init loop, the seeding's per-particle function, the
    // substep's count reset and step launch call, the step job's block loop, the step and the
    // pass-through, the merge, and the destructor's call that frees each pool
    DWORD fluidInit, fluidSeed, fluidReset, fluidStepCall, fluidJob, fluidStepFn, fluidPassFn, fluidMerge, fluidFree;
} GameExe;
const GameExe *game_exe(const BYTE *exe);  // the table for this image (timestamp and size), or NULL
const GameExe *game_exe_dx11(void);        // for the tests' stand-in images
BYTE *exe_import_slot(BYTE *exe, const char *name);  // by name; bound or not
BYTE *exe_branch_target(BYTE *exe, const BYTE *p);   // E8/E9 rel32 at p, or NULL

// Each part's check of the exe's code and data at its table's places, without changing anything:
// NULL if all is as expected, else what differs. Used at start and by CivProbe_CheckExe (the
// self-test, on exe files mapped without binding their imports).
const char *cmdstream_check(BYTE *exe, const GameExe *g);
const char *msgqueue_check(BYTE *exe, const GameExe *g);
const char *uiqueue_check(BYTE *exe, const GameExe *g);
const char *uiqueue_tables_check(BYTE *exe, const GameExe *g);
const char *mapgrid_check(BYTE *exe, const GameExe *g);
const char *luaheap_check(BYTE *exe, const GameExe *g);
const char *coretick_check(BYTE *exe, const GameExe *g);
const char *smallblock_check(BYTE *exe, const GameExe *g);
const char *luaguard_check(BYTE *exe, const GameExe *g);
const char *d3dstats_check(BYTE *exe, const GameExe *g);
const char *texhandles_check(BYTE *exe, const GameExe *g);
const char *terrain_check(BYTE *exe, const GameExe *g);
const char *intro_check(BYTE *exe, const GameExe *g);
const char *memwarn_check(BYTE *exe, const GameExe *g);
const char *fluidpool_check(BYTE *exe, const GameExe *g);

// alloctrack.c
void alloc_init(const WCHAR *ini);
void alloc_report(const char *label);
int alloc_enabled(void);
HMODULE alloc_crt_module(void);  // the tracked CRT (AllocCrt), or NULL
LONG alloc_est_live_mb(void);     // estimated live bytes of the tracked CRT, in MB

// luagc.c
void luagc_init(const WCHAR *ini);
void luagc_report(void);
int luagc_on(void);  // after luagc_init: the DB call wrappers are on (memwarn.c shows its warning there)
// The Lua state of tid's running wrapped DB call, or NULL; *arg is where that call's first
// argument sits on tid's stack, and the word below it equals *ret while the call runs.
struct lua_State *luagc_thread_call(DWORD tid, void **arg, const void **ret);
DWORD alloc_report_seconds(void);

// mialloc.c
void mialloc_init(const WCHAR *ini);  // before luagc_init
void mialloc_report(void);
void *mialloc_replacement(void *crtFn);  // the mimalloc stand-in for a CRT export, or crtFn
__declspec(dllexport) int CivProbe_IsMimalloc(const void *p);  // p is in a mimalloc segment (MiMalloc=1)

// luaguard.c
void luaguard_init(const WCHAR *ini);
void luaguard_report(void);
CRITICAL_SECTION *luaguard_lua_lock(void);  // the exe's Lua lock (also with LuaUnrefGuard=0), or NULL

// memwarn.c
void memwarn_init(const WCHAR *ini);  // DllMain (takes the main thread's id)
void memwarn_report(void);
int memwarn_feed(ULONGLONG freeBytes);  // sampler thread: 1 when free space just went under the threshold
DWORD memwarn_threshold_mb(void);       // LowMemoryWarnMB (0 = off)
int memwarn_due(void);                  // an in-game warning waits to be shown
void memwarn_show(struct lua_State *L); // luagc.c, at a wrapped DB call's return: shows it if it may

// audioguard.c
void audioguard_init(const WCHAR *ini);
void audioguard_tick(void);  // sampler thread

// soundlog.c
void soundlog_init(const WCHAR *ini);  // after audioguard_init
void soundlog_report(void);

// cmdstream.c
void cmdstream_init(const WCHAR *ini);
void cmdstream_report(void);

// watch.c
void watch_init(const WCHAR *ini);
void watch_tick(void);  // sampler thread
void watch_report(void);

// modules.c
void modules_init(void);  // DllMain: logs non-system DLLs now and as they load
const char *module_at(const void *addr, ULONG_PTR *base);  // lock-free; NULL if in no known module
void modules_log_stack(const CONTEXT *ctx, const NT_TIB *tib, int overflow);  // exception path: return-address candidates
void modules_log_stack_copy(const void *words, size_t bytes);  // a stack copied from a suspended thread

// crtfail.c
void crtfail_init(const WCHAR *ini);
void crtfail_note_cxx(const EXCEPTION_RECORD *er);  // exception path: remembers a C++ exception's type
void crtfail_log_last_cxx(void);  // at exit
void log_memory_now(const char *when);  // probe.c: free address space, no heap calls

// coretick.c
void coretick_init(const WCHAR *ini);
void coretick_report(void);

// smallblock.c
void smallblock_init(const WCHAR *ini);
void smallblock_tick(void);  // sampler thread

// uiqueue.c
void uiqueue_init(const WCHAR *ini);
void uiqueue_report(void);

// mapgrid.c
void mapgrid_init(const WCHAR *ini);
void mapgrid_report(void);
void mapgrid_tick(void);  // sampler thread

// gpuwatch.c
void gpuwatch_init(const WCHAR *ini);
void gpuwatch_tick(void);  // sampler thread
void gpuwatch_report(void);

// luaheap.c
void luaheap_init(const WCHAR *ini);  // after mialloc_init
void luaheap_report(void);

// d3dstats.c
void d3dstats_init(const WCHAR *ini);
void d3dstats_tick(void);  // sampler thread
void d3dstats_report(void);

// texhandles.c
void texhandles_init(const WCHAR *ini);  // DllMain: before the exe's static constructors
void texhandles_tick(void);  // sampler thread
void texhandles_report(void);
DWORD texhandles_count(void);  // the table's size (12,288 when unchanged)
DWORD texhandles_room(void);   // how many more handles Alloc can hand out
// Moves the records to `records` (n x 0x398 bytes, zeroed) and grows the pool to n handles in the
// image at exe; texhandles_check must have passed. NULL when done, else why nothing changed.
const char *texhandles_apply(BYTE *exe, const GameExe *g, DWORD n, BYTE *records);

// terrain.c
#define TERRAIN_MAX_SLOTS 22528  // 32,768 handles less the 10,240 the game's other textures get
void terrain_init(const WCHAR *ini);  // DllMain, after texhandles_init
void terrain_report(void);
DWORD terrain_extra_handles(const WCHAR *ini);  // TerrainBaseSlots past the game's 6,144
// Sends the hand-over to a map's tile manager and the release at exit to array (cap entries,
// zeroed), which gets the game's handles at the first hand-over, and hooks the exe's qsort import
// so the tile update's off-screen sort adds what the map needs, up to cap: made by makeExtra(out,
// n) (NULL = the game's own texture calls; it returns how many it made) and added to the
// manager's free list with reserve (NULL = the game's; thiscall on the list, the new size).
// terrain_check must have passed. NULL when done, else why nothing changed.
const char *terrain_apply(BYTE *exe, const GameExe *g, DWORD cap, DWORD *array, DWORD (*makeExtra)(DWORD *, DWORD),
                          void *reserve);
void *terrain_handover_stub(void);  // for the test
void *terrain_release_stub(void);

// intro.c
void intro_init(const WCHAR *ini);

// fluidpool.c
void fluidpool_init(const WCHAR *ini);
void fluidpool_report(void);
// Sends the seeding and the step launch to the stubs; fluidpool_check must have passed. alloc, del
// and launcher stand in for the exe's malloc import, the game's delete and the step launcher (NULL =
// the game's). NULL when done, else why nothing changed.
const char *fluidpool_apply(BYTE *exe, const GameExe *g, void *(__cdecl *alloc)(size_t), void(__cdecl *del)(void *),
                            void *launcher);
void *fluidpool_seed_stub(void);  // for the test
void *fluidpool_step_stub(void);

// msgqueue.c
void msgqueue_init(const WCHAR *ini);  // DllMain (takes the main thread's id)
void msgqueue_tick(void);  // sampler thread
void msgqueue_report(void);

// hang.c
void hang_init(const WCHAR *ini);
void hang_tick(void);  // sampler thread
int hang_catch_fault(void);  // exception handler: doesn't return if the fault is the watchdog's Lua stack read

// lastrun.c
void lastrun_init(const WCHAR *dir, const WCHAR *ini);  // after the events log is open

// topdown.c
void topdown_init(const WCHAR *ini);
void topdown_report(void);
void topdown_drain(void);  // sampler thread: writes logged reservations
