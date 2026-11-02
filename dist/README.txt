Civ V Crash Patch 1.4.1
https://github.com/PureXYZ/civ5-crash-patch
=============================================

Keeps big Civilization V games from crashing: Huge maps, lots of civilizations, big mods
like Vox Populi, and long late games full of units. It also makes late-game AI turns
shorter.

Civ V is a 32-bit program, so it can only use about 4 GB of memory, and big games get close
to that. A few bugs in the game waste memory or corrupt it when the game is busy. This fix:

  - stops crashes in busy turns and right after loading a big save
  - makes the game's memory last longer and cleans up memory it leaves behind
  - warns you in the game when memory is nearly full, so you can save and restart
    before it crashes
  - stops the crash when you quit
  - shortens AI turns and cuts hitches while you pan the map during them
  - makes room for more textures, so icons don't turn black in big modded games
    (DirectX 11)
  - keeps the whole map's terrain textured, so it doesn't turn grey when you scroll
    (DirectX 11)

It doesn't change gameplay, rules, saves or mods. It works in every mode Steam offers when
you start the game: DirectX 11, DirectX 9 and tablet.

One change you'll see: the opening movie and logos no longer play. The movie's video player
loads about 75 MB of drivers that stay for the whole session, so the fix never starts it.
To see the movie again, put the settings file (optional\civprobe.ini, see OTHER FILES below)
in the game folder and change its ;SkipIntroPlayer=2 line to SkipIntroPlayer=0.


INSTALL
-------
1. In Steam, go to Library. Right-click "Sid Meier's Civilization V" and choose
   Manage > Browse local files. A folder opens that contains CivilizationV_DX11.exe.

2. Copy msimg32.dll (from this folder) into that game folder.
   If Windows asks for permission, click Continue. Don't rename the file.

3. Start the game and choose DirectX 11 when Steam asks.

You don't need optional\civprobe.ini. It's only for changing settings (see OTHER FILES below).

To check that it works: after you start the game, the game folder has a new folder called
"civprobe". The first line of the newest events_....log file in it contains
"Civ V Crash Patch 1.4.1".


GAME SETTINGS FOR BIG GAMES
---------------------------
The fix works with the game's settings as they are, but a few of them matter in big games:

  - Play in DirectX 11. DirectX 9 keeps a second copy of the game's textures and models in
    memory (about 500 MB more on a Huge late-game save), and the texture fixes are
    DirectX 11 only.
  - Keep GPU Texture Decode on (Options > Video Options; on by default). With it off, the
    game keeps every leader's textures in its own memory instead of on the graphics card:
    about 150 MB in a game with 17 leaders.
  - With Vox Populi, turn on its quick animations: about 15 seconds off each late-game AI
    turn in testing (how: see the tips link at the end).
  - Don't set FluidFOW = 1 in config.ini to save memory. It breaks the map's terrain.


UPDATE
------
Close the game and copy the new msimg32.dll over the old one. Then start the game and check
the newest log's first line for the new version (as in INSTALL).


UNINSTALL
---------
Delete msimg32.dll from the game folder (and civprobe.ini and the civprobe folder, if you
like).
Nothing else was changed.


STILL CRASHING?
---------------
Please report it: https://github.com/PureXYZ/civ5-crash-patch/issues

1. Start the game once more (you can quit at the menu). This adds what Windows recorded
   about the crash to the old log.
2. Attach the events_....log from the civprobe folder for the game that crashed. The time
   in its name is when that game started, so it's usually not the newest one.
3. With Vox Populi or the Community Patch, also zip crashes.log and the CvMiniDump_....dmp
   from that time in the game's crashlogs folder, and attach the zip.

Attachments are public. The events log leaves out your Windows user name, but a .dmp file
can contain it and other details of your PC. If you'd rather not post it, leave it out.

Tips for big games (slow AI turns, grey tiles, black icons, long sessions):
https://github.com/PureXYZ/civ5-crash-patch/blob/main/docs/TROUBLESHOOTING.md


OTHER FILES IN THIS ZIP
-----------------------
optional\civprobe.ini    Settings. Not needed. To use it, copy it into the game folder, next
                         to msimg32.dll, and edit it there. What each setting does:
                         https://github.com/PureXYZ/civ5-crash-patch/blob/main/docs/SETTINGS.md
LICENSE.txt              MIT license.
THIRD-PARTY-NOTICES.txt  License of the included mimalloc library.

Sid Meier's Civilization and Civ are trademarks of Take-Two Interactive Software, Inc.
This project is not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.
