# How it works

For technical readers: what was wrong in the game, and what each part of the fix changes. This is
the short version of the [research notes](RESEARCH-NOTES.md), which have every session, dump and
dead end. Players don't need any of this; see the [README](../README.md).

All addresses are RVAs in the final Steam `CivilizationV_DX11.exe` (1.0.3.279, PE timestamp
2014-11-19, image base 0x400000). The same places in the DirectX 9 and tablet exes are listed in
[`exes.c`](../probe/exes.c); see [Other exes](#other-exes-directx-9-and-tablet).

- [At a glance](#at-a-glance)
- [Loading and safety checks](#loading-and-safety-checks)
- Crash fixes: [render command stream](#the-main-crash-render-command-stream-overflow),
  [audio guard](#audio-guard), [event queue](#load-crash-event-queue-overflow),
  [quit crash](#quit-crash)
- Memory: [mimalloc](#long-session-fragmentation-mimalloc),
  [Lua database lookups](#huge-map-memory-spike-lua-database-lookups),
  [Lua heap](#lua-heap-on-mimalloc), [map detail grids](#map-detail-grids)
- [Faster AI turns and fewer hitches](#faster-ai-turns-and-fewer-hitches)
- [Other exes: DirectX 9 and tablet](#other-exes-directx-9-and-tablet)
- [Known limits](#known-limits)
- [Diagnostics](#diagnostics)

## At a glance

Civ V is a 32-bit game, so it gets at most 4 GB of address space. On Huge maps with Vox Populi it
crashed in places that had nothing to do with each other: the Miles audio engine
(`AIL_sample_status`, `AIL_stop_sample` on handle `0xFFFFFFFF`), a corrupted `FAudioGame` vtable,
and writes past the end of a 2 MB block. All of them traced back to one bug, a render command
stream overflow. The other parts fix further bugs found along the way, and make memory and AI
turns go further.

| Part | What it does | Exact places | Source |
|---|---|:-:|---|
| Command stream fix | Spare halves for the overflowing stream (commands ×4, groups ×2, 2 MB data per half) and a bound on all 8 counter increments and on the record allocator. | ✔ | [`cmdstream.c`](../probe/cmdstream.c) |
| Audio guard | 28 Miles sample imports only pass handles Miles handed out and hasn't released. | | [`audioguard.c`](../probe/audioguard.c) |
| Event queue spill | Posts that don't fit the game's 4 MB event queue half go to a spill buffer, delivered in order after the game's next pass. | ✔ | [`msgqueue.c`](../probe/msgqueue.c) |
| Quit-crash guard | Skips `luaL_unref` on a closed `lua_State`; runs registry unrefs on the main thread under the game's Lua lock. | | [`luaguard.c`](../probe/luaguard.c) |
| mimalloc | Allocations up to 16 KB from the exe, game core, database and Lua go to mimalloc 2.5.2 instead of the fragmenting CRT heap. | | [`mialloc.c`](../probe/mialloc.c) |
| Lua GC fix | A Lua collection every 64 MB of database allocations made from Lua, spread over the following calls. | | [`luagc.c`](../probe/luagc.c) |
| Lua heap on mimalloc | The game's own Lua heap, which never shrinks, goes to mimalloc. | ✔ | [`luaheap.c`](../probe/luaheap.c) |
| Map grid reserve | Gives back the 178 MB of map detail grids reserved at startup for the largest map. | ✔ | [`mapgrid.c`](../probe/mapgrid.c) |
| Faster AI turns | Core update every 10 ms instead of 100, the exe's full small-block allocator switched off, up to 3 extra paced passes per frame over the core-to-UI queue. | ✔ | [`coretick.c`](../probe/coretick.c), [`smallblock.c`](../probe/smallblock.c), [`uiqueue.c`](../probe/uiqueue.c) |
| Fewer hitches | Data-dirty records within 250 ms and `TeamMet` records within 30 s of the last run are merged into one late run. | ✔ | [`uiqueue.c`](../probe/uiqueue.c) |

## Loading and safety checks

The DLL is a proxy for `msimg32.dll`. The exe imports it, it isn't a KnownDLL, and no
compatibility shim loads it first, so the copy in the game folder wins. It loads
`SysWOW64\msimg32.dll` and forwards its 5 exports. Inside any exe whose name doesn't start with
`CivilizationV` it does nothing else.

A `version.dll` proxy is built too, but Civ V never loads it: the AcGenral compatibility shim
(applied when e.g. "Override high DPI scaling" is set) imports version.dll and maps the system copy
before the game's imports are bound.

How the fix changes the game, from safest to most specific:

- **Import-table hooks**, set up while the DLL loads: the CRT allocator, the Lua heap's Windows
  heap calls, Lua, Miles, two `Interlocked*` slots, `PeekMessageW`.
- **Data writes**: handler tables, a frame-time accumulator, the map grid vectors.
- **Code patches**, only in the render command stream code, and only where the bytes match the
  expected ones exactly.

Parts marked "Exact places" above use addresses that differ per exe: they look up the running
exe (PE timestamp and image size) in `exes.c`, which has the final Steam DX11, DirectX 9 and
tablet exes. Each part checks the code or data it relies on first. If anything differs (another
build, an unknown exe), that part logs why in the events log and stays off. The game's main loop is never patched:
Steam's CEG checksums it, and a 6-byte patch there made the game quit itself ~30 s after launch.

## The main crash: render command stream overflow

The symptoms looked unrelated:

- `AIL_sample_status` / `AIL_stop_sample` in `mss32.dll`, called with garbage sample handles
  (`0xFFFFFFFF`, or a pointer into render data);
- `FAudioGame`'s vtable found to be `1` (crashes at `exe+0x4dc828` / `exe+0x517414`);
- a write past the end of a 2084 KB block at `exe+0x20932e`.

They all come from `core\CommandStream.cpp`. A stream is created by the factory `exe+0x4bd0c0` and
the constructor `exe+0x4ce150(a1, a2, bytes, 0)`. It holds two halves (double-buffered by frame),
and each half has:

| Array | Size per half | Notes |
|---|---|---|
| commands | a1 × 0x20 | entry +0x14 = record pointer, +4 = −1 or an argument |
| groups | a2 × 0x80 | +0x5c = first command index |
| data (records) | `bytes` | bump allocator `exe+0x4cfdc0` |
| list | 1024 dwords | |

**None of the writers checks a capacity:** not the record allocator, not the command appenders
`exe+0x4ccd80` (159 callers), `+0x4cce00` and `+0x4cce90`, and not the group writers.

The stream created at `exe+0x1c9964` (or the pair at `+0x1c993b` / `+0x1c995d`, depending on a
renderer flag) has a1 = 0x1000, a2 = 0x400 and 1 MB of data per half. On a Huge map with about
1,700 units, busy frames (scrolling, for example) need up to about 5,700 commands. Command entries
then run past the 128 KB command array into the next heap block, which in the dumps was
`FAudioSystemMiles`'s sample table: 12-byte slots `{flag, HSAMPLE, flag}`. The −1 field and the
record pointer of a command entry became "sound handles".

### What the fix changes (`probe/cmdstream.c`)

- **Spare halves, only for that stream.** The exe's `malloc` / `_aligned_malloc` imports are
  wrapped. When an allocation comes from the stream constructor called from one of
  `CommandStreamSpareCallers` (`0x1c9940, 0x1c9962, 0x1c9969`, the constructor's return
  addresses), the wrapper enlarges it: commands +3× per half, groups +1×, data +1 MB. Two
  constructor stubs (`exe+0x4ce1e8` for data, `exe+0x4ce20a` for commands and groups) place the
  second halves from that grant, so the layout always matches the allocation. The game's own
  capacity fields stay unchanged.
- **Bounds on every writer.** The record allocator `exe+0x4cfdc0` is redirected to a bounded one.
  Past a full half, records go to a per-stream pool. The 8 counter increments
  (`exe+0x4ccdf9, +0x4cce7b, +0x4ccefd, +0x4cdb30, +0x4cdc36, +0x4cdc9e, +0x4cdd0e, +0x4cdd6e`)
  are replaced with stubs that stop at the half's last slot. No branch lands inside the replaced
  bytes.
- **4 KB of slack** after every command-stream buffer, as a last line of defence.
- **Stats.** The flip call `exe+0x4bd9a7` goes through a counter, which logs each stream's peak use
  and whether it went past the game's capacity.

**Confirmed in play:** peak 5,665 / 4,096 commands and 1,280 / 1,024 KB data, over the game's
capacity on 63% of 48,102 frames, with zero entries past the enlarged halves and no crash.

What didn't work, so you don't have to try it: making the stream 4 MB by patching the
`push 100000h` immediates broke terrain rendering, and dropping overflowing records broke audio
(the consumer writes results back into records).

### Audio guard

`probe/audioguard.c` thunks the exe's 28 Miles sample imports. It records the handles Miles hands
out (`AIL_allocate_sample_handle`, `AIL_stream_sample_handle`) and forgets them when they're
released. A call with any other handle returns at once with a neutral result (`SMP_DONE`; getters
write zeros to their outputs). With the overflow fixed this should never trigger; the events log
counts the skips if it does.

## Load crash: event queue overflow

`probe/msgqueue.c`. Threads post events for the main thread into a queue object at
`exe+0x160e400`: two halves of 4 MB, a flip counter, and a table of 192 (handler, context) pairs. A
post reads the counter, reserves its record with `InterlockedExchangeAdd` on the half's used-bytes
word (through the exe's import) and writes `[size][type]` and the payload. Once a frame the main
loop's pass (`exe+0x59d20`) try-locks the queue (`InterlockedCompareExchange` import), flips and
walks the other half. Nothing checks the 4 MB. A Huge late-game save posted 5.07 MB on the main
thread while loading; the extra records ran into the other half, and the walk then called a NULL
handler (`exe+0x2a59a3`).

The fix hooks those two import slots, nothing else. A post that doesn't fit its half gets an offset
that points into a spill buffer (8 MB per half, allocated on first use), and so does every later
post to that half until its pass, to keep the order. At the pass's try-lock the probe runs the
game's own pass, then the spilled records' handlers in order under the same lock, and the game's
call returns as if the lock were taken. Delivered records are cleared, and the delivery waits
briefly for a record another thread has reserved but not yet written. The code around both slots is
checked first; another exe version is left alone.

Confirmed in play: a half hit 4.00 MB during a load, 6,779 posts were spilled and all delivered,
no crash.

## Quit crash

`probe/luaguard.c`. On exit, `exe+0x50523d` calls `luaL_unref` on a `lua_State` that `lua_close`
has already freed: an access violation at `lua51_Win32.dll+0x105b`, or `+0x3b05` when the memory is
still mapped. This happens without any of the fixes too. The guard wraps `lua_close` /
`lua_newstate` to remember closed states, and skips `luaL_unref` on a closed or unreadable state.

The same call site also releases refs it took through a Lua coroutine (`lua_newthread`) that the
garbage collector has since freed while the main state lives on: the `lua_State` block is still
mapped but its stack is gone, so the push through `L->top` faults at `+0x3b05` (more often under
mimalloc, which returns empty memory to Windows). The registry is shared by every thread of a
state, so the guard runs each registry `luaL_unref` on the state's main thread
(`global_State.mainthread`, +0x70), which is never collected.

The game releases these references without taking its own Lua lock (a critical section the exe's
script calls hold), while the game-core thread runs GameEvents on the main thread under it. So the
guard does each registry release under that lock, found from the code bytes of the exe's lock
wrapper. It only ever tries the lock: when another thread holds it, the release goes into a queue
that the next release holding the lock works through.

Saves made before quitting were never affected.

## Long-session fragmentation: mimalloc

`probe/mialloc.c`. Over a long session the CRT heap (NT heap with LFH) keeps adding 16 MB segments
while about 200 MB sits free in small holes: free address space drops with live bytes flat. The DLL
patches the CRT allocation imports of the exe, the game core, the database, localization, Lua and
msvcp90, so blocks up to 16 KB come from mimalloc 2.5.2. A free goes to whichever allocator owns
the block (`mi_is_in_heap_region`), so blocks from before the switch are still freed correctly.
Freed pages are never decommitted: the Vox Populi game core, called from a Lua script, once read a
deleted object, and that crashed on a decommitted page.

On the same Huge-map save, free space after 40 minutes was 346 MB with it and 166 MB without
(the full table is in [Settings](SETTINGS.md#mimalloc-mimalloc)).

## Huge-map memory spike: Lua database lookups

`probe/luagc.c`. Each Lua database query (`GameInfo`, `DB.Query`) is a small Lua userdata. Its
first column lookup builds a C++ hash of the column names (`Database::Results::HashColumnPositions`)
in the database DLL. Lua's GC only sees the small userdata, about 1/38 of the real cost, so dead
queries piled up to **420 MB** while a Huge game was being created, and the NT heap then kept that
address space.

The fix patches the database DLL's import of `lua_pushcclosure`, so every C function it registers
runs through a small thunk. A light counter tracks the DLL's CRT allocations made inside those
calls. Every `LuaGcStepMB` (64 MB) the thunk starts a collection on return, the same as a script's
`collectgarbage()`. It never runs inside a collection or a finalizer, and only if the Lua 5.1
`global_State` layout check passes. Result: free address space after creating a new Huge game went
from about 300 MB to about 850 MB.

A full collection of the late-game Lua heap (~100 MB) holds the UI for 30-80 ms, a visible
stutter. So the collection is spread out: `lua_gc(L, LUA_GCSTEP, 0)` from the following calls, at
most 1 ms of work in every 10 ms (`LuaGcSliceUs`), until the cycle has decided what is garbage and
swept it. If the database allocates another 64 MB first, the rest is done at once.

## Lua heap on mimalloc

`probe/luaheap.c`. The exe's `lua_Alloc` keeps every Lua object in a Windows heap of its own, which
never gives space back (~190 MB of address space for ~110 MB live, late in a Huge game). The exe's
four import slots for that heap (allocate, reallocate, free, size) go to wrappers that serve that
heap's calls from mimalloc and pass any other heap through. An 8-byte header keeps the requested
size, so the game's own Lua byte counter stays exact. In game the Windows Lua heap went from 193 to
20 MB.

## Map detail grids

`probe/mapgrid.c`. At startup the exe reserves two tiled grids for the largest map it supports
(152 × 96): 178 MB in two blocks. A map load sizes them for the real map anyway. Once both still
hold that reserve with no tiles for 20 s (the main menu), the sampler empties the two vectors
(capacity first, with compare-exchanges, so a load starting then reserves anew) and frees the
blocks with the exe's own aligned delete. Data writes only. About 99 MB is saved on a 97 × 66 Huge
map.

## Faster AI turns and fewer hitches

`probe/coretick.c`, `probe/smallblock.c`, `probe/uiqueue.c`. Most of a late-game AI turn is the
game waiting on itself: Vox Populi runs one AI player per game-core update, and the main loop hands
the core its next update only on a 100 ms frame-time tick and only once the core-to-UI queue is
empty. Three parts, all without changing game code:

- **Core tick.** A probe thread sets the main loop's frame-time accumulator (`exe+0x8cf91c`) to the
  0.1 s mark every `GameCoreIntervalMs` (10 ms), so the loop's own check passes sooner.
- **Small blocks.** Once the exe's small-block allocator has filled its 32 MB region, every small
  allocation scans ~1,300 chunk entries before falling back to `malloc`. The probe calls the exe's
  own `FSBADisable` export once the region has been full for 30 s.
- **Queue passes.** The UI takes in at most one record per unit per frame. From the run loop's
  `PeekMessageW` import the probe runs the game's queue pass up to 3 more times per frame, under the
  game's own conditions, paced by a time bucket so panning stays smooth.

**Fewer hitches.** Records of the two costliest generic events (`SerialEventGameDataDirty`,
`TeamMet`) that arrive within a window of the last run are merged into one late run, through thunks
in the handler table (a data write).

Measured results and every setting: [Settings](SETTINGS.md#faster-ai-turns-on-by-default).

## Other exes: DirectX 9 and tablet

The game ships three exes built from the same source on the same day: `CivilizationV_DX11.exe`,
`CivilizationV.exe` (DirectX 9) and `CivilizationV_Tablet.exe` (DirectX 11 with touch support).
Their code is laid out differently, so every DX11 address was looked up in the other two with
[`portmap.py`](../probe/portmap.py): the DX11 instructions at and before the address, with
relocated dwords and branch displacements masked, searched in the other exe; data addresses come
from the instructions that reference them. The few it couldn't place (tiny functions that occur
many times) were found by their callers. The test harness then maps each exe file and runs every
part's check against its table.

The game code the fix relies on is the same in all three, with these exceptions:

- **Tablet:** the command stream's record allocator rounds each record up to 256 bytes, so its
  replacement does too.
- **DirectX 9:** the render command stream is single-buffered. A stream holds one set of arrays
  (groups are 0x90 bytes, not 0x80), there is no frame flip that swaps halves, and a reset
  function (`exe+0x4eca40`) zeroes the counters each frame. The stream that overflowed in DX11 is
  created at the same place, with the same sizes (4096 commands, 1024 groups, 1 MB), and nothing
  checks them either. So the spare is plain extra room at the end of each array, every
  constructor allocation is noted with its spare, and the counters and the record allocator stop
  at what was allocated; they look the note up only once a count reaches the game's own capacity.
  The stats hook the reset instead of the flip.
- **DirectX 9:** the third command appender has no list, the exe's aligned delete has
  `FSBADeallocate` inlined, and the UI queue's call site tests the mode pointer with
  `test eax,eax` instead of `cmp eax,ebx`.

## Known limits

- **Large saves still use most of the address space.** A late-game Huge save with many civs takes
  about 1.1 GB just to load, leaving 300–400 MB of headroom. The events log warns under 200 MB free.
- **GPU driver regions.** The Direct3D driver maps CPU-visible buffers into the game in 32 MB
  regions that the kernel creates and never gives back; late in a session they were the only thing
  still growing. They come from the game's per-frame buffer uploads. Trimming the device, splitting
  uploads, GPU-only buffers and DXVK were all tried; none helped (see the
  [research notes](RESEARCH-NOTES.md#conclusion-on-the-gpu-driver-regions-2026-10-30)).
- **Grey terrain tiles after long scrolls on Huge maps.** The game textures under half of such a
  map at once, capped by a fixed table of 12,288 texture handles. Raising the pool or the page-in
  rate past the game's 8 broke textures or stranded tiles
  ([details](RESEARCH-NOTES.md#grey-tiles-after-scrolling-far-2026-10-30-terrain-page-in-and-base-texture-pool)).
- **Black UI squares** on DX11 with EUI/VP are a separate GPU/driver issue. Turning off GPU texture
  decode and the HD strategic view fixes them.

## Diagnostics

Everything used to find the bugs is still in the DLL, off by default: 2-second address-space
samples, full VA maps, CRT allocation attribution by call stack, a top-down placement test, a
reservation log with stacks, a CRT heap histogram, a page-protection watchpoint, the GPU mapping
watch, D3D11 call statistics and the UI handler times. See [Settings](SETTINGS.md#diagnostics-off-by-default)
and the [developer guide](../probe/README.md).
