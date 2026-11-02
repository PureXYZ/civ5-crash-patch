# Civ V Crash Patch

![A late-game Huge map in Civ V: turn 510, Vox Populi, the map covered in cities and units](docs/images/huge-map-late-game.webp)

A drop-in fix that keeps big games of **Sid Meier's Civilization V** from crashing: Huge maps,
lots of civilizations, big mods like Vox Populi, and long late games full of units. It also makes
late-game AI turns shorter.

You install it by copying one file into the game folder. It doesn't change gameplay, rules, saves
or mods, and it works with or without mods.

> **Version 1.3.0.** Tested on a large Vox Populi + EUI game on a Huge map: the crashes went
> away, and free memory stayed level over a 49-minute session. Also tested on the unmodded game.
> Please
> [report how it goes](https://github.com/PureXYZ/civ5-crash-patch/issues), good or bad.

**Contents:** [Why big games crash](#why-big-games-crash) ·
[What it fixes](#what-it-fixes) · [Install](#install) ·
[Game settings for big games](#game-settings-for-big-games) · [Questions](#questions) ·
[Documentation](#documentation) · [Build from source](#build-from-source)

---

## Why big games crash

Civ V is a 32-bit program. However much RAM your PC has, the game can only use about 4 GB of
memory, and a big game gets close to that. On top of that, a few bugs in the game waste memory,
and two of them corrupt memory when the game gets busy. The result is random crashes, usually in
busy turns, right after loading a big save, or late in a long session.

## What it fixes

| Problem | What you'd notice without the fix |
|---|---|
| **Crashes in busy turns.** With many units on the map, the game's graphics code overflows a fixed-size list and damages the memory next to it, often the sound system's. | Random crashes while scrolling or in AI turns, often blamed on the sound (`mss32.dll`). |
| **Crash right after loading a big save.** A big save posts more events while loading than the game's event list has room for. | The game crashes as the save finishes loading. |
| **Memory running out in long sessions.** The game's memory fills up with small gaps it can't reuse. The fix gives it a better memory manager, so free memory stays level. | Crashes after an hour or so of play. |
| **Memory spikes from mods.** Mods look things up in the game's database a lot, and the leftovers piled up (up to 420 MB when starting a new Huge game). They're now cleaned up as the game goes. | Crashes while setting up a Huge game, or early in it. |
| **Memory set aside and never used.** The game reserves room for the largest possible map, and keeps its script memory in a heap that never shrinks. The fix gives that room back (about 100 MB on a Huge map). | Less memory to spare late in a game. |
| **Crash when you quit.** | A crash report on the way out to the desktop. Your saves were never affected. |
| **Slow AI turns.** Most of a late-game AI turn is the game waiting on itself, not the AI thinking. | Late-game AI turns of a minute or more. In testing they went from about 65-70 s to about 45-50 s (with one Vox Populi option, see [tips](docs/TROUBLESHOOTING.md#ai-turns-are-slow)). |
| **Hitches while you pan the map during AI turns.** Some screen panels (and mods such as Global Relations) redraw every time the AI changes something. | The map stutters or freezes for a moment while you scroll during AI turns. |
| **Black icons in big modded games.** The game can hold only 12,288 textures and keeps nearly every one it loads. Big mod sets fill that, sooner with GPU Texture Decode and High Detail Strategic View. The fix makes room for 16,384 (DirectX 11). | Black or magenta squares on icons and city banners, "Error loading font texture". |
| **Grey terrain when you scroll.** The game keeps terrain textures for under half of a Huge map and rebuilds the rest from grey. The fix gives each map enough for all its tiles (DirectX 11; about 40-110 MB of memory space on a Huge map, 14 MB on maps the game already covers). | Grey patches that fill in slowly after you scroll far, worst during AI turns. |

All of them work in every mode the game offers: DirectX 11, DirectX 9 and tablet, except the
texture fixes, which are DirectX 11 only for now. Some change
exact places in the game, and each checks those places first; with a version of the game other
than the final Steam one (1.0.3.279) they switch themselves off and say so in the log.

**One change you'll see:** the opening movie and the Firaxis and 2K logos no longer play. The
movie's video player loads about 75 MB of video and Direct3D 9 drivers that stay for the whole
session, so the fix never starts it. To see the movie again, set `SkipIntroPlayer=0`
([Settings](docs/SETTINGS.md#memory-savings-on-by-default)).

**What it can't do:** nothing can raise the 4 GB limit of a 32-bit game, so a very large late
game can still get close to it. When that happens, the fix tells you in the game to save and
restart before it crashes; see
[Troubleshooting](docs/TROUBLESHOOTING.md#the-game-says-it-is-running-out-of-memory).

---

## Install

Takes about two minutes.

1. **Download** `CivVCrashPatch-1.3.0.zip` from the
   [latest release](https://github.com/PureXYZ/civ5-crash-patch/releases/latest) (under *Assets*).
2. **Unzip it.** Right-click the zip, choose **Extract All…**, then **Extract**.
3. **Open the game folder.** In Steam, go to **Library**, right-click
   **Sid Meier's Civilization V**, then choose **Manage → Browse local files**.
   A folder opens that contains `CivilizationV_DX11.exe`.
4. **Copy `msimg32.dll`** from the unzipped folder into that game folder.
   If Windows asks for permission, click **Continue**. Don't rename the file.
5. **Play.** When Steam asks how to launch, choose **DirectX 11**.

That's it. You don't need the settings file in the zip's `optional` folder; the fix works
without it.

### Check that it's working

After you start the game, a new folder named `civprobe` appears in the game folder. Open the
newest `events_….log` file in it with Notepad. The first line should start with
`Civ V Crash Patch 1.3.0`.

### Update

Close the game, download the new zip, and copy its `msimg32.dll` over the old one.

### Uninstall

Delete `msimg32.dll` from the game folder. You can delete the `civprobe` folder too. Nothing else
was changed. (Steam's *Verify integrity of game files* doesn't remove extra files, so delete the
DLL yourself.)

---

## Game settings for big games

The fix works with the game's settings as they are, but a few of them matter in big games:

- **Play in DirectX 11.** DirectX 9 keeps a second copy of the game's textures and models in
  memory (about 500 MB more on a Huge late-game save), and the texture fixes are DirectX 11 only.
- **Keep GPU Texture Decode on** (Options → Video Options; on by default). With it off, the game
  keeps every leader's textures in its own memory instead of on the graphics card: about 150 MB in
  a game with 17 leaders. Older advice turned it off against black icons; the fix's bigger texture
  table makes that unnecessary ([details](docs/TROUBLESHOOTING.md#keep-gpu-texture-decode-on)).
- **With Vox Populi, turn on its quick animations.** It took about 15 seconds off each late-game AI
  turn in testing ([how](docs/TROUBLESHOOTING.md#ai-turns-are-slow)).
- **Don't set `FluidFOW = 1`** in `config.ini` to save memory. It breaks the map's terrain.

More in [Troubleshooting and tips](docs/TROUBLESHOOTING.md).

---

## Questions

**Is it safe?** The full source code is in this repository. The DLL on the Releases page is
built from that source by GitHub's servers ([see the builds](https://github.com/PureXYZ/civ5-crash-patch/actions)),
not on anyone's PC, and each release lists the file's SHA-256 checksum.

**My antivirus flagged it.** Small unsigned DLLs that load into a game sometimes get flagged by
antivirus heuristics. The file isn't digitally signed, because signing costs money. You can
compare its checksum with the one on the release page, or [build it yourself](#build-from-source).

**Does it work with Vox Populi, EUI and other mods?** Yes. It was developed on a Vox Populi + EUI
game.

**Multiplayer, achievements, saves?** It doesn't touch game rules or save files, and your saves
work without it too. Multiplayer hasn't been tested.

**Which game version?** The final Steam version, 1.0.3.279. If your game is a different version,
each fix that depends on it notices and switches itself off (the log says so), and the game runs
as it would without that fix.

**Steam Deck or Linux (Proton)?** Not tested. Proton has to be told to load the DLL: add
`WINEDLLOVERRIDES="msimg32=n,b" %command%` to the game's launch options.

**Mac?** No. The Mac version is a different program.

**The game still crashed.** Please report it: see
[Reporting a crash](docs/TROUBLESHOOTING.md#reporting-a-crash) for the two files to attach.

**Anything else I can do for big games?** Yes: start with
[Game settings for big games](#game-settings-for-big-games).
[Troubleshooting and tips](docs/TROUBLESHOOTING.md) covers slow AI turns with Vox Populi, grey
tiles when you scroll far, very long sessions, and settings to avoid.

---

## Documentation

| If you want to… | Read |
|---|---|
| Fix a problem, get more out of big games, or report a crash | [Troubleshooting and tips](docs/TROUBLESHOOTING.md) |
| Change or turn off part of the fix | [Settings](docs/SETTINGS.md) |
| Understand what was wrong in the game and what the fix changes | [How it works](docs/HOW-IT-WORKS.md) |
| Build it, run the tests, or use the diagnostic tools | [Developer guide](probe/README.md) |
| Read the whole investigation, dead ends included | [Research notes](docs/RESEARCH-NOTES.md) |
| See what changed between versions | [Changelog](CHANGELOG.md) |

### In one paragraph, for technical readers

The fix is a proxy `msimg32.dll`: the exe imports it, so the copy in the game folder loads first
and forwards the real exports to the system DLL. The main crash was a render command stream
(`core\CommandStream.cpp`) with no capacity checks: on a Huge map with about 1,700 units, busy
frames needed up to ~5,700 of its 4,096 command slots, and the overflow landed in the Miles audio
engine's sample table, so render data was used as sound handles. The fix gives that stream spare
room and bounds every writer, after checking each patch site byte for byte. The other fixes hook
import tables or write game data; none patches the game's main loop. Details:
[How it works](docs/HOW-IT-WORKS.md).

## Build from source

You need [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`) and a POSIX shell (Git Bash works).

```bash
sh probe/build.sh
```

The DLL is `probe/build/msimg32.dll`. Tests and tools: [Developer guide](probe/README.md).
Releases are built by [`.github/workflows/build.yml`](.github/workflows/build.yml).

## License

MIT; see [LICENSE](LICENSE). Includes [mimalloc](https://github.com/microsoft/mimalloc)
(MIT); see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Sid Meier's Civilization and Civ are trademarks of Take-Two Interactive Software, Inc. This
project is not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.
