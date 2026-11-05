// Recipe steps: every recordable operation is a serializable Step applied through one runner,
// so the exact same code path powers the UI buttons and one-click recipe replay.
import { engine } from "../engine/client";
import { SAVE, type EditOp } from "../engine/types";
import {
  getState,
  setDoc,
  applyEdits,
  commit,
  newId,
  setState,
  type PageRef,
} from "../store/app";
import { parseRanges, parseRangeGroups, chunk } from "../lib/util";
import { annotsToOps } from "../lib/overlay";
import { db } from "../store/db";
import { pngToCanvas, canvasPixels } from "../lib/images";
import { recognizePage, ocrOverlay } from "../lib/ocr";

export type Step =
  | { op: "merge" }
  | { op: "rotate"; pages: string; angle: number }
  | { op: "delete"; pages: string }
  | { op: "keep"; pages: string }
  | { op: "reverse" }
  | { op: "split"; every?: number; ranges?: string }
  | { op: "compress"; level: "lossless" | "balanced" | "strong" }
  | { op: "stripMetadata" }
  | {
      op: "stamp";
      template: string;
      position: number;
      size: number;
      margin: number;
      start: number;
      pages: string;
      font: string;
      color: number;
    }
  | { op: "redactText"; terms: string[] }
  | {
      op: "sign";
      signatureId: number;
      pages: string;
      x: number;
      y: number;
      w: number;
    }
  | { op: "flattenForms" }
  | { op: "ocr"; skipText: boolean };

export interface StepCtx {
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
}

/** Result of a run: usually the workspace is updated in place; split produces parts. */
export interface StepOutput {
  parts?: { name: string; pages: PageRef[] }[];
  report?: string;
}

const POSITIONS = [
  "top left",
  "top center",
  "top right",
  "bottom left",
  "bottom center",
  "bottom right",
];

export function describeStep(s: Step): string {
  switch (s.op) {
    case "merge":
      return "Merge all files in order";
    case "rotate":
      return `Rotate ${s.pages} pages by ${s.angle}°`;
    case "delete":
      return `Delete pages ${s.pages}`;
    case "keep":
      return `Keep only pages ${s.pages}`;
    case "reverse":
      return "Reverse page order";
    case "split":
      return s.every
        ? `Split every ${s.every} page${s.every > 1 ? "s" : ""}`
        : `Split into ${s.ranges}`;
    case "compress":
      return `Compress (${s.level})`;
    case "stripMetadata":
      return "Strip metadata";
    case "stamp":
      return `Stamp “${s.template}” at ${POSITIONS[s.position] ?? "bottom center"}`;
    case "redactText":
      return `Redact every “${s.terms.join("”, “")}”`;
    case "sign":
      return `Sign ${s.pages} page${s.pages === "all" ? "s" : ""}`;
    case "flattenForms":
      return "Flatten form fields";
    case "ocr":
      return "Make scanned pages searchable (OCR)";
  }
}

const pagesOf = () => getState().doc.pages;

export async function runStep(
  step: Step,
  ctx: StepCtx = {},
): Promise<StepOutput> {
  const st = getState();
  const pages = pagesOf();
  const n = pages.length;
  switch (step.op) {
    case "merge":
      return {}; // opening several files already appends them in order
    case "rotate": {
      const sel = new Set(parseRanges(step.pages, n));
      setDoc({
        ...st.doc,
        pages: pages.map((p, i) =>
          sel.has(i)
            ? { ...p, rot: (((p.rot + step.angle) % 360) + 360) % 360 }
            : p,
        ),
      });
      return {};
    }
    case "delete": {
      const sel = new Set(parseRanges(step.pages, n));
      if (sel.size >= n)
        throw new Error(
          "That would delete every page — I'll keep at least one noodle.",
        );
      setDoc({ ...st.doc, pages: pages.filter((_, i) => !sel.has(i)) });
      return {};
    }
    case "keep": {
      const sel = parseRanges(step.pages, n);
      if (!sel.length) throw new Error("No pages match that range.");
      setDoc({
        ...st.doc,
        pages: sel.map((i) => ({ ...pages[i], id: newId() })),
      });
      return {};
    }
    case "reverse":
      setDoc({ ...st.doc, pages: [...pages].reverse() });
      return {};
    case "split": {
      const groups = step.every
        ? chunk(
            pages.map((_, i) => i),
            Math.max(1, step.every),
          )
        : parseRangeGroups(step.ranges ?? "", n);
      if (!groups.length) throw new Error("No pages match that split.");
      const parts = groups.map((g) => ({
        name:
          g.length === 1
            ? `p${g[0] + 1}`
            : `p${g[0] + 1}-${g[g.length - 1] + 1}`,
        pages: g.map((i) => pages[i]),
      }));
      return { parts };
    }
    case "compress": {
      const flags =
        SAVE.GC |
        SAVE.COMPRESS |
        SAVE.OBJSTM |
        SAVE.DEDUP |
        SAVE.RECOMPRESS |
        (st.saveFlags & SAVE.STRIPMETA);
      const ops: EditOp[] = [];
      if (step.level === "balanced")
        ops.push({ type: "compressImages", quality: 0.72, maxDim: 2200 });
      if (step.level === "strong")
        ops.push({ type: "compressImages", quality: 0.5, maxDim: 1400 });
      const before = await currentSize(ctx);
      const res = await applyEdits(ops, flags, ctx);
      setState({
        saveFlags: st.saveFlags | SAVE.OBJSTM | SAVE.DEDUP | SAVE.RECOMPRESS,
      });
      return { report: `${before}→${res.bytes.length}` };
    }
    case "stripMetadata": {
      setState({ saveFlags: st.saveFlags | SAVE.STRIPMETA });
      await applyEdits(
        [],
        st.saveFlags | SAVE.GC | SAVE.COMPRESS | SAVE.STRIPMETA,
        ctx,
      );
      return {};
    }
    case "stamp": {
      const sel = parseRanges(step.pages, n);
      if (!sel.length) return {};
      // contiguous runs keep numbering simple: number relative to the first selected page
      const ops: EditOp[] = [];
      const runs: number[][] = [];
      for (const i of sel) {
        const last = runs[runs.length - 1];
        if (last && last[last.length - 1] === i - 1) last.push(i);
        else runs.push([i]);
      }
      let num = step.start;
      for (const r of runs) {
        ops.push({
          type: "stamp",
          template: step.template,
          position: step.position,
          size: step.size,
          margin: step.margin,
          from: r[0],
          to: r[r.length - 1],
          start: num,
          font: step.font,
          color: step.color,
        });
        num += r.length;
      }
      // {total} should be the overall count, not per run
      for (const o of ops)
        if (o.type === "stamp")
          o.template = o.template.replaceAll(
            "{total}",
            String(step.start + sel.length - 1),
          );
      await applyEdits(ops, undefined, ctx);
      return {};
    }
    case "redactText": {
      const src = await commit(ctx);
      const byPage = new Map<number, number[][]>();
      for (const term of step.terms) {
        if (!term.trim()) continue;
        const hits = await engine.search(src.key, term, ctx);
        for (const h of hits)
          byPage.set(h.p, [...(byPage.get(h.p) ?? []), ...h.r]);
      }
      if (!byPage.size) return { report: "nothing found" };
      const ops: EditOp[] = [...byPage].map(([page, rects]) => ({
        type: "redact",
        page,
        rects: rects.map(pad),
        fill: 0,
      }));
      const res = await applyEdits(
        ops,
        getState().saveFlags | SAVE.GC | SAVE.COMPRESS,
        ctx,
      );
      const v = await engine.verify(
        res.bytes,
        [...byPage].map(([page, rects]) => ({ page, rects: rects.map(pad) })),
        step.terms,
      );
      if (v.remaining || v.stillFound.length)
        throw new Error(
          `Redaction verification failed: ${v.stillFound.join(", ") || v.leaked}`,
        );
      return {
        report: `${[...byPage.values()].reduce((a, r) => a + r.length, 0)} areas redacted and verified`,
      };
    }
    case "sign": {
      const sigs = await db.listSignatures();
      const sig = sigs.find((s) => s.id === step.signatureId) ?? sigs[0];
      if (!sig)
        throw new Error(
          "That saved signature is missing — save one in Fill & Sign first.",
        );
      const canvas = await pngToCanvas(sig.png);
      const px = canvasPixels(canvas);
      const src = await commit(ctx);
      const ops: EditOp[] = [];
      for (const i of parseRanges(step.pages, n)) {
        const pi = src.info.pageInfo[i];
        const w = step.w * pi.vw;
        const h = (w * px.h) / px.w;
        const { ops: o, images } = annotsToOps(
          [
            {
              id: "s",
              kind: "image",
              x: step.x * pi.vw,
              y: step.y * pi.vh,
              w,
              h,
              rgba: px.rgba,
              iw: px.w,
              ih: px.h,
            },
          ],
          pi,
        );
        ops.push({ type: "overlay", page: i, ops: o, images });
      }
      await applyEdits(ops, undefined, ctx);
      return {};
    }
    case "flattenForms":
      await applyEdits([{ type: "formFlatten" }], undefined, ctx);
      return {};
    case "ocr": {
      const src = await commit(ctx);
      const ops: EditOp[] = [];
      let done = 0;
      for (let i = 0; i < src.info.pages; i++) {
        if (ctx.signal?.aborted)
          throw new DOMException("aborted", "AbortError");
        if (step.skipText) {
          const t = await engine.text(src.key, i);
          if (t.text.replace(/\s+/g, "").length > 30) {
            done++;
            continue;
          }
        }
        const pi = src.info.pageInfo[i];
        const words = await recognizePage(
          src.key,
          i,
          pi,
          (p) => ctx.onProgress?.(done + p, src.info.pages),
          ctx.signal,
        );
        if (words.length) ops.push(ocrOverlay(i, words, pi));
        done++;
        ctx.onProgress?.(done, src.info.pages);
      }
      if (!ops.length)
        return {
          report: "No scanned pages needed OCR — they already have text.",
        };
      await applyEdits(ops, undefined, ctx);
      return {
        report: `${ops.length} page${ops.length > 1 ? "s" : ""} made searchable`,
      };
    }
  }
}

/** Grow a text box slightly so glyph edges are fully covered. */
function pad(r: number[]): number[] {
  const h = r[3] - r[1];
  const p = Math.max(0.5, h * 0.06);
  return [r[0], r[1] - p, r[2], r[3] + p]; // vertical only: never nibble the neighbouring glyphs
}

async function currentSize(ctx: StepCtx) {
  const src = await commit(ctx);
  return src.bytes.length;
}
