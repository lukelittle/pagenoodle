import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

// Strict Content Security Policy. GitHub Pages cannot send headers, so it is delivered as a
// <meta> tag (production builds only — the dev server needs inline HMR scripts).
// connect-src 'self' means the page can only ever talk to its own origin: no uploads, no analytics.
export const CSP = [
  "default-src 'none'",
  "script-src 'self' 'wasm-unsafe-eval'",
  "worker-src 'self'",
  "connect-src 'self'",
  "img-src 'self' blob: data:",
  "style-src 'self' 'unsafe-inline'",
  "font-src 'self' data:",
  "manifest-src 'self'",
  "media-src 'self' blob:",
  "object-src 'none'",
  "frame-src 'none'",
  "base-uri 'none'",
  "form-action 'none'",
].join('; ');

const cspPlugin = () => ({
  name: 'pagenoodle-csp',
  apply: 'build' as const,
  transformIndexHtml(html: string) {
    return html.replace('<!--CSP-->', `<meta http-equiv="Content-Security-Policy" content="${CSP}" />`);
  },
});

// Relative base so the site works from https://<user>.github.io/<repo>/ as well as at a domain root.
export default defineConfig({
  base: './',
  plugins: [preact(), cspPlugin()],
  worker: { format: 'es' },
  build: {
    target: 'es2022',
    sourcemap: false,
    chunkSizeWarningLimit: 2000,
    assetsInlineLimit: 0,
  },
  server: { port: 5173 },
});
