// Shared UI bits: icons, mascot, dialog, busy overlay, toasts, celebration.
import type { ComponentChildren, JSX } from 'preact';
import { useEffect, useRef } from 'preact/hooks';
import { cancelBusy, useApp } from '../store/app';
import { prefersReducedMotion } from '../lib/util';

const P: Record<string, string> = {
  open: 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z',
  download: 'M12 3v12m0 0-5-5m5 5 5-5M4 19h16',
  rotl: 'M4 9a8 8 0 1 1 1.8 9M4 4v5h5',
  rotr: 'M20 9a8 8 0 1 0-1.8 9M20 4v5h-5',
  trash: 'M4 7h16M9 7V4h6v3m-8 0 1 13h8l1-13',
  copy: 'M8 8h11v11H8zM5 16V5h11',
  scissors: 'M6 6a3 3 0 1 0 0 .01M6 18a3 3 0 1 0 0 .01M8.5 7.5 20 18M8.5 16.5 20 6',
  extract: 'M14 3h7v7M21 3l-9 9M19 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h5',
  undo: 'M9 14 4 9l5-5M4 9h10a6 6 0 0 1 0 12h-3',
  redo: 'm15 14 5-5-5-5m5 5H10a6 6 0 0 0 0 12h3',
  plus: 'M12 5v14M5 12h14',
  text: 'M5 6V4h14v2M12 4v16m-3 0h6',
  rect: 'M4 6h16v12H4z',
  ellipse: 'M12 5c4.4 0 8 3.1 8 7s-3.6 7-8 7-8-3.1-8-7 3.6-7 8-7z',
  arrow: 'M5 19 19 5m0 0h-8m8 0v8',
  line: 'M5 19 19 5',
  highlight: 'M4 20h7M14 4l6 6-9 9H5v-6z',
  pen: 'M3 21c3-1 5-3 7-5l9-9a2 2 0 0 0-3-3l-9 9c-2 2-4 4-4 8z',
  pointer: 'M5 3l14 8-6 2-2 6z',
  sign: 'M3 17c3-6 5-10 7-10 3 0-2 10 1 10 2 0 3-4 5-4s1 3 3 3M3 21h18',
  redact: 'M4 6h16v5H4zM4 14h10v4H4z',
  search: 'M11 4a7 7 0 1 1 0 14 7 7 0 0 1 0-14zm5.5 12.5L21 21',
  shield: 'M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6z',
  wifi: 'M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 19.5h.01',
  check: 'M5 12l5 5 9-11',
  x: 'M6 6l12 12M18 6 6 18',
  chevl: 'M15 5l-7 7 7 7',
  chevr: 'M9 5l7 7-7 7',
  record: 'M12 7a5 5 0 1 1 0 10 5 5 0 0 1 0-10z',
  stop: 'M7 7h10v10H7z',
  play: 'M8 5v14l11-7z',
  layers: 'M12 3 2 8l10 5 10-5zM2 13l10 5 10-5',
  compress: 'M8 3v5H3M16 3v5h5M8 21v-5H3M16 21v-5h5',
  hash: 'M5 9h14M5 15h14M10 4 8 20M16 4l-2 16',
  image: 'M4 5h16v14H4zM4 15l5-5 4 4 3-3 4 4M15 9h.01',
  eye: 'M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12zm10-3a3 3 0 1 0 0 6 3 3 0 0 0 0-6z',
  diff: 'M7 3v12M3 11l4 4 4-4M17 21V9M13 13l4-4 4 4',
  form: 'M4 4h16v16H4zM8 9h8M8 13h8M8 17h5',
  reverse: 'M7 4v16M3 16l4 4 4-4M17 20V4M13 8l4-4 4 4',
  info: 'M12 8h.01M11 12h1v5h1M12 3a9 9 0 1 1 0 18 9 9 0 0 1 0-18z',
};

export function Icon({ name, title }: { name: keyof typeof P | string; title?: string }) {
  return (
    <svg
      class="icon"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="2"
      stroke-linecap="round"
      stroke-linejoin="round"
      aria-hidden={title ? undefined : 'true'}
      role={title ? 'img' : undefined}
    >
      {title && <title>{title}</title>}
      <path d={P[name] ?? P.info} />
    </svg>
  );
}

export function Mascot({ size = 44, wiggle = false, mood = 'happy' }: { size?: number; wiggle?: boolean; mood?: 'happy' | 'wow' | 'sleepy' }) {
  return (
    <svg class={`mascot${wiggle ? ' wiggle' : ''}`} width={size} height={size * 0.62} viewBox="0 0 100 62" aria-hidden="true">
      <path class="body" d="M6 40c10-18 22-18 30-4s20 14 28 0 20-16 28 2" fill="none" stroke="#ffb84d" stroke-width="13" stroke-linecap="round" />
      <path d="M6 38c10-18 22-18 30-4" fill="none" stroke="#ffd27f" stroke-width="4" stroke-linecap="round" opacity=".8" />
      {mood === 'sleepy' ? (
        <g stroke="#3a2a1a" stroke-width="2.5" stroke-linecap="round">
          <path d="M78 30h6M88 33h6" />
        </g>
      ) : (
        <g fill="#3a2a1a">
          <circle cx="81" cy="30" r={mood === 'wow' ? 3.6 : 3} />
          <circle cx="91" cy="34" r={mood === 'wow' ? 3.6 : 3} />
        </g>
      )}
      {mood === 'wow' ? (
        <circle cx="85" cy="41" r="3" fill="#3a2a1a" />
      ) : (
        <path d="M80 39q5 5 10 2" fill="none" stroke="#3a2a1a" stroke-width="2.2" stroke-linecap="round" />
      )}
      <ellipse cx="76" cy="37" rx="3" ry="2" fill="#ff8a8a" opacity=".7" />
      <ellipse cx="95" cy="42" rx="3" ry="2" fill="#ff8a8a" opacity=".7" />
    </svg>
  );
}

export function Dialog({
  title,
  onClose,
  children,
  actions,
  wide,
}: {
  title: string;
  onClose: () => void;
  children: ComponentChildren;
  actions?: ComponentChildren;
  wide?: boolean;
}) {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const prev = document.activeElement as HTMLElement | null;
    const el = ref.current!;
    const focusables = () =>
      [...el.querySelectorAll<HTMLElement>('button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])')].filter(
        (x) => !x.hasAttribute('disabled'),
      );
    (el.querySelector<HTMLElement>('[autofocus]') ?? focusables()[0] ?? el).focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        onClose();
      } else if (e.key === 'Tab') {
        const f = focusables();
        if (!f.length) return;
        if (e.shiftKey && document.activeElement === f[0]) {
          e.preventDefault();
          f[f.length - 1].focus();
        } else if (!e.shiftKey && document.activeElement === f[f.length - 1]) {
          e.preventDefault();
          f[0].focus();
        }
      }
    };
    el.addEventListener('keydown', onKey);
    return () => {
      el.removeEventListener('keydown', onKey);
      prev?.focus?.();
    };
  }, []);
  return (
    <div class="backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div
        class="dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="dlg-title"
        ref={ref}
        tabIndex={-1}
        style={wide ? { width: 'min(820px, 100%)' } : undefined}
      >
        <h2 id="dlg-title">{title}</h2>
        {children}
        {actions && <div class="actions">{actions}</div>}
      </div>
    </div>
  );
}

export function BusyOverlay() {
  const { busy } = useApp();
  if (!busy) return null;
  const pct = busy.total > 0 ? Math.min(100, Math.round((busy.done / busy.total) * 100)) : 0;
  return (
    <div class="busy" role="alertdialog" aria-busy="true" aria-labelledby="busy-label">
      <div class="box">
        <Mascot size={70} wiggle={!prefersReducedMotion()} />
        <div id="busy-label" style={{ fontWeight: 700, marginTop: 6 }}>
          {busy.label}
        </div>
        <div
          class={`progress${busy.total ? '' : ' indeterminate'}`}
          role="progressbar"
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={busy.total ? pct : undefined}
          aria-label="Progress"
        >
          <div style={{ width: `${pct}%` }} />
        </div>
        <div class="hint">{busy.total ? `${pct}%` : 'Working on it…'} · all on your device</div>
        <button class="btn" style={{ marginTop: 12 }} onClick={cancelBusy}>
          <Icon name="x" /> Cancel
        </button>
      </div>
    </div>
  );
}

export function Toasts() {
  const { toasts } = useApp();
  return (
    <div class="toasts" role="status" aria-live="polite">
      {toasts.map((t) => (
        <div key={t.id} class={`toast ${t.kind}`}>
          {t.text}
        </div>
      ))}
    </div>
  );
}

const CHEERS = ['Noodle-licious! 🍝', 'Al dente and done!', 'Pasta la vista, baby!', 'You’re oodles of awesome!', 'Fresh out of the pot!'];
export function Celebrate() {
  const { celebrate } = useApp();
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!celebrate || prefersReducedMotion() || !ref.current) return;
    const host = ref.current;
    const colors = ['#ffb84d', '#e4572e', '#3a7d44', '#2563a6', '#ffd27f'];
    const bits: HTMLElement[] = [];
    for (let i = 0; i < 28; i++) {
      const d = document.createElement('div');
      d.className = 'confetti';
      d.style.left = `${Math.random() * 100}vw`;
      d.style.background = colors[i % colors.length];
      d.style.animationDelay = `${Math.random() * 0.4}s`;
      d.style.width = `${6 + Math.random() * 6}px`;
      host.appendChild(d);
      bits.push(d);
    }
    const t = setTimeout(() => bits.forEach((b) => b.remove()), 2800);
    return () => {
      clearTimeout(t);
      bits.forEach((b) => b.remove());
    };
  }, [celebrate]);
  if (!celebrate) return null;
  return (
    <div class="celebrate" ref={ref} key={celebrate} aria-hidden="true">
      <div class="bubble">
        <Mascot size={110} wiggle={!prefersReducedMotion()} />
        <div>{CHEERS[celebrate % CHEERS.length]}</div>
      </div>
    </div>
  );
}

export function Field({ label, children, hint }: { label: string; children: ComponentChildren; hint?: string }) {
  return (
    <div class="field">
      <label>
        <span class="lbl">{label}</span>
        {children}
      </label>
      {hint && <span class="hint">{hint}</span>}
    </div>
  );
}

export const COLORS = ['#111111', '#e4572e', '#2563a6', '#3a7d44', '#ffb800', '#9b5de5', '#ffffff'];
export function Swatches({
  value,
  onChange,
  colors = COLORS,
  label = 'Color',
}: {
  value: string;
  onChange: (c: string) => void;
  colors?: string[];
  label?: string;
}) {
  return (
    <div class="swatches" role="group" aria-label={label}>
      {colors.map((c) => (
        <button key={c} class="swatch" style={{ background: c }} aria-pressed={value === c} aria-label={`${label} ${c}`} onClick={() => onChange(c)} />
      ))}
      <input
        type="color"
        value={value}
        onInput={(e) => onChange((e.target as HTMLInputElement).value)}
        aria-label={`Custom ${label.toLowerCase()}`}
        style={{
          width: 32,
          height: 30,
          border: 0,
          background: 'none',
          padding: 0,
        }}
      />
    </div>
  );
}

export type Child = JSX.Element | null | false;
