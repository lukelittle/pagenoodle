// Types shared between the UI thread and the engine worker.

export const SAVE = {
  GC: 1,
  COMPRESS: 2,
  OBJSTM: 4,
  DEDUP: 8,
  STRIPMETA: 16,
  RECOMPRESS: 32,
} as const;
export const SAVE_DEFAULT = SAVE.GC | SAVE.COMPRESS;

export interface PageInfo {
  rot: number;
  /** visual (as displayed) width/height in points */
  vw: number;
  vh: number;
  /** matrix mapping visual bottom-left-origin coords to PDF user space */
  m: [number, number, number, number, number, number];
}

export interface DocInfo {
  pages: number;
  repaired: boolean;
  hasXmp: boolean;
  hasForm: boolean;
  info: Record<string, string>;
  pageInfo: PageInfo[];
}

export interface Part {
  key: string;
  page: number;
  rot: number;
}

export interface OverlayImage {
  /** RGBA pixels (alpha becomes a soft mask) */
  rgba?: Uint8ClampedArray | Uint8Array;
  /** or JPEG bytes passed straight through */
  jpeg?: Uint8Array;
  width: number;
  height: number;
}

/**
 * Overlay content uses placeholders resolved by the worker:
 *   /%F:Helvetica%       → font resource
 *   /%G:ca:CA:Blend%     → ExtGState resource
 *   /%I:n%               → n-th image of `images`
 */
export type EditOp =
  | { type: 'overlay'; page: number; ops: string; images?: OverlayImage[] }
  | { type: 'redact'; page: number; rects: number[][]; fill: number }
  | { type: 'scrubTerm'; term: string }
  | { type: 'formSet'; id: number; value: string }
  | { type: 'formFlatten' }
  | {
      type: 'stamp';
      template: string;
      position: number;
      size: number;
      margin: number;
      from: number;
      to: number;
      start: number;
      font: string;
      color: number;
    }
  | { type: 'compressImages'; quality: number; maxDim: number };

export interface RedactReport {
  ok: boolean;
  glyphs: number;
  imagesRemoved: number;
  imagesScrubbed: number;
  forms: number;
  annots: number;
  removedText: string;
  needRaster?: number[];
}

export interface EditResult {
  bytes: Uint8Array;
  reports: unknown[];
}

export interface SearchHit {
  p: number;
  r: [number, number, number, number][];
}

export interface Word {
  t: string;
  l: number;
  b: [number, number, number, number];
}

export interface FormWidget {
  id: number;
  page: number;
  rect: [number, number, number, number];
  on: string;
  state: string;
}

export interface FormField {
  id: number;
  name: string;
  type: 'text' | 'checkbox' | 'radio' | 'combo' | 'list' | 'button' | 'signature';
  readonly: boolean;
  multiline: boolean;
  maxlen: number;
  value: string;
  options: string[];
  widgets: FormWidget[];
}

export interface ImageSpec {
  bytes: Uint8Array;
  mime: string;
}

export type PageSizeMode = 'fit-a4' | 'fit-letter' | 'original';

export interface WorkerRequest {
  id: number;
  method: string;
  args: unknown[];
}

export type WorkerResponse =
  | { id: number; type: 'result'; value: unknown }
  | { id: number; type: 'error'; message: string }
  | { id: number; type: 'progress'; done: number; total: number };
