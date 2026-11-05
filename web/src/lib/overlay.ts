// Converts on-screen annotations (in visual page points, top-left origin) to PDF content operators.
import type { OverlayImage, PageInfo } from '../engine/types';
import { HELVETICA, HELVETICA_BOLD } from './metrics';

export type Annot =
  | {
      id: string;
      kind: 'text';
      x: number;
      y: number;
      text: string;
      size: number;
      color: string;
      bold?: boolean;
    }
  | {
      id: string;
      kind: 'rect' | 'ellipse';
      x: number;
      y: number;
      w: number;
      h: number;
      color: string;
      width: number;
      fill?: string | null;
    }
  | {
      id: string;
      kind: 'line' | 'arrow';
      x1: number;
      y1: number;
      x2: number;
      y2: number;
      color: string;
      width: number;
    }
  | {
      id: string;
      kind: 'highlight';
      x: number;
      y: number;
      w: number;
      h: number;
      color: string;
    }
  | {
      id: string;
      kind: 'ink';
      points: [number, number][];
      color: string;
      width: number;
    }
  | {
      id: string;
      kind: 'image';
      x: number;
      y: number;
      w: number;
      h: number;
      rgba: Uint8ClampedArray;
      iw: number;
      ih: number;
      url?: string;
      sigId?: number;
    }
  | { id: string; kind: 'redact'; x: number; y: number; w: number; h: number };

const n = (v: number) => (Math.round(v * 1000) / 1000).toString();

export function hexToRgb(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim());
  if (!m) return [0, 0, 0];
  return [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255];
}
const rgb = (hex: string, op: 'rg' | 'RG') => hexToRgb(hex).map(n).join(' ') + ' ' + op;

/** Unicode → WinAnsi bytes as a PDF hex string (unmappable characters become '?'). */
export function winAnsiHex(s: string): string {
  const high: Record<number, number> = {
    0x20ac: 0x80,
    0x201a: 0x82,
    0x0192: 0x83,
    0x201e: 0x84,
    0x2026: 0x85,
    0x2020: 0x86,
    0x2021: 0x87,
    0x02c6: 0x88,
    0x2030: 0x89,
    0x0160: 0x8a,
    0x2039: 0x8b,
    0x0152: 0x8c,
    0x017d: 0x8e,
    0x2018: 0x91,
    0x2019: 0x92,
    0x201c: 0x93,
    0x201d: 0x94,
    0x2022: 0x95,
    0x2013: 0x96,
    0x2014: 0x97,
    0x02dc: 0x98,
    0x2122: 0x99,
    0x0161: 0x9a,
    0x203a: 0x9b,
    0x0153: 0x9c,
    0x017e: 0x9e,
    0x0178: 0x9f,
  };
  let out = '<';
  for (const ch of s) {
    const u = ch.codePointAt(0)!;
    const c = u < 0x80 || (u >= 0xa0 && u <= 0xff) ? u : (high[u] ?? 0x3f);
    out += c.toString(16).padStart(2, '0');
  }
  return out + '>';
}
export function winAnsiCodes(s: string): number[] {
  return [...winAnsiHex(s).slice(1, -1).matchAll(/../g)].map((m) => parseInt(m[0], 16));
}
export function textWidth(s: string, size: number, bold = false): number {
  const t = bold ? HELVETICA_BOLD : HELVETICA;
  return (winAnsiCodes(s).reduce((w, c) => w + (t[c] || 500), 0) * size) / 1000;
}

/** cm operator that makes subsequent drawing use visual (as displayed) bottom-left coordinates. */
export const frameCm = (pi: PageInfo) => `${pi.m.map(n).join(' ')} cm`;

const KAPPA = 0.5522847498;
function ellipsePath(x: number, y: number, w: number, h: number) {
  const rx = w / 2,
    ry = h / 2,
    cx = x + rx,
    cy = y + ry,
    ox = rx * KAPPA,
    oy = ry * KAPPA;
  return [
    `${n(cx + rx)} ${n(cy)} m`,
    `${n(cx + rx)} ${n(cy + oy)} ${n(cx + ox)} ${n(cy + ry)} ${n(cx)} ${n(cy + ry)} c`,
    `${n(cx - ox)} ${n(cy + ry)} ${n(cx - rx)} ${n(cy + oy)} ${n(cx - rx)} ${n(cy)} c`,
    `${n(cx - rx)} ${n(cy - oy)} ${n(cx - ox)} ${n(cy - ry)} ${n(cx)} ${n(cy - ry)} c`,
    `${n(cx + ox)} ${n(cy - ry)} ${n(cx + rx)} ${n(cy - oy)} ${n(cx + rx)} ${n(cy)} c`,
  ].join('\n');
}

/** Build overlay content for one page. Coordinates in annots are visual points from the top-left. */
export function annotsToOps(annots: Annot[], pi: PageInfo): { ops: string; images: OverlayImage[] } {
  const H = pi.vh;
  const out: string[] = ['q', frameCm(pi)];
  const images: OverlayImage[] = [];
  for (const a of annots) {
    switch (a.kind) {
      case 'text': {
        const lines = a.text.split('\n');
        const font = a.bold ? 'Helvetica-Bold' : 'Helvetica';
        out.push(`BT /%F:${font}% ${n(a.size)} Tf ${rgb(a.color, 'rg')} ${n(a.size * 1.2)} TL`);
        out.push(`${n(a.x)} ${n(H - a.y - a.size * 0.8)} Td`);
        lines.forEach((l, i) => out.push(`${i ? 'T* ' : ''}${winAnsiHex(l)} Tj`));
        out.push('ET');
        break;
      }
      case 'rect':
      case 'ellipse': {
        const y = H - a.y - a.h;
        out.push(`q ${n(a.width)} w ${rgb(a.color, 'RG')}${a.fill ? ' ' + rgb(a.fill, 'rg') : ''}`);
        out.push(a.kind === 'rect' ? `${n(a.x)} ${n(y)} ${n(a.w)} ${n(a.h)} re` : ellipsePath(a.x, y, a.w, a.h));
        out.push(a.fill ? 'B Q' : 'S Q');
        break;
      }
      case 'line':
      case 'arrow': {
        const y1 = H - a.y1,
          y2 = H - a.y2;
        out.push(`q ${n(a.width)} w 1 J 1 j ${rgb(a.color, 'RG')} ${rgb(a.color, 'rg')} ${n(a.x1)} ${n(y1)} m ${n(a.x2)} ${n(y2)} l S`);
        if (a.kind === 'arrow') {
          const ang = Math.atan2(y2 - y1, a.x2 - a.x1);
          const L = Math.max(8, a.width * 4);
          const p1 = [a.x2 - L * Math.cos(ang - 0.45), y2 - L * Math.sin(ang - 0.45)];
          const p2 = [a.x2 - L * Math.cos(ang + 0.45), y2 - L * Math.sin(ang + 0.45)];
          out.push(`${n(a.x2)} ${n(y2)} m ${n(p1[0])} ${n(p1[1])} l ${n(p2[0])} ${n(p2[1])} l h f`);
        }
        out.push('Q');
        break;
      }
      case 'highlight':
        out.push(`q /%G:0.4:0.4:Multiply% gs ${rgb(a.color, 'rg')} ${n(a.x)} ${n(H - a.y - a.h)} ${n(a.w)} ${n(a.h)} re f Q`);
        break;
      case 'ink': {
        if (a.points.length < 2) break;
        const pts = a.points.map(([x, y]) => [x, H - y]);
        const path = [`${n(pts[0][0])} ${n(pts[0][1])} m`];
        for (let i = 1; i < pts.length; i++) path.push(`${n(pts[i][0])} ${n(pts[i][1])} l`);
        out.push(`q ${n(a.width)} w 1 J 1 j ${rgb(a.color, 'RG')} ${path.join(' ')} S Q`);
        break;
      }
      case 'image': {
        images.push({ rgba: a.rgba, width: a.iw, height: a.ih });
        out.push(`q ${n(a.w)} 0 0 ${n(a.h)} ${n(a.x)} ${n(H - a.y - a.h)} cm /%I:${images.length - 1}% Do Q`);
        break;
      }
      case 'redact':
        break;
    }
  }
  out.push('Q');
  return { ops: out.join('\n'), images };
}

/** Visual top-left rect → PDF user-space rect [x0,y0,x1,y1]. */
export function visualToUser(pi: PageInfo, x: number, y: number, w: number, h: number): number[] {
  const [a, b, c, d, e, f] = pi.m;
  const pts = [
    [x, pi.vh - y],
    [x + w, pi.vh - y - h],
  ].map(([vx, vy]) => [a * vx + c * vy + e, b * vx + d * vy + f]);
  return [Math.min(pts[0][0], pts[1][0]), Math.min(pts[0][1], pts[1][1]), Math.max(pts[0][0], pts[1][0]), Math.max(pts[0][1], pts[1][1])];
}

/** PDF user-space rect → visual top-left {x,y,w,h}. */
export function userToVisual(pi: PageInfo, r: number[]): { x: number; y: number; w: number; h: number } {
  const [a, b, c, d, e, f] = pi.m;
  const det = a * d - b * c || 1;
  const inv = (x: number, y: number) => {
    const X = x - e,
      Y = y - f;
    return [(d * X - c * Y) / det, (-b * X + a * Y) / det];
  };
  const p1 = inv(r[0], r[1]),
    p2 = inv(r[2], r[3]);
  const x0 = Math.min(p1[0], p2[0]),
    x1 = Math.max(p1[0], p2[0]);
  const vy0 = Math.min(p1[1], p2[1]),
    vy1 = Math.max(p1[1], p2[1]);
  return { x: x0, y: pi.vh - vy1, w: x1 - x0, h: vy1 - vy0 };
}
