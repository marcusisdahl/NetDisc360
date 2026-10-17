# NetISO for Aurora

Lists the games on your [NetISO](https://consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)) server in Aurora like installed games, with cover art, title updates and DLC. Pick one and it plays; no file browser, no Mount folder.

Each game gets a small launcher (`default.xex` plus `netiso.ini`) carrying the game's Title ID and Media ID. Aurora scans it as that game. Starting it asks the NetISO plugin to mount the game's ISO from the server, waits until the emulated disc is that game, and boots the disc. The game itself runs from the plugin's emulated DVD drive, so it can stream from the disc as usual.

## What you need

- The NetISO plugin (`NetISO.xex`) loaded by DashLaunch, and a NetISO server (`server.exe`, [netiso-srv](https://github.com/tuxuser/netiso-srv) or Docker) with your ISOs. The modified `Nova.xex` is not needed.
- To build: Visual Studio with the Xbox 360 SDK (the same setup as FastFTP).

## Use

1. Build: `.\build.ps1 -Tests` makes `build\NetISO Sync\` (`default.xex` and `netiso.ini`).
2. Edit `netiso.ini` there if needed (`server=` if the plugin's `netiso.xex.txt` isn't at the root of Hdd1:), and copy the folder to the console as `Hdd1:\NetISO\NetISO Sync`.
3. In Aurora: Settings, Content, Scan paths, add `Hdd1:\NetISO` with depth 1, and scan. NetISO Sync appears as a tile (Title ID FFFF4E49).
4. Start NetISO Sync. It asks the server for its ISOs, makes a folder in `Hdd1:\NetISO` for each one it hasn't seen (a copy of itself carrying that game's Title ID and Media ID, plus `netiso.ini`), removes the folders of ISOs that are gone, and returns to Aurora. Scan again (or let Aurora scan at startup) and the games are there.

After that, add ISOs to the server's folder and start NetISO Sync again. To have it run at every boot, make it DashLaunch's Default and set `after=` in its `netiso.ini` to Aurora's XEX.

Sync reads only a few sectors of each new ISO over its own server connection, so it never changes the disc the plugin has mounted. ISOs it already has folders for are not read at all; if their launcher came from an older NetISO Sync, it is replaced with this version's, keeping the game's IDs, so updating NetISO Sync's own default.xex and running it updates every game. Folders are removed only if they hold nothing but the launcher's own files, and nothing is removed when the server lists no ISOs.

`tools\netiso_aurora.py` does the same job on a PC, for when you'd rather copy the folders yourself.

Starting a game: the game's launcher (which carries the game's Title ID for Aurora's tile) first pings the NetISO server; if it doesn't answer, the progress bar turns red for 5 s and you're back in Aurora. Otherwise it starts `boot.xex` next to it, a copy of the same program built without a Title ID, like Aurora. That helper asks the plugin to mount the ISO, leaves the disc alone for 5 s, checks that the disc is the game, and starts it the way Aurora's disc entry does: `XLaunchNewImage("Dvd:\default.xex")` with `Dvd:` linked to `\Device\Cdrom0` (read from Aurora.xex 0.7b.2). The plugin swaps the disc at once and then fakes a tray open and close on its own thread, after which the console reads the new disc. When the launcher read the disc right away and kept polling it meanwhile, the game it started had the Guide dead and crashed at its storage/profile prompt; the same mount from Aurora's file browser, which leaves the disc alone, was fine. A red bar means something failed, and `netiso.log` in the game's folder says why. Sync copies `boot.xex` from its own folder into every game folder; without it, the launcher mounts by itself.

Four settings in a game folder's `netiso.ini` help test this on the console. `stop_after=` stops short of the mount and goes back to Aurora: after linking the NetIso drive (`link`), listing its root (`root`) or listing the ISO's folder (`iso`). `quiet_ms=` sets how long the launcher leaves the disc alone after the mount (5000 by default), `mount_only=1` mounts the game and goes back to Aurora without starting it, so you can start the disc from Aurora instead, and `start=` picks how the disc is started: `aurora` (the default), `nova` (XamLoaderLaunchTitleEx(default.xex, \Device\Cdrom0, flags 0x10), as Nova's file browser starts files; the previous way, which started the game with the Guide dead) or `media` (whatever disc is in the drive, as the dashboard starts it). If the chosen start fails, the launcher tries the `nova` one.

The NetIsoTiles plugin (`build\NetIsoTiles.xex`, from `plugin/main.cpp`) is the way around a dead Guide. Whenever the launcher asked for the mount, the Guide stayed dead until reboot (with or without Nova.xex, whatever started the game), while the same mount from Aurora's file browser, done while Aurora runs, kept it. So the plugin does the mount from Aurora. Add it to DashLaunch as a plugin, next to `NetISO.xex`, and reboot. It hooks xam's two launch calls (ordinals 420 and 421, which Aurora 0.7b.2 starts tiles with). When Aurora starts `default.xex` in a folder whose `netiso.ini` names an ISO, the plugin mounts that ISO on Aurora's thread, reaching the plugin's folders by their device path (`\Device\NetIso\...`) with no drive links of its own, as Aurora's file browser does, waits until the disc is the game, and starts `Dvd:\default.xex` the way Aurora's disc entry does. Two test switches in the game folder's `netiso.ini` cover what else the file browser might do around its mount: `hold_mount=1` keeps the Mount folder open until the disc is the game, and `relist=1` lists the ISO's folder and the root again right after the mount. Aurora sits still for those few seconds. It first pings the NetISO server (the tile's `server=`, or the NetISO plugin's `netiso.xex.txt` on Hdd1: or Usb0:) over Aurora's own network; if the server doesn't answer, it shows a notice and goes back to Aurora without touching either drive or starting anything. For a minute or two after boot the console can't reach the server, and a tile started then froze Aurora for 14 s on the NetISO drive and left the Guide dead without mounting anything. If the mount itself fails (ISO not listed, no disc after 35 s), it shows a notice and lets the tile's own launcher run as before. `tiles_stop=` in the game folder's `netiso.ini` is a test switch that goes back to Aurora after one step, silently: `server` (the ping), `disc` (reading the disc drive), `root` (listing the NetISO drive's root), `iso` (finding the ISO's Mount folder) or `mount` (the mount, before the start). It logs to the game folder's `netiso.log` with a `NetISO Tiles plugin` header line. On the console (2026-10-17), the plugin's first version mounted from inside Aurora and the Guide still died, while a file-browser mount followed by the plugin's start kept it; the device-path version's first listing of the NetISO root, without the root's trailing backslash and without the pattern FindFirstFile gives, never returned and left Aurora stuck, which this version fixes.

There is no unmount step: the plugin keeps the last ISO mounted until another one is mounted. Starting another game's tile mounts that game in its place, and starting the same game again skips the mount.

If something goes wrong, read `netiso.log`: Sync writes one in its folder, and each game writes one in its own folder (each folder it listed, and why it stopped). Every run adds to the end of the file, each line starting with the console's time. A run begins with a `====` line giving the date, the build time and how long the console has been on, so a fresh boot shows as a small uptime. A file over 512 KB starts over on the next run.

## How the mount works

The plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its folders lists the server's ISOs (`GetIsoName`), each with a `Mount` folder inside. Listing a `Mount` folder sends `MountIso` with that path; the server strips `\Mount` and backslashes and mounts the ISO whose name ends with the rest.
- After a successful mount the plugin cycles a fake disc tray, and the console reads the ISO as the disc.

The launcher walks those folders by name (root, the ISO, `Mount`) rather than assuming their depth, and then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID before launching it. Its XEX has privilege 0 so the fake tray cycle doesn't close it.

## Not verified on a console yet

- Starting the disc as Aurora's disc entry does (`Dvd:\default.xex`) and whether the Guide then works. Mounting from the launcher works on the console; XamLoaderLaunchTitleEx starts the game but with the Guide dead, and `XLaunchNewImage` on `\Device\Cdrom0\default.xex` ended on the Xbox dashboard.
- Sync on the console: network startup, the server connection, and writing to Hdd1:.
- That the kernel loads the launcher after its execution info header is rewritten. Sync and the PC tool recompute the header digest (SHA-1 at security info + 0x164) so it matches the edited header; only the RSA signature over it is left stale, which a patched kernel skips. BadUpdate/XeUnshackle is untested.
- How Aurora names the games: it takes the Title ID from the header, but the launcher has no title name resource, so names come from Aurora's online lookup.
- Multi-disc games: each disc's ISO gets its own launcher; swapping discs mid-game is not handled.

## Tests

`python tests\test_sync.py` builds Sync's core for Linux and runs it against `tests/netiso_server.py`: it checks that Sync writes exactly what the PC tool writes, adds only new ISOs, removes folders of ISOs that are gone (and keeps folders with other files in them), removes nothing when the server lists nothing, skips bad ISOs, and keeps clear of folders it didn't make.

`python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the generator: execution info copied exactly and nothing else changed, the ini, FATX-safe unique folder names, bad ISOs skipped, recursive search and duplicate file names.
