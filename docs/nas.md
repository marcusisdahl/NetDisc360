# NetDisc360 on a NAS

NetDisc360's Docker image runs on any NAS that runs Docker containers. A NAS is where big game libraries usually live, and the server only reads the games, so it can serve them straight from your shares. These steps use the published image, `ghcr.io/marcusisdahl/netdisc360`, with **host networking**: the consoles connect on port 4323, and the dashboard is on port 8360. [The README's Docker section](../README.md#docker) explains each setting.

Whatever the NAS, set a **dashboard password**, and keep the settings folder private: on Linux the saved Aurora FTP password is kept there as plain text.

## Unraid

1. **Docker** tab, **Add Container**.
2. In **Template**, paste this address and press Enter (or copy [`docker/unraid/netdisc360.xml`](../docker/unraid/netdisc360.xml) to `/boot/config/plugins/dockerMan/templates-user/` and pick it):

   ```
   https://raw.githubusercontent.com/marcusisdahl/NetDisc360/main/docker/unraid/netdisc360.xml
   ```

3. Set **Games** to the share with your games (it is mounted read-only), and a **Dashboard password**. Change the Aurora FTP login if you changed it in Aurora.
4. **Apply**, then open the dashboard from the container's icon (**WebUI**).

The template runs the container as `nobody:users` (99:100), the owner of Unraid's shares and of `appdata`, so it can read your games and save its settings without a `chown`.

## TrueNAS SCALE (24.10 and later)

1. **Apps**, **Discover Apps**, the menu at the top right, **Install via YAML**.
2. Name it `netdisc360` and paste this, with your own paths and password:

   ```yaml
   services:
     netdisc360:
       image: ghcr.io/marcusisdahl/netdisc360:latest
       network_mode: host
       user: "568:568"          # TrueNAS's apps user; it needs to read the games dataset
       environment:
         - TZ=Europe/Oslo
         - NETDISC_WEB_PASSWORD=a password of your own
         - NETDISC_FTP_USER=xboxftp
         - NETDISC_FTP_PASSWORD=xboxftp
       volumes:
         - /mnt/tank/games/xbox360:/games:ro
         - /mnt/tank/apps/netdisc360:/config
       restart: unless-stopped
   ```

3. Make the `/config` dataset (here `/mnt/tank/apps/netdisc360`) first and give it to the `apps` user (568), or the dashboard can't keep its settings.
4. **Save**. The dashboard is at `http://<your NAS>:8360`.

## Synology (Container Manager)

1. Make two folders, for example `/volume1/docker/netdisc360` (the settings) and keep your games where they are, say `/volume1/games/xbox360`.
2. Put [`docker-compose.yml`](../docker-compose.yml) and `.env` (from [`.env.example`](../.env.example)) in `/volume1/docker/netdisc360`, with `NETDISC_GAMES_DIR=/volume1/games/xbox360` and `NETDISC_CONFIG_DIR=./config` in `.env`.
3. **Container Manager**, **Project**, **Create**: pick that folder as the path and use the `docker-compose.yml` that is there.
4. The container runs as user 1000:1000. Give the `config` folder to that ID over SSH (`sudo chown 1000:1000 /volume1/docker/netdisc360/config`), and make sure it can read the games folder (or add `user: "<uid>:<gid>"` of a user that can to the service).

## Updating

Pull the new image and recreate the container: **Check for Updates**, then **Apply Update** on Unraid; **Update** on TrueNAS; **Action**, **Build** on Synology (or `docker compose pull && docker compose up -d`). The dashboard says when a new version is out.
