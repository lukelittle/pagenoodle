// Copies third-party runtime assets into public/vendor so that EVERYTHING is served
// from our own origin (no CDN at runtime). Run automatically by `npm run dev/build`.
import { cpSync, mkdirSync, existsSync, rmSync, copyFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const nm = join(root, 'node_modules');
const out = join(root, 'public', 'vendor');
rmSync(out, { recursive: true, force: true });
const copy = (from, to) => {
  const src = join(nm, from);
  const dst = join(out, to);
  mkdirSync(dirname(dst), { recursive: true });
  cpSync(src, dst, { recursive: true });
};

// pdf.js (Apache-2.0): worker + font data + image decoder wasm
copy('pdfjs-dist/standard_fonts', 'pdfjs/standard_fonts');
copy('pdfjs-dist/cmaps', 'pdfjs/cmaps');
copy('pdfjs-dist/iccs', 'pdfjs/iccs');
for (const f of ['openjpeg.wasm', 'jbig2.wasm', 'qcms_bg.wasm', 'openjpeg_nowasm_fallback.js', 'jbig2_nowasm_fallback.js']) copy(`pdfjs-dist/wasm/${f}`, `pdfjs/wasm/${f}`);
copy('pdfjs-dist/LICENSE', 'pdfjs/LICENSE');

// SQLite WASM (public domain / Apache-2.0 wrapper)
copy('@sqlite.org/sqlite-wasm/dist/index.mjs', 'sqlite/index.mjs');
copy('@sqlite.org/sqlite-wasm/dist/sqlite3.wasm', 'sqlite/sqlite3.wasm');
copy('@sqlite.org/sqlite-wasm/dist/sqlite3-opfs-async-proxy.js', 'sqlite/sqlite3-opfs-async-proxy.js');

// Tesseract OCR (Apache-2.0): worker, LSTM-only cores, English model (best_int)
copy('tesseract.js/dist/worker.min.js', 'tesseract/worker.min.js');
copy('tesseract.js-core/tesseract-core-simd-lstm.wasm.js', 'tesseract/core/tesseract-core-simd-lstm.wasm.js');
copy('tesseract.js-core/tesseract-core-lstm.wasm.js', 'tesseract/core/tesseract-core-lstm.wasm.js');
copy('tesseract.js-core/LICENSE', 'tesseract/LICENSE');
copy('@tesseract.js-data/eng/4.0.0_best_int/eng.traineddata.gz', 'tessdata/eng.traineddata.gz');

if (!existsSync(join(root, 'public', 'engine', 'noodle.wasm'))) {
  console.warn('⚠️  public/engine/noodle.wasm missing — run `npm run engine` (needs Emscripten).');
}
console.log('vendor assets copied to public/vendor');
