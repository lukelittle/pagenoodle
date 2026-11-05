# 🍝 Pagenoodle

**A free, cute, goofy — and seriously private — alternative to Adobe Acrobat that runs entirely in your browser.**

Load PDFs and images, merge and split them, sign, fill forms, redact *for real*, compress, OCR, compare versions,
and download the result. No document, page, or byte of your data ever leaves your device. It even works with
Wi-Fi turned off.

- 🔒 **Provably private** — static site, strict Content Security Policy, offline-first service worker, no backend, no analytics.
- 🧠 **PDF engine written in C**, compiled to WebAssembly with Emscripten (180 KB).
- 📴 **Installable PWA** — everything (engine, OCR model, SQLite) is cached for offline use.
- ♿ **Accessible** — keyboard-first, screen-reader labels, reduced-motion support, automated axe checks.

---

## Features

| | |
|---|---|
| **Open** | PDFs and images (PNG, JPG, WebP…) via drag-and-drop anywhere, file picker, multiple files at once, or “Open with” once installed. Damaged PDFs are repaired automatically. |
| **Organize** | Thumbnail grid (lazy-rendered, smooth with hundreds of pages): drag or <kbd>Alt</kbd>+arrow to reorder, rotate, delete, duplicate, extract, reverse. |
| **Merge & split** | Open several files → they're merged in grid order. Split every *N* pages or by ranges (`1-3, 4-6, 7-`) into a ZIP. |
| **Annotate** | Text, boxes, ovals, lines, arrows, highlights and freehand ink — flattened into the page so they look the same everywhere. |
| **Fill forms** | Text, checkboxes, radio buttons, dropdowns, with generated appearances; optional flattening (“lock answers”). |
| **Sign** | Draw (mouse, finger, pen with pressure), type (handwriting font), or upload (with white-background removal). Optionally remember signatures locally. |
| **Page numbers, headers & footers** | `Page {n} of {total}` or any text, 6 positions, size, colour, margins, page ranges; respects rotated pages. |
| **Compress** | Lossless (object streams, dedup, max deflate) or re-encode large images (balanced / tiny). |
| **Convert** | Images → PDF (JPEGs embedded without recompression); PDF pages → PNG/JPG at 72/150/300 dpi. |
| **Download** | Sensible names: `contract-signed.pdf`, `report-merged.pdf`, `scan-searchable.pdf`… with a celebratory noodle wiggle. |

### What makes Pagenoodle special

1. **Provably private.** A visible *“Works with Wi-Fi off”* badge and a **Privacy page with live checks**: it shows the active CSP, makes
   a deliberate request to another server and shows the browser blocking it, confirms the service worker cache and where local data lives.
2. **True redaction.** Text under a box is *removed from the content stream* (not just covered), image pixels are wiped, form values and
   annotations are deleted, the file is fully rewritten, metadata can be stripped — and then Pagenoodle **re-opens the result and re-extracts
   the text to prove the redacted content is gone**. “Find & redact” marks every occurrence of a name or number.
3. **Recipes.** Hit record, do your routine (e.g. merge → compress → number pages → sign), save it. Replay it on new files in one click.
   Recipes live in local SQLite and can be exported/imported as a small JSON file.
4. **Compare two versions.** Side-by-side text with additions and deletions highlighted, plus a page-by-page change summary.
5. **In-browser OCR.** Make scans searchable and selectable with an invisible text layer, using Tesseract compiled to WebAssembly. The
   English model ships with the app and is cached offline — never fetched from a third party.

---

## Privacy model

Pagenoodle is a static website. There is **no server-side code** anywhere, so there is nothing that *could* receive your files.

| Layer | What it does |
|---|---|
| Static hosting | GitHub Pages serves only HTML/JS/WASM files. No API, no accounts, no cookies, no analytics. |
| **Content Security Policy** | `default-src 'none'; connect-src 'self'; script-src 'self' 'wasm-unsafe-eval'; …` — the browser refuses any connection to other origins. Delivered as a `<meta>` tag because Pages can't set headers. |
| **Service worker** | Precaches the whole app and returns a network error for *every* cross-origin request. |
| **Network guard** | `fetch`, `XHR`, `WebSocket`, `EventSource`, `RTCPeerConnection`, `sendBeacon` and `importScripts` to other origins throw — in the page *and in every worker* (a `<meta>` CSP does not reach workers). |
| **Vendored assets** | PDF.js data, SQLite, Tesseract core and language model are served from the same origin; library CDN defaults are overridden. |
| Local storage only | Recipes, saved signatures and settings go into SQLite (WASM) in the Origin Private File System, or IndexedDB as a fallback. Documents stay in memory only. |

The E2E test-suite fails if the app ever makes a request to another origin.

### Verify it yourself (offline test)

1. Open the deployed site once (it caches itself — the badge turns to **“Works with Wi-Fi off”**).
2. **Turn Wi-Fi off** (or DevTools → Network → *Offline*) and **reload**. The app loads and the badge says *“You're offline — still works!”*.
3. Open a PDF, sign it, redact it, OCR it, download it. All of it works.
4. With Wi-Fi on, open DevTools → **Network** while using the app: only same-origin static files appear (served from the service worker) — never your document.
5. View the page source and read the `Content-Security-Policy` meta tag, or open the in-app **Privacy** tab for live checks.

**Self-hosting tip:** if your server can send headers, also send the CSP as an HTTP header (this additionally covers workers at the browser level):

```
Content-Security-Policy: default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self'; connect-src 'self'; img-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; font-src 'self' data:; manifest-src 'self'; media-src 'self' blob:; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'
```

---

## Library choices & licenses

Pagenoodle is **MIT-licensed**. Every runtime dependency is permissive — **no GPL/AGPL** — so anyone can fork and host it.

| Need | Choice | License | Why |
|---|---|---|---|
| PDF engine (parse, repair, write, merge/split, forms, overlays, text, redaction, compression) | **Pagenoodle engine** (this repo, C99) + **miniz** | MIT + MIT | Small (180 KB WASM), full control of the security-critical redaction path. MuPDF/Ghostscript were rejected because AGPL would bind every host and fork; PDFium is C++ and ~20 MB. |
| Page rendering | **PDF.js** | Apache-2.0 | Battle-tested renderer; used only to *draw* pages, never to write them. |
| Local database | **SQLite WASM** (official build) on **OPFS**, IndexedDB fallback | Public domain / Apache-2.0 | Real SQL, persistent, private to the origin. |
| OCR | **Tesseract** via `tesseract.js` + English `best_int` model | Apache-2.0 | Mature, runs locally in WASM. |
| UI | **Preact** + TypeScript, built with **Vite** | MIT | Tiny and fast. |
| Signature font | **Caveat** | SIL OFL 1.1 | Friendly handwriting style for typed signatures. |

Font metrics for the 14 standard PDF fonts come from Adobe's freely redistributable AFM files. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the full list, and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design,
the redaction algorithm, and known risks/limitations.

---

## Build locally

Requirements: **Node 22+**, **Emscripten** (tested with 6.0.13), a C compiler and `make`.

```bash
# 1. Emscripten (once)
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 6.0.13 && ~/emsdk/emsdk activate 6.0.13
source ~/emsdk/emsdk_env.sh

# 2. Engine: native tests, then the WebAssembly build (writes web/public/engine/)
make -C engine test          # C test-suite (merge, split, redaction verification, …)
make -C engine asan          # same tests under AddressSanitizer + UBSan (incl. fuzzing corrupt files)
make -C engine wasm          # → web/public/engine/noodle.{mjs,wasm}
make -C engine wasm-test     # the C test-suite again, compiled to WASM and run in Node

# 3. Web app
cd web
npm ci
npm run dev                  # http://localhost:5173 (no CSP/service worker in dev)
npm run build                # production build in web/dist (CSP + precache manifest)
npm run preview              # serve the production build on :4173
npx playwright install chromium
npm run test:e2e             # end-to-end tests against the production build
```

Handy engine CLI for debugging: `engine/build/pnoodle info|text|merge|pages|redact|redact-text|compress|forms|fill|number …`.
Test fixtures are generated by an independent writer (ReportLab): `python3 engine/tests/make_fixtures.py engine/tests/fixtures`.

### Deploy

`.github/workflows/ci.yml` builds the engine with Emscripten, runs the native, sanitizer and WASM engine tests, builds the site, runs the
Playwright suite, and deploys `web/dist` to **GitHub Pages** on every push to `main`. Enable it once under *Settings → Pages → Source:
GitHub Actions*. The site uses relative paths, so it works at `https://<user>.github.io/<repo>/`.

## Repository layout

```
engine/            C PDF engine
  include/noodle.h   public nd_* API (exported to WASM)
  src/               parser, writer, pages, text, redaction, forms, overlays, images, stamps
  vendor/miniz/      deflate (MIT)
  cli/pnoodle.c      debugging CLI
  tests/             C test-suite + fixture generator
web/               Progressive Web App (Preact + TypeScript + Vite)
  src/engine/        engine worker + RPC client (progress & cancel)
  src/render/        PDF.js rendering, lazy thumbnails
  src/store/         app state, SQLite worker
  src/pipeline/      Steps shared by UI actions and recipes
  src/components/    UI views
  public/sw.js       offline service worker + cross-origin fence
  e2e/               Playwright tests
tools/gen_tables.py  generates font metric tables
docs/ARCHITECTURE.md design notes, redaction algorithm, risks
```

## Keyboard shortcuts

<kbd>Ctrl/⌘</kbd>+<kbd>O</kbd> open · <kbd>Ctrl/⌘</kbd>+<kbd>S</kbd> download · <kbd>Ctrl/⌘</kbd>+<kbd>Z</kbd> / <kbd>Shift</kbd>+<kbd>Z</kbd> undo/redo ·
in the page grid: arrows move focus, <kbd>Space</kbd> select, <kbd>Alt</kbd>+arrows move pages, <kbd>R</kbd>/<kbd>Shift</kbd>+<kbd>R</kbd> rotate,
<kbd>Del</kbd> delete · in the editor: <kbd>Del</kbd> removes the selected mark, arrows nudge, <kbd>PgUp</kbd>/<kbd>PgDn</kbd> change page,
<kbd>Esc</kbd> deselects.

## Known limitations (v1)

Encrypted PDFs aren't supported yet; added text is Latin-1 only (standard Helvetica); annotations are flattened; merging drops bookmarks;
OCR is English-only out of the box. Details in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#risks--limitations-v1).

## License

MIT © The Pagenoodle contributors. Third-party components retain their own licenses — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
