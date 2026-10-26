# How NetDisc360 works

What happens on the console when you pick a game, and what `NetIsoTiles.xex` changes in the NetISO plugin. For setting it up, see the [README](../README.md).

## The launcher

Each game's folder in `Hdd1:\NetISO` holds `default.xex` and `netiso.ini`. `default.xex` is a small tile launcher (about 130 KB) that carries the game's Title ID and Media ID, so Aurora scans it as that game. It hands over to the helper all the games share, `Hdd1:\NetISO\NetDisc360\boot.xex` (the full launcher without a Title ID, which Aurora doesn't list), telling it which game folder it came from. The helper draws the loading screen, asks the NetISO plugin to mount the game, waits until the emulated disc is that game, and boots the disc the way Aurora's own disc entry does. The game runs from the plugin's emulated DVD drive.

## The mount

The NetISO plugin has no published source. This is what `NetISO.xex` 1.0 does, read from the binary:

- It registers the file system device `\Device\NetIso` and hooks `\Device\Cdrom0`.
- Listing its root lists the server's ISOs, each with a `Mount` folder inside, plus `[Disable Current ISO]` while one is mounted. Listing a `Mount` folder sends the server a mount request for that ISO.
- After a successful mount it swaps the disc and fakes a tray open and close on its own thread, after which the console reads the ISO as the disc.
- Right after every mount request it calls `XamAppUnloadSelf` on the thread that asked.

The launcher walks those folders by name (root, the ISO, `Mount`), leaves the disc alone for 5 s, then checks `\Device\Cdrom0\default.xex` for the expected Title ID and Media ID. It starts the game with `XLaunchNewImage("Dvd:\default.xex")` and `Dvd:` linked to `\Device\Cdrom0`, as Aurora 0.7b.2's disc entry does.

## The Guide

`XamAppUnloadSelf` is what killed the Guide. From the Guide's own file browser that call just closes the Guide, but a mount asked from anywhere else left the Guide dead until reboot. At start-up `NetIsoTiles.xex` points NetISO's import of that call at its own function, which skips it unless the Guide's browser asked, after checking that NetISO 1.0's code is where it was read.

The launcher also starts the game the way Aurora's disc entry does (`XamLoaderLaunchTitleEx`, as Nova's browser starts files, left the Guide dead), stays off the disc for 5 s after the mount (reading it during the fake tray cycle left the Guide dead), and keeps drawing frames until it launches (a title that never drew left the Guide dead too).

`NetIsoTiles.xex` also hooks xam's two launch calls (ordinals 420 and 421, which Aurora starts tiles with). By default it leaves tiles to their launcher.

## Disc swaps

A game asks for another of its discs with `XSwapDisc` (xam.xex ordinal 2600): the disc number, and a kernel event to set once that disc is in. The NetISO kit's `Nova.xex` points every game's import of that call at its own swap. For a game whose disc is 1 of 2 or more, that links the game's `GAME:` and `D:` to the other disc's folder; a tile's folder holds only its launcher, so GTA V hung at "Loading story mode" after its install disc asked for the play disc. For a single-disc tile, Nova's swap fails, and its failure path crashes the game.

So when a game starts from a tile, `NetIsoTiles.xex` finds its other discs: the tile folders next to its own with the same Title ID in `netiso.ini`, numbered by their launcher's execution info, as Aurora numbers them. Once the game is loaded, and after Nova's change, the plugin points the game's import of `XSwapDisc` at its own function, which puts in the disc the game asked for. NetISO's mount opens and closes the tray, and a tray opened under a running title sends the console to the dashboard, so the function first calls xam's real `XSwapDisc`, which shows "insert disc 2" and lets the title through a disc change. Then it mounts the other disc's ISO through NetISO and waits until the drive has it. xam tells the game the disc is in; if it hasn't within 15 s, the plugin sets the event itself.

If the console still heads for the dashboard within 30 s of a swap, the plugin starts `Dvd:\default.xex` instead, so the console boots the disc just put in. A Guide notice says when the game is ready to swap and when the other disc goes in, and the swap's lines go to the game's `netiso.log`. Tested on a console with GTA V, whose install disc asks for the play disc when it has finished.

## Read speed

`NetISO.xex` 1.0 splits every disc read into 4 KB requests and sends the next only when the last one's data is in, so each 4 KB costs a round trip (about 0.6 ms through a USB adapter). It receives straight into the caller's buffer, so the limit is only a constant: at start-up, before NetISO has a disc to read, `NetIsoTiles.xex` rewrites the three instructions that clamp a request to 4 KB to clamp it to `read_kb` instead. NetDisc360 answers a request of up to 16 MB in one go.

NetISO sets one option on its socket before connecting and leaves the receive buffer at XNet's default, which caps how much the server can have in flight. The plugin points that one `setsockopt` call at its own function, which also asks for `rcv_kb` KB with `SO_RCVBUF` on each connection NetISO makes. Both patches check NetISO 1.0's code first, like the Guide fix.

## Logs

Each game writes `netiso.log` in its folder, and NetISO Sync in its own: each step and why it stopped. Every run adds to the end of the file, each line starting with the console's time, and a run begins with a `====` line giving the date, the build and how long the console has been on. A file over 512 KB starts over on the next run. NetDisc360 logs on its dashboard's **Log** page (and to the console when it runs in a command prompt or Docker).

## Troubleshooting switches

These go in a game folder's `netiso.ini` and help find out what breaks on a console.

- `stop_after=link|root|iso` stops short of the mount and goes back to Aurora: after linking the NetISO drive, listing its root, or listing the game's folder.
- `quiet_ms=` sets how long the launcher leaves the disc alone after the mount (5000).
- `hold_ms=` sets how long the full bar stays up before the game starts (1000).
- `mount_only=1` mounts the game and goes back to Aurora without starting it.
- `start=aurora|nova|media` picks how the disc is started: as Aurora's disc entry does (the default), as Nova's file browser does, or whatever disc is in the drive, as the dashboard does. If the chosen start fails, the launcher tries `nova`.
- `tiles=aurora` has `NetIsoTiles.xex` mount from inside Aurora instead, the older way, with no loading screen and no cancel.
