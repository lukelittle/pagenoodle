// Myers O(ND) diff over token arrays, plus a document-level compare that reports per-page changes.

export type DiffOp = {
  type: "eq" | "del" | "ins";
  a0: number;
  a1: number;
  b0: number;
  b1: number;
};

export function myers<T>(a: T[], b: T[], maxD = 4000): DiffOp[] {
  // trim common prefix/suffix (cheap and makes D small)
  let pre = 0;
  while (pre < a.length && pre < b.length && a[pre] === b[pre]) pre++;
  let suf = 0;
  while (
    suf < a.length - pre &&
    suf < b.length - pre &&
    a[a.length - 1 - suf] === b[b.length - 1 - suf]
  )
    suf++;
  const A = a.slice(pre, a.length - suf);
  const B = b.slice(pre, b.length - suf);
  const N = A.length,
    M = B.length,
    MAX = N + M;
  const ops: DiffOp[] = [];
  const push = (
    type: DiffOp["type"],
    a0: number,
    a1: number,
    b0: number,
    b1: number,
  ) => {
    if (a1 - a0 === 0 && b1 - b0 === 0) return;
    const last = ops[ops.length - 1];
    if (last && last.type === type && last.a1 === a0 && last.b1 === b0) {
      last.a1 = a1;
      last.b1 = b1;
    } else ops.push({ type, a0, a1, b0, b1 });
  };
  push("eq", 0, pre, 0, pre);
  if (N === 0 || M === 0 || MAX === 0) {
    push("del", pre, pre + N, pre, pre);
    push("ins", pre + N, pre + N, pre, pre + M);
  } else {
    const off = MAX + 1;
    const V = new Int32Array(2 * MAX + 3);
    const trace: Int32Array[] = [];
    let found = -1;
    const limit = Math.min(MAX, maxD);
    for (let d = 0; d <= limit && found < 0; d++) {
      trace.push(V.slice());
      for (let k = -d; k <= d; k += 2) {
        let x =
          k === -d || (k !== d && V[off + k - 1] < V[off + k + 1])
            ? V[off + k + 1]
            : V[off + k - 1] + 1;
        let y = x - k;
        while (x < N && y < M && A[x] === B[y]) {
          x++;
          y++;
        }
        V[off + k] = x;
        if (x >= N && y >= M) {
          found = d;
          break;
        }
      }
    }
    if (found < 0) {
      // too different: treat the middle as a replacement
      push("del", pre, pre + N, pre, pre);
      push("ins", pre + N, pre + N, pre, pre + M);
    } else {
      // backtrack
      const path: [number, number, number, number][] = [];
      let x = N,
        y = M;
      for (let d = found; d > 0; d--) {
        const Vd = trace[d];
        const k = x - y;
        const prevK =
          k === -d || (k !== d && Vd[off + k - 1] < Vd[off + k + 1])
            ? k + 1
            : k - 1;
        const prevX = Vd[off + prevK];
        const prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
          path.push([x - 1, y - 1, x, y]);
          x--;
          y--;
        }
        path.push([prevX, prevY, x, y]);
        x = prevX;
        y = prevY;
      }
      while (x > 0 && y > 0) {
        path.push([x - 1, y - 1, x, y]);
        x--;
        y--;
      }
      path.reverse();
      for (const [x0, y0, x1, y1] of path) {
        if (x1 - x0 === 1 && y1 - y0 === 1)
          push("eq", pre + x0, pre + x1, pre + y0, pre + y1);
        else if (x1 - x0 === 1)
          push("del", pre + x0, pre + x1, pre + y0, pre + y0);
        else if (y1 - y0 === 1)
          push("ins", pre + x0, pre + x0, pre + y0, pre + y1);
      }
    }
  }
  push("eq", a.length - suf, a.length, b.length - suf, b.length);
  return ops;
}

export interface Token {
  t: string;
  page: number;
}
export function tokenize(pages: string[]): Token[] {
  const out: Token[] = [];
  pages.forEach((text, page) => {
    for (const m of text.matchAll(/\S+/g)) out.push({ t: m[0], page });
  });
  return out;
}

export interface Segment {
  type: "eq" | "del" | "ins";
  text: string;
}
export interface PageSummary {
  pageA: number | null;
  pageB: number | null;
  added: number;
  removed: number;
}
export interface CompareResult {
  left: Segment[][]; // per page of A
  right: Segment[][]; // per page of B
  summary: PageSummary[];
  added: number;
  removed: number;
  identical: boolean;
}

export function compareDocs(pagesA: string[], pagesB: string[]): CompareResult {
  const ta = tokenize(pagesA),
    tb = tokenize(pagesB);
  const ops = myers(
    ta.map((t) => t.t),
    tb.map((t) => t.t),
  );
  const left: Segment[][] = pagesA.map(() => []);
  const right: Segment[][] = pagesB.map(() => []);
  const addSeg = (
    arr: Segment[][],
    page: number,
    type: Segment["type"],
    text: string,
  ) => {
    const segs = arr[page];
    const last = segs[segs.length - 1];
    if (last && last.type === type) last.text += " " + text;
    else segs.push({ type, text });
  };
  const perA = new Map<
    number,
    { added: number; removed: number; pageB: number | null }
  >();
  const perB = new Map<number, number>();
  let added = 0,
    removed = 0;
  for (const op of ops) {
    if (op.type === "eq") {
      for (let i = op.a0; i < op.a1; i++)
        addSeg(left, ta[i].page, "eq", ta[i].t);
      for (let j = op.b0; j < op.b1; j++)
        addSeg(right, tb[j].page, "eq", tb[j].t);
    } else if (op.type === "del") {
      for (let i = op.a0; i < op.a1; i++) {
        addSeg(left, ta[i].page, "del", ta[i].t);
        const e = perA.get(ta[i].page) ?? { added: 0, removed: 0, pageB: null };
        e.removed++;
        perA.set(ta[i].page, e);
        removed++;
      }
    } else {
      for (let j = op.b0; j < op.b1; j++) {
        addSeg(right, tb[j].page, "ins", tb[j].t);
        perB.set(tb[j].page, (perB.get(tb[j].page) ?? 0) + 1);
        added++;
      }
    }
  }
  const summary: PageSummary[] = [];
  const pages = Math.max(pagesA.length, pagesB.length);
  for (let p = 0; p < pages; p++) {
    const r = perA.get(p)?.removed ?? 0;
    const a = perB.get(p) ?? 0;
    summary.push({
      pageA: p < pagesA.length ? p : null,
      pageB: p < pagesB.length ? p : null,
      added: a,
      removed: r,
    });
  }
  return {
    left,
    right,
    summary,
    added,
    removed,
    identical: added === 0 && removed === 0,
  };
}
