# Party relay (Join from a phone)

Guests at a LAN party open a page on their phone to join the party and vote
for the next game. A phone on the internet can't reach NetDisc360 on the
LAN, so this small Cloudflare Worker sits in between:

- NetDisc360 posts the party (names, teams, the vote, points, the live feed;
  no addresses) to the relay every 3 seconds while a party is on, and takes
  back what phones asked for. It always connects out, so nothing on the LAN
  is opened to the internet.
- Phones open `https://<relay>/<CODE>` (the QR code on the big screen) and
  read the party from the relay. Joining or voting queues an ask that
  NetDisc360 picks up on its next post, and the answer ("Welcome back, Ann!")
  goes back to that phone.
- Each party gets a new 6-character code. The first NetDisc360 to post a code
  claims it with its own key, so no one else can post to that room. The relay
  forgets a room 12 hours after NetDisc360 last posted to it.

It runs on Cloudflare's free plan: one Worker with a Durable Object per party
code, and the phone page as a static asset.

## Deploy

From this folder, signed in to Cloudflare:

```
npm install
npx wrangler login
npx wrangler deploy
```

`wrangler deploy` prints the relay's address, such as
`https://netdisc360-party.<your-subdomain>.workers.dev`. In NetDisc360's
dashboard, open **LAN party**, then **Join from a phone** (under Start on
all), paste that address and press **Turn on**. The code and its QR code
show there and on the big screen.

## Try it on the PC

`npx wrangler dev` runs the relay at `http://127.0.0.1:8787`. Put that address
in the dashboard instead: NetDisc360 posts to plain `http://` addresses too.

## The API

| Request | Who | What |
| --- | --- | --- |
| `GET /api/room/CODE?phone=ID` | phone | The party, and the answer to this phone's last ask |
| `POST /api/room/CODE/join` `{phone, name}` | phone | Ask to join with a name |
| `POST /api/room/CODE/vote` `{phone, vote, game}` | phone | Vote for game (an index of the vote's games; -1 takes it back) |
| `POST /api/room/CODE/host` `{party, answers}` | NetDisc360 (`Authorization: Bearer KEY`) | Post the party; the answer has the asks since the last post |
