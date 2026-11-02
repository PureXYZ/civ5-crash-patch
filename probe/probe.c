// civprobe (released as "Civ V Crash Patch") - crash fixes and address-space/heap telemetry
// for Civilization V (32-bit).
//
// Installed as a proxy DLL next to CivilizationV*.exe: msimg32.dll (default) or
// version.dll (build with -DPROXY_VERSION). It forwards every export to the real
// system DLL and, inside Civ V only, runs a low-priority sampler thread. On by default:
// the render command stream fix (cmdstream.c), the Miles sample-handle guard
// (audioguard.c), the quit-crash guard (luaguard.c), the Lua GC fix (luagc.c) and mimalloc
// for the small CRT allocations (mialloc.c). Everything else is opt-in in civprobe.ini:
// telemetry files (Telemetry=1), allocation tracking (alloctrack.c), top-down placement and
// the reservation log (topdown.c) and the watchpoint (watch.c).
//
// msimg32.dll is the default because Windows' compatibility shim (AcGenral.dll,
// applied when e.g. "Override high DPI scaling" is set) imports version.dll and
// loads the system copy before the game's own imports are resolved.
//
// Output (in <game dir>\civprobe\; files of the newest KeepSessions launches are kept):
//   events_<session>.log         startup info, triggers, exceptions, fix counters
//   samples_<session>.csv        Telemetry=1: one row per interval: VA usage below/above 2 GB, heaps, Lua heap
//   vamap_<session>_<label>.csv  Telemetry=1: full virtual-address map at trigger points
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "probe.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/* Export forwarding                                                   */
/* ------------------------------------------------------------------ */

#ifdef PROXY_VERSION
#define PROXY_DLL L"version.dll"
#define PROXY_EXPORTS(X)     X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA)     X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA)     X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW)     X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW)     X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)
#else
#define PROXY_DLL L"msimg32.dll"
#define PROXY_EXPORTS(X)     X(vSetDdrawflag) X(AlphaBlend) X(DllInitialize) X(GradientFill) X(TransparentBlt)
#endif

// Each stub is a bare `jmp [p_Name]`, so calling convention and arguments pass
// through untouched.
#define DECLARE_STUB(n) void *p_##n; __asm__(".section .text\n.globl _stub_" #n "\n_stub_" #n ":\n\tjmp *_p_" #n "\n");
PROXY_EXPORTS(DECLARE_STUB)

static HMODULE g_realDll;

static void load_real_dll(void)
{
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return;
    lstrcatW(path, L"\\" PROXY_DLL);
    g_realDll = LoadLibraryW(path);
    if (!g_realDll) return;
#define RESOLVE(n) p_##n = (void *)GetProcAddress(g_realDll, #n);
    PROXY_EXPORTS(RESOLVE)
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define LOW_LIMIT 0x80000000ull
#define TOP_LIMIT 0x100000000ull
#define MAX_HEAPS 128
#define MAX_GPU_CHUNKS 64

typedef struct {
    DWORD cb;
    SIZE_T cbAllocated, cbCommitted, cbReserved, cbMaxReserve;
} HEAP_SUMMARY_T;
typedef BOOL(WINAPI *HeapSummary_t)(HANDLE, DWORD, HEAP_SUMMARY_T *);
typedef DWORD(WINAPI *GetMappedFileNameW_t)(HANDLE, LPVOID, LPWSTR, DWORD);
typedef HANDLE(__cdecl *get_heap_handle_t)(void);

typedef struct {
    // Address space, split at the 2 GB line. All sizes in bytes.
    ULONGLONG commit[2], reserve[2], free_[2], largestFree[2];
    DWORD freeBlocks1M[2];         // free blocks >= 1 MB
    ULONGLONG imageCommit, mappedCommit, privateCommit;
    // Heaps (from HeapSummary).
    DWORD heapCount;
    ULONGLONG luaHeapCommit, luaHeapReserve, luaHeapAlloc;
    ULONGLONG crtHeapCommit, crtHeapReserve, crtHeapAlloc;
    ULONGLONG procHeapCommit, otherHeapsCommit, otherHeapsReserve;
    // The exe's own Lua allocator counters.
    DWORD luaCur, luaPeak;
    // GPU driver chunks (see walk_va).
    DWORD gpuChunks;
    ULONGLONG gpuCommit, gpuReserve;
    ULONG_PTR gpuBase[MAX_GPU_CHUNKS];
} Sample;

static WCHAR g_dir[MAX_PATH];        // <game>\civprobe
WCHAR g_session[32];
static HANDLE g_events = INVALID_HANDLE_VALUE;
static HANDLE g_samples = INVALID_HANDLE_VALUE;
static ULONGLONG g_t0;
static volatile LONG g_inSnapshot;

static HeapSummary_t pHeapSummary;
static GetMappedFileNameW_t pGetMappedFileNameW;

// Lua heap and counters located inside the exe's lua_Alloc (see find_lua_counters).
static HANDLE *g_luaHeapVar;
static volatile DWORD *g_luaCurVar, *g_luaPeakVar;

// Heap handles cached by the sampler so the exception path can label regions
// without taking any heap lock.
static HANDLE g_heaps[MAX_HEAPS];
static volatile LONG g_heapCount;
static HANDLE g_crtHeap, g_procHeap;

static DWORD g_intervalMs = 2000;
static DWORD g_periodicSnapshotMin = 10;
static DWORD g_peakStepMb;  // write a "peak" alloc report each time CRT live bytes rise this much
static int g_heapHist;      // log a CRT heap block-size histogram with each periodic snapshot
// HeapSummary per heap on every tick ([probe] HeapStats=1; off by default). It walks the whole
// heap under its lock: on the game's ~1.9 GB LFH heap the walk stalled for minutes and froze
// the game behind the lock (2026-10-28, sampler stuck in RtlpWalkLFHBlock). The free address
// space columns don't need it.
static int g_heapStats;
// Samples CSV and VA maps ([probe] Telemetry=1). Off by default: players get the fixes and the
// small events log only.
static int g_telemetry;
#define MAX_SEEN 32
#define MAX_EXCEPTION_SNAPSHOTS 6
static void *g_seenAddr[MAX_SEEN];
static volatile LONG g_seenHits[MAX_SEEN];
static volatile LONG g_seenCount;

/* ------------------------------------------------------------------ */
/* Logging (no heap allocation, safe to call from the exception path)  */
/* ------------------------------------------------------------------ */

static double now_s(void) { return (GetTickCount64() - g_t0) / 1000.0; }

void write_all(HANDLE h, const char *buf, int len)
{
    DWORD w;
    if (h != INVALID_HANDLE_VALUE && len > 0) WriteFile(h, buf, (DWORD)len, &w, NULL);
}

void log_event(const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "[%9.2f] ", now_s());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;  // a cut line: its last byte is vsnprintf's NUL
    buf[n++] = '\r';
    buf[n++] = '\n';
    write_all(g_events, buf, n);
}

HANDLE open_out(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%s", g_dir, name);
    path[MAX_PATH - 1] = 0;
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

// Deletes every output file of all but the newest `keep` sessions. A session is named by its
// events log (events_YYYYMMDD_HHMMSS.log); its other files carry the same stamp.
#define MAX_PRUNE 512
static void prune_sessions(int keep)
{
    // Newest first by the events log's creation time (UTC), not by the local-time stamp in its
    // name: after the clocks go back an hour, a newer launch can have the older-looking name.
    static WCHAR stamps[MAX_PRUNE][16];
    static ULONGLONG created[MAX_PRUNE];
    int n = 0;
    WCHAR pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%s\\events_*.log", g_dir);
    pat[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (wcslen(fd.cFileName) == 26 && n < MAX_PRUNE) {
            created[n] = (ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32 | fd.ftCreationTime.dwLowDateTime;
            lstrcpynW(stamps[n++], fd.cFileName + 7, 16);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (n <= keep) return;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (created[j] > created[i] || (created[j] == created[i] && wcscmp(stamps[j], stamps[i]) > 0)) {
                WCHAR t[16];
                lstrcpyW(t, stamps[i]); lstrcpyW(stamps[i], stamps[j]); lstrcpyW(stamps[j], t);
                ULONGLONG c = created[i]; created[i] = created[j]; created[j] = c;
            }
    for (int i = keep; i < n; i++) {
        _snwprintf(pat, MAX_PATH, L"%s\\*_%s*", g_dir, stamps[i]);
        pat[MAX_PATH - 1] = 0;
        f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            WCHAR path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%s\\%s", g_dir, fd.cFileName);
            path[MAX_PATH - 1] = 0;
            DeleteFileW(path);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
}

/* ------------------------------------------------------------------ */
/* Locating the exe's Lua allocator                                     */
/* ------------------------------------------------------------------ */

// The free path of the exe's lua_Alloc (CivilizationV_DX11.exe RVA 0x2ce454):
//   A1 <heap>          mov  eax, [g_luaHeap]
//   56 57 50           push esi / push edi / push eax
//   FF 15 <HeapSize>   call [HeapSize]
//   8B 0D <cur>        mov  ecx, [g_luaBytes]
//   2B C8              sub  ecx, eax
//   89 0D <cur>        mov  [g_luaBytes], ecx
//   3B 0D <peak>       cmp  ecx, [g_luaPeak]
// Scanning for it (rather than hardcoding RVAs) also covers the DX9 and tablet exes.
static const BYTE kLuaPat[] = {0xA1, 0, 0, 0, 0, 0x56, 0x57, 0x50, 0xFF, 0x15, 0, 0, 0, 0,
                               0x8B, 0x0D, 0, 0, 0, 0, 0x2B, 0xC8, 0x89, 0x0D, 0, 0, 0, 0, 0x3B, 0x0D};
static const char kLuaMask[] = "x????xxxxx????xx????xxxx????xx";

typedef struct { DWORD heapRva, curRva, peakRva, siteRva; int matches; } LuaLocation;

static int find_lua_counters(BYTE *base, LuaLocation *out)
{
    memset(out, 0, sizeof *out);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return 0;
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    DWORD b = (DWORD)(ULONG_PTR)base;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        BYTE *p = base + sec->VirtualAddress;
        DWORD len = sec->Misc.VirtualSize;
        DWORD plen = sizeof kLuaPat;
        for (DWORD o = 0; o + plen + 4 <= len; o++) {
            DWORD k = 0;
            while (k < plen && (kLuaMask[k] == '?' || p[o + k] == kLuaPat[k])) k++;
            if (k != plen) continue;
            DWORD heap = *(DWORD *)(p + o + 1);
            DWORD cur = *(DWORD *)(p + o + 16);
            DWORD cur2 = *(DWORD *)(p + o + 24);
            DWORD peak = *(DWORD *)(p + o + 30);
            if (cur != cur2) continue;
            if (heap - b >= imageSize || cur - b >= imageSize || peak - b >= imageSize) continue;
            if (out->matches++ == 0) {
                out->heapRva = heap - b;
                out->curRva = cur - b;
                out->peakRva = peak - b;
                out->siteRva = sec->VirtualAddress + o;
            }
        }
    }
    return out->matches == 1;
}

// Exported for the test harness: maps an exe image and reports what the scanner finds.
__declspec(dllexport) int CivProbe_SelfTest(const WCHAR *exePath, char *report, int cap)
{
    HMODULE m = LoadLibraryExW(exePath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        snprintf(report, cap, "LoadLibraryEx failed: %lu", GetLastError());
        return 0;
    }
    LuaLocation loc;
    int ok = find_lua_counters((BYTE *)m, &loc);
    snprintf(report, cap, "base=%p matches=%d site=+0x%lx heapVar=+0x%lx bytesVar=+0x%lx peakVar=+0x%lx",
             (void *)m, loc.matches, loc.siteRva, loc.heapRva, loc.curRva, loc.peakRva);
    FreeLibrary(m);
    return ok;
}

// Exported for the test harness: maps an exe file (imports not bound) and runs every part's check
// against its table. 1 if the exe is known and every part found what it expects.
__declspec(dllexport) int CivProbe_CheckExe(const WCHAR *exePath, char *report, int cap)
{
    HMODULE m = LoadLibraryExW(exePath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        snprintf(report, cap, "LoadLibraryEx failed: %lu", GetLastError());
        return 0;
    }
    BYTE *exe = (BYTE *)m;
    const GameExe *g = game_exe(exe);
    if (!g) {
        snprintf(report, cap, "not a known build");
        FreeLibrary(m);
        return 0;
    }
    static const struct { const char *name; const char *(*check)(BYTE *, const GameExe *); } parts[] = {
        {"cmdstream", cmdstream_check}, {"msgqueue", msgqueue_check}, {"uiqueue", uiqueue_check},
        {"uiqueue tables", uiqueue_tables_check}, {"mapgrid", mapgrid_check}, {"luaheap", luaheap_check},
        {"coretick", coretick_check}, {"smallblock", smallblock_check}, {"luaguard", luaguard_check},
        {"d3dstats", d3dstats_check}, {"texhandles", texhandles_check},
    };
    int o = snprintf(report, cap, "%s:", g->name), ok = 1;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0] && o < cap; i++) {
        const char *why = parts[i].check(exe, g);
        if (why && !(parts[i].check == d3dstats_check && !g->d3dContext) && !(parts[i].check == texhandles_check && !g->texPool))
            ok = 0;
        o += snprintf(report + o, cap - o, " %s=%s", parts[i].name, why ? why : "ok");
    }
    if (g->gameFault && o < cap) {  // the game's own NULL write: mov [reg], ... (a write through a register)
        const BYTE *f = exe + g->gameFault;
        int w = (f[0] == 0x89 || f[0] == 0xC7 || f[0] == 0x88 || f[0] == 0xC6) && (f[1] & 0xC0) == 0;
        if (!w) ok = 0;
        snprintf(report + o, cap - o, " gameFault=%s", w ? "ok" : "not a write");
    }
    FreeLibrary(m);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */

static void add_split(ULONGLONG lo, ULONGLONG size, ULONGLONG v[2])
{
    ULONGLONG hi = lo + size;
    if (hi <= LOW_LIMIT) v[0] += size;
    else if (lo >= LOW_LIMIT) v[1] += size;
    else { v[0] += LOW_LIMIT - lo; v[1] += hi - LOW_LIMIT; }
}

// GPU driver chunks: private regions of exactly 32 MB that the kernel creates for the D3D
// driver's CPU-visible buffers (none of them goes through the ntdll allocation calls the
// reservation log hooks). The driver maps and unmaps buffers inside them and never releases one,
// so their count is the address space it has taken. Told apart from the probe's and the game's
// own 32 MB regions (the small-block region, Steam's reserve: one or two pieces) by being cut
// into 3 or more pieces or having write-combined pages. A new chunk may only count once it is cut up.
static void end_region(Sample *s, ULONG_PTR ab, ULONGLONG total, ULONGLONG commit, int pieces, int wc)
{
    if (!ab || total != (32ull << 20) || (pieces < 3 && !wc)) return;
    if (s->gpuChunks < MAX_GPU_CHUNKS) s->gpuBase[s->gpuChunks] = ab;
    s->gpuChunks++;
    s->gpuCommit += commit;
    s->gpuReserve += total - commit;
}

static void walk_va(Sample *s)
{
    ULONGLONG a = 0;
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR ab = 0;  // the private region being added up
    ULONGLONG total = 0, commit = 0;
    int pieces = 0, wc = 0;
    while (a < TOP_LIMIT && VirtualQuery((LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) {
        ULONGLONG base = (ULONG_PTR)mbi.BaseAddress, size = mbi.RegionSize;
        ULONG_PTR nab = mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE ? (ULONG_PTR)mbi.AllocationBase : 0;
        if (nab != ab) {
            end_region(s, ab, total, commit, pieces, wc);
            ab = nab;
            total = commit = 0;
            pieces = wc = 0;
        }
        if (ab) {
            total += size;
            pieces++;
            if (mbi.State == MEM_COMMIT) {
                commit += size;
                if (mbi.Protect & PAGE_WRITECOMBINE) wc = 1;
            }
        }
        if (mbi.State == MEM_COMMIT) {
            add_split(base, size, s->commit);
            if (mbi.Type == MEM_IMAGE) s->imageCommit += size;
            else if (mbi.Type == MEM_MAPPED) s->mappedCommit += size;
            else s->privateCommit += size;
        } else if (mbi.State == MEM_RESERVE) {
            add_split(base, size, s->reserve);
        } else if (mbi.State == MEM_FREE) {
            add_split(base, size, s->free_);
            // A free block straddling 2 GB counts toward both halves.
            ULONGLONG hi = base + size;
            ULONGLONG loPart = base < LOW_LIMIT ? (hi < LOW_LIMIT ? hi : LOW_LIMIT) - base : 0;
            ULONGLONG hiPart = hi > LOW_LIMIT ? hi - (base > LOW_LIMIT ? base : LOW_LIMIT) : 0;
            if (loPart > s->largestFree[0]) s->largestFree[0] = loPart;
            if (hiPart > s->largestFree[1]) s->largestFree[1] = hiPart;
            if (loPart >= (1u << 20)) s->freeBlocks1M[0]++;
            if (hiPart >= (1u << 20)) s->freeBlocks1M[1]++;
        }
        if (size == 0) break;
        a = base + size;
    }
    end_region(s, ab, total, commit, pieces, wc);
}

static void sample_heaps(Sample *s)
{
    if (!g_crtHeap) {
        HMODULE crt = alloc_crt_module();  // msvcr90 in the game; the test exes set AllocCrt
        if (!crt) crt = GetModuleHandleW(L"msvcr90.dll");
        get_heap_handle_t f = crt ? (get_heap_handle_t)GetProcAddress(crt, "_get_heap_handle") : NULL;
        if (f) g_crtHeap = f();
    }
    HANDLE heaps[MAX_HEAPS];
    DWORD n = GetProcessHeaps(MAX_HEAPS, heaps);
    if (n > MAX_HEAPS) n = MAX_HEAPS;
    memcpy(g_heaps, heaps, n * sizeof(HANDLE));
    InterlockedExchange(&g_heapCount, (LONG)n);
    s->heapCount = n;
    HANDLE lua = g_luaHeapVar ? *g_luaHeapVar : NULL;
    for (DWORD i = 0; i < n && pHeapSummary && g_heapStats; i++) {
        HEAP_SUMMARY_T hs = {0};
        hs.cb = sizeof hs;
        if (!pHeapSummary(heaps[i], 0, &hs)) continue;
        if (heaps[i] == lua) {
            s->luaHeapCommit = hs.cbCommitted; s->luaHeapReserve = hs.cbReserved; s->luaHeapAlloc = hs.cbAllocated;
        } else if (heaps[i] == g_crtHeap) {
            s->crtHeapCommit = hs.cbCommitted; s->crtHeapReserve = hs.cbReserved; s->crtHeapAlloc = hs.cbAllocated;
        } else if (heaps[i] == g_procHeap) {
            s->procHeapCommit = hs.cbCommitted;
        } else {
            s->otherHeapsCommit += hs.cbCommitted; s->otherHeapsReserve += hs.cbReserved;
        }
    }
}

static void take_sample(Sample *s, int withHeaps)
{
    memset(s, 0, sizeof *s);
    walk_va(s);
    if (withHeaps) sample_heaps(s);
    if (g_luaCurVar) { s->luaCur = *g_luaCurVar; s->luaPeak = *g_luaPeakVar; }
}

#define MB(x) ((double)(x) / 1048576.0)

static void write_sample(const Sample *s, const char *tag)
{
    if (g_samples == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
        "%.2f,%s,%.1f,%.1f,%.1f,%.2f,%lu,%.1f,%.1f,%.1f,%.2f,%lu,%.1f,%.1f,%.1f,"
        "%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%.1f,%.1f\r\n",
        now_s(), tag,
        MB(s->commit[0]), MB(s->reserve[0]), MB(s->free_[0]), MB(s->largestFree[0]), s->freeBlocks1M[0],
        MB(s->commit[1]), MB(s->reserve[1]), MB(s->free_[1]), MB(s->largestFree[1]), s->freeBlocks1M[1],
        MB(s->imageCommit), MB(s->mappedCommit), MB(s->privateCommit),
        s->heapCount, MB(s->luaCur), MB(s->luaPeak),
        MB(s->luaHeapAlloc), MB(s->luaHeapCommit), MB(s->luaHeapReserve),
        MB(s->crtHeapAlloc), MB(s->crtHeapCommit), MB(s->crtHeapReserve),
        MB(s->procHeapCommit), MB(s->otherHeapsCommit), MB(s->otherHeapsReserve),
        s->gpuChunks, MB(s->gpuCommit), MB(s->gpuReserve));
    // No lock, as for the events log: writes to one synchronous handle don't interleave, and a
    // lock the sampler held when ExitProcess killed it would end the process at DLL detach.
    write_all(g_samples, buf, n);
}

static const char kSampleHeader[] =
    "t_s,tag,low_commit_mb,low_reserve_mb,low_free_mb,low_largest_free_mb,low_free_blocks_1mb,"
    "high_commit_mb,high_reserve_mb,high_free_mb,high_largest_free_mb,high_free_blocks_1mb,"
    "image_commit_mb,mapped_commit_mb,private_commit_mb,"
    "heaps,lua_bytes_mb,lua_peak_mb,lua_heap_alloc_mb,lua_heap_commit_mb,lua_heap_reserve_mb,"
    "crt_heap_alloc_mb,crt_heap_commit_mb,crt_heap_reserve_mb,"
    "process_heap_commit_mb,other_heaps_commit_mb,other_heaps_reserve_mb,"
    "gpu_chunks,gpu_chunk_commit_mb,gpu_chunk_reserve_mb\r\n";

/* ------------------------------------------------------------------ */
/* CRT heap block-size histogram ([probe] HeapHistogram=1)             */
/* ------------------------------------------------------------------ */

// Busy and free bytes in the CRT heap by block size (powers of two), to tell
// fragmentation (free space spread over many small holes) from a few large free
// blocks. HeapWalk holds the heap lock for the whole walk (about a second on a
// 2 GB heap), so the game hitches once per snapshot. Never call it from the
// exception path.
#define HIST_BUCKETS 26
static void heap_histogram(const char *label)
{
    if (!g_heapHist || !g_crtHeap) return;
    static int once;
    if (!once) {
        once = 1;
        ULONG mode = 0;  // 0 standard, 1 look-aside, 2 low-fragmentation heap
        HeapQueryInformation(g_crtHeap, HeapCompatibilityInformation, &mode, sizeof mode, NULL);
        log_event("heaphist: CRT heap %p front end = %lu", (void *)g_crtHeap, mode);
    }
    ULONGLONG busyB[HIST_BUCKETS] = {0}, freeB[HIST_BUCKETS] = {0}, uncommitted = 0, largestFree = 0;
    DWORD busyN[HIST_BUCKETS] = {0}, freeN[HIST_BUCKETS] = {0}, regions = 0;
    ULONGLONG t0 = GetTickCount64();
    if (!HeapLock(g_crtHeap)) return;
    PROCESS_HEAP_ENTRY e;
    e.lpData = NULL;
    while (HeapWalk(g_crtHeap, &e)) {
        if (e.wFlags & PROCESS_HEAP_REGION) { regions++; continue; }
        if (e.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) { uncommitted += e.cbData; continue; }
        DWORD sz = e.cbData;
        int b = 0;
        while (b < HIST_BUCKETS - 1 && (sz >> (b + 1))) b++;
        if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
            busyB[b] += sz; busyN[b]++;
        } else {
            freeB[b] += sz; freeN[b]++;
            if (sz > largestFree) largestFree = sz;
        }
    }
    HeapUnlock(g_crtHeap);
    DWORD ms = (DWORD)(GetTickCount64() - t0);
    ULONGLONG busy = 0, free_ = 0;
    for (int b = 0; b < HIST_BUCKETS; b++) { busy += busyB[b]; free_ += freeB[b]; }
    log_event("heaphist %s: %lu regions, busy %.1f MB, free %.1f MB (largest %.1f KB), uncommitted %.1f MB; walk %lu ms",
              label, regions, MB(busy), MB(free_), largestFree / 1024.0, MB(uncommitted), ms);
    for (int b = 0; b < HIST_BUCKETS; b++)
        if (busyN[b] || freeN[b])
            log_event("heaphist %s: %9lu+ B: busy %8lu blocks %8.1f MB | free %8lu blocks %8.1f MB", label,
                      1ul << b, busyN[b], MB(busyB[b]), freeN[b], MB(freeB[b]));
}

/* ------------------------------------------------------------------ */
/* Full VA map snapshot                                                */
/* ------------------------------------------------------------------ */

static const char *state_name(DWORD s)
{
    return s == MEM_COMMIT ? "commit" : s == MEM_RESERVE ? "reserve" : "free";
}

static const char *type_name(DWORD t)
{
    return t == MEM_IMAGE ? "image" : t == MEM_MAPPED ? "mapped" : t == MEM_PRIVATE ? "private" : "";
}

static void heap_label(ULONG_PTR allocBase, char *out, int cap)
{
    out[0] = 0;
    HANDLE lua = g_luaHeapVar ? *g_luaHeapVar : NULL;
    if (lua && (HANDLE)allocBase == lua) { snprintf(out, cap, "heap:lua"); return; }
    if (g_crtHeap && (HANDLE)allocBase == g_crtHeap) { snprintf(out, cap, "heap:crt(msvcr90)"); return; }
    if ((HANDLE)allocBase == g_procHeap) { snprintf(out, cap, "heap:process"); return; }
    LONG n = g_heapCount;
    for (LONG i = 0; i < n; i++)
        if ((ULONG_PTR)g_heaps[i] == allocBase) { snprintf(out, cap, "heap:#%ld", i); return; }
}

// Labels a private region with the heap that owns it. A heap's first segment
// starts at the heap handle itself; segments added as the heap grows carry a
// HEAP_SEGMENT header (x86 NT heap: signature 0xFFEEFFEE at +0x8, owning heap
// at +0x18). Only reads the header when it is committed and readable.
static ULONG_PTR g_ownerBase;
static char g_ownerLabel[48];

static void private_owner(const MEMORY_BASIC_INFORMATION *mbi, char *out, int cap)
{
    ULONG_PTR ab = (ULONG_PTR)mbi->AllocationBase;
    if (ab != g_ownerBase) {
        g_ownerBase = ab;
        heap_label(ab, g_ownerLabel, sizeof g_ownerLabel);
        DWORD rw = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
        if (!g_ownerLabel[0] && (ULONG_PTR)mbi->BaseAddress == ab && mbi->State == MEM_COMMIT &&
            (mbi->Protect & rw) && !(mbi->Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi->RegionSize >= 0x20) {
            const DWORD *hdr = (const DWORD *)ab;
            if (hdr[2] == 0xFFEEFFEE) {
                char owner[32];
                heap_label(hdr[6], owner, sizeof owner);
                if (owner[0]) snprintf(g_ownerLabel, sizeof g_ownerLabel, "%s", owner);
                else snprintf(g_ownerLabel, sizeof g_ownerLabel, "heap:unlisted(0x%08lx)", (unsigned long)hdr[6]);
            }
        }
        // mimalloc maps a pointer to the segment holding the byte before it (p - 1), so ask about
        // the region's second byte: its first would name the 4 MB before it.
        if (!g_ownerLabel[0] && CivProbe_IsMimalloc((const BYTE *)ab + 1)) snprintf(g_ownerLabel, sizeof g_ownerLabel, "mimalloc");
    }
    snprintf(out, cap, "%s", g_ownerLabel);
}

// Static buffer: the exception path must not touch the heap.
static char g_snapBuf[64 * 1024];

// Writes the VA map to h (closed after) and releases g_inSnapshot, which the caller took. On the
// exception path (onException) the allocation report is left out: it opens files.
static void snapshot_to(HANDLE h, const char *label, int onException)
{
    int n = snprintf(g_snapBuf, sizeof g_snapBuf, "base,size_kb,state,type,protect,alloc_base,owner\r\n");
    ULONGLONG a = 0;
    MEMORY_BASIC_INFORMATION mbi;
    WCHAR wname[MAX_PATH];
    char owner[MAX_PATH];
    HANDLE self = GetCurrentProcess();
    g_ownerBase = (ULONG_PTR)-1;
    while (a < TOP_LIMIT && VirtualQuery((LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) {
        ULONG_PTR ab = (ULONG_PTR)mbi.AllocationBase;
        owner[0] = 0;
        if (mbi.State != MEM_FREE) {
            if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
                DWORD len = pGetMappedFileNameW ? pGetMappedFileNameW(self, mbi.BaseAddress, wname, MAX_PATH) : 0;
                if (len) {
                    const WCHAR *slash = wcsrchr(wname, L'\\');
                    if (!WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : wname, -1, owner, sizeof owner, NULL, NULL))
                        owner[0] = 0;  // too long: a failed conversion leaves no NUL
                    for (char *c = owner; *c; c++) if (*c == ',') *c = ';';
                }
            } else {
                private_owner(&mbi, owner, sizeof owner);
            }
        }
        if (n > (int)sizeof g_snapBuf - 512) { write_all(h, g_snapBuf, n); n = 0; }
        n += snprintf(g_snapBuf + n, sizeof g_snapBuf - n, "0x%08lx,%lu,%s,%s,0x%lx,0x%08lx,%s\r\n",
                      (unsigned long)(ULONG_PTR)mbi.BaseAddress, (unsigned long)(mbi.RegionSize >> 10),
                      state_name(mbi.State), mbi.State == MEM_FREE ? "" : type_name(mbi.Type),
                      mbi.State == MEM_FREE ? 0ul : (unsigned long)mbi.Protect,
                      mbi.State == MEM_FREE ? 0ul : (unsigned long)ab, owner);
        if (mbi.RegionSize == 0) break;
        a = (ULONG_PTR)mbi.BaseAddress + (ULONGLONG)mbi.RegionSize;
    }
    write_all(h, g_snapBuf, n);
    CloseHandle(h);
    log_event("snapshot written: %s", label);
    if (!onException) alloc_report(label);
    InterlockedExchange(&g_inSnapshot, 0);
}

static void snapshot(const char *label)
{
    if (!g_telemetry || InterlockedExchange(&g_inSnapshot, 1)) return;
    WCHAR name[96];
    _snwprintf(name, 96, L"vamap_%s_%S.csv", g_session, label);
    name[95] = 0;
    HANDLE h = open_out(name);
    if (h == INVALID_HANDLE_VALUE) { InterlockedExchange(&g_inSnapshot, 0); return; }
    snapshot_to(h, label, 0);
}

// The exception path can't open a file: CreateFileW allocates the NT path on the process heap,
// whose lock the faulting thread may hold. So the sampler keeps the next exception's VA map file
// open (exception1..MAX_EXCEPTION_SNAPSHOTS, in order); an unused one is deleted at exit.
static HANDLE volatile g_spareSnap = INVALID_HANDLE_VALUE;
// The game's own NULL write (CivilizationV_DX11.exe+0x523dcb; GameExe.gameFault), raised and handled at every start:
// logged and counted, but it doesn't use up the spare file.
static void *g_gameFault;
static char g_spareLabel[32];
static LONG g_nSpareSnaps;

static void spare_snapshot_name(WCHAR *name, int cap)
{
    _snwprintf(name, cap, L"vamap_%s_%S.csv", g_session, g_spareLabel);
    name[cap - 1] = 0;
}

static void prepare_exception_snapshot(void)
{
    if (!g_telemetry || g_spareSnap != INVALID_HANDLE_VALUE || g_nSpareSnaps >= MAX_EXCEPTION_SNAPSHOTS) return;
    snprintf(g_spareLabel, sizeof g_spareLabel, "exception%ld", g_nSpareSnaps + 1);
    WCHAR name[96];
    spare_snapshot_name(name, 96);
    HANDLE h = open_out(name);
    if (h == INVALID_HANDLE_VALUE) return;
    g_nSpareSnaps++;
    g_spareSnap = h;
}

static void exception_snapshot(void)
{
    if (g_spareSnap == INVALID_HANDLE_VALUE || InterlockedExchange(&g_inSnapshot, 1)) return;
    char label[sizeof g_spareLabel];
    memcpy(label, g_spareLabel, sizeof label);  // the sampler reuses g_spareLabel once the handle is taken
    HANDLE h = InterlockedExchangePointer((PVOID volatile *)&g_spareSnap, INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) { InterlockedExchange(&g_inSnapshot, 0); return; }
    snapshot_to(h, label, 1);
}

static void drop_exception_snapshot(void)  // DLL detach: the unused spare file
{
    HANDLE h = InterlockedExchangePointer((PVOID volatile *)&g_spareSnap, INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    CloseHandle(h);
    WCHAR name[96], path[MAX_PATH];
    spare_snapshot_name(name, 96);
    _snwprintf(path, MAX_PATH, L"%s\\%s", g_dir, name);
    path[MAX_PATH - 1] = 0;
    DeleteFileW(path);
}

// Address-space headroom for the events log (written without Telemetry too, so bug reports show
// it): free space below and above 2 GB, the largest free block and the lowest total so far.
static ULONGLONG g_minFree = ~0ull;
static double g_minFreeT;

static void note_headroom(const Sample *s, double t)
{
    ULONGLONG total = s->free_[0] + s->free_[1];
    if (total < g_minFree) { g_minFree = total; g_minFreeT = t; }
}

static void log_headroom(const Sample *s, const char *when)
{
    ULONGLONG big = s->largestFree[0] > s->largestFree[1] ? s->largestFree[0] : s->largestFree[1];
    log_event("memory (%s): %.0f MB free (%.0f below 2 GB, %.0f above), largest free block %.0f MB, "
              "private commit %.0f MB; lowest so far %.0f MB at %.0f min",
              when, MB(s->free_[0] + s->free_[1]), MB(s->free_[0]), MB(s->free_[1]), MB(big),
              MB(s->privateCommit), MB(g_minFree), g_minFreeT / 60);
}

// Sampler thread: logs each GPU driver chunk the first time a sample sees it (the first tick
// only counts them), with the time, so new ones can be matched with what the game was doing.
static ULONG_PTR g_gpuSeen[MAX_GPU_CHUNKS];
static DWORD g_gpuSeenCount;

static int in_sample(const Sample *s, ULONG_PTR base)
{
    DWORD n = s->gpuChunks < MAX_GPU_CHUNKS ? s->gpuChunks : MAX_GPU_CHUNKS;
    for (DWORD i = 0; i < n; i++)
        if (s->gpuBase[i] == base) return 1;
    return 0;
}

static void note_gpu_chunks(const Sample *s, int first)
{
    DWORD n = s->gpuChunks < MAX_GPU_CHUNKS ? s->gpuChunks : MAX_GPU_CHUNKS;
    for (DWORD i = 0; i < n; i++) {
        DWORD k = 0;
        while (k < g_gpuSeenCount && g_gpuSeen[k] != s->gpuBase[i]) k++;
        if (k < g_gpuSeenCount) continue;
        if (g_gpuSeenCount == MAX_GPU_CHUNKS) {
            // Full: older bases that are gone make room; otherwise this one isn't tracked (or logged).
            DWORD j = 0;
            while (j < g_gpuSeenCount && in_sample(s, g_gpuSeen[j])) j++;
            if (j == g_gpuSeenCount) continue;
            g_gpuSeen[j] = s->gpuBase[i];
        } else {
            g_gpuSeen[g_gpuSeenCount++] = s->gpuBase[i];
        }
        if (!first)
            log_event("gpu chunk: a new 32 MB driver region at 0x%08lx; %lu now, %.0f MB mapped in them, %.0f MB only reserved",
                      (unsigned long)s->gpuBase[i], (unsigned long)s->gpuChunks, MB(s->gpuCommit), MB(s->gpuReserve));
    }
}

static void log_gpu_chunks(const Sample *s)
{
    log_event("gpu chunks: %lu driver regions of 32 MB, %.0f MB mapped in them, %.0f MB only reserved",
              (unsigned long)s->gpuChunks, MB(s->gpuCommit), MB(s->gpuReserve));
}

// For crtfail.c: free address space now, without heap calls.
void log_memory_now(const char *when)
{
    Sample s;
    take_sample(&s, 0);
    note_headroom(&s, now_s());
    log_headroom(&s, when);
}

/* ------------------------------------------------------------------ */
/* Exception and exit capture                                          */
/* ------------------------------------------------------------------ */

// Logs the k-th new exception site. tib and tid are the faulting thread's: for a stack overflow
// this runs on the overflow thread instead.
static void log_exception(EXCEPTION_POINTERS *ep, const NT_TIB *tib, DWORD tid, LONG k)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    char mod[MAX_PATH] = "?";
    HMODULE hm;
    ULONG_PTR mb;
    const char *mname = module_at(addr, &mb);  // no loader lock, which another thread may hold
    if (mname) {
        snprintf(mod, sizeof mod, "%s+0x%lx", mname, (unsigned long)((ULONG_PTR)addr - mb));
    } else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)addr, &hm)) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(hm, full, MAX_PATH)) {
            const char *s = strrchr(full, '\\');
            snprintf(mod, sizeof mod, "%s+0x%lx", s ? s + 1 : full,
                     (unsigned long)((ULONG_PTR)addr - (ULONG_PTR)hm));
        }
    }
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    ULONG_PTR op = er->NumberParameters > 1 ? er->ExceptionInformation[0] : 0;
    ULONG_PTR target = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
    log_event("exception 0x%08lx at %p (%s) op=%lu target=%p thread=%lu",
              code, addr, mod, (unsigned long)op, (void *)target, tid);
    modules_log_stack(ep->ContextRecord, tib, code == EXCEPTION_STACK_OVERFLOW);

    topdown_report();

    Sample s;
    take_sample(&s, 0);  // no heap calls: the heap lock may be held or the heap corrupt
    write_sample(&s, "exception");
    note_headroom(&s, now_s());
    log_headroom(&s, "at exception");
    luaguard_report();
    coretick_report();
    uiqueue_report();
    msgqueue_report();
    if (addr != g_gameFault) exception_snapshot();
}

// A stack overflow leaves the faulting thread about a page of stack, too little for
// log_exception (1.0.0 died silently in it). That thread hands the work to this one, which
// waits from startup, and blocks until it is done. Only the first overflow is logged.
static HANDLE g_overflowReq, g_overflowDone;
static volatile LONG g_overflowTaken;
static EXCEPTION_POINTERS *g_overflowEp;
static const NT_TIB *g_overflowTib;
static DWORD g_overflowTid;
static LONG g_overflowK;

static DWORD WINAPI overflow_thread(LPVOID unused)
{
    (void)unused;
    WaitForSingleObject(g_overflowReq, INFINITE);
    log_exception(g_overflowEp, g_overflowTib, g_overflowTid, g_overflowK);
    SetEvent(g_overflowDone);
    return 0;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    hang_catch_fault();  // the freeze watchdog's read of a suspended thread's Lua state
    if (code == 0xE06D7363) { crtfail_note_cxx(ep->ExceptionRecord); return EXCEPTION_CONTINUE_SEARCH; }  // C++ throw
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000374 /* heap corruption */ &&
        code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000017 /* no memory */)
        return EXCEPTION_CONTINUE_SEARCH;
    // The game raises and handles some exceptions on purpose (e.g. a NULL write at
    // CivilizationV_DX11.exe+0x523dcb), so record each faulting address only once
    // and keep snapshot slots for new ones.
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    LONG n = g_seenCount;
    for (LONG i = 0; i < n && i < MAX_SEEN; i++)
        if (g_seenAddr[i] == addr) { InterlockedIncrement(&g_seenHits[i]); return EXCEPTION_CONTINUE_SEARCH; }
    LONG slot = InterlockedIncrement(&g_seenCount) - 1;
    if (slot >= MAX_SEEN) {
        if (slot == MAX_SEEN)
            log_event("exception 0x%08lx at %p: more than %d exception sites, later new ones aren't logged",
                      code, addr, MAX_SEEN);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_seenAddr[slot] = addr;
    g_seenHits[slot] = 1;
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    if (code == EXCEPTION_STACK_OVERFLOW) {
        if (g_overflowReq && !InterlockedExchange(&g_overflowTaken, 1)) {
            g_overflowEp = ep;
            g_overflowTib = tib;
            g_overflowTid = GetCurrentThreadId();
            g_overflowK = slot + 1;
            SetEvent(g_overflowReq);
            WaitForSingleObject(g_overflowDone, 30000);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    log_exception(ep, tib, GetCurrentThreadId(), slot + 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ */
/* Sampler thread                                                      */
/* ------------------------------------------------------------------ */

static DWORD WINAPI sampler(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Each threshold snapshots once, the first time it is crossed.
    int snapStart = 0, snapLow64 = 0, snapLow16 = 0, snapLow4 = 0, warnLow = 0;
    double lastPeriodic = 0, lastAllocReport = 0;
    ULONGLONG peakReported = 0;
    int firstTick = 1;
    for (;;) {
        Sample s;
        take_sample(&s, 1);
        write_sample(&s, "tick");
        double t = now_s();
        note_headroom(&s, t);
        note_gpu_chunks(&s, firstTick);
        firstTick = 0;
        if (!warnLow && s.free_[0] + s.free_[1] < (200ull << 20)) {
            warnLow = 1;
            log_headroom(&s, "under 200 MB free: save and restart the game soon");
        }

        prepare_exception_snapshot();
        topdown_drain();
        watch_tick();
        audioguard_tick();
        hang_tick();
        smallblock_tick();
        mapgrid_tick();
        gpuwatch_tick();
        msgqueue_tick();
        d3dstats_tick();
        texhandles_tick();
        if (!snapStart && t >= 60) { snapStart = 1; log_headroom(&s, "1 min"); snapshot("t60s"); heap_histogram("t60s"); }
        if (!snapLow64 && s.free_[0] < (64ull << 20)) {
            snapLow64 = 1;
            log_event("low 2 GB: free < 64 MB");
            snapshot("low2g_free_lt64mb");
        }
        if (!snapLow16 && s.largestFree[0] < (16ull << 20)) {
            snapLow16 = 1;
            log_event("low 2 GB: largest free block < 16 MB");
            snapshot("low2g_block_lt16mb");
        }
        if (!snapLow4 && s.largestFree[0] < (4ull << 20)) {
            snapLow4 = 1;
            log_event("low 2 GB: largest free block < 4 MB");
            snapshot("low2g_block_lt4mb");
        }
        if (g_periodicSnapshotMin && t - lastPeriodic >= g_periodicSnapshotMin * 60.0) {
            lastPeriodic = t;
            char label[32];
            snprintf(label, sizeof label, "periodic_%04.0fmin", t / 60);
            char when[16];
            snprintf(when, sizeof when, "%.0f min", t / 60);
            log_headroom(&s, when);
            log_gpu_chunks(&s);
            snapshot(label);
            heap_histogram(label);
            topdown_report();
            luagc_report();
            mialloc_report();
            luaheap_report();
            luaguard_report();
            coretick_report();
            uiqueue_report();
            mapgrid_report();
            gpuwatch_report();
            msgqueue_report();
            d3dstats_report();
            texhandles_report();
            watch_report();
            cmdstream_report();
        }
        // Transient spikes (e.g. creating a game) are freed again within a minute, and the
        // rolling "latest" report misses them. Keep one report taken at the highest point.
        if (alloc_enabled() && g_peakStepMb && s.crtHeapAlloc >= peakReported + ((ULONGLONG)g_peakStepMb << 20)) {
            peakReported = s.crtHeapAlloc;
            log_event("CRT heap peak: %.1f MB live, %.1f MB reserved; alloc report \"peak\" updated",
                      MB(s.crtHeapAlloc), MB(s.crtHeapReserve));
            alloc_report("peak");
        }
        if (alloc_enabled() && alloc_report_seconds() && t - lastAllocReport >= alloc_report_seconds()) {
            lastAllocReport = t;
            alloc_report("latest");
        }
        Sleep(g_intervalMs);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static int host_is_civ(const WCHAR *exePath)
{
    const WCHAR *name = wcsrchr(exePath, L'\\');
    name = name ? name + 1 : exePath;
    return _wcsnicmp(name, L"CivilizationV", 13) == 0;
}

static void start_probe(void)
{
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!host_is_civ(exe)) return;

    // Only one probe per process, in case both proxy DLLs are installed.
    WCHAR mutexName[64];
    _snwprintf(mutexName, 64, L"civprobe_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(NULL, FALSE, mutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return;

    lstrcpyW(g_dir, exe);
    WCHAR *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    lstrcatW(g_dir, L"\\civprobe");
    CreateDirectoryW(g_dir, NULL);

    WCHAR ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%s\\civprobe.ini", g_dir);
    ini[MAX_PATH - 1] = 0;
    if (!GetPrivateProfileIntW(L"probe", L"Enabled", 1, ini)) return;
    g_intervalMs = GetPrivateProfileIntW(L"probe", L"IntervalMs", 2000, ini);
    if (g_intervalMs < 250) g_intervalMs = 250;
    if (g_intervalMs > 60000) g_intervalMs = 60000;  // also the freeze watchdog's and the guards' tick
    g_periodicSnapshotMin = GetPrivateProfileIntW(L"probe", L"PeriodicSnapshotMinutes", 10, ini);
    g_peakStepMb = GetPrivateProfileIntW(L"probe", L"PeakReportStepMB", 32, ini);
    g_heapHist = GetPrivateProfileIntW(L"probe", L"HeapHistogram", 0, ini);
    g_heapStats = GetPrivateProfileIntW(L"probe", L"HeapStats", 0, ini);
    g_telemetry = GetPrivateProfileIntW(L"probe", L"Telemetry", 0, ini);
    int keep = GetPrivateProfileIntW(L"probe", L"KeepSessions", 20, ini);
    // This launch is the newest. The previous one stays at least until this launch has checked
    // how it ended (lastrun.c), also with KeepSessions=1.
    if (keep > 0) prune_sessions(keep > 1 ? keep - 1 : 1);

    g_t0 = GetTickCount64();
    // The stamp is local time, one second later if a launch in the same second already took it
    // (its files would be overwritten).
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    WCHAR name[64];
    for (int i = 0; i < 60; i++) {
        FILETIME lt;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &lt);
        FileTimeToSystemTime(&lt, &st);
        _snwprintf(g_session, 32, L"%04d%02d%02d_%02d%02d%02d",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        WCHAR path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%s\events_%s.log", g_dir, g_session);
        path[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
        ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
        u.QuadPart += 10000000;  // 1 s
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
    }

    _snwprintf(name, 64, L"events_%s.log", g_session);
    g_events = open_out(name);
    if (g_telemetry) {
        _snwprintf(name, 64, L"samples_%s.csv", g_session);
        g_samples = open_out(name);
        write_all(g_samples, kSampleHeader, sizeof kSampleHeader - 1);
        prepare_exception_snapshot();  // also for an exception before the sampler's first tick
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    pHeapSummary = (HeapSummary_t)GetProcAddress(k32, "HeapSummary");
    pGetMappedFileNameW = (GetMappedFileNameW_t)GetProcAddress(k32, "K32GetMappedFileNameW");
    g_procHeap = GetProcessHeap();

    char exeUtf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeUtf8, sizeof exeUtf8, NULL, NULL);
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    log_event("Civ V Crash Patch %s (civprobe) started in %s (pid %lu)", CIVFIX_VERSION_STR, exeUtf8, GetCurrentProcessId());
    log_event("exe base=%p timestamp=0x%08lx size=0x%lx LAA=%d", (void *)base,
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0);
    const GameExe *known = game_exe(base);
    if (known) {
        g_gameFault = base + known->gameFault;
        log_event("exe: the final Steam %s build", known->name);
    } else {
        log_event("exe: not a known build of the game; the parts that change exact places stay off");
    }
    log_event("proxying %ls (real copy %s); interval=%lums periodicSnapshot=%lumin HeapSummary=%s",
              PROXY_DLL, g_realDll ? "loaded" : "MISSING", g_intervalMs, g_periodicSnapshotMin,
              pHeapSummary ? "yes" : "no");

    modules_init();
    lastrun_init(g_dir, ini);
    topdown_init(ini);

    LuaLocation loc;
    if (find_lua_counters(base, &loc)) {
        g_luaHeapVar = (HANDLE *)(base + loc.heapRva);
        g_luaCurVar = (volatile DWORD *)(base + loc.curRva);
        g_luaPeakVar = (volatile DWORD *)(base + loc.peakRva);
        log_event("lua_Alloc found at +0x%lx: heap var +0x%lx, bytes var +0x%lx, peak var +0x%lx",
                  loc.siteRva, loc.heapRva, loc.curRva, loc.peakRva);
    } else {
        log_event("lua_Alloc pattern not found (matches=%d); Lua columns will be 0", loc.matches);
    }

    alloc_init(ini);
    mialloc_init(ini);
    luaheap_init(ini);
    luagc_init(ini);
    luaguard_init(ini);
    crtfail_init(ini);
    hang_init(ini);
    // watch before cmdstream: the exe's malloc then reaches cmdstream's wrapper first. Its
    // constructor search covers 48 stack dwords, and watch's wrapper alone takes 0x310 bytes,
    // which hid the stream constructor (no spare or slack for its arrays, session 20261029_003228).
    watch_init(ini);
    cmdstream_init(ini);
    audioguard_init(ini);
    coretick_init(ini);
    smallblock_init(ini);
    uiqueue_init(ini);
    mapgrid_init(ini);
    gpuwatch_init(ini);
    msgqueue_init(ini);
    d3dstats_init(ini);
    texhandles_init(ini);

    g_overflowReq = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_overflowDone = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE ot = g_overflowReq && g_overflowDone ?
        CreateThread(NULL, 64 * 1024, overflow_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (ot) CloseHandle(ot);
    else g_overflowReq = NULL;
    AddVectoredExceptionHandler(1, on_exception);
    HANDLE th = CreateThread(NULL, 64 * 1024, sampler, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (th) CloseHandle(th);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_real_dll();
        start_probe();
    } else if (reason == DLL_PROCESS_DETACH && g_events != INVALID_HANDLE_VALUE) {
        Sample s;
        take_sample(&s, 0);
        write_sample(&s, "exit");
        note_headroom(&s, now_s());
        log_headroom(&s, "exit");
        log_gpu_chunks(&s);
        for (LONG i = 0; i < g_seenCount && i < MAX_SEEN; i++)
            log_event("exception site %p hit %ld times", g_seenAddr[i], g_seenHits[i]);
        topdown_drain();
        topdown_report();
        luagc_report();
        mialloc_report();
        luaheap_report();
        luaguard_report();
        coretick_report();
        uiqueue_report();
        mapgrid_report();
        gpuwatch_report();
        msgqueue_report();
        d3dstats_report();
        texhandles_report();
        watch_report();
        cmdstream_report();
        crtfail_log_last_cxx();
        log_event("process exiting");
        drop_exception_snapshot();
        snapshot("exit");
    }
    return TRUE;
}
