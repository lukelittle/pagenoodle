// In-browser OCR with Tesseract (WASM). The model, core and worker are served from this origin and
// precached by the service worker — nothing is fetched from a third party, ever.
import { createWorker, OEM } from "tesseract.js";
import type { EditOp, PageInfo } from "../engine/types";
import { renderToCanvas } from "../render/pdf";
import { frameCm, textWidth, winAnsiHex } from "./overlay";

type TWorker = Awaited<ReturnType<typeof createWorker>>;
let worker: Promise<TWorker> | null = null;
let progressCb: ((p: number) => void) | null = null;

const SIMD_TEST = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8,
  0, 65, 0, 253, 15, 253, 98, 11,
]);

function getWorker(): Promise<TWorker> {
  if (!worker) {
    const base = document.baseURI;
    const simd =
      typeof WebAssembly !== "undefined" && WebAssembly.validate(SIMD_TEST);
    worker = createWorker("eng", OEM.LSTM_ONLY, {
      workerPath: new URL("ocr/tess-worker.js", base).href,
      corePath: new URL(
        `vendor/tesseract/core/tesseract-core-${simd ? "simd-" : ""}lstm.wasm.js`,
        base,
      ).href,
      langPath: new URL("vendor/tessdata", base).href,
      gzip: true,
      cacheMethod: "none",
      workerBlobURL: false,
      logger: (m: { status: string; progress: number }) => {
        if (m.status === "recognizing text") progressCb?.(m.progress);
      },
    });
    worker.catch(() => {
      worker = null;
    });
  }
  return worker;
}

export async function terminateOcr() {
  const w = worker;
  worker = null;
  if (w) (await w).terminate();
}

export interface OcrWord {
  text: string;
  x: number;
  y: number;
  w: number;
  h: number;
  conf: number;
}

/** Recognise a page; returns words in visual points (top-left origin). */
export async function recognizePage(
  key: string,
  index: number,
  pi: PageInfo,
  onProgress?: (p: number) => void,
  signal?: AbortSignal,
): Promise<OcrWord[]> {
  const scale = Math.min(3.5, 2400 / Math.max(pi.vw, pi.vh));
  const canvas = await renderToCanvas(key, index, {
    scale,
    dpr: 1,
    signal,
    priority: 5,
    annotations: true,
  });
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  const w = await getWorker();
  progressCb = onProgress ?? null;
  const abortHandler = () => void terminateOcr();
  signal?.addEventListener("abort", abortHandler);
  try {
    const res = await w.recognize(canvas, {}, { blocks: true, text: false });
    if (signal?.aborted) throw new DOMException("aborted", "AbortError");
    const words: OcrWord[] = [];
    for (const b of res.data.blocks ?? [])
      for (const p of b.paragraphs)
        for (const l of p.lines)
          for (const wd of l.words) {
            const t = wd.text.trim();
            if (!t || wd.confidence < 25) continue;
            words.push({
              text: t,
              x: wd.bbox.x0 / scale,
              y: wd.bbox.y0 / scale,
              w: (wd.bbox.x1 - wd.bbox.x0) / scale,
              h: (wd.bbox.y1 - wd.bbox.y0) / scale,
              conf: wd.confidence,
            });
          }
    return words;
  } finally {
    signal?.removeEventListener("abort", abortHandler);
    progressCb = null;
  }
}

/** Invisible (render mode 3) text layer placed exactly over each recognised word. */
export function ocrOverlay(
  page: number,
  words: OcrWord[],
  pi: PageInfo,
): EditOp {
  const n = (v: number) => (Math.round(v * 100) / 100).toString();
  const out: string[] = ["q", frameCm(pi), "BT 3 Tr"];
  for (const w of words) {
    const size = Math.max(4, w.h * 0.9);
    const natural = textWidth(w.text, size);
    const tz =
      natural > 0 ? Math.max(10, Math.min(500, (w.w / natural) * 100)) : 100;
    const baseline = pi.vh - (w.y + w.h) + size * 0.18;
    out.push(
      `/%F:Helvetica% ${n(size)} Tf ${n(tz)} Tz 1 0 0 1 ${n(w.x)} ${n(baseline)} Tm ${winAnsiHex(w.text + " ")} Tj`,
    );
  }
  out.push("ET", "Q");
  return { type: "overlay", page, ops: out.join("\n") };
}
