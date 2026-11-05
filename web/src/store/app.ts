// Application state: the open documents, the page arrangement, selection, history and UI flags.
import { useEffect, useState } from 'preact/hooks';
import { engine, CancelledError } from '../engine/client';
import type { DocInfo, EditOp, Part, PageSizeMode } from '../engine/types';
import { SAVE_DEFAULT } from '../engine/types';
import { loadPdf, unloadPdf } from '../render/pdf';
import type { Step } from '../pipeline/steps';

export interface Source {
  key: string;
  name: string;
  bytes: Uint8Array;
  info: DocInfo;
}
export interface PageRef {
  id: string;
  src: string;
  index: number;
  rot: number;
}
export interface Doc {
  sources: Record<string, Source>;
  pages: PageRef[];
}
export interface Busy {
  label: string;
  done: number;
  total: number;
}
export interface Toast {
  id: number;
  kind: 'info' | 'success' | 'error';
  text: string;
}
export type Mode = 'organize' | 'edit' | 'redact' | 'tools' | 'recipes' | 'compare' | 'privacy';

export interface State {
  doc: Doc;
  selected: string[];
  mode: Mode;
  busy: Busy | null;
  toasts: Toast[];
  recording: Step[] | null;
  baseName: string;
  past: Doc[];
  future: Doc[];
  celebrate: number;
  editPage: number;
  /** sticky save flags (e.g. after compress / strip metadata) used for exports */
  saveFlags: number;
  /** filename suffix describing the last thing done, e.g. "signed" */
  suffix: string;
}

let state: State = {
  doc: { sources: {}, pages: [] },
  selected: [],
  mode: 'organize',
  busy: null,
  toasts: [],
  recording: null,
  baseName: 'document',
  past: [],
  future: [],
  celebrate: 0,
  editPage: 0,
  saveFlags: SAVE_DEFAULT,
  suffix: '',
};
const listeners = new Set<() => void>();

export const getState = () => state;
export function setState(patch: Partial<State> | ((s: State) => Partial<State>)) {
  const p = typeof patch === 'function' ? patch(state) : patch;
  state = { ...state, ...p };
  listeners.forEach((l) => l());
}
export function useApp(): State {
  const [, force] = useState(0);
  useEffect(() => {
    const l = () => force((n) => n + 1);
    listeners.add(l);
    return () => void listeners.delete(l);
  }, []);
  return state;
}

// ---------- helpers ----------
let uid = 0;
export const newId = (p = 'p') => `${p}${Date.now().toString(36)}${(uid++).toString(36)}`;
export const hasDoc = () => state.doc.pages.length > 0;

export function toast(text: string, kind: Toast['kind'] = 'info') {
  const id = ++uid;
  setState((s) => ({ toasts: [...s.toasts, { id, kind, text }] }));
  setTimeout(() => setState((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) })), kind === 'error' ? 8000 : 4500);
}

let abort: AbortController | null = null;
export async function withBusy<T>(
  label: string,
  fn: (ctx: { onProgress: (d: number, t: number) => void; signal: AbortSignal }) => Promise<T>,
): Promise<T | undefined> {
  if (state.busy) {
    // one operation at a time: concurrent edits could silently overwrite each other
    toast('Hang on — still working on the last thing.');
    return undefined;
  }
  abort = new AbortController();
  const signal = abort.signal;
  setState({ busy: { label, done: 0, total: 0 } });
  try {
    return await fn({
      signal,
      onProgress: (done, total) => setState((s) => (s.busy ? { busy: { ...s.busy, done, total } } : {})),
    });
  } catch (e) {
    if (e instanceof CancelledError || (e instanceof DOMException && e.name === 'AbortError') || signal.aborted) {
      toast('Cancelled — no noodles were harmed.', 'info');
      return undefined;
    }
    console.error(e);
    toast(e instanceof Error ? e.message : String(e), 'error');
    return undefined;
  } finally {
    abort = null;
    setState({ busy: null });
  }
}
export function cancelBusy() {
  abort?.abort();
  engine.cancelAll();
}

// ---------- documents ----------
async function registerSource(name: string, bytes: Uint8Array): Promise<Source> {
  const key = newId('s');
  const info = await engine.open(key, bytes);
  try {
    await loadPdf(key, bytes);
  } catch (e) {
    engine.forget(key);
    unloadPdf(key);
    throw e;
  }
  return { key, name, bytes, info };
}

const MAX_HISTORY = 15;
function gcSources(keep: Doc[]) {
  const used = new Set<string>();
  for (const d of keep) for (const k of Object.keys(d.sources)) used.add(k);
  for (const d of [state.doc, ...state.past, ...state.future]) {
    for (const k of Object.keys(d.sources)) {
      if (!used.has(k)) {
        used.add(k); // only forget once
        engine.forget(k);
        unloadPdf(k);
      }
    }
  }
}

/** Replace the document, recording undo history. */
export function setDoc(doc: Doc, opts: { history?: boolean } = {}) {
  const history = opts.history ?? true;
  const past = history ? [...state.past, state.doc].slice(-MAX_HISTORY) : state.past;
  const future = history ? [] : state.future;
  const live = [doc, ...past, ...future];
  gcSources(live);
  const ids = new Set(doc.pages.map((p) => p.id));
  setState({
    doc,
    past,
    future,
    selected: state.selected.filter((id) => ids.has(id)),
    editPage: Math.min(state.editPage, Math.max(0, doc.pages.length - 1)),
  });
}
export function setPages(pages: PageRef[]) {
  const used = new Set(pages.map((p) => p.src));
  const sources = Object.fromEntries(Object.entries(state.doc.sources).filter(([k]) => used.has(k)));
  setDoc({ sources, pages });
}
export function undo() {
  const prev = state.past[state.past.length - 1];
  if (!prev) return;
  setState({
    doc: prev,
    past: state.past.slice(0, -1),
    future: [state.doc, ...state.future],
    selected: [],
  });
  toast('Undone ↩︎');
}
export function redo() {
  const next = state.future[0];
  if (!next) return;
  setState({
    doc: next,
    past: [...state.past, state.doc],
    future: state.future.slice(1),
    selected: [],
  });
  toast('Redone ↪︎');
}
export function clearAll() {
  gcSources([]);
  setState({
    doc: { sources: {}, pages: [] },
    past: [],
    future: [],
    selected: [],
    editPage: 0,
    baseName: 'document',
    saveFlags: SAVE_DEFAULT,
    suffix: '',
  });
}

const isPdf = (f: File) => f.type === 'application/pdf' || /\.pdf$/i.test(f.name);
const isImage = (f: File) => f.type.startsWith('image/') || /\.(png|jpe?g|webp|gif|bmp|avif)$/i.test(f.name);
const mimeOf = (f: File) => f.type || (/\.png$/i.test(f.name) ? 'image/png' : /\.jpe?g$/i.test(f.name) ? 'image/jpeg' : 'image/webp');

/** Open PDFs and images; everything is appended to the current workspace (= merge). */
export async function addFiles(files: File[], imageMode: PageSizeMode = 'fit-a4') {
  const pdfs = files.filter(isPdf);
  const images = files.filter((f) => !isPdf(f) && isImage(f));
  const unknown = files.filter((f) => !isPdf(f) && !isImage(f));
  if (unknown.length) toast(`I can only eat PDFs and images — skipped ${unknown.map((f) => f.name).join(', ')}`, 'error');
  if (!pdfs.length && !images.length) return;
  await withBusy(files.length > 1 ? `Slurping ${files.length} files…` : `Slurping ${files[0].name}…`, async ({ onProgress }) => {
    const newSources: Source[] = [];
    let i = 0;
    for (const f of files) {
      if (!isPdf(f)) continue;
      onProgress(i++, files.length);
      try {
        const bytes = new Uint8Array(await f.arrayBuffer());
        const src = await registerSource(f.name, bytes);
        newSources.push(src);
        if (src.info.repaired) toast(`${f.name} was a bit tangled — I repaired it.`, 'info');
      } catch (e) {
        toast(`${f.name}: ${e instanceof Error ? e.message : e}`, 'error');
      }
    }
    if (images.length) {
      const specs = await Promise.all(
        images.map(async (f) => ({
          bytes: new Uint8Array(await f.arrayBuffer()),
          mime: mimeOf(f),
        })),
      );
      const pdf = await engine.imagesToPdf(specs, imageMode, 24, {
        onProgress,
      });
      newSources.push(await registerSource(images.length === 1 ? images[0].name.replace(/\.[^.]+$/, '') + '.pdf' : 'images.pdf', pdf));
    }
    if (!newSources.length) return;
    const sources = { ...state.doc.sources };
    const pages = [...state.doc.pages];
    for (const s of newSources) {
      sources[s.key] = s;
      for (let p = 0; p < s.info.pages; p++) pages.push({ id: newId(), src: s.key, index: p, rot: 0 });
    }
    const wasEmpty = state.doc.pages.length === 0;
    setDoc({ sources, pages });
    if (wasEmpty)
      setState({
        baseName: newSources[0].name.replace(/\.[^.]+$/, '') || 'document',
      });
    const total = newSources.reduce((n, s) => n + s.info.pages, 0);
    toast(`Added ${total} page${total === 1 ? '' : 's'} 🍝`, 'success');
  });
}

/** True if the workspace is exactly one source in natural order with no pending rotations. */
function isNormalized(doc: Doc): string | null {
  const keys = Object.keys(doc.sources);
  if (keys.length !== 1) return null;
  const src = doc.sources[keys[0]];
  if (doc.pages.length !== src.info.pages) return null;
  for (let i = 0; i < doc.pages.length; i++) if (doc.pages[i].index !== i || doc.pages[i].rot !== 0 || doc.pages[i].src !== src.key) return null;
  return src.key;
}

export function partsOf(pages: PageRef[]): Part[] {
  return pages.map((p) => ({ key: p.src, page: p.index, rot: p.rot }));
}

/** Materialize the arrangement into a single source so edits can be applied. */
export async function commit(opts?: { onProgress?: (d: number, t: number) => void; signal?: AbortSignal }): Promise<Source> {
  const k = isNormalized(state.doc);
  if (k) return state.doc.sources[k];
  const bytes = await engine.assemble(partsOf(state.doc.pages), SAVE_DEFAULT, opts);
  const src = await registerSource(`${state.baseName}.pdf`, bytes);
  // Not an undoable step by itself: swap silently but keep page ids where possible
  const pages = state.doc.pages.map((p, i) => ({
    id: p.id,
    src: src.key,
    index: i,
    rot: 0,
  }));
  setDoc({ sources: { [src.key]: src }, pages }, { history: false });
  return src;
}

/** Apply engine edits to the (committed) document as one undoable step. */
export async function applyEdits(ops: EditOp[], flags?: number, ctx?: { onProgress?: (d: number, t: number) => void; signal?: AbortSignal }) {
  const src = await commit(ctx);
  const res = await engine.edit(src.key, ops, flags ?? state.saveFlags, ctx);
  await replaceWithBytes(res.bytes, src.name);
  return res;
}

export async function replaceWithBytes(bytes: Uint8Array, name: string) {
  const ids = state.doc.pages.map((p) => p.id);
  const ns = await registerSource(name, bytes);
  const pages: PageRef[] = [];
  for (let i = 0; i < ns.info.pages; i++) pages.push({ id: ids[i] ?? newId(), src: ns.key, index: i, rot: 0 });
  setDoc({ sources: { [ns.key]: ns }, pages });
  return ns;
}

/** Bytes for download/export of the current arrangement. */
export async function exportBytes(ctx?: { onProgress?: (d: number, t: number) => void; signal?: AbortSignal }, pages = state.doc.pages) {
  if (pages === state.doc.pages) {
    const k = isNormalized(state.doc);
    if (k) return state.doc.sources[k].bytes; // already saved with the right flags by the last edit
  }
  return engine.assemble(partsOf(pages), state.saveFlags, ctx);
}

export function record(step: Step) {
  if (state.recording) setState((s) => ({ recording: [...(s.recording ?? []), step] }));
}
