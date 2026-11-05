// Canvas / image helpers for signatures and exports.

export interface Pixels {
  rgba: Uint8ClampedArray;
  w: number;
  h: number;
}

/** Crop a canvas to its non-transparent content (with padding). */
export function trimCanvas(
  src: HTMLCanvasElement,
  pad = 6,
): HTMLCanvasElement | null {
  const ctx = src.getContext("2d")!;
  const { width: W, height: H } = src;
  const d = ctx.getImageData(0, 0, W, H).data;
  let x0 = W,
    y0 = H,
    x1 = -1,
    y1 = -1;
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++)
      if (d[(y * W + x) * 4 + 3] > 8) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  if (x1 < 0) return null;
  x0 = Math.max(0, x0 - pad);
  y0 = Math.max(0, y0 - pad);
  x1 = Math.min(W - 1, x1 + pad);
  y1 = Math.min(H - 1, y1 + pad);
  const out = document.createElement("canvas");
  out.width = x1 - x0 + 1;
  out.height = y1 - y0 + 1;
  out
    .getContext("2d")!
    .drawImage(src, x0, y0, out.width, out.height, 0, 0, out.width, out.height);
  return out;
}

export function canvasPixels(c: HTMLCanvasElement): Pixels {
  return {
    rgba: c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data,
    w: c.width,
    h: c.height,
  };
}

export async function canvasPng(c: HTMLCanvasElement): Promise<Uint8Array> {
  const blob: Blob = await new Promise((res, rej) =>
    c.toBlob(
      (b) => (b ? res(b) : rej(new Error("encode failed"))),
      "image/png",
    ),
  );
  return new Uint8Array(await blob.arrayBuffer());
}

export async function pngToCanvas(png: Uint8Array): Promise<HTMLCanvasElement> {
  const bmp = await createImageBitmap(
    new Blob([png as BlobPart], { type: "image/png" }),
  );
  const c = document.createElement("canvas");
  c.width = bmp.width;
  c.height = bmp.height;
  c.getContext("2d")!.drawImage(bmp, 0, 0);
  bmp.close();
  return c;
}

/** Make near-white pixels transparent (for photographed/scanned signatures). */
export function knockOutWhite(c: HTMLCanvasElement, threshold = 225) {
  const ctx = c.getContext("2d")!;
  const img = ctx.getImageData(0, 0, c.width, c.height);
  const d = img.data;
  for (let i = 0; i < d.length; i += 4) {
    const lum = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
    if (lum >= threshold) d[i + 3] = 0;
    else if (lum > threshold - 40)
      d[i + 3] = Math.round((d[i + 3] * (threshold - lum)) / 40);
  }
  ctx.putImageData(img, 0, 0);
}

export function canvasToDataUrl(c: HTMLCanvasElement) {
  return c.toDataURL("image/png");
}
