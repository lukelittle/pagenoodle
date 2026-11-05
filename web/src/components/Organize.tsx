// Organize: page thumbnails grid — reorder by drag or keyboard, rotate, delete, duplicate, extract, split.
import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { useApp, getState, setState, setPages, newId, withBusy, exportBytes, toast, record, type PageRef } from '../store/app';
import { thumbnail } from '../render/pdf';
import { Icon, Dialog, Field } from './ui';
import { runStep, type Step } from '../pipeline/steps';
import { downloadBlob, makeZip, safeName } from '../lib/util';
import { celebrate, openFilePicker } from './actions';

const SRC_COLORS = ['#ffb84d', '#2563a6', '#3a7d44', '#e4572e', '#9b5de5', '#00a6a6', '#c2185b', '#6d4c41'];

export function toRangeSpec(indices: number[]): string {
  const s = [...indices].sort((a, b) => a - b);
  const out: string[] = [];
  for (let i = 0; i < s.length; i++) {
    let j = i;
    while (j + 1 < s.length && s[j + 1] === s[j] + 1) j++;
    out.push(j > i ? `${s[i] + 1}-${s[j] + 1}` : `${s[i] + 1}`);
    i = j;
  }
  return out.join(',') || 'all';
}

// one shared IntersectionObserver for lazy thumbnails
const visibleCallbacks = new Map<Element, (v: boolean) => void>();
let io: IntersectionObserver | null = null;
function observe(el: Element, cb: (v: boolean) => void) {
  if (!io) io = new IntersectionObserver((entries) => entries.forEach((e) => visibleCallbacks.get(e.target)?.(e.isIntersecting)), { rootMargin: '300px' });
  visibleCallbacks.set(el, cb);
  io.observe(el);
  return () => {
    visibleCallbacks.delete(el);
    io?.unobserve(el);
  };
}

export function PageThumb({ page, width }: { page: PageRef; width: number }) {
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [visible, setVisible] = useState(false);
  const [ready, setReady] = useState(false);
  useEffect(() => observe(wrap.current!, setVisible), []);
  useEffect(() => {
    if (!visible) return;
    const ac = new AbortController();
    setReady(false);
    const w = Math.round(width * Math.min(window.devicePixelRatio || 1, 2));
    thumbnail(page.src, page.index, page.rot, w, ac.signal)
      .then((bmp) => {
        const c = canvas.current;
        if (!c) return;
        c.width = bmp.width;
        c.height = bmp.height;
        c.getContext('2d')!.drawImage(bmp, 0, 0);
        setReady(true);
      })
      .catch(() => {});
    return () => ac.abort();
  }, [visible, page.src, page.index, page.rot, width]);
  return (
    <div class="canvas-wrap" ref={wrap}>
      <canvas ref={canvas} style={{ display: ready ? 'block' : 'none' }} aria-hidden="true" />
      {!ready && <div class="skeleton" />}
    </div>
  );
}

export function Organize() {
  const st = useApp();
  const { pages, sources } = st.doc;
  const selected = new Set(st.selected);
  const [size, setSize] = useState(150);
  const [focusIdx, setFocusIdx] = useState(0);
  const [drag, setDrag] = useState<{
    ids: string[];
    over: number;
    after: boolean;
  } | null>(null);
  const [splitOpen, setSplitOpen] = useState(false);
  const gridRef = useRef<HTMLUListElement>(null);
  const srcOrder = useMemo(() => Object.keys(sources), [sources]);
  const multiSource = srcOrder.length > 1;
  const lastClick = useRef<number | null>(null);

  const sel = (ids: string[]) => setState({ selected: ids });
  const targetIdx = () => (selected.size ? pages.map((p, i) => (selected.has(p.id) ? i : -1)).filter((i) => i >= 0) : pages.map((_, i) => i));
  const spec = () => (selected.size ? toRangeSpec(targetIdx()) : 'all');

  const doStep = async (step: Step, label: string) => {
    await withBusy(label, async (ctx) => {
      await runStep(step, ctx);
      record(step);
    });
  };
  const rotate = (angle: number) => doStep({ op: 'rotate', pages: spec(), angle }, 'Twirling pages…');
  const remove = () => {
    if (!selected.size) return toast('Select some pages first (click, or Space on a page).');
    void doStep({ op: 'delete', pages: spec() }, 'Removing pages…');
    sel([]);
  };
  const duplicate = () => {
    const idx = targetIdx();
    if (!selected.size) return toast('Select pages to duplicate.');
    const out: PageRef[] = [];
    pages.forEach((p, i) => {
      out.push(p);
      if (idx.includes(i)) out.push({ ...p, id: newId() });
    });
    setPages(out);
  };
  const extract = async () => {
    if (!selected.size) return toast('Select the pages you want to extract.');
    const chosen = pages.filter((p) => selected.has(p.id));
    await withBusy('Extracting pages…', async (ctx) => {
      const bytes = await exportBytes(ctx, chosen);
      downloadBlob(bytes, `${safeName(getState().baseName)}-pages-${toRangeSpec(targetIdx())}.pdf`);
      celebrate();
    });
  };

  // ---------- keyboard ----------
  const columns = () => {
    const g = gridRef.current;
    if (!g || g.children.length < 2) return 1;
    const top = (g.children[0] as HTMLElement).offsetTop;
    let c = 0;
    for (const ch of g.children) if ((ch as HTMLElement).offsetTop === top) c++;
    return Math.max(1, c);
  };
  const focusAt = (i: number) => {
    const k = Math.max(0, Math.min(pages.length - 1, i));
    setFocusIdx(k);
    (gridRef.current?.children[k] as HTMLElement | undefined)?.focus();
  };
  const move = (ids: string[], to: number) => {
    const set = new Set(ids);
    const moving = pages.filter((p) => set.has(p.id));
    const rest = pages.filter((p) => !set.has(p.id));
    const before = pages.slice(0, to).filter((p) => !set.has(p.id)).length;
    rest.splice(before, 0, ...moving);
    setPages(rest);
    return rest;
  };
  const onKey = (e: KeyboardEvent, i: number) => {
    const p = pages[i];
    const cols = columns();
    const delta = {
      ArrowLeft: -1,
      ArrowRight: 1,
      ArrowUp: -cols,
      ArrowDown: cols,
    }[e.key];
    if (delta !== undefined) {
      e.preventDefault();
      if (e.altKey) {
        const ids = selected.has(p.id) ? st.selected : [p.id];
        const to = Math.max(0, Math.min(pages.length, i + delta + (delta > 0 ? 1 : 0)));
        const res = move(ids, to);
        const ni = res.findIndex((x) => x.id === p.id);
        setTimeout(() => focusAt(ni));
        announce(`Moved to position ${ni + 1} of ${pages.length}`);
      } else {
        focusAt(i + delta);
        if (e.shiftKey) sel([...new Set([...st.selected, p.id, pages[Math.max(0, Math.min(pages.length - 1, i + delta))].id])]);
      }
    } else if (e.key === ' ' || e.key === 'Enter') {
      e.preventDefault();
      sel(selected.has(p.id) ? st.selected.filter((x) => x !== p.id) : [...st.selected, p.id]);
    } else if (e.key === 'Delete' || e.key === 'Backspace') {
      e.preventDefault();
      if (!selected.size) sel([p.id]);
      setTimeout(remove);
    } else if (e.key.toLowerCase() === 'r') {
      e.preventDefault();
      if (!selected.size) sel([p.id]);
      setTimeout(() => rotate(e.shiftKey ? -90 : 90));
    } else if (e.key === 'Home') focusAt(0);
    else if (e.key === 'End') focusAt(pages.length - 1);
  };
  const [live, setLive] = useState('');
  const announce = (t: string) => setLive(t);

  // ---------- pointer drag ----------
  const dragState = useRef<{
    id: string;
    x: number;
    y: number;
    started: boolean;
    pointer: number;
  } | null>(null);
  const onPointerDown = (e: PointerEvent, p: PageRef) => {
    if ((e.target as HTMLElement).closest('button, .qa-btn') || e.button !== 0) return;
    dragState.current = {
      id: p.id,
      x: e.clientX,
      y: e.clientY,
      started: false,
      pointer: e.pointerId,
    };
  };
  useEffect(() => {
    const onMove = (e: PointerEvent) => {
      const d = dragState.current;
      if (!d) return;
      if (!d.started) {
        if (Math.hypot(e.clientX - d.x, e.clientY - d.y) < 6) return;
        d.started = true;
      }
      const g = gridRef.current;
      if (!g) return;
      let over = -1;
      let after = false;
      [...g.children].forEach((ch, i) => {
        const r = ch.getBoundingClientRect();
        if (e.clientX >= r.left - 8 && e.clientX <= r.right + 8 && e.clientY >= r.top && e.clientY <= r.bottom) {
          over = i;
          after = e.clientX > r.left + r.width / 2;
        }
      });
      const st2 = getState();
      const ids = st2.selected.includes(d.id) ? st2.selected : [d.id];
      setDrag(over >= 0 ? { ids, over, after } : { ids, over: -1, after: false });
      // auto-scroll near edges
      if (e.clientY < 80) window.scrollBy(0, -12);
      else if (e.clientY > window.innerHeight - 60) window.scrollBy(0, 12);
    };
    const onUp = () => {
      const d = dragState.current;
      dragState.current = null;
      setDrag((cur) => {
        if (d?.started && cur && cur.over >= 0) {
          move(cur.ids, cur.over + (cur.after ? 1 : 0));
          toast(`Moved ${cur.ids.length} page${cur.ids.length > 1 ? 's' : ''}`);
        }
        return null;
      });
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    return () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
  });

  const onClick = (e: MouseEvent, i: number) => {
    const p = pages[i];
    if (e.shiftKey && lastClick.current !== null) {
      const [a, b] = [Math.min(lastClick.current, i), Math.max(lastClick.current, i)];
      sel([...new Set([...st.selected, ...pages.slice(a, b + 1).map((x) => x.id)])]);
    } else if (e.metaKey || e.ctrlKey) {
      sel(selected.has(p.id) ? st.selected.filter((x) => x !== p.id) : [...st.selected, p.id]);
    } else sel(selected.has(p.id) && st.selected.length === 1 ? [] : [p.id]);
    lastClick.current = i;
    setFocusIdx(i);
  };

  return (
    <div class="view">
      <div class="toolbar" role="toolbar" aria-label="Page tools">
        <button class="btn" onClick={openFilePicker}>
          <Icon name="plus" /> Add files
        </button>
        <span class="sep" />
        <button class="btn" onClick={() => sel(selected.size === pages.length ? [] : pages.map((p) => p.id))}>
          {selected.size === pages.length ? 'Select none' : 'Select all'}
        </button>
        <button class="btn" onClick={() => rotate(-90)} title="Rotate left (Shift+R)">
          <Icon name="rotl" /> <span>Rotate left</span>
        </button>
        <button class="btn" onClick={() => rotate(90)} title="Rotate right (R)">
          <Icon name="rotr" /> <span>Rotate right</span>
        </button>
        <button class="btn" onClick={duplicate} disabled={!selected.size}>
          <Icon name="copy" /> Duplicate
        </button>
        <button class="btn danger" onClick={remove} disabled={!selected.size}>
          <Icon name="trash" /> Delete
        </button>
        <button class="btn" onClick={extract} disabled={!selected.size}>
          <Icon name="extract" /> Extract
        </button>
        <button class="btn" onClick={() => setSplitOpen(true)}>
          <Icon name="scissors" /> Split…
        </button>
        <button class="btn" onClick={() => doStep({ op: 'reverse' }, 'Reversing…')}>
          <Icon name="reverse" /> Reverse
        </button>
        <span class="grow" />
        <label class="hint" style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          Size
          <input type="range" min={90} max={320} value={size} onInput={(e) => setSize(+(e.target as HTMLInputElement).value)} aria-label="Thumbnail size" />
        </label>
      </div>
      <p class="hint" style={{ margin: '-6px 0 12px' }}>
        {pages.length} page{pages.length === 1 ? '' : 's'}
        {multiSource ? ` from ${srcOrder.length} files — they'll be merged in this order` : ''}. Drag to reorder, or use <kbd>Alt</kbd>+<kbd>←</kbd>/
        <kbd>→</kbd>. <kbd>Space</kbd> selects, <kbd>R</kbd> rotates, <kbd>Del</kbd> deletes.
      </p>
      <div class="sr-only" aria-live="assertive">
        {live}
      </div>
      <ul class="grid" ref={gridRef} role="listbox" aria-multiselectable="true" aria-label="Pages" style={{ '--thumb': `${size}px` } as Record<string, string>}>
        {pages.map((p, i) => {
          const isDragSrc = drag?.ids.includes(p.id);
          const cls = ['thumb', isDragSrc ? 'drag-src' : '', drag && drag.over === i ? (drag.after ? 'drop-after' : 'drop-before') : ''].join(' ');
          const color = SRC_COLORS[srcOrder.indexOf(p.src) % SRC_COLORS.length];
          return (
            <li
              key={p.id}
              class={cls}
              role="option"
              aria-selected={selected.has(p.id)}
              aria-label={`Page ${i + 1}${p.rot ? `, rotated ${p.rot}°` : ''}${multiSource ? `, from ${sources[p.src]?.name}` : ''}`}
              tabIndex={i === focusIdx ? 0 : -1}
              onKeyDown={(e) => onKey(e as unknown as KeyboardEvent, i)}
              onClick={(e) => onClick(e as unknown as MouseEvent, i)}
              onPointerDown={(e) => onPointerDown(e as unknown as PointerEvent, p)}
              onFocus={() => setFocusIdx(i)}
              data-testid="page-thumb"
            >
              <PageThumb page={p} width={size} />
              <div class="label">
                <span>
                  {multiSource && <span class="src-dot" style={{ background: color }} title={sources[p.src]?.name} />}
                  <span class="num">{i + 1}</span>
                </span>
                {p.rot ? <span>{p.rot}°</span> : null}
              </div>
              <div class="qa" aria-hidden="true">
                <span
                  class="qa-btn"
                  title="Rotate (R)"
                  onClick={(e) => {
                    e.stopPropagation();
                    setPages(pages.map((x) => (x.id === p.id ? { ...x, rot: (x.rot + 90) % 360 } : x)));
                  }}
                >
                  <Icon name="rotr" />
                </span>
                <span
                  class="qa-btn"
                  title="Delete (Del)"
                  onClick={(e) => {
                    e.stopPropagation();
                    if (pages.length > 1) setPages(pages.filter((x) => x.id !== p.id));
                    else toast('A PDF needs at least one page!');
                  }}
                >
                  <Icon name="trash" />
                </span>
              </div>
            </li>
          );
        })}
      </ul>
      {splitOpen && <SplitDialog onClose={() => setSplitOpen(false)} />}
    </div>
  );
}

function SplitDialog({ onClose }: { onClose: () => void }) {
  const st = getState();
  const [mode, setMode] = useState<'every' | 'ranges'>('every');
  const [every, setEvery] = useState(1);
  const [ranges, setRanges] = useState('1-2, 3-');
  const run = async () => {
    onClose();
    const step: Step = mode === 'every' ? { op: 'split', every } : { op: 'split', ranges };
    await withBusy('Splitting…', async (ctx) => {
      const out = await runStep(step, ctx);
      record(step);
      await downloadParts(out.parts ?? [], ctx);
    });
  };
  return (
    <Dialog
      title="Split into separate PDFs"
      onClose={onClose}
      actions={
        <>
          <button class="btn" onClick={onClose}>
            Cancel
          </button>
          <button class="btn primary" onClick={run}>
            <Icon name="scissors" /> Split &amp; download
          </button>
        </>
      }
    >
      <div class="radio-row" role="radiogroup" aria-label="Split mode">
        <label>
          <input type="radio" name="mode" checked={mode === 'every'} onChange={() => setMode('every')} />
          <span>Every N pages</span>
        </label>
        <label>
          <input type="radio" name="mode" checked={mode === 'ranges'} onChange={() => setMode('ranges')} />
          <span>By page ranges</span>
        </label>
      </div>
      {mode === 'every' ? (
        <Field label="Pages per file" hint={`Makes ${Math.ceil(st.doc.pages.length / Math.max(1, every))} files.`}>
          <input type="number" min={1} max={st.doc.pages.length} value={every} onInput={(e) => setEvery(Math.max(1, +(e.target as HTMLInputElement).value))} />
        </Field>
      ) : (
        <Field label="Ranges (one file per comma group)" hint="Example: 1-3, 4-6, 7-  (a trailing dash means “to the end”)">
          <input type="text" value={ranges} onInput={(e) => setRanges((e.target as HTMLInputElement).value)} />
        </Field>
      )}
    </Dialog>
  );
}

export async function downloadParts(parts: { name: string; pages: PageRef[] }[], ctx: { onProgress?: (d: number, t: number) => void; signal?: AbortSignal }) {
  const base = safeName(getState().baseName);
  const files: { name: string; data: Uint8Array }[] = [];
  for (let i = 0; i < parts.length; i++) {
    ctx.onProgress?.(i, parts.length);
    files.push({
      name: `${base}-${parts[i].name}.pdf`,
      data: await exportBytes({ signal: ctx.signal }, parts[i].pages),
    });
  }
  if (files.length === 1) downloadBlob(files[0].data, files[0].name);
  else downloadBlob(makeZip(files), `${base}-split.zip`, 'application/zip');
  celebrate();
  toast(`Made ${files.length} file${files.length > 1 ? 's' : ''} ✂️`, 'success');
}
