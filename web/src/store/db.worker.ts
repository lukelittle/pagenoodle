/// <reference lib="webworker" />
// Local-only storage: SQLite (WASM) persisted in the Origin Private File System, with an
// IndexedDB snapshot fallback. Holds recipes, saved signatures and settings — never documents.
import '../lib/guard-first';

/* eslint-disable @typescript-eslint/no-explicit-any */
let sqlite3: any = null;
let db: any = null;
let mode: 'opfs' | 'indexeddb' | 'memory' = 'memory';
let sahPool: any = null;
const compact = () => db.exec('VACUUM;');

const SCHEMA = `
CREATE TABLE IF NOT EXISTS recipes(id INTEGER PRIMARY KEY, name TEXT NOT NULL, steps TEXT NOT NULL, created INTEGER, updated INTEGER);
CREATE TABLE IF NOT EXISTS signatures(id INTEGER PRIMARY KEY, name TEXT, png BLOB NOT NULL, w INTEGER, h INTEGER, created INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
`;

// ---- IndexedDB snapshot fallback ----
const IDB_NAME = 'pagenoodle';
let idbConn: IDBDatabase | null = null;
function idb(): Promise<IDBDatabase> {
  if (idbConn) return Promise.resolve(idbConn);
  return new Promise((resolve, reject) => {
    const r = indexedDB.open(IDB_NAME, 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => {
      idbConn = r.result;
      // let deleteDatabase (wipe, possibly from another tab) proceed instead of blocking forever
      idbConn.onversionchange = () => {
        idbConn?.close();
        idbConn = null;
      };
      resolve(idbConn);
    };
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

let baseUrl = '';
let initP: Promise<string> | null = null;
function init(base: string): Promise<string> {
  if (!initP)
    initP = initInner(base).catch((e) => {
      initP = null;
      throw e;
    });
  return initP;
}
async function initInner(base: string) {
  if (db) return mode;
  baseUrl = base;
  const mod = await import(/* @vite-ignore */ new URL('vendor/sqlite/index.mjs', base).href);
  sqlite3 = await mod.default({ print: () => {}, printErr: () => {} });
  try {
    const pool = await sqlite3.installOpfsSAHPoolVfs({
      name: 'pagenoodle-pool',
    });
    const odb = new pool.OpfsSAHPoolDb('/pagenoodle.sqlite3');
    odb.exec('PRAGMA secure_delete=ON;');
    odb.exec(SCHEMA);
    db = odb;
    sahPool = pool;
    mode = 'opfs';
  } catch {
    const mem = new sqlite3.oo1.DB(':memory:', 'c');
    mode = 'memory';
    try {
      const snap = await idbGet('db');
      if (snap && snap.length) {
        const p = sqlite3.wasm.allocFromTypedArray(snap);
        const flags = sqlite3.capi.SQLITE_DESERIALIZE_FREEONCLOSE | sqlite3.capi.SQLITE_DESERIALIZE_RESIZEABLE;
        mem.checkRc(sqlite3.capi.sqlite3_deserialize(mem.pointer, 'main', p, snap.length, snap.length, flags));
      }
      mode = 'indexeddb';
    } catch {
      mode = 'memory';
    }
    // Deleted rows are overwritten with zeros, and the file is compacted after deletes, so removed
    // signatures/recipes don't linger in free pages on disk.
    mem.exec('PRAGMA secure_delete=ON;');
    mem.exec(SCHEMA);
    db = mem; // publish only once fully ready
  }
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
    compact();
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
    compact();
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
  wipe() {
    // every request waits until the storage has been destroyed and recreated
    const prev = initP;
    initP = (async () => {
      await prev; // never tear down a database that is still being opened
      // Remove the storage itself, not just the rows: close the database, delete the OPFS pool
      // (all its files) and the IndexedDB snapshot, then start again empty.
      db.exec('DELETE FROM recipes; DELETE FROM signatures; DELETE FROM settings;');
      compact();
      db.close();
      db = null;
      try {
        if (sahPool) await sahPool.removeVfs();
      } catch {
        /* ignore */
      }
      try {
        const root = await navigator.storage.getDirectory();
        for await (const name of (root as unknown as { keys(): AsyncIterable<string> }).keys()) {
          if (name.includes('pagenoodle')) await root.removeEntry(name, { recursive: true });
        }
      } catch {
        /* ignore */
      }
      idbConn?.close();
      idbConn = null;
      await new Promise<void>((resolve) => {
        const r = indexedDB.deleteDatabase(IDB_NAME);
        r.onsuccess = r.onerror = r.onblocked = () => resolve();
      });
      sahPool = null;
      mode = 'memory';
      return initInner(baseUrl);
    })();
    return initP;
  },
};

self.onmessage = async (e: MessageEvent<{ id: number; method: string; args: unknown[] }>) => {
  const { id, method, args } = e.data;
  try {
    if (method !== 'init') {
      if (!initP) throw new Error('storage not ready');
      if (method !== 'wipe') await initP;
    }
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
