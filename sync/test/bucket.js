// SPDX-License-Identifier: MIT
// An R2 bucket kept in memory, for the tests (test/*.test.js, tests/sync_worker.mjs).
// The parts of R2's bucket API the Worker uses.
export class Bucket {
  constructor() {
    this.objects = new Map();
  }
  async put(key, value, options = {}) {
    const bytes = typeof value === "string" ? new TextEncoder().encode(value) : new Uint8Array(value);
    this.objects.set(key, { bytes, customMetadata: options.customMetadata || {}, uploaded: new Date() });
  }
  async head(key) {
    const o = this.objects.get(key);
    return o ? { key, size: o.bytes.length, customMetadata: o.customMetadata, uploaded: o.uploaded } : null;
  }
  async get(key) {
    const o = this.objects.get(key);
    if (!o) return null;
    return {
      key,
      body: o.bytes,
      customMetadata: o.customMetadata,
      json: async () => JSON.parse(new TextDecoder().decode(o.bytes)),
    };
  }
  async delete(key) {
    this.objects.delete(key);
  }
  async list({ prefix = "", cursor, limit = 1000 } = {}) {
    const keys = [...this.objects.keys()].filter((k) => k.startsWith(prefix)).sort();
    const from = cursor ? keys.findIndex((k) => k > cursor) : 0;
    const page = from < 0 ? [] : keys.slice(from, from + limit);
    const truncated = from >= 0 && from + limit < keys.length;
    return {
      objects: page.map((k) => ({ key: k, size: this.objects.get(k).bytes.length, uploaded: this.objects.get(k).uploaded, customMetadata: this.objects.get(k).customMetadata })),
      truncated,
      cursor: truncated ? page[page.length - 1] : undefined,
    };
  }
}

