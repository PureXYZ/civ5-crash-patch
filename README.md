# Civ V Crash Patch

![A late-game Huge map in Civ V: turn 510, Vox Populi, the map covered in cities and units](docs/images/huge-map-late-game.webp)

A fix for the crashes in big games of **Sid Meier's Civilization V**: Huge maps, lots of
civilizations, big mods like Vox Populi, and long late games full of units. It also frees memory
the game wastes and makes late-game AI turns shorter.

- **One file.** Copy `msimg32.dll` into the game folder. Delete it to uninstall.
- **No gameplay changes.** Rules, saves and mods are untouched, and it works with or without mods.
  The one thing you'll notice is that the intro movie no longer plays.
- **Tested on big games:** a late-game Vox Populi + EUI game on a Huge map, and the unmodded
  game. Please [report how it goes](https://github.com/PureXYZ/civ5-crash-patch/issues), good or
  bad.

**[Install](#install)** · [What it fixes](#what-it-fixes) ·
[Game settings for big games](#game-settings-for-big-games) · [Questions](#questions) ·
[Documentation](#documentation)

---

## What it fixes

Civ V is a 32-bit program, so it can use only about 4 GB of memory, however much RAM your PC has.
Big games get close to that, and a few bugs in the game waste memory or corrupt it when the game
gets busy. That's where the random crashes come from: in busy turns, right after loading a big
save, or late in a long session.

### Crashes

- **Busy turns.** With many units on the map, a list in the game's graphics code overflowed and
  damaged the memory next to it, often the sound system's. That's why these crashes were often
  blamed on the sound (`mss32.dll`).
- **Loading a big save.** Loading posted more events than the game's event list had room for, and
  the game crashed as the save finished loading.
- **Quitting.** No more crash report on the way out to the desktop. (Your saves were never
  affected.)

### Memory

- **Long sessions.** The game's memory filled up with small gaps it couldn't reuse, until it
  crashed after an hour or so. A better memory manager keeps free memory level.
- **Mods.** Mods look things up in the game's database a lot, and the leftovers piled up: up to
  420 MB while starting a new Huge game. They're now cleaned up as the game goes.
- **Memory held for nothing.** The game reserves room for the largest possible map, gives the fog
  of war 24 MB per job thread that it barely uses, and keeps its script and database memory in
  heaps that never shrink. The fix gives that room back: about 100 MB on a Huge map, plus about
  90 MB of fog buffers with 4 threads.
- **The intro movie.** Its video player loads about 75 MB of video and Direct3D 9 drivers that
  stay for the whole session, so the fix never starts it. The opening movie and the Firaxis and 2K
  logos no longer play; `SkipIntroPlayer=0` brings them back
  ([Settings](docs/SETTINGS.md#memory-savings-on-by-default)).
- **A warning before it runs out.** Nothing can raise the 4 GB limit, so a very large late game
  can still fill it. When less than 200 MB is left, the game says so at the top of the screen:
  save, quit to the desktop and load the save again
  ([more](docs/TROUBLESHOOTING.md#the-game-says-it-is-running-out-of-memory)).

### Speed

- **Late-game AI turns.** Most of a late-game AI turn was the game waiting on itself, not the AI
  thinking. In testing, AI turns went from about 65-70 s to about 45-50 s, and Vox Populi's quick
  animations take off about 15 s more ([see below](#game-settings-for-big-games)).
- **Panning during AI turns.** Some screen panels, and mods such as Global Relations, redrew every
  time the AI changed something, so the map stuttered or froze for a moment while you scrolled.
  Redraws that come close together are now merged into one.

### Graphics (DirectX 11)

- **Black icons in big modded games.** The game can hold only 12,288 textures and keeps nearly
  every one it loads. Big mod sets fill that, and icons and city banners turn into black or
  magenta squares ("Error loading font texture"). Now there's room for 16,384.
- **Grey terrain when you scroll.** The game kept terrain textures for under a third of a Huge
  map and rebuilt the rest from grey as you scrolled, slowest in AI turns. Each map now gets
  enough for all its tiles. That costs about 40-110 MB of memory space on a Huge map, 14 MB on
  smaller ones.

Everything works in DirectX 11, DirectX 9 and tablet mode, except the graphics fixes and the
smaller fog buffers, which are DirectX 11 only.

---

## Install

1. **Download** `CivVCrashPatch-1.4.0.zip` from the
   [latest release](https://github.com/PureXYZ/civ5-crash-patch/releases/latest) (under *Assets*).
2. **Unzip it.** Right-click the zip, choose **Extract All…**, then **Extract**.
3. **Open the game folder.** In Steam, go to **Library**, right-click
   **Sid Meier's Civilization V**, then choose **Manage → Browse local files**. A folder opens that
   contains `CivilizationV_DX11.exe`.
4. **Copy `msimg32.dll`** from the unzipped folder into that game folder. If Windows asks for
   permission, click **Continue**. Don't rename the file.
5. **Play.** When Steam asks how to launch, choose **DirectX 11**.

That's all. You don't need the settings file in the zip's `optional` folder.

**Check that it's working.** After you start the game, a `civprobe` folder appears in the game
folder. Open the newest `events_….log` in it with Notepad. The first line should start with
`Civ V Crash Patch 1.4.0`.

**Update.** Close the game and copy the new `msimg32.dll` over the old one.

**Uninstall.** Delete `msimg32.dll` from the game folder, and the `civprobe` folder if you like.
Nothing else was changed. Steam's *Verify integrity of game files* doesn't remove extra files, so
delete the DLL yourself.

---

## Game settings for big games

The fix works with the game's settings as they are, but a few of them matter in big games:

- **Play in DirectX 11.** DirectX 9 keeps a second copy of the game's textures and models in
  memory (about 500 MB more on a Huge late-game save), and the graphics fixes are DirectX 11 only.
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

**Is it safe?** The full source code is in this repository. The DLL on the Releases page is built
from that source on GitHub's servers
([see the builds](https://github.com/PureXYZ/civ5-crash-patch/actions)), not on anyone's PC, and
each release lists the file's SHA-256 checksum.

**My antivirus flagged it.** Antivirus heuristics sometimes flag small DLLs that load into a game
and aren't digitally signed, and signing costs money. You can compare the file's checksum with the
one on the release page, or [build it yourself](#build-from-source).

**Does it work with Vox Populi, EUI and other mods?** Yes. It was developed on a Vox Populi + EUI
game.

**Saves and multiplayer?** It doesn't touch game rules or save files, and your saves work without
it too. Multiplayer hasn't been tested.

**Which game version?** The final Steam version, 1.0.3.279. Some fixes change exact places in the
game and check them first. On any other version those switch themselves off and say so in the log,
and the game runs as it would without them.

**Steam Deck or Linux (Proton)?** Not tested. Proton has to be told to load the DLL: add
`WINEDLLOVERRIDES="msimg32=n,b" %command%` to the game's launch options.

**Mac?** No. The Mac version is a different program.

**The game still crashed.** Please report it.
[Reporting a crash](docs/TROUBLESHOOTING.md#reporting-a-crash) lists the two files to attach.

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

## For technical readers

The fix is a proxy `msimg32.dll`: the exe imports it, so the copy in the game folder loads first
and forwards the real exports to the system DLL. The main crash was a render command stream
(`core\CommandStream.cpp`) with no capacity checks: on a Huge map with about 1,700 units, busy
frames needed up to ~5,700 of its 4,096 command slots, and the overflow landed in the Miles audio
engine's sample table, so render data was used as sound handles. The fix gives that stream spare
room and bounds every writer. The other fixes hook imports, write game data or change code, each
after checking the bytes it relies on. None touches the game's main loop, and the newer code
changes stay out of the code Steam's copy protection checksums. Details:
[How it works](docs/HOW-IT-WORKS.md#loading-and-safety-checks).

### Build from source

You need [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`) and a POSIX shell (Git Bash works).

```bash
sh probe/build.sh
```

The DLL is `probe/build/msimg32.dll`. Tests and tools: [Developer guide](probe/README.md). Releases
are built by [`.github/workflows/build.yml`](.github/workflows/build.yml).

## License

MIT; see [LICENSE](LICENSE). Includes [mimalloc](https://github.com/microsoft/mimalloc) (MIT); see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Sid Meier's Civilization and Civ are trademarks of Take-Two Interactive Software, Inc. This
project is not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.
