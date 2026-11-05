// Main-thread client for the engine worker. Heavy work never blocks the UI; any call can be
// cancelled — cancelling terminates the worker and a fresh one is started transparently.
import type { DocInfo, EditOp, EditResult, FormField, ImageSpec, PageSizeMode, Part, SearchHit, Word, WorkerResponse } from './types';

export class CancelledError extends Error {
  constructor() {
    super('Cancelled');
    this.name = 'CancelledError';
  }
}

export interface CallOptions {
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
}

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: unknown) => void;
  onProgress?: (d: number, t: number) => void;
}

class EngineClient {
  private worker: Worker | null = null;
  private ready: Promise<void> | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  /** bytes for every document the UI knows about, so a restarted worker can reload them */
  private registry = new Map<string, Uint8Array>();
  private loaded = new Set<string>();

  private start(): Promise<void> {
    if (this.ready) return this.ready;
    this.worker = new Worker(new URL('./engine.worker.ts', import.meta.url), {
      type: 'module',
      name: 'noodle-engine',
    });
    this.worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const msg = e.data;
      const p = this.pending.get(msg.id);
      if (!p) return;
      if (msg.type === 'progress') p.onProgress?.(msg.done, msg.total);
      else {
        this.pending.delete(msg.id);
        if (msg.type === 'result') p.resolve(msg.value);
        else p.reject(new Error(msg.message));
      }
    };
    this.worker.onerror = (e) => {
      for (const p of this.pending.values()) p.reject(new Error(e.message || 'Engine crashed'));
      this.pending.clear();
      this.reset();
    };
    this.ready = this.raw('init', [document.baseURI]) as Promise<void>;
    return this.ready;
  }

  private reset() {
    this.worker?.terminate();
    this.worker = null;
    this.ready = null;
    this.loaded.clear();
  }

  /** Abort everything in flight (used by the Cancel button). */
  cancelAll() {
    for (const p of this.pending.values()) p.reject(new CancelledError());
    this.pending.clear();
    this.reset();
  }

  private raw(method: string, args: unknown[], opts: CallOptions = {}): Promise<unknown> {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      if (opts.signal?.aborted) return reject(new CancelledError());
      const onAbort = () => this.cancelAll();
      opts.signal?.addEventListener('abort', onAbort, { once: true });
      const done = () => opts.signal?.removeEventListener('abort', onAbort);
      this.pending.set(id, {
        resolve: (v) => (done(), resolve(v)),
        reject: (e) => (done(), reject(e)),
        onProgress: opts.onProgress,
      });
      this.worker!.postMessage({ id, method, args });
    });
  }

  private async call<T>(method: string, args: unknown[], keys: string[] = [], opts: CallOptions = {}): Promise<T> {
    await this.start();
    for (const k of new Set(keys)) {
      if (this.loaded.has(k)) continue;
      const bytes = this.registry.get(k);
      if (!bytes) throw new Error(`Unknown document ${k}`);
      await this.raw('load', [k, bytes]);
      this.loaded.add(k);
    }
    return (await this.raw(method, args, opts)) as T;
  }

  /** Register a document and return its parsed info. */
  async open(key: string, bytes: Uint8Array, opts?: CallOptions): Promise<DocInfo> {
    await this.start();
    this.registry.set(key, bytes);
    try {
      const info = (await this.raw('load', [key, bytes], opts)) as DocInfo;
      this.loaded.add(key);
      return info;
    } catch (e) {
      this.registry.delete(key); // don't keep the bytes of a file that failed to open
      throw e;
    }
  }

  forget(key: string) {
    this.registry.delete(key);
    if (this.loaded.has(key)) {
      this.loaded.delete(key);
      void this.raw('drop', [key]).catch(() => {});
    }
  }

  assemble(parts: Part[], flags: number, opts?: CallOptions) {
    return this.call<Uint8Array>(
      'assemble',
      [parts, flags],
      parts.map((p) => p.key),
      opts,
    );
  }
  edit(key: string, ops: EditOp[], flags: number, opts?: CallOptions) {
    return this.call<EditResult>('edit', [key, ops, flags], [key], opts);
  }
  text(key: string, page: number) {
    return this.call<{ text: string; words: Word[] }>('text', [key, page], [key]);
  }
  textAll(key: string, opts?: CallOptions) {
    return this.call<string[]>('textAll', [key], [key], opts);
  }
  search(key: string, needle: string, opts?: CallOptions) {
    return this.call<SearchHit[]>('search', [key, needle], [key], opts);
  }
  forms(key: string) {
    return this.call<FormField[]>('forms', [key], [key]);
  }
  verify(bytes: Uint8Array, items: { page: number; rects: number[][] }[], terms: string[]) {
    return this.call<{
      remaining: number;
      leaked: string;
      stillFound: string[];
    }>('verify', [bytes, items, terms]);
  }
  imagesToPdf(specs: ImageSpec[], mode: PageSizeMode, margin: number, opts?: CallOptions) {
    return this.call<Uint8Array>('imagesToPdf', [specs, mode, margin], [], opts);
  }
  version() {
    return this.call<string>('version', []);
  }
}

export const engine = new EngineClient();
