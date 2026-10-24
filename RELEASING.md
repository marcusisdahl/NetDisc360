# Branches

- **`dev`** is where new work goes: every pull request targets `dev`. Each push to `dev` publishes the Docker image `ghcr.io/marcusisdahl/netdisc360:dev` (and `dev-<commit>`), so it can be tested with `NETDISC_TAG=dev` in `.env` before a release. It uses the latest release's launchers.
- **`main`** is the released code: it only changes when `dev` is merged into it for a release (a pull request from `dev` into `main`, merged with a merge commit so the two stay in step), plus an urgent fix now and then, which is then merged back into `dev`.

# Making a release

0. Open a pull request from `dev` into `main`, check CI is green, test the `dev` image and a build from it, and merge it. Steps 1 to 6 happen on `main`.
1. Set the new version in `server/version.h` (`NETDISC_VERSION` and `NETDISC_VERSION_NUMBERS`) and commit it. The tag must be that version with a `v` in front: the installed apps compare it with their own version to see that an update is out, and the Docker workflow stops if the two differ.
2. On the PC with Visual Studio and the Xbox 360 SDK, on the commit to release: `.\build.ps1 -Tests`.
3. Test on a console: the new `NetIsoTiles.xex`, and a game started from its tile with the new `NetDisc360.exe`.
4. On GitHub, draft a new release with a tag like `v1.0.0`, and attach these files from `build\` **before publishing**:
   - `NetDisc360.exe` (exactly this name: the in-app update looks for it)
   - `NetIsoTiles.xex`
   - `NetISO-Sync.zip`
5. Make the repository public (Settings, General, Danger Zone, Change visibility), if it isn't yet. Do this before publishing: the Docker workflow downloads `NetISO-Sync.zip` from the release, which only works once the repository is public.
6. Publish the release. The "Docker image" workflow then builds `ghcr.io/marcusisdahl/netdisc360` for x86-64 and ARM64 with that release's launchers, tagged with the version and `latest`.
7. The first time only: on GitHub, open the package (your profile, Packages, `netdisc360`), Package settings, and set its visibility to **Public**, so `docker compose pull` works without a login.

If the workflow ran before the files were attached, run it again from the Actions tab ("Docker image", Run workflow) with the release's tag.

Write the release notes for people updating: the dashboard shows them under **Settings > Updates** before they install. Say when `NetIsoTiles.xex` changed, since that one is copied to the USB drive by hand. Mark a test version as a pre-release: only those who turn on **Include test versions** are offered it.
