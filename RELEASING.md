# Making a release

1. On the PC with Visual Studio and the Xbox 360 SDK, on the commit to release: `.\build.ps1 -Tests`.
2. Test on a console: the new `NetIsoTiles.xex`, and a game started from its tile with the new `netiso-server.exe`.
3. On GitHub, draft a new release with a tag like `v1.0.0`, and attach these files from `build\` **before publishing**:
   - `netiso-server.exe`
   - `NetIsoTiles.xex`
   - `NetISO-Sync.zip`
4. Publish the release. The "Docker image" workflow then builds `ghcr.io/marcusisdahl/netiso-aurora` for x86-64 and ARM64 with that release's launchers, tagged with the version and `latest`.
5. The first time only: on GitHub, open the package (your profile, Packages, `netiso-aurora`), Package settings, and set its visibility to **Public**, so `docker compose pull` works without a login.

If the workflow ran before the files were attached, run it again from the Actions tab ("Docker image", Run workflow) with the release's tag.
