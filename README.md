# NetISO for Aurora

Lists the games on your [NetISO](https://consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)) server in Aurora like installed games, with cover art, title updates and DLC. Pick one and it plays; no file browser, no Mount folder.

Each game gets a small launcher (`default.xex` plus `netiso.ini`) carrying the game's Title ID and Media ID. Aurora scans it as that game. Starting it asks the NetISO plugin to mount the game's ISO from the server, waits until the emulated disc is that game, and boots the disc. The game itself runs from the plugin's emulated DVD drive, so it can stream from the disc as usual.

## What you need

- The NetISO plugin (`NetISO.xex`) loaded by DashLaunch, and a NetISO server (`server.exe`, [netiso-srv](https://github.com/tuxuser/netiso-srv) or Docker) with your ISOs. The modified `Nova.xex` is not needed for the launchers.
- To build the launcher: Visual Studio with the Xbox 360 SDK (the same setup as FastFTP). To make the launchers: Python 3.

## Use

1. Build: `.\build.ps1 -Tests` makes `build\xbox\NetIsoLauncher.xex`.
2. Make the launchers from the folder your server serves:
   `python tools\netiso_aurora.py D:\ISOs D:\NetISO-Aurora` (add `--recursive` if the server runs with `-r`).
3. Copy the output folder to the console, for example to `Hdd1:\NetISO`.
4. In Aurora: Settings, Content, Scan paths, add `Hdd1:\NetISO` with depth 1, and scan.

Run step 2 again after adding ISOs. It rewrites the game folders it makes and leaves everything else alone.

If a game goes back to Aurora instead of starting, read `netiso.log` in its folder. It records each folder the launcher listed and why it stopped.

## How the mount works

The plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its folders lists the server's ISOs (`GetIsoName`), each with a `Mount` folder inside. Listing a `Mount` folder sends `MountIso` with that path; the server strips `\Mount` and backslashes and mounts the ISO whose name ends with the rest.
- After a successful mount the plugin cycles a fake disc tray, and the console reads the ISO as the disc.

The launcher walks those folders by name (root, the ISO, `Mount`) rather than assuming their depth, and then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID before launching it. Its XEX has privilege 0 so the fake tray cycle doesn't close it.

## Not verified on a console yet

- Mounting from the launcher, and launching the disc with `XLaunchNewImage`.
- That the kernel loads the launcher after its execution info header is rewritten. RGH/JTAG kernels accept edited headers in general; BadUpdate/XeUnshackle is untested.
- How Aurora names the games: it takes the Title ID from the header, but the launcher has no title name resource, so names come from Aurora's online lookup.
- Multi-disc games: each disc's ISO gets its own launcher; swapping discs mid-game is not handled.

## Tests

`python tests\test_generator.py` builds small XGD ISOs (XSF, XGD2 and XGD3 layouts) and checks the generator: execution info copied exactly and nothing else changed, the ini, FATX-safe unique folder names, bad ISOs skipped, recursive search and duplicate file names.
