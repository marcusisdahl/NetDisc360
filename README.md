# NetDisc 360

Play the Xbox 360 games on your PC straight from Aurora's game list. NetDisc 360 is a [NetISO](https://consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)) server for Aurora: every game it serves shows up in Aurora like an installed game, with its name, cover art, title updates and DLC. Pick it and it plays: no file browser, no Mount folder.

- **One small program on the PC.** Double-click `NetDisc.exe`, pick your game folders on its dashboard in your web browser, and forget about it. It serves your games to the console and keeps Aurora's game list up to date by itself, and it can start with Windows. On Linux or a NAS it runs in Docker, with the same dashboard.
- **Any number of consoles.** Several consoles can play from one server at once, each with its own game list in Aurora. The dashboard shows what each one plays and how fast its game streams.
- **ISOs, extracted games and Games on Demand**, from as many folders as you like. Nothing is converted or copied.
- **A loading screen** with progress, B to cancel at any step, and a clear message when something is wrong.
- **The Guide keeps working** in games started this way.

**Contents:** [What you need](#what-you-need) · [Install](#install) · [Docker](#docker) · [Playing](#playing) · [Recommended extras](#recommended-extras) · [Troubleshooting](#troubleshooting) · [Updating](#updating) · [Security](#security) · [The server in detail](#the-server-in-detail) · [Build](#build) · [How it works](#how-it-works)

## What you need

- **A modded Xbox 360** with DashLaunch and Aurora (tested with BadUpdate / XeUnshackle and Aurora 0.7b.2), on the same network as your PC.
- **The NetISO plugin**, `NetISO.xex` 1.0 by cOz. It is not part of this project; get it where you get NetISO. The modified `Nova.xex` from the NetISO kit is not needed.
- **From this project's [Releases](../../releases) page:** `NetIsoTiles.xex` for the console, and `NetDisc.exe` for a Windows 10 or 11 PC (or the Docker image for Linux or a NAS).
- **Your games:** ISOs, extracted game folders (with `default.xex` at the top) or Games on Demand packages.

## Install

### 1. Set up the console

1. **Copy the plugins to the root of your USB drive** (the one DashLaunch reads `launch.ini` from):

   ```
   Usb:\
     launch.ini
     NetISO.xex
     NetISO.xex.txt      <- you make this one in step 2
     NetIsoTiles.xex
     CUsbEnet.xex        <- optional, see Recommended extras
   ```

2. **Tell NetISO where your PC is.** Make a text file `NetISO.xex.txt` next to `NetISO.xex` with one line: your PC's IP address, for example `192.168.1.20`. The server's dashboard shows this address. Give your PC a fixed address in your router, so it doesn't change.

3. **Load the plugins in DashLaunch.** Open `launch.ini` and set its `[Plugins]` section like this (leave the rest of the file as it is):

   ```ini
   [Paths]
   Default = Hdd:\Apps\Aurora\Aurora.xex

   [Plugins]
   plugin1 = Usb:\CUsbEnet.xex
   plugin2 = Usb:\NetISO.xex
   plugin3 = Usb:\NetIsoTiles.xex
   ```

   `Default` is where your Aurora is; this makes the console boot straight into it. Leave out the `CUsbEnet` line if you don't use a USB network adapter, and number the others from `plugin1`. DashLaunch loads at most 5 plugins.

4. **Turn on Aurora's FTP server and add the game folder.** Reboot the console into Aurora. In Aurora's settings, make sure the FTP server is on, and under Content, Scan paths, add `Hdd1:\NetISO` with depth 1 (the folder can be empty; the PC fills it).

   Aurora's FTP login is `xboxftp` / `xboxftp` unless you changed it. Changing it is a good idea (see [Security](#security)); then enter the same login on the PC.

### 2. Start the server

**On Windows:** put `NetDisc.exe` anywhere, for example in the folder with your games, and double-click it. It opens its dashboard in your web browser (`http://127.0.0.1:8360`) and keeps running behind its icon in the notification area, by the clock.

- The first time, it serves the folder it is in, if that folder has games (otherwise the list starts empty). Add your other game folders with **Add a folder**. Subfolders count too. The **Game folders** page lists them; removing one there only stops serving it.
- The **Dashboard** lists the setup steps on a first run and ticks them off as they are done. Then it shows how many games it serves, which consoles are connected, how fast each one's game streams (a chart of the last two minutes), and what happened lately.
- The **Games** page shows every game it serves as covers or a list, with its format, Title ID and size, and which console is playing it. Click a game for its details.
- The **Consoles** page lists every console that has used the server: what it plays, what it was sent, whether its Aurora answers and when its game folders were updated. Choose there which consoles get your games in Aurora (every console that connects, or only the ones you name).
- The **Settings** page has **Aurora's FTP login** (enter it if you changed it in Aurora) and **Start with Windows**, to have the server start, hidden, every time you sign in.
- **Close the browser tab and forget about it.** The server keeps running. Click its icon to open the dashboard again (starting `NetDisc.exe` again does the same); right-click it and pick **Quit NetDisc 360** to stop the server.
- When Windows Firewall asks, allow it on **private networks**.
- **From your phone or another computer:** on the **Settings** page, set a password and turn on **Other devices can open it**. Then open `http://<your PC's address>:8360` there.

Settings are saved in `%APPDATA%\NetDisc 360\netdisc.ini`, with the FTP password encrypted for your Windows user. The **Log** page shows what the server does. The exe also still runs from a command prompt with [options](#the-server-in-detail), with the same dashboard.

**With Docker** (Linux, a NAS, a Raspberry Pi): see [Docker](#docker) below.

### 3. Turn on the console

Turn the console on. Within a minute or two of Aurora starting, the server adds a folder for each game to `Hdd1:\NetISO`, asks Aurora to scan it, and Aurora fetches the names and cover art. Your games are then in Aurora's game list next to your installed games, in the Xbox 360 category too. It doesn't matter whether the PC or the console starts first.

For more consoles, repeat step 1 on each one (their `NetISO.xex.txt` holds the same PC address). Each console gets its own game folders and can play a different game at the same time.

## Docker

The same server runs in Docker on Linux, a NAS or anything else with Docker, with the same dashboard. Put [`docker-compose.yml`](docker-compose.yml) in a folder, set the path to your games in it, then:

```
mkdir config && sudo chown 1000:1000 config
docker compose up -d
```

and open the dashboard at `http://<the machine's address>:8360`. Set a password for it on its **Settings** page (or with `NETDISC_WEB_PASSWORD`): without one, anyone on your network can open it and change its settings. `docker compose logs -f` shows the server's log too.

It uses the image `ghcr.io/marcusisdahl/netdisc-360`, published with each release for x86-64 and ARM64. To build it yourself instead, clone this repository and run `docker compose up -d --build`.

| Setting | Default | |
|---|---|---|
| volume `/games` | | Your games (read-only is fine). Mount more folders under it, like `/games/isos`, or anywhere else and add them on the dashboard. |
| volume `/config` | | The dashboard's settings (`netdisc.ini`). It must be writable by user 1000:1000. |
| `NETDISC_WEB_PASSWORD` | | The dashboard's password. Or `NETDISC_WEB_PASSWORD_FILE` with the path of a Docker secret. Set this way, it can't be changed on the dashboard. |
| `NETDISC_WEB_PORT` | 8360 | The dashboard's port. `0`: no dashboard. |
| `NETDISC_XBOX` | every console that connects | Only these consoles' game folders are kept up to date: IP addresses, separated by commas. Needed on Docker Desktop (Windows, macOS), where the server doesn't see the consoles' own addresses. |
| `NETDISC_FTP_USER` | `xboxftp` | Aurora's FTP login. |
| `NETDISC_FTP_PASSWORD` | `xboxftp` | Or `NETDISC_FTP_PASSWORD_FILE` with the path of a Docker secret. |
| `NETDISC_NO_CATEGORY` | | `true`: leave Aurora's Xbox 360 category alone. |
| `NETDISC_NO_SCAN` | | `true`: don't ask Aurora to scan after changes. |
| `NETDISC_NO_PUSH` | | `true`: only serve games; leave the console alone. |
| `NETDISC_XBOX_FOLDER`, `NETDISC_AURORA` | `/Hdd1/NetISO`, `/Hdd1/Apps/Aurora` | As `--xbox-folder` and `--aurora`. |
| `NETDISC_PORT`, `NETDISC_FTP_PORT` | 4323, 21 | |
| `NETDISC_VERBOSE` | | `true`: log every request. |
| `TZ` | UTC | Your time zone, for the log's times. |

- The variables from `NETDISC_XBOX` down are only the first start's settings. Once the server has saved its settings in `/config`, what's saved there wins, and you change them on the dashboard. (`NETDISC_WEB_PASSWORD`, the ports and `NETDISC_VERBOSE` always apply.)
- The compose file uses **host networking**, so the console reaches the server on port 4323 of the host and the server sees the console's real address. With port mapping instead (`ports:` with `4323:4323` and `8360:8360`, as on Docker Desktop), set `NETDISC_XBOX`.
- The server runs as user 1000:1000. If it can't read your games, set `user:` in the compose file to an ID that can (and give `config` to that ID).
- Without Docker Compose: `docker run -d --name netdisc --network host --restart unless-stopped -v /path/to/games:/games:ro -v /path/to/config:/config -e NETDISC_WEB_PASSWORD=<a password> ghcr.io/marcusisdahl/netdisc-360`.

## Playing

Pick the game's tile. The loading screen shows four steps (Server, Mount, Disc and Start), a bar that fills to 100%, and how long it has been since you pressed the tile. Then the game starts.

- **B cancels at any step** and goes back to Aurora at once.
- Right after a reboot the console can't reach the server for a minute or two, and the NetISO plugin needs a little longer still. For the first 4 minutes after boot the screen waits for both instead of failing.
- When something goes wrong, the screen says what and what to do: the server doesn't answer, the NetISO plugin isn't loaded, the game is no longer on the server, the disc didn't show up, a different game is in the drive, or the game didn't start. **A** goes back to Aurora and **X** tries again; after a minute it goes back to Aurora by itself.

There is no unmount step: the plugin keeps the last game mounted until another one is mounted.

## Recommended extras

Both are separate homebrew projects that work well alongside NetISO.

- **[CUsbEnet](https://github.com/marcusisdahl/CUsbEnet)**: a DashLaunch plugin that lets the Xbox 360 use a USB Ethernet adapter. The console's own network port is 100 Mbit/s, and NetISO reads every game over the network; with a gigabit USB adapter (ASIX AX88179 or Realtek based, such as the TP-Link UE306) FTP to the console has reached about 29 MiB/s. It is `plugin1` in the `launch.ini` above; plug the adapter in instead of the network cable.
- **[FastFTP](https://github.com/marcusisdahl/Xbox360_FastFTP)**: a fast FTP server app for the console, for copying games, saves and files to and from it (about 27 MiB/s, where Aurora's own FTP managed about 19.5 MiB/s on the same console). It uses the same folder names and login as Aurora's FTP. Start it from Aurora when you need it; NetDisc 360 keeps using Aurora's own FTP, which runs whenever Aurora is open.

## Troubleshooting

- **No games appear in Aurora.** Check that Aurora's scan paths include `Hdd1:\NetISO` (depth 1), that Aurora's FTP server is on, and that the FTP login on the dashboard's **Settings** page matches Aurora's. The **Consoles** page says whether each console's Aurora answers, and the **Log** page what the server is waiting for.
- **"The server doesn't answer" on the loading screen, or NetISO lists only "No Server!".** Check the address in `NetISO.xex.txt` against the one the dashboard shows, and that Windows Firewall allows `NetDisc.exe` on private networks. Right after a reboot the console needs a minute or two before it can reach the server.
- **A game has no cover art.** Rename its folder in `Hdd1:\NetISO` (for example over FTP); at the next scan Aurora looks it up again. See [Cover art](#cover-art).
- **"Can't listen on port 4323".** Another NetISO server (`netiso-srv`, `server.exe`, or a second copy of this one) is still running. Close it.
- **A game is listed under Extra copies on the Games page.** The same game is in your folders more than once; the page shows where, so you can remove the copy you don't need.
- **The dashboard doesn't open.** Another program may be using port 8360; the log says so. Start the server with `--web-port` and another port. Opened from another device by a name rather than an address, it says to use the address until it has a password.

## Updating

Replace `NetDisc.exe` with the new one (quit the old one first from its notification-area icon) and start it again; the settings stay. On its next visit to the console it replaces the launchers in `Hdd1:\NetISO` with the new build by itself. Copy the new `NetIsoTiles.xex` over the old one on your USB drive and reboot the console. With Docker: `docker compose pull && docker compose up -d`.

## The server in detail

What `NetDisc.exe` does, all from one file:

- **Serves your games** to the NetISO plugin over the NetISO protocol (TCP port 4323), like `netiso-srv`. New and removed games are picked up without a restart.
- **Keeps `Hdd1:\NetISO` current** over Aurora's FTP. A new game gets its folder once its file has stopped changing for 10 s, named after the game without region, language and revision tags (`Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso` becomes `Banjo-Kazooie - Nuts & Bolts`; tags like `(Disc 2)` stay). A removed game loses its folder, and launchers from an older build are replaced. Only folders that hold nothing but the launcher's own files are ever removed, and nothing is removed when the server has no games.
- **Asks Aurora to scan** after adding or removing games (`SITE INITIATESCAN`, a command of Aurora's FTP server), so new games show up at once with their art.
- **Puts the games in Aurora's Xbox 360 category** (see below).
- **Serves each game once.** A game that is there more than once (the same Title ID and disc, as an ISO, a folder or a package) is served as an ISO before a GOD package before a folder, and the dashboard's **Games** page lists the other copies with their paths so you can remove them.
- **Serves several consoles at once.** Each console plays its own game, and each one's `Hdd1:\NetISO` is kept current on its own: one console being off doesn't hold up another.

| Option | Default | |
|---|---|---|
| `<folder> ...` | the current folder | Folders with your games. |
| `--config <file>` | | Keep the settings in this file. What's saved there (the dashboard's changes) wins over these options. The Windows app uses `%APPDATA%\NetDisc 360\netdisc.ini`, the Docker image `/config/netdisc.ini`. |
| `--web-port <n>` | 8360 | The dashboard's port. `0`: no dashboard. |
| `--web-bind <address>` | 127.0.0.1 | Where the dashboard listens: `127.0.0.1` for this computer only, `0.0.0.0` for the network too (the Docker image's default). Set a password for it on the dashboard or with `NETDISC_WEB_PASSWORD`. |
| `--xbox <address>` | every console that connects | Keep only this console's game folders up to date. Give it once for each console. |
| `--ftp-user`, `--ftp-password` | `xboxftp` | Aurora's FTP login. |
| `--xbox-folder <path>` | `/Hdd1/NetISO` | The game folder on the console, as an FTP path. |
| `--aurora <path>` | `/Hdd1/Apps/Aurora` | Aurora's folder, for the Xbox 360 category. |
| `--no-push` | | Only serve games; leave the console alone. |
| `--no-category` | | Leave Aurora's Xbox 360 category alone. |
| `--no-scan` | | Don't ask Aurora to scan after adding or removing games. |
| `--online-wait <s>` | 120 | After Aurora's FTP comes back, the longest wait for the console to come online before writing game folders (see below). |
| `--remove-wait <s>` | 60 | How long after the server starts (or its game folders change on the dashboard) a game folder whose game is gone stays on the console, so a games folder or drive that isn't there yet (or isn't added on the dashboard yet) doesn't drop its games from Aurora. |
| `--port`, `--ftp-port` | 4323, 21 | |
| `-v` | | Log every request. |

`-r` and `-b` from `netiso-srv` are accepted and ignored. `NetDisc.exe --help` lists every option.

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

- **Change Aurora's FTP login.** Aurora's default login (`xboxftp` / `xboxftp`) lets anyone on your network read and write the console's drives. Set your own in Aurora's settings and enter it on the dashboard's **Settings** page (or `NETDISC_FTP_USER`/`NETDISC_FTP_PASSWORD` in Docker, or `--ftp-user`/`--ftp-password`). FTP sends the login unencrypted, so keep the console on a network you trust. On Windows the settings file keeps the password encrypted for your Windows user; the dashboard never shows it again, and the server never writes it to its log.
- **Keep the NetISO server on your home network.** The NetISO protocol has no login: anything that can reach port 4323 can list and read the games you serve. Don't forward that port from the internet. When Windows Firewall asks, allow `NetDisc.exe` on private networks only. The Docker image runs as an unprivileged user and only needs read access to your games.
- **Give the dashboard a password when other devices can open it.** On Windows only the PC itself can open it until you turn on **Other devices can open it**, which needs a password. In Docker it is open to your network, so set one. The password is kept only as a salted hash; logging in gives the browser a cookie for 30 days, and changing the password logs every browser out. Without a password the dashboard only answers to addresses and local names (like `nas.local`), so a web site can't reach it through your browser, and settings only change from its own page. Don't forward port 8360 from the internet either.
- **Covers on the dashboard.** Your browser downloads them from GitHub (the XboxUnity-Scraper archive), asking only for each game's Title ID. Turn it off under **Settings > Cover art**. The server itself never goes online.
- The server only reads your game folders; it never writes to them. On the console it writes only inside `--xbox-folder` and, unless `--no-category`, the one Aurora script above.

## Limitations

- Original Xbox games (`default.xbe`) are not supported, as NetISO doesn't play them; they are skipped.
- Multi-disc games get one launcher per disc; swapping discs during a game is not handled.
- The launcher has no title name of its own; Aurora names each game from its online lookup by Title ID.
- `NetIsoTiles.xex` patches `NetISO.xex` 1.0 in memory and checks that its code is exactly where it expects; with any other NetISO version it leaves the plugin alone, so the Guide fix is off.

## NetISO Sync (for other NetISO servers)

If your PC runs another NetISO server (`netiso-srv`, `server.exe`) instead of NetDisc 360, NetISO Sync makes the game folders from the console. `build\NetISO Sync\` is the launcher without a game: copied to `Hdd1:\NetISO\NetISO Sync` and started from Aurora, it asks the NetISO server for its ISOs, writes and removes game folders the way `NetDisc.exe` does, and returns to Aurora. It reads ISOs only (no extracted folders or GOD packages, which only `NetDisc.exe` serves) and doesn't change Aurora's category or start a scan.

`server=` in its `netiso.ini` names the server if `netiso.xex.txt` doesn't, and `after=` names what to start when it's done (to run it at every boot as DashLaunch's Default, set `after=` to Aurora's XEX). `tools\netiso_aurora.py` makes the same folders on a PC, for copying by hand.

## Build

You need Visual Studio 2022 with the "Desktop development with C++" workload, and the Xbox 360 SDK integration for Visual Studio (the same setup as [Xbox360-FastFTP](https://github.com/marcusisdahl/Xbox360_FastFTP)), and Python 3. The SDK is not included and can't be redistributed.

```
.\build.ps1 -Tests
```

This makes `build\NetDisc.exe` (with the launcher built in), `build\NetIsoTiles.xex`, `build\NetISO Sync\` and `build\NetISO-Sync.zip` (the launchers, as the release and the Docker image use them). The Docker image builds the server for Linux from the same source; see the [`Dockerfile`](Dockerfile).

To make a release, see [RELEASING.md](RELEASING.md).

## Logs

Each game writes `netiso.log` in its folder, and NetISO Sync in its own: each step and why it stopped. Every run adds to the end of the file, each line starting with the console's time, and a run begins with a `====` line giving the date, the build and how long the console has been on. A file over 512 KB starts over on the next run. NetDisc 360 logs on its dashboard's **Log** page (and to the console when it runs in a command prompt or Docker).

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

- `python tests\test_server.py` builds `netdisc` for Linux and checks the NetISO protocol (listing, mounting, reading, new ISOs without a restart, reads past the end), the push to `tests/aurora_ftp.py`, a stand-in for Aurora's FTP server (folders, removal, repair, and the category edit), and the dashboard's web API (games, consoles and bandwidth, settings kept in the settings file, the password, and changes only from its own page).
- `python tests\test_discs.py` checks extracted folders and GOD packages: every file of a folder's disc reads back as it is on disk, walked as the console walks it; GOD packages in iso2god's and Microsoft's layouts read back across hash groups and `Data` files; duplicates are served once and reported once; original Xbox folders are skipped; a file over 4 GB is refused.
- `python tests\test_sync.py` builds Sync's core for Linux and runs it against `tests/netiso_server.py`.
- `python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the launcher folders made from them.
- `tests/ui_preview.cpp` draws the loading screen on a PC into a BMP, for each step and error.

## Credits

- NetISO and its plugin `NetISO.xex` by cOz. This project doesn't include or modify the plugin's file; `NetIsoTiles.xex` patches it in memory at run time.
- [netiso-srv](https://github.com/tuxuser/netiso-srv) by tuxuser, whose protocol behaviour the server follows.
- [Aurora](https://phoenix.xboxunity.net/) by Phoenix.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT, `third_party/imgui/LICENSE.txt`) and the Roboto font (Apache 2.0, `third_party/fonts/LICENSE-Roboto.txt`) for the loading screen.
- [Lucide](https://lucide.dev) icons (ISC, `third_party/lucide/LICENSE`) for the dashboard. Its covers come from [XboxUnity](https://xboxunity.net), through the [XboxUnity-Scraper](https://github.com/UncreativeXenon/XboxUnity-Scraper) archive.

Xbox and Xbox 360 are trademarks of Microsoft. This project is not affiliated with Microsoft, cOz or Team Phoenix. Use it only with games you own.

## License

MIT, see [LICENSE](LICENSE). The files in `third_party/` keep their own licenses.
