# Third-party notices

Pagenoodle itself is released under the MIT License (see `LICENSE`). It ships the following
third-party components, all under licenses compatible with a free, public, open-source project.
No component is copyleft (no GPL/AGPL), so Pagenoodle can be freely forked, hosted and modified.

| Component | Used for | License | Shipped to users |
|---|---|---|---|
| **miniz** 3.0.2 (Rich Geldreich, RAD Game Tools, Valve) | Deflate/inflate inside the C engine | MIT | yes (compiled into `noodle.wasm`) |
| **Adobe Core 14 AFM font metrics** | Glyph widths/encodings for the 14 standard PDF fonts (`engine/src/tables.c`) | Adobe AFM license — “may be used, copied, and distributed for any purpose and without charge” | yes (data tables) |
| **Adobe Glyph List** | Glyph-name → Unicode mapping | BSD-3-Clause (Adobe) | yes (data tables) |
| **PDF.js** (`pdfjs-dist`, Mozilla) | Rendering pages and thumbnails | Apache-2.0 | yes |
| ↳ OpenJPEG (via PDF.js wasm) | JPEG 2000 image decoding | BSD-2-Clause | yes |
| ↳ PDFium JBIG2 decoder (via PDF.js wasm) | JBIG2 image decoding | BSD-3-Clause / Apache-2.0 | yes |
| ↳ qcms (via PDF.js wasm) | ICC colour management | MIT | yes |
| **SQLite** + official WASM build (`@sqlite.org/sqlite-wasm`) | Local storage for recipes, signatures, settings | SQLite: public domain; JS/WASM wrapper: Apache-2.0 | yes |
| **Tesseract OCR** (`tesseract.js`, `tesseract.js-core`) | In-browser OCR | Apache-2.0 (Tesseract), BSD-2-Clause (Leptonica) | yes |
| **eng.traineddata** (`@tesseract.js-data/eng`, from tesseract-ocr/tessdata) | English OCR model | Apache-2.0 (model), MIT (npm packaging) | yes |
| **Preact** | UI library | MIT | yes |
| **Caveat** font (`@fontsource/caveat`, Pablo Impallari) | Handwriting style for typed signatures | SIL Open Font License 1.1 | yes |
| **Fraunces** font (`@fontsource/fraunces`, Undercase Type) | Retro display headings | SIL Open Font License 1.1 | yes |
| ReportLab, fontTools | Dev-time only: generating metric tables and test fixtures | BSD | no |
| Vite, TypeScript, Playwright | Dev-time build & test tooling | MIT / Apache-2.0 | no |
| axe-core (`@axe-core/playwright`) | Dev-time accessibility tests | MPL-2.0 | no |
| Emscripten | Dev-time C → WebAssembly compiler | MIT / UIUC | runtime glue (MIT) |

Full license texts are included next to each component in the deployed site under `vendor/`
(e.g. `vendor/pdfjs/LICENSE`, `vendor/pdfjs/wasm/LICENSE_*`, `vendor/tesseract/LICENSE`) and in
`engine/vendor/miniz/LICENSE`.
