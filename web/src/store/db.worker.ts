/// <reference lib="webworker" />
// Local-only storage: SQLite (WASM) persisted in the Origin Private File System, with an
// IndexedDB snapshot fallback. Holds recipes, saved signatures and settings — never documents.
import '../lib/guard-first';

/* eslint-disable @typescript-eslint/no-explicit-any */
let sqlite3: any = null;
let db: any = null;
let mode: 'opfs' | 'indexeddb' | 'memory' = 'memory';

const SCHEMA = `
CREATE TABLE IF NOT EXISTS recipes(id INTEGER PRIMARY KEY, name TEXT NOT NULL, steps TEXT NOT NULL, created INTEGER, updated INTEGER);
CREATE TABLE IF NOT EXISTS signatures(id INTEGER PRIMARY KEY, name TEXT, png BLOB NOT NULL, w INTEGER, h INTEGER, created INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
`;

// ---- IndexedDB snapshot fallback ----
const IDB_NAME = 'pagenoodle';
function idb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open(IDB_NAME, 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function idbGet(key: string): Promise<Uint8Array | undefined> {
  const d = await idb();
  return new Promise((resolve, reject) => {
    const r = d.transaction('kv').objectStore('kv').get(key);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function idbPut(key: string, value: Uint8Array) {
  const d = await idb();
  return new Promise<void>((resolve, reject) => {
    const tx = d.transaction('kv', 'readwrite');
    tx.objectStore('kv').put(value, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}
async function persist() {
  if (mode !== 'indexeddb') return;
  const bytes = sqlite3.capi.sqlite3_js_db_export(db);
  await idbPut('db', bytes);
}

async function init(base: string) {
  if (db) return mode;
  const mod = await import(/* @vite-ignore */ new URL('vendor/sqlite/index.mjs', base).href);
  sqlite3 = await mod.default({ print: () => {}, printErr: () => {} });
  try {
    const pool = await sqlite3.installOpfsSAHPoolVfs({
      name: 'pagenoodle-pool',
    });
    db = new pool.OpfsSAHPoolDb('/pagenoodle.sqlite3');
    mode = 'opfs';
  } catch {
    db = new sqlite3.oo1.DB(':memory:', 'c');
    mode = 'memory';
    try {
      const snap = await idbGet('db');
      if (snap && snap.length) {
        const p = sqlite3.wasm.allocFromTypedArray(snap);
        const flags = sqlite3.capi.SQLITE_DESERIALIZE_FREEONCLOSE | sqlite3.capi.SQLITE_DESERIALIZE_RESIZEABLE;
        db.checkRc(sqlite3.capi.sqlite3_deserialize(db.pointer, 'main', p, snap.length, snap.length, flags));
      }
      mode = 'indexeddb';
    } catch {
      mode = 'memory';
    }
  }
  db.exec(SCHEMA);
  return mode;
}

const rows = (sql: string, bind: unknown[] = []) => db.exec({ sql, bind, rowMode: 'object', returnValue: 'resultRows' });

const methods: Record<string, (...a: any[]) => unknown> = {
  init,
  mode: () => mode,
  listRecipes: () => rows('SELECT id, name, steps, created, updated FROM recipes ORDER BY updated DESC'),
  async saveRecipe(r: { id?: number; name: string; steps: string }) {
    const now = Date.now();
    if (r.id)
      db.exec({
        sql: 'UPDATE recipes SET name=?, steps=?, updated=? WHERE id=?',
        bind: [r.name, r.steps, now, r.id],
      });
    else
      db.exec({
        sql: 'INSERT INTO recipes(name, steps, created, updated) VALUES (?,?,?,?)',
        bind: [r.name, r.steps, now, now],
      });
    const id = r.id ?? db.selectValue('SELECT last_insert_rowid()');
    await persist();
    return id;
  },
  async deleteRecipe(id: number) {
    db.exec({ sql: 'DELETE FROM recipes WHERE id=?', bind: [id] });
    await persist();
  },
  listSignatures: () => rows('SELECT id, name, png, w, h, created FROM signatures ORDER BY created DESC'),
  async saveSignature(s: { name: string; png: Uint8Array; w: number; h: number }) {
    db.exec({
      sql: 'INSERT INTO signatures(name, png, w, h, created) VALUES (?,?,?,?,?)',
      bind: [s.name, s.png, s.w, s.h, Date.now()],
    });
    const id = db.selectValue('SELECT last_insert_rowid()');
    await persist();
    return id;
  },
  async deleteSignature(id: number) {
    db.exec({ sql: 'DELETE FROM signatures WHERE id=?', bind: [id] });
    await persist();
  },
  getSetting: (key: string) => db.selectValue('SELECT value FROM settings WHERE key=?', [key]) ?? null,
  async setSetting(key: string, value: string) {
    db.exec({
      sql: 'INSERT INTO settings(key, value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value',
      bind: [key, value],
    });
    await persist();
  },
  async wipe() {
    db.exec('DELETE FROM recipes; DELETE FROM signatures; DELETE FROM settings;');
    await persist();
  },
};

self.onmessage = async (e: MessageEvent<{ id: number; method: string; args: unknown[] }>) => {
  const { id, method, args } = e.data;
  try {
    if (method !== 'init' && !db) throw new Error('storage not ready');
    const value = await methods[method](...args);
    (self as DedicatedWorkerGlobalScope).postMessage({ id, ok: true, value });
  } catch (err) {
    (self as DedicatedWorkerGlobalScope).postMessage({
      id,
      ok: false,
      error: err instanceof Error ? err.message : String(err),
    });
  }
};
