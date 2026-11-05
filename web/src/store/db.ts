// Main-thread API for the local SQLite store.
export interface RecipeRow {
  id: number;
  name: string;
  steps: string;
  created: number;
  updated: number;
}
export interface SignatureRow {
  id: number;
  name: string;
  png: Uint8Array;
  w: number;
  h: number;
  created: number;
}

let worker: Worker | null = null;
let ready: Promise<string> | null = null;
let nextId = 1;
const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: unknown) => void }>();

function call<T>(method: string, ...args: unknown[]): Promise<T> {
  if (!worker) {
    worker = new Worker(new URL('./db.worker.ts', import.meta.url), {
      type: 'module',
      name: 'noodle-db',
    });
    worker.onmessage = (
      e: MessageEvent<{
        id: number;
        ok: boolean;
        value?: unknown;
        error?: string;
      }>,
    ) => {
      const p = pending.get(e.data.id);
      if (!p) return;
      pending.delete(e.data.id);
      if (e.data.ok) p.resolve(e.data.value);
      else p.reject(new Error(e.data.error));
    };
  }
  const id = nextId++;
  return new Promise<T>((resolve, reject) => {
    pending.set(id, { resolve: resolve as (v: unknown) => void, reject });
    worker!.postMessage({ id, method, args });
  });
}

export function initDb(): Promise<string> {
  if (!ready)
    ready = call<string>('init', document.baseURI).catch((e) => {
      console.warn('local storage unavailable', e);
      return 'unavailable';
    });
  return ready;
}
async function c<T>(method: string, ...args: unknown[]) {
  const m = await initDb();
  if (m === 'unavailable') throw new Error('Local storage is unavailable in this browser.');
  return call<T>(method, ...args);
}

export const db = {
  mode: () => initDb(),
  listRecipes: () => c<RecipeRow[]>('listRecipes'),
  saveRecipe: (r: { id?: number; name: string; steps: string }) => c<number>('saveRecipe', r),
  deleteRecipe: (id: number) => c<void>('deleteRecipe', id),
  listSignatures: () => c<SignatureRow[]>('listSignatures'),
  saveSignature: (s: { name: string; png: Uint8Array; w: number; h: number }) => c<number>('saveSignature', s),
  deleteSignature: (id: number) => c<void>('deleteSignature', id),
  getSetting: (key: string) => c<string | null>('getSetting', key),
  setSetting: (key: string, value: string) => c<void>('setSetting', key, value),
  wipe: () => c<void>('wipe'),
};
