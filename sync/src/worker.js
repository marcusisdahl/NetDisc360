// SPDX-License-Identifier: MIT
// NetDisc360's sync on Cloudflare: every NetDisc360 (the PC at home, a
// laptop, any other) keeps its own settings and copies the parts worth
// sharing through here, kept in an R2 bucket (BUCKET):
//
//   files/<kind>/<path>      backups that never change once made (game saves,
//                            profiles): any device adds them, none replaces
//                            one with other bytes, and none deletes them
//   state/<device>/<name>    a device's own documents (its play history):
//                            only that device writes them, every device reads
//   devices/<id>.json        the devices and a hash of each one's key
//
// Every request has "Authorization: Bearer <key>". The owner key (the
// OWNER_KEY secret) only connects devices: NetDisc360 sends it once, gets a
// key of the device's own back, and keeps only that. A device's key can be
// taken away (DELETE /v1/devices/<id>) without touching the others.
//
//   POST   /v1/devices {name}        connect a device -> {id, key, name}
//                                    (owner key, or any device's key)
//   GET    /v1/devices               the devices
//   DELETE /v1/devices/<id>          take a device's key away
//   GET    /v1/hello                 who this key is
//   GET    /v1/list?prefix=&after=   files and states under prefix, by key
//                                    -> {items: [{key, size, sha256, device,
//                                    time}], next}  (next: pass as after)
//   GET    /v1/object/<key>          an object's bytes
//   PUT    /v1/object/<key>          store one (X-Sha256: its SHA-256 in hex)

const ID = /^[a-z0-9]{10}$/;
const KIND = /^[a-z][a-z0-9-]{0,31}$/;
// A path below a kind or a device: up to 4 parts of letters, digits and
// . _ - (no part only dots), as NetDisc360 names its backups.
const PART = /^(?!\.+$)[A-Za-z0-9._-]{1,128}$/;
const NAME_MOST = 64;
const DEVICES_MOST = 50;
const OBJECT_MOST = 95 * 1024 * 1024;  // Under the Workers request limit (100 MB on the free plan).
const LIST_MOST = 1000;

function json(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}
const refuse = (status, error) => json({ ok: false, error }, status);

const hex = (bytes) => [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, "0")).join("");
async function sha256(data) {
  const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data;
  return hex(await crypto.subtle.digest("SHA-256", bytes));
}
function random(chars, length) {
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  return [...bytes].map((b) => chars[b % chars.length]).join("");
}
// The same length both ways, so the time it takes says nothing.
function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// A device's name: trimmed, no control characters, 1-64 long.
function cleanName(value) {
  if (typeof value !== "string") return "";
  const name = value.replace(/[\u0000-\u001f\u007f]/g, "").replace(/\s+/g, " ").trim();
  return [...name].length >= 1 && [...name].length <= NAME_MOST ? name : "";
}

// Whether key is one a device may name: files/<kind>/<path> or
// state/<device>/<path>.
function goodKey(key) {
  const parts = key.split("/");
  if (parts.length < 3 || parts.length > 6) return false;
  if (parts[0] === "files") return KIND.test(parts[1]) && parts.slice(2).every((p) => PART.test(p));
  if (parts[0] === "state") return ID.test(parts[1]) && parts.slice(2).every((p) => PART.test(p));
  return false;
}

// Who the request's key is: {owner: true}, {device}, or null.
async function who(request, env) {
  const auth = request.headers.get("authorization") || "";
  if (!auth.startsWith("Bearer ")) return null;
  const key = auth.slice(7).trim();
  // Trimmed: a secret piped in from a shell can end in a line break.
  const owner = typeof env.OWNER_KEY === "string" ? env.OWNER_KEY.trim() : "";
  if (owner.length >= 16 && same(key, owner)) return { owner: true };
  const m = /^nd1-([a-z0-9]{10})-([A-Za-z0-9]{40})$/.exec(key);
  if (!m) return null;
  const stored = await env.BUCKET.get(`devices/${m[1]}.json`);
  if (!stored) return null;
  const device = await stored.json();
  return same(await sha256(m[2]), device.hash) ? { device } : null;
}

async function devices(env) {
  const out = [];
  let cursor;
  do {
    const page = await env.BUCKET.list({ prefix: "devices/", cursor });
    for (const o of page.objects) {
      const d = await (await env.BUCKET.get(o.key))?.json();
      if (d) out.push({ id: d.id, name: d.name, added: d.added });
    }
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
  return out.sort((a, b) => a.added - b.added);
}

async function addDevice(request, env) {
  let body;
  try {
    body = JSON.parse(await request.text());
  } catch {
    return refuse(400, "Send {\"name\": \"...\"}.");
  }
  const name = cleanName(body && body.name);
  if (!name) return refuse(400, `A device's name is 1 to ${NAME_MOST} characters.`);
  const all = await devices(env);
  if (all.length >= DEVICES_MOST) return refuse(409, `There are already ${DEVICES_MOST} devices; remove one first.`);
  const id = random("abcdefghijklmnopqrstuvwxyz0123456789", 10);
  const secret = random("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 40);
  const device = { id, name, added: Date.now(), hash: await sha256(secret) };
  await env.BUCKET.put(`devices/${id}.json`, JSON.stringify(device), {
    httpMetadata: { contentType: "application/json" },
  });
  return json({ ok: true, id, name, key: `nd1-${id}-${secret}` });
}

async function list(url, env) {
  const prefix = url.searchParams.get("prefix") || "";
  if (!/^(files|state)\//.test(prefix)) return refuse(400, "prefix starts with files/ or state/.");
  const after = url.searchParams.get("after") || undefined;
  const page = await env.BUCKET.list({ prefix, cursor: after, limit: LIST_MOST, include: ["customMetadata"] });
  const items = page.objects.map((o) => ({
    key: o.key,
    size: o.size,
    sha256: (o.customMetadata && o.customMetadata.sha256) || "",
    device: (o.customMetadata && o.customMetadata.device) || "",
    time: o.uploaded.getTime(),
  }));
  return json({ ok: true, items, next: page.truncated ? page.cursor : "" });
}

async function put(key, request, env, device) {
  const parts = key.split("/");
  if (parts[0] === "state" && parts[1] !== device.id) return refuse(403, "A device writes only its own state.");
  const length = Number(request.headers.get("content-length") || "-1");
  if (length > OBJECT_MOST) return refuse(413, "That file is too large to sync.");
  const given = (request.headers.get("x-sha256") || "").toLowerCase();
  if (!/^[0-9a-f]{64}$/.test(given)) return refuse(400, "Send the file's SHA-256 in X-Sha256.");
  const data = await request.arrayBuffer();
  if (data.byteLength > OBJECT_MOST) return refuse(413, "That file is too large to sync.");
  if ((await sha256(data)) !== given) return refuse(400, "The file didn't arrive whole (its SHA-256 differs).");
  if (parts[0] === "files") {
    const there = await env.BUCKET.head(key);
    if (there) {
      if ((there.customMetadata && there.customMetadata.sha256) === given) return json({ ok: true, same: true });
      return refuse(409, "Another file of that name is already synced.");
    }
  }
  await env.BUCKET.put(key, data, { customMetadata: { sha256: given, device: device.id } });
  return json({ ok: true });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (!path.startsWith("/v1/")) return refuse(404, "This is NetDisc360's sync; put its address in NetDisc360's Settings.");
    const me = await who(request, env);
    if (!me) return refuse(401, "That key isn't one of this sync's.");
    const method = request.method;

    if (path === "/v1/devices" && method === "POST") return addDevice(request, env);
    if (me.owner) return refuse(403, "The owner key only connects devices.");
    const device = me.device;

    if (path === "/v1/hello" && method === "GET") return json({ ok: true, id: device.id, name: device.name });
    if (path === "/v1/devices" && method === "GET") return json({ ok: true, devices: await devices(env) });
    const gone = /^\/v1\/devices\/([a-z0-9]{10})$/.exec(path);
    if (gone && method === "DELETE") {
      if (gone[1] === device.id) return refuse(400, "A device can't remove itself; remove it from another one.");
      await env.BUCKET.delete(`devices/${gone[1]}.json`);
      return json({ ok: true });
    }
    if (path === "/v1/list" && method === "GET") return list(url, env);
    if (path.startsWith("/v1/object/")) {
      let key;
      try {
        key = decodeURIComponent(path.slice(11));
      } catch {
        return refuse(400, "That isn't a file's name.");
      }
      if (!goodKey(key)) return refuse(400, "That isn't a file's name.");
      if (method === "GET") {
        const o = await env.BUCKET.get(key);
        if (!o) return refuse(404, "No such file.");
        return new Response(o.body, {
          headers: {
            "content-type": "application/octet-stream",
            "cache-control": "no-store",
            "x-sha256": (o.customMetadata && o.customMetadata.sha256) || "",
          },
        });
      }
      if (method === "PUT") return put(key, request, env, device);
    }
    return refuse(404, "Not found.");
  },
};
