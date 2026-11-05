// pdf.js worker, wrapped so our network guard is installed before pdf.js evaluates.
import "../lib/guard-first";
import "pdfjs-dist/build/pdf.worker.min.mjs";
