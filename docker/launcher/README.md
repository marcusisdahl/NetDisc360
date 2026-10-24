# Launchers for the Docker image

The Docker build takes the game launchers (`default.xex` and `boot.xex`) from the latest release's `NetISO-Sync.zip`. To use your own build instead, copy `build\NetISO Sync\default.xex` and `build\NetISO Sync\boot.xex` here before `docker build -t ghcr.io/marcusisdahl/netdisc360 .`. They are ignored by git.
