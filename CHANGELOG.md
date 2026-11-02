# Changelog

## Unreleased

- **SQLite heap on mimalloc** (experimental, off by default; `DbHeapMimalloc=1`). The game
  database's SQLite memory lives in a Windows heap that keeps its load-time peak: 94 MB held for
  about 14 MB live after loading a large mod set. The option sends that heap's allocate, reallocate,
  free and size calls to mimalloc, as `LuaHeapMimalloc` does for Lua, after checking the database
  DLL's SQLite memory methods byte for byte. In one test on a late Huge save: 74 MB more free at
  10 minutes, no change in loading time. `luaheaptest` now drives both heaps and checks the
  installed database DLL when the game is present.

## 1.2.1

### What's new

To update, close the game and copy the new `msimg32.dll` over the old one.

- **New name: Civ V Crash Patch.** Civ V Crash Fix is now Civ V Crash Patch, at
  [github.com/PureXYZ/civ5-crash-patch](https://github.com/PureXYZ/civ5-crash-patch); links to the
  old address still work. The fixes and settings are the same as 1.2.0. Only the name changed: in
  the events log, in the DLL's file details and in the zip's name. The repository also starts from
  a fresh history, so earlier releases are no longer listed; their changes are below.

## 1.2.0

### What's new

To update, close the game and copy the new `msimg32.dll` over the old one.

- **Every fix now works in tablet mode and DirectX 9 too.** Until now the crash fixes for busy
  turns and big saves, the memory savings and the faster AI turns only worked in DirectX 11.
  DirectX 11 is still the mode to play in: on some current PCs the game's DirectX 9 mode doesn't
  draw the map, with or without the fix
  ([details](https://github.com/PureXYZ/civ5-crash-patch/blob/main/docs/TROUBLESHOOTING.md#directx-9-shows-only-icons-smeared-across-the-map)).

- **Shorter AI turns, just as smooth.** The extra screen-update passes are paced less
  (`UiQueueShare` 25 -> 50). With the redraw merges from 1.1.0 on, this was no less smooth while
  panning, and AI turns were about 3 s shorter in testing.

### Details

- **DirectX 9 and tablet exes.** The parts that change exact places in the game now look up the
  running exe in a table (`probe/exes.c`) of the final Steam DX11, DirectX 9 and tablet exes. The
  addresses were found with the new `probe/portmap.py`, and the test harness checks every part
  against each exe file (`CivProbe_CheckExe`). Differences handled: the tablet's record allocator
  rounds records up to 256 bytes; the DirectX 9 command stream is single-buffered (spare room at
  the end of each array instead of spare halves, counters bounded by what was allocated, stats from
  the per-frame reset). The events log names the exe build at start (`exe: the final Steam ...`).
  `streamtest` covers the tablet rounding and the DirectX 9 layout. Played in DX11 and tablet mode;
  DirectX 9 starts with every part on and its busy-frame overflow held, but on the test PC the
  game's DirectX 9 mode doesn't draw the map at all, with or without the fix.
- The map grid reserve now checks its free function through the vectors' own reserve code, and the
  spare-caller defaults come from the table; a custom `CommandStreamSpareCallers` still works.
- **`UiQueueShare` default 25 -> 50.** Live A/B in a late-game Huge VP save, one setting per AI
  turn, three turns each, while panning (AI turn mean / time in frames over 33 ms): no extra
  passes 46.6 s / 18.4%, share 10 46.1 s / 19.6%, 25 40.0 s / 20.3%, 50 36.8 s / 19.5%. A second
  run, unpaced against 50: 28.7 s / 29.2% against 34.7 s / 22.8%. Before the merges, 50 had
  cost clearly more stutter than 25 (~23% against ~17%).
- `GameCoreIntervalMs` stays 10: 20, 40 and 100 ms gave more time in long frames (27%, 32%,
  37% against 25%) and no shorter AI turns.

## 1.1.0

### What's new

To update, close the game and copy the new `msimg32.dll` over the old one. Everything below is
on by default; you don't need the settings file.

- **No more crash right after loading a big save.** A late-game Huge save could overflow the
  game's event list while loading and crash as the load finished. Those events now wait their
  turn instead.
- **More free memory.** The game's script memory now gives space back, and the room the game sets
  aside for the largest possible map is returned: about 100 MB more free memory on a Huge map,
  more on smaller ones.
- **Shorter late-game AI turns.** Most of an AI turn was the game waiting on itself. In testing,
  turns went from about 65-70 s to about 45-50 s.
- **Smoother panning during AI turns.** Panels that redraw on every AI change (and the Global
  Relations mod) now redraw at most a few times a second, and the memory cleanup no longer pauses
  the screen.
- **The crash on quitting is fixed in two more forms.**
- **Better crash reports.** The log now records freezes, "Microsoft Visual C++ Runtime Library"
  errors, and, on the next launch, what Windows recorded about a crash. If you report a crash,
  start the game once more first, then send the log.

### Details

- **Audit fixes.**
  - The Lua heap on mimalloc kept freed pages mapped only while `MiMalloc=1`; with `MiMalloc=0` or
    `AllocTracking=1` it ran on mimalloc's default 10 ms decommit, which a scripted read of a
    deleted object has crashed on. mimalloc's options are now set either way.
  - Event queue spill: each half has its own spill buffer, so posts that overflow the next half
    while a spill is delivered wait for their own pass instead of running ahead of that half's
    records; delivered records are cleared, so a record reserved but not yet written by another
    thread can't be mistaken for an old one (the delivery waits up to 20 ms for it).
    `msgqueuetest` no longer fails now and then (its stand-in pass stopped at a record a worker
    thread hadn't written yet).
  - Map grid reserve: the wait at the main menu is 20 s of wall time (it was 10 sampler ticks:
    2.5 s with `IntervalMs=250`), and the vectors are emptied with compare-exchanges, capacity
    first, so a map load that starts at that moment reserves anew instead of using a freed block.
  - The exception handler no longer writes the allocation report (`AllocTracking=1` with
    `Telemetry=1`): it opens files, which can't be done there. The VA map's label is copied first,
    so a second exception can't rename it, and a mapped file whose name doesn't fit is left
    unnamed instead of unterminated.
  - Session order (the previous launch's crash check, `KeepSessions` pruning, `analyze.py`) goes
    by file time, not the local-time stamp in the name, which sorts wrong after the clocks go
    back. A launch in the same second as the previous one takes the next second instead of
    overwriting its logs, and a launch while another instance runs no longer reports that one as
    ended.
  - Merged `TeamMet` records that still can't run 10 s after their time (a save loading) are
    dropped, so they can't reach the next game's listeners with the last game's teams.
  - Skipped Miles getters (`AIL_sample_volume_pan` and three more) write zeros to their outputs
    instead of leaving the game's stack values.
  - Command stream: an overflow pool unused for 10 s can serve another stream, and the last-resort
    slot is a 64 KB buffer of its own (it was the half's last 16 KB, too small for a large record).
  - Quit-crash guard: an unref deferred while its Lua state was being closed is dropped instead of
    queued, so it can't reach a new state at the same address.
  - Diagnostics: `GpuWatch=1` could overrun a stack buffer with long module names and logged every
    lock as a first map once 1,024 regions were known; `D3DStats=1` shared one saved register
    between threads and counted no buffer creation when `UpdateSubresource` couldn't be patched.
    `AllocTracking=1` no longer hangs the exit when a thread was killed holding its lock.
  - Scripts: `analyze.py` works without a samples file (`Telemetry=0`), and the scripts skip a last
    CSV row cut short; `resdiff.py` labels reserve+commit calls as such.
  - CI runs `msgqueuetest` and `luaheaptest` too, and a tag fails the build when the release notes
    or the player READMEs don't name its version.

- **Fewer hitches while panning in AI turns** (on by default; [Settings](docs/SETTINGS.md#fewer-hitches-while-panning-in-ai-turns-on-by-default)).
  Timing the screen's handlers for each kind of AI update showed where the long frames go in a
  late-game Huge save: `TeamMet` 100 s in 10 minutes (~300 ms a run, only the Global Relations
  mod listens), `HexFOWStateChanged` 22 s (50,000 short runs), `SerialEventGameDataDirty` 21 s
  (~43 ms a run). `UiDirtyCoalesceMs` and `UiTeamMetCoalesceMs` run those two events' listeners
  at most every N ms, merging the records in between; the last one always runs. Measured live:
  data-dirty at 250 ms took the 95th-percentile AI-turn frame from 60 to 41 ms; TeamMet at 2 s
  cut its ~300 ms runs ~80% in AI turns, and the default 30 s leaves at most about two a minute
  (Global Relations redraws its screen when opened anyway). A mod that needs every `TeamMet`
  would see only the last of a merged burst: `UiTeamMetCoalesceMs=0` for that. `UiQueueStats=1`
  (diagnostic) logs the handler times.
- **Fix: the extra queue passes stopped for whole sessions.** The pacing's time bucket had no
  floor, so loading a save (or the game in the background) put it so far in debt that most
  sessions ran 0 extra passes, and AI turns were as slow as without `UiQueuePasses`. The debt is
  now capped at 5 frame targets (100 ms).

- **`D3DStats=1`** (diagnostic): counts the game's Direct3D 11 buffer uploads, maps and resource
  creation by size, kind and calling code. `GpuWatch=1` now also samples the stacks of the driver's
  allocations. The events log names the last C++ exception thrown when the game exits, which
  identifies aborts that end in the C runtime's "terminate it in an unusual way" dialog.
- **Lua heap on mimalloc** (on by default; `LuaHeapMimalloc=0` turns it off). The game keeps every
  Lua object in a Windows heap of its own that never gives space back; late in a Huge game it held
  ~190 MB of address space for ~110 MB of live Lua. The probe now puts those objects in mimalloc,
  which returns empty space to Windows: in game, that heap went from 193 to 20 MB (~33 MB net at
  10 minutes, growing over a session). `luaheaptest` checks the wrappers (4 threads, 1.2 M calls).
- **Load crash fix: the game's event queue overflow** (`MsgQueueGuard=1`, on by default). Events for
  the main thread go through a queue whose halves hold 4 MB each, with no check. A Huge late-game
  save posts more than that on the main thread while it loads (5.07 MB, 6,585 posts past the end,
  measured), so the extra records overwrite others and the main thread then calls a handler for a
  garbage record: a crash at `exe+0x2a59a3` (`call eax` with eax = 0) right after the load, or, by
  luck, nothing. Posts that don't fit now go to a spill buffer, and their handlers run right after
  the game's next pass over the queue, in order. Logged as `msgqueue:`.
- **GPU driver memory in the logs.** The D3D driver gets CPU-visible buffers mapped into the
  game in 32 MB regions that the kernel creates and never gives back, and in a late-game session
  they were the only thing still growing after 30 minutes (+96 MB from 30 to 50 min). The events
  log now has a `gpu chunk:` line each time a new one appears, with the time, and a `gpu chunks:`
  line (count, mapped, only reserved) with each 10-minute report and at exit; the samples CSV has
  three new columns for them.
- **Smaller map detail grids** (on by default; `MapGridReserve=0` turns it off;
  [Settings](docs/SETTINGS.md#memory-savings-on-by-default)). At startup the game reserves two map
  detail grids for the largest map it supports (152 x 96): 178 MB in two blocks, whatever map you
  play. The probe gives that reserve back at the main menu, so loading a map reserves only what
  that map needs: about 99 MB more free address space on a Huge Communitu map (measured in game:
  108,640 of 243,200 tiles), more on smaller maps. Each report logs the
  grids' size (`mapgrid:`).
- **`GpuWatch=1`** (diagnostic): names the game code that makes the D3D driver add a 32 MB region,
  by hooking the two Windows calls that map driver buffers (in `win32u.dll`, not the game).
- **Reservation log:** regions reserved at an address their caller chose are now logged too, with
  `0x40000000` added to the type.

- **Faster AI turns** (on by default; [Settings](docs/SETTINGS.md#faster-ai-turns-on-by-default)). In a
  late-game Huge save with Vox Populi, AI turns went from a ~64-68 s median to 30-35 s with all of
  these plus VP's `UI_QUICK_ANIMATIONS` option, or ~44-58 s (against 63-73 s) with the pacing
  that keeps panning smooth (`UiQueueShare`, below). Measured by sampling the game's queue of AI
  updates to the screen at ~600 Hz: the game runs one AI player per update and hands over the
  next only once the screen has taken in the last one, so ~73% of an AI turn was waiting on the
  screen side, not AI work.
  - `SmallBlockFallback=1`: once the game's small-object allocator has filled its 32 MB region,
    every small allocation scanned ~1,300 chunk entries for space it couldn't find and fell back
    to malloc anyway, 27% of the AI's CPU time. The probe now switches that allocator off through
    the game's own `FSBADisable` export once the region has been full for 30 s; blocks already in
    it are still freed by address. AI CPU time per turn: 21-30 s → 14-18 s.
  - `UiQueuePasses=4`: the screen takes in at most one update per unit per frame,
    so units' moves trickled in one frame each (~20 s per AI turn). The probe now runs the game's
    own queue pass up to 4 times per frame, before each frame, from the run loop's
    `PeekMessageW` import. `4`: ~20 s → ~8 s of waiting per AI turn; no visual problems seen.
  - `UiQueueShare=25`, `UiQueueFrameMs=20`: unpaced, the extra passes made panning the map stutter
    during AI turns. The updates they take in run the screen's handlers (mod Lua, VP's
    `TechPanel.lua` most), and frames with extra passes ran over 33 ms 18% of the time, back to
    back: ~29% of AI-turn time was in such frames (~17% without extra passes). Now, after any
    frame over 20 ms, the extra passes wait until 25% of the time since has paid it back. Measured
    live in the same save: ~17.5% of AI-turn time in long frames, AI turns ~50 s (unpaced 39-46 s,
    no extra passes 63-73 s).
  - `GameCoreIntervalMs` (below) now defaults to 10; measured on its own: ~3 s per AI turn.
  - Each setting goes back to the game's behaviour with `SmallBlockFallback=0`, `UiQueuePasses=1`
    or `GameCoreIntervalMs=0`, and each leaves another exe version alone.

- **Quit crash, a gap in the guard.** A Lua reference released while the game closes a Lua state
  (by a C++ object's destructor that a `__gc` finalizer runs) marked the closing state as good
  again, and the next release after the close went through to the freed state: the
  `lua51_Win32.dll+0x3b05` quit crash. A state is now marked closed before `lua_close` runs, the
  releases made during the close are skipped, and its queued ones are dropped with it. Found in a
  code review; the guard test reproduces it (the old guard crashed at `+0x3b05`).
- **The Lua cleanup can't switch itself off.** A Lua error in a collection (from a `__gc`
  metamethod, or out of memory) jumped out of it and left the cleanup thinking a collection was
  still running, so it never ran again in that session. Collections now run inside `lua_cpcall`;
  an error is logged and skipped, and the finalizers left over from it are run.
- **Crash and freeze logging is safer.**
  - The freeze watchdog reads the frozen thread's Lua stack while that thread is suspended, maybe
    in the middle of changing it. A read that faults now gives up on the Lua stack
    (`freeze: reading the Lua stack ... faulted`) instead of crashing the game.
  - With `Telemetry=1`, an exception no longer opens a file from the exception handler (opening
    one takes the process heap's lock, which the crashing thread may hold). The next exception's VA
    map file is opened ahead of time.
  - A log line longer than 1 KB no longer puts a NUL byte in the events log, which could hide its
    `process exiting` line from the next launch's check.
  - `KeepSessions=1` deleted the previous launch's log before the next launch could check how it
    ended; the previous launch is now always kept.
  - At exit the probe no longer waits for a lock that the sampler thread may have held when Windows
    ended it.
  - After 32 distinct exception sites, one line says that later new ones aren't logged.
  - The game's own handled NULL write at `CivilizationV_DX11.exe+0x523dcb`, which happens at every
    start, no longer takes the `exception1` VA map, so that file now means a real exception.
  - `cmdstream: ... filled a half` is no longer logged for the streams of a few entries that are
    full every frame by design.
- **Smaller fixes.**
  - Out-of-range settings are clamped instead of wrapping around: `IntervalMs` (at most 60 s),
    `HangSeconds` and `HangRepeatSeconds` (an hour), `LuaGcStepMB` (2048), `MiMaxKB` (1 GB).
  - The audio guard falls back to rejecting only implausible handles if its tables of live samples
    or streams fill up, instead of silencing the sounds that didn't fit.
  - The quit-crash guard forgets its oldest closed Lua state when its table is full, instead of
    remembering none after 16.
  - The reservation log (`ReserveLog=1`) no longer loses a ring slot for good after a slow writer.
  - Allocation tracking (`AllocTracking=1`) counts large blocks past 2 GB correctly.
- **Build:** CI also runs the mimalloc redirect test, verifies the LLVM-MinGW download's SHA-256,
  uses the Node 24 versions of the GitHub actions, and gives write access only to the release job.

- **Experimental: faster AI turns** (`GameCoreIntervalMs`; at first off by default, now on, see
  above). The game runs AI turns
  on its game-core thread but hands that thread a new update only every 100 ms of frame time (a
  constant in the exe). In a late-game Huge save the core ran 8-20 ms and then sat paused for
  100-200 ms: paused 91% of each AI turn, which took ~40 s. `GameCoreIntervalMs=10` hands it an
  update every 10 ms instead. No game code is changed: a probe thread sets the main loop's
  frame-time accumulator (`exe+0x8cf91c`) to the 0.1 s mark every N ms, so the loop's own check
  passes. (A first version patched the call site instead, and Steam's CEG wrapper made the game
  quit by itself 30 s into each launch.) The code around the accumulator is checked first;
  another exe version is left alone. Result in that save: the core then worked 16-17% of each AI
  turn instead of 8%, but turns weren't shorter (median 47 s against 40 s). The next limit is the
  UI: the main loop also holds the core back while the core-to-UI event queue isn't empty, and
  that waiting took 54 s of 103 s of AI turns. So it was left off by default. (Later measured
  per AI turn instead of by turn length, which varies ±15 s: it saves ~3 s per turn, and it was
  turned on; see above.)
- **Exits are logged.** `PostQuitMessage`, `exit`, `_exit`, `ExitProcess` and `TerminateProcess`
  calls log their caller and the stack (`exit:` lines), so a launch that quits by itself shows
  which code decided to.
- **No more stutter from the Lua cleanup.** The Lua GC fix ran a full collection of the game's
  ~100 MB Lua heap at once, which held the UI for 30-80 ms each time (18 times in 10 minutes of
  late-game play, more in busy screens). It now spreads the same collection over the following
  database calls, at most 1 ms of work in each 10 ms (`LuaGcSliceUs=1000`; 0 brings back the
  full collection). In a test at the game's busiest pace the longest pause fell from 63-125 ms to
  ~3 ms with the same memory peak. If the database allocates another `LuaGcStepMB` before a
  spread-out collection is done, the rest is done at once as before.
- **Freezes in Lua name the script.** When the frozen UI thread is inside a Lua database call
  (the long freezes seen so far were: a 116 s one in a late-game save), the freeze lines are
  followed by that Lua state's call stack, one `lua:` line per frame with the file, line and
  function name (`lua:   <file>.lua:<line> in global '<function>'`). That
  points at the mod script to fix. Read while the thread is suspended, without allocating.
- **The quit-crash guard costs less.** It checked the main Lua state's memory with VirtualQuery on
  each of the ~2,900 reference releases a second, about 4% of the UI thread. The main state is now
  checked once and remembered until it's closed, which halves those calls.
- **Quit crash, another form.** Quitting could still crash at `lua51_Win32.dll+0x3b05`: on its way
  out the game releases a Lua reference through a Lua thread (coroutine) that the garbage
  collector had already freed, and writes to that thread's freed stack. The quit-crash guard now
  releases registry references through the Lua state's main thread, which the registry is shared
  with and which is never collected. Seen with mimalloc on (1.0.0), which hands empty memory back
  to Windows, so the stale write faults where the unmodified game usually got away with it.
- **Lua references are released under the game's Lua lock.** The game releases UI callbacks
  without taking its own Lua lock, while the game-core thread runs GameEvents on the main Lua
  thread under it. With the change above, both could use the main thread's stack at once (a
  GameEvent crashed at `lua51_Win32.dll+0x1b072` on a damaged table). The guard now does each
  registry release under that lock; when another thread holds it, the release waits in a queue
  instead of blocking. The events log counts both cases.
- **Better crash reports.** The events log lists every DLL loaded from outside the Windows folder
  (base, size, PE timestamp) at start and as they load or unload, so a crash offset in a mod's
  DLL can be matched to its build. Each logged exception is followed by a `memory (at
  exception)` line with the free address space at that moment. Paths under the user profile
  are written as `%USERPROFILE%`.
- **Stack in the events log.** Each logged exception gets a `stack:` line: up to 24 return
  address candidates (values on the stack that point just after a call in a loaded module's
  code), so a crash without a minidump still shows how it got there. Stale values in locals
  pass too, so read it innermost first. Module names come from a table kept as DLLs load, not
  from the loader, whose lock another thread may hold at the crash.
- **Stack overflows are logged.** 1.0.0 ran out of stack in its own handler and logged nothing.
  The overflowing thread now hands the logging to a thread that waits for it, and the `stack:`
  line shows the recursion once, with its count, then its caller.
- **C runtime exits are logged** (`CrtFailureLog=1`). The "Microsoft Visual C++ Runtime Library"
  kind of crash never raised an exception the log could see: `abort()`, `terminate()`, pure
  virtual calls (R6025), and the stack-cookie (`/GS`, `0xC0000409`) and invalid-parameter
  (`0xC0000417`) reports. Each now gets a `crt:` line with its caller, the stack and free
  memory; for `/GS`, the function whose cookie was overwritten. An uncaught C++ exception is
  named by its type.
- **The next launch fills in what the log couldn't see.** When the previous launch's events log
  doesn't end with `process exiting`, the next launch looks up Windows' record of it in the
  Application event log and appends it to that log, e.g. `Windows recorded the crash (Application
  Error): exception 0xc000041d in mss32.dll+0xac07`. This covers fast-fail crashes, which no
  handler sees, and freezes that were closed (Application Hang). `LastRunCheck=1`.
- **Freezes are logged.** When the game window hasn't responded for 30 s (`HangSeconds`), the log
  gets where its thread is stuck: its position, the stack (copied while the thread is briefly
  suspended, scanned after) and free memory, again every minute while it lasts, and how long it
  took when it responds again. Long loads that stop the window show up too, with their end.
- The crash report form and READMEs ask players to start the game once more before reporting,
  so the previous-launch lookup has run.
- The crash report form asks for the events log of the game that crashed (not just the newest),
  the Vox Populi / Community Patch version, and the `crashlogs` folder's `crashes.log` and
  `CvMiniDump_….dmp`.

## 1.0.0

The first public release.

- **Render command stream fix:** spare halves for the stream that overflows on busy frames, and
  bounds on its record allocator and all 8 counter increments. This fixes the crashes in the
  Miles audio engine, the corrupted `FAudioGame` vtable and the writes past a 2 MB block.
- **Audio guard:** Miles sample calls only accept live handles.
- **Quit-crash guard:** `luaL_unref` on a closed Lua state is skipped.
- **mimalloc for small allocations:** blocks up to 16 KB from the game's CRT go to mimalloc
  2.5.2 instead of the fragmenting NT heap. In a comparison on the same Huge-map save, free memory after 40 minutes was 346 MB with it and 166 MB without
  (under 200 MB at 34 minutes).
- **Lua GC fix:** a full Lua collection every 64 MB of database memory used from Lua. This caps
  the Huge-map game-creation spike.
- Quiet by default: only the events log is written, and only the newest 20 launches are kept.
  The diagnostics are opt-in (`Telemetry=1`, `AllocTracking=1` and others).
- A version resource in the DLL (Properties → Details).
- The events log reports free address space (below and above 2 GB, largest free block, lowest so
  far) at 1 minute, every 10 minutes and at exit, and warns once below 200 MB free. VA maps label
  mimalloc's segments.
