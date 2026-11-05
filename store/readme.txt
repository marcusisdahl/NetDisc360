NetDisc360 @VERSION@: NetDisc360.xex
Play your Xbox 360 games from your PC or NAS, straight from Aurora.
https://github.com/marcusisdahl/NetDisc360

This is only the console's part. NetDisc360 needs two things:

- NetDisc360.exe on your Windows PC, or its Docker image on Linux or a NAS,
  from the GitHub page above. It serves your games.
- NetDisc360.xex, the file in this folder: the console's disc drive for games
  on the network, which also starts them from their Aurora tiles. It is a
  DashLaunch plugin, not an app: starting it from Aurora does nothing.
  NetISO.xex isn't needed. If you had it in launch.ini, take its line out.

To set up the console:

1. Make a text file NetDisc360.ini next to NetDisc360.xex with the line
   server= and your PC's IP address, for example server=192.168.1.20.
   NetDisc360's dashboard shows it, and its setup wizard can make this file.
2. Open launch.ini (DashLaunch's settings file, on the root of your USB drive
   or Hdd:) and add the plugin. Use the folder the store put this file in;
   Hdd:\Apps\NetDisc360\ is the usual one. If you use NetUsb360, it stays
   plugin1. DashLaunch loads at most 5 plugins.

     [Plugins]
     plugin1 = Hdd:\Apps\NetDisc360\NetDisc360.xex

3. In Aurora's settings, turn on the FTP server, and under Content, Scan
   paths, add Hdd1:\NetDisc360\Games with depth 1. The folder can be empty.
   Keep Aurora's LiNK off.
4. Start NetDisc360 on the PC, add your game folders, and reboot the console.
   Within a minute or two your games show up in Aurora. Pick one and play.

Faster loading: the lines read_kb=256 and rcv_kb=64 in NetDisc360.ini. See
Speed on the GitHub page.

NetDisc360 is MIT licensed (LICENSE.txt). It isn't affiliated with Microsoft.
