// Independent redaction check with PDF.js — a different parser from the C engine that did the
// redacting, so a blind spot in one is unlikely to be shared by the other.
import { getPage, loadPdf, unloadPdf } from '../render/pdf';

export interface IndependentResult {
  /** characters PDF.js still sees under a redaction box */
  underBoxes: string;
  /** search terms PDF.js still finds in the page text */
  termsFound: string[];
}

const squash = (s: string) => s.toLowerCase().replace(/\s+/g, '');

export async function independentCheck(
  bytes: Uint8Array,
  items: { page: number; rects: number[][] }[],
  terms: string[],
  pageCount: number,
): Promise<IndependentResult> {
  const key = `verify-${Date.now()}-${Math.random().toString(36).slice(2)}`;
  await loadPdf(key, bytes);
  try {
    let underBoxes = '';
    const allText: string[] = [];
    const boxesByPage = new Map(items.map((it) => [it.page, it.rects]));
    for (let p = 0; p < pageCount; p++) {
      const page = await getPage(key, p);
      const content = await page.getTextContent();
      const rects = boxesByPage.get(p) ?? [];
      for (const it of content.items) {
        if (!('str' in it) || !it.str) continue;
        allText.push(it.str);
        if (!rects.length) continue;
        // Approximate each character's box along the text run (PDF user space).
        const [a, b, , , e, f] = it.transform as number[];
        const len = [...it.str].length;
        const dx = a / Math.hypot(a, b || 0 || 1e-9);
        const dy = (b || 0) / Math.hypot(a, b || 0 || 1e-9);
        const step = it.width / Math.max(1, len);
        const h = it.height || Math.hypot(a, b);
        [...it.str].forEach((ch, i) => {
          if (!ch.trim()) return;
          const cx = e + dx * step * (i + 0.5) - dy * h * 0.35;
          const cy = f + dy * step * (i + 0.5) + dx * h * 0.35;
          for (const r of rects) {
            // shrink slightly so neighbours that merely touch the box edge don't count
            if (cx > r[0] + 0.75 && cx < r[2] - 0.75 && cy > r[1] + 0.75 && cy < r[3] - 0.75) {
              underBoxes += ch;
              break;
            }
          }
        });
      }
    }
    const hay = squash(allText.join(''));
    const termsFound = terms.filter((t) => squash(t) && hay.includes(squash(t)));
    return { underBoxes, termsFound };
  } finally {
    unloadPdf(key);
  }
}

/** Does PDF.js agree with the engine about where this page is? (fail closed if not) */
export async function geometryMatches(srcKey: string, index: number, pi: { vw: number; vh: number; m: number[] }): Promise<boolean> {
  const page = await getPage(srcKey, index);
  const vp = page.getViewport({ scale: 1 });
  if (Math.abs(vp.width - pi.vw) > 0.5 || Math.abs(vp.height - pi.vh) > 0.5) return false;
  const [a, b, c, d, e, f] = pi.m;
  // visual top-left (0,0) and bottom-right (vw,vh) in CSS px → PDF user space, both ways
  for (const [x, y] of [
    [0, 0],
    [pi.vw, pi.vh],
    [pi.vw / 3, pi.vh / 5],
  ]) {
    const [px, py] = vp.convertToPdfPoint(x, y);
    const vx = x,
      vy = pi.vh - y;
    const ux = a * vx + c * vy + e,
      uy = b * vx + d * vy + f;
    if (Math.abs(px - ux) > 0.5 || Math.abs(py - uy) > 0.5) return false;
  }
  return true;
}
