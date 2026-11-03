// Tests the sync Worker against an R2 bucket kept in memory: node --test test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import worker from "../src/worker.js";
import { Bucket } from "./bucket.js";

const OWNER = "owner-key-for-the-tests-0123456789";
const sha = (data) => createHash("sha256").update(data).digest("hex");

function setup() {
  const env = { BUCKET: new Bucket(), OWNER_KEY: OWNER };
  const call = (method, path, key, body, headers = {}) =>
    worker.fetch(
      new Request("https://sync.example" + path, {
        method,
        headers: { ...(key ? { authorization: "Bearer " + key } : {}), ...headers },
        body,
      }),
      env,
    );
  return { env, call };
}

async function connect(call, name, key = OWNER) {
  const r = await call("POST", "/v1/devices", key, JSON.stringify({ name }));
  assert.equal(r.status, 200);
  const j = await r.json();
  assert.match(j.key, /^nd1-[a-z0-9]{10}-[A-Za-z0-9]{40}$/);
  return j;
}

function putFile(call, key, path, data, digest = sha(data)) {
  return call("PUT", "/v1/object/" + path, key, data, { "x-sha256": digest, "content-length": String(Buffer.byteLength(data)) });
}

test("needs a key, and the owner key only connects devices", async () => {
  const { call } = setup();
  assert.equal((await call("GET", "/v1/hello")).status, 401);
  assert.equal((await call("GET", "/v1/hello", "wrong")).status, 401);
  assert.equal((await call("GET", "/v1/hello", OWNER)).status, 403);
  assert.equal((await call("GET", "/v1/hello", "nd1-abcdefghij-" + "a".repeat(40))).status, 401);
});

test("a connected device has a key of its own, and can be taken away", async () => {
  const { call } = setup();
  const pc = await connect(call, "Home PC");
  const laptop = await connect(call, "Laptop", pc.key);  // A device can connect another.
  const hello = await (await call("GET", "/v1/hello", laptop.key)).json();
  assert.equal(hello.name, "Laptop");
  const list = await (await call("GET", "/v1/devices", pc.key)).json();
  assert.deepEqual(list.devices.map((d) => d.name), ["Home PC", "Laptop"]);
  assert.equal((await call("DELETE", "/v1/devices/" + pc.id, pc.key)).status, 400);
  assert.equal((await call("DELETE", "/v1/devices/" + laptop.id, pc.key)).status, 200);
  assert.equal((await call("GET", "/v1/hello", laptop.key)).status, 401);
  // A key with the right id but another secret doesn't pass.
  assert.equal((await call("GET", "/v1/hello", pc.key.slice(0, -1) + (pc.key.endsWith("A") ? "B" : "A"))).status, 401);
});

test("names are checked", async () => {
  const { call } = setup();
  assert.equal((await call("POST", "/v1/devices", OWNER, JSON.stringify({ name: "  " }))).status, 400);
  assert.equal((await call("POST", "/v1/devices", OWNER, "not json")).status, 400);
  const pc = await connect(call, "PC");
  for (const bad of ["files/saves/../x.zip", "files/Saves/a.zip", "devices/x.json", "files/saves", "state/zzzzzzzzzz/../a", "files/saves/a b.zip"]) {
    assert.equal((await putFile(call, pc.key, encodeURIComponent(bad).replace(/%2F/g, "/"), "x")).status, 400, bad);
  }
});

test("files never change once synced, and arrive whole", async () => {
  const { call } = setup();
  const pc = await connect(call, "PC");
  const laptop = await connect(call, "Laptop");
  const path = "files/saves/4D5307E6_2026-11-03_21-00-00.zip";
  assert.equal((await putFile(call, pc.key, path, "save one", sha("other"))).status, 400);
  assert.equal((await putFile(call, pc.key, path, "save one")).status, 200);
  const again = await putFile(call, laptop.key, path, "save one");
  assert.equal(again.status, 200);
  assert.equal((await again.json()).same, true);
  assert.equal((await putFile(call, laptop.key, path, "save two")).status, 409);
  const got = await call("GET", "/v1/object/" + path, laptop.key);
  assert.equal(await got.text(), "save one");
  assert.equal(got.headers.get("x-sha256"), sha("save one"));
  assert.equal((await call("GET", "/v1/object/files/saves/none.zip", laptop.key)).status, 404);
});

test("a device writes only its own state, and everyone reads it", async () => {
  const { call } = setup();
  const pc = await connect(call, "PC");
  const laptop = await connect(call, "Laptop");
  assert.equal((await putFile(call, laptop.key, `state/${pc.id}/plays.ini`, "x")).status, 403);
  assert.equal((await putFile(call, pc.key, `state/${pc.id}/plays.ini`, "1|60|5|halo 3\n")).status, 200);
  assert.equal((await putFile(call, pc.key, `state/${pc.id}/plays.ini`, "2|90|6|halo 3\n")).status, 200);
  const got = await call("GET", `/v1/object/state/${pc.id}/plays.ini`, laptop.key);
  assert.equal(await got.text(), "2|90|6|halo 3\n");
});

test("lists by prefix, page by page", async () => {
  const { call } = setup();
  const pc = await connect(call, "PC");
  for (let i = 0; i < 3; i++) await putFile(call, pc.key, `files/profiles/E000000000000001/p${i}.zip`, "p" + i);
  await putFile(call, pc.key, "files/saves/a.zip", "a");
  assert.equal((await call("GET", "/v1/list?prefix=devices/", pc.key)).status, 400);
  const all = await (await call("GET", "/v1/list?prefix=files/profiles/", pc.key)).json();
  assert.deepEqual(all.items.map((i) => i.key.split("/").pop()), ["p0.zip", "p1.zip", "p2.zip"]);
  assert.equal(all.items[0].sha256, sha("p0"));
  assert.equal(all.items[0].device, pc.id);
  assert.equal(all.next, "");
});
