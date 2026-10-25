NetDisc360 @VERSION@: NetIsoTiles.xex
Play your Xbox 360 games from your PC or NAS, straight from Aurora.
https://github.com/marcusisdahl/NetDisc360

This is only the console's part. NetDisc360 needs three things:

- NetDisc360.exe on your Windows PC, or its Docker image on Linux or a NAS,
  from the GitHub page above. It serves your games.
- NetISO.xex 1.0 by cOz, from ConsoleMods' NetISO page
  (consolemods.org/wiki/Xbox_360:Playing_Games_over_Network_(NetISO)).
  Download Nova+NetISO-Aurora0.7b.zip and take only NetISO.xex from it.
  It isn't included here.
- NetIsoTiles.xex, the file in this folder. It is a DashLaunch plugin, not an
  app: starting it from Aurora does nothing.

To set up the console:

1. Copy NetISO.xex into this folder, next to NetIsoTiles.xex.
2. Make a text file NetISO.xex.txt next to NetISO.xex with one line: your PC's
   IP address, for example 192.168.1.20. NetDisc360's dashboard shows it.
3. Open launch.ini (DashLaunch's settings file, on the root of your USB drive
   or Hdd:) and add both plugins, NetISO.xex first. Use the folder the store
   put this file in; Hdd:\Apps\NetDisc360\ is the usual one. If you use
   UsbNet360, it stays plugin1. DashLaunch loads at most 5 plugins.

     [Plugins]
     plugin1 = Hdd:\Apps\NetDisc360\NetISO.xex
     plugin2 = Hdd:\Apps\NetDisc360\NetIsoTiles.xex

4. In Aurora's settings, turn on the FTP server, and under Content, Scan
   paths, add Hdd1:\NetISO with depth 1. The folder can be empty.
5. Start NetDisc360 on the PC, add your game folders, and reboot the console.
   Within a minute or two your games show up in Aurora. Pick one and play.

Faster loading: a text file NetIsoTiles.ini on the root of your USB drive or
Hdd1: with the two lines read_kb=256 and rcv_kb=64. See Speed on the GitHub
page.

NetDisc360 is MIT licensed (LICENSE.txt). NetISO.xex is cOz's, and
NetIsoTiles.xex only patches it in memory. It isn't affiliated with Microsoft.
