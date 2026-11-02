# Settings

**Most players don't need any of this.** With no settings file, every fix is on and only a small
events log is written. This page is for turning a part off, tuning it, or switching on the
diagnostics used to find the bugs.

- [Using a settings file](#using-a-settings-file)
- [Common changes](#common-changes)
- [The crash fixes](#the-crash-fixes-on-by-default)
- [Memory savings](#memory-savings-on-by-default)
- [Faster AI turns](#faster-ai-turns-on-by-default)
- [Fewer hitches while panning in AI turns](#fewer-hitches-while-panning-in-ai-turns-on-by-default)
- [More textures](#more-textures-on-by-default)
- [General](#general)
- [Diagnostics](#diagnostics-off-by-default)

## Using a settings file

1. Start the game once with the fix installed, so the `civprobe` folder appears in the game folder.
2. Create a text file named `civprobe.ini` in that folder. The release zip has a commented example
   in `optional\civprobe.ini` that you can copy and edit.
3. Put the settings you want to change under a `[probe]` line, one per line. `1` means on and `0`
   off.
4. Restart the game. Settings are read only when the game starts.

```ini
[probe]
Telemetry=1
```

Any setting you leave out keeps its default. Delete the file to go back to all defaults.

Parts that change exact places in the game work in the three exes of the final Steam version
(1.0.3.279): `CivilizationV_DX11.exe`, `CivilizationV.exe` (DirectX 9) and
`CivilizationV_Tablet.exe`. They check the game's code before changing anything, and in another
exe they log why and stay off. `D3DStats` needs a Direct3D 11 exe (DX11 or tablet).

## Common changes

| I want to… | Setting |
|---|---|
| Turn the whole fix off without uninstalling it | `Enabled=0` |
| Get shorter AI turns, accepting more stutter while panning during them | `UiQueueShare=0` (no pacing) |
| Undo the faster AI turns because something looks out of date | `UiQueuePasses=1` and `UiDirtyCoalesceMs=0` |
| Let a mod see every meeting of two civilizations | `UiTeamMetCoalesceMs=0` |
| Keep every log file instead of the newest 20 launches | `KeepSessions=0` |
| Write detailed memory logs for a bug report | `Telemetry=1` |
| Stop the log from recording freezes | `HangSeconds=0` |
| Give back the address space the terrain textures take on big maps (they turn grey again when you scroll far) | `TerrainBaseSlots=0` |
| Give back the 29 MB the bigger texture table takes (also turns the terrain textures off) | `TextureHandles=0` |

---

## The crash fixes (on by default)

| Key | Default | What it does |
|---|---|---|
| `CommandStreamSplit` | 1 | Spare room for the render command stream that overflows in busy frames. |
| `CommandStreamCap` | 1 | Stops every writer of that stream at the end of its array. |
| `MsgQueueGuard` | 1 | Fixes the event queue overflow that crashes the game right after a big save loads. 0 = log only. |
| `AudioGuard` | 1 | The sound engine only accepts sound handles it really handed out. |
| `LuaUnrefGuard` | 1 | Fixes the crash when you quit. |
| `LuaGcStepMB` | 64 | Cleans up memory left behind by Lua database lookups every N MB (at most 2048). 0 = off. |
| `MiMalloc` | 1 | Small allocations go to the mimalloc memory manager, so memory doesn't run out in long sessions. 0 = the game's own. |

### Render command stream (fine-tuning)

The fix and its root cause are described in [How it works](HOW-IT-WORKS.md#the-main-crash-render-command-stream-overflow).
These keys only matter for testing.

| Key | Default | What it does |
|---|---|---|
| `CommandStreamSpareCallers` | `0x1c9940,0x1c9962,0x1c9969` | Which stream constructor calls get the spare room (RVAs of the constructor's return addresses). |
| `CommandStreamCmdSpare` | 3 | Extra command capacity per half, in multiples of the game's (max 15). |
| `CommandStreamGroupSpare` | 1 | The same for groups. |
| `CommandStreamSlackKB` | 4 | Extra bytes after every command-stream buffer. |
| `CommandStreamStats` | 1 | Logs each stream's peak use at each report and at exit. |
| `CommandStreamMB` | 0 | A rejected experiment that enlarged the stream in place. It breaks terrain rendering: leave it at 0. |

### Event queue (`MsgQueueGuard`)

Events posted for the game's main thread go into a queue whose two halves hold 4 MB each, and
nothing checks that limit. A Huge late-game save posts more than that on the main thread while it
loads; the extra records overwrite others, and the load crashes on a record with no handler (or, by
luck, doesn't). With the guard on, posts that don't fit go to a spill buffer (8 MB for each half,
allocated the first time it's needed), and their handlers run right after the game's next pass
over the queue, in the order they were posted. The events log (`msgqueue:` lines) records the
queue's high-water mark when it passes 4 MB and what was spilled. `MsgQueueGuard=0` keeps only the
logging.

### Audio guard and quit-crash guard

| Key | Default | What it does |
|---|---|---|
| `AudioGuardTrack` | 1 | Track live sound handles. 0 = only reject obviously invalid ones. |

`LuaUnrefGuard` works around an existing bug in the game's shutdown: it skips the game's release of
a Lua reference on a Lua state that is already closed, and runs each release on the main Lua state
(never collected) under the game's own Lua lock, instead of on a Lua thread that may already have
been freed. Details: [How it works](HOW-IT-WORKS.md#quit-crash).

### Lua cleanup (`LuaGcStepMB`)

Every Lua database lookup (by the game's UI and by mods) holds some memory that Lua's garbage
collector can't see, so dead lookups piled up to 420 MB while a Huge game was being created. The
fix runs a Lua garbage collection every `LuaGcStepMB` of such memory.

| Key | Default | What it does |
|---|---|---|
| `LuaGcSliceUs` | 1000 | Spreads each collection over the following database calls, at most N microseconds of work per 10 ms, so the UI doesn't pause. 0 = one full collection at once (as in 1.0.0, and always with `AllocTracking=1`). |
| `LuaGcModule` | `CvGameDatabaseWin32Final Release.dll` | The DLL whose `lua_pushcclosure` import is wrapped. |
| `LuaGcCrt` | `msvcr90.dll` | The C runtime whose allocations count toward the trigger. |

### mimalloc (`MiMalloc`)

Serves the game's C runtime allocations (the `msvcr90` imports of the exe, the game core DLL, the
database DLL, the localization DLL, Lua and msvcp90) from [mimalloc](https://github.com/microsoft/mimalloc)
2.5.2, to stop the game's heap from slowly fragmenting. Blocks the game's heap handed out before
the switch, or larger than `MiMaxKB`, stay there; each free goes to whichever allocator owns the
block. Not used while `AllocTracking=1`.

| Key | Default | What it does |
|---|---|---|
| `MiMaxKB` | 16 | Only blocks up to this size go to mimalloc; larger ones stay on the game's heap. 0 = all sizes. |
| `MiPurgeMs` | -1 | Never give freed pages back to Windows. A mod script that read a deleted object crashed on a page that had been given back. Also applies to the Lua heap (`LuaHeapMimalloc`) when `MiMalloc=0`. |
| `MiLargePadBytes` | 256 | Slack added to the blocks that stay on the game's heap. |
| `MiArenaMB` | 0 | Let mimalloc reserve arenas of this size (more as it needs them; never released) instead of 4 MB segments it gives back when empty. |

Measured on the same Huge-map save (sessions 20261029_012631 and 20261029_021851):

| Minutes | 10 | 20 | 30 | 40 |
|---|---|---|---|---|
| Free MB, mimalloc on | 429 | 362 | 342 | 346 |
| Free MB, mimalloc off | 402 | 354 | 268 | 166 |

With mimalloc, free memory levels off after about 20 minutes. The cost is a somewhat smaller
largest free block (158 MB vs 209 MB at 30 minutes), because mimalloc reserves 4 MB segments and
only returns a segment once it is empty. With every block size on mimalloc (`MiMaxKB=0`) the
overhead was about 340 MB, which is why large blocks stay on the game's heap.

---

## Memory savings (on by default)

| Key | Default | What it does |
|---|---|---|
| `MapGridReserve` | 1 | Gives back the room the game reserves at startup for the largest possible map (178 MB), so loading a map reserves only what it needs. 0 = off. |
| `LuaHeapMimalloc` | 1 | Keeps the game's Lua objects in mimalloc instead of a Windows heap that never shrinks. 0 = off. |
| `DbHeapMimalloc` | 1 | Keeps the game database's SQLite memory in mimalloc instead of a Windows heap that keeps its load-time peak. 0 = off. |

**`MapGridReserve`.** At startup the game reserves room for two map detail grids as large as the
biggest map it supports (152 × 96), whatever map you play. Loading a map sizes them for that map
anyway. The fix gives the startup reserve back at the main menu, so the map load reserves only
what the map needs: about 99 MB less on a Huge Communitu map (97 × 66; 108,640 of 243,200 tiles
used), about 50 MB on a 128 × 80 map, and more on smaller maps. No game code is changed. The
`mapgrid:` line of each report shows the grids' size either way.

**`LuaHeapMimalloc`.** The game keeps every Lua object in a Windows heap of its own, which keeps
its peak size and its holes: late in a Huge game, ~190 MB of address space for ~110 MB of live Lua.
mimalloc gives empty space back. Only that heap's four calls (allocate, reallocate, free, size)
are redirected, and each block keeps its requested size, so the game's own Lua memory count stays
exact. In game the Windows Lua heap went from 193 to 20 MB (about 33 MB saved at 10 minutes, more
as a session goes on). The `luaheap:` line of each report shows live Lua MB.

**`DbHeapMimalloc`.** The game database gives SQLite a Windows heap of its own, which keeps its
load-time peak. With a large mod
set SQLite peaks at about 78 MB while the mods load and then uses about 14 MB, but the heap still
holds 94 MB. This option moves that heap's four calls to mimalloc the same way as
`LuaHeapMimalloc`, after checking the database DLL's code. The `dbheap:` line of each report shows
live SQLite MB. In one test on a late Huge save it gave 74 MB more free memory at 10 minutes
(SQLite peaked at 72 MB while loading and kept 12 MB live), with no change in loading time.

---

## Faster AI turns (on by default)

The game runs AI turns on its own thread, one AI player per update, and hands that thread its next
update only once the screen has caught up with the last one. In a late-game Huge save with Vox
Populi (2026-10-30), most of an AI turn was that waiting, not AI work. These settings cut the
waiting. None of them changes game code, and each checks the game code it relies on first.

| Key | Default | What it does |
|---|---|---|
| `GameCoreIntervalMs` | 10 | Hands the AI thread its next update every N ms (1-99) instead of on the game's 100 ms tick. 0 (or 100 and up) = the game's tick. |
| `SmallBlockFallback` | 1 | Switches the game's small-object allocator off once it's full, so it stops wasting AI time. 0 = off. |
| `UiQueuePasses` | 4 | Works through the AI's queued screen updates up to N times per frame (2-8). 1 = the game's once per frame. |
| `UiQueueShare` | 50 | Paces those extra passes so panning stays smooth. Higher = faster AI turns, more stutter. 0 or 100 = unpaced. |
| `UiQueueFrameMs` | 20 | The frame time the pacing aims for (at most 1000). 0 = unpaced. |

How each one works, and what it was worth in that save:

- **`GameCoreIntervalMs`.** A probe thread advances the main loop's timer, so the AI thread gets
  its update as soon as the screen has caught up. 10 ms saved about 3 seconds per AI turn. A
  longer interval doesn't make panning smoother: at 20, 40 and 100 ms there was more time in
  frames over 33 ms (27%, 32%, 37%) than at 10 (25%).
- **`SmallBlockFallback`.** Once the game's own small-object allocator has filled its 32 MB region
  (for 30 s), the fix switches it off through the game's own `FSBADisable` function, so small
  objects go straight to the normal allocator (mimalloc with `MiMalloc=1`). When that region is
  full, every small allocation scans ~1,300 entries for space it can't find: 27% of the AI's CPU
  time, ~6 s per AI turn. The cost is at most the region's 32 MB, once freed blocks in it aren't
  reused.
- **`UiQueuePasses`.** The screen takes in the AI's updates once per frame, and only one update per
  unit per frame, so a unit's moves go one frame each. Extra passes (each before a frame, under the
  game's own conditions) cut that waiting from ~20 s to ~8 s per AI turn. If units, flags or banners
  ever look out of date after AI turns, set it back to 1.
- **`UiQueueShare` and `UiQueueFrameMs`.** The updates a pass takes in run the screen's handlers
  (mod Lua included), which can make a frame take 50-100 ms; unpaced, those frames came back to
  back. After a frame longer than `UiQueueFrameMs`, the extra passes wait until `UiQueueShare`
  percent of the time since has paid the overrun back.

Measured one setting per AI turn, three turns each, while panning, with the redraw merges below on
(the default was 25 before them, when 50 stuttered clearly more):

| Setup (same save) | AI turn (mean) | Time in frames over 33 ms |
|---|---|---|
| No extra passes | 46.6 s | 18.4% |
| `UiQueueShare=10` | 46.1 s | 19.6% |
| `UiQueueShare=25` | 40.0 s | 20.3% |
| `UiQueueShare=50` (default) | 36.8 s | 19.5% |

In a second run, unpaced against 50: 28.7 s and 29.2% against 34.7 s and 22.8%.

All three together, unpaced, took AI turns from a 64-68 s median to 30-35 s in that save.

**With Vox Populi, also turn on its quick animations** (`UI_QUICK_ANIMATIONS`); see
[Troubleshooting](TROUBLESHOOTING.md#ai-turns-are-slow). The AI waits 2-3 s for each "activate"
animation it shows you: ~15 s per AI turn in that save.

---

## Fewer hitches while panning in AI turns (on by default)

The AI's updates reach the screen as records, and some record types make every Lua listener redraw
something. Late in a Huge game with Vox Populi (2026-10-30), two of them took about half the time
of the long frames:

- `TeamMet`: ~300 ms a run, ~350 runs in 10 minutes of AI turns, all from the Global Relations mod;
- `SerialEventGameDataDirty`: ~43 ms a run (the Community Patch's tech panel, EUI panels, mods).

These settings merge records of one type that come close together: one that arrives within N ms
of the end of the last run is held back, and the last one held back runs N ms after that run. The
listeners redraw from the game's current state, so they end up showing the same thing. Records are
merged only on the main thread and outside other handlers, and the late run waits for the same
conditions as the game's own pass. No game code is changed (the record handler table in the game's
data is wrapped).

| Key | Default | What it does |
|---|---|---|
| `UiDirtyCoalesceMs` | 250 | Runs `SerialEventGameDataDirty` listeners at most about every N ms (0-60000). 0 = off. |
| `UiTeamMetCoalesceMs` | 30000 | The same for `TeamMet`. 0 = off. |

- **`UiDirtyCoalesceMs=250`** merged ~25-40% of those records and took the 95th-percentile AI-turn
  frame from 60 to 41 ms, with no panel seen out of date. A single change (in your own turn, say)
  still redraws at once.
- **`UiTeamMetCoalesceMs=30000`** leaves at most about two of Global Relations' ~300 ms redraws a
  minute; that mod redraws its screen when you open it anyway. (A 2 s window cut its runs ~80%, but
  each run left was still a ~300 ms freeze.) Listeners see only the last meeting of a merged burst,
  up to 30 s late. No listener in the base game, EUI or Vox Populi needs every meeting; if a mod
  shows or records every one, set 0.

---

## More textures (on by default)

| Key | Default | What it does |
|---|---|---|
| `TextureHandles` | 16384 | How many textures the game can hold at once, at most 32768. 0 = the game's 12,288. |
| `TerrainBaseSlots` | 22528 | The most terrain base textures a map may get; each map gets two per tile that isn't deep water, plus 64. 22528 is the most and covers any map. 0 = the game's 6,144 (4,096 below the highest terrain detail). |

**`TextureHandles`.** Every texture the game makes (icons, UI atlases, fonts, terrain tiles,
leader scenes) takes one of 12,288 handles, and the game almost never gives one back: it keeps
every icon atlas it has loaded. A large mod set fills that table, sooner with GPU Texture Decode,
High Detail Strategic View and the highest terrain detail, and every texture made after that is
blank: black or magenta squares on icons and city banners, and the "Error loading font texture"
message. With 16384 there is room for 4,096 more without lowering any setting. In a test with a
big mod set on a Huge map, with all three of those on, a save loaded with 12,991 textures, which
is already past the game's limit, and every icon was right. The fix moves the game's per-texture
records (920 bytes each) to a block of their own, 14.4 MB of address space at 16384, and grows the
table in place, after checking the game's code byte for byte. The `texhandles:` line of each
report shows the handles in use, and the log says when the table passes 90% and when it is full.
DirectX 11 exe only for now. `TerrainBaseSlots` adds its slots past 6,144 to this number by itself, so
by default the table holds 32,768 (28.8 MB of records).

**`TerrainBaseSlots`.** Every land or shallow-water tile of the map needs two base textures before
it is drawn, and the game has 6,144 of them: 3,072 tiles. That is under half of a Huge map (8,297
of the 10,752 tiles of a Sapiens Huge map aren't deep water). Tiles you scroll away from keep
their textures until the game needs them elsewhere, so when you scroll far, the tiles there are
grey until the game rebuilds them, 8 per frame: about a second for a screen, longer during AI
turns. With this fix each map gets what it needs: when it has finished loading,
the fix counts its tiles that aren't deep water and makes two textures per tile plus 64, past the
game's 6,144 and up to this number (10,514 more in 0.3 s on that Huge map). A map that fits
in 6,144 gets nothing extra. Once the whole map fits, the game's own background pre-render gives
every tile its textures a few minutes after a load, and scrolling never shows grey: on that Huge
map, from 29% of the tiles to 100%, and no untextured tile came into view while scrolling across
the map. The default, 22528, is the most and covers any map. The log says what it made for each map
(`terrain: … map tiles aren't deep water; made … more base textures`), and the `terrain:` line of
each 10-minute report says how many tiles have textures.

The cost is address space: the Direct3D runtime and driver's share of each texture made, plus its
handle record. With the 10,514 made for that Huge map, 553 MB were free at 10 minutes instead of
589 MB on the same save (480 MB in an earlier test that made them during the load), so about
40-110 MB on a Huge map. The bigger texture table also takes 920 bytes per
slot of the setting past 6,144, made or not (15 MB at 22528). And each texture made takes 43 KB of
video memory. Late in a long game on a Huge map that address space can matter: watch the
`memory (10 min)` lines, and set a lower number if they drop toward the 200 MB warning (the map
then keeps only part of its tiles textured, still far more than the game's). The game's texture
pool setup itself isn't changed, because Steam's copy protection checksums that code (changing it
crashed the game while loading a save). DirectX 11 exe only; it needs `TextureHandles` on.

---

## General

| Key | Default | What it does |
|---|---|---|
| `Enabled` | 1 | 0 turns everything off. The DLL then only forwards `msimg32.dll`. |
| `KeepSessions` | 20 | How many launches' worth of log files to keep. 0 = keep everything. The previous launch is always kept, so the next one can check how it ended (`KeepSessions=1` keeps two). |
| `Telemetry` | 0 | Write detailed memory logs: the samples CSV and address-space maps ([below](#telemetry-telemetry1)). Without it, the events log still gets a `memory (…)` line at 1 minute, every 10 minutes, at each logged exception and at exit, and a warning below 200 MB free. |
| `CrtFailureLog` | 1 | Log the C runtime's ways of ending the game (`/GS` and invalid-parameter reports, `abort`, `terminate`, pure virtual calls, and the type of an uncaught C++ exception), with the caller and the stack. |
| `LastRunCheck` | 1 | When the previous launch's events log doesn't end with `process exiting`, look up what Windows recorded for it (Application Error 1000 or Application Hang 1002) and add it to that log. Catches endings no handler sees, like fast-fail. |
| `HangSeconds` | 30 | When the game window hasn't responded this long, log where its thread is (position, stack, memory, and the Lua stack when it is inside a Lua database call), and when it responds again. At least 6 (Windows calls a window hung after 5 s). 0 = off. |
| `HangRepeatSeconds` | 60 | While a freeze lasts, log the thread again this often (up to 5 times). Both at most 3600. |
| `IntervalMs` | 2000 | Sampler interval, 250 to 60000. |
| `PeriodicSnapshotMinutes` | 10 | Interval of the periodic reports and address-space maps. |

---

## Diagnostics (off by default)

These were used to find the bugs. They make the game slower or write large files, so turn them on
only to investigate something. The [developer guide](../probe/README.md#diagnostic-modes) has more
on reading their output.

| Key | What it's for |
|---|---|
| [`Telemetry=1`](#telemetry-telemetry1) | Address-space samples and maps |
| [`UiQueueStats=1`](#record-handler-times-uiqueuestats1) | Which screen updates make frames long |
| [`AllocTracking=1`](#allocation-tracking-alloctracking1) | Which code holds the game's heap memory |
| [`ReserveLog=1`](#reservation-log-reservelog1) | Who reserves address space |
| [`TopDown=1`](#address-space-placement-topdown1) | Does the game break with pointers above 2 GB |
| [`GpuWatch=1`](#gpu-mapping-watch-gpuwatch1) | Which code makes the graphics driver grow |
| [`D3DStats=1`](#d3d11-statistics-d3dstats1) | What the game asks of Direct3D 11 |
| [`HeapHistogram=1`](#heap-histogram-heaphistogram1) | Busy and free heap blocks by size |
| [`Watch=1`](#watchpoint-watch1) | Catch a write to one heap object |

### Telemetry (`Telemetry=1`)

- `samples_<session>.csv`, one row per interval: commit, reserve, free and largest free block
  below and above 2 GB; image, mapped and private totals; the exe's own Lua byte counters; the
  Lua, CRT, process and other heaps; the GPU driver's 32 MB regions (count, mapped, only reserved).
- `vamap_<session>_<label>.csv`, a full address-space map with region owners. Written 60 s after
  launch, every 10 minutes, the first time the low 2 GB gets under 64 MB free (or its largest free
  block under 16 MB and 4 MB), on the first 6 distinct crash-type exceptions (`exception1` to
  `exception6`; not the game's own handled one at `exe+0x523dcb`), and at exit. The next
  exception's file is kept open in advance, because an exception may come while the heap a new
  file needs is locked.
- `HeapStats=1` adds per-heap `HeapSummary` columns. It locks each heap every tick, which can
  freeze the game on a 1.9 GB heap, so it's off.

Summarise with `python -I probe/analyze.py "<game>\civprobe"`.

### Record handler times (`UiQueueStats=1`)

Times every handler of the AI-to-screen queue's records, by record type. At each report it logs
the frames over 33 ms and 100 ms (and how much of them was in handlers) and the 12 types with the
most time in those frames: calls, total, longest call. The record types are the game's UI event
numbers (`0x66` `SerialEventGameDataDirty`, `0x72` `HexFOWStateChanged`, `0x77` `TeamMet`,
`0x7c` `ActivePlayerTurnStart`, …).

### Allocation tracking (`AllocTracking=1`)

Hooks the C runtime allocator imports (`msvcr90.dll`) of the exe, the game core DLL, the database
DLL, the localization DLL, Lua and msvcp90, and writes live memory per call stack.

| Key | Default | What it does |
|---|---|---|
| `AllocBigBytes` | 4096 | Blocks this size and up are tracked exactly. |
| `AllocSampleShift` | 6 | Smaller blocks are sampled 1 in 2^N (1-12). |
| `AllocReportSeconds` | 30 | How often `allocsites_*` (live MB per call stack) and `alloccalls_*` (churn per call site) are written. |
| `PeakReportStepMB` | 32 | Keeps a report taken at the session's peak; `probe/peakdiff.py` diffs it against the latest. |
| `AllocCrt` | `msvcr90.dll` | The C runtime DLL to hook (tests use `ucrtbase.dll`). |

It makes turns noticeably slower, turns mimalloc off, and makes the Lua cleanup collect all at
once (`LuaGcSliceUs` needs its light trigger).

### Address-space placement (`TopDown=1`)

Every new reservation, heap segment and (with `TopDownViews=1`, the default) mapped view asks for
the highest free address. This is the per-process version of `AllocationPreference=0x100000`. It
tests whether the game breaks with pointers above 2 GB (it didn't).

### Reservation log (`ReserveLog=1`)

Logs every new region of `ReserveLogMinKB` (1024) or more, with 16 stack frames, to
`reserves_<session>.csv`, along with its release. Summarise with `probe/resdiff.py` (what is still
held) and `probe/resspikes.py` (regions released again within seconds). A region reserved at an
address its caller chose has `0x40000000` added to its type.

### GPU mapping watch (`GpuWatch=1`)

Finds the game code that makes the graphics driver add a 32 MB region (the `gpu chunk:` lines).
Hooks three Windows graphics calls in `win32u.dll` (nothing in the game is changed): the two that
give a driver buffer its CPU address (`NtGdiDdDDILock` and `NtGdiDdDDILock2`) and the one that
makes driver allocations (`NtGdiDdDDICreateAllocation`). The first time a mapping lands in a new
32 MB region, the events log gets a `gpuwatch:` line with the exe frames that led to it. Each
report also lists the callers that map the most and those that make allocations
(`gpuwatch: allocations by`; every 16th call, at most 20 stacks a second).

### D3D11 statistics (`D3DStats=1`)

Counts what the game asks of Direct3D 11: buffer uploads (`UpdateSubresource`) by size, kind and
calling code; `Map` calls by type (discard maps by size, kind and code); and the buffers and
textures it creates. For the game's two buffer flush loops it also counts the bytes written per
discard against the buffer's size. The four functions are hot-patched in `d3d11.dll` itself (the
game's own tables are only read), and each wrapper only counts and calls the original. Logged with
each report and at exit (`d3dstats:`). Add `GpuWatch=1` to see which of those calls make driver
allocations. **Direct3D 11 exes only** (DX11, tablet).

### Heap histogram (`HeapHistogram=1`)

Walks the C runtime heap at the 60 s and 10-minute snapshots and logs busy and free bytes by block
size. The game pauses for about a second each time.

### Watchpoint (`Watch=1`)

A page-protection watchpoint on a heap object. The defaults watch `FAudioGame`'s vtable.

| Key | Default | What it does |
|---|---|---|
| `WatchGlobalRva` / `WatchGlobalExport` | `0x5b54f34` / – | The pointer global that holds the object. |
| `WatchSize` | `0x46C` | The object's size. |
| `WatchOffset` / `WatchLen` | 0 / 4 | The watched field. |
| `WatchCallerRva` | `0x31942a` | The allocation's call site. |
| `WatchMode` | `page` | The only mode that works in the game: Steam's DRM ends the game when debug registers are set. |
