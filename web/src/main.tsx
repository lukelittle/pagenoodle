import './lib/guard-first';

import { render } from 'preact';
import '@fontsource/caveat/400.css';
import './styles.css';
import { App } from './components/App';
import { getState } from './store/app';

render(<App />, document.getElementById('app')!);

if (import.meta.env.PROD && 'serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').then((reg) => {
      reg.addEventListener('updatefound', () => {
        const w = reg.installing;
        w?.addEventListener('statechange', () => {
          // a new version finished caching: activate it when no work is in flight
          if (w.state === 'installed' && navigator.serviceWorker.controller && !getState().doc.pages.length) w.postMessage('skipWaiting');
        });
      });
    });
  });
}

// expose a tiny hook for end-to-end tests
(window as unknown as { __pagenoodle: unknown }).__pagenoodle = { getState };
