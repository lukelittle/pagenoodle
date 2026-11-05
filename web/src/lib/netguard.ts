/**
 * Network guard: defence-in-depth behind the Content Security Policy and the service worker.
 * Installed first thing in the page and in every worker. Any attempt to reach another origin
 * (fetch, XHR, WebSocket, EventSource, sendBeacon, importScripts) throws instead.
 */
type G = typeof globalThis & {
  importScripts?: (...urls: string[]) => void;
  __pnGuard?: boolean;
};

export function isSameOrigin(input: string | URL, base: string): boolean {
  try {
    const url = new URL(String(input), base);
    if (url.protocol === 'blob:' || url.protocol === 'data:') return true;
    return url.origin === new URL(base).origin;
  } catch {
    return false;
  }
}

export function installNetGuard(): void {
  const g = globalThis as G;
  if (g.__pnGuard) return;
  g.__pnGuard = true;
  const base = g.location.href;
  const deny = (what: string, url: unknown) => {
    const msg = `Pagenoodle privacy guard blocked ${what} to ${String(url)} — nothing leaves your device.`;
    console.warn(msg);
    return new TypeError(msg);
  };
  const origFetch = g.fetch?.bind(g);
  if (origFetch) {
    g.fetch = ((input: RequestInfo | URL, init?: RequestInit) => {
      const url = typeof input === 'string' || input instanceof URL ? input : input.url;
      if (!isSameOrigin(url, base)) return Promise.reject(deny('fetch', url));
      return origFetch(input, init);
    }) as typeof fetch;
  }
  if (typeof XMLHttpRequest !== 'undefined') {
    const open = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (this: XMLHttpRequest, method: string, url: string | URL, ...rest: unknown[]) {
      if (!isSameOrigin(url, base)) throw deny('XMLHttpRequest', url);
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return (open as any).call(this, method, url, ...rest);
    } as typeof XMLHttpRequest.prototype.open;
  }
  const block = (name: 'EventSource' | 'RTCPeerConnection' | 'WebTransport' | 'fetchLater') => {
    if (name in g) {
      Object.defineProperty(g, name, {
        configurable: false,
        writable: false,
        value: function () {
          throw deny(name, '(any)');
        },
      });
    }
  };
  // WebSockets are only allowed back to this very host (used by the dev server's hot reload).
  const OrigWS = g.WebSocket;
  if (OrigWS) {
    const host = new URL(base).host;
    const Guarded = function (this: unknown, url: string | URL, protocols?: string | string[]) {
      const u = new URL(String(url), base);
      if (u.host !== host) throw deny('WebSocket', url);
      return new OrigWS(u, protocols);
    } as unknown as typeof WebSocket;
    Guarded.prototype = OrigWS.prototype;
    Object.defineProperty(g, 'WebSocket', { configurable: false, writable: false, value: Guarded });
  }
  block('EventSource');
  block('RTCPeerConnection');
  block('WebTransport');
  block('fetchLater');
  if (typeof navigator !== 'undefined' && 'sendBeacon' in navigator) {
    try {
      Object.defineProperty(navigator, 'sendBeacon', {
        value: () => false,
        configurable: false,
      });
    } catch {
      /* ignore */
    }
  }
  if (typeof g.importScripts === 'function') {
    const imp = g.importScripts.bind(g);
    (g as { importScripts: unknown }).importScripts = (...urls: (string | URL)[]) => {
      for (const u of urls) if (!isSameOrigin(u, base)) throw deny('importScripts', u);
      imp(...(urls as string[]));
    };
  }
}
