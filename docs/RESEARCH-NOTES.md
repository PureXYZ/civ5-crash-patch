# Civ V memory research notes

The lab notebook behind [Civ V Crash Patch](../README.md), written as the work happened
(2026-10-27 to 2026-10-30), dead ends and wrong guesses included. Later sections often correct
earlier ones, so for the current state of each fix read [How it works](HOW-IT-WORKS.md) first and
come here for the evidence. "The user" is the player whose game was being investigated;
"session" names (`20261028_162412`) are the probe's per-launch log stamps.

Target: `CivilizationV_DX11.exe` (Steam final build, PE timestamp 2014-11-19, MSVC 2008,
LAA set). RVAs below are from image base 0x400000; at runtime add the actual base minus 0x400000.

## Contents by topic

In the order they happened:

1. **Measuring the address space.**
   [Why the game crashes](#why-the-game-crashes) ·
   [First session](#first-civprobe-session-2026-10-27-vp-latest-launch-to-main-menu-5-min) ·
   [Who owns the CRT heap](#who-owns-the-16-gb-crt-heap-civprobe-alloc-tracking-2026-10-27-2215-save-loaded) ·
   [Allocators in the exe](#allocators-in-the-exe) ·
   [Footprint in dumps](#address-space-footprint-seen-in-dumps) ·
   [Tools used](#tools-used)
2. **Not address-space exhaustion after all; the top-down test.**
   [Re-reading crashes.log](#re-reading-crasheslog-2026-10-28-the-crashes-were-not-address-space-exhaustion)
3. **The Huge-map spike and the Lua GC fix.**
   [Lua-held query results](#the-huge-map-spike-is-lua-held-database-query-results-session-20261028_162412) ·
   [Black squares are a separate DX11 issue](#black-squares-a-known-dx11--euivp-issue-not-address-space-2026-10-28)
4. **The slow creep: CRT heap fragmentation and mimalloc.**
   [The plan](#next-project-the-address-space-creep-planned-2026-10-28-the-user-starts-after-a-context-compact) ·
   [Measuring it](#creep-investigation-step-1-2026-10-28-after-the-compact) ·
   [The mimalloc fix](#the-fix-mimalloc-for-the-crt-allocations-2026-10-28-mimalloc1)
5. **The crash: the render command stream.**
   [The audio engine](#the-recurring-crash-is-in-the-audio-engine-session-20261028_222613-mimalloc0-no-heap-walks) ·
   [First root cause](#root-cause-the-render-command-stream-overflows-session-20261028_230012) ·
   [The real overflow, the command array](#the-real-overflow-the-command-array-not-the-records-2026-10-29-after-the-compact) ·
   [Confirmed in play](#confirmed-in-play-session-20261029_004317-corrected-build-the-fix-works)
6. **The quit crash.**
   [First form](#crash-on-quitting-an-existing-shutdown-bug-not-luagc-2026-10-28) ·
   [Collected Lua thread](#quit-crash-third-form-a-collected-lua-thread-session-20261029_190826) ·
   [Under the game's Lua lock](#gameevent-crash-on-a-corrupt-lua-table-guard-v3-under-the-games-lua-lock-session-20261029_192343)
7. **AI turns and panning stutter.**
   [AI turn time](#ai-turn-time-2026-10-2930-alexander-huge-save-turns-367-425-vp--eui) ·
   [Stutter while panning](#stutter-while-panning-during-ai-turns-2026-10-30-same-save-turns-429-450-session-20261030_003648) ·
   [The handlers themselves](#round-2-the-handlers-themselves-2026-10-30-afternoon-same-save-sessions-131238-and-133814)
8. **Where the rest of the 4 GB goes; map grids; the load crash.**
   [Where the 3.9 GB goes](#where-the-39-gb-goes-and-the-map-detail-grids-2026-10-30-session-20261030_003648) ·
   [GPU driver chunks](#the-growth-after-30-minutes-gpu-driver-chunks-the-dips-mimalloc-segment-churn-2026-10-30) ·
   [The event queue overflow](#the-load-crashes-were-the-games-event-queue-overflowing-2026-10-30-0500) ·
   [Lua heap on mimalloc](#lua-heap-on-mimalloc-works-session-20261030_113554-144-min-clean-with-map-grid-v3) ·
   [Conclusion on the GPU regions](#conclusion-on-the-gpu-driver-regions-2026-10-30)
9. **Grey terrain tiles.**
   [Page-in rate and texture pool](#grey-tiles-after-scrolling-far-2026-10-30-terrain-page-in-and-base-texture-pool)
10. **The DirectX 9 and tablet exes.**
   [Porting](#porting-to-the-directx-9-and-tablet-exes-2026-10-30)

## Why the game crashes

From the 7 VP 5.4.6 crash dumps (2026-09):
- **The low 2 GB is always exhausted:** 17–27 MB free, largest free block 0.5–5 MB.
- **The high 2 GB still has 1.0–1.5 GB free,** largest block about 1 GB.
- **The crash sites:**
  - 3× `ntdll!RtlSizeHeap` called from the exe's `lua_Alloc` on a Lua block above 2 GB with an invalid heap header;
  - 2× in `CvGameDatabase` `ResultsCache::Prepare`;
  - 2× on the Miles audio thread, dereferencing 0xFFFFFFFF.
- **This matches VP issue #13270:** the DLL's `operator new` returns NULL on failure, so the
  failure surfaces later as an access violation instead of a clean out-of-memory error.

## First civprobe session (2026-10-27, VP latest, launch to main menu, 5 min)

| t | low 2 GB commit | high commit | CRT heap (alloc) | process heap | Lua | images |
|---|---|---|---|---|---|---|
| 0 s | 169 MB | 0 | 0 | 0 | 0 | 164 MB |
| 10 s | 644 MB | 34 MB | 128 MB | 34 MB | 0 | 358 MB |
| 34 s | 1641 MB | 66 MB | 885 MB | 136 MB | 6 MB | 444 MB |
| 88 s | 1695 MB | 1063 MB | 1600 MB | 184 MB | 81 MB | 458 MB |
| 2–5 min (idle) | ~1665 MB | ~1063 MB | 1602 MB | 197 MB | 79 MB | 458 MB |

- **At the main menu, ~2.73 GB is committed and ~0.46 GB more is reserved.** Only about
  870 MB of address space is left for a game to grow into.
- **The msvcr90 CRT heap is the dominant consumer: 1.6 GB of live allocations.** The exe,
  the VP DLL and CvGameDatabase all share it. Lua is only ~80 MB.
- **The low 2 GB fills by 34 s.** That's just bottom-up allocation, not a symptom.
- **A first-chance AV at `exe+0x523dcb` (write to NULL) is raised and handled during normal
  startup.** Ignore it.
- **Next step:** attribute the CRT heap by caller to see which of VP DLL / database DLL / exe
  owns the 1.6 GB. Built 2026-10-27 as `probe/alloctrack.c` (IAT hooks on msvcr90, live
  bytes per stack). Results: see the next section.

## Who owns the 1.6 GB CRT heap (civprobe alloc tracking, 2026-10-27 22:15, save loaded)

The tracked live total (1608 MB) matches the CRT heap's HeapSummary. By owner:

| Owner | Live MB | Allocated by | Identified from |
|---|---|---|---|
| Granny 3D (statically linked) | ~427 | grows 114 → 427 MB when the save loads | `granny_aggr_alloc.cpp`, `granny_file.cpp`, `granny_dag_pose_cache.cpp` asserts near exe+0x598770 (Granny's allocator callback at [exe+0x8b3824] → exe+0x4e2ce0, tag 0xB) |
| Fluid fog of war | ~240 | startup (by 34 s) | `FluidParticles.cpp` l.623: 24 MB **per job thread** (`MaxSimultaneousThreads`, 8 here = 192 MB, from exe+0x28f780; count from exe+0x41d870 → global +0x5d96d54). l.718: 32 + 16 MB (exe+0x329dd4). Caller exe+0x4b1fb loads `FX_Fog_dif.dds`. |
| Map-sized grids | ~178 | startup | exe+0x75f41: X = Defines `MAX_SUPPORTED_MAP_X`, default **152**; Y = `MAX_SUPPORTED_MAP_Y`, default **96**. Read at engine startup from the base DB, before mods load. FFastVector (`FFastVector.h`) of 512-B (119 MB) and 256-B (59 MB) cells, about 16.7 cells per plot. |
| VP game core DLL | ~111 | grows when the save loads | 1067 distinct stacks, none over 15 MB |
| Temp heap | 68 | startup | `GetTempHeap` + exe+0x22822; size = config.ini `Maximum Temp Heap Size` (68) |
| Render command streams | ~59 | startup | `CommandStream.cpp` l.0x25C, per-thread contexts at exe+0x4c6b00 |
| Localization DLL | ~22 | | |
| Lua via CRT / database DLL | ~3 | | Lua's own heap is separate (~80 MB) |
| Remaining exe sites | ~500 | | mostly < 30 MB each |

Allocator wrappers seen at frame0: exe+0x413a80/0x413b02 (aligned new: FSBA or `_aligned_malloc`),
exe+0x42c090/0x42c0d5 (new with file/line: FSBA or `malloc`), exe+0x4e2ce0 (Granny callback).
Churn: 43.8 GB cumulative through exe+0x413b02 in about 3 minutes.

### Config levers this points at

**Tried 2026-10-27:**
- **`FluidFOW = 1` breaks rendering.** The visible map turned into flat territory colours with
  no terrain. It is a debug "disable subsystem" switch, not a quality option. **Reverted to 0.**
  - It did measure the system's cost: at the same point after loading the same save, the CRT
    heap was 1276 MB instead of 1608 MB (**−332 MB**), and the high 2 GB had 1145 MB free
    instead of ~870 MB.
- **`MaxResidentScenes = 2` (GraphicsSettingsDX11.ini) is kept.** Its effect only shows after
  meeting more than 2 leaders, and isn't measured yet.
- Backups of both files sit next to them as `*.civprobe-backup-20261027`.
- **`MaxSimultaneousThreads` 8 → 4 applied 2026-10-27** (config.ini). Map renders normally (user
  confirmed). Measured on the same save (session 20261027_224135 vs 221500):
  - fluid-fog buffers: 4 × 24 MB = 96 MB instead of 8 × 24 = 192 MB; total fluid 144 MB, was 240;
  - CommandStream buffers: 31 MB instead of 59 (they're per-thread too);
  - CRT heap live: ~1395 MB instead of ~1605 MB (**−~210 MB**);
  - address space at t≈170 s: committed 2535 MB instead of 2818 MB; free 1079 MB instead of
    849 MB (**+230 MB headroom**). Granny (~411 MB), the grids (178 MB) and the temp heap
    (68 MB) are unchanged, as expected.
  - Turn-time and frame-rate impact not measured.
- **`Maximum Temp Heap Size` 68 → 32 applied 2026-10-27** (config.ini; `Minimum Temp Heap Size`
  is already 32). Expected −36 MB (the single temp-heap block, exe+0x22822). When the temp heap
  is full the exe falls back to FSBA/`_aligned_malloc` (exe+0x429b23). Measured (session
  20261027_224816, same save): the temp heap block is 32 MB instead of 68, and the CRT heap is
  1356 MB instead of 1395. Free address space at the end of a ~4 min session: 1144 MB instead
  of 1108. No new exceptions.

**Combined effect so far** (8 thr / 68 MB / unlimited scenes → 4 thr / 32 MB / 2 scenes, same
save): CRT heap ~1605 → ~1356 MB (**−~250 MB**). Steady-state free address space after
loading ~850–930 MB → ~1144 MB (**+~210–290 MB**).

- `config.ini` `FluidFOW = 1`: −332 MB measured, but **breaks terrain rendering**. Do not use.
- `config.ini` `MaxSimultaneousThreads` 8 → 4: −96 MB of fluid buffers (if fluid fog stays on),
  plus probably part of the command streams.
- `GraphicsSettingsDX11.ini` `MaxResidentScenes = -1` (unlimited leader scenes stay resident)
  → 2 or 3: caps Granny/leader growth over a game. `UseScreenShots = 1` removes 3D leaders entirely.
- `config.ini` `Maximum Temp Heap Size` 68 → 32: −36 MB. The temp heap falls back to
  FSBA/`_aligned_malloc` when full (exe+0x429b23).
- `MAX_SUPPORTED_MAP_X/Y` (152×96 default) can only be set in the base game's defines (read
  before mods load). 128×80 (Huge) would save ~53 MB; risky if a map script exceeds it.

## Allocators in the exe

| What | Where | Details |
|---|---|---|
| Lua heap | `HeapCreate(0, 0x1400000, 0)` at RVA 0x4b903 | Growable private heap (20 MB initial commit) with the Low Fragmentation Heap enabled (`HeapSetInformation(...,0,&2,4)`). Handle at global RVA 0x894b1c. Created in the script-system constructor (RVA 0x4b8b4), which also calls `lua_newstate(lua_Alloc, ...)` at RVA 0x4b993. There is **one** Lua state for the whole game, so all UI and game Lua lives in this one heap. |
| `lua_Alloc` | RVA 0x2ce440 | `HeapAlloc`/`HeapReAlloc`/`HeapSize`/`HeapFree` on the Lua heap. Tracks current bytes (global RVA 0x29d2084) and peak (RVA 0x29d2080). DX9 exe: bytes +0x29c1e84 / peak +0x29c1e80. Tablet exe: +0x29d406c / +0x29d4068. |
| Lua heap stats | RVA 0x23df20 | `HeapLock` + `HeapWalk` over the Lua heap, summing busy bytes, region commit/uncommit and overhead into globals at RVA 0x29d2088..98. Probably the FireTuner "Lua Mem Tracking" panel. |
| Small-block allocator (FSBA) | exports `FSBAAllocate` etc. (RVA 0x413870); setup RVA 0x41ba00; params from `GetSmallObjectAllocatorInitParams` (RVA 0x2f8d70) | Params: 32 MB, 32 MB, 64 KB, 4 KB page, max block 512 B, 32 B granularity. **Reserves with `MEM_RESERVE \| MEM_TOP_DOWN`** (RVA 0x41bb2e–0x41bb4e), commits pages on demand (RVA 0x41bbe8). It's the only direct `VirtualAlloc` user in the exe. Shared with DLLs through `GetSmallObjectAllocator` (GetModuleHandle/GetProcAddress lookup). |
| Temp heap | export `GetTempHeap` (RVA 0x429aa0) | Returns a static object at RVA 0x5d96d70. Not mapped yet. |
| Process heap | WinMain, about RVA 0x2023ef | `GetProcessHeap` with LFH enabled (after the single-instance mutex check). |
| CRT heap | msvcr90 `malloc`/`free` | 227 `malloc` and 571 `free` call sites in the exe. The VP DLL and the database DLL use the same msvcr90 heap. |
| Other | `_aligned_malloc` (46 sites), `HeapAlloc` (31 sites, mostly process heap) | |

Only one `HeapCreate` exists in the exe, so every other private heap in a dump belongs to a
DLL: the driver, d3d, Miles, Steam, and so on.

## Address-space footprint seen in dumps

- exe image: 101 MB (88 MB of it `.data`)
- NVIDIA D3D11 user-mode drivers: ~200 MB (nvgpucomp32 75, nvd3dum 65, nvwgf2um 58)
- steamclient (overlay): 21 MB
- VP DLL: 20 MB
- AMD iGPU driver DLLs (~50 MB each) are loaded and unloaded repeatedly at startup (adapter enumeration)

## Ideas for interventions (after measuring with civprobe)

1. **Move the Lua heap high:** replace `lua_Alloc`'s heap with one whose segments are
   reserved top-down (or a custom allocator) so Lua stops competing for the low 2 GB.
2. **Reserve-then-release trick:** at startup, reserve the low holes that only drivers would
   fill, so that drivers and DLLs that allocate bottom-up land high.
3. **Handle allocation failure deliberately:** log where it happened and flush a save, instead
   of crashing later with an access violation.
4. **Investigate the Lua heap corruption above 2 GB:** watch for pointer-sign bugs with
   addresses ≥ 0x80000000. A test is forcing top-down allocation for just this process (by
   injection, not the system-wide `AllocationPreference` registry key).

## Tools used

- WinDbg `cdbX86.exe` (Microsoft Store WinDbg): dump analysis, and `-z <exe>` for static disassembly.
- Python PE/IAT/xref scripts: kept in the session scratchpad and easy to recreate.

## Re-reading crashes.log (2026-10-28): the crashes were not address-space exhaustion

VP's crash handler writes live memory markers into `crashlogs\crashes.log` at the moment of each crash.
All 8 entries (2026-09-12..18, VP 5.4.6) show:
- **free address space 1.06–1.56 GB, with the largest free block 0.99–1.48 GB** (always above 2 GB);
- total commit of 2.05–2.60 GB. That's *less* than a freshly loaded save measured with civprobe on the old settings (~2.8 GB).

So no allocation of a normal size could have failed. The full low 2 GB is normal from ~34 s after launch,
so it isn't a sign of trouble either. The crash classes look like **heap corruption, a race, or a
pointer bug**, not out-of-memory:
- ntdll+0x61f39 ×3 (RtlSizeHeap from `lua_Alloc`, invalid header);
- CvGameDatabase+0x496ea ×2 (`ResultsCache::Prepare`);
- exe +0x517414 / +0x51f60c / +0x50e2a6, all within one second of the same session.

More memory cuts add headroom, but they won't fix these crashes. Next: (1) a per-process top-down
allocation test (hook NtAllocateVirtualMemory/NtMapViewOfSection to add MEM_TOP_DOWN) to check
whether high pointers trigger the bug; (2) a guarded Lua allocator (canaries, quarantine,
allocation stacks) behind `lua_Alloc` (RVA 0x2ce440) to catch the heap corruption at its source.
No crash has been logged since 2026-09-18. VP was updated and the configs were changed on 2026-10-27; neither has been tested in a long session.

### Top-down test built (2026-10-28)

`probe/topdown.c` adds `TopDown=1` to civprobe.ini (see probe/README.md). In the harness,
VirtualAlloc/HeapCreate results moved from 0x03xxxxxx–0x17xxxxxx to 0xEBxxxxxx–0xEFxxxxxx, all
3 hooks installed, and nothing fell back. Installed in the game folder with **TopDown=1 enabled**;
the previous DLL is kept as `probe/build/msimg32.prev-20261027.dll`. Not yet run in the game.

### First top-down run: new Huge map, many AIs (session 20261028_161055)

- **The game exited 390 s in, with no access violation** (`process exiting` through ExitProcess, no VP crash
  log, no Windows error event). The user saw graphics glitches first. That fits a failed allocation the game
  handled by quitting. It is **not** the September crash type.
- **This one was real address-space exhaustion:** from 250 s on, only ~118 MB stayed free, and the largest
  free block was 13–17 MB.
- **Cause: a spike at game creation that the CRT heap never gave back.** Its live bytes went
  1376 → **1907 MB** (176–227 s), then fell back to ~1420 MB. But the CRT heap's *reserve* stayed at
  **2175 MB**. Freed segments get decommitted, but the NT heap never releases a growth segment, so
  ~750 MB of address space was held for nothing.
- **Top-down mode is probably not the cause.** At the setup screen (150 s) the free space was 1131 MB,
  the same as earlier normal-mode sessions (~1144 MB).
- **High pointers:** startup, mod loading, map generation and AI setup all ran with almost everything
  above 2 GB, with no access violation. That's evidence against (not proof) the high-pointer theory.
- **Workaround to test:** start the game, save on turn 1, quit to desktop, relaunch, and load. Loading in
  a fresh process should skip the spike and give back ~600–750 MB.
- **Next RE target:** attribute the spike. The `latest` alloc report is overwritten every 30 s and missed
  it, so add a report taken at each new CRT-live peak. Then make that memory release its address space
  (redirect the responsible allocation sizes or sites to an allocator that frees whole regions).
  A system-level alternative is the Segment Heap (IFEO `FrontEndHeapDebugOptions`), which releases
  empty segments; it would be the user's registry change and is untested.

### Peak report added (2026-10-28)

civprobe now rewrites an alloc report labelled `peak` at each new CRT-live high (step 32 MB), and
`probe/peakdiff.py` diffs it against the later `latest` report to isolate the spike. Installed; the
game-folder civprobe.ini now has **TopDown=0** and PeakReportStepMB=32. Next: create a Huge / many-AI
game again, then run peakdiff.py on that session.

## The Huge-map spike is Lua-held database query results (session 20261028_162412)

- **The peak** (303 s): CRT heap 1805 MB live; after it, ~1422 MB live but **2033 MB still reserved**.
  ~309 MB was left free and the user saw glitched graphics.
- **peakdiff: 420 MB was live at the peak but freed afterwards, almost all of it in the game database
  DLL, reached from Lua:**
  - 225.6 MB `new` at DB+0x493c ← DB+0x78b5 ← **`Database::Results::HashColumnPositions`** (DB+0x7df2) ←
    **`Results::ColumnPosition`** (DB+0x7e81) ← lua51+0x1b9f;
  - 150.8 MB `new` from MSVCP90 (+0x51dc ← +0x5938 ← +0x6066), probably the std::string keys of the same hash;
  - 38 MB DB+0x1e0a ← +0x51e9 ← +0x7964 ← HashColumnPositions.
  - (These names are the nearest *exports* in cdb; the small exported functions HashColumnPositions and
    ColumnPosition contain the return addresses.)
- **Mechanism:** each Lua DB query (GameInfo / DB.Query) is a `Results` userdata (the DB DLL imports
  `lua_newuserdata`). The first column lookup builds a per-object hash table of column names in C++.
  Lua's GC sees only the small userdata, not the KBs of C++ memory behind it, so tens of thousands of
  dead Results objects pile up. Evidence: the 420 MB fell at 277–311 s, at the same moment Lua bytes fell
  103.6 → 92.3 MB (a GC cycle). The NT heap keeps the address space it grew into.
- **Same component as the September crash sites** (`ResultsCache::Prepare` ×2 in this DLL).
- **Fix plan (probe): GC driven by C++-side pressure.** The DB DLL registers its Lua C functions through
  its imported `lua_pushcclosure`. Patch that import so each function is wrapped in a thunk. On return
  (a clean API boundary, like `collectgarbage()`), when CRT live has grown more than N MB since the last
  collection, call `lua_gc(L, LUA_GCCOLLECT, 0)`. GC parameter tuning (pause/stepmul) alone can't keep
  up: ~38 bytes of C++ per byte of Lua.
- **Workaround now:** save, quit to desktop, relaunch and load. That drops the ~610 MB of heap
  reserve slack.

### Lua GC fix built (2026-10-28)

`probe/luagc.c` (`LuaGcStepMB=64`) is installed and enabled in the game folder. In the harness, with the
game's own lua51_Win32.dll, the layout check passed, and a 625 MB pile-up of dead userdata was held to
64 MB. Not yet run in the game. Check the `luagc:` lines in the next Huge-map session: the number of
wrapped functions (expect dozens), the collection times, and whether the CRT peak and reserve stay down.

### First in-game run of the Lua GC fix: Huge map, many AIs (session 20261028_164019)

- **Layout check passed in the game.** The DB DLL's import was patched at startup, and collections ran
  without errors.
- **Memory stayed much lower:** CRT heap peak ~1497 MB (before: 1805 MB), CRT reserve ~1598 MB
  (before: 2033 MB), ~840–870 MB free (before: ~300 MB). No graphics glitches reported.
- **A long turn the user took for a freeze**, from ~566 to ~691 s. The main thread was in a Lua
  script making DB queries (stack: lua_pcall → Lua → luagc thunk → DB/SQLite). It created dead
  Results at ~19 MB/s. luagc collected 34 times, every ~3.4 s, each 1497 → 1433 MB in 110–200 ms
  (one took 563 ms): ~4–5% of the turn. The window answered again once the turn ended.
- **Without the fix**, that turn alone would have piled up ~2 GB of dead Results unless Lua's own GC
  happened to run. That's well past the free address space.
- **Open:** how much the turn length is VP's AI versus probe overhead (alloc tracking hooks every CRT
  call, and luagc needs it for its trigger). A HeapSummary-based trigger would let alloc tracking be
  turned off.

### Black squares with plenty of address space (same session, turn-402 save)

- **What the user saw:** black rectangles on city banners (above the strength number), on the right
  edge of the score list, and next to the minimap. Turns are slow and the game lags.
- **Memory was fine the whole time:** ~825 MB free above 2 GB, largest block 808 MB, no exceptions
  besides the known one. So **these glitches are not address-space exhaustion.**
- **Suspects, newest first:**
  1. **luagc** (frequent full collections). Possible mechanism: DB `Results` objects may share a
     cached prepared statement, so a collected Results' finalizer could reset a statement a live
     query is still using. Icon-atlas lookups (IconHookup → GameInfo.IconTextureAtlases) would
     then come back empty. This is unproven; see the ResultsCache crash sites.
  2. **`Maximum Temp Heap Size` 68 → 32** (2026-10-27): texture loading may use the temp heap,
     and not every path may fall back to other memory when it's full.
  3. Unrelated to these changes (a driver or Vox Populi rendering issue).
- **Turn lag:** the probe's alloc tracking adds a cost to every CRT allocation; that hasn't been measured.
- **Plan:** test one change per restart on the same save. First run with `AllocTracking=0` (which also
  disables luagc and the peak report) as a baseline for both the glitches and the turn time. If the
  glitches remain, put the temp heap back to 68.

### Baseline run (AllocTracking=0, so no luagc), session 20261028_170335, turn-402 save

- **Black squares still there, but fewer** (e.g. on the Babylon banner and the right edge of the
  score list), with ~945 MB free at 5 min. **luagc and address space are both ruled out.**
- **The turn felt smoother.** Alloc tracking's per-allocation cost is a real share of the lag. luagc
  should get a trigger that doesn't depend on it (HeapSummary on the CRT heap, rate-limited).
- **Next:** revert all three config.ini / GraphicsSettingsDX11.ini changes from 2026-10-27 at once
  (threads 4→8, temp heap 32→68, MaxResidentScenes 2→-1), with the game closed (it rewrites
  config.ini). If the squares go away, bisect. If not, they predate this work.
- **2026-10-28: all three config changes reverted** (threads 8, temp heap 68, MaxResidentScenes -1).
  GraphicsSettingsDX11.ini now matches its backup; config.ini differs only in the game's own
  LastDifficulty. The probe still has AllocTracking=0.

### Config revert: city-banner squares gone (session after 20261028_170335)

- **With threads 8, temp heap 68 and MaxResidentScenes -1 (and no luagc), the city-banner squares
  were gone.** Two remain: a black panel at the right edge by the minimap, in the same place in all
  three screenshots (probably part of the UI, not a texture failure), and a small dark-red square at
  the top left (maybe a unit flag).
- **Prime suspect: `Maximum Temp Heap Size = 32`.** Not bisected; it's left at 68.
- **HeapSummary is too slow for a trigger.** Measured: 68 ms per call on a 572 MB / 3M-block LFH heap,
  because it walks the heap. It does **not** block other threads' allocations (worst case 0.9 ms
  while it ran), so the probe's 2-second sampler isn't what makes turns lag.
- **luagc now has a light trigger** for AllocTracking=0 (see probe/README.md). Same test result as the
  full one. Installed by renaming the loaded DLL (the renamed old copy has since been
  deleted). civprobe.ini now has AllocTracking=0 and LuaGcStepMB=64.
- **Next run** (original configs plus light luagc): black squares back means luagc is involved.
  Otherwise it's the intended setup for normal play.

### Light luagc in the game (session 20261028_172143, turn-402 save, original configs)

- **The light trigger works:** 11 functions wrapped; 5 collections at 112–164 s (load), each after
  64 MB allocated in DB calls, **16–62 ms each** (110–200 ms with full alloc tracking).
- **The Babylon banner square is back** (probably its production icon).
- **Square count across runs vs. GC frequency:**

  | Run | Collections | Banner squares |
  |---|---|---|
  | 1: luagc + full tracking, Oct 27 configs | 41 | many |
  | 2: no luagc, Oct 27 configs | Lua's own only | a few |
  | 3: no luagc, original configs | Lua's own only | none |
  | 4: light luagc, original configs | 5 | one |

- **Hypothesis: any Lua GC can break an icon lookup**, and luagc makes it more frequent. A dead
  Results' `__gc` would reset or release a cached prepared statement that a live query (e.g. the
  IconHookup → IconTextureAtlases lookup) still uses. That would also fit the
  `ResultsCache::Prepare` crashes.
- **Discriminating test:** `LuaGcStepMB=8`. Many squares means GC-related: dig into the
  Results `__gc` and ResultsCache in the DB DLL. Otherwise it's intermittent and unrelated.

### LuaGcStepMB=8 test (session 20261028_174138): GC is not the cause of the black squares

- **47 collections, ~16 ms each, and no extra squares.** The banners were clean; only the right-edge
  panel remained. **GC frequency is ruled out**, and LuaGcStepMB is back at 64.
- **User observation:** no squares right after loading; **they appear while a turn processes.**
- **Cost of reverting the Oct 27 configs on this save:** CRT heap at 200 s was ~1426 MB with them and
  ~1737 MB without; free address space ~970 → ~650–710 MB. The squares showed up with both settings,
  so the configs are probably not the cause either (run 3's clean screenshot was likely taken before
  a turn).
- **Next:**
  1. Turn on `LoggingEnabled = 1` in config.ini (with the game closed) and read Lua.log after a turn.
     A UI script error (CityBannerManager / IconHookup) aborting an icon update would explain
     squares that appear during turns.
  2. If that's clean, test a **DB thread race**: the game core (VP DLL, possibly on its own thread)
     and UI Lua both query the DB DLL, whose ResultsCache may not be thread-safe. That would also
     explain the September `ResultsCache::Prepare` crashes. Instrument DB exports via the importers'
     IATs and log overlapping calls from different threads.
  3. Restore threads 4 / MaxResidentScenes 2 (~250 MB), keeping temp heap at 68.
- **2026-10-28, applied:** config.ini `LoggingEnabled = 1` and `MaxSimultaneousThreads = 4`; temp heap
  stays at 68; GraphicsSettingsDX11.ini `MaxResidentScenes = 2`. Next: read `Logs\Lua.log` after a
  turn in which squares appear. Turn logging back off afterwards (it writes a lot).

### Black squares: a known DX11 + EUI/VP issue, not address space (2026-10-28)

- **Lua.log during a square-producing turn:** no banner errors. There are two benign notes:
  `VPUI_loader.lua` file error in the ToolTips context, and repeated
  `TechPanel: IconHookup error … CORPORATION_ATLAS_VP size 214`, which the script handles by hiding the icon.
- **What the squares are** (found by scanning the screenshots): solid-black UI image quads. A
  56×120 px block at screen (1944,768), right edge, is in all 6 screenshots, and 24×24 squares sit on
  city banners. These are UI textures drawn black.
- **Known issue:** CivFanatics thread 644664, "Graphics glitches keep popping up with EUI/VP". Icons
  turn into black boxes after N turns and come back about 10 turns after a reload. Vox Populi's lead
  developer, quoted there, calls it a GPU/driver VRAM issue on the DX11 path. Users fixed it by
  turning off **GPU Texture Decode** and **High Detail Strategic View**, dropping memory-heavy mods
  (**R.E.D. Modpack**, Ethnic Units), or playing in DX9.
- **This setup:** R.E.D. Modpack is active, and GraphicsSettingsDX11.ini has `UseGPUDecompress = 1`,
  `HDStrategicView = 1` and `MSAASamples = 8`.
- **To try:** `UseGPUDecompress = 0` and `HDStrategicView = 0`, with lower MSAA. R.E.D. can't be dropped
  from the current save; skip it in new games, which also cuts Granny/unit-art memory.
- **2026-10-28, applied:** `UseGPUDecompress = 0` and `HDStrategicView = 0` (MSAA left at 8). Backup:
  `GraphicsSettingsDX11.ini.before-texdecode-20261028`. Edited while the game was running; the game
  hasn't rewritten that file in any session today, so the change takes effect at the next launch.

### Result: no black squares with GPU decode and HD strategic view off (session 20261028_180229)

- **No glitches after several turns** (user report). Current setup:
  - threads 4, temp heap 68, MaxResidentScenes 2;
  - UseGPUDecompress 0, HDStrategicView 0, MSAA 8;
  - probe: AllocTracking 0, luagc light at 64.
- **Memory on the turn-402 save:**
  - at 200 s: CRT 1473 MB, reserve 1558 MB, **1107 MB free** (650–710 MB with the original configs);
  - at 590 s, after a few turns: CRT 1678 MB, reserve 1807 MB, 733 MB free (largest block 586 MB).
  - luagc ran 11 collections. No exceptions besides the known benign one.
- **Still to do:** set `LoggingEnabled = 0` once the game is closed. Watch how memory grows over long
  sessions (free space went 1107 → 733 MB in ~7 min of turns).
- **30-minute trend (same session):** CRT live stays flat (~1678 → 1691 MB), but the **CRT heap reserve
  creeps up** 1807 → 1910 MB. Free space fell 733 → 548 MB (largest block 586 → 491 MB): ~6 MB/min,
  from fragmentation, not live growth. luagc ran 32 collections; no new exceptions. At this rate a
  session has roughly an hour of headroom before free space gets tight (< ~250 MB). Save and restart
  about every hour, or add a probe warning when free space drops low.

## Where things stand (2026-10-28, end of session)

- **Setup:** see the "Result: no black squares" section above.
- **Still open:**
  - (done) `LoggingEnabled = 0`;
  - an optional low-free-space warning in the probe;
  - leave R.E.D. out of new games;
  - the September ResultsCache / Lua-heap crash type hasn't come back and is still unexplained.

### Crash on quitting: an existing shutdown bug, not luagc (2026-10-28)

- **Where:** CvMiniDump_20261028_183346 (VP 5.4.8). AV reading 0x8429C468 at lua51+0x105b,
  `mov eax,[ecx+10h]`: Lua 5.1 `index2adr(L, LUA_REGISTRYINDEX)` reading `L->l_G`, called from
  **`luaL_unref`** (lua51+0x3af7). The caller is CivilizationV_DX11.exe+0x50523d, during exit.
  `L = 0x8429C458` is no longer readable.
- **What it means:** the exe releases a registry ref on a Lua state after shutdown has already freed it,
  a teardown-order bug. The luagc wrapper is not on the stack.
- **The same crash happened without luagc:** session 20261028_162412 (16:35, luagc not built yet) hit
  the identical site lua51+0x105b, and VP's crashes.log records both. Saves made before quitting are
  unaffected.
- **Optional fix:** patch the exe's IAT entry for `luaL_unref` (and maybe other teardown-time Lua calls)
  to return early when `L` isn't readable (VirtualQuery). Low risk, because it only matters once the
  state is gone.

### What eats free space over a long session (session 20261028_180229, vamap 10 → 30 min)

- **Free space fell 732 → 548 MB (−184 MB in 20 min, ~9 MB/min).** Breakdown:
  - CRT heap regions: **+95 MB** (fragmentation; live bytes stayed flat);
  - unattributed private reserve: **+56 MB** (heap big-block VirtualAllocs, FSBA chunks or the driver; not
    yet identified);
  - process heap: +16 MB; other private commit: +16 MB.
- **Projection:** if the rate holds, ~1.1 GB free after load reaches ~250 MB after about 1.5 hours of
  continuous play on this save. It may plateau instead; that's untested.
- **Levers:**
  - restart about every 1–1.5 hours;
  - a low-free warning;
  - find what the +56 MB private reserve is (with alloc tracking for one session, or an owner lookup
    on the new regions);
  - move medium-size CRT allocations to an allocator that releases address space, or try the Segment
    Heap (IFEO, the user's own registry change; untested).

## Next project: the address-space creep (planned 2026-10-28; the user starts after a context compact)

**Goal:** stop free address space shrinking ~9 MB/min during long sessions (CRT heap +95 MB, unattributed
private reserve +56 MB, process heap +16 MB, other commit +16 MB per 20 min on the turn-402 Huge save;
CRT live stays flat).

**Plan, in order:**
1. **Attribute the growth.** Run one measurement session (≥ 45 min of turns) with `AllocTracking=1`
   (slow but exact), keeping luagc on. Then:
   - diff `allocsites_*_periodic*` / `latest` against the early report, by stack: which sites' *live*
     bytes grow, and which churn sizes cause it;
   - identify the new private-reserve regions: vamap diff by region, owner lookup by allocation base;
     candidates are heap big blocks (>508 KB VirtualAlloc'd by the NT heap), FSBA 32 MB chunks
     (`MEM_RESERVE|MEM_TOP_DOWN`, exe RVA 0x41bb2e), and the D3D driver;
   - check whether the CRT heap's LFH is active (HeapQueryInformation) and which block-size ranges
     the new segments serve.
   - Probe additions that may help: the periodic alloc report already exists; add a size histogram of
     live blocks, and log heap segment creation (NtAllocateVirtualMemory hook from topdown.c,
     counting-only mode, with the caller).
2. **Cheap experiment:** Segment Heap for CivilizationV_DX11.exe only (IFEO
   `FrontEndHeapDebugOptions = 0x08`). This is a system registry change the **user** makes; give
   them the add and undo commands. Measure free space at 10/30/60 min on the same save. Watch the
   probe's heap columns (they rely on NT heap segment headers).
3. **Real fix candidates,** depending on step 1:
   - a specific cache or leak: fix it at the site (e.g. clear or limit it via a hook);
   - backend fragmentation by medium blocks: route those sizes (or the identified sites) through a
     separate allocator that releases empty regions. mimalloc is the main candidate. Ownership must be
     checked on free/realloc/_msize/_expand, and blocks from the old CRT heap passed back to msvcr90;
   - FSBA or driver growth: different levers; investigate.

**Baseline to compare against:** session 20261028_180229 (free 1107 MB at 200 s; 732 / 609 / 548 MB
at 10 / 20 / 30 min).

## Creep investigation, step 1 (2026-10-28, after the compact)

### What the existing data already shows (session 20261028_180229, vamap 10 → 30 min, per allocation)

- **CRT heap: six new 16 MB segments (+95 MB).** HeapSummary shows *commit* growing with reserve
  (1794 → 1891 MB committed while live went 1678 → 1691 MB). Committed-but-free space inside the heap
  grew ~116 → ~200 MB: the heap keeps adding segments instead of reusing free blocks it already has.
  That points at backend fragmentation (free holes that don't fit the requests), not a leak.
- **Process heap:** one new 16 MB segment.
- **Unattributed private reserve (+72 MB net):** two new **32 MB regions** (0xd2830000, 0xd9000000)
  plus ~12 MB of 1–4 MB regions. All the 32 MB regions follow the same pattern: reserved whole,
  committed page by page, partly decommitted later (e.g. 0xadc60000: 29.7 → 24.2 → 21.4 MB committed),
  never released. Their total commit stayed flat (167 → 174 MB) while two new ones were reserved.
- **They are not the exe's FSBA.** The exe's small-block allocator is a singleton
  (`GetSmallObjectAllocator`, global at exe+0x5d96fa0; constructor exe+0x41c120 → region ctor
  exe+0x41bab0) with a single 32 MB reservation. Its page code commits on first use (exe+0x41bbe8) and
  decommits empty pages (MEM_DECOMMIT at exe+0x41bc6d). The other 32 MB regions come from another
  module (VP DLL, driver, Granny, ...), which the reservation log will name.

### New probe instrumentation

- `ReserveLog=1`: stacks for every new region of 1 MB or more, and releases (`reserves_*.csv`);
  summary with `probe/resdiff.py`.
- `HeapHistogram=1`: CRT heap busy/free bytes by block size at 60 s and every 10 min.
- Tested with `CivilizationV_restest.exe`: heap growth (1/2/4/8/16 MB segments), big blocks and
  VirtualAlloc reservations are logged with the right callers, and the histogram works.
- **Installed 2026-10-28** in the game with `ReserveLog=1` and `HeapHistogram=1`
  (AllocTracking stays 0, luagc 64). Previous DLL: `probe/build/msimg32.prev-20261028-luagc.dll`.
- **Next:** one ≥45-minute session on the turn-402 Huge save, then:
  - `resdiff.py --from 600` for what grows after the load;
  - compare the heaphist free columns between the 10/20/30/40-minute snapshots.

### Startup crash in the first ReserveLog build (fixed 2026-10-28)

- Both launches (sessions 190027 and 190108) died at 3 s with an AV at MSIMG32.dll+0x71aa reading
  0x646E6157. The stack scan cached executable ranges, and a DLL around 0x646E0000 (GPU driver probing
  loads and unloads DLLs at startup) was unloaded. A stale stack value then matched the cache and the
  call-instruction check read unmapped memory.
- Fix: no cache; every candidate gets VirtualQuery, and the bytes are read with ReadProcessMemory (it
  fails instead of faulting). `CivilizationV_restest.exe` now covers it (load dbghelp, leave its code
  address on the stack, unload, reserve): the old build exits with 0xC0000005, the fixed one passes.
  The crashing build is kept as `probe/build/msimg32.reslog-crash-20261028.dll`.

### Measurement session 20261028_190333 (new Huge game, ~52 min): the creep is CRT heap fragmentation

- **Trend:** free address space 811 MB (3 min) → 522 (6 min) → 432 (18 min) → 246 MB (51 min). CRT heap
  live 1752 → 1799 MB (+47 MB, real game growth) but committed 1849 → 2040 MB and reserved
  1877 → 2061 MB (+184 MB). The process heap went 214 → 240 MB.
- **Reservation log (regions reserved after 400 s and still held, 211 MB):** 11 new 16 MB CRT heap
  segments (~174 MB), reached through msvcr90 `malloc` from many callers: the VP DLL (several
  CvGameCore_Expansion2 sites), the exe's aligned-new wrapper exe+0x413b02, the DB DLL, d3d11. **No
  single site leaks; whichever allocation finds no fitting hole grows the heap.** Smaller items: the VP
  DLL reserved 4 + 8 + 2 MB directly (CvGameCore+0x4be790), and the exe made 6 × ~1.2 MB big blocks
  (exe+0x4c4a9d, render path). No new 32 MB regions this session. Pure churn: the NVIDIA driver
  (79 MB) and zlib via the VP DLL (79 MB) reserve and free again, so they're harmless.
- **Heap histogram (CRT heap, front end = LFH):** free space found by HeapWalk grew 27 → 106 MB from
  10 to 50 min while busy bytes rose only 37 MB. The growth is in tiny and small holes:
  - 32–63 B: 91 k → 744 k free blocks (3 → 27 MB), with busy bytes in that size flat at ~27 MB;
  - 8–31 B: ~6 k → ~178 k free blocks;
  - 1–8 KB: 11 → 45 MB free;
  - largest free block ≤ 508 KB throughout. The walk takes 30–94 ms, so the hitch is negligible.
  - HeapSummary commit minus busy is ~240 MB, more than the walk's free total; the rest is probably
    LFH/segment overhead that HeapWalk doesn't report as free.
- **Conclusion:** classic NT-heap fragmentation under a long-running mixed workload. Free memory piles
  up in small holes (backend split remainders and partly empty LFH subsegments), and new requests get
  new segments. A site fix can't solve it; it needs a different allocator.
- **Next:** the Segment Heap experiment (IFEO `FrontEndHeapDebugOptions = 8` for
  CivilizationV_DX11.exe, set by the user), on a similar session. If that isn't enough, route the CRT
  allocations through mimalloc.

## The fix: mimalloc for the CRT allocations (2026-10-28, `MiMalloc=1`)

- **What:** `probe/mialloc.c` plus mimalloc v2.5.2 (`probe/third_party/mimalloc`, the official Microsoft repo,
  released 2026-09-12). The imports of malloc, calloc, realloc, free, _aligned_malloc, _aligned_free and
  operator new/new[]/delete/delete[] that point into msvcr90 are redirected in every module, including
  ones loaded later (DLL load notifications). Blocks that came from the CRT heap (made before the
  patch, or inside msvcr90 by _strdup/_malloc_crt) are recognised with mi_is_in_heap_region and passed
  back to msvcr90; realloc moves them into mimalloc. If mimalloc fails, the call falls back to msvcr90.
- **Settings:** no arenas (MiArenaMB=0, the default). Arenas are 128 MB reservations that double after
  8 and are never released; without them every 4 MB segment goes back to Windows when it empties.
- **Why it's safe here (checked):**
  - no game module imports _msize, _expand or _recalloc;
  - the CRT's _malloc_crt/_calloc_crt/_realloc_crt stay on the CRT heap, consistent with themselves;
  - the exe's only _get_heap_handle use (GetMemTrackerForDLL, exe+0x424800) just tests it for NULL;
  - the exe's TBB looks for tbbmalloc.dll (absent), then uses the exe's malloc/free imports, which
    are patched before exe code runs.
- **Modules redirected in the game:** exe, VP DLL, DB DLL, localization DLL, lua51, msvcp90. The DLLs that
  use msvcrt.dll (d3dcompiler, zlib, libeay, dbghelp) keep their own heap.
- **luagc chaining:** luagc's counting wrappers now call the mimalloc functions, and they patch a slot
  holding either the CRT export or its replacement, so either load-notification order works.
- **Test:** `CivilizationV_mitest.exe` (manifest-bound msvcr90 and msvcp90 like the game) plus
  `mitest_dll.dll` (no CRT, allocates only through msvcr90 imports, loaded late). It covers data
  integrity, cross-thread frees, realloc/calloc/malloc(0) semantics, alignments 1–8192, operators, CRT
  blocks (_strdup) freed and moved, a 64 MB block, the heap staying valid, and luagc counting msvcp90
  through mimalloc. **Free address space 4031 → 3839 MB with 200 MB of 1 KB blocks, back to 4031–4035 MB
  after freeing**; the NT heap would have kept the 200 MB.
- **Defender:** the stripped build was quarantined as `Program:Win32/Contebrew.A!ml` (an ML heuristic
  false positive: a proxy DLL that patches imports). The probe is now built without `-s`; that build
  passes.
- **Installed 2026-10-28** with MiMalloc=1 (ReserveLog and HeapHistogram still on). Previous DLL:
  `probe/build/msimg32.prev-20261028-reslog.dll`. **Undo:** `MiMalloc=0`.

### First mimalloc session (20261028_201545, same new Huge game, ~19 min so far)

- **Works:** all seven modules redirected, including the VP DLL when it loaded at 30 s and again at 50 s.
  The CRT heap stays at 0.1 MB; one CRT-heap free passed on; no fallbacks; no new exceptions.
- **Free address space:** 668 MB (3 min) → 437 (6) → 351 (9) → 351 (12) → 343 (15) → 339 MB (18 min).
  After the load phase it's almost flat: −12 MB in 9 minutes, where the NT heap lost ~70 MB in the same
  window (505 → 432) and kept falling to 246 MB at 51 min.
- **But the level is lower:** mimalloc reserved 2040 MB at 10 min (413 regions), against the NT heap's
  ~1880 MB at that point of the previous session. The game is further along now, so some of that is
  real growth, but mimalloc probably carries ~100–200 MB more overhead (size-class rounding, partly
  filled per-thread pages and segments). The largest free block is also smaller (91 MB, against ~300 MB).
- **Churn after loading:** ~14 segment reservations a minute, so the OS-call cost is negligible.
- **Next:** a stats build (mimalloc compiled with MI_STAT=1, merged every 16 k allocations per thread)
  reports live block bytes, pages, threads and abandoned segments, to split the overhead. It's staged in
  the game folder for the next launch; the build that was running is kept as
  `msimg32.loaded-20261028-2015.dll`. Depending on what it shows: fewer or smaller segments, or arenas
  sized for reuse.

### Quit-crash guard (2026-10-28, `LuaUnrefGuard=1`, on by default)

- **Same crash a third time:** session 20261028_201545 quit with the AV at lua51_Win32.dll+0x105b
  (luaL_unref on a freed lua_State, target 0x8D6935B0). It was also seen at 16:35 (before luagc) and
  18:33, so mimalloc isn't the cause.
- **Guard:** `probe/luaguard.c` points the luaL_unref imports of every module (exe and VP DLL import it)
  at a wrapper. The wrapper skips the call when the lua_State (0x48 bytes) or its global_State (0x80
  bytes at L->l_G) isn't committed readable memory. It logs the first five skips with the caller and
  the total at exit. A live state is always readable, so normal play is unaffected.
- **Test:** `CivilizationV_guardtest.exe` with a copy of the game's lua51_Win32.dll in build/. A live
  state's ref is really released (the same number is handed out again), and an unmapped state or an
  unmapped global_State is skipped without a fault.
- **Installed** together with the mimalloc stats build. Undo with `LuaUnrefGuard=0`.

### Crash with mimalloc: VP reads a deleted object (session 20261028_204549, 10.8 min in)

- **Crash:** AV at CvGameCore_Expansion2.dll+0xd91a9 (`mov ecx,[eax+4CCh]`, eax = 0xE4B07000). VP's crash
  log shows 377 MB free (largest block 118 MB), so this wasn't exhaustion. Dump:
  CvMiniDump_20261028_205636. The stack runs from a Lua UI call into VP.
- **Cause:** VP+0x21e40 is the Lua-to-C++ instance lookup (checks the argument is a table, reads its
  `__instance` light userdata). A script still held the table of a deleted game object. The pointer is
  inside mimalloc segment 0xE4800000, which was still reserved (the reserve log shows no release), so
  mimalloc had *purged* (decommitted) the freed page. The CRT heap leaves freed memory mapped and
  readable, so this stale read used to return old data silently. It's a latent VP/script bug that
  mimalloc exposed.
- **Fix:** `MiPurgeMs` defaults to -1 (mi_option_purge_delay = -1). Checked in the source:
  segment.c:896 sets `allow_purge = false`, and os.c:556 refuses purges, so freed pages stay committed.
  Address space is unchanged (purging only saved commit charge), and empty 4 MB segments are still
  released.
- **Residual risk:** a stale pointer into a segment that has been *released* (all its blocks freed)
  would still fault. That's rarer, but if it shows up, the answer is arenas (address space kept mapped
  and reused) with purging off.
- **Overhead (stats build, 10 min):** reserved 2069 MB, live blocks ~1727 MB (691 MB small ≤ 32 KB,
  1036 MB large) → **~340 MB overhead (≈20%)**; 411 segments, 16558 pages, 6 threads, nothing abandoned.
  The NT heap's overhead in the comparable session was ~120 MB at 10 min, growing to ~260 MB at 50 min.
- Test: `mitest` now checks that freed 40 KB / 200 KB / 1 MB blocks stay readable while their segment
  lives. It couldn't provoke a purge even with MiPurgeMs=10, so the fix rests on the source check.

### Session 20261028_210235 (no-purge build, ~16 min): two remaining sinks

- **Free space:** 433 MB at 5 min, flat to 9 min, then 434 → 321 MB from 9 to 15.5 min. No crash so far;
  the quit guard is patched into the exe and VP DLL.
- **mimalloc at 10 min:** reserved 2073 MB for 1737 MB of live blocks (701 MB ≤ 32 KB, 1036 MB larger)
  → ~335 MB overhead; 16649 pages; 6 threads.
- **Live VA diff, 10 → 15.5 min (+108 MB private):** 5 new mimalloc segments (20 MB); a 16 MB Lua-heap
  segment (lua_Alloc exe+0x2ce4aa); a 16 MB NVIDIA user-mode heap segment (nvwgf2um); and **two new
  32 MB regions the reservation log never saw.**
- **The 32 MB regions are GPU-driver memory:** 10 of them (308 MB) are absent from the reservation log,
  so they aren't created through the user-mode NtAllocateVirtualMemory stubs. Several are
  PAGE_READWRITE|PAGE_WRITECOMBINE (0x404), with commit rising and falling. These are kernel-created
  mappings for CPU-visible GPU allocations (dynamic/staging buffers), which explains the "unattributed
  32 MB regions" of the first session too. Possible levers (untested): NVIDIA "Low Latency Mode" / max
  pre-rendered frames 1 (fewer renamed buffers in flight), or the DX9 exe.
- **Tuning plan for mimalloc's overhead:** a size split. Keep blocks ≤ 16 KB on mimalloc (where the NT
  heap fragmented: the 8–63 B and 1–8 KB holes) and send larger blocks back to the CRT heap. The NT
  backend fits large blocks exactly and released big ones; its 16 KB+ free stayed ~10 MB in the
  histogram. That avoids mimalloc's large-object rounding and slice slack on ~1 GB of live data.

### Size split and a stronger quit guard (2026-10-28, installed)

- **Split (`MiMaxKB`, default 16):** mimalloc serves only blocks up to 16 KB. Larger malloc/calloc/new/
  new[]/_aligned_malloc blocks go to the CRT heap. realloc moves a block across the split when its size
  crosses it (both directions tested, data intact). free still routes by ownership. MiMaxKB=0 restores
  "every size on mimalloc". Expected: most of the ~335 MB overhead (on the ~1 GB of large blocks) goes
  away, while the small-hole fragmentation stays fixed. The CRT heap columns and heaphist are
  meaningful again (large blocks only).
- **Quit crash, second form:** session 20261028_210235 quit with an AV *writing* 0x8E653018 at
  lua51+0x3b05, inside luaL_unref (caller exe+0x50523d again). The guard's readability check passed:
  the small lua_State was still in mapped heap memory, but the registry's array part had been
  released.
- **Guard v2:** `luaguard.c` also wraps lua_close (it remembers each closed global_State, up to 16)
  and lua_newstate (it forgets one when a new state reuses the address). luaL_unref is skipped for a
  closed state, then for unreadable memory as before. Test: unref after lua_close is skipped; a new
  state made after a close still releases refs normally; the unmapped cases still pass.

### Crash with the split: the game overruns a 2 MB buffer (session 20261028_212523, 4.5 min)

- **Crash:** AV *writing* 0x2F9E6000 at exe+0x1e0e63 (`movss [esi+80h]`, esi = 0x2F9E5F80) on the main
  thread. Free space was fine: 520 MB, largest block 462 MB. Dump: CvMiniDump_20261028_212951.
- **The buffer:** reserved at 20.7 s with stack exe+0x413b02 → _aligned_malloc → malloc → NT heap
  big block. That's 2076 KB, laid out as 20 KB uncommitted in front (big-block randomisation), 2052 KB
  committed to 0x2F9E6000, then uncommitted. Under the split it's on the CRT heap, exactly as in the
  unmodified game.
- **The bug:** the function fills a record of at least 0x98 bytes (fields +0x20…+0x94) starting 0x80
  bytes before the end of the committed memory, so it writes past the end of the game's own buffer.
  That's latent in the unmodified game too (it faults only when the overrun crosses into an
  uncommitted page). With every size on mimalloc, the huge-block rounding hid it. First occurrence in
  crashes.log.
- **Fix:** large blocks routed to the CRT heap get `MiLargePadBytes` (default 256) of slack (malloc,
  calloc, realloc, new, new[], _aligned_malloc), about 10 MB in total. Test: 2 MB malloc and
  _aligned_malloc blocks at eight sizes take a 160-byte overrun; with MiLargePadBytes=0 the same test
  faults (0xC0000005), so it really exercises the bug. The guard and mimalloc tests pass.

### Crash after the padding: the September crash again (session 20261028_213635, 4.8 min)

- **Main thread:** AV reading 0xFFFFFFFF at exe+0x517414 (file offset; live +0x518014). Stack: exe+0x4fdf46,
  0x2b3477, 0x466919, 0x2a59a5, 0x59d8a, 0xb406c (main loop). **One second later:** an AV reading 0xB at
  exe+0x51f60c on the Miles sound thread (mss32 stack), then ntdll+0xefe01. 557–577 MB free.
- **The same pair is in crashes.log from 15/09/2026 19:26 (VP 5.4.6, ~1.1 GB free), before civprobe
  existed**, so it's a pre-existing game crash.
- The two previous sessions crashed 4.5–4.8 min after loading the same save (first as the buffer
  overrun, now this). That looks like one trigger in this save, with the allocator only changing which
  symptom appears first.
- **Test now:** MiMalloc=0 (luagc and the quit guard stay on); replay the same save to the same point.
  A crash there means it's the game's bug in this turn.
- **Result with MiMalloc=0 (session 20261028_214426):** it crashed anyway at 8.6 min. AV reading 0x2 at
  exe+0x4dc828 (file offset 0x4dbc28) on the main thread, through the same path: exe+0x34c82f ← 0x517f80
  ← 0x4fdf62 ← 0x466919 ← 0x2a59a5 ← 0x59d8a ← 0xb406c. **So the recent crashes are a game bug in this
  save's current turns, not mimalloc.** mimalloc only changed which symptom showed first. Crash times
  after loading: 10.8, 4.5, 4.8 and 8.6 min. Export names don't identify the subsystem (the frames sit
  between GetFileSystemMgr and the CRT stubs). Next: game logging (LoggingEnabled=1) to see what runs
  just before it, and an older autosave as the workaround.
- **Logging run (session 20261028_215507, MiMalloc=0):** a hang, not a crash, at ~7.5 min (22:02:38–41).
  The probe's sampler and every game log stopped at the same moment, so the whole process stalled. The
  sampler blocking (it takes the heap lock in HeapSummary) fits a thread stuck while holding a heap lock.
  This was during AI turn processing: net_message_debug ends at player 3's auto-moves. **CustomMods.log's
  last line, its only occurrence:** `CvUnit.cpp[5782]: jumpToNearestValidPlotWithinRange(1) failed for unit
  Rocket Artillery at plot (56, 53)`. That's a strong lead: a unit that can't be relocated, at the same
  point of the turn each time, would explain a deterministic crash or hang in this save (via different
  symptoms). Lua.log has only the known benign VPUI_loader error. Next freeze: leave the game running so
  the thread stacks can be captured (cdbX86 -pv attach).

### The freezes were the probe's own heap walk (2026-10-28 22:2x)

- **Second freeze, attached live (cdbX86 -pv, !runaway twice):** two threads spinning.
  - Thread 1 is the civprobe sampler, inside KERNELBASE!HeapSummary → RtlQueryHeapInformation →
    RtlpWalkHeap → RtlpWalkLFHBlock → RtlpIsLFHZoneAllocation, ~1 s of CPU per second, holding the heap
    lock.
  - Thread 0 (main) is spinning in the job system (exe!FJobSetAddJobImpl), waiting for workers that
    block on that heap lock.
- **Cause:** with MiMalloc=0 the CRT heap is back to ~1.9 GB of LFH blocks, and the sampler called
  HeapSummary on every heap every 2 s. The LFH walk checks every block against the zone list, so it gets
  slower as the heap grows, and here it stalled for minutes. The first "freeze" (session 215507: the
  sampler and every game log stopped at the same instant) fits the same cause, so the Rocket Artillery
  log line was probably just the last thing logged before it.
- **Fix:** `HeapStats` (default 0) gates the per-tick HeapSummary. The heap columns in samples_*.csv now
  read 0 unless it's enabled, and the address-space columns are unaffected. HeapHistogram is off in the
  game ini as well. Installed (the frozen process's DLL renamed to msimg32.loaded-222443.dll).
- **The crashes themselves still stand as a game bug:** the September match, and the same main-thread
  path with and without mimalloc.

### The recurring crash is in the audio engine (session 20261028_222613, MiMalloc=0, no heap walks)

- **Crash:** 5.6 min in, after a turn, **while scrolling the map** (user). AV reading 0x1 at exe+0x4dc828:
  `mov edx,[esi]` / `mov edx,[edx]` / `call edx`, a virtual call. The object's vtable pointer reads as
  **0x00000001**. Stack: main loop 0xb406c → 0x59d8a → 0x2a59a5 → 0x466919 → 0x34c82f → 0x4dc828. Dump:
  CvMiniDump_20261028_223152.
- **The object** is the singleton at exe RVA 0x5b54f34 (84 references). It's created lazily at
  exe+0x319425: new(0x46C) via the file/line new wrapper, tagged
  `FirePlace\FireEngine2\FAudioGame.h` line 0x39, so it's the **FAudioGame** audio object. It's
  destroyed at exe+0x37e8cd–0x37e8f6 (dtor, free, global cleared). A vtable of 1 means its memory was
  freed-and-reused or overwritten while the global still pointed at it.
- **The same subsystem shows up across the crashes:**
  - 213635: a second crash in mss32 (Miles) right after the main-thread crash.
  - September 2026: the same exe+0x517414 / exe+0x51f60c pair, with an mss32 frame.
  - 214426 and this one: identical exe+0x4dc828.
  - Every one happened with plenty of address space free, and with or without mimalloc.
- **Candidate workarounds (config.ini, game closed):** `Enable markered sounds = 0` (unit-animation
  sound triggers, likely heavy with ~1700 units on a Huge map), or as a decisive test `Audio = 1` (the
  config comment says 1 disables the audio system).

### Watchpoint on the FAudioGame vtable (installed 2026-10-28, `Watch=1`)

- `probe/watch.c`: the sampler polls the pointer at exe RVA 0x5b54f34 (`WatchGlobalRva`). When it changes,
  it arms DR0 (write, 4 bytes, `WatchOffset` 0) on every thread via Suspend/Get/SetThreadContext, and on
  new threads at each tick. A first vectored handler catches EXCEPTION_SINGLE_STEP with DR6.B0 and logs
  `watch: WRITE #n ... after <module+offset of the next instruction>: now <value>`, with registers and the
  return addresses on the stack, resolved against a module table the sampler caches (no loader calls in
  the handler). Then it resumes. The first 30 writes are logged in full; the count goes out at the
  periodic and exit reports.
- Test `CivilizationV_watchtest.exe` (Watch=1, WatchGlobalExport=g_watched): a thread created after arming
  overwrites the "vtable" with 1. Exactly one trap, at corrupt_object (+0x1536 is the instruction after
  the store) with late_thread on the stack. A write to another field doesn't trap. The mimalloc and guard
  suites still pass.
- Expected in the game: no trap until the corruption, or the object's destructor at exit. The trap's
  stack names the writer.
- **DR mode made the game quit:** session 20261028_223917 armed 60 threads at 22 s, and the game ran
  ExitProcess cleanly at 33 s, right after system_init finished. No crash, no trap. That fits Steam
  DRM checking the debug registers. **WatchMode=dr is unusable in the game.**
- **Page mode (the default now):** the exe's malloc/free imports are wrapped (matched by name). The
  allocation of 0x46C bytes whose stack holds exe+0x31942a (the FAudioGame new: the 0x42c090 wrapper
  falls through to the malloc import, and the free comes back through FSBADeallocate → free import) gets
  two private pages, with the object at first page + 4096 − 8. When the global points at it, the first
  page goes read-only. A write there faults, so the handler unprotects, sets TF, single-steps, logs the
  write (with stack for the watched field, otherwise counted) and re-protects. The free is logged with
  its stack. The handler is registered at the first tick, ahead of the probe's own.
- Test (watchtest, page mode): the vtable write is logged with the right function and thread and still
  takes effect; a same-page field write is counted and completes; an other-page write doesn't fault;
  the free is logged. The mimalloc and guard suites pass. Installed with Watch=1.
- **Two page-mode sessions never isolated the object.** The diagnostic build logged FAudioGame's
  malloc(0x46C) at 20.8 s with "caller differs" and an empty stack. Cause: w_malloc passed `&n` to the
  stack scan. Reading past a single variable is undefined, so clang reduced the scan to `n == ret` and
  the stack printout found nothing. Fixed by scanning from `__builtin_frame_address(0) + 2` (as
  topdown's HOOK_FROM does). watchtest now goes through a game-like wrapper with WatchCallerRva set to
  the real return address: the caller matches, the object is isolated, the vtable write and the free
  are logged. The regression suites pass; installed.
- The same crash happened again in between (session 225134, 252 s): an AV writing 0x2EE26030 at
  exe+0xcef89. That's a different site, not analysed yet.

### Root cause: the render command stream overflows (session 20261028_230012)

- **The watch worked:** FAudioGame was isolated at 20.8 s (caller exe+0x31942a matched) and protected
  from 22 s. Its vtable was never written. Instead the game crashed at 280 s writing 0x2EF68000 at
  exe+0x20932e: a record (fields +0x40…+0x80) starting at esi = 0x2EF67F90, running past the end of a
  2084 KB block.
- **The block:** reserved at 20.56 s by exe+0x4ce1e5 → aligned new exe+0x413a93/0x413b02 → msvcr90,
  from exe+0x1c9969. That's the same kind of block as the 21:29 overrun at exe+0x1e0e63 (2076 KB at
  20.7 s via 0x413b02).
- **What it is:** `core\CommandStream.cpp`. The factory exe+0x4bd0c0 news a 0x2080-byte stream and runs the
  ctor exe+0x4ce150(a1 = 0x1000, a2 = 0x400, bytes, 0): arrays of a1×2×0x20 and a2×2×0x80, plus
  a data buffer of 2×round16(bytes). The size goes in +0x34 and the second half's start in +0x2068.
  exe+0x1c9928 creates a pair (+0x6F4 / +0x6F8) with **bytes = 0x100000 (1 MB per half)**. On a Huge
  map with ~1700 units a busy frame (scrolling) overruns it. Next to unmapped memory that's the write
  crash; next to a heap neighbour it's silent corruption (FAudioGame's vtable = 1 → exe+0x4dc828 /
  exe+0x517414 plus mss32, the September crash). There are 42 command-stream factory call sites in
  total; only this pair is implicated so far.
- **Fix:** `probe/cmdstream.c` (CommandStreamMB, default 4) rewrites the two `push 100000h` immediates
  (RVA 0x1c992a, 0x1c9942) to 4 MB before they run (~20 s), only if the bytes match exactly
  (it refuses on other exes, as the test exe showed). +12 MB of address space. Installed with Watch=1
  still on (to confirm FAudioGame stays intact) and MiMalloc=0.

### Enlarging the stream broke rendering; slack instead (2026-10-28 23:2x)

- **CommandStreamMB=4 (session 20261028_231026):** the terrain was missing on load ("ground missing",
  user screenshot: icons and water only). On the next turn the game's error path exe+0x523dcb fired
  **116 times** (once in a normal session) and the game exited at 244 s. So something downstream of
  that stream assumes 1 MB, probably a fixed-size GPU buffer it's copied into. Rolled back to
  CommandStreamMB=0 (the option stays, default 0).
- **Slack fix (CommandStreamSlackKB, default 4):** both overrun crashes started the record *inside*
  the buffer and ran 0x14–0x18 bytes past it, so the writer checks the start, not the end. The
  stream's sizes stay untouched. cmdstream.c wraps the exe's malloc and _aligned_malloc imports. When
  the stack holds one of the constructor's three allocation returns (exe+0x4ce19a, +0x4ce1c5,
  +0x4ce1e5; the constructor's first bytes at +0x4ce150 are verified), it adds 4 KB. That covers all 42
  streams, including small ones inside heap segments where an overrun hits a neighbour (the likely
  FAudioGame path). Test `CivilizationV_slacktest.exe` (CommandStreamTestRets): the marked malloc and
  _aligned_malloc blocks are exactly 4 KB bigger; others are unchanged.
- **mitest was flaky** (large blocks "on mimalloc"): its 4 MB-alignment heuristic misfired when the CRT
  heap grew into a freed mimalloc slot, and HeapValidate misjudged too. The probe now exports
  CivProbe_IsMimalloc (mi_is_in_heap_region) for the test: 8/8 passes. All suites pass; installed.

### The overrun has no bound: capping the record allocator (2026-10-28 23:3x)

- **Slack wasn't enough (session 20261028_232318, 229 s):** an AV writing 0x2E309030 at exe+0x1062c0. The
  buffer was 2088 KB (slack applied); the record started at 0x2E308FA0, *inside the 4 KB slack*, and
  ran past it. So the overrun is many records, not one.
- **The writer:** exe+0x106220 takes the current stream `[[0x...2dd1d1c]+0x6F0]` (the overflowing pair's
  current member) and gets a 0x160-byte record from exe+0x26faa0 → **exe+0x4cfdc0**, which is a bare bump
  allocator: `p = [s+0x24] + [s+0x40]; [s+0x40] += (WORD)size; [s+0x30] = p; [s+0x4C] = size`, with
  no capacity check. 79 call sites.
- **Halves:** exe+0x4cdd90 swaps [s+0]↔[s+0x206C], [s+0x24]↔[s+0x2068] (the half bases), [s+0x58]↔[s+0x60]
  and [s+0x5C]↔[s+0x64] at the frame flip. The consumer (exe+0x4c06ef) uses `(ptr − [s+0x2068]) >> 4`, so
  record offsets are tied to the 1 MB half, which explains why enlarging broke the terrain.
- **Fix (CommandStreamCap, default 1):** exe+0x4cfdc0 (its 37 bytes are verified) now jumps to
  CivProbe_StreamAlloc. Same contract, but if `off + size > cap − 16 KB` it returns the scratch slot
  `base + cap − 16 KB` and doesn't advance the offset. Records past capacity are dropped (those objects
  aren't drawn that frame) instead of overrunning; the first drop is logged and the count reported.
- Test `CivilizationV_streamtest.exe`: a fake 1 MB half with a NOACCESS page right after it. 4000 × 0x160
  records → 2932 fit, 1068 go to scratch, the final offset is 2932 × 0x160, nothing is written past
  the half. All suites pass; installed (slack stays on; Watch=1; MiMalloc=0).

### The cap held; the audio side crashed in the same frame (session 20261028_233210)

- At 113.14 s the cap logged its first dropped record (a 1 MB half was full). In the same instant the
  game crashed in **Mss32!AIL_sample_status+0x4c**: `mov ecx,[esi+4]` gave 0, then a read of [0+0xD14].
  The HSAMPLE (0x2EA94030) had a NULL driver field, which Miles sets on release, so the audio engine held
  a stale sample handle. Call path: the render loop exe+0x34c82f → exe+0x4dc899 (FAudioGame) → … →
  exe+0x52308b (thin wrapper `push; call [AIL_sample_status]`). FAudioGame itself stayed intact (isolated,
  no write logged). So the heavy frames trip two separate bugs: the command stream (now capped) and
  stale Miles sample handles.
- **Fix (AudioGuard, default 1):** `probe/audioguard.c` gives every exe import from mss32.dll whose
  first parameter is an HSAMPLE (29 functions: the *sample* functions plus AIL_register_EOS_callback; not
  allocate_sample_handle or stream_sample_handle) a generated stdcall thunk. If the handle is non-NULL
  and its +4 driver field is NULL, it returns at once (`ret N` from the import's @N; AIL_sample_status
  returns SMP_DONE = 2, the rest 0) and counts the skip; otherwise it jumps to Miles. The sampler logs
  new skips per function.
- Test: a fake mss32.dll with game-style decorated import names (`_AIL_sample_status@4`) and
  CivilizationV_audiotest.exe. Live samples reach "Miles"; released ones return SMP_DONE/0 with the stack
  balanced; non-sample functions are left alone; with AudioGuard=0 the test faults like the game.
  All suites pass; installed.

### Shared scratch slot → overflow pool (session 20261028_233828)

- With AudioGuard on, the game crashed **20 ms after the first dropped record** again (215.89 → 215.91 s).
  It was inside audioguard thunk 13 (AIL_set_sample_volume_pan, reached through exe+0x523339), reading
  [0xFFFFFFFF + 4]: the sample handle was garbage. Two runs crashing in audio right after the first
  drop points at the cap itself: every over-capacity record shared one scratch slot, so code that reads
  its record back later in the frame (animation-triggered sounds, probably) got another object's data.
- **Fix:** over-capacity records now come from a per-stream overflow pool (4 MB, VirtualAlloc'd lazily,
  reset when the stream's offset is 0 at the start of a frame). Each gets distinct memory, and the
  offset still doesn't advance, so they stay out of the frame. The shared slot is only the last resort
  (counted). AudioGuard also rejects handles < 0x10000 or ≥ 0xFFFF0000.
- Tests: streamtest runs two frames of 4000 records each: 2932 fit, 1068 in their own pool memory, zero
  aliased. audiotest covers 0xFFFFFFFF too. All suites pass; installed.

### Dropping records breaks the audio; split layout instead (session 20261028_234539)

- With the pool, the first drop came at 105.09 s. By 106.70 s AudioGuard had skipped 103
  AIL_sample_status, 103 AIL_set_named_sample_file and **2849 AIL_set_sample_volume_pan** calls on
  invalid handles. At 107.42 s Miles crashed in AIL_sample_status on a handle whose driver was
  non-NULL but whose +0xD14 table was NULL (garbage). **Dropped records are never consumed, and the
  consumer writes results back (sound handles included) that game code reads later.** So dropping is
  wrong; the unmodified game consumes overflow records (it only tramples memory).
- **Split layout (CommandStreamSplit, default 1):** the constructor's half split at exe+0x4ce1e8
  (`89 46 24 03 c7 89 86 68 20 00 00`, verified) becomes `call split_stub` + 6 NOPs. The stub sets
  [esi+0x24] = buffer and [esi+0x2068] = buffer + half + spare (spare = min(half, 1 MB) for halves of
  4 KB or more, else 0), and leaves eax = buffer + half as the original did. The data allocation (returning
  to exe+0x4ce1e5) grows by 2 × spare + slack. The half size (+0x34) is unchanged, so records keep their
  half-relative addresses and overflow continues into the half's own spare (consumed as in the
  unmodified game). The cap now drops only beyond half + spare. The split is applied only after the
  allocation hook is in place.
- Tests (streamtest): the stub gives half 2 at +0x800 / +0x100000 / +0x300000 for halves of
  0x800 / 0x80000 / 0x200000, with eax = buffer + half. With the split on, 2932 records go into the
  spare, the 2089 beyond double capacity get pool memory, and none land past the spare. All suites
  pass; installed.

### Split held; the audio still reads unprocessed records (session 20261028_235227) — where things stand

- **No drops this time** (no "filled up" line), so the split layout absorbed the overflow. Two audio
  crashes, which the game's handler reported separately (two crash dialogs):
  - 188 s: Mss32!AIL_sample_status+0x52;
  - 201 s: AIL_stop_sample+0x57.
  Both had HSAMPLE 0x2F1E4F10 with "driver" = 0x42000000 (32.0f) and "index" = 0x450B9000 (2224.0f).
  The handle points at float render data, not a Miles sample, and it passed AudioGuard's checks.
- **Conclusion:** the renderer processes at most ~1 MB per stream half (about 2978 0x160-byte records).
  Records beyond that are never processed, whether they're dropped, in the spare area, or (in the
  unmodified game) on top of other memory. The fields the consumer should write back, sound handles
  among them, keep stale data, and the animation-sound code passes them to Miles. That's very likely
  the September crash family too. Raising the consumer's real capacity needs finding what broke the
  terrain with CommandStreamMB=4: something keyed on +0x34 failed from load, before any big frame
  (a GPU buffer creation?). Not found yet; grepping 0x8b0000–0x8d8000 for +0x34 was too generic.
- **Proposed next (user to choose after the compact):**
  - `Enable markered sounds = 0` in config.ini, game closed. Its comment says "triggered sounds from
    units (3dsmax Note tracks)": the animation-marker sounds that read handles back from records. It
    keeps music, ambience and UI sounds. All memory fixes stay on.
  - Or keep tracing the consumer's capacity limit.
- **Current probe ini (game folder):** AllocTracking 0, ReserveLog 1, HeapHistogram 0, **MiMalloc 0**,
  LuaGcStepMB 64, **Watch 1** (page mode; FAudioGame never written so far, could be turned off),
  CommandStreamMB 0, CommandStreamSlackKB 4, **CommandStreamSplit 1**, **CommandStreamCap 1**,
  **AudioGuard 1**, LuaUnrefGuard default on. config.ini LoggingEnabled = 1 (turn it off when done).

## The real overflow: the command array, not the records (2026-10-29, after the compact)

- **The bad handle was a command entry.** In dump CivilizationV_DX11.exe.94272.dmp (second crash, 201 s, raised
  while the first crash dialog took focus: WM_ACTIVATEAPP -> pause all sounds), AIL_stop_sample got
  HSAMPLE 0x2F1E4F10 from FAudioSystemMiles::PauseSound (exe+0x520940 -> exe+0x520320: sample table
  `[this+0x58]`, 12-byte slots {flag, HSAMPLE, flag}, type 1, index 0). That address is inside the
  stream's own 4104 KB data block (region 0x2EEE0000 in vamap_..._exception2), 0x4F10 past 1 MB of half 2.
  So the sample table had been overwritten with a pointer into the render data.
- **Who writes such pointers:** the command stream's *command* array. Layout, from the constructor
  exe+0x4ce150 and the flip exe+0x4cdd80:
  - commands: a1 x 0x20 bytes per half at [s+0x14]/[s+0x2070], count [s+0xc]. Entry: +0 key, +4 = -1
    (or an argument), +8 word, +0x10 argument, **+0x14 = record pointer**, +0x18 record size.
  - groups: a2 x 0x80 per half at [s]/[s+0x206c], count [s+4], +0x5c = first command index.
  - data [s+0x24]/[s+0x2068], plus a 1024-dword list [s+0x2078]/[s+0x207c].
  - Appenders: commands exe+0x4ccd80 (159 callers), +0x4cce00, +0x4cce90 (also the list); groups
    exe+0x4cd930 (begin), +0x4cdb20 (end), +0x4cdb40, +0x4cdc50, +0x4cdcb0, +0x4cdd20. **None checks a
    capacity**, just as the record allocator doesn't.
  - Both garbage handles seen so far are command-entry fields: 0xFFFFFFFF (+4, session 233828) and a
    record pointer (+0x14, session 235227). The FAudioGame vtable = 1 in September fits as well.
- **The overflowing stream** is created at exe+0x1c9964 (the user's game takes the single-stream branch,
  [0x3011238] == 0; exe+0x1c993b/+0x1c995d make a pair otherwise): a1 = 0x1000, a2 = 0x400, 1 MB per half.
  4096 commands of 0x160-byte records is ~1.4 MB of data, so in the unmodified game the data overflows
  first. My split layout gave the data 2 MB per half, so busy frames now ran past 4096 commands and
  wrote 32-byte entries past the 256 KB command array, into the next heap block (the Miles sample
  table). The earlier "write-back" theory (the renderer writes sound handles back into records) was
  wrong: dropping records "broke audio" only because the commands still overflowed.
- 42 factory call sites create streams with very different sizes (a1 0..0x8000, a2 1..0x4000, data 0..4 MB;
  ~87 MB in total). A spare area for every stream (the 2026-10-28 split) cost ~34 MB of address space.

### Fix (installed 2026-10-29 00:30)

- **Spare halves only for the overflowing stream** (CommandStreamSpareCallers = the constructor's
  return address: 0x1c9940, 0x1c9962, 0x1c9969). The malloc/_aligned_malloc wrappers find the
  constructor's frame (from the allocation's return address: 6 args, 12 for the groups, then
  edi/esi/ebx, then the constructor's return address), enlarge commands by 3x, groups by 1x, data by
  min(half, 1 MB) per half and record a "grant" for the pointer. The constructor stubs at exe+0x4ce1e8
  (data) and exe+0x4ce20a (commands/groups; the constructor's return address is at [esp+0x1c] there)
  place the second halves from the grants, so the layout always matches the allocation. The game's
  capacity fields (+8, +0x10, +0x34) stay unchanged. The big stream: commands 0x4000 per half, groups
  0x800, data 2 MB.
- **Bounds on every writer:** record allocator as before (now the whole half is usable, overflow goes
  to the per-stream pool); the command, group and list counters are replaced at their increments
  (exe+0x4ccdf9, +0x4cce7b, +0x4ccefd, +0x4cdb30, +0x4cdc36, +0x4cdc9e, +0x4cdd0e, +0x4cdd6e; all bytes
  verified, no branch lands inside them). A counter stops at its half's limit: the last spare slot when
  the stream has spare, else the game's own capacity (tiny streams with exactly 1 slot keep it), so
  later entries overwrite one slot instead of marching through memory.
- **Stats:** the flip call exe+0x4bd9a7 goes through flip_note: per-stream peaks, a log line the
  first time a stream goes past the game's capacity or fills a half, and a table in each report.
- **AudioGuard v2:** live-handle tracking (AIL_allocate_sample_handle / AIL_stream_sample_handle add,
  AIL_release_sample_handle / AIL_close_stream / AIL_close_digital_driver remove). The 29 sample
  thunks call sample_ok, so a forged-but-plausible handle like 0x2F1E4F10 is now refused; releasing an
  invalid handle is skipped too.
- Tests: streamtest (records, data/command/group half stubs, all 7 counter stubs entered with the
  game's frame shapes and `ret N`, both bound modes), sparetest (new: fake constructors with the
  game's stack shape; the parent writes the child's ini; spared callers get the spare, others only
  slack; the stubs lay out from the grants), audiotest (forged handle, released handle, double
  release, garbage), slacktest. All pass.
- **What to look for in the next session's events log:** "streams created from ... get spare halves",
  "counters too", then "stream from exe+0x1c9969 went past the game's capacity" (expected on busy
  frames, now harmless) and, at exit, the per-stream peak table. Any "filled a half" line or
  audioguard skip means something still overflows.
### First run of the fix (session 20261029_003228): no crash, but only half applied

- No crash. At 195 s the overflowing stream (exe+0x1c9969) hit 4096/4096 commands and 4 commands shared
  the overflow slot instead of trampling the sample table (the bound worked). At 241 s it went past its
  1 MB data into the 1 MB spare. The small streams logging "filled a half" (1/1 commands etc.) are at
  their normal capacity, not overflowing.
- **But the command and group arrays got no spare:** the live stream read with cdb (-pv) showed
  [s+0x2070]-[s+0x14] = 0x20000 and [s+0x206c]-[s] = 0x20000. The probe's counters showed 18
  padded allocations and only one grant ever made (the data one). Cause: the FAudioGame watch's
  malloc wrapper (installed after cmdstream, so called first) reserves 0x310 bytes of stack, and
  cmdstream's constructor search covers only 48 dwords, so it never saw the constructor for
  malloc'd arrays (only _aligned_malloc, which watch doesn't wrap). So the slack had never applied to
  those arrays either, in any session with Watch=1.
- Fix: probe.c now runs watch_init before cmdstream_init, so the exe's malloc reaches cmdstream's
  wrapper first. sparetest's child now runs with Watch=1; against the old build it fails exactly like
  the game (commands/groups unspared, data spared) and passes with the new order. watchtest still
  passes. Installed 2026-10-29 ~00:45 (previous: msimg32.loaded-20261029-0032.dll).
### Confirmed in play (session 20261029_004317, corrected build): the fix works

- Spares applied (commands +12288, groups +1024, data +1 MB per half on exe+0x1c9969). 171 constructor
  buffers padded (vs 18 before the init-order fix). No crash, no audioguard skips, nothing past a
  full half (0 records, 0 commands, 0 groups, 0 list entries).
- 10-minute peak table: the big stream peaked at **5665 commands** (game: 4096) and **1280 KB of data**
  (game: 1024 KB); **30489 of 48102 frames (63%)** were past the game's own capacity. The unmodified
  game would have written up to ~1570 command entries (~50 KB) past the command array on most
  frames. Headroom now: 5665/16384 commands, 1280/2048 KB data, groups 2/2048.
- Every other stream stayed far below its capacity (largest: exe+0x4038fd 4114/32768 commands,
  exe+0x2b83fa 848/32768); the "full" small streams (exe+0x199b2, +0x2388c8, +0x2436ce) sit at their
  exact normal size (1/1, 3/3). So the targeted spare (one stream) is the right scope.

## Quit crash, third form: a collected Lua thread (session 20261029_190826)

- **Crash:** the user quit after 8 min (1.0.0, mimalloc on). AV *writing* 0xA1C81D20 at
  lua51+0x3b05 (`mov [ecx],edx`, ecx = `L->top`), inside luaL_unref called from exe+0x50523d.
  Dump: crashlogs\CvMiniDump_20261029_191629. No `luaguard: Lua state ... closed` line before it,
  so the main state was still open, and the guard's checks passed.
- **What the call is:** exe+0x505220 is a small Lua-ref destructor, `{lua_State *L; int ref}` →
  `luaL_unref(L, LUA_REGISTRYINDEX, ref)` (skipped when L is NULL or ref is LUA_NOREF), reached
  from the UI context teardown (exe+0x84334 / +0x7fd52 / +0x19e621, nested).
- **The L it passes is a coroutine, not the main state:** L = 0xA2592D60, while luagc had confirmed
  the main state as 0xA258E960 (g = 0x29C98638); the registry table it read (0x29C98B50) belongs to
  that same g. So the thread had been garbage collected (its last reference dropped during
  teardown) while the state lives on. Its lua_State block was still mapped (a mimalloc small block
  stays committed) with l_G intact, but its stack had been freed and its mimalloc segment returned
  to Windows, so luaL_unref's first push through `L->top` faulted. In the unmodified game the same
  write lands in freed NT-heap memory (silent). This is also the likely explanation of the second
  form (session 20261028_210235, also under mimalloc); "the registry's array was released" was a
  guess.
- **Fix (guard v3):** for `t == LUA_REGISTRYINDEX`, luaguard calls the real luaL_unref with
  `G(L)->mainthread` (global_State +0x70, read off lua_newstate's `mov [eax+70h],esi`) when it
  differs from L, after checking that the main thread is readable and points back at the same g.
  The registry is shared by all threads of a state, and luaL_unref needs only one stack slot, so
  this is exact for live threads too. Logs the first five calls whose thread stack was unmapped
  and a count at exit. Other `t` values are unchanged.
- **Test:** guardtest's new case uses an allocator that gives Lua stacks (>= 512 B) their own
  pages, released on free, and never frees small blocks. A coroutine takes a ref, is dropped and
  collected, then luaL_unref goes through it: the 1.0.0 guard faults at lua51+0x3b05 exactly like
  the game; the new guard releases the ref (handed out again) on the main thread. All six cases pass.


## GameEvent crash on a corrupt Lua table; guard v3 under the game's Lua lock (session 20261029_192343)

- **Crash:** 5.6 min in (first run of guard v3 and the other post-1.0.0 commits, mimalloc on).
  AV reading 0x478 at lua51+0x1b072 (luaH_getnum, under lua_objlen) on the game-core thread, not
  the UI thread. Dump: crashlogs\CvMiniDump_20261029_192919. First crash ever at this site.
- **Chain:** the Community Patch DLL fires a GameEvent → exe+0x2012a3 calls `lua_cpcall` on the
  *main* Lua state (L = 0x297C85C0, the one luagc confirmed) through exe+0x504e30, which holds the
  exe's Lua lock (a CRITICAL_SECTION at exe+0x5c6bad4, preferred 0x606bad4) → exe+0x48e450 fetches
  the event's `TestAll` and calls it → `TestAll` (C closure exe+0x1a75e0, registered at
  exe+0x2cb643 next to `Test`) walks the event's listener table with lua_next and takes `#` of each
  table value. One value was garbage: `node` = 0x460, `lsizenode` 0 (a live table never has that;
  empty tables point at the dummy node). Its block is in the `heap:lua` region, not mimalloc.
- **What the other threads were doing:** the UI thread (thread 0) was waiting for that same Lua
  lock (entered at exe+0x2cc7f4, one of ~45 UI call sites of the lock wrappers exe+0x5042a0 /
  +0x5042b0). So no race was in progress at the dump; the table was damaged earlier.
- **Ruled out or unlikely:** luagc (collects only where a DB C function returns to Lua, the same
  kind of point a script's collectgarbage() uses); mimalloc (not the table's memory); address space
  (305 MB free at the crash, lowest 163 MB at 4 min; Lua out-of-memory raises a Lua error).
- **Suspect: guard v3.** It sends *every* registry unref made through a non-main Lua thread to the
  main thread. Every UI context is a coroutine, so that's every UI callback release, not only those
  of collected threads. The ref destructor exe+0x505220 doesn't take the Lua lock, and the
  game-core thread runs GameEvents on the main thread under it. A release on the UI thread during a
  GameEvent would make both threads move the main thread's `top` at once, which can leave a stale
  TValue where a table is expected. In the unmodified game those pushes went to the context's own
  (idle) stack. One crash, about 10 earlier v2 sessions of 30-49 min without it: suggestive only.
- **Change (guard v4):** the guard finds the lock from the wrapper bytes (`mov ecx, cs; jmp w`
  at exe+0x5042a0 / +0x5042b0, both wrappers `push ecx; call [..CriticalSection]`) and runs each
  registry unref under it: inside it already (counted), or after a `TryEnterCriticalSection`
  (logged, first five). It never waits for the lock: when another thread holds it, the unref goes
  into a queue (4096 entries) and is done by the next unref that has the lock. Without the lock
  (another exe) it reroutes only threads whose stack is unmapped. Counts are logged at each
  10-minute report, at each exception and at exit. guardtest: lock free, lock held by another
  thread (deferred, then done), collected thread under the lock; all pass.
- **What the next session should show:** how many unrefs happen outside the lock and how many
  are deferred. Many deferred ones would confirm the race was possible; none would point back at
  the game or a mod.

## AI turn time (2026-10-29/30, Alexander Huge save, turns ~367-425, VP + EUI)

Tools (outside the game, read-only except where said): a 1 kHz poller of the core-to-UI queue
and the core thread's state (ReadProcessMemory + QueryThreadCycleTime), stack samplers for the UI
and core threads (Wow64SuspendThread), an external Lua 5.1 stack walker, and the AI turn windows
from the autosave times (`AutoSave_Post_N` -> `AutoSave_N`).

### Mechanism

- **The queue.** Owner object at exe+0x2610200 (static 0x2a10200); queue at owner+0xc08: two byte
  buffers (write side `[+0x20]&1`, read side the other) of records `[size][type]`, a 0x34-byte
  descriptor at +0x10 (player +0x18, unit +0x1c, active count +0x3c, flags +0x40: bit0 check
  conflicts, bit1 barrier, bit2 stop behind a deferred record), push counter at +0x28. The core
  thread appends; the UI thread drains it once per frame in exe+0x385af0 (lock dword
  owner+0xd00): compaction keeps records still active (+0x3c != 0, e.g. a running animation),
  the transfer (exe+0xac910) defers a record that conflicts (exe+0x20eb10 -> 0x327a30) with an
  active record or an earlier one of the same pass (same unit / plot / player), records with bit2
  stop behind it, then a sequential dispatch by type (exe+0x23aba0). So a unit's records go one
  per frame.
- **Hand-over.** The main loop (exe+0x3253e0) locks record processing (exe+0x10a170, byte
  exe+0x29e0447) for each core update, and only once the queue is completely empty, then hands
  the update over on the 100 ms frame-time tick (the accumulator restarts every 100 ms even when
  the hand-over is skipped). VP's `CvGame::updateMoves` runs one AI player per update (`break`
  after the first active AI), so an AI turn is ~62-64 strict ping-pongs of core update -> screen
  catch-up.
- **Type 0x49** is "play a unit animation". The ACTIVATE (exe+0x26d5d0) and BOMBARD_DEFEND
  (exe+0x18c1d0) versions are posted with flags=1 and stay active 2.3-3.3 s. VP posts ACTIVATE
  from `CvUnit::build`, `greatperson`, `DoSpreadReligion` and others whenever the plot is visible,
  unless `MOD_UI_QUICK_ANIMATIONS` and Quick Movement are both on (VP's option was 0).

### Split of AI turns 376-379 (avg 67 s, nothing changed yet)

Core AI work 20%; hand-over latency 7% (4.6 s); UI record handlers 29% (6.5-39 s; ~44% of it
database queries from Lua: Global Relations' `InfoAddictCivRelations.lua` redraw on TeamMet, VP's
`TechPanel.lua` on `SerialEventGameDataDirty`; the engine's conflict scan only ~1% of UI time);
records waiting 44% (~15 s blocked behind active ACTIVATE records, ~12 s one-per-frame trickle).
The Quick Turns mod is active but the user's settings keep quick movement/combat on everywhere.

### Changes and results

| Change | Effect (per AI turn) |
|---|---|
| VP `UI_QUICK_ANIMATIONS` 0 -> 1 | blocked behind animations 15.4 -> 3.6 s |
| `GameCoreIntervalMs=10` | hand-over latency 4.4 -> 1.4 s (A/B by pausing the probe's pacer thread live) |
| `FSBADisable(1)` (first live via CreateRemoteThread, then `SmallBlockFallback=1`) | core CPU 21-30 -> 14-18 s |
| `UiQueuePasses=4` | waiting ~20 -> ~8 s; UI handlers unchanged |

AI turns: median ~64-68 s before; 41-50 s with the first three; 30-35 s (turns 419-425) with all
four. Free address space at 10 min was 371-402 MB in these sessions (394 before).

- **The small-block allocator.** FSBA (exports `FSBAAllocate`/`FSBADeallocate`/`FSBADisable`,
  allocator cached at exe+0x5d96fa0): one 32 MB region committed up front, 4 KB chunks, 16 size
  classes (32-512 bytes), per-class chunk descriptor arrays. VP's operator new is
  `FSBAAllocate(n) ?: malloc(n)`, delete `FSBADeallocate(p) || free(p)`. With the region full
  (8,176 chunks; classes 96 and 160 bytes with no free slot) every allocation of such a class
  scans the whole descriptor array (exe+0x41bed0) under a lock and fails: 26.5% of busy core
  samples, 0% after the switch. Every reader of the disabled flag (`[alloc+0xb8]`) only decides
  where a *new* block comes from; deallocation and realloc decide ownership by address range, so
  switching mid-game is safe. The exe's own "chunk allocation failed" flag (exe+0x5d96fa4) is set
  during startup loading, when the region is full for a while and then shrinks to ~1/3, so it
  isn't a usable trigger; `SmallBlockFallback` waits for a full region for 30 s, which in
  practice also happens at the main menu (60 s after launch).
- **Core time rose after a reload** (13.5 -> ~22 s per turn, same code profile, same updates per
  turn, not the pacer): game state, not explained further.
- **Extra queue passes.** `UiQueuePasses` calls exe+0x385af0 again from the run loop's
  `PeekMessageW` (import of exe; return addresses exe+0x41db0d / +0x41dc8d) under the conditions
  of the game's own call site (exe+0xb3fe1): the per-frame check `[[exe+0x8c8e74]+0x1460]`
  vtable+0x3a4 false, normal mode (`[exe+0x2610f80]+0x2b0 & 2` clear), not locked for a core
  update, records waiting; stops when a pass moves nothing. 8,483 extra passes in 2,911 frames
  moved 29 MB of records in the first 10 minutes; the user saw nothing wrong.

### Left (not ours to change, or not done)

- AI work itself (~15 s per turn, VP's DLL) and mod Lua handlers (~7 s): report-only.
- The rest of the trickle (~6 s) and the blocked BOMBARD_DEFEND animations.

## Stutter while panning during AI turns (2026-10-30, same save, turns 429-450, session 20261030_003648)

The user reported choppy panning during AI turns with all the AI-turn speedups on.

**Measuring frames from outside.** No clean per-frame counter turned up in the exe's .data. The
probe's own `frames` count for the big render stream (cmdstream stats, creator exe+0x1c9969,
flipped once per frame) does the job: it was read with ReadProcessMemory at ~5 kHz, with symbols
from the loaded DLL file (no `-s`, so `nm` gives `_g_streams`, `_g_passes`, ...). Main-thread
stacks were sampled every 2 ms, and inside frames past 25 ms the Lua stack was read too
(`global_State` from luaguard's `_g_okG`). Live A/B: `_g_passes` written to 1 or 4 with
WriteProcessMemory (read on every pass), the coretick pacer thread suspended and resumed.

| Setup (5 min each, panning) | AI turn | AI-turn time in frames > 33 ms | > 100 ms per turn |
|---|---|---|---|
| All speedups on, unpaced | 39-46 s | ~29% (23-34% per turn) | ~23 |
| `UiQueuePasses=1`, pacer suspended | 63-73 s | ~17% | ~21 |
| Paced passes, share 25%, target 20 ms (driven from outside) | 44-58 s | ~17.5% | ~18 |
| Own turn (for reference) | | 4-15% | |

- About the same number of long frames per AI turn in every setup (~90-150): the UI's work per
  turn is fixed, and the speedups compressed it into a shorter turn.
- Frames that ran extra passes: 18% over 33 ms, 112 over 100 ms (of 2,996); frames that didn't:
  0.3% (of 11,535). Nearly all of them ran all 3 extra passes (median 17 ms, p90 59 ms).
- Inside long frames only ~10% of samples were inside an extra pass and ~7% inside the game's own
  pass; most were later in the frame, in UI handlers that the records set off: VP
  `TechPanel.lua:111` 12%, Global Relations `InfoAddictCivRelations.lua:1420` 2.5%, unit flags,
  city banners. So pacing by the passes' own time would undercount; the whole frame's overrun is
  the charge.
- One ~800 ms frame per AI turn in every setup: the autosave (zlib on the stack). Game behaviour.
- Note for stack heuristics: the run loop keeps `esi` = the PeekMessageW import (our hook) on the
  stack, so the hook's start address shows up in every main-thread stack.

**Fix:** `UiQueueShare` / `UiQueueFrameMs` in uiqueue.c (see SETTINGS). The run loop's first
PeekMessageW call marks each iteration; every iteration longer than the target takes its overrun
out of a bucket that refills at the share of wall time, capped at the target; extra passes run
only while it is positive.

### Round 2: the handlers themselves (2026-10-30 afternoon, same save, sessions 131238 and 133814)

The user still saw hitches while panning in AI turns.

- **The pacing never let a pass through.** Most sessions after `ac6f1ad` logged "0 extra passes,
  thousands paced": the bucket had no floor, so the save's load (and time in the background) put
  it minutes in debt. Capped at 5 targets; the next session ran 337 extra passes in 10 minutes.
- **Handler table.** The queue's record handlers are (fn, context) pairs in .data at owner + 8
  (+ [owner] * 0x600), one per record type (0xc0 types); the dispatcher exe+0x23aba0 calls
  `fn(record + 0x44, context)` (cdecl). Registration functions write the pair, e.g. exe+0xc583c
  for type 0x66 with the generic handler exe+0x23e530 that calls every Lua listener of the event
  with the payload. Record types are the UI event numbers (`typemap`: push type; call
  exe+0xa2210 with the event's name). `UiQueueStats=1` puts a timing thunk in each fn slot.
- **10 minutes of mostly AI turns (session 131238):** 938 frames over 33 ms (253 s), 297 over
  100 ms (211 s, 140 s of it in handlers). By type: `TeamMet` 0x77 348 calls, 100 s, ~290 ms each
  (max 342); `HexFOWStateChanged` 0x72 50,106 calls, 22.5 s (max 70 ms); `SerialEventGameDataDirty`
  0x66 478 calls, 20.8 s (~43 ms; CP `TechPanel.lua` rebuilds its unlock buttons each time);
  `ActivePlayerTurnStart` 0x7c 10 calls, 3.5 s. `TeamMet`'s only listeners are Global Relations'
  `InfoAddictCivRelations.lua:1424` and `DiploTradesStatus.lua:638`, both arg-less full redraws of
  a screen that redraws itself when opened.
- **Merging** (`UiDirtyCoalesceMs`, `UiTeamMetCoalesceMs`): a record of the type within N ms of
  the end of its last run is skipped and kept (payload copy); the last one runs N ms later from
  the PeekMessageW hook, under `may_process` and the queue lock (owner + 0xd00, try-locked like
  exe+0x385af0). Live A/B by writing the window from outside (scratchpad `ab.py`, frames from the
  render stream's flip count, only frames with queue activity within 0.5 s):

| Setting, A/B | Off | On |
|---|---|---|
| Data-dirty 250 ms, 90 s switching, 15 min | p95 59.7 ms, 59% of time in frames > 33 ms | p95 41.2 ms, 54% |
| TeamMet 2 s, 60 s switching, 5 min | 127 runs in 2 off-minutes | 42 runs for 215 records in 3 on-minutes |

  Each `TeamMet` run left is still a ~300 ms frame (the >300 ms frames per minute tracked the
  runs), so the window for Global Relations is 30 s. The user saw no out-of-date panels with the
  250 ms data-dirty merging.
- Left: `HexFOWStateChanged` (per-plot, not mergeable), `ActivePlayerTurnStart` (once a turn,
  ~400 ms, mostly Global Relations again), the autosave.

### Round 3: retuning the pacing with the merges on (2026-10-30 evening, same save, turns 392-422)

The user asked whether a longer `GameCoreIntervalMs` would make the game smoother. Method: one
setting per AI turn, chosen from shuffled blocks (each block has every setting once), switched
from outside when `AutoSave_Post_N` appears and back to the defaults when `AutoSave_N` does; the
user panned through every AI turn. Frames from the render stream's flip count as before. Interval:
the probe's pacer was pointed at a scratch float (its `g_acc`) and the script paced the
accumulator itself (100 = no early hand-overs). Share: `_g_share` / `_g_passes` written live.
Three AI turns per setting.

| `GameCoreIntervalMs` (turns 392-403) | AI turn (median / mean) | Time in frames > 33 ms | > 100 ms per turn | p95 |
|---|---|---|---|---|
| 10 | 30.3 / 33.2 s | 24.7% | 30 | 20 ms |
| 20 | 36.0 / 36.3 s | 26.8% | 39 | 19 ms |
| 40 | 33.8 / 35.9 s | 31.5% | 50 | 24 ms |
| 100 | 36.5 / 36.2 s | 36.9% | 60 | 39 ms |

| Pacing (turns 404-415) | AI turn (median / mean) | Time in frames > 33 ms | > 100 ms per turn |
|---|---|---|---|
| `UiQueuePasses=1` | 46.8 / 46.6 s | 18.4% | 31 |
| share 10 | 47.6 / 46.1 s | 19.6% | 32 |
| share 25 | 39.8 / 40.0 s | 20.3% | 28 |
| share 50 | 36.5 / 36.8 s | 19.5% | 23 |

| Pacing (turns 417-422) | AI turn (median / mean) | Time in frames > 33 ms | p99 |
|---|---|---|---|
| share 50 | 33.3 / 34.7 s | 22.8% | 107 ms |
| unpaced | 27.5 / 28.7 s | 29.2% | 142 ms |

- A longer interval is worse on every measure; how a slower core makes frames longer wasn't
  looked into.
- With the merges on, the pacing between no extra passes and share 50 no longer shows in the long
  frames (per-turn spread 17-22%, bigger than the gaps); it only costs AI-turn time. Unpaced still
  stutters (every unpaced turn 28-30%, every share-50 turn 21-25%). Default changed to 50.
- The last 9 s of turn 415's frames were lost (the script was stopped with its file buffered).

## Where the 3.9 GB goes, and the map detail grids (2026-10-30, session 20261030_003648)

The session ran nearly out of address space: 371 MB free at 10 min, 185 at 50, a 69 MB low at
51 min (largest free block 10.8 MB). Three layers:

- **Static, from load:** ~3.7 GB in use at 10 min, the same as earlier sessions on this save
  (371-402 MB free). At 50 min (vamap, committed + reserved): mimalloc 904 MB (769 live), images
  459, unowned private blocks under 16 MB 545, ten blocks of 16 MB or more 406, GPU-style 32 MB
  regions 416, reserve-only regions 237 (thread stacks, a 128 MB NVIDIA reserve), CRT heap 284,
  process heap 206, Lua heap 193 (for ~110-150 MB of Lua data), other ~240.
- **Creep, 10 -> 50 min (-186 MB):** three new 32 MB regions (+96 MB of address space for +35 MB of
  commit), one new 16 MB segment each in the Lua, CRT and process heaps (+48), mimalloc +28 (live
  growth), 1-4 MB blocks +15.
- **Spikes:** free space dips 40-90 MB for one or two 2 s samples. The deepest come 4-6 s after
  each `AutoSave_Post_N` file (the AI turn starting): turns 448-453 dipped to 128, 108, 101, 89,
  69 and 89 MB, private commit 80-90 MB above its baseline. No dip at the `AutoSave_N` saves.
  Unattributed (no ReserveLog in that session).

### The big static blocks are the game's own allocations

The same sizes appear in every session since 2026-10-27 (118.75, 68, 59.4, 4 x 24, 32, 16 MB ...).
They are CRT heap big blocks (the NT heap places a big block at a random offset into its own
reservation, hence the reserved head of 12-60 KB and the 4 KB tail). The AllocTracking report of
session 20261028_164019 names them: the 118.75 and 59.4 MB ones are the map detail grids below,
68 MB the temp heap (config.ini `Maximum Temp Heap Size`), 4 x 24 MB the fluid fog's per-thread
buffers (`MaxSimultaneousThreads`), 32 + 16 MB the fluid fog's map-sized buffers (sized from the
loaded map's bounds, exe+0x329dd4, so nothing to gain there). Not MSAA render targets, as their
sizes first suggested (2560 x 1440 x 8 samples x 4 B = 112.5 MB, not 118.75).

### The 128 MB NVIDIA reserve in the low 2 GB

Reserved at ~8.8 s by `nvwgf2um.dll+0xa2f63` (a VirtualAlloc wrapper with a log call) from
`NvMemMapStorage.dll+0x5ce51`; only 416 KB of it is ever committed. Before that, the D3D drivers
(NVIDIA, and the AMD iGPU's amdxx32 during adapter probing) reserve 128 MB at 0x10020000 a dozen
times and 32 + 32 + 20 MB at 0x10020000/0x12020000/0x14020000, releasing each again. Driver
internals; not pursued.

### The map detail grids (exe+0x991270): 178 MB sized for the largest map

- Startup (exe+0x75f55 onwards, inside the engine init that returns to exe+0x76022): the exe runs
  `Select Value From Defines Where Name = ? limit 1` for `MAX_SUPPORTED_MAP_X` and `_Y` (defaults
  0x98 = 152 and 0x60 = 96 when the query gives nothing or <= 0), passes them through exe+0x2d10d0
  (mode 1: {X, Y + 4}) and calls exe+0x24c620(grids, 16 x X x (Y + 4) = 243,200), which stores
  that count at +0x9c and reserves two FFastVectors (data, size, capacity): +0x28 with 512-byte
  elements (exe+0x1d73a0, 118.75 MB) and +0x68 with 256-byte ones (exe+0x2cfe10, 59.4 MB).
- The elements are tiles of 16 x 16 fine cells, uint16 in the first grid and bytes in the second,
  behind tile directories at +0x1c / +0x5c (8-byte entries: a flag word, then either the uniform
  value or the tile's index; width in tiles at +0x14 / +0x54). Reader: exe+0x493e1c.
- Map load (exe+0x47e350): computes the map's tile count (fine width / 16 x fine height / 16), calls
  each grid's Init (exe+0x798cb0, exe+0x5f84d0) with it, and Init reserves that many if the
  capacity is smaller (the only other callers of the two reserve functions). Then every tile gets
  a block (exe+0x654550: reuse a freed tile, else push back, growing through exe+0x412c00 when
  full). So the startup reserve only saves a reallocation for maps up to 152 x 96; for a smaller
  map most of it is never used. The count at +0x9c is written there and read by none of the
  object's methods.
- **Measured in the game** (session 20261030_024539, read with cdb from the live process after the
  save loaded): both grids hold 0x1a860 = **108,640 tiles of 0x3b600 = 243,200** reserved, exactly
  16 x 97 x 70 for this 97 x 66 map. ~99 MB of the 178 MB is never touched.
- **The Defines don't exist.** `Civ5CoreDatabase.db` and `Civ5DebugDatabase.db` have no
  `MAX_SUPPORTED_MAP_X/_Y` rows, so the query's `Step` returns false and the hard-coded 152 x 96 are
  always used (the earlier note that they are "read from the base DB" was wrong: editing defines
  would only work by adding the rows).
- **`MapGridReserve=1` (mapgrid.c), first version: no effect.** It hooked only the exe's
  `Database::Results::GetInt` import for the two reads (return addresses exe+0x75fa4 / +0x75fe3);
  without a row they are never reached (the probe's counters stayed 0, grids full size at 25 s).
- **Second version (built, not installed):** also hooks the exe's `Results::Step` import at the
  two sites (exe+0x75f94 / +0x75fd3) to say there is a row, and GetInt returns 1 there without
  reading. The startup reserve is then for a 1 x 1 map and the map load sizes the grids. No code
  bytes changed (CEG). `mapgrid:` report lines give both grids' tiles in use and capacity.

### Crash loading the save with the first mapgrid build (session 20261030_023923, ~75 s)

VP's `CvGame::Read` (CvGame.cpp line 11013) logged "Potential savefile format mismatch" and threw a
`stdext::exception("unknown")` through exe+0x42b4c0; VP's handler wrote crashes.log and
`CvMiniDump_20261030_024038` (unhandled C++ exception, 2 GB still free). Just before it VP released
the 16 MB it reserves at VP+0x494cc3 during loading, which it keeps in a good load. The probe's
events log just stops (no handler of ours saw it). In that build the grid hook had no effect (above),
so the differences from the last good build were the pass-through GetInt hook and the reservation
log now logging placed reservations (`ReserveLog=1` was on). **The next launch of the same build
and ini loaded the same save fine** (session 20261030_024539), so the crash is not deterministic
and its cause is unknown. The previous load crash (20261030_015612, the D3DLog build, NULL handler
in exe+0x2a5970) was also during a load, also with a new exe import hook; unexplained too.

### Second version crashed too: the same NULL handler as the D3DLog build (session 20261030_030248)

The Step + GetInt build worked as meant (2360 MB free at 1 min instead of 2118) but crashed 252 s in,
while the save loaded: `call eax` with eax = 0 at exe+0x2a59a3, in the record dispatcher
exe+0x2a5970 (records [size][type] at esi, handler table of (fn, ctx) pairs at static exe+0x160e408,
called from exe+0x59d8a), the identical stack to the D3DLog build's crash (20261030_015612). VP's
dump has no data memory, so the record type is unknown. Builds that crashed while loading: D3DLog
(exe `GetProcAddress` import hooked), mapgrid v1 (`Results::GetInt`, the VP read error), mapgrid v2
(`Step` + `GetInt`). The pacing build, whose exe import hooks (msvcr90, lua51, `PeekMessageW`) have
run for days, never has. Working theory: new hooks in those exe import slots trip the Steam DRM
(CEG), whose tamper responses aren't always a clean quit. Unproven, but the fix avoids them.

**Third version (`mapgrid_tick`, data only):** the sampler thread waits until both grid vectors
hold the startup reserve with no tiles, unchanged for 10 ticks (~20 s, at the main menu), then sets
their capacity and data pointer to 0 and frees the blocks with the exe's own aligned delete
(exe+0x413cf0, as Init frees an old block). The map load then reserves the map's tile count. No
import hooks, no code bytes. Installed 2026-10-30 ~03:20 with ReserveLog=1.

### The reservation log missed placed reservations

topdown.c logged only reservations with no address requested. The unattributed 32 MB regions
above 2 GB (and whatever makes the spikes) may be reserved at addresses their callers chose, so
those are now logged as well, with 0x40000000 added to the type. (The NT heap's big blocks were
logged all along, as commits: type 0x1000.) restest has a case for it.

### Levers not tried yet

- `MaxSimultaneousThreads` 4 -> 2 (config.ini): -48 MB of fluid-fog buffers, plus some command
  streams. The job workers mostly wait (2026-10-29 sampling), but frame time in busy scenes is
  untested.
- `Maximum Temp Heap Size` 68 -> 32: -36 MB. Measured fine on 2026-10-27; the crash it was
  suspected of was the command-stream overflow.
- Lua heap on mimalloc: an import hook on the exe's HeapAlloc/HeapReAlloc/HeapFree/HeapSize for
  the Lua heap handle (lua_Alloc exe+0x2ce440 uses those four, and HeapSize for its byte count, so
  the exact requested size has to be kept). Estimated gain only ~15-25 MB plus the Lua heap's
  16 MB segment steps.
- The GPU driver's 32 MB regions: NVIDIA's "Low Latency Mode" / max pre-rendered frames 1 might
  mean fewer buffers in flight. Needs the new reservation log first, to confirm who reserves them.

## The growth after 30 minutes: GPU driver chunks; the dips: mimalloc segment churn (2026-10-30)

- **The 32 MB regions are made in the kernel.** In session 20261030_024539 (ReserveLog with placed
  reservations) none of the 10 regions in the exit VA map appears in the reservation log, at any
  address. So they never go through the ntdll allocation stubs: the graphics kernel maps the D3D
  driver's CPU-visible allocations into the process in 32 MB chunks. The first three sit at
  0x18020000 / 0x1a020000 / 0x1c020000 from the main menu, the rest above 2 GB.
- **Inside them (session 20261030_003648):** the plain cached ones (PAGE_READWRITE) held 145 MB in
  125 pieces at 10 min and 147 MB in 422 pieces at 50 min, mostly 128 and 256 KB: small buffers
  mapped and unmapped over and over, leaving holes, so new chunks get added though the mapped total
  hardly changes. The write-combined ones (0x404) grew from 50 to 83 MB mapped. Looks like the
  driver's upload/staging and dynamic buffers; who asks for them is not known.
- **The dips at AI-turn start are mimalloc segment churn.** In 024539 the reservation log shows
  4 MB mimalloc segments (msimg32+0x31fe8, mimalloc's OS allocation) reserved and released within
  seconds in bursts of 30-60 (120-240 MB) at 339, 345, 369-374, 446-451 and 470 s, under VP and
  CvGameDatabase (Lua DB results) frames: a burst of short-lived small allocations, given back at
  once ("no arenas: empty segments go back to Windows"). Transient, not growth.
- **New probe lines:** `gpu chunk:` when a new driver region shows up (2 s resolution), `gpu chunks:`
  with each report and at exit, three samples CSV columns. Regions of exactly 32 MB cut into 3 or
  more pieces or with write-combined pages count; the small-block region and Steam's 32 MB reserve
  (one or two pieces) don't. Build installed 2026-10-30 ~03:25 (sha 2746e5a3), no other changes.
- **Next:** match new chunks with what the game does (AI turns, autosaves, diplomacy screens), then
  try driver settings one session each: NVIDIA "Threaded optimization" off for the game, "Low
  Latency Mode" on.

### NVIDIA "Threaded optimization" off: no visible effect (session 20261030_034206, 14 min)

Same build and ini as 20261030_032156; only the NVIDIA per-game setting changed (the user's). Driver
chunks: 4 by the main menu, 4 while loading, then new ones at 196 and 202 s (AI turn 471), 258 s
(AI turn 472), 372 s (the end of AI turn 473) and 432 s (6 s after AI turn 474 ended), none from
432 to 850 s (6 more AI turns). 11 chunks at most (10 in the 16.6 min reference session), mapped
~185-210 MB, only reserved up to 165 MB; 299 MB free at 10 min (384 in the reference). So the
setting doesn't stop the chunks; new ones come in the first few AI turns after a load, then
level off for a while (the 50-min session 003648 added three more at 35-50 min).

### The trigger finder: `GpuWatch=1` (gpuwatch.c, 2026-10-30)

- The driver's CPU address for a buffer comes from D3DKMTLock / Lock2. The 32-bit `win32u.dll`
  stubs `NtGdiDdDDILock` / `NtGdiDdDDILock2` / `NtGdiDdDDICreateAllocation` have the same shape as
  the ntdll ones (`mov eax,n; mov edx,Wow64SystemServiceCall; call edx; ret 4`) and are hooked the
  same way as topdown.c's. After a successful lock: VirtualQuery the returned `pData`
  (D3DKMT_LOCK +0x14, D3DKMT_LOCK2 +0x0c); a 32 MB region not seen before gets a stack record;
  every 16th lock (at most 20 a second) gets a sampled one with the mapped piece's size.
- Stacks: the first 16 return addresses, plus up to 32 return addresses inside the exe from up to
  64 KB of stack (the driver's frames are deep and full of stale code addresses; a plain 16-frame
  scan never reached the caller's code).
- Test (scratchpad d3dtest.c, a hardware D3D11 device on the RTX 5080 mapping dynamic and staging
  buffers): every new region was caught by the Lock2 that first mapped into it, all on the calling
  thread, attributed to the test's own `Map` call (2801 Lock2 / 2819 CreateAllocation in 8 s).
- Installed 2026-10-30 ~04:15 (sha 09c1a70e) with `GpuWatch=1`; Threaded optimization should
  stay off for that session, so the driver locks on the game's threads.

### The load crashes were the game's event queue overflowing (2026-10-30 05:00)

- **GpuWatch session 20261030_045643 crashed 148 s in, while loading:** `call eax` with eax = 0 at
  exe+0x2a59a3, stack exe+0x2a59a5 < 0x59d8a < 0xb406c, the same as the D3DLog (015612) and
  mapgrid v2 (030248) crashes. GpuWatch hooks no exe import (only win32u stubs), so the theory that
  new exe import hooks trip the DRM is out. All three were bad luck in a game bug.
- **The queue (object at exe+0x160e400):** two queues (dword +0 picks one; only 0 in use), each a pair
  of halves of 0x400080 bytes: used bytes at +0 / +0x400080, records from +0x80, flip counter at
  +0x800100, the game's own high-water mark at +0x800104; handler tables of 192 (fn, ctx) pairs at
  +8 + queue * 0x600. A post (~240 inlined sites, e.g. exe+0xc230) picks half `counter & 1`, takes
  its offset with `InterlockedExchangeAdd(&used, size)` through the exe's import (slot
  exe+0x5d647c), then writes `[size][type][0][0]` and the payload. **No capacity check.** Once a
  frame the main loop (exe+0xb4067 -> exe+0x59d20, try-lock via InterlockedCompareExchange at
  +0x1001000) bumps the counter (exe+0x90df0), zeroes the new half's used bytes, updates the high
  mark and walks the old half (exe+0x2a5970: `call [table + type * 8]`, `add esi,[esi]`).
- **Measured:** in the next session (045941, the pacing build, no crash), the live high-water mark
  read **0x511f80 = 5.07 MB**, 1.07 MB past the half. So every load of this save overflows. What
  that does depends on the timing: half 0's overflow runs into half 1, whose used word the next
  flip zeroes (a zero size would loop forever) and whose start new posts overwrite while the main
  thread still walks the old records (garbage type -> NULL handler, the crash); half 1's overflow
  starts exactly on the flip counter (record size -> counter, type -> high mark), so the next
  flip can pick the wrong half: 5 MB of events dropped and an old half's records replayed.
  Replayed or dropped events could also explain the earlier crashes in handlers called from this
  dispatcher (exe+0x466919 < 0x2a59a5: the FAudioGame vtable = 1 crash and the September pair).
- **Fix (msgqueue.c, `MsgQueueGuard=1`):** the exe's InterlockedExchangeAdd import slot points at a
  hook. Only the four used-bytes words count. A post that would go past 4 MB, on any thread but the
  main one, waits (Sleep(1)) until two flips have emptied its own half (the caller already has the
  half's address, so it can't be moved to the other half), then takes offset 0 there. The main
  thread's own overflowing posts are counted, not stopped. A wait gives up after 20 s and no post
  waits again until the counter moves (in case the main thread waits for the poster). The sampler
  logs the high-water mark past 4 MB and the overflowing posts' sites and threads.
- **Test (msgqueuetest.c):** a stand-in queue, 4 posting threads, a "main thread" flipping every
  5 ms with a 300 ms stall every 40 frames. Guard on: 5.6 M posts, 0 past a half's end. Guard off:
  ~98 M would-be overruns. Installed 2026-10-30 ~05:10 (sha 7af24b0a) with `GpuWatch=1` too.
- **Open:** whether the overflowing posts come from the main thread (then the guard can't help, and
  the next step would be dispatching early from the main thread) or from the core/job threads.
  The first `msgqueue:` lines of the next load answer it.

### The overflowing posts are the main thread's own; spill instead of wait (session 20261030_051035)

- The waiting guard ran: 6,585 posts found their half full, **all on the main thread** (sites
  exe+0x2fad97, 0x404de, 0x2fac7a, 0x22af85, 0x10383e, 0x469a5d, 0x8e82e, 0x20610f), during the
  35 s load stall whose stack is in the game's Lua UI code (lua51 -> exe+0x7815). High-water mark
  5.07 MB again, and the same NULL-handler crash 1.4 s after it (116 s). The main thread can't
  wait for itself, so waiting can't fix it.
- **Spill (msgqueue.c v2):** the post sites all write their record at `half + 0x80 + returned
  offset` (checked at 0xc230, 0x2fad97, 0x404de), so the InterlockedExchangeAdd hook returns an
  offset that points into a spill buffer (8 MB, allocated at the first overflow) for a post that
  doesn't fit, and for every later post to that half until the next pass (same counter, queue and
  half), to keep the order. The InterlockedCompareExchange import slot (exe+0x5d646c) is hooked
  too: when the pass's try-lock on +0x1001000 comes in and the spill holds records, the hook calls
  the game's own pass (exe+0x59d20; its try-lock re-enters the hook and goes through), then, if the
  counter moved, takes the lock and calls `fn(record + 0x10, ctx)` from the queue's table for each
  spilled record (sanity: size >= 0x10, 4-aligned, type < 192), unlocks, and returns 1 so the
  game's call skips its own (second) pass. Records the half held come first, then the spill.
- **Test (msgqueuetest.c v2):** a stand-in pass called the way the game calls it, main-thread posts
  every frame plus a 5.1 MB "load" every 25th frame, handlers that post. Main thread only (the
  game's case): 815,500 records over 15 loads all delivered, once each, in order, 0 overruns; with
  the fix off, 138k overruns. With two worker threads both modes lose or reorder a few records: a
  post reserved before a flip and written after it races the walk, which is the queue's own design.
- Installed 2026-10-30 ~05:25 (sha dea1af0e) with `GpuWatch=1`.

### First GpuWatch results (session 20261030_093651, 23.7 min, clean quit)

- **Event queue:** never above 0.30 MB this time (the two crashed loads: 5.07 MB), so the spill
  wasn't exercised. Whatever posts the 5 MB doesn't happen on every load of this save.
- **Free address space:** 587 MB at 5 min, 473 at 10, 352 at 20, low 314 at 23. 300 s -> 1400 s:
  private commit +207 MB, GPU regions 10 -> 11 (+32 MB). VA diff 10 -> 20 min: mimalloc +72 MB
  (live small blocks 613.6 -> 688.3 MB), GPU +32 MB reserve, the rest < 10 MB each. mimalloc's live
  total goes up and down across sessions (003648: 730, 677, 780, 749, 769 MB at 10-50 min; 225634:
  649, 635, 685, 740, 671), so that's turn-phase data, not a steady leak; the steady part remains
  the GPU regions.
- **GPU regions:** 4 at 1 min (load), 8 at 115 s, 10 at 160 s, then 11-12 for the rest, and the count
  also drops (12 -> 11 at 20 min): the driver does release some. 21 regions were first mapped by
  a lock over the session (old addresses reused). 41,082 Lock2 and **43,981 CreateAllocation** calls
  in 24 min (~30/s), about one new allocation per lock.
- **Who maps (sampled 1 in 16):** almost everything is the main thread's render path, main loop
  exe+0xb40cd -> 0x2282bd -> 0x6136c -> 0x4c6c96 / 0x4c6c8f, then:
  - exe+0x4c1373 (590 samples, 1205 MB of pieces) and exe+0x4c14c6 / 0x4c1432:
    `ID3D11DeviceContext::UpdateSubresource` (vtable +0xC0; context pointer at static 0x3010584).
    0x4c1373 updates whole resources from the array at static 0x301a708 (index edi); 0x4c14c6
    updates subresource edi of [ebp+0x2b8] with row/depth pitch [esi+0xd8]/[esi+0x1b0].
  - exe+0x4c1319: `Map(..., D3D11_MAP_WRITE_DISCARD)` (vtable +0x38), then the copy helper
    exe+0x4c1c80 (frames 0x4c2dc6).
  - New regions after the load: all from 0x4c14c6 (1 MB pieces) or 0x4c2dc6 < 0x4c1319 (3-3.6 MB).
  - Job threads: exe+0x4bfdb2 < 0x4c0721 < 0x4c293e < 0x41d8ef. NVIDIA's own thread (DDRAW
    < nvd3dum) maps ~100 MB too.
- **Local driver test (scratchpad d3dupd.c, RTX 5080):** 300 UpdateSubresource calls of a 2 MB
  texture made 3 allocations; split into 64 KB bands, 50; a 2 MB DEFAULT buffer, 0. The driver
  pools plain UpdateSubresource uploads, so splitting the game's updates isn't a fix, and the ~30
  allocations a second come from something else.
- **Next:** gpuwatch now records sampled stacks for CreateAllocation too (`gpuwatch: allocations
  by ...`, every 8th, at most 10/s). Tested with d3dtest (allocations attributed to the test's Map
  calls). Installed 2026-10-30 ~10:20 (sha 1a9bd8ad) with the queue fix.

### The queue fix in the game, and who makes the driver allocate (session 20261030_100718, 12 min, clean)

- **Queue fix worked:** during the load the half reached exactly 4.00 MB; one main-thread post
  (exe+0x22af85) found it full, then **6,779 posts (1.11 MB) went to the spill and all 6,779 were
  delivered after the next pass** (0 bad, 0 without a handler). The load stall lasted ~35 s between
  the first spill (118 s) and the pass (130 s). No crash; the game quit cleanly at 12 min.
- **CreateAllocation stacks** (15-16k in 12 min): exe+0x4c1373 (~30% of samples), exe+0x4c1432
  (~18%), the load path exe+0xa0074 < 0x27a227 (~9%), Map DISCARD exe+0x4c1319 (~6%), NVIDIA's own
  thread (~5%), exe+0x4362ea < 0x167442 (~5%), exe+0x413d0a < 0x4e2d1a, exe+0x416ed0 < 0x416093.
- **exe+0x4c1290 is the per-frame flush of pending uploads:** two lists of buffers (count
  [0x30142c0], 0x30-byte entries ending at 0x3020860, resources at 0x301a708[i]; and count
  [0x3068880], entries 0x3074e20, resources 0x306ecc8[i]): entry type 2 -> Map(WRITE_DISCARD) +
  memcpy (ptr at 0x3011240) + Unmap, else a whole-resource UpdateSubresource; the source data is
  then freed (0x8bde10 on 0x3011248). A third list (count [0x413e300], 0x398-byte objects at
  0x4170b80) updates texture mips with UpdateSubresource (0x4c14c6) and frees the data (0x813cf0).
- **Local test, buffers the GPU is using** (64 DEFAULT buffers of 16-256 KB, each updated whole once a
  frame then read by a CopyResource, 300 frames, 2.4 GB): 210 allocations, committed in the
  driver's 32 MB regions 2 -> 31 MB; in 64 KB pieces 208 allocations and a new 32 MB region. The same
  updates to idle buffers: 0. A 2 MB texture in use: 3. So the driver's upload pool grows with the
  volume uploaded per frame into in-use buffers, and that pool is what spreads over new 32 MB
  regions. Splitting updates doesn't help.
- **Where that leaves the growth:** the GPU regions grow when a frame uploads more than the pool
  has had to hold before (the load: 4 -> 8-10 regions; then ~1-2 more per 20 min), and the game's
  live heap swings 630-780 MB by turn phase. A real fix for the GPU part would replace the game's
  per-frame UpdateSubresource of in-use buffers with a fixed upload ring of our own (a vtable hook
  on the context, CopySubresourceRegion from a persistent DYNAMIC/STAGING ring): large and risky
  render-path work, not started.
- GpuWatch set back to 0 in the game ini (MsgQueueGuard stays 1).

### GPU trim: give the driver's upload pool back (gputrim.c, 2026-10-30)

- Session 20261030_102618 (4 min, clean): 10 driver regions after the load, 320 MB of address space
  for 201 MB mapped (119 MB of holes). Queue max 0.46 MB.
- **Local test (d3dupd.c):** after the in-use-buffer uploads had grown the pool to 61 MB in 3
  regions, `IDXGIDevice3::Trim` (no ClearState) dropped it to 2 MB and released one 32 MB region;
  ClearState + Trim afterwards changed nothing more; one frame of in-use uploads after the trim
  rebuilt 8 MB (60 allocations).
- **gputrim.c (`GpuTrim=1`, off by default):** the device comes from the game's immediate context
  pointer (exe+0x2c10584, as read by the upload flush at exe+0x4c12fb; its vtable must be in
  d3d11.dll), QI for IDXGIDevice3. The trim runs on the main thread at the start of a run-loop
  iteration (uiqueue.c's PeekMessageW hook at exe+0x41db0d, now installed for this even with
  UiQueuePasses=1). The sampler asks 15 s after the region count last grew, and every
  GpuTrimMinutes (5). Each trim is logged with the regions, mapped/reserved MB and free address
  space before and after (the after values from the next sample).
- Installed 2026-10-30 ~10:55 (sha 8cb096da) with GpuTrim=1; previous build
  msimg32.prev-20261030-msgqueue.dll, ini backup civprobe.ini.before-gputrim-20261030.
- **First run (session 20261030_103653, 10 min, clean): no trim happened.** Nothing logged by the
  main-thread side: the check that the context's vtable is in d3d11.dll most likely failed (it
  returned silently). v2 (sha 1e599309) accepts a vtable in any loaded module once the object
  answers QueryInterface for ID3D11DeviceContext, logs where its vtable is, and warns if a
  requested trim hasn't run after 2 min. (uiqueue's "0 extra passes" there is old behaviour, as in
  sessions 093651 and 102618.)
- **Second run (session 20261030_104915, 10 min, clean):** "the game's context pointer 0EB63E88
  (vtable 0EB63E8C in no module) isn't a D3D11 context; off". The object exe+0x2c10584 points to has
  its "vtable" 4 bytes after itself: a wrapper of the game's own, not the D3D11 context. v3 (sha
  ce7a2421) wraps the exe's `D3D11CreateDevice` import instead (the exe imports it statically from
  d3d11.dll; CreateDXGIFactory1 from dxgi.dll) and keeps an IDXGIDevice3 of the device it returns.
  Tested in CivilizationV_d3dtest.exe: device captured, QI fine.
- **Third run (session 20261030_110123, 8.6 min, clean):** the wrapped import never ran ("asked 2
  min ago and no trim yet (device 00000000)"), so the game doesn't create its device through that
  import slot. v4 (sha below) also points d3d11.dll's export-table entries for D3D11CreateDevice and
  D3D11CreateDeviceAndSwapChain at wrappers (GetProcAddress lookups), logs the caller of the
  creation, and checks 30 s in whether the exe's import slot still holds the wrapper. Tested: a
  program that calls D3D11CreateDevice through GetProcAddress gets the wrapper.
- **Fourth run (session 20261030_111237, 6.8 min, clean): Trim ran, and doesn't help.** The device
  was caught via the export table: the game creates it through NVIDIA's API
  (`nvapi_impl.dll+0x2aa8d4`, with a context); dxdiagn.dll made ~20 probe devices first. Four trims,
  8-20 ms each: mapped in the driver's regions -4 to -16 MB, free address space identical before
  and after every time (2118, 696, 502, 502 MB). The 9 -> 8 region drop was only the probe's
  classification; no region was released. Unlike in the test, the game's regions also hold live
  resources, so none empties. **GpuTrim removed from the tree** (source kept in the session
  scratchpad as gputrim.c).
- **Address-space use, 10 -> 50 min of session 003648 (~190 MB lost):** GPU regions +96 (3 new),
  mimalloc +28, Lua heap +16, CRT heap +16, process heap +16, 1-16 MB private blocks +15. The Lua
  heap holds ~190 MB for ~110 MB of live Lua (its peak counter: 146-150 MB): ~80 MB of slack an NT
  heap never gives back.

### Map grid v3 never actually ran; trying it now (2026-10-30 ~11:30)

Sessions 032156 and 034206 log the old "get room for the loaded map only" message (an earlier
build), and their grids kept 243,200 tiles. v3 (sha aa6aaecf, the committed build) installed with
`MapGridReserve=1` (ini backup civprobe.ini.before-mapgrid3-20261030). Expect "mapgrid: gave back
the startup reserve ... 178 MB" at the main menu and 108,640-tile grids at the 10-minute report.

### Lua heap on mimalloc (luaheap.c, `LuaHeapMimalloc=1`, built 2026-10-30)

- lua_Alloc (exe+0x2ce440): nsize 0 -> bytes -= HeapSize(heap, 0, p); HeapFree. p NULL -> HeapAlloc(heap,
  0, n); bytes += n. Else bytes += n - HeapSize(p); HeapReAlloc(heap, 0, p, n). Counters at static
  0x2dd2084 (bytes) and 0x2dd2080 (peak); heap handle at static 0xc94b1c (RVA 0x894b1c). Slots
  (RVA): HeapSize 0x5d63f4, HeapFree 0x5d63f8, HeapAlloc 0x5d63fc, HeapReAlloc 0x5d6400.
- Other users of the handle: HeapCreate + HeapSetInformation(LFH) at exe+0x44b912 (startup, never
  destroyed), HeapCompact at exe+0x6217af, a HeapLock/HeapWalk stats pass at exe+0x63df20.
- Wrappers on those four exe slots, for that handle only: mimalloc block with an 8-byte header
  (requested size, tag "LuaM"); HeapSize returns the requested size so the game's counter stays
  exact; blocks that aren't ours (mi_is_in_heap_region + tag) go to the real heap.
- luaheaptest: 4 threads, 1.2 M random alloc/realloc/free with size and content checks, a pre-hook
  block, another heap, HEAP_ZERO_MEMORY: 0 errors, no busy blocks left in the Windows heap.

### Map grid v3 works (session 20261030_112602, 8.8 min, clean)

"mapgrid: gave back the startup reserve of the map detail grids (243200 + 243200 tiles, 178.1 MB)"
at 46 s (main menu); at exit the grids had room for 108,640 + 108,640 tiles (53.0 + 26.5 MB), the
map's own size: ~99 MB saved. Lowest free 543 MB at 7 min (the four sessions before: 389-478 MB at
5-10 min). Next: LuaHeapMimalloc=1 on top (sha f589b6cb; ini backup civprobe.ini.before-luaheap-20261030;
the watcher falls back to msimg32.mapgrid3b-20261030.dll).

### Lua heap on mimalloc works (session 20261030_113554, 14.4 min, clean; with map grid v3)

- 786,239 Lua blocks, 102.9 MB live in mimalloc at 10 min (peak 122 MB); 0 fallbacks, 0 foreign
  frees; all freed by exit.
- VA at 10 min against session 093651: Windows Lua heap 193 -> 20 MB (what's left is the heap's own
  startup commit), mimalloc 768 -> 908 MB (Lua's 103 MB live is in there), private >= 16 MB
  615 -> 516 MB (the map grids), GPU regions unchanged. Net: Lua ~-33 MB, grids -99 MB.
- Free at 10 min 686 MB (sessions 093651 / 100718: 503 / 362); lowest 623 MB at 14 min.

### DXVK test (2026-10-30 ~12:05)

- The GPU regions hold mostly live resources plus holes (Trim freed only 4-16 MB); new regions
  after the load start with 1-3.6 MB pieces (texture mip uploads exe+0x4c14c6, 3 MB DISCARD buffers
  exe+0x4c1319) in a space whose holes are mostly 128-256 KB.
- DXVK 3.1.1 (github.com/doitsujin/dxvk, released 2026-09-15; tar.gz sha256 40565b4a...608), x32
  d3d11.dll + dxgi.dll. d3dupd.exe with them on the RTX 5080 (driver 610.88, Vulkan): one setup of
  39 MB in the 32 MB regions, then 0 allocations and no growth through every mode (NVIDIA's
  D3D11: ~210 allocations and a new region for the in-use buffer uploads).
- Installed in the game folder (d3d11.dll sha1 6a5c5a6a, dxgi.dll 99529b4f) with the probe build
  of c12da4e. revert_dxvk.sh renames them to *.dxvk-disabled.dll after an unclean exit. DXVK logs
  CivilizationV_DX11_d3d11.log / _dxgi.log in the game folder.
- **Result (session 20261030_120020): DXVK is a net loss in this 32-bit game; removed.** Free
  address space at 10 s (main menu) 2,674 MB against ~3,350 MB normally: ~670 MB for Vulkan and
  NVIDIA's Vulkan driver in-process (image +320 MB, private +400 MB). DXVK hid the NVIDIA GPU
  (reported AMD 0x1002; the PC also has an AMD iGPU), dxdiagn's survey created ~20 devices, and the
  game then asked for display modes (1,296 times) and a swap chain with a garbage format
  (2565355805); swap chain creation failed and ~2 GB went in 2 s (28-30 s), 37 MB free, then the
  process died. Even with that fixed, the startup cost is several times the GPU regions' growth
  over a whole session. DXVK files and logs moved to the session scratchpad (dxvk-removed).

### Texture uploads in small bands (uploadsplit.c, `UploadBandKB`, 2026-10-30)

- **Local driver test (scratchpad d3dband.c, each mode in its own process; 48 textures of 1 MB,
  each updated then read by a CopyResource, 120 frames):**

  | upload | allocations | 32 MB regions | mapped | ms/frame (48 MB) |
  |---|---|---|---|---|
  | whole 1 MB | ~4,100 | 2 -> 6 | 147 MB | 4.78 |
  | 256 KB bands | 6,400-7,300 | 2 -> 6 | 145 MB | - |
  | 64 KB bands | ~670 | 2 -> 4 | 85 MB | 3.37 |
  | 32 KB bands | ~420 | 2 -> 3 | 54 MB | 4.13 |

  Same for RGBA8 512 x 512 and BC3 1024 x 1024. Small updates apparently travel in the driver's
  command stream; 256 KB is no better than whole.
- **uploadsplit.c:** the sampler patches slot 48 (UpdateSubresource, +0xC0) of the function table
  of the game's context object at exe+0x2c10584 (that table sits in the object itself), once the
  object answers QueryInterface for ID3D11DeviceContext and GetType says immediate, and again if the
  object or table changes. 2D-texture updates over the band size go as whole (block) rows, at most
  N KB per call, same data and order; buffers and others pass through, counted.
- **uploadsplittest** (real driver, byte-for-byte readback): 11 formats (plain and BC1-BC7), all mip
  levels, odd sizes, arrays, partial boxes: 1,848 subresource uploads, 0 mismatches. It caught one
  real bug first: a BC mip whose width isn't a multiple of 4 (250 x 150) lost its last block column
  when a band's box ended at the logical width; a box edge at the texture's edge is now rounded up
  to whole blocks (the stored size).
- Installed 2026-10-30 ~12:20 (sha fba46c1e) with `UploadBandKB=32` and `GpuWatch=1` for the
  allocation count; fallback msimg32.prev-20261030-defaults.dll + civprobe.ini.before-uploadsplit-20261030.
- **First run (session 20261030_121139, 17 s): the game aborted at the main menu.** The patch went in
  at 10.06 s (context 0EBDABA8, table 0EBDABAC = the object + 4; the entry pointed to
  d3d11.dll+0x1b0730), Steam initialised at ~10.2 s (CSERHelper.dll loaded), and at 17.30 s the main
  thread called ucrtbase's abort, which reached UnhandledExceptionFilter; Steam's handler ended the
  process with ExitProcess(3). No exception for the probe's handler, no Windows error record, no
  dump. The wrapper had not been called (0 uploads counted). Never seen before in any logged
  session. The game's cache databases written by the DXVK run (12:00) pass SQLite's quick_check, so
  not those. Cause unknown; the one new cross-thread action was the sampler calling QueryInterface
  and GetType on the game's immediate context. v2 (sha 2f69c895) drops those: it only checks that
  the upload entry points into d3d11.dll and swaps that pointer. The crash watcher now also treats
  an exit through CSERHelper as a crash (the probe still logs "process exiting" for it).
- **v2 aborted the same way (session 20261030_121552):** patched at 10.06 s, "Microsoft Visual C++
  Runtime Library: Runtime Error! This application has requested the Runtime to terminate it in an
  unusual way" (the user's screenshot), ExitProcess(3) via CSERHelper at 15.5 s, 0 uploads through
  the wrapper. So writing that table entry is what does it: something checks the table (the copy
  protection is a suspect). The watcher restored the defaults-on build.
- **v3 (sha fbca8c10): hot-patch D3D11 instead.** The table is only read, to find D3D11's
  UpdateSubresource (d3d11.dll+0x1b0730 here: `8b ff` mov edi,edi at the entry, int3 padding
  before). The padding gets `jmp wrapper`, the entry a 2-byte `jmp $-5`, and the wrapper calls
  entry + 2. uploadsplittest pass 2 does exactly that to the real d3d11.dll and calls
  UpdateSubresource the normal way: 1,848 uploads, 0 mismatches (pass 1, wrapper called directly:
  also 0).
- **v3 (hot-patch) aborted too (session 20261030_121936):** hot-patched at 10.06 s, the same "Runtime
  Error! ... terminate it in an unusual way" dialog, this time with MSVCR90.dll+0x238b3 on the stack
  (the abort comes from inside the game's CRT, as from terminate() after an uncaught C++ exception),
  0 uploads through the wrapper, only 238 MB committed at 1 min (the main menu wasn't loaded yet).
  So writing the table isn't the cause either. The three aborting runs all had GpuWatch=1 together
  with LuaHeapMimalloc=1; the good runs had one or the other, never both. Next run: UploadBandKB=32
  with GpuWatch=0 (sha e8375002), and the probe now logs the last C++ exception's type at exit.
- **v3 with GpuWatch=0 aborted too (session 20261030_122248, 33 s).** The new exit line named the
  last C++ exception: type `.J` (long), the way code throws a failed HRESULT. Every launch since the
  DXVK run (12:00) has aborted, and that run rewrote GraphicsSettingsDX11.ini: `FullScreen = 0` and
  `HDStrategicView = 0`, where both backups (2026-10-27, -28) and the user's normal play have 1. A
  window at 2560 x 1440 gives a ~2554 x 1405 client area (DXVK's log shows that extent), which a
  block-compressed render resource can't have: a likely E_INVALIDARG thrown as long at startup.
  Restored both to 1 (the DXVK-era file kept as GraphicsSettingsDX11.ini.after-dxvk-20261030);
  next launch with UploadBandKB=32, GpuWatch=0 (sha e8375002).
- **Confirmed (session 20261030_122543):** with FullScreen and HDStrategicView back at 1, the same
  build (hot-patch at 10.08 s, UploadBandKB=32) reached the main menu; map grid gave back its reserve
  at 46 s. The four aborts were the DXVK run's windowed-mode setting, not the banding. Lesson: an
  experiment that crashes the game can leave the game's own settings changed; check
  GraphicsSettingsDX11.ini / config.ini against backups after one.
- **In game (session 20261030_122543, 11 min, clean): it works, and it doesn't help.** 378 texture
  updates split into 5,292 bands (165 MB), 1,009 small texture updates, **27,295 buffer updates**
  (whole buffer, no box), 21 other. Driver regions: 11 at 10 min, 206 MB mapped, 146 MB holes, the
  same as without it (sessions 093651, 100718: 10-11). Texture uploads are a small share; whole-buffer
  UpdateSubresource (~41/s, from exe+0x4c1373) dominates and lines up with the ~30 allocations/s.
- **Local buffer test (scratchpad bufband.c, 64 in-use DEFAULT buffers of 16-256 KB, 120 frames):**
  whole 192 allocations, 0.51 ms/frame, no new region, 28 MB mapped; 64 KB / 32 KB pieces 239 / 224;
  8 KB 136 (1.8 ms); 4 KB 113 (3.0 ms, 15 MB mapped). The driver already pools in-use buffer uploads;
  the game's pattern (about one allocation per buffer update) isn't reproduced, so its buffers must
  differ (size? creation flags?), unmeasured. UploadBandKB set back to 0 in the game ini.
- Last C++ exception at a normal exit: `_com_error` (thrown and caught somewhere during the session;
  harmless).

### D3D11 statistics (d3dstats.c, `D3DStats=1`, diagnostic, 2026-10-30)

Hot-patches (same technique as uploadsplit) d3d11's Map and UpdateSubresource, found through the
game's context table (read only), and at the first upload, from the main thread, the device's
CreateBuffer and CreateTexture2D (via GetDevice). Counts buffer updates by size (whole / boxed),
usage+bind and call site, distinct buffers updated, Map by type (WRITE_DISCARD by size, kind, site),
buffers created by size, initial data, kind, site. d3dstatstest checks it on the real d3d11.dll
(10 whole 256 KB updates + 1 boxed, 5 discard maps of 3 MB, 1 buffer created: all counted right).
Installed 2026-10-30 ~13:25 (sha 66ba90a2) with D3DStats=1, GpuWatch=1, UploadBandKB=0.
- **First statistics run (session 20261030_124139, 3.7 min incl. the load, clean):** UpdateSubresource
  9,106 whole buffers (0.8 GB; 5,653 vertex buffers from exe+0x4c1373, 3,450 index buffers from
  exe+0x4c1432; 281 distinct buffers; sizes mostly <=64 KB by count, 0.5 GB in the 256 KB-4 MB ones).
  **Map(WRITE_DISCARD) on buffers 443,353 times, 83 GB of buffer size:** exe+0x4c1319 (the upload
  flush, DYNAMIC vertex buffers) 34,149 maps / 71 GB (~2.1 MB a buffer); exe+0x4c4e3e (DYNAMIC
  shader-resource buffers, several copies per map) 27,556 / 6.9 GB; exe+0x4c13d8 (DYNAMIC index
  buffers) 15,143 / 4 GB; exe+0x4cfd8d (constant buffers) 350,621 / 105 MB. Every discard gets a new
  copy of the whole buffer from the driver's CPU-visible pool, the memory in the 32 MB regions. The big
  dynamic vertex buffers were created before the device was patched (few, created early). Created
  after: 7,549 buffers (5,105 with initial data; mostly IMMUTABLE), 3,572 Texture2D. GpuWatch: 14,805
  CreateAllocation, 9 regions at exit.
- **Next:** how much the flush actually writes per discard (its size at [esi - 4] of the list entry,
  read through a stub that saves the caller's esi; sha 16c6ee6f). If it's a small part of the
  buffers, those buffers could live in GPU memory with only the written bytes uploaded.
- **Bytes written (session 20261030_124828, 5 min, clean):** the vertex flush (exe+0x4c1319) made
  102,208 discard maps writing 32 GB into 200 GB of buffer copies (16%); one 3 MB buffer alone got
  16,081 maps; writes mostly 64 KB - 4 MB. The index flush (exe+0x4c13d8): 57,316 maps, 24% written,
  four buffers (0.9 / 0.19 / 0.12 / 0.02 MB). Shader-resource buffers (exe+0x4c4e3e) 132,703 maps /
  33 GB; constant buffers 1.28 M maps / 481 MB. (The distinct-buffer list for the vertex flush filled
  with tiny buffers mapped once; its 3 MB one is the one that matters.)
- **Would GPU-only buffers help? No (scratchpad discard.c).** A 3 MB buffer, 6 writes a frame of
  16 KB - 1.5 MB, read by the GPU after each, 300 frames: the game's way (DYNAMIC + Map DISCARD)
  15 allocations, regions 2 -> 3, 41 MB mapped, 0.18 ms a frame; DEFAULT + UpdateSubresource of the
  written bytes 305 allocations, 2 -> 6, 130 MB, 0.52 ms. The driver cycles a small fixed set of
  copies for repeated discards of one buffer; uploads go through its pool and cost three times more.

### Conclusion on the GPU driver regions (2026-10-30)

Everything tried on the driver's 32 MB regions either didn't help or made things worse: Trim (frees
no address space: the regions hold live resources), banding texture uploads (works, no effect in
game: buffers dominate), small buffer pieces (only at 4-8 KB, at 3-6x the CPU), DXVK (+670 MB at
startup in a 32-bit process), GPU-only buffers (worse). The driver memory is the cost of the game's
rendering volume: 1.6 M discard maps in 5 minutes. The regions jump during the load (2 -> 8-10)
and then add ~1-2 per 20 minutes. Not pursued further. uploadsplit.c was removed from the tree
(source kept in the session scratchpad); d3dstats.c stays as a diagnostic. The game ini is back to
GpuWatch=0 with no D3DStats or UploadBandKB.

## Grey tiles after scrolling far (2026-10-30, terrain page-in and base texture pool)

The user saw grey tiles filling in top to bottom after a fast scroll to a far part of the map,
worst during AI turns. Two limits, both in the game's terrain tile manager (DX11 exe, static
analysis plus live reads with `cdbX86 -pv` of session 20261030_135935).

### The page-in rate

- `GraphicsDetailSettings` keys `TerrainPageinSpeedStill` / `TerrainPageinSpeedMoving` are read
  into the ints at static 0xd90f64 / 0xd90f60 (registered at exe+0x1d8ea0).
- The tile manager's update (exe+0x3ba820, called from exe+0x11b096 each frame) takes a
  still/moving flag; `MinimizeGrayTiles` (0xd90f20, 1 in the user's ini) forces "still". Still =
  up to `Still` tiles gathered and at most 16 built a frame; moving = `Moving` for both. Tiles
  are taken in list order, hence the top-to-bottom fill. A slow AI-turn frame rate shows directly
  as slow fill.
- The settings code (exe+0x53f80, run from exe+0x1d9406 after the ini is read) clamps both to 8.
  The quality presets (exe+0x1d84b1 jump table) use 1/0, 4/1 and 6/3. The user's ini had 2 / 1.
- Changed the user's `GraphicsSettingsDX11.ini` to 8 / 8 (backup
  `GraphicsSettingsDX11.ini.before-pagein-20261030`). In play: fills noticeably faster; all
  command streams stayed within capacity (10-minute report).

### The base texture pool: why the grey appears at all

- Every tile needs two base textures (tile +0x198 / +0x19c, BC1 at the minimum terrain resolution
  0xd90f30 = 256 with 6 mips, ~43 KB) before it is drawn. They come from a pool made once at
  terrain init (exe+0x2b8370 on the static terrain object exe+0x9165e0, called from
  exe+0x76967): count at +0x62a04, handles at +0x6d888, handed to the tile manager's free list
  (+0x21e8c, a growable vector, exe+0x23bcb0; allocator exe+0x1c60b0). When the list is empty,
  tiles off screen give theirs back (exe+0x175030).
- The count is not an ini key: on DX11 hardware the settings code sets it from
  `TerrainDetailLevel` (0xd90f38: 6,144 at level 3, 4,096 at 1-2), and at levels 1-3 it also
  overwrites the ini's `Terrain64/128/256/512Chunks` (level 3: 16 / 0 / 256 / 64). Those four
  pools hold the higher-detail 256/512 composites (129 tiles had one), not the base.
- Live, Huge Communitu map: **6,790 tiles, 3,088 with base textures, 0 free**. So at most ~45%
  of the map can be textured at once, and a scroll to a part not seen recently always shows grey
  until the page-in rebuilds it.
- The handle array in the object is 8,096 entries (zero past the 6,144th; the next field is at
  +0x75708), the same size as the 64-texel array. Only the pool init (exe+0x2b8656) reads the
  count global. So 8,096 (4,048 tiles, 60%) is the most the game's structure allows without
  moving the array; covering all 6,790 tiles would need 13,580.
- Tried: `TerrainBaseSlots` (terrain.c, a probe thread keeping the count global at N before
  the pool is made; data only). It worked mechanically (session 20261030_141618: pool made at
  32.7 s with 8,096 slots, the settings code had set 6,144 at 8.9 s) but **broke textures**: UI
  frames garbled at the main menu, magenta boxes on every map icon. Cause, read live: the game's
  texture handle table (static object exe+0x3d3e2c0, allocator exe+0x4c5cb0) is a fixed 0x3000 =
  12,288 handles (next-handle counter at +0x40, free list at +0x32898); the counter was at
  12,287 with no free handles, so every texture made after that got handle 0. The game's own
  use already sits close to that limit, and 1,952 more base textures pushed it over. So the base
  pool can't be raised by a useful amount; terrain.c was removed (source in the session
  scratchpad). The game folder is back on build 538332db and the previous ini
  (`TerrainBaseSlots` gone; the glitching DLL kept as `msimg32.terrain-glitch-20261030.dll`).
- Conclusion: with this map size some grey after long scrolls is the game's design limit (two
  hard caps: 8,096 base slots in the terrain object, 12,288 texture handles). What helps is the
  page-in rate (8 / 8).
- Tried: the rate past the settings' cap. The update (exe+0x3ba820) gathers up to `Still`
  candidates from priority-sorted lists (qsort, comparator exe+0x1e7010: state +0x314, +0x310,
  +0x1bc, then float +0x1a4 descending) and renders at most 16 (immediate at exe+0x3babe0). The
  globals are read every frame, so `TerrainPageinTiles` (pagein.c) rewrote them to 16 from the
  sampler thread. **In play (session 20261030_142716) tiles stopped filling for good**, even
  with the camera still. Live: 3,371 base slots free but only 1,396 of 6,790 tiles textured, 628
  tiles stuck with +0x314 = 5. Cause, a game bug the higher rate triggers: the update marks a
  tile 5 and adds it to the render list (manager + 0x1b00) before the build (exe+0x324600)
  allocates its mesh buffers (+0x318 / +0x31c) from the temp heap (`Maximum Temp Heap Size` 68
  MB). When an allocation fails, the build frees what it got and returns without queueing the
  job or clearing the 5, and the gather skips tiles in state 5, so they stay grey until a
  reload. More builds in flight per frame ran the temp heap out. Reverted (game folder back on
  538332db, ini backup restored; the 16 build kept as `msimg32.pagein16-stuck-20261030.dll`);
  pagein.c removed (scratchpad copy). The same bug can in principle strand tiles at 8 when the
  temp heap is busy.
- Left: a bigger temp heap (address space, which this game is short of), or un-sticking tiles
  whose build never started (game state written from another thread; not attempted).

## Porting to the DirectX 9 and tablet exes (2026-10-30)

The three exes share a PE date (2014-11-19) and, apart from the renderer, the same code at other
addresses: `CivilizationV_DX11.exe` (timestamp 0x546cd0a8, SizeOfImage 0x6519000),
`CivilizationV_Tablet.exe` (0x546cd5f8, 0x651b000) and `CivilizationV.exe` (DX9, 0x546ccb59,
0x5a74000). All are unpacked and keep their relocations, which made a relocation-aware search
possible.

- **portmap.py** decodes the DX11 instructions at an address (and before it) with `llvm-objdump`,
  masks relocated dwords and branch displacements, and searches the other `.text`; it widens the
  window until one match is left, and requires the forward and backward matches to agree. Data
  addresses are voted on by up to 12 referencing instructions. Tablet: 52 of 59 places found
  directly; DX9: 41. The rest were tiny functions found many times (`mov ecx, cs; jmp wrapper`),
  places that differ, or DX9's different command stream; found by hand (the Lua lock pair and the
  TeamMet / data-dirty registrations are unique byte patterns in all three exes).
- **Tablet:** identical to DX11 except the record allocator: `add ecx,0FFh / and ecx,0FF00h` on
  the size, so records take 256-byte steps (the 1 MB data half holds fewer records than in DX11).
- **DX9 command stream:** the object is 0x1058 bytes (DX11 0x2080), the constructor allocates
  `a1 * 0x20` commands, `a2 * 0x90` groups and the data once, and the same factory/creation site
  (exe+0x1f1a3a..0x1f1a79, 0x1000 / 0x400 / 1 MB) makes the streams. The counter sites are the
  same code, except the third command appender (no 1024-dword list: `inc [ecx+0Ch] / pop edi /
  pop esi / ret 8`). Allocation goes through the same `malloc` / `_aligned_malloc` imports via the
  same helper depth, so the constructor's return addresses and the stack offsets of the wrappers
  carry over. No flip: a 24-byte reset (exe+0x4eca40, 11 callers) zeroes +0xc, +4, +0x40, +0x44,
  +0x48, +0x30, +0x54. Whether DX9 overflows like DX11 hasn't been seen in play yet; the stats
  lines will tell.
- **Same in all three (checked):** the event queue (sizes 0x800180 / 0x400080, base +0xc80, lock
  +0x1001000), the core-to-UI queue processing and its handler tables, the map grid reserve, the
  Lua allocator, the game-core hand-over, the small-block allocator (exports compared
  instruction by instruction), the Lua lock wrappers and the game's own NULL write at start.
- **First sessions (2026-10-30, the Alexander Huge VP save, turn ~428):** every part started in
  all three exes. DX11 (180725) and tablet (181848) played normally; the busy stream overflowed as
  before (DX11 5268/4096 commands, tablet 5086), held by the spare, nothing dropped. The tablet's
  256-byte rounding overflowed a 32 KB stream (47,100 records to the pool, no visible effect).
  DX9 (181439, 182738): the busy stream overflowed too (4740/4096, held), but the map and units
  weren't drawn, only icons smearing over an uncleared screen (the loading picture still behind
  them). Memory: private commit 2.8-3.2 GB (DX11 2.6), lowest free 69-131 MB; the extra is the
  process heap, 430-950 MB against ~125 MB in DX11 and tablet (D3D9's system-memory copies of
  managed resources, most likely). `CommandStreamSplit=0` (183447): the same picture, the stream
  stayed under capacity (772/4096). `Enabled=0` (the game as shipped): the same picture, then VP's
  crash box "32-bit address space nearly exhausted" (222 MB free, largest block 152 MB,
  CivilizationV.exe+0x13fd32). That conclusion (memory) was wrong as the cause of the picture:
  small maps without mods showed the same with the fix (sessions 184551, 184759, 185009; lowest
  free 870-2106 MB) and without it (`Enabled=0`). The D3DX9 runtime (d3dx9_42.dll) is installed.
  So the game's DX9 mode doesn't render on this PC (Windows 11, NVIDIA), fix or not; forum reports
  describe the same with no known cure. The DX9 port could only be checked for starting, finding
  its code and not crashing (the busy stream did overflow there, 4740/4096, held by the spare);
  rendering with it remains unseen. Player advice in TROUBLESHOOTING ("DirectX 9 shows only icons").

## The "d3d11.dll" 117 MB is the fluid fog of war (2026-11-01)

The reservation log of session 20261101_191043 (`AllocTracking=1`, so mimalloc off) grouped
117 MB in 8 regions under `d3d11.dll+0x2b145`, as if the D3D11 runtime held 16-24 MB blocks.
It doesn't. With the public d3d11.pdb those frames are `NOutermost::CDevice::CreateLayeredChild`
(+0x255, +0xc9, +0x275) and `CCreateBufferValidator::Validate`, i.e. `CreateBuffer`, but in the
full rows they sit between ntdll's heap frames and `MSVCR90 malloc -> probe -> exe+0x42c0d5`
(the game's malloc wrapper). The reservation log's stacks are a stack scan, and these are stale
return addresses left on the load thread's stack by the `CreateBuffer` calls just before; the
same three frames turn up in frees made from the game's own code. The real callers (all at
30.1 s, all from the fluid fog setup at exe+0x4b1c8-0x4b256, all confirmed by the
`allocsites_*_latest.csv` of that session):

| Size | Caller | Source (strings at the call) |
|---|---|---|
| 4 x 24 MB | exe+0x28f79a | `FluidParticles.cpp` l.623, one per job thread (the row in "Who owns the 1.6 GB CRT heap") |
| 16 MB | exe+0x4cc6cb, from exe+0x4b1f9 | `FTextureData.cpp` l.2151: the CPU copy of `flowfield.dds` (object made at `FluidFOW.cpp` l.290, kept at fog object +0x1340 and handed to the particle init), size = sum over mips of w x h x 16 B |
| 2 x 0.75 MB | exe+0x28f808 / +0x28f82e | `FluidParticles.cpp` l.633 |

So the D3D11 runtime owns nothing big here, and the "d3d11 114 MB" lead counted the 96 MB of
per-thread fluid buffers a second time. The flow field data is read by the particle jobs on the
CPU, so it is needed while fluid fog is on. No new lever: the fluid fog's size still only
follows `MaxSimultaneousThreads` (24 MB per job thread) and `FluidFOW` (which breaks rendering).
Lesson for the reservation log: take the owner from the frame after the allocator entry
(`MSVCR90`/probe malloc wrapper), not the first non-system frame, when both appear.
