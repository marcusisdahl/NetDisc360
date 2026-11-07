# Sync between computers

NetDisc360 on your PC at home, on a laptop you take along, or on any other
computer, keeps the same **game save backups**, **profile backups** and
**play history**. Each computer keeps its own game folders, consoles and
settings. This small Cloudflare Worker sits in between and keeps the files
in an R2 bucket on your own Cloudflare account:

- Every computer connects out to the Worker over https, so nothing at home is
  opened to the internet, and it works from anywhere.
- A computer syncs when NetDisc360 starts, every 5 minutes, after each save
  backup, and when you press **Sync now**.
- Backups are never replaced or deleted in the cloud. A backup a computer
  deleted (an old one beyond what it keeps) isn't fetched back to it.
- Play history is kept per computer and added up on the Games page.
- Each computer has a key of its own. Remove one (Settings, Sync, Your
  computers) and only that one stops syncing.

It fits Cloudflare's free tier for R2 (10 GB stored, 1 million writes and 10
million reads a month; no fees for downloads). Save and profile backups are
small, so most people never pay. Turning R2 on asks for a payment method.

## Set it up once

1. In the Cloudflare dashboard, open **R2 Object Storage** and turn it on.
2. From this folder, signed in to Cloudflare:

   ```
   npm install
   npx wrangler login
   npx wrangler r2 bucket create netdisc360-sync
   npx wrangler secret put OWNER_KEY
   npx wrangler deploy
   ```

   For `OWNER_KEY`, type a long random password (at least 16 characters)
   and keep it in your password manager. It only connects computers; each
   one gets its own key from it.

   `wrangler deploy` prints the sync's address, such as
   `https://netdisc360-sync.<your-subdomain>.workers.dev`.

3. On each computer, in NetDisc360's dashboard, open **Settings**, **Sync**,
   paste the address and the owner key, and press **Connect**.

Restore a save backup that came from another computer from **Settings,
Backups, Game saves**, on the console you choose.

## Try it on the PC

`npx wrangler dev` runs the Worker at `http://127.0.0.1:8787` with a local
bucket. Put that address in the dashboard instead: NetDisc360 syncs with
plain `http://` addresses too. `npm test` runs the Worker's tests.

## The API

Every request has `Authorization: Bearer <key>`.

| Request | Key | What |
| --- | --- | --- |
| `POST /v1/devices` `{name}` | owner or a device | Connect a device: `{id, key, name}` |
| `GET /v1/devices` | device | The devices |
| `DELETE /v1/devices/<id>` | device | Take a device's key away (not its own) |
| `GET /v1/hello` | device | Who this key is |
| `GET /v1/list?prefix=&after=` | device | Files under `files/` or `state/`: `{items: [{key, size, sha256, device, time}], next}` |
| `GET /v1/object/<key>` | device | A file |
| `PUT /v1/object/<key>` | device | Store a file (`X-Sha256` its SHA-256) |

`files/<kind>/<path>` never change once stored (another file of the same
name is refused with 409); `state/<device>/<name>` is written only by that
device.
