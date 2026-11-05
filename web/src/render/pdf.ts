// Page rendering with pdf.js (Apache-2.0). Parsing happens in a guarded worker; drawing is
// queued with limited concurrency and thumbnails are cached, so hundreds of pages stay smooth.
import * as pdfjs from "pdfjs-dist";
import type { PDFDocumentProxy, PDFPageProxy } from "pdfjs-dist";

let port: Worker | null = null;
function ensureWorker() {
  if (!port) {
    port = new Worker(new URL("./pdfjs.worker.ts", import.meta.url), {
      type: "module",
      name: "pdfjs",
    });
    pdfjs.GlobalWorkerOptions.workerPort = port;
  }
}

const docs = new Map<string, Promise<PDFDocumentProxy>>();
const tasks = new Map<string, { destroy(): Promise<void> }>();
const vendor = (p: string) =>
  new URL(`vendor/pdfjs/${p}`, document.baseURI).href;

export function loadPdf(
  key: string,
  bytes: Uint8Array,
): Promise<PDFDocumentProxy> {
  ensureWorker();
  let d = docs.get(key);
  if (!d) {
    const task = pdfjs.getDocument({
      data: bytes.slice(), // pdf.js transfers the buffer; keep ours intact
      enableXfa: false,
      standardFontDataUrl: vendor("standard_fonts/"),
      cMapUrl: vendor("cmaps/"),
      cMapPacked: true,
      iccUrl: vendor("iccs/"),
      wasmUrl: vendor("wasm/"),
      useSystemFonts: false,
    });
    tasks.set(key, task);
    d = task.promise;
    docs.set(key, d);
  }
  return d;
}

export function unloadPdf(key: string) {
  docs.delete(key);
  tasks
    .get(key)
    ?.destroy()
    .catch(() => {});
  tasks.delete(key);
  for (const k of [...thumbCache.keys()])
    if (k.startsWith(key + ":")) thumbCache.delete(k);
}

export async function getPage(
  key: string,
  index: number,
): Promise<PDFPageProxy> {
  const d = docs.get(key);
  if (!d) throw new Error("document not loaded in renderer");
  return (await d).getPage(index + 1);
}

// ---------- queue ----------
type Job = { run: () => Promise<void>; priority: number };
const queue: Job[] = [];
let active = 0;
const MAX_ACTIVE = 3;
function pump() {
  while (active < MAX_ACTIVE && queue.length) {
    queue.sort((a, b) => b.priority - a.priority);
    const job = queue.shift()!;
    active++;
    job.run().finally(() => {
      active--;
      pump();
    });
  }
}
function enqueue<T>(
  fn: () => Promise<T>,
  priority: number,
  signal?: AbortSignal,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const job: Job = {
      priority,
      run: async () => {
        if (signal?.aborted)
          return reject(new DOMException("aborted", "AbortError"));
        try {
          resolve(await fn());
        } catch (e) {
          reject(e);
        }
      },
    };
    queue.push(job);
    signal?.addEventListener("abort", () => {
      const i = queue.indexOf(job);
      if (i >= 0) {
        queue.splice(i, 1);
        reject(new DOMException("aborted", "AbortError"));
      }
    });
    pump();
  });
}

export interface RenderOpts {
  /** extra rotation on top of the page's own /Rotate */
  rotation?: number;
  /** target CSS width in px (scale computed) or explicit scale */
  width?: number;
  scale?: number;
  dpr?: number;
  signal?: AbortSignal;
  priority?: number;
  /** draw form widgets etc. */
  annotations?: boolean;
}

/** Render a page to a new canvas. */
export function renderToCanvas(
  key: string,
  index: number,
  opts: RenderOpts = {},
): Promise<HTMLCanvasElement> {
  return enqueue(
    async () => {
      const page = await getPage(key, index);
      const rotation =
        (((page.rotate + (opts.rotation ?? 0)) % 360) + 360) % 360;
      const base = page.getViewport({ scale: 1, rotation });
      const dpr = opts.dpr ?? Math.min(window.devicePixelRatio || 1, 2);
      const scale =
        (opts.scale ?? (opts.width ? opts.width / base.width : 1)) * dpr;
      const vp = page.getViewport({ scale, rotation });
      const canvas = document.createElement("canvas");
      canvas.width = Math.max(1, Math.floor(vp.width));
      canvas.height = Math.max(1, Math.floor(vp.height));
      const task = page.render({
        canvas,
        viewport: vp,
        annotationMode:
          opts.annotations === false
            ? pdfjs.AnnotationMode.DISABLE
            : pdfjs.AnnotationMode.ENABLE,
      });
      const onAbort = () => task.cancel();
      opts.signal?.addEventListener("abort", onAbort);
      try {
        await task.promise;
      } finally {
        opts.signal?.removeEventListener("abort", onAbort);
      }
      return canvas;
    },
    opts.priority ?? 0,
    opts.signal,
  );
}

// ---------- thumbnail cache (LRU of ImageBitmaps) ----------
const thumbCache = new Map<string, ImageBitmap | HTMLCanvasElement>();
const THUMB_MAX = 500;

export async function thumbnail(
  key: string,
  index: number,
  rotation: number,
  width: number,
  signal?: AbortSignal,
) {
  const ck = `${key}:${index}:${rotation}:${width}`;
  const hit = thumbCache.get(ck);
  if (hit) {
    thumbCache.delete(ck);
    thumbCache.set(ck, hit);
    return hit;
  }
  const canvas = await renderToCanvas(key, index, {
    rotation,
    width,
    signal,
    annotations: true,
  });
  let bmp: ImageBitmap | HTMLCanvasElement = canvas;
  try {
    bmp = await createImageBitmap(canvas);
  } catch {
    /* keep canvas */
  }
  thumbCache.set(ck, bmp);
  while (thumbCache.size > THUMB_MAX) {
    const first = thumbCache.keys().next().value as string;
    const v = thumbCache.get(first);
    if (v && "close" in v) v.close();
    thumbCache.delete(first);
  }
  return bmp;
}

export { pdfjs };
