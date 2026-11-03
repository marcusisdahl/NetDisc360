// SPDX-License-Identifier: MIT
// Runs NetDisc360's sync Worker (sync/src/worker.js) on plain http with an
// R2 bucket kept in memory, for tests/test_server.py:
//   node tests/sync_worker.mjs <owner key>
// prints "listening <port>" once it answers.
import http from "node:http";
import worker from "../sync/src/worker.js";
import { Bucket } from "../sync/test/bucket.js";

const env = { BUCKET: new Bucket(), OWNER_KEY: process.argv[2] };
const server = http.createServer(async (req, res) => {
  const parts = [];
  for await (const part of req) parts.push(part);
  const body = Buffer.concat(parts);
  const headers = new Headers();
  for (const [k, v] of Object.entries(req.headers)) headers.set(k, Array.isArray(v) ? v.join(", ") : v);
  const request = new Request("http://" + req.headers.host + req.url, {
    method: req.method, headers, body: ["GET", "HEAD"].includes(req.method) ? undefined : body,
  });
  const answer = await worker.fetch(request, env);
  const out = Buffer.from(await answer.arrayBuffer());
  res.writeHead(answer.status, Object.fromEntries(answer.headers));
  res.end(out);
});
server.listen(0, "127.0.0.1", () => console.log("listening " + server.address().port));
