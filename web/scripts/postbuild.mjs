// Generates the service-worker precache manifest from the built files, so the
// whole app (including OCR model + WASM) is available offline after first load.
import { readdirSync, statSync, readFileSync, writeFileSync } from 'node:fs';
import { join, relative, dirname } from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const dist = join(dirname(fileURLToPath(import.meta.url)), '..', 'dist');
const files = [];
const walk = (d) => {
  for (const f of readdirSync(d)) {
    const p = join(d, f);
    if (statSync(p).isDirectory()) walk(p);
    else files.push(p);
  }
};
walk(dist);
const skip = (rel) =>
  rel === 'sw.js' || rel.endsWith('.map') || /(^|\/)LICENSE/.test(rel) || (rel.startsWith('vendor/pdfjs/cmaps/') && !rel.endsWith('.bcmap'));
const hash = createHash('sha256');
hash.update(readFileSync(join(dist, 'sw.js'))); // a changed worker must get a fresh cache
const list = [];
for (const f of files.sort()) {
  const rel = relative(dist, f).split('\\').join('/');
  if (skip(rel)) continue;
  hash.update(rel);
  hash.update(readFileSync(f));
  list.push(rel === 'index.html' ? './' : rel);
}
const version = hash.digest('hex').slice(0, 12);
const swPath = join(dist, 'sw.js');
const sw = readFileSync(swPath, 'utf8').replace('__PRECACHE__', JSON.stringify(list)).replace('__VERSION__', version);
writeFileSync(swPath, sw);
writeFileSync(join(dist, 'version.json'), JSON.stringify({ version, files: list.length }));
console.log(`service worker: ${list.length} files precached, version ${version}`);
