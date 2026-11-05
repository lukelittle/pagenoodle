// Small helpers: page ranges, downloads, zip, sizes.

/** Parse "1-3, 5, 8-", "odd", "even", "all", "last", "-2" into sorted unique 0-based indices. */
export function parseRanges(spec: string, count: number): number[] {
  const s = spec.trim().toLowerCase();
  const all = Array.from({ length: count }, (_, i) => i);
  if (!s || s === "all") return all;
  if (s === "odd") return all.filter((i) => i % 2 === 0);
  if (s === "even") return all.filter((i) => i % 2 === 1);
  const out = new Set<number>();
  for (const part0 of s.split(/[,;\s]+/)) {
    const part = part0.trim();
    if (!part) continue;
    if (part === "last") {
      if (count) out.add(count - 1);
      continue;
    }
    if (part === "first") {
      if (count) out.add(0);
      continue;
    }
    const m = /^(\d*)\s*-\s*(\d*|last)$/.exec(part);
    if (m) {
      const a = m[1] ? parseInt(m[1], 10) : 1;
      const b = m[2] === "last" || !m[2] ? count : parseInt(m[2], 10);
      for (let i = Math.max(1, a); i <= Math.min(count, b); i++) out.add(i - 1);
      continue;
    }
    const k = parseInt(part, 10);
    if (!Number.isNaN(k) && k >= 1 && k <= count) out.add(k - 1);
  }
  return [...out].sort((a, b) => a - b);
}

/** Split into groups of ranges, e.g. "1-3, 4-6" → [[0,1,2],[3,4,5]] (each comma group is a file). */
export function parseRangeGroups(spec: string, count: number): number[][] {
  return spec
    .split(/[,;]+/)
    .map((g) => parseRanges(g, count))
    .filter((g) => g.length);
}

export function chunk<T>(arr: T[], n: number): T[][] {
  const out: T[][] = [];
  for (let i = 0; i < arr.length; i += n) out.push(arr.slice(i, i + n));
  return out;
}

export function downloadBlob(
  data: Uint8Array | Blob,
  name: string,
  mime = "application/pdf",
) {
  const blob =
    data instanceof Blob ? data : new Blob([data as BlobPart], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

export function safeName(base: string): string {
  return (
    (base || "document")
      .replace(/[\\/:*?"<>|\u0000-\u001f]+/g, "_")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 120) || "document"
  );
}

export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

// ---------- minimal ZIP (store only) ----------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();
export function crc32(d: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < d.length; i++)
    c = CRC_TABLE[(c ^ d[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export function makeZip(files: { name: string; data: Uint8Array }[]): Blob {
  const enc = new TextEncoder();
  const parts: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  const used = new Set<string>();
  for (const f of files) {
    let name = f.name;
    for (let i = 2; used.has(name); i++)
      name = f.name.replace(/(\.[^.]*)?$/, ` (${i})$1`);
    used.add(name);
    const nb = enc.encode(name);
    const crc = crc32(f.data);
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true);
    lh.setUint16(4, 20, true);
    lh.setUint16(6, 0x0800, true); // UTF-8 names
    lh.setUint16(8, 0, true);
    lh.setUint32(14, crc, true);
    lh.setUint32(18, f.data.length, true);
    lh.setUint32(22, f.data.length, true);
    lh.setUint16(26, nb.length, true);
    parts.push(new Uint8Array(lh.buffer), nb, f.data);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true);
    ch.setUint16(4, 20, true);
    ch.setUint16(6, 20, true);
    ch.setUint16(8, 0x0800, true);
    ch.setUint32(16, crc, true);
    ch.setUint32(20, f.data.length, true);
    ch.setUint32(24, f.data.length, true);
    ch.setUint16(28, nb.length, true);
    ch.setUint32(42, offset, true);
    central.push(new Uint8Array(ch.buffer), nb);
    offset += 30 + nb.length + f.data.length;
  }
  const csize = central.reduce((n, p) => n + p.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, csize, true);
  end.setUint32(16, offset, true);
  return new Blob(
    [...parts, ...central, new Uint8Array(end.buffer)] as BlobPart[],
    { type: "application/zip" },
  );
}

export const prefersReducedMotion = () =>
  typeof matchMedia !== "undefined" &&
  matchMedia("(prefers-reduced-motion: reduce)").matches;
