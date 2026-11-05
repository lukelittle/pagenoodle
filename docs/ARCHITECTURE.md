# Pagenoodle architecture

```
┌──────────────────────────── browser tab (static site, CSP: connect-src 'self') ───────────────────────────┐
│                                                                                                          │
│  UI thread (Preact + TypeScript)                                                                         │
│   store/app.ts      documents, page arrangement, undo/redo, busy/progress/cancel                        │
│   pipeline/steps.ts every recordable operation as a serializable Step (powers buttons AND recipes)      │
│   components/*      Organize · Edit & Sign · Redact · Tools · Recipes · Compare · Privacy               │
│   lib/netguard.ts   blocks fetch/XHR/WebSocket/beacon to other origins (page + every worker)            │
│        │ postMessage                     │                          │                       │          │
│        ▼                                 ▼                          ▼                       ▼          │
│  engine.worker ─ noodle.wasm      pdfjs.worker (PDF.js)     db.worker ─ sqlite3.wasm    tess-worker      │
│  C PDF engine (this repo)         page rendering only       OPFS (SAH pool) or          Tesseract OCR    │
│  open/repair, merge/split,                                  IndexedDB snapshot          (LSTM, eng)      │
│  overlays, forms, redaction,                                recipes · signatures ·                       │
│  text, compression, images                                  settings                                     │
│                                                                                                          │
│  sw.js: precaches every file (≈220, incl. WASM + OCR model) and refuses all cross-origin requests       │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

## Why a custom C engine?

The brief asks for a document engine in C compiled with Emscripten and a license that suits a free,
public project. The candidates:

| Option | License | Verdict |
|---|---|---|
| MuPDF | AGPL-3.0 (or commercial) | Excellent, but AGPL forces the whole app (and every fork/host) under AGPL and imposes network-use obligations. Rejected to keep Pagenoodle permissive. |
| Ghostscript | AGPL-3.0 | Same problem, and huge. |
| PDFium | BSD-3 / Apache-2.0 | License fine, but C++, ~20 MB WASM, heavy Chromium-style build. Overkill for editing. |
| qpdf | Apache-2.0 | Great structural tool, but C++ and no content-level features (text extraction, redaction). |
| **Own engine in C99 + miniz (MIT)** | MIT | **Chosen.** ≈4,500 lines of C (plus generated font tables), 180 KB WASM, total control over the security-critical redaction path, zero copyleft. |

Rendering (rasterizing pages) is a different, enormous problem (fonts, shading, colour management,
image codecs). Pagenoodle uses **PDF.js** (Apache-2.0) only to *draw* pages; every byte that is
*written* comes from the C engine.

## The C engine (`engine/`)

| File | Responsibility |
|---|---|
| `obj.c`, `lex.c` | Ref-counted object model; tokenizer and parser shared by files and content streams |
| `doc.c` | xref tables, xref streams, object streams, hybrid files, `/Prev` chains, **repair by scanning** for broken files, page tree flattening & inheritance |
| `filter.c` | Flate (+PNG/TIFF predictors), LZW, ASCIIHex, ASCII85, RunLength |
| `write.c` | Full rewrite (never incremental), reachability GC, renumbering, object streams + xref stream, stream dedup, re-deflate, metadata stripping |
| `pages.c` | Cross-document deep copy with reference remapping; `assemble` = merge/split/reorder/rotate/duplicate; AcroForm carry-over |
| `content.c`, `font.c`, `text.c` | Content-stream interpreter: text state, CTM, fonts (simple, Type0/CID, Type3, ToUnicode CMaps, standard-14 metrics), text extraction & search with glyph boxes |
| `redact.c` + `text.c` | **True redaction** (below) |
| `overlay.c` | Appends drawing on top of a page (annotations, signatures, stamps, OCR layer) with isolated graphics state |
| `form.c` | AcroForm enumeration, value setting with generated appearance streams, flattening |
| `image.c` | Images → PDF (JPEG passthrough, raw pixels + soft mask), image listing / pixel access / replacement (used by compression) |
| `stamp.c` | Page numbers, headers & footers, aware of `/Rotate` and crop boxes |
| `api.c` | Handle-based `nd_*` API exported to JavaScript |

### True redaction

For each page and each redaction rectangle (in PDF user space):

1. The page's content stream is interpreted with full graphics/text state. Every glyph's box is
   computed in user space (`Trm × CTM`). Glyphs whose centre lies in a box or which overlap it by
   ≥ 10 % are **deleted from the content stream**; `Tj`/`'`/`"` are rewritten to `TJ` with numeric
   offsets so the remaining text keeps its exact position. Invisible text (render mode 3, e.g. OCR
   layers) is removed too.
2. **Images** under a box have their **pixels overwritten** (any bit depth/colour space the engine
   can decode). JPEGs are first decoded to pixels by the worker and re-inserted losslessly so they
   can be scrubbed. Images that cannot be decoded safely (JPEG 2000, JBIG2, CCITT) are **removed
   entirely**. Inline images under a box are removed.
3. **Form XObjects** under a box are recursively redacted into new private copies.
4. **Annotations** under a box are removed; form widgets also lose their field values.
5. A black box is drawn on top, the file is **fully rewritten with garbage collection** (so the old
   content streams and images are gone — no incremental-update leftovers), and metadata can be
   stripped.
6. **Verification**: the saved bytes are re-opened, text is extracted again, and Pagenoodle checks
   that zero glyphs remain under any box and that searched terms appear nowhere in the document.
   The C test-suite additionally checks that the secret is absent from every decompressed stream.

Known gaps (shown in the docs): vector paths under a box are covered, not deleted; text drawn as
vector outlines or inside images is only affected via the image path.

## Data flow for edits

The UI keeps a list of *source* PDFs and an *arrangement* (`[{source, pageIndex, rotation}]`).
Organizing pages only edits that list (instant, undoable). Content edits first **commit** the
arrangement into a single PDF (`nd_assemble`), then call `engine.edit(key, ops)`, which opens a
fresh copy, applies operations (`overlay`, `redact`, `formSet`, `stamp`, `compressImages`, …) and
saves. Every result is a new immutable source, so undo/redo is just swapping snapshots.

Heavy work runs in workers; progress is reported from C via `EM_JS` callbacks; **Cancel** terminates
the worker, and a fresh one transparently re-loads the documents it needs.

## Privacy enforcement (defence in depth)

1. **CSP** (`<meta>`, as GitHub Pages cannot send headers): `default-src 'none'; connect-src 'self'`
   and friends. The browser refuses to contact any other origin from the page.
2. **Service worker** answers every cross-origin request with a network error and serves the app
   from its precache, so it runs with Wi-Fi off.
3. **Network guard** monkey-patches `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`,
   `RTCPeerConnection`, `sendBeacon` and `importScripts` in the page **and in every worker** —
   important because a `<meta>` CSP does not apply to dedicated workers.
4. All third-party runtime assets (PDF.js worker/fonts, SQLite, Tesseract core + model) are
   **vendored and served from the same origin**; none of their CDN defaults are used.

If you self-host on a server that can send headers, also send the CSP as an HTTP header (see the
README) so workers are covered by the browser as well.

## Risks & limitations (v1)

- **Encrypted PDFs** (password or owner-restricted) are rejected with a friendly message.
- Added text (annotations, stamps, form values, OCR layer) uses the standard Helvetica font with
  WinAnsi encoding: Latin-1 characters only. Non-Latin text needs embedded font subsetting (planned).
- Annotations are **flattened** into the page (they look identical everywhere but are no longer
  editable annotations).
- Merging drops bookmarks/outlines and named destinations.
- OCR ships with the English model only (≈3 MB). More languages can be added to `vendor/tessdata`.
- The first visit downloads ≈17 MB (OCR model, cores, CMaps) so that everything works offline later.
- Documents live in memory (UI, engine worker, PDF.js); very large files (hundreds of MB) are limited
  by browser memory. Hundreds of *pages* are fine: thumbnails render lazily with an LRU cache.
- Compare is text-based (visual diff is a possible extension).
