import './lib/guard-first';

import { render } from 'preact';
import '@fontsource/caveat/400.css';
import './styles.css';
import { App } from './components/App';
import { getState } from './store/app';

// Clickjacking defence: a <meta> CSP cannot set frame-ancestors, so refuse to run inside a frame.
if (window.top !== window.self) {
  document.getElementById('app')!.textContent = 'Pagenoodle cannot run inside a frame. Open it directly in its own tab.';
} else {
  render(<App />, document.getElementById('app')!);

  if (import.meta.env.PROD && 'serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('./sw.js').then((reg) => {
        reg.addEventListener('updatefound', () => {
          const w = reg.installing;
          w?.addEventListener('statechange', () => {
            // A new version finished caching. Ask it to take over only when this tab has no work
            // open; the worker itself also refuses while any other tab is open.
            if (w.state === 'installed' && navigator.serviceWorker.controller && !getState().doc.pages.length) w.postMessage('skipWaiting');
          });
        });
      });
    });
  }
}
