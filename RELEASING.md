# Branches

- **`dev`** is where new work goes: every pull request targets `dev`, never `main` directly. Each push to `dev` publishes the Docker image `ghcr.io/marcusisdahl/netdisc360:dev` (and `dev-<commit>`), so it can be tested with `NETDISC_TAG=dev` in `.env` before a release. It uses the dev launchers when there are some (below), else the latest release's.
- **`main`** is the released code: it only changes when `dev` is merged into it for a release (a pull request from `dev` into `main`, merged with a merge commit so the two stay in step), plus an urgent fix now and then, which is then merged back into `dev`.

# Dev launchers for the dev Docker image

The Xbox launchers can only be built on the PC with the Xbox 360 SDK, so CI can't make them. When a change on `dev` touches the launchers (`launcher/`, `shared/`), build them with `.\build.ps1` and put `build\NetISO-Sync.zip` on the GitHub pre-release with the tag **`dev-launchers`** (create it once as a pre-release; after that, delete its old `NetISO-Sync.zip` and attach the new one). The next push to `dev` (or a rerun of the "Docker image" workflow) builds the `dev` image with them. The in-app update and the dashboard ignore that release, as its tag isn't a version.

# Making a release

0. On `dev`, the README's Quick install downloads `docker-compose.yml` and `.env.example` from the `dev` branch (`raw.githubusercontent.com/marcusisdahl/NetDisc360/dev/...`). In the pull request from `dev` into `main`, change those two links to `main`, check CI is green, test the `dev` image and a build from it, and merge it; then merge `main` back into `dev` and set the links to `dev` again. Steps 1 to 6 happen on `main`.
1. Set the new version in `server/version.h` (`NETDISC_VERSION` and `NETDISC_VERSION_NUMBERS`) and commit it: on `dev` it carries `-dev` (`1.1.0-dev`), which comes off for the release (`1.1.0`). After the release, set `dev` to the next one (`1.2.0-dev`). The tag must be that version with a `v` in front: the installed apps compare it with their own version to see that an update is out, and the Docker workflow stops if the two differ.
2. On the PC with Visual Studio and the Xbox 360 SDK, on the commit to release: `.\build.ps1 -Tests`, then `.\store\package.ps1` (needs 7-Zip) for the Xbox 360 Homebrew Store files.
3. Test on a console: the new `NetDisc360.xex` (without NetISO.xex in launch.ini), and a game started from its tile with the new `NetDisc360.exe`.
4. On GitHub, draft a new release with a tag like `v1.0.0`, and attach these files from `build\` **before publishing**:
   - `NetDisc360.exe` (exactly this name: the in-app update looks for it)
   - `NetDisc360.xex` (the console plugin; `NetIsoTiles.xex` up to 1.0)
   - `NetISO-Sync.zip`
   - `NetDisc360.exe.sig`, once `NETDISC_UPDATE_KEY` is set in `server/version.h`: `python tools\sign_release.py sign <your key.pem> build\NetDisc360.exe` makes it. Builds with a key install an update only when its signature matches, so a release without it can't be installed from the app. To start signing: `python tools\sign_release.py new-key <somewhere safe, outside the repo>\netdisc-update.pem`, put the printed key in `version.h`, and release; from that version on, sign every release.
   - `NetDisc360-<version>.7z` (the store package: `NetDisc360.xex`, its `readme.txt` and the license)
5. Make the repository public (Settings, General, Danger Zone, Change visibility), if it isn't yet. Do this before publishing: the Docker workflow downloads `NetISO-Sync.zip` from the release, which only works once the repository is public.
6. Publish the release. The "Docker image" workflow then builds `ghcr.io/marcusisdahl/netdisc360` for x86-64 and ARM64 with that release's launchers, tagged with the version and `latest`.
7. The first time only: on GitHub, open the package (your profile, Packages, `netdisc360`), Package settings, and set its visibility to **Public**, so `docker compose pull` works without a login.

When `NetDisc360.xex` changed, send `build\NetDisc360-<version>.ini` to whoever keeps the Homebrew Store repo listing it, so the store offers the new plugin. Its `dataurl` points at the 7z on this release; the text comes from [`store/consolemods.ini`](store/consolemods.ini), and the 7z's readme from [`store/readme.txt`](store/readme.txt).

If the workflow ran before the files were attached, run it again from the Actions tab ("Docker image", Run workflow) with the release's tag.

Write the release notes for people updating: the dashboard shows them under **Settings > Updates** before they install. Say when `NetDisc360.xex` changed, since that one is copied to the console by hand. Mark a test version as a pre-release: only those who turn on **Include test versions** are offered it.
