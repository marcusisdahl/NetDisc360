# NetISO for Aurora

Lists the games on your [NetISO](https://consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)) server in Aurora like installed games, with cover art, title updates and DLC. Pick one and it plays; no file browser, no Mount folder.

Each game gets a small launcher folder in `Hdd1:\NetISO` (`default.xex`, `boot.xex` and `netiso.ini`). Its `default.xex` carries the game's Title ID and Media ID, so Aurora scans it as that game. Starting it shows a loading screen, asks the NetISO plugin to mount the game's ISO from the server, waits until the emulated disc is that game, and boots the disc the way Aurora's own disc entry does. The game runs from the plugin's emulated DVD drive, with a working Guide.

## What you need

- DashLaunch with the NetISO plugin (`NetISO.xex`) and this project's `NetIsoTiles.xex` as plugins. The modified `Nova.xex` is not needed.
- Aurora with its FTP server on (the default login, `xboxftp`, is fine).
- A Windows PC with your ISOs, on the same network as the Xbox.
- To build: Visual Studio 2022 with the Xbox 360 SDK (the same setup as Xbox360-FastFTP) and the "Desktop development with C++" workload.

## Set up

1. Build: `.\build.ps1 -Tests` makes `build\netiso-server.exe`, `build\NetIsoTiles.xex` and `build\NetISO Sync\`.
2. Add `NetIsoTiles.xex` to DashLaunch's plugins in `launch.ini`, next to `NetISO.xex`, and reboot the Xbox.
3. Put the PC's IP address in `Hdd1:\netiso.xex.txt` (or `Usb0:\netiso.xex.txt`), as the NetISO plugin needs anyway.
4. On the PC, run `netiso-server.exe` in the folder with your ISOs, in place of `netiso-srv` or `server.exe`. Subfolders count too.
5. In Aurora: Settings, Content, Scan paths, add `Hdd1:\NetISO` with depth 1.

That's all. `netiso-server.exe` is one file: it serves the ISOs to the NetISO plugin, and once the Xbox connects it keeps `Hdd1:\NetISO` current over Aurora's FTP. A new ISO gets its folder (after the file has stopped changing for 10 s), named after the game without the region, language and revision tags (`Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso` gets `Banjo-Kazooie - Nuts & Bolts`; tags like `(Disc 2)` stay). A removed ISO loses its folder, and launchers from an older build are replaced. Then the server asks Aurora to scan the folder (`SITE INITIATESCAN`, a command of Aurora's FTP server), so the game shows up at once and Aurora looks up its name and art online while the network is up. Aurora looks a game up only once, when its scan first finds it, and its own scan at boot runs before the Xbox has a network: a game first found then keeps its folder name and no art until you refresh it in Aurora, or rename its folder in `Hdd1:\NetISO`: at its next scan Aurora drops the old entry and looks the game up again. The server keeps a renamed folder, since it matches folders to ISOs by their `netiso.ini`. The server checks the Xbox every 15 s while it's off and every minute while it's on, so it doesn't matter which starts first.

| Option | Default | |
|---|---|---|
| `--xbox <address>` | the console that connects | The Xbox to update. |
| `--ftp-user`, `--ftp-password` | `xboxftp` | Aurora's FTP login. |
| `--xbox-folder <path>` | `/Hdd1/NetISO` | The game folder, as an FTP path. |
| `--aurora <path>` | `/Hdd1/Apps/Aurora` | Aurora's folder, for the Xbox 360 category. |
| `--no-push` | | Only serve ISOs; leave the Xbox alone. |
| `--no-category` | | Leave Aurora's Xbox 360 category alone. |
| `--no-scan` | | Don't ask Aurora to scan after adding or removing games. |
| `--port`, `--ftp-port` | 4323, 21 | |
| `-v` | | Log every request. |

`-r` and `-b` from netiso-srv are accepted and ignored.

### Aurora's Xbox 360 category

Aurora files the launchers under Unsigned, because they aren't retail-signed, so its Xbox 360 category doesn't show them. The server edits `Media\Scripts\Content\FilterFunctions.lua` in Aurora's folder once, so that the category also lists games in the NetISO folder (NetISO Sync itself stays out), and keeps the original next to it as `FilterFunctions.lua.before-netiso`. If an Aurora update puts the original back, the server makes the same edit again. A filter it doesn't recognise is left alone, and `--no-category` turns this off.

## Playing

Pick the game's tile. The loading screen shows four steps: Server, Mount, Disc and Start.

- **B cancels at any step** and goes back to Aurora at once, even while the network or the server is slow.
- Right after a reboot the Xbox can't reach the server for a minute or two, and the NetISO plugin takes a little longer still (it lists only "No Server!" until then). For the first 4 minutes after boot the screen waits for both instead of failing.
- When something goes wrong, the screen says what and what to do: the server doesn't answer, the NetISO plugin isn't loaded, the ISO is no longer on the server, the disc didn't show up, a different game is in the drive, or the game didn't start. **A** goes back to Aurora and **X** tries again; after a minute it goes back to Aurora by itself.

There is no unmount step: the plugin keeps the last ISO mounted until another one is mounted. Starting another game mounts that game in its place, and starting the same game again skips the mount.

## NetISO Sync

`build\NetISO Sync\` is the same launcher without a game: copied to the console as `Hdd1:\NetISO\NetISO Sync` and started from Aurora, it asks the NetISO server for its ISOs, writes and removes game folders the way `netiso-server.exe` does, and returns to Aurora. It works with any NetISO server, for when the PC can't run `netiso-server.exe`. `server=` in its `netiso.ini` names the server if `netiso.xex.txt` doesn't, and `after=` names what to start when it's done (to run it at every boot as DashLaunch's Default, set `after=` to Aurora's XEX). `tools\netiso_aurora.py` makes the same folders on a PC, for copying by hand.

Folders are removed only if they hold nothing but the launcher's own files, and nothing is removed when the server lists no ISOs. Original Xbox ISOs (no `default.xex`) are skipped.

## How it works

### The mount

The NetISO plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its root lists the server's ISOs (`GetIsoName`), each with a `Mount` folder inside, plus `[Disable Current ISO]` while one is mounted. Listing a `Mount` folder sends `MountIso` with that path; the server strips `\Mount` and backslashes and mounts the ISO whose name ends with the rest.
- After a successful mount it swaps the disc and fakes a tray open and close on its own thread, after which the console reads the ISO as the disc.
- Right after every mount request it calls `XamAppUnloadSelf` on the thread that asked.

The launcher walks those folders by name (root, the ISO, `Mount`) rather than assuming their depth, leaves the disc alone for 5 s, then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID. It starts the game with `XLaunchNewImage("Dvd:\default.xex")` and `Dvd:` linked to `\Device\Cdrom0`, as Aurora 0.7b.2's disc entry does.

### The Guide

`XamAppUnloadSelf` is what killed the Guide. From the Guide's own file browser that call just closes the Guide, but a mount asked from anywhere else left the Guide dead until reboot. At start-up `NetIsoTiles.xex` points NetISO's import of that call at its own function, which skips it unless the Guide's browser asked, after checking that NetISO 1.0's code is where it was read. With that, the launcher's mount keeps the Guide (seen on the console, 2026-10-17).

Two more things the launcher does for the Guide, each found on the console: it starts the game the way Aurora's disc entry does (`XamLoaderLaunchTitleEx`, as Nova's browser starts files, left the Guide dead), and it stays off the disc for 5 s after the mount (reading it during the fake tray cycle left the Guide dead and crashed the game at its storage prompt). The game's `default.xex` also hands the mount to `boot.xex` next to it, the same program without a Title ID, like Aurora; the mount happens there. The launcher keeps presenting frames until it launches, since a title that never drew left the Guide dead too.

`NetIsoTiles.xex` also hooks xam's two launch calls (ordinals 420 and 421, which Aurora starts tiles with). By default it leaves tiles to their launcher. `tiles=aurora` in a game's `netiso.ini` has it mount from inside Aurora instead, the older way, with no loading screen and no cancel.

## Test switches

These go in a game folder's `netiso.ini` and help find out what breaks on the console.

- `stop_after=link|root|iso` stops short of the mount and goes back to Aurora: after linking the NetISO drive, listing its root, or listing the ISO's folder.
- `quiet_ms=` sets how long the launcher leaves the disc alone after the mount (5000).
- `mount_only=1` mounts the game and goes back to Aurora without starting it.
- `start=aurora|nova|media` picks how the disc is started: as Aurora's disc entry does (the default), as Nova's file browser does, or whatever disc is in the drive, as the dashboard does. If the chosen start fails, the launcher tries `nova`.
- With `tiles=aurora`: `hold_mount=1`, `relist=1`, `tiles_stop=server|disc|root|iso|mount` and `tiles_thread=system` change how the plugin mounts.

## Logs

Each game writes `netiso.log` in its folder, and NetISO Sync in its own: each folder listed, each step and why it stopped. Every run adds to the end of the file, each line starting with the console's time. A run begins with a `====` line giving the date, the build time and how long the console has been on, so a fresh boot shows as a small uptime. A file over 512 KB starts over on the next run. `netiso-server.exe` logs to its window.

## Not verified on a console yet

- BadUpdate/XeUnshackle. The launchers' rewritten headers keep a matching digest (SHA-1 at security info + 0x164); only the RSA signature over it is stale, which a patched kernel skips.
- Multi-disc games: each disc's ISO gets its own launcher; swapping discs mid-game is not handled.
- Aurora names the games from its online lookup by Title ID; the launcher has no title name resource.

## Tests

- `python tests\test_server.py` builds `netiso-server` for Linux and checks the NetISO protocol (listing, mounting, reading, new ISOs without a restart, reads past the end) and the push to `tests/aurora_ftp.py`, a stand-in for Aurora's FTP server: folders as Sync makes them, removal, repair, and the category edit (made once, line endings kept, the Lua still valid; an unknown filter left alone).
- `python tests\test_sync.py` builds Sync's core for Linux and runs it against `tests/netiso_server.py`: it writes exactly what the PC tool writes, adds only new ISOs, removes folders of ISOs that are gone (and keeps folders with other files in them), removes nothing when the server lists nothing, skips bad ISOs, keeps clear of folders it didn't make, and rebuilds a launcher that carries another game's IDs.
- `python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the generator: execution info copied exactly and nothing else changed, the ini, FATX-safe unique folder names without release tags, bad ISOs skipped, recursive search and duplicate file names.
- `tests/ui_preview.cpp` draws the loading screen on a PC into a BMP, for each step and error, to review it without a console.
