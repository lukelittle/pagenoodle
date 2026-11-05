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

1. **Geometry check.** Page size, crop box (clipped to the media box) and `/Rotate` are computed exactly as
   PDF.js does. Before anything is changed, the UI confirms that the renderer and the engine agree on where
   the page is; if they don't, it refuses (a box must land where the user drew it).
2. **Text.** The content stream is interpreted with full graphics/text state (growable `q`/`Q` stack,
   horizontal and vertical writing, `Tz`/`Tc`/`Tw`/`Ts`). Glyphs whose centre lies in a box, or which overlap
   it by ≥ 10 %, are **deleted from the content stream**; `Tj`/`'`/`"` are rewritten as `TJ` with offsets so
   remaining text keeps its exact position. Invisible (`Tr 3`, OCR) and zero-size text is removed too.
   `/ActualText` and `/Alt` in marked content are dropped on redacted pages.
3. **Images** under a box have their **pixels overwritten**, along with their soft masks; `/Alternates`,
   `/OPI` etc. are dropped. JPEGs are decoded by the worker and re-inserted losslessly so they can be scrubbed.
   Images that cannot be decoded safely (JPEG 2000, JBIG2, CCITT) are **removed entirely**.
4. **Shared objects are redacted in place.** A form XObject or image used on several pages (or referenced
   from the structure tree, an annotation appearance, …) is replaced *itself*, so no original copy can survive
   in the file. The redacted area is therefore removed everywhere that object appears.
5. **Fail closed.** Content too deeply nested to analyse, undecodable forms, and tiling patterns that draw
   text on a redacted page are emptied rather than passed through.
6. **Annotations** under a box are removed together with their popups and content-bearing keys; form widgets
   lose their values. Page `/PieceInfo`, `/Thumb`, `/Metadata` and the form's XFA copy are removed.
7. **Find & redact** additionally scrubs the term from annotations, form values, bookmarks, document info
   and any other string in the file.
8. The file is **fully rewritten with garbage collection** (no incremental-update leftovers), and metadata can
   be stripped.
9. **Two independent verifications** of the saved bytes: (a) the engine re-extracts text under every box and
   scans every string and every decompressed stream for the searched terms; (b) PDF.js — a different parser —
   extracts the text again. If *either* finds anything, the redaction is **undone** and the user is told, so a
   half-redacted file can never be downloaded by mistake.

Known gaps: vector paths under a box are covered, not deleted; text drawn as outlines or inside undecodable
images is handled only via the image rules; structure-tree `/Alt` text is only scrubbed for searched terms.

## Security hardening

The engine parses untrusted files inside the browser, so it is built defensively and tested adversarially:

- **Fuzzing**: `engine/fuzz/fuzz_engine.c` is a libFuzzer harness exercising open, text, search, redaction,
  overlays, stamps, assembly and save (with ASan + UBSan). Every crash found is kept in
  `engine/tests/fuzz-regressions/` and replayed by the test-suite.
- **Security review proofs-of-concept** live in `engine/tests/security/` and are asserted by the test-suite
  (no crash/hang, and no redacted secret survives).
- **Limits**: object nesting (64), decompressed stream size (512 MB), image dimensions (65 536 px), CID width
  entries, load recursion (32); page/field trees and object streams are visited once; the WASM build aborts
  on allocation failure instead of writing through a null pointer.

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

**Use a dedicated origin.** Pages served from `https://<user>.github.io/<repo>/` share an origin with every
other GitHub Pages site of that user — they could read Pagenoodle's local storage (saved signatures). Deploy at a
custom domain (or a user/org with no other Pages sites). The app also refuses to run inside a frame
(clickjacking), since a `<meta>` CSP cannot set `frame-ancestors`.

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
