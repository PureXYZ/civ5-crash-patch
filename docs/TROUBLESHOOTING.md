# Troubleshooting and tips

For players. Nothing here is required: the fix works as installed. These are answers to common
problems with big games, and what to do if something goes wrong.

- [Reporting a crash](#reporting-a-crash)
- [The log and the `civprobe` folder](#the-log-and-the-civprobe-folder)
- Problems
  - [Black squares on icons or city banners](#black-squares-on-icons-or-city-banners)
  - [Grey tiles when you scroll far across the map](#grey-tiles-when-you-scroll-far-across-the-map)
  - [AI turns are slow](#ai-turns-are-slow)
  - [The log says "under 200 MB free"](#the-log-says-under-200-mb-free)
  - [DirectX 9 shows only icons, smeared across the map](#directx-9-shows-only-icons-smeared-across-the-map)
  - [Units, flags or panels look out of date after AI turns](#units-flags-or-panels-look-out-of-date-after-ai-turns)
  - [The game freezes for a long time in AI turns](#the-game-freezes-for-a-long-time-in-ai-turns)
  - [My antivirus removed the DLL](#my-antivirus-removed-the-dll)
- [Settings to avoid](#settings-to-avoid)
- [Turning the fix off to compare](#turning-the-fix-off-to-compare)

---

## Reporting a crash

Please [open an issue](https://github.com/PureXYZ/civ5-crash-patch/issues/new/choose). Two things
help most:

1. **The events log of the game that crashed.**
   - First start the game once more (you can quit at the main menu). That adds what Windows
     recorded about the crash to the old log.
   - Then, in the `civprobe` folder inside the game folder, find the `events_YYYYMMDD_HHMMSS.log`
     of the game that crashed. The date and time in its name are when that game *started*, so it's
     usually not the newest file.
2. **The crash files, with Vox Populi or the Community Patch.** These mods write `crashes.log` and a
   `CvMiniDump_….dmp` (with the crash time in its name) to the game's `crashlogs` folder. Put both
   in a zip (right-click → **Send to → Compressed (zipped) folder**), because GitHub doesn't accept
   `.dmp` files.

Issues and their attachments are public. The events log writes your user folder as
`%USERPROFILE%`, but a `.dmp` file can contain your Windows user name (in the paths of mods under
Documents) and other details of your PC. If you'd rather not post it, leave it out and say so.

Also say which mode you launched (DirectX 11, DirectX 9 or tablet), your mods, the map size and
the number of players.

## The log and the `civprobe` folder

The fix keeps a small log of each game launch in the `civprobe` folder inside the game folder
(Steam → right-click the game → **Manage → Browse local files**).

- **`events_….log`**: one per launch. The first line names the fix and its version. After that it
  lists what the fix turned on, the free memory at 1 minute, every 10 minutes and at exit, and
  anything unusual: crashes, freezes, warnings.
- The newest 20 launches are kept; older logs are deleted automatically
  (`KeepSessions` in [Settings](SETTINGS.md#general)).
- It's safe to delete the folder at any time. It comes back the next time you start the game.

Lines that start with `exception` don't always mean a crash. The game raises and handles a few
errors on purpose (one at `CivilizationV_DX11.exe+0x523dcb` at every start), and the log records
those too.

---

## Black squares on icons or city banners

Common with EUI, Vox Populi and other big mod sets in DirectX 11. The game can hold only 12,288
textures and keeps nearly every one it loads; once that's full, new icons come out black or
magenta, and you may see "Error loading font texture". **GPU Texture Decode**, **High Detail
Strategic View** and the highest terrain detail fill it sooner. The fix makes room for 16,384 in
DirectX 11, which was enough for a big mod set on a Huge map with all three on.

If you still see them, the events log in the `civprobe` folder says `the texture table is full`.
Then open **Options → Video Options** in the game and do one of these, then restart the game:

- Lower **Terrain Detail Level** one step from the highest. That alone frees about 2,000
  textures. (In `GraphicsSettingsDX11.ini` it's `TerrainDetailLevel = 2` instead of 3.)
- Turn off **GPU Texture Decode** and **High Detail Strategic View**.

## Grey tiles when you scroll far across the map

The game builds only a few pieces of terrain per frame (often 1 or 2), so after a long scroll they
fill in slowly, most of all in slow AI-turn frames. To make them fill in faster:

1. Close the game.
2. Open `Documents\My Games\Sid Meier's Civilization 5\GraphicsSettingsDX11.ini` in Notepad
   (it's next to `config.ini`).
3. Set `TerrainPageinSpeedStill = 8` and `TerrainPageinSpeedMoving = 8`, and save.

8 is the most the game accepts. If panning starts to stutter, lower both to 4. Changing the
graphics quality in the game's options may reset them.

On big maps that only shortens the grey. The game keeps terrain textures for at most 3,072 tiles,
under half of a Huge map, so a part of the map you haven't looked at lately is always rebuilt
from grey. To keep the whole map textured:

1. Play for 10 minutes, then open the newest `events_….log` in the `civprobe` folder and find
   the line `terrain: TerrainBaseSlots=… would hold every tile of this map`.
2. Close the game, add that line (without `terrain: ` and the words after the number) to
   `civprobe\civprobe.ini`, for example `TerrainBaseSlots=16700`, and start the game again.

A few minutes after loading, every tile has its textures and scrolling shows no grey. The cost is
address space, the game's scarcest resource: about 10 KB per slot past the game's 6,144 (110 MB
less free on a Huge save with 16700). If the `memory (10 min)` lines get near the 200 MB warning
late in a game, use a smaller number: half way still keeps much more of the map textured.
([Settings](SETTINGS.md#more-textures-on-by-default),
[details](RESEARCH-NOTES.md#grey-tiles-second-try-the-whole-map-in-the-pool-2026-11-01).)

## AI turns are slow

The fix already shortens AI turns by itself in DirectX 11. **With Vox Populi, also turn on its
quick animations.** Vox Populi plays the "activate" animation of workers, great people,
missionaries and diplomats even with Quick Movement on, and the AI waits 2-3 seconds for each one
it shows you: about 15 seconds per AI turn in a late-game test save.

1. Close the game.
2. Open `MODS\(1) Community Patch\Database Changes\NewCustomModOptions.xml` (in
   `Documents\My Games\Sid Meier's Civilization 5`) in Notepad.
3. Find the `UI_QUICK_ANIMATIONS` line and change it to
   `<Row Class="4" Name="UI_QUICK_ANIMATIONS" Value="1"/>`. Save.
4. Make sure **Quick Movement** is on in the game options.

A Vox Populi update resets this file, so redo it after updating. Together, the fix and this option
took late-game AI turns from about 65-70 seconds to about 45-50 in testing.

If you'd rather have even shorter AI turns and don't mind more stutter while panning during them,
see `UiQueueShare` in [Settings](SETTINGS.md#faster-ai-turns-on-by-default).

## The log says "under 200 MB free"

A big late-game save can still use most of the memory a 32-bit game gets, and the fix can't raise
that limit. When the newest `events_….log` says *under 200 MB free*, save, quit to the desktop and
load the save again. That starts the game with a clean slate.

## DirectX 9 shows only icons, smeared across the map

In DirectX 9 the map and units are missing: only unit flags, city banners and other icons are
drawn, and they leave trails when the view moves (the loading picture may still show behind them).
This is the game's DirectX 9 mode on some current PCs, not the fix: in testing (Windows 11, an
NVIDIA card) the game as shipped, with the fix turned off, showed the same on a small map without
mods. Others have reported it too, with no known cure.

**Play in DirectX 11.** A different graphics driver may help, untested.

DirectX 9 also needs much more memory than DirectX 11: it keeps a second copy of the game's
textures and models in the game's own memory. On a Huge late-game save it ran out (about 500 MB
more than DirectX 11), and the game as shipped crashed.

## Units, flags or panels look out of date after AI turns

This hasn't been seen in testing, but the faster AI turns work through the AI's updates faster
than the game normally does, and some screen panels redraw a little less often. If something on
screen looks stale after an AI turn, add these lines to a settings file (see
[Settings](SETTINGS.md#using-a-settings-file)) and restart the game:

```ini
[probe]
UiQueuePasses=1
UiDirtyCoalesceMs=0
```

Then please [open an issue](https://github.com/PureXYZ/civ5-crash-patch/issues) saying what looked
wrong.

## The game freezes for a long time in AI turns

Long freezes (a minute or more) in AI turns have so far come from mods, for example the Global
Relations mod in some late games. The fix logs where the game is stuck when the window stops
responding for 30 seconds, including the Lua script and line when it's inside a mod script. Look
for lines starting with `freeze:` or `lua:` in the events log, and include the log if you report it.

## My antivirus removed the DLL

Small unsigned DLLs that load into a game sometimes get flagged by antivirus heuristics. Download
it again from the [release page](https://github.com/PureXYZ/civ5-crash-patch/releases/latest), check
that its SHA-256 matches the one listed there, and allow it in your antivirus. Or
[build it yourself](../README.md#build-from-source).

---

## Settings to avoid

- **Don't set `FluidFOW = 1`** in `config.ini` to save memory. It breaks terrain rendering.

## Turning the fix off to compare

To check whether a problem comes from the fix, you don't have to uninstall it. Put a settings
file in the `civprobe` folder (see [Settings](SETTINGS.md#using-a-settings-file)) with:

```ini
[probe]
Enabled=0
```

The DLL then does nothing but pass calls through to Windows' own `msimg32.dll`. Delete the line (or set `Enabled=1`) to turn it back on. Each
part can also be turned off on its own; [Settings](SETTINGS.md) lists them.
