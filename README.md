# NetISO for Aurora

Play the Xbox 360 games on your PC straight from Aurora's game list. Every game on your [NetISO](https://consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)) server shows up in Aurora like an installed game, with its name, cover art, title updates and DLC. Pick it and it plays: no file browser, no Mount folder.

- **One program on the PC.** `netiso-server.exe` replaces `netiso-srv` / `server.exe`. It serves your games to the console and keeps Aurora's game list up to date by itself.
- **ISOs, extracted games and Games on Demand**, from as many folders as you like. Nothing is converted or copied.
- **A loading screen** with progress, B to cancel at any step, and a clear message when something is wrong.
- **The Guide keeps working** in games started this way.

## What you need

On the console:
- A modded Xbox 360 with DashLaunch and Aurora (tested with BadUpdate / XeUnshackle and Aurora 0.7b.2).
- The NetISO plugin, `NetISO.xex` 1.0 by cOz, loaded by DashLaunch. It is not part of this project; get it where you got NetISO. The modified `Nova.xex` from the NetISO kit is not needed.
- `NetIsoTiles.xex` from this project, loaded by DashLaunch next to `NetISO.xex`.
- Aurora's FTP server turned on.

On the PC:
- Windows 10 or 11 (64-bit), on the same network as the console.
- Your games: ISOs, extracted game folders (with `default.xex` at the top) or Games on Demand packages.

## Install

1. Download `netiso-server.exe` and `NetIsoTiles.xex` from the [Releases](../../releases) page (or build them, below).
2. Copy `NetIsoTiles.xex` to the console and add it to DashLaunch's `launch.ini` as a plugin, next to `NetISO.xex`. DashLaunch loads at most 5 plugins. Reboot the console.
3. Put the PC's IP address in `Hdd1:\netiso.xex.txt` (or `Usb0:\netiso.xex.txt`), as the NetISO plugin needs anyway.
4. In Aurora: Settings, Content, Scan paths, add `Hdd1:\NetISO` with depth 1.
5. On the PC, run `netiso-server.exe` with your game folders, for example:

   ```
   netiso-server.exe "D:\Xbox 360\ISOs" "D:\Xbox 360\Games"
   ```

   With no folder it serves the folder it runs in. Subfolders count too. If you changed Aurora's FTP login (recommended, see [Security](#security)), add `--ftp-user` and `--ftp-password`.

That's all. Once the console is on, the server makes a small launcher folder for each game in `Hdd1:\NetISO` and asks Aurora to scan it, and Aurora looks up the game's name and art online. It doesn't matter whether the PC or the console starts first. The server only logs in to the console when it has something to do (at start, when your games change, or to retry a scan Aurora was too busy for), and while the console is off it tries every 15 s.

## Playing

Pick the game's tile. The loading screen shows four steps (Server, Mount, Disc and Start), a bar that fills to 100%, and how long it has been since you pressed the tile. Then the game starts.

- **B cancels at any step** and goes back to Aurora at once.
- Right after a reboot the console can't reach the server for a minute or two, and the NetISO plugin needs a little longer still. For the first 4 minutes after boot the screen waits for both instead of failing.
- When something goes wrong, the screen says what and what to do: the server doesn't answer, the NetISO plugin isn't loaded, the game is no longer on the server, the disc didn't show up, a different game is in the drive, or the game didn't start. **A** goes back to Aurora and **X** tries again; after a minute it goes back to Aurora by itself.

There is no unmount step: the plugin keeps the last game mounted until another one is mounted.

## The server

What `netiso-server.exe` does, all from one file:

- **Serves your games** to the NetISO plugin over the NetISO protocol (TCP port 4323), like `netiso-srv`. New and removed games are picked up without a restart.
- **Keeps `Hdd1:\NetISO` current** over Aurora's FTP. A new game gets its folder once its file has stopped changing for 10 s, named after the game without region, language and revision tags (`Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso` becomes `Banjo-Kazooie - Nuts & Bolts`; tags like `(Disc 2)` stay). A removed game loses its folder, and launchers from an older build are replaced. Only folders that hold nothing but the launcher's own files are ever removed, and nothing is removed when the server has no games.
- **Asks Aurora to scan** after adding or removing games (`SITE INITIATESCAN`, a command of Aurora's FTP server), so new games show up at once with their art.
- **Puts the games in Aurora's Xbox 360 category** (see below).
- **Serves each game once.** A game that is there more than once (the same Title ID and disc, as an ISO, a folder or a package) is served as an ISO before a GOD package before a folder, and the server window lists the other copies with their paths so you can remove them.

| Option | Default | |
|---|---|---|
| `<folder> ...` | the current folder | Folders with your games. |
| `--xbox <address>` | the console that connects | The console to update. |
| `--ftp-user`, `--ftp-password` | `xboxftp` | Aurora's FTP login. |
| `--xbox-folder <path>` | `/Hdd1/NetISO` | The game folder on the console, as an FTP path. |
| `--aurora <path>` | `/Hdd1/Apps/Aurora` | Aurora's folder, for the Xbox 360 category. |
| `--no-push` | | Only serve games; leave the console alone. |
| `--no-category` | | Leave Aurora's Xbox 360 category alone. |
| `--no-scan` | | Don't ask Aurora to scan after adding or removing games. |
| `--online-wait <s>` | 120 | After Aurora's FTP comes back, the longest wait for the console to come online before writing game folders (see below). |
| `--port`, `--ftp-port` | 4323, 21 | |
| `-v` | | Log every request. |

`-r` and `-b` from `netiso-srv` are accepted and ignored. `netiso-server.exe --help` lists every option.

### Cover art

Aurora looks a game up online (name and art) only once, when its scan first finds it, and a game it finds while the console is still coming online keeps its folder name and gets no art. Aurora's FTP server answers a minute or two before the console can reach out after a boot, so when Aurora's FTP comes back the server writes new game folders once the console has connected to it (the NetISO plugin does that as soon as it can), or after 2 minutes at most (`--online-wait`). After a game played over NetISO there is no wait. A game that has no art anyway can be refreshed in Aurora, or its folder in `Hdd1:\NetISO` renamed: at the next scan Aurora drops the old entry and looks the game up again. The server keeps a renamed folder, since it matches folders to games by their `netiso.ini`.

### Aurora's Xbox 360 category

Aurora files the launchers under Unsigned, because they aren't retail-signed, so its Xbox 360 category doesn't show them. The server edits `Media\Scripts\Content\FilterFunctions.lua` in Aurora's folder once, so that the category also lists games in the NetISO folder, and keeps the original next to it as `FilterFunctions.lua.before-netiso`. If an Aurora update puts the original back, the server makes the same edit again. A filter it doesn't recognise is left alone, and `--no-category` turns this off.

### Extracted games and Games on Demand

Besides ISOs, the server serves Xbox 360 games extracted to a folder (`default.xex` at the top) and Games on Demand packages (`<Title ID>\00007000\<package>` with its `.data` folder). Nothing is converted or written next to them: the NetISO plugin only reads a disc as sectors, so the server answers with the disc image the folder or package came from, put together as it's read.

- A folder is listed as `<folder name>.iso`, a package as `<game folder>.iso` (or the package's own title when it isn't in a folder of its own). A `Disc 2` folder with its own `default.xex` inside a game's folder is a disc of its own, `<game> - Disc 2.iso`. A name that is already taken gets a number.
- An extracted game is a GDF (XDVDFS) disc built in memory when it's mounted. A disc can't hold a file of 4 GB or more.
- A GOD package is the disc's game partition cut into `Data0000`, `Data0001`, ... with hash blocks between; reads skip them.
- A folder given on the command line is never taken as a game itself, even with a `default.xex` at its top; only what is inside it counts.

## Security

- **Change Aurora's FTP login.** Aurora's default login (`xboxftp` / `xboxftp`) lets anyone on your network read and write the console's drives. Set your own in Aurora's settings and pass it with `--ftp-user` and `--ftp-password`. FTP sends the login unencrypted, so keep the console on a network you trust. The server never writes the password to its window or logs.
- **Keep the NetISO server on your home network.** The NetISO protocol has no login: anything that can reach port 4323 can list and read the games you serve. Don't forward that port from the internet. When Windows Firewall asks, allow `netiso-server.exe` on private networks only.
- The server only reads your game folders; it never writes to them. On the console it writes only inside `--xbox-folder` and, unless `--no-category`, the one Aurora script above.

## Limitations

- Original Xbox games (`default.xbe`) are not supported, as NetISO doesn't play them; they are skipped.
- Multi-disc games get one launcher per disc; swapping discs during a game is not handled.
- The launcher has no title name of its own; Aurora names each game from its online lookup by Title ID.
- `NetIsoTiles.xex` patches `NetISO.xex` 1.0 in memory and checks that its code is exactly where it expects; with any other NetISO version it leaves the plugin alone, so the Guide fix is off.

## NetISO Sync (for other NetISO servers)

If your PC runs another NetISO server (`netiso-srv`, `server.exe`, Docker) instead of `netiso-server.exe`, NetISO Sync makes the game folders from the console. `build\NetISO Sync\` is the launcher without a game: copied to `Hdd1:\NetISO\NetISO Sync` and started from Aurora, it asks the NetISO server for its ISOs, writes and removes game folders the way `netiso-server.exe` does, and returns to Aurora. It reads ISOs only (no extracted folders or GOD packages, which only `netiso-server.exe` serves) and doesn't change Aurora's category or start a scan.

`server=` in its `netiso.ini` names the server if `netiso.xex.txt` doesn't, and `after=` names what to start when it's done (to run it at every boot as DashLaunch's Default, set `after=` to Aurora's XEX). `tools\netiso_aurora.py` makes the same folders on a PC, for copying by hand.

## Build

You need Visual Studio 2022 with the "Desktop development with C++" workload, and the Xbox 360 SDK integration for Visual Studio (the same setup as [Xbox360-FastFTP](https://github.com/marcusisdahl/Xbox360-FastFTP)), and Python 3. The SDK is not included and can't be redistributed.

```
.\build.ps1 -Tests
```

This makes `build\netiso-server.exe` (with the launcher built in), `build\NetIsoTiles.xex` and `build\NetISO Sync\`.

## Logs

Each game writes `netiso.log` in its folder, and NetISO Sync in its own: each step and why it stopped. Every run adds to the end of the file, each line starting with the console's time, and a run begins with a `====` line giving the date, the build and how long the console has been on. A file over 512 KB starts over on the next run. `netiso-server.exe` logs to its window.

### Troubleshooting switches

These go in a game folder's `netiso.ini` and help find out what breaks on a console.

- `stop_after=link|root|iso` stops short of the mount and goes back to Aurora: after linking the NetISO drive, listing its root, or listing the game's folder.
- `quiet_ms=` sets how long the launcher leaves the disc alone after the mount (5000).
- `hold_ms=` sets how long the full bar stays up before the game starts (1000).
- `mount_only=1` mounts the game and goes back to Aurora without starting it.
- `start=aurora|nova|media` picks how the disc is started: as Aurora's disc entry does (the default), as Nova's file browser does, or whatever disc is in the drive, as the dashboard does. If the chosen start fails, the launcher tries `nova`.
- `tiles=aurora` has `NetIsoTiles.xex` mount from inside Aurora instead, the older way, with no loading screen and no cancel. With it, `hold_mount=1`, `relist=1`, `tiles_stop=server|disc|root|iso|mount` and `tiles_thread=system` change how the plugin mounts.

## How it works

Each game's folder in `Hdd1:\NetISO` holds `default.xex`, `boot.xex` and `netiso.ini`. `default.xex` carries the game's Title ID and Media ID, so Aurora scans it as that game. It hands over to `boot.xex` next to it (the same program without a Title ID), which draws the loading screen, asks the NetISO plugin to mount the game, waits until the emulated disc is that game, and boots the disc the way Aurora's own disc entry does. The game runs from the plugin's emulated DVD drive.

### The mount

The NetISO plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its root lists the server's ISOs, each with a `Mount` folder inside, plus `[Disable Current ISO]` while one is mounted. Listing a `Mount` folder sends the server a mount request for that ISO.
- After a successful mount it swaps the disc and fakes a tray open and close on its own thread, after which the console reads the ISO as the disc.
- Right after every mount request it calls `XamAppUnloadSelf` on the thread that asked.

The launcher walks those folders by name (root, the ISO, `Mount`), leaves the disc alone for 5 s, then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID. It starts the game with `XLaunchNewImage("Dvd:\default.xex")` and `Dvd:` linked to `\Device\Cdrom0`, as Aurora 0.7b.2's disc entry does.

### The Guide

`XamAppUnloadSelf` is what killed the Guide. From the Guide's own file browser that call just closes the Guide, but a mount asked from anywhere else left the Guide dead until reboot. At start-up `NetIsoTiles.xex` points NetISO's import of that call at its own function, which skips it unless the Guide's browser asked, after checking that NetISO 1.0's code is where it was read.

The launcher also starts the game the way Aurora's disc entry does (`XamLoaderLaunchTitleEx`, as Nova's browser starts files, left the Guide dead), stays off the disc for 5 s after the mount (reading it during the fake tray cycle left the Guide dead), and keeps drawing frames until it launches (a title that never drew left the Guide dead too).

`NetIsoTiles.xex` also hooks xam's two launch calls (ordinals 420 and 421, which Aurora starts tiles with). By default it leaves tiles to their launcher.

## Tests

- `python tests\test_server.py` builds `netiso-server` for Linux and checks the NetISO protocol (listing, mounting, reading, new ISOs without a restart, reads past the end) and the push to `tests/aurora_ftp.py`, a stand-in for Aurora's FTP server: folders, removal, repair, and the category edit.
- `python tests\test_discs.py` checks extracted folders and GOD packages: every file of a folder's disc reads back as it is on disk, walked as the console walks it; GOD packages in iso2god's and Microsoft's layouts read back across hash groups and `Data` files; duplicates are served once and reported once; original Xbox folders are skipped; a file over 4 GB is refused.
- `python tests\test_sync.py` builds Sync's core for Linux and runs it against `tests/netiso_server.py`.
- `python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the launcher folders made from them.
- `tests/ui_preview.cpp` draws the loading screen on a PC into a BMP, for each step and error.

## Credits

- NetISO and its plugin `NetISO.xex` by cOz. This project doesn't include or modify the plugin's file; `NetIsoTiles.xex` patches it in memory at run time.
- [netiso-srv](https://github.com/tuxuser/netiso-srv) by tuxuser, whose protocol behaviour the server follows.
- [Aurora](https://phoenix.xboxunity.net/) by Phoenix.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT, `third_party/imgui/LICENSE.txt`) and the Roboto font (Apache 2.0, `third_party/fonts/LICENSE-Roboto.txt`) for the loading screen.

Xbox and Xbox 360 are trademarks of Microsoft. This project is not affiliated with Microsoft, cOz or Team Phoenix. Use it only with games you own.

## License

MIT, see [LICENSE](LICENSE). The files in `third_party/` keep their own licenses.
