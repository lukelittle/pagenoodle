/// <reference lib="webworker" />
// The engine worker: hosts the C/WebAssembly PDF engine off the main thread.
import { installNetGuard } from '../lib/netguard';
import type {
  DocInfo,
  EditOp,
  FormField,
  ImageSpec,
  OverlayImage,
  PageSizeMode,
  Part,
  RedactReport,
  SearchHit,
  Word,
  WorkerRequest,
  WorkerResponse,
} from './types';

installNetGuard();

/* eslint-disable @typescript-eslint/no-explicit-any */
type Engine = any;
let M: Engine | null = null;
let currentId = 0;
const docs = new Map<string, { bytes: Uint8Array; h: number }>();
const dec = new TextDecoder();

const post = (msg: WorkerResponse, transfer: Transferable[] = []) => (self as DedicatedWorkerGlobalScope).postMessage(msg, transfer);
const progress = (done: number, total: number) => post({ id: currentId, type: 'progress', done, total });

async function init(base: string) {
  if (M) return;
  const mod = await import(/* @vite-ignore */ new URL('engine/noodle.mjs', base).href);
  M = await mod.default({
    locateFile: (f: string) => new URL('engine/' + f, base).href,
    onProgress: (d: number, t: number) => progress(d, t),
  });
}

// ---------- low-level helpers ----------
function withBytes<T>(bytes: Uint8Array | Uint8ClampedArray, fn: (ptr: number) => T): T {
  const ptr = M._malloc(Math.max(1, bytes.length));
  M.HEAPU8.set(bytes, ptr);
  try {
    return fn(ptr);
  } finally {
    M._free(ptr);
  }
}
function withStr<T>(s: string, fn: (ptr: number) => T): T {
  const ptr = M.stringToNewUTF8(s);
  try {
    return fn(ptr);
  } finally {
    M._free(ptr);
  }
}
function withInts<T>(arr: number[], fn: (ptr: number) => T): T {
  const ptr = M._malloc(Math.max(4, arr.length * 4));
  M.HEAP32.set(arr, ptr >> 2);
  try {
    return fn(ptr);
  } finally {
    M._free(ptr);
  }
}
function withDoubles<T>(arr: number[], fn: (ptr: number) => T): T {
  const ptr = M._malloc(Math.max(8, arr.length * 8));
  M.HEAPF64.set(arr, ptr >> 3);
  try {
    return fn(ptr);
  } finally {
    M._free(ptr);
  }
}
const outBytes = (): Uint8Array => {
  const p = M._nd_out_ptr();
  const n = M._nd_out_len();
  return M.HEAPU8.slice(p, p + n);
};
const outStr = (): string => {
  const p = M._nd_out_ptr();
  const n = M._nd_out_len();
  return dec.decode(M.HEAPU8.subarray(p, p + n));
};
const lastError = (): string => M.UTF8ToString(M._nd_error());

function openBytes(bytes: Uint8Array): number {
  const h = withBytes(bytes, (p) => M._nd_open(p, bytes.length));
  if (!h) throw new Error(lastError() || 'Could not open this PDF.');
  return h;
}
function info(h: number): DocInfo {
  M._nd_info(h);
  return JSON.parse(outStr());
}
function save(h: number, flags: number): Uint8Array {
  const n = M._nd_save(h, flags);
  if (n < 0) throw new Error('Saving failed');
  const b = outBytes();
  M._nd_out_free();
  return b;
}
function getDoc(key: string) {
  const d = docs.get(key);
  if (!d) throw new Error(`Document ${key} is not loaded`);
  return d;
}

// ---------- images ----------
async function decodeToRGBA(bytes: Uint8Array, mime: string, maxDim = 0) {
  const bmp = await createImageBitmap(new Blob([bytes as BlobPart], { type: mime }));
  let w = bmp.width;
  let h = bmp.height;
  if (maxDim && Math.max(w, h) > maxDim) {
    const s = maxDim / Math.max(w, h);
    w = Math.max(1, Math.round(w * s));
    h = Math.max(1, Math.round(h * s));
  }
  const c = new OffscreenCanvas(w, h);
  const ctx = c.getContext('2d')!;
  ctx.drawImage(bmp, 0, 0, w, h);
  bmp.close();
  return { data: ctx.getImageData(0, 0, w, h).data, w, h, canvas: c };
}
function splitRGBA(rgba: Uint8ClampedArray | Uint8Array, w: number, h: number) {
  const n = w * h;
  const rgb = new Uint8Array(n * 3);
  const a = new Uint8Array(n);
  let opaque = true;
  for (let i = 0; i < n; i++) {
    rgb[i * 3] = rgba[i * 4];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    a[i] = rgba[i * 4 + 3];
    if (a[i] !== 255) opaque = false;
  }
  return { rgb, alpha: opaque ? null : a };
}
function newImage(h: number, img: OverlayImage): number {
  if (img.jpeg) return withBytes(img.jpeg, (p) => M._nd_image_new(h, p, img.jpeg!.length, 0, 0, 0, 1, 0));
  const { rgb, alpha } = splitRGBA(img.rgba!, img.width, img.height);
  return withBytes(rgb, (p) =>
    alpha
      ? withBytes(alpha, (ap) => M._nd_image_new(h, p, rgb.length, img.width, img.height, 3, 0, ap))
      : M._nd_image_new(h, p, rgb.length, img.width, img.height, 3, 0, 0),
  );
}
async function rasterizeJpeg(h: number, num: number) {
  const n = M._nd_stream_raw(h, num);
  if (n <= 0) return false;
  const raw = outBytes();
  try {
    const { data, w, h: ih } = await decodeToRGBA(raw, 'image/jpeg');
    const { rgb } = splitRGBA(data, w, ih);
    return withBytes(rgb, (p) => M._nd_image_replace(h, num, p, rgb.length, w, ih, 3, 0)) === 0;
  } catch {
    return false;
  }
}

// ---------- edits ----------
function resolvePlaceholders(ops: string, images: number[]): string {
  return ops
    .replace(/\/%F:([A-Za-z-]+)%/g, (_, f) => '/' + withStr(f, (p) => M.UTF8ToString(M._nd_ov_font(p))))
    .replace(
      /\/%G:([\d.]+):([\d.]+):([A-Za-z]*)%/g,
      (_, ca, CA, bm) => '/' + withStr(bm, (p) => M.UTF8ToString(M._nd_ov_gstate(parseFloat(ca), parseFloat(CA), p))),
    )
    .replace(/\/%I:(\d+)%/g, (_, i) => '/' + M.UTF8ToString(M._nd_ov_xobject(images[parseInt(i, 10)])));
}

async function applyOps(h: number, ops: EditOp[], dropUndecodable: boolean) {
  const reports: unknown[] = [];
  const needRaster = new Set<number>();
  M._nd_redact_mode(dropUndecodable ? 1 : 0);
  for (let i = 0; i < ops.length; i++) {
    const op = ops[i];
    progress(i, ops.length);
    switch (op.type) {
      case 'overlay': {
        const nums = (op.images ?? []).map((img) => newImage(h, img));
        if (M._nd_ov_begin(h, op.page) < 0) throw new Error('Bad page');
        const resolved = resolvePlaceholders(op.ops, nums);
        withStr(resolved, (p) => M._nd_ov_ops(p));
        M._nd_ov_end();
        reports.push(null);
        break;
      }
      case 'redact': {
        const flat = op.rects.flat();
        const rc = withDoubles(flat, (p) => M._nd_redact(h, op.page, p, op.rects.length, op.fill));
        const rep = JSON.parse(outStr()) as RedactReport;
        if (rc !== 0 || !rep.ok)
          throw new Error(
            (rep as RedactReport & { tooComplex?: boolean }).tooComplex
              ? `Page ${op.page + 1} is too complex for me to redact safely, so nothing was changed. Try “Pages → PNG/JPG”, then redact the image.`
              : `Redaction failed on page ${op.page + 1} — nothing was saved.`,
          );
        rep.needRaster?.forEach((n) => needRaster.add(n));
        reports.push(rep);
        break;
      }
      case 'scrubTerm':
        reports.push({ scrubbed: withStr(op.term, (p) => M._nd_scrub_term(h, p)) });
        break;
      case 'formSet':
        if (withStr(op.value, (p) => M._nd_form_set(h, op.id, p)) !== 0) throw new Error(lastError());
        reports.push(null);
        break;
      case 'formFlatten':
        reports.push(M._nd_form_flatten(h));
        break;
      case 'stamp':
        withStr(op.template, (tp) => withStr(op.font, (fp) => M._nd_stamp(h, tp, op.position, op.size, op.margin, op.from, op.to, op.start, fp, op.color)));
        reports.push(null);
        break;
      case 'compressImages':
        reports.push(await compressImages(h, op.quality, op.maxDim));
        break;
    }
  }
  return { reports, needRaster };
}

async function compressImages(h: number, quality: number, maxDim: number) {
  M._nd_images(h);
  const list = JSON.parse(outStr()) as {
    num: number;
    w: number;
    h: number;
    bpc: number;
    comps: number;
    mask: boolean;
    filter: string;
    len: number;
    decode: boolean;
    indexed: boolean;
  }[];
  let saved = 0;
  let count = 0;
  for (let i = 0; i < list.length; i++) {
    const im = list[i];
    progress(i, list.length);
    if (im.mask || im.bpc !== 8 || im.decode || im.w * im.h < 64 * 64) continue;
    let rgba: Uint8ClampedArray | null = null;
    let w = im.w;
    let hh = im.h;
    let canvas: OffscreenCanvas | null = null;
    try {
      if (im.filter === 'DCTDecode') {
        M._nd_stream_raw(h, im.num);
        const r = await decodeToRGBA(outBytes(), 'image/jpeg', maxDim);
        ({ w, h: hh, canvas } = r);
        rgba = r.data;
      } else if (im.filter === 'FlateDecode' || im.filter === '') {
        const comps = M._nd_image_pixels(h, im.num);
        if (!comps) continue;
        const px = outBytes();
        const id = new ImageData(im.w, im.h);
        for (let k = 0, n = im.w * im.h; k < n; k++) {
          const s = comps === 1 ? [px[k], px[k], px[k]] : [px[k * 3], px[k * 3 + 1], px[k * 3 + 2]];
          id.data[k * 4] = s[0];
          id.data[k * 4 + 1] = s[1];
          id.data[k * 4 + 2] = s[2];
          id.data[k * 4 + 3] = 255;
        }
        const src = new OffscreenCanvas(im.w, im.h);
        src.getContext('2d')!.putImageData(id, 0, 0);
        const s = maxDim && Math.max(im.w, im.h) > maxDim ? maxDim / Math.max(im.w, im.h) : 1;
        w = Math.max(1, Math.round(im.w * s));
        hh = Math.max(1, Math.round(im.h * s));
        canvas = new OffscreenCanvas(w, hh);
        canvas.getContext('2d')!.drawImage(src, 0, 0, w, hh);
        rgba = id.data;
      } else continue;
    } catch {
      continue;
    }
    if (!canvas || !rgba) continue;
    const blob = await canvas.convertToBlob({ type: 'image/jpeg', quality });
    const jpeg = new Uint8Array(await blob.arrayBuffer());
    if (jpeg.length < im.len * 0.9) {
      if (withBytes(jpeg, (p) => M._nd_image_replace(h, im.num, p, jpeg.length, w, hh, 3, 1)) === 0) {
        saved += im.len - jpeg.length;
        count++;
      }
    }
  }
  return { images: count, saved };
}

// ---------- RPC methods ----------
const methods: Record<string, (...args: any[]) => unknown> = {
  init,
  load(key: string, bytes: Uint8Array): DocInfo {
    const old = docs.get(key);
    if (old) M._nd_close(old.h);
    const h = openBytes(bytes);
    docs.set(key, { bytes, h });
    return info(h);
  },
  has(key: string) {
    return docs.has(key);
  },
  drop(key: string) {
    const d = docs.get(key);
    if (d) M._nd_close(d.h);
    docs.delete(key);
  },
  info(key: string) {
    return info(getDoc(key).h);
  },
  assemble(parts: Part[], flags: number): Uint8Array {
    const hs = parts.map((p) => getDoc(p.key).h);
    const h = withInts(hs, (hp) =>
      withInts(
        parts.map((p) => p.page),
        (pp) =>
          withInts(
            parts.map((p) => p.rot),
            (rp) => M._nd_assemble(hp, pp, rp, parts.length),
          ),
      ),
    );
    if (!h) throw new Error(lastError());
    try {
      return save(h, flags);
    } finally {
      M._nd_close(h);
    }
  },
  async edit(key: string, ops: EditOp[], flags: number) {
    const src = getDoc(key).bytes;
    let h = openBytes(src);
    try {
      let r = await applyOps(h, ops, false);
      if (r.needRaster.size) {
        // JPEG images under redaction boxes: convert them to pixels, then redo everything on a fresh copy
        M._nd_close(h);
        h = openBytes(src);
        for (const num of r.needRaster) await rasterizeJpeg(h, num);
        r = await applyOps(h, ops, true);
      }
      return { bytes: save(h, flags), reports: r.reports };
    } finally {
      M._nd_close(h);
    }
  },
  text(key: string, page: number): { text: string; words: Word[] } {
    M._nd_text(getDoc(key).h, page);
    return JSON.parse(outStr());
  },
  textAll(key: string): string[] {
    const h = getDoc(key).h;
    const n = M._nd_page_count(h);
    const out: string[] = [];
    for (let p = 0; p < n; p++) {
      M._nd_text_plain(h, p);
      out.push(outStr());
      if (p % 10 === 0) progress(p, n);
    }
    return out;
  },
  search(key: string, needle: string): SearchHit[] {
    withStr(needle, (p) => M._nd_search(getDoc(key).h, p, 1));
    return JSON.parse(outStr());
  },
  forms(key: string): FormField[] {
    M._nd_forms(getDoc(key).h);
    return JSON.parse(outStr());
  },
  verify(bytes: Uint8Array, items: { page: number; rects: number[][] }[], terms: string[]) {
    const h = openBytes(bytes);
    try {
      let remaining = 0;
      let leaked = '';
      for (const it of items) {
        remaining += withDoubles(it.rects.flat(), (p) => M._nd_verify(h, it.page, p, it.rects.length));
        leaked += JSON.parse(outStr()).leaked;
      }
      // byte-level scan of the whole file: page text, every string object, every decoded stream
      const stillFound: string[] = [];
      for (const t of terms) {
        if (withStr(t, (p) => M._nd_scan_term(h, p)) > 0) {
          const where = JSON.parse(outStr()) as { text: number; strings: number; streams: number };
          const parts = [where.text && 'page text', where.strings && 'annotations/fields/metadata', where.streams && 'embedded data'].filter(Boolean);
          stillFound.push(`${t} (${parts.join(', ')})`);
        }
      }
      return { remaining, leaked, stillFound };
    } finally {
      M._nd_close(h);
    }
  },
  async imagesToPdf(specs: ImageSpec[], mode: PageSizeMode, margin: number): Promise<Uint8Array> {
    const h = M._nd_new();
    try {
      for (let i = 0; i < specs.length; i++) {
        progress(i, specs.length);
        const s = specs[i];
        let num = 0;
        let w = 0;
        let ih = 0;
        if (s.mime === 'image/jpeg') {
          num = withBytes(s.bytes, (p) => M._nd_image_new(h, p, s.bytes.length, 0, 0, 0, 1, 0));
          if (num) {
            const bmp = await createImageBitmap(new Blob([s.bytes as BlobPart], { type: s.mime }));
            w = bmp.width;
            ih = bmp.height;
            bmp.close();
          }
        }
        if (!num) {
          const r = await decodeToRGBA(s.bytes, s.mime);
          w = r.w;
          ih = r.h;
          num = newImage(h, { rgba: r.data, width: r.w, height: r.h });
        }
        if (!num) throw new Error(lastError());
        let pw = 0;
        let ph = 0;
        if (mode !== 'original') {
          [pw, ph] = mode === 'fit-a4' ? [595.28, 841.89] : [612, 792];
          if (w > ih) [pw, ph] = [ph, pw];
        }
        M._nd_add_image_page(h, num, pw, ph, mode === 'original' ? 0 : margin);
      }
      return save(h, 1 | 2);
    } finally {
      M._nd_close(h);
    }
  },
  version() {
    return M.UTF8ToString(M._nd_version());
  },
};

self.onmessage = async (e: MessageEvent<WorkerRequest>) => {
  const { id, method, args } = e.data;
  currentId = id;
  try {
    if (method !== 'init' && !M) throw new Error('Engine not initialised');
    const fn = methods[method];
    if (!fn) throw new Error(`Unknown method ${method}`);
    const value = await fn(...args);
    const transfer: Transferable[] = [];
    if (value instanceof Uint8Array) transfer.push(value.buffer);
    else if (value && typeof value === 'object' && (value as { bytes?: unknown }).bytes instanceof Uint8Array)
      transfer.push((value as { bytes: Uint8Array }).bytes.buffer);
    post({ id, type: 'result', value }, transfer);
  } catch (err) {
    post({
      id,
      type: 'error',
      message: err instanceof Error ? err.message : String(err),
    });
  }
};
