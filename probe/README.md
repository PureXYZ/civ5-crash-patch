# Developer guide

`probe/` is the source of [Civ V Crash Patch](../README.md). Internally it's called **civprobe**: it
started as memory telemetry for Civilization V (32-bit), to see what fills the game's address
space over a play session and what it looks like at the moment of a crash. The crash fixes it
found are built in and on by default; the measurement tools are still there, off by default.

Related pages: [How it works](../docs/HOW-IT-WORKS.md) (what each fix changes) ·
[Settings](../docs/SETTINGS.md) (every `civprobe.ini` key) ·
[Research notes](../docs/RESEARCH-NOTES.md) (the lab notebook).

- [Build](#build)
- [Tests](#tests)
- [Source map](#source-map)
- [Trying a build in the game](#trying-a-build-in-the-game)
- [Output files](#output-files)
- [Diagnostic modes](#diagnostic-modes)
- [The Lua GC fix in detail](#the-lua-gc-fix-in-detail)
- [Caveats](#caveats)
- [Releasing](#releasing)

## Build

Needs [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`) with its `bin` on `PATH`, and a POSIX shell
(Git Bash works). From the repository root:

```bash
sh probe/build.sh
```

This writes `probe/build/msimg32.dll` (the fix), `build/version.dll` (see
[why it isn't used](#why-msimg32-and-not-versiondll)) and the test programs.

Build without `-s`: Defender's ML heuristic quarantined a stripped build (false positive
`Contebrew.A!ml`). Exit code `0xC0000906` from a test means an antivirus blocked it.

## Tests

Most tests are named `CivilizationV_*.exe` so the probe activates inside them. They load
`build/msimg32.dll` and read `build/civprobe/civprobe.ini`. Run them from `probe/build`.

**Self-checking, run in CI** (exit code 0 = pass; see
[`build.yml`](../.github/workflows/build.yml) for the exact ini):

| Test | Checks |
|---|---|
| `streamtest` | The command stream's record, counter and constructor stubs. Needs `CommandStreamTestRets` set to anything. |
| `audiotest` | The audio guard, with the fake `mss32.dll`. Needs `CommandStreamTestRets` too. |
| `sparetest` | The spare halves (writes its own ini). |
| `mitest` | The mimalloc redirect, with `mitest_dll.dll`; msvcr90 and msvcp90 are bound by its manifest. |
| `stacktest` | Crashes three children of itself and checks their `stack:` lines (with `stacktest_dll.dll`). |
| `crttest` | Ends five children through each C runtime exit path (with `crttest_dll.dll`). |
| `lastruntest` | Crashes a child and checks that the next launch appends Windows' record (or the "no event" line where error reporting is off) and leaves normal logs alone. About 75 s. |
| `hangtest` | A child that stops taking window messages for 15 s; with the game's `lua51_Win32.dll` in `build/`, also one frozen inside a Lua database call (its Lua stack must be logged) and one whose Lua state reads as corrupt (must not crash). |
| `msgqueuetest` | The event-queue spill against a stand-in queue, posting from three threads. |
| `uiqueuetest` | Handler thunks and merged records against a stand-in exe image. |
| `luaheaptest` | The Lua heap wrappers, 4 threads. |
| `texhandlestest` | The game's texture handle pool code, grown to 16,384 and to 32,768 handles, in the installed DX11 exe mapped as an image: every handle, release and reuse across the old limit, the record getters. Skipped without the game. |
| `terraintest` | `TerrainBaseSlots`' three changed instructions in the installed DX11 exe mapped as an image, its two stubs, and the top-up through the hooked `qsort` on a stand-in terrain object and tile manager. Skipped without the game. |

The last five link their module directly and need no ini.

**Self-checking, not in CI:**

- `d3dstatstest` needs a hardware D3D11 device.
- `guardtest` needs a copy of the game's `lua51_Win32.dll` in `build/`. Among other cases: an
  unref from a `__gc` finalizer during `lua_close`, then one after it, which the old guard let
  through to `lua51+0x3b05`.
- `luagctest`: see [the Lua GC fix in detail](#the-lua-gc-fix-in-detail).
- `slacktest`: `CommandStreamTestRets` = two return addresses (see `slacktest.c`).
- `milabel`: `MiMalloc=1`, `Telemetry=1`; after 65 s the `t60s` VA map must label its 4 MB
  segments `mimalloc`.
- `watchtest`: `Watch=1`, `WatchGlobalExport=g_watched`.

**Compared by hand** against the printed expectation, all with `AllocTracking=1` and
`AllocCrt=ucrtbase.dll`: `alloctest` (known patterns), `allocstress` (8 threads; prints the true
live total), `peaktest`, `restest`.

**Harness.** Run `CivilizationV_probetest.exe` with the game exes as arguments. For each it
checks the Lua counter scan, then maps the exe file and runs every part's check of the code and
data it relies on against that exe's table in `exes.c` (`check ...: ok=1 DX11: cmdstream=ok ...`).
Run it on all three exes after changing an address or a check:

```bash
build/CivilizationV_probetest.exe "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\CivilizationV_DX11.exe" "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\CivilizationV.exe" "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\CivilizationV_Tablet.exe"
```

## Source map

| Source | Part | Default |
|---|---|---|
| `probe.c`, `probe.h` | Proxy exports, sampler, events log, telemetry, VA maps, exception log | events log on, `Telemetry=0` |
| `exes.c` | The addresses each part uses in the DX11, DX9 and tablet exes (found with `portmap.py`) | |
| **Crash fixes** | | |
| `cmdstream.c` | Render command stream fix | on |
| `audioguard.c` | Miles sample-handle guard | on |
| `msgqueue.c` | The game's event queue overflow while loading | on (`MsgQueueGuard=1`) |
| `luaguard.c` | Quit-crash guard (`luaL_unref` on a closed state or a collected Lua thread) | on |
| **Memory** | | |
| `mialloc.c` | mimalloc for the small CRT allocations | on (`MiMaxKB=16`) |
| `luagc.c` | Lua GC for database query results | on (`LuaGcStepMB=64`) |
| `luaheap.c` | The game's Lua and SQLite heaps on mimalloc; both Windows heaps made with nothing up front | on (`LuaHeapMimalloc=1`, `DbHeapMimalloc=1`) |
| `mapgrid.c` | Gives back the map detail grids' startup reserve before a map loads | on (`MapGridReserve=1`) |
| `intro.c` | The intro movie's video player (and the Direct3D 9 driver it loads) isn't started; 1 = only with the game's Skip intro video on | on (`SkipIntroPlayer=2`) |
| **Graphics** | | |
| `texhandles.c` | Room for more than the game's 12,288 textures | on (`TextureHandles=16384`) |
| `terrain.c` | Each map gets the terrain base textures it needs, so a big map stays textured | on (`TerrainBaseSlots=22528`) |
| **AI turns** | | |
| `coretick.c` | How often the game core gets an update | on (`GameCoreIntervalMs=10`) |
| `smallblock.c` | The exe's small-block allocator off once its 32 MB region is full | on (`SmallBlockFallback=1`) |
| `uiqueue.c` | Extra, paced passes over the core-to-UI queue; merged data-dirty and `TeamMet` records | on (`UiQueuePasses=4`) |
| **Crash and freeze reports** | | |
| `crtfail.c` | Logs the C runtime's exits (`/GS`, invalid parameter, `abort`, `terminate`, `_purecall`, uncaught C++ exceptions) | on |
| `hang.c` | Freeze watchdog: the stuck window thread's position and stack, and its Lua stack inside a Lua DB call | on (`HangSeconds=30`) |
| `lastrun.c` | After a launch that didn't end normally, appends Windows' Application Error / Hang record to its log | on |
| `modules.c` | Non-system DLLs in the events log (base, size, timestamp) at start and as they load; the stack scan for logged exceptions | on |
| **Diagnostics** | | |
| `alloctrack.c` | CRT allocation attribution | off |
| `topdown.c` | Top-down placement, reservation log | off |
| `watch.c` | Page-protection watchpoint | off |
| `gpuwatch.c` | Which game code maps the GPU driver's memory regions | off |
| `d3dstats.c` | Counts the game's D3D11 uploads, maps and buffer creation | off |
| **Other** | | |
| `version.h`, `version.rc` | Release version and the DLL's version resource | |
| `third_party/mimalloc/` | mimalloc 2.5.2 (`include/`, `src/`, `LICENSE` only) | |
| `*test*.c`, `harness.c`, `fakemss*`, `*.def` | Tests, stand-ins and export definitions | |
| `*.py` | [Analysis scripts](#analysis-scripts) | |

### Why msimg32 and not version.dll

The game exe imports `msimg32.dll`, so a copy in the game folder gets loaded first; it forwards
the 5 real exports to `C:\Windows\SysWOW64\msimg32.dll`. Inside any exe whose name doesn't start
with `CivilizationV`, it stays dormant.

A `version.dll` build exists too, but **it does not work in Civ V**. On 2026-10-27 the running
game had the system `version.dll` mapped, not the proxy, because something loads the system copy
before the game's own imports are resolved. The prime suspect is the compatibility shim
`AcGenral.dll` (applied because the exe has "Override high DPI scaling" set), which imports
version.dll; that wasn't reproduced in the harness. msimg32 is not imported by the shim or by any
KnownDLL, and it loads.

## Trying a build in the game

- **Install:** copy `build/msimg32.dll` into
  `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\`.
- **While the game runs:** rename the loaded `msimg32.dll` first, then copy the new one in. The
  running game is unaffected and the next launch picks up the new build.
- **Uninstall:** delete `msimg32.dll` from the game folder. Steam's "verify integrity" ignores
  extra files, so it won't remove it.
- **Settings:** `<game folder>\civprobe\civprobe.ini`; every key is in
  [Settings](../docs/SETTINGS.md).

Hard-won rules for changes that touch the game:

- **Never patch main-loop or game-logic code bytes.** Steam's CEG checksums them; a 6-byte patch in
  the main loop made the game quit itself ~30 s into every launch. Prefer data writes, import-table
  hooks or calling the exe's own functions. The render command stream patches are fine.
- **Never cache module or code ranges.** The game unloads DLLs (GPU driver probing at startup), and
  a stale range crashed the game at launch. Use `VirtualQuery` each time.
- **No hardware breakpoints.** Setting debug registers on the game's threads makes it exit ~11 s
  later (Steam DRM). Use page protection instead (`Watch=1`).
- **Don't walk the game's heaps on a timer.** On the ~1.9 GB CRT heap a walk stalled for minutes
  under the heap lock and froze the game.
- **After an experiment crashes, compare the game's own ini files with a backup.** A crashed
  experiment once left `GraphicsSettingsDX11.ini` changed, which caused unrelated aborts later.

## Output files

Written to `<game folder>\civprobe\`, one set of files per game launch. The newest
`KeepSessions` (20) launches are kept. Only the events log is written unless `Telemetry=1`:

| File | Contents |
|---|---|
| `events_<session>.log` | Exe identity, non-system DLLs (base, size, PE timestamp; loads and unloads), where the Lua counters were found, what each fix did, memory headroom lines, threshold triggers, and crash-type exceptions (code, module+offset, target address, up to 24 return-address candidates from the stack, free address space at that moment) |
| `samples_<session>.csv` | One row every 2 s: commit/reserve/free/largest free block **below and above 2 GB**; image/mapped/private totals; Lua bytes and peak (the exe's own counters); Lua heap, CRT heap (msvcr90, used by the exe, the VP DLL and the database DLL), process heap and other heaps; GPU driver regions |
| `vamap_<session>_<label>.csv` | Full address-space map: every region with its owner (module or mapped file name, or which heap; growth segments are found through their segment header) |

VA maps are taken 60 s after launch; every 10 minutes; the first time the low 2 GB has under 64 MB
free; the first time its largest free block drops under 16 MB, and again under 4 MB; on the first
6 access-violation / heap-corruption exceptions; and at exit. The exception path makes no heap
calls.

### Analysis scripts

Run them with `python -I` and `C:/...` paths (Git Bash `/c/...` paths don't work with Windows
Python).

| Script | What it does |
|---|---|
| `analyze.py "<game>\civprobe"` | Summarises the newest session: samples, VA maps, allocation reports grouped by module. |
| `peakdiff.py "<game>\civprobe"` | Diffs the newest session's `peak` allocation report against its `latest` (or `exit`) report: what was live at the peak and freed afterwards. |
| `resdiff.py <reserves csv> [--from S] [--to S] [--at S] [--frames N]` | Regions reserved in a time window that are still held, grouped by the first non-system frames. |
| `resspikes.py <reserves csv> [--min-kb N] [--max-s S] [--from S] [--frames N] [--placed]` | Regions released again within a few seconds, for brief free-space dips (`--placed`: only those reserved at a chosen address). |
| `portmap.py <DX11 exe> <other exe> [--objdump PATH] [--only NAMES] [--extra RVAS]` | Finds the DX11 addresses of `exes.c` in another build of the exe: code by the DX11 instructions around it (relocated dwords and branch displacements masked, decoded with LLVM-MinGW's `llvm-objdump`), data by the instructions that reference it. Prints how each was found; a few need checking by hand (see `exes.c`). |
| `cegall.py <exe> [0xRVA[:len] ...] [--list]` | The code ranges Steam's CEG checksums (the `mov eax, fn; call eax; ret` checks), and whether the given places are inside one. Run it on any new code change: one inside exe+0x2d00f0's ranges crashed map loads. |

## Diagnostic modes

All off by default; the keys are listed in [Settings](../docs/SETTINGS.md#diagnostics-off-by-default).

### Allocation tracking (`AllocTracking=1`)

Hooks the shared C runtime's allocator, `msvcr90.dll` (`malloc`/`calloc`/`realloc`/`_recalloc`/
`free`, the aligned variants, and `new`/`delete`), so the 1.6 GB CRT heap can be attributed to
callers.

- **How:** it patches the import tables of the exe, the VP game core DLL (including each reload),
  the database DLL, the localization DLL, Lua and msvcp90.
- **Exact vs sampled:** blocks ≥ 4 KB are tracked exactly; smaller blocks are estimated from a
  1-in-64 address sample.
- **Stacks:** each tracked allocation records its caller plus up to 4 more return addresses, found
  by scanning the stack. Release builds don't keep frame pointers, so deeper frames can
  occasionally be stale.
- **Cost:** the bookkeeping uses about 22 MB, allocated top-down so it lands in the upper 2 GB.
  Every malloc/free does a little more work; turns get noticeably slower.

Reports, every 30 s (`_latest`), at every snapshot, and at exit:

| File | Contents |
|---|---|
| `allocsites_<session>_<label>.csv` | Estimated live MB per stack (frame0 = immediate caller) |
| `alloccalls_<session>_<label>.csv` | Cumulative MB and call count per immediate call site (churn) |

**Peak report.** A short spike, such as creating a game, is freed again within a minute, so the
rolling `latest` report misses it. Each time the CRT heap's live bytes rise `PeakReportStepMB` (32)
above the last peak, the sampler logs `CRT heap peak:` and rewrites the `_peak` reports, which end
up holding the attribution at the session's highest point. `peakdiff.py` shows what the spike was
made of. Test: `CivilizationV_peaktest.exe` attributes its 320 MB spike to the one allocating call.

**Testing with UCRT instead of msvcr90:** put a `civprobe.ini` with `AllocCrt=ucrtbase.dll` in
`<test dir>\civprobe\` and run `alloctest` or `allocstress` next to `msimg32.dll`. On 2026-10-27
both matched within 1%.

A measurement setup, for example:

```ini
[probe]
Telemetry=1
IntervalMs=2000
PeriodicSnapshotMinutes=10
AllocTracking=1
AllocCrt=msvcr90.dll
AllocBigBytes=4096
AllocSampleShift=6        ; sample 1 in 2^6 small blocks
AllocReportSeconds=30
```

### Top-down placement (`TopDown=1`)

Tests whether the game breaks when pointers are at or above 0x80000000.

- **What it does:** every new reservation, heap segment and non-image mapped view in the game asks
  for the highest free address (`MEM_TOP_DOWN`). It's the per-process equivalent of the
  system-wide `AllocationPreference=0x100000` registry value, with no system change.
  `TopDownViews=0` leaves mapped files and shared memory at their normal place.
- **How:** `topdown.c` patches the WOW64 ntdll stubs of `NtAllocateVirtualMemory`,
  `NtAllocateVirtualMemoryEx` and `NtMapViewOfSection`. Each stub is copied to a trampoline, and its
  first instruction becomes a `jmp` via one atomic 8-byte write. DLL images are untouched. An
  unexpected stub shape is logged and left unhooked.
- **Expected result:** the samples CSV shows the *high* 2 GB filling first and the low 2 GB staying
  mostly free. The events log has `topdown:` lines with redirect counts at every periodic snapshot,
  at each new exception and at exit.
- **Reading the result:** an early crash (menu, load, first turns) means high addresses trigger it,
  and the logged exception site is where to look. A long normal session rules high pointers out.
  (In the game it played normally.)

### Reservation log and heap histogram (`ReserveLog=1`, `HeapHistogram=1`)

- **`ReserveLog=1`** hooks `NtAllocateVirtualMemory(Ex)` and `NtFreeVirtualMemory` (the same stub
  hooks as top-down mode, without changing placement). Every new region of at least
  `ReserveLogMinKB` (1024) goes to `reserves_<session>.csv` with up to 16 stack frames, and so does
  its later release. New heap segments show up too, with the malloc caller that made the heap grow.
  The hooks only copy addresses into a ring buffer; the sampler thread names the modules each tick.
  The reports log the counts (`reserve log: N reservations, M releases; D dropped`). Summarise with
  `resdiff.py` and `resspikes.py`.
- **`HeapHistogram=1`** walks the CRT heap at the 60 s and 10-minute snapshots and logs busy and
  free bytes by block size (`heaphist` lines), plus the heap's front end (2 = LFH). The walk holds
  the heap lock, so the game pauses briefly each time.
- **Test:** `CivilizationV_restest.exe` (5 s run), or `CivilizationV_restest.exe 64` with
  `AllocTracking=1`, `AllocCrt=ucrtbase.dll` and `HeapHistogram=1` to check the histogram.

## The Lua GC fix in detail

Background: [How it works](../docs/HOW-IT-WORKS.md#huge-map-memory-spike-lua-database-lookups) and
the research notes, "The Huge-map spike". Each Lua database query is a small Lua userdata that owns
KBs of C++ memory in CvGameDatabase, which Lua's collector doesn't see.

- **How:** `luagc.c` patches CvGameDatabase's import of `lua_pushcclosure`, so every Lua C function
  it registers goes through a 10-byte thunk. When the function returns and the trigger says
  `LuaGcStepMB` has accumulated, a collection starts.
- **Trigger, with `AllocTracking=0` (normal play):** a light counter. Only the DB DLL's and
  MSVCP90's msvcr90 allocation imports are patched, and they count bytes only while the thread is
  inside a wrapped call: the memory that waits on Lua's GC. Set `LuaGcCrt` in tests.
- **Trigger, with `AllocTracking=1`:** growth of the CRT live estimate.
- **Incremental (`LuaGcSliceUs=1000`, the default):** a full collection of the game's ~100 MB Lua
  heap holds the UI 30-80 ms. Instead, once due, the collection is driven with
  `lua_gc(L, LUA_GCSTEP, 0)` from the following wrapped calls, at most `LuaGcSliceUs` of work per
  10 ms of wall time, until the cycle has passed its atomic step and swept (then Lua's own steps run
  the finalizers). It falls back to one full collection if another `LuaGcStepMB` is allocated first.
  `LuaGcSliceUs=0` = full collections only.
- **Guards:** no collection inside a running one (the DB's `__gc` finalizers are wrapped too), and
  none while Lua is running finalizers. Collections and steps run inside `lua_cpcall`: a Lua error
  from a `__gc` metamethod or out of memory is popped and logged
  (`luagc: a Lua error during a collection`), and the finalize phase it interrupted is finished, so
  collections go on. The Lua 5.1 `global_State` layout is checked against `lua_getallocf` before
  `gcstate` is read; if it doesn't match, collections stay off.
- **Log:** `luagc:` lines in the events log: one per collection (CRT live before → after, Lua heap,
  ms), and totals at each periodic snapshot and at exit.

**Test:** `build/CivilizationV_luagctest.exe "<game>\lua51_Win32.dll" 20000`, with
`AllocCrt=ucrtbase.dll` and `LuaGcModule=luagc_testdb.dll`: 20,000 userdata of 32 KB each, on a
60 MB Lua heap.

- The C memory peaked at **625 MB with the fix off and 64 MB with it on** (9 collections, ~21 ms
  each). The light trigger gives the same result and counts exactly the 625 MB.
- A third argument paces the calls (`... 8000 900`: ~0.9 ms of Lua work between calls, about the
  game's busiest 34 MB/s). Paced, the incremental mode's longest pause is ~3 ms against 63-125 ms
  for full collections, with the same 12 MB peak. Unpaced (far faster than the game), it falls back
  to full collections and peaks at 128 MB.
- A fourth argument N makes every Nth `__gc` raise a Lua error (`... 20000 0 50`, with
  `LuaGcCrt=ucrtbase.dll`): the collections must go on, with the same peaks (64 MB full, 128 MB
  incremental); the test fails if the peak reaches half of all allocated memory.

## Caveats

- First-chance exceptions are logged even when the game handles them, so an `exception` entry
  doesn't by itself mean the game crashed. Each faulting address is logged once, with hit counts at
  exit, and snapshots are kept for the first 6 distinct addresses. The game deliberately writes to
  NULL at `CivilizationV_DX11.exe+0x523dcb` at every start and handles it: ignore that one (it gets
  no VA map).
- `HeapStats=1` holds each heap's lock every interval; on the game's big CRT heap that can freeze
  it. Keep it off in play.

## Releasing

1. Bump `CIVFIX_VERSION_STR` and `CIVFIX_VERSION_NUM` in `version.h`.
2. Rename the `## Unreleased` section of [CHANGELOG.md](../CHANGELOG.md) to `## <version>`; it
   becomes the release notes.
3. Update the version in the player READMEs: `CivVCrashPatch-<version>.zip` (and the version in the
   "Check that it's working" line) in [README.md](../README.md), and the first line of
   [dist/README.txt](../dist/README.txt).
4. Push a tag `v<version>`. The [workflow](../.github/workflows/build.yml) builds, tests, checks
   steps 1-3 and publishes the release with the zip and checksums.
