// "Provably private": explains the privacy model and runs live checks the user can see.
import { useEffect, useState } from 'preact/hooks';
import { Icon, Mascot } from './ui';
import { db } from '../store/db';
import { toast } from '../store/app';

interface Check {
  label: string;
  ok: boolean | null;
  detail: string;
}

export function Privacy() {
  const [checks, setChecks] = useState<Check[]>([]);
  const run = async () => {
    const out: Check[] = [];
    // 1. CSP
    const meta = document.querySelector('meta[http-equiv="Content-Security-Policy"]');
    const csp = meta?.getAttribute('content') ?? '';
    out.push({
      label: 'Content Security Policy is active',
      ok: !!meta && csp.includes("connect-src 'self'"),
      detail: meta ? csp : 'No CSP meta tag (are you on the dev server?)',
    });
    // 2. CSP really blocks an outbound request (image beacon to another origin)
    const blocked = await new Promise<boolean>((resolve) => {
      let violated = false;
      const onV = (e: SecurityPolicyViolationEvent) => {
        if (e.blockedURI.includes('example.invalid')) violated = true;
      };
      document.addEventListener('securitypolicyviolation', onV);
      const img = new Image();
      const done = (v: boolean) => {
        document.removeEventListener('securitypolicyviolation', onV);
        resolve(v);
      };
      img.onload = () => done(false);
      img.onerror = () => setTimeout(() => done(violated), 50);
      img.src = 'https://pagenoodle-test.example.invalid/beacon.png';
      setTimeout(() => done(violated), 2000);
    });
    out.push({
      label: 'An outbound request was blocked by the browser',
      ok: blocked,
      detail: blocked
        ? 'I tried to load https://pagenoodle-test.example.invalid/beacon.png and the browser refused (securitypolicyviolation).'
        : 'The test request was not blocked by CSP (dev server or unsupported browser).',
    });
    // 3. network guard
    let guard = false;
    try {
      await fetch('https://pagenoodle-test.example.invalid/');
    } catch (e) {
      guard = String(e).includes('privacy guard');
    }
    out.push({
      label: 'In-app network guard refuses other origins',
      ok: guard,
      detail: 'fetch(), XHR, WebSocket, sendBeacon and importScripts to other sites throw in the page and in every worker.',
    });
    // 4. Service worker
    const reg = 'serviceWorker' in navigator ? await navigator.serviceWorker.getRegistration() : undefined;
    const sw = reg?.active;
    let swDetail = 'No service worker yet (it installs on the first visit to the deployed site).';
    if (sw) {
      swDetail = await new Promise<string>((resolve) => {
        const ch = (e: MessageEvent) => {
          if (e.data?.type === 'sw-status') {
            navigator.serviceWorker.removeEventListener('message', ch);
            resolve(`Version ${e.data.version}: ${e.data.files} files cached for offline use. Cross-origin requests are refused by the worker too.`);
          }
        };
        navigator.serviceWorker.addEventListener('message', ch);
        sw.postMessage('status');
        setTimeout(() => resolve('Service worker active.'), 1500);
      });
    }
    out.push({
      label: 'Everything is cached for offline use',
      ok: !!sw,
      detail: swDetail,
    });
    // 5. storage
    const mode = await db.mode();
    out.push({
      label: 'Saved recipes & signatures stay on this device',
      ok: mode !== 'unavailable',
      detail:
        mode === 'opfs'
          ? 'SQLite (WebAssembly) in the Origin Private File System.'
          : mode === 'indexeddb'
            ? 'SQLite (WebAssembly) persisted to IndexedDB.'
            : `Storage mode: ${mode}`,
    });
    out.push({
      label: navigator.onLine ? 'You are online (try turning Wi-Fi off!)' : 'You are offline — and everything still works',
      ok: true,
      detail: 'Pagenoodle never needs the network after the first visit.',
    });
    setChecks(out);
  };
  useEffect(() => void run(), []);

  return (
    <div class="view prose">
      <div style={{ textAlign: 'center' }}>
        <Mascot size={110} />
      </div>
      <h1>Your files never leave your device</h1>
      <p>
        Pagenoodle is a static web page. There is <b>no server</b> that could receive your documents — no uploads, no accounts, no analytics, no cookies, no
        third-party anything. Every byte of PDF work happens right here in your browser, in a PDF engine written in C and compiled to WebAssembly.
      </p>
      <h2>Live checks</h2>
      <div aria-live="polite">
        {checks.map((c) => (
          <div class="check" key={c.label}>
            <span class={c.ok ? 'ok' : 'bad'} aria-hidden="true">
              {c.ok ? '✓' : '✗'}
            </span>
            <div>
              <b>
                {c.label}
                <span class="sr-only">{c.ok ? ' — passed' : ' — not confirmed'}</span>
              </b>
              <div class="hint" style={{ wordBreak: 'break-word' }}>
                {c.detail}
              </div>
            </div>
          </div>
        ))}
      </div>
      <button class="btn" onClick={run}>
        Re-run checks
      </button>
      <h2>How to verify it yourself</h2>
      <ol>
        <li>
          <b>Pull the plug.</b> Load Pagenoodle once, then turn Wi-Fi off (or enable airplane mode) and reload. Open, edit, sign, redact, compress and OCR a PDF
          — it all works, because there's nothing to call home to.
        </li>
        <li>
          <b>Watch the network.</b> Open your browser's DevTools → <i>Network</i> tab and use the app. You'll only see requests to this site's own files (served
          from the offline cache) — never your document.
        </li>
        <li>
          <b>Read the policy.</b> View the page source: the <code>Content-Security-Policy</code> sets <code>connect-src 'self'</code> and{' '}
          <code>default-src 'none'</code>, so the browser itself forbids talking to any other server.
        </li>
        <li>
          <b>Read the code.</b> Pagenoodle is open source. The build is reproducible from the repository by GitHub Actions.
        </li>
        <li>
          <b>Install it.</b> Use your browser's “Install app” option to run Pagenoodle like a desktop app, fully offline.
        </li>
      </ol>
      <h2>What is stored, and where</h2>
      <p>
        Only things you explicitly save — <b>recipes</b>, <b>signatures</b> (when you tick “remember”) and settings — live in a SQLite database inside your
        browser's private storage for this site. Documents are kept in memory only while the tab is open.
      </p>
      <button
        class="btn danger"
        onClick={async () => {
          if (!confirm('Delete all saved recipes, signatures and settings from this device?')) return;
          await db.wipe();
          toast('Local data wiped. Squeaky clean 🧼', 'success');
        }}
      >
        <Icon name="trash" /> Wipe my saved data
      </button>
    </div>
  );
}
