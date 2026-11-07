# Launchers for the Docker image

The Docker build takes the Xbox files it puts on consoles (the game launcher `default.xex` and its helper `boot.xex`, the LAN party app and the plugin) out of the latest release's `NetDisc360.exe`, with `tools/exe_files.py`. To use your own build instead, copy `build\NetISO Sync\default.xex` and `build\NetISO Sync\boot.xex` here before `docker build -t ghcr.io/marcusisdahl/netdisc360 .`. They are ignored by git.
