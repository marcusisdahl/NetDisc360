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

Starting a game: its launcher mounts the ISO through the plugin, waits until the disc has been the game for 5 s, then starts the disc the way Aurora does. While it works it shows a plain screen with a progress bar: a launcher that drew nothing left the Guide dead, in the game it started too.

If something goes wrong, read `netiso.log`: Sync writes one in its folder, and each game writes one in its own folder (each folder it listed, and why it stopped).

## How the mount works

The plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its folders lists the server's ISOs (`GetIsoName`), each with a `Mount` folder inside. Listing a `Mount` folder sends `MountIso` with that path; the server strips `\Mount` and backslashes and mounts the ISO whose name ends with the rest.
- After a successful mount the plugin cycles a fake disc tray, and the console reads the ISO as the disc.

The launcher walks those folders by name (root, the ISO, `Mount`) rather than assuming their depth, and then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID before launching it. Its XEX has privilege 0 so the fake tray cycle doesn't close it.

## Not verified on a console yet

- Launching the disc with XamLoaderLaunchTitleEx(default.xex, \Device\Cdrom0, flags 0x10), the call Aurora uses. Mounting from the launcher works on the console; `XLaunchNewImage` on the disc ended on the Xbox dashboard.
- Sync on the console: network startup, the server connection, and writing to Hdd1:.
- That the kernel loads the launcher after its execution info header is rewritten. Sync and the PC tool recompute the header digest (SHA-1 at security info + 0x164) so it matches the edited header; only the RSA signature over it is left stale, which a patched kernel skips. BadUpdate/XeUnshackle is untested.
- How Aurora names the games: it takes the Title ID from the header, but the launcher has no title name resource, so names come from Aurora's online lookup.
- Multi-disc games: each disc's ISO gets its own launcher; swapping discs mid-game is not handled.

## Tests

`python tests\test_sync.py` builds Sync's core for Linux and runs it against `tests/netiso_server.py`: it checks that Sync writes exactly what the PC tool writes, adds only new ISOs, removes folders of ISOs that are gone (and keeps folders with other files in them), removes nothing when the server lists nothing, skips bad ISOs, and keeps clear of folders it didn't make.

`python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the generator: execution info copied exactly and nothing else changed, the ini, FATX-safe unique folder names, bad ISOs skipped, recursive search and duplicate file names.
