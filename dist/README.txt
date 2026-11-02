Civ V Crash Patch 1.2.1
https://github.com/PureXYZ/civ5-crash-patch
=============================================

Keeps big Civilization V games from crashing: Huge maps, lots of civilizations, big mods
like Vox Populi, and long late games full of units. It also makes late-game AI turns
shorter.

Civ V is a 32-bit program, so it can only use about 4 GB of memory, and big games get close
to that. A few bugs in the game waste memory or corrupt it when the game is busy. This fix:

  - stops crashes in busy turns and right after loading a big save
  - makes the game's memory last longer and cleans up memory it leaves behind
  - stops the crash when you quit
  - shortens AI turns and cuts hitches while you pan the map during them
  - makes room for more textures, so icons don't turn black in big modded games
    (DirectX 11)

It doesn't change gameplay, rules, saves or mods. It works in every mode Steam offers when
you start the game: DirectX 11, DirectX 9 and tablet.


INSTALL
-------
1. In Steam, go to Library. Right-click "Sid Meier's Civilization V" and choose
   Manage > Browse local files. A folder opens that contains CivilizationV_DX11.exe.

2. Copy msimg32.dll (from this folder) into that game folder.
   If Windows asks for permission, click Continue. Don't rename the file.

3. Start the game and choose DirectX 11 when Steam asks.

To check that it works: after you start the game, the game folder has a new folder called
"civprobe". The first line of the newest events_....log file in it starts with
"Civ V Crash Patch 1.2.1".


UPDATE
------
Close the game and copy the new msimg32.dll over the old one.


UNINSTALL
---------
Delete msimg32.dll from the game folder (and the civprobe folder if you like).
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
optional\civprobe.ini    Settings. Not needed. To use it, copy it into the civprobe folder
                         inside the game folder. What each setting does:
                         https://github.com/PureXYZ/civ5-crash-patch/blob/main/docs/SETTINGS.md
LICENSE.txt              MIT license.
THIRD-PARTY-NOTICES.txt  License of the included mimalloc library.

Sid Meier's Civilization and Civ are trademarks of Take-Two Interactive Software, Inc.
This project is not affiliated with or endorsed by Firaxis Games, 2K or Take-Two Interactive.
