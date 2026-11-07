// SPDX-License-Identifier: MIT
// NetDisc360's party relay on Cloudflare: guests join the LAN party and vote
// from their phones. A phone on the internet can't reach NetDisc360 on the
// LAN, so NetDisc360 posts the party here every few seconds (the room, one
// per party code) and takes back what phones asked for. Phones read the
// room and queue their asks; nothing here changes the party itself.
//
//   GET  /api/room/CODE?phone=ID   the party as phones see it (+ your answer)
//   POST /api/room/CODE/join       {phone, name}
//   POST /api/room/CODE/vote       {phone, vote, game}   (game -1 takes it back)
//   POST /api/room/CODE/host       NetDisc360 only (Authorization: Bearer KEY):
//                                  {party, answers: [{id, phone, ok, said, name}]}
//                                  -> {asks: [{id, phone, kind, name | vote, game}]}
//
// The first host post claims the code with its key; a room is forgotten 12
// hours after NetDisc360 last posted.

const CODE = /^[A-HJ-NP-Z2-9]{6}$/;
const FORGET_MS = 12 * 3600 * 1000;
const ASKS_MOST = 100;
const PHONES_MOST = 300;
const NAME_MOST = 32;  // As NetDisc360 takes (GuestNameMost).
const BODY_MOST = 64 * 1024;
const ASK_EVERY_MS = 800;

function json(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}
const refuse = (status, error) => json({ ok: false, error }, status);

async function readJson(request) {
  const text = await request.text();
  if (text.length > BODY_MOST) return null;
  try {
    const value = JSON.parse(text);
    return value && typeof value === "object" && !Array.isArray(value) ? value : null;
  } catch {
    return null;
  }
}

async function sha256(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// A name as NetDisc360 takes it: trimmed, no control characters, 1-32 long.
function cleanName(value) {
  if (typeof value !== "string") return "";
  const name = value.replace(/[\u0000-\u001f\u007f\t]/g, "").replace(/\s+/g, " ").trim();
  return [...name].length >= 1 && [...name].length <= NAME_MOST ? name : "";
}
const cleanPhone = (value) => (typeof value === "string" && /^[A-Za-z0-9_-]{8,40}$/.test(value) ? value : "");

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    if (parts[0] !== "api") return env.ASSETS.fetch(request);
    if (parts[1] !== "room" || !parts[2]) return refuse(404, "Not found.");
    const code = parts[2].toUpperCase();
    if (!CODE.test(code)) return refuse(404, "That isn't a party code.");
    const room = env.ROOMS.get(env.ROOMS.idFromName(code));
    return room.fetch(new Request(new URL("/" + (parts[3] || "") + url.search, url), request));
  },
};

export class Room {
  // The room lives in memory while it's in use, and in storage (written only
  // when something changed) for when Cloudflare moves or restarts it: the
  // free plan counts writes, and NetDisc360 posts every few seconds.
  constructor(ctx) {
    this.ctx = ctx;
    this.lastAsk = new Map();  // Phone -> when it last asked (in memory only).
    this.saved = {};           // What storage has, as JSON text, by key.
    ctx.blockConcurrencyWhile(async () => {
      const all = await ctx.storage.get(["owner", "party", "seenAt", "asks", "phones"]);
      this.owner = all.get("owner") || "";
      this.party = all.get("party") || null;
      this.seenAt = all.get("seenAt") || 0;
      this.asks = all.get("asks") || [];
      this.phones = all.get("phones") || {};
      for (const [key, value] of all) this.saved[key] = JSON.stringify(value);
    });
  }

  // Writes the keys whose value changed since storage last had them.
  async save(keys) {
    const changed = {};
    for (const key of keys) {
      const text = JSON.stringify(this[key]);
      if (this.saved[key] !== text) { changed[key] = this[key]; this.saved[key] = text; }
    }
    if (Object.keys(changed).length) await this.ctx.storage.put(changed);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const what = url.pathname.slice(1);
    if (what === "" && request.method === "GET") return this.read(url.searchParams.get("phone"));
    if (request.method !== "POST") return refuse(405, "Use POST.");
    const body = await readJson(request);
    if (!body) return refuse(400, "Send JSON.");
    if (what === "host") return this.host(request, body);
    if (what === "join" || what === "vote") return this.ask(what, body);
    return refuse(404, "Not found.");
  }

  read(phone) {
    if (!this.party) return refuse(404, "No party has this code right now. Check the code on the big screen.");
    const out = { ok: true, party: this.party, seenSecondsAgo: Math.max(0, Math.round((Date.now() - this.seenAt) / 1000)) };
    phone = cleanPhone(phone || "");
    if (phone && this.phones[phone]) out.you = this.phones[phone];
    return json(out);
  }

  async ask(kind, body) {
    const phone = cleanPhone(body.phone);
    if (!phone) return refuse(400, "Reload the page and try again.");
    const party = this.party;
    if (!party) return refuse(404, "No party has this code right now.");
    const now = Date.now();
    if (now - this.seenAt > 60 * 1000) return refuse(503, "NetDisc360 isn't reachable right now; try again in a minute.");
    if (now - (this.lastAsk.get(phone) || 0) < ASK_EVERY_MS) return refuse(429, "One moment, then try again.");
    this.lastAsk.set(phone, now);
    const ask = { id: crypto.randomUUID(), phone, kind, at: now };
    if (kind === "join") {
      ask.name = cleanName(body.name);
      if (!ask.name) return refuse(400, `Type your name (up to ${NAME_MOST} letters).`);
    } else {
      if (!party.vote || !party.vote.open) return refuse(409, "There's no vote on right now.");
      if (!Number.isInteger(body.game) || body.game < -1 || body.game >= party.vote.games.length) return refuse(400, "That isn't a game of the vote.");
      if (body.vote !== party.vote.id) return refuse(409, "That vote is over; here's the new one.");
      ask.vote = body.vote;
      ask.game = body.game;
    }
    // A phone's newer ask of a kind replaces its older one.
    const asks = this.asks.filter((a) => a.phone !== phone || a.kind !== kind);
    if (asks.length >= ASKS_MOST) return refuse(503, "Lots of people at once; try again in a moment.");
    asks.push(ask);
    this.asks = asks;
    await this.save(["asks"]);
    return json({ ok: true, at: now });
  }

  async host(request, body) {
    const key = (request.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
    if (key.length < 32) return refuse(401, "Send the host key.");
    const hash = await sha256(key);
    if (this.owner && this.owner !== hash) return refuse(403, "Another NetDisc360 has this party code.");
    if (!body.party || typeof body.party !== "object" || Array.isArray(body.party)) return refuse(400, "Send the party.");
    const now = Date.now();
    for (const a of Array.isArray(body.answers) ? body.answers : []) {
      const phone = cleanPhone(a && a.phone);
      if (!phone) continue;
      const you = this.phones[phone] || {};
      if (typeof a.name === "string" && a.ok) you.name = cleanName(a.name) || you.name;
      if (typeof a.said === "string") you.said = a.said.slice(0, 200);
      you.ok = !!a.ok;
      you.at = now;
      this.phones[phone] = you;
    }
    // The phones heard from last, PHONES_MOST at most.
    const phones = Object.entries(this.phones);
    if (phones.length > PHONES_MOST) this.phones = Object.fromEntries(phones.sort((a, b) => b[1].at - a[1].at).slice(0, PHONES_MOST));
    const asks = this.asks;
    const firstPost = !this.owner;
    this.owner = hash;
    this.party = body.party;
    this.asks = [];
    // When NetDisc360 was heard from is kept in storage now and then (for the alarm).
    const keepSeen = now - this.seenAt > 10 * 60 * 1000;
    this.seenAt = now;
    await this.save(["owner", "party", "asks", "phones"].concat(keepSeen ? ["seenAt"] : []));
    if (firstPost || keepSeen) await this.ctx.storage.setAlarm(now + FORGET_MS);
    return json({ ok: true, asks });
  }

  // Forgotten FORGET_MS after NetDisc360 last posted (then the code is free again).
  async alarm() {
    const seenAt = Math.max(this.seenAt, (await this.ctx.storage.get("seenAt")) || 0);
    if (Date.now() - seenAt >= FORGET_MS - 60 * 1000) {
      await this.ctx.storage.deleteAll();
      Object.assign(this, { owner: "", party: null, seenAt: 0, asks: [], phones: {}, saved: {} });
    } else await this.ctx.storage.setAlarm(seenAt + FORGET_MS);
  }
}
