// True redaction: removes the underlying text, image pixels and annotations — then proves it.
import { useRef, useState } from 'preact/hooks';
import { useApp, setState, applyEdits, withBusy, toast, newId, record, getState, undo } from '../store/app';
import { independentCheck, geometryMatches, type IndependentResult } from '../lib/verify';
import { engine } from '../engine/client';
import { SAVE, type EditOp, type RedactReport } from '../engine/types';
import { userToVisual, visualToUser, type Annot } from '../lib/overlay';
import { Icon, Dialog } from './ui';
import { PageView, MiniPage, pointOf, useStageWidth } from './PageView';
import { AnnotShape, useCommitted } from './Editor';
import { setSuffix } from './actions';

type Box = Extract<Annot, { kind: 'redact' }>;

interface Verification {
  reports: RedactReport[];
  remaining: number;
  leaked: string;
  stillFound: string[];
  terms: string[];
  stripped: boolean;
  independent: IndependentResult;
  reverted: boolean;
}

export function Redact() {
  const st = useApp();
  const src = useCommitted();
  const [boxes, setBoxes] = useState<Record<number, Box[]>>({});
  const [draft, setDraft] = useState<Box | null>(null);
  const [selId, setSelId] = useState<string | null>(null);
  const [query, setQuery] = useState('');
  const [terms, setTerms] = useState<string[]>([]);
  const [strip, setStrip] = useState(true);
  const [result, setResult] = useState<Verification | null>(null);
  const stageRef = useRef<HTMLDivElement>(null);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const start = useRef<[number, number] | null>(null);

  const page = Math.min(st.editPage, Math.max(0, (src?.info.pages ?? 1) - 1));
  const info = src?.info.pageInfo[page];
  const stageW = useStageWidth(stageRef, info, 1);
  if (!src || !info) return <div class="view hint">Preparing pages…</div>;
  const list = boxes[page] ?? [];
  const total = Object.values(boxes).reduce((n, b) => n + b.length, 0);

  const findAll = async () => {
    const q = query.trim();
    if (!q) return;
    const hits = await engine.search(src.key, q);
    if (!hits.length) return toast(`No “${q}” found. (Scanned pages? Run OCR first.)`);
    setBoxes((b) => {
      const out = { ...b };
      for (const h of hits) {
        const pi = src.info.pageInfo[h.p];
        const add = h.r.map((r) => {
          const pad = (r[3] - r[1]) * 0.06;
          const v = userToVisual(pi, [r[0], r[1] - pad, r[2], r[3] + pad]);
          return { id: newId('r'), kind: 'redact' as const, ...v };
        });
        out[h.p] = [...(out[h.p] ?? []), ...add];
      }
      return out;
    });
    setTerms((t) => [...new Set([...t, q])]);
    setQuery('');
    toast(`Marked ${hits.length} match${hits.length > 1 ? 'es' : ''} for redaction`, 'success');
  };

  const onDown = (e: PointerEvent) => {
    const t = (e.target as Element).closest('[data-annot]');
    if (t) {
      setSelId(t.getAttribute('data-annot'));
      return;
    }
    setSelId(null);
    start.current = pointOf(e, svgRef.current!, info);
    svgRef.current!.setPointerCapture(e.pointerId);
    setDraft({
      id: newId('r'),
      kind: 'redact',
      x: start.current[0],
      y: start.current[1],
      w: 0,
      h: 0,
    });
  };
  const onMove = (e: PointerEvent) => {
    if (!start.current || !draft) return;
    const [x, y] = pointOf(e, svgRef.current!, info);
    const [sx, sy] = start.current;
    setDraft({
      ...draft,
      x: Math.min(sx, x),
      y: Math.min(sy, y),
      w: Math.abs(x - sx),
      h: Math.abs(y - sy),
    });
  };
  const onUp = () => {
    if (draft && draft.w > 2 && draft.h > 2) setBoxes((b) => ({ ...b, [page]: [...(b[page] ?? []), draft] }));
    setDraft(null);
    start.current = null;
  };
  const removeSel = () => {
    if (!selId) return;
    setBoxes((b) => ({
      ...b,
      [page]: (b[page] ?? []).filter((x) => x.id !== selId),
    }));
    setSelId(null);
  };

  const apply = async () => {
    if (!total) return toast('Draw boxes over what should vanish, or search for words.');
    const items = Object.entries(boxes)
      .filter(([, b]) => b.length)
      .map(([p, b]) => ({
        page: +p,
        rects: b.map((x) => visualToUser(src.info.pageInfo[+p], x.x, x.y, x.w, x.h)),
      }));
    const usedTerms = terms.filter((t) => t.trim());
    const ops: EditOp[] = [
      ...items.map((it): EditOp => ({ type: 'redact', page: it.page, rects: it.rects, fill: 0x000000 })),
      // the same words may also hide in annotations, form values, bookmarks and metadata
      ...usedTerms.map((term): EditOp => ({ type: 'scrubTerm', term })),
    ];
    const flags = getState().saveFlags | SAVE.GC | SAVE.COMPRESS | (strip ? SAVE.STRIPMETA : 0);
    await withBusy('Redacting for real (not just a black box)…', async (ctx) => {
      // Fail closed if the renderer and the engine disagree about where a page is: a box drawn on
      // screen must land on exactly the same spot in the file.
      for (const it of items) {
        if (!(await geometryMatches(src.key, it.page, src.info.pageInfo[it.page]))) {
          throw new Error(`Page ${it.page + 1} has unusual geometry, so I can't be sure the box lands in the right place. Nothing was changed.`);
        }
      }
      const res = await applyEdits(ops, flags, ctx);
      // Two independent checks on the saved bytes: the engine's own byte-level scan, and PDF.js.
      const v = await engine.verify(res.bytes, items, usedTerms);
      const independent = await independentCheck(res.bytes, items, usedTerms, src.info.pages);
      const ok = v.remaining === 0 && v.stillFound.length === 0 && !independent.underBoxes && independent.termsFound.length === 0;
      if (!ok) {
        undo(); // never leave a half-redacted document around to be downloaded by mistake
        setResult({ reports: res.reports as RedactReport[], ...v, terms: usedTerms, stripped: strip, independent, reverted: true });
        return;
      }
      if (strip) setState({ saveFlags: getState().saveFlags | SAVE.STRIPMETA });
      setResult({ reports: res.reports as RedactReport[], ...v, terms: usedTerms, stripped: strip, independent, reverted: false });
      if (usedTerms.length) record({ op: 'redactText', terms: usedTerms });
      if (strip) record({ op: 'stripMetadata' });
      setBoxes({});
      setTerms([]);
      setSuffix('redacted');
    });
  };

  const agg = result?.reports
    .filter((r) => r && 'glyphs' in (r as object))
    .reduce(
      (a, r) => ({
        glyphs: a.glyphs + (r?.glyphs ?? 0),
        scrub: a.scrub + (r?.imagesScrubbed ?? 0),
        rm: a.rm + (r?.imagesRemoved ?? 0),
        annots: a.annots + (r?.annots ?? 0),
      }),
      { glyphs: 0, scrub: 0, rm: 0, annots: 0 },
    );
  const verified = result && !result.reverted;

  return (
    <div class="editor">
      <nav class="pages" aria-label="Pages">
        {Array.from({ length: src.info.pages }, (_, i) => (
          <button
            key={i}
            aria-current={i === page}
            onClick={() => setState({ editPage: i })}
            aria-label={`Page ${i + 1}${(boxes[i] ?? []).length ? `, ${(boxes[i] ?? []).length} redactions` : ''}`}
          >
            <MiniPage srcKey={src.key} index={i} />
            <span class="hint">
              {i + 1}
              {(boxes[i] ?? []).length ? ` ■${(boxes[i] ?? []).length}` : ''}
            </span>
          </button>
        ))}
      </nav>
      <section class="stage" ref={stageRef} aria-label="Redaction editor">
        <div class="stage-toolbar">
          <span class="hint">Drag to cover anything secret.</span>
          <button class="btn small" onClick={() => setState({ editPage: Math.max(0, page - 1) })} disabled={page === 0} aria-label="Previous page">
            <Icon name="chevl" />
          </button>
          <span class="hint">
            {page + 1} / {src.info.pages}
          </span>
          <button
            class="btn small"
            onClick={() => setState({ editPage: Math.min(src.info.pages - 1, page + 1) })}
            disabled={page >= src.info.pages - 1}
            aria-label="Next page"
          >
            <Icon name="chevr" />
          </button>
          <button class="btn small danger" onClick={removeSel} disabled={!selId}>
            <Icon name="trash" /> Remove box
          </button>
        </div>
        <PageView
          srcKey={src.key}
          index={page}
          info={info}
          width={stageW}
          label={`Page ${page + 1}`}
          overlayProps={{
            ref: (el: SVGSVGElement | null) => (svgRef.current = el),
            onPointerDown: onDown,
            onPointerMove: onMove,
            onPointerUp: onUp,
            style: { cursor: 'crosshair' },
            'data-testid': 'redact-overlay',
          }}
        >
          {[...list, ...(draft ? [draft] : [])].map((b) => (
            <AnnotShape key={b.id} a={b} selected={b.id === selId} />
          ))}
        </PageView>
      </section>
      <aside class="side">
        <div class="panel">
          <h2>
            <Icon name="search" /> Find &amp; redact
          </h2>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              void findAll();
            }}
            class="row"
          >
            <input
              class="text"
              style={{ flex: 1 }}
              type="text"
              placeholder="e.g. a name or account number"
              value={query}
              onInput={(e) => setQuery((e.target as HTMLInputElement).value)}
              aria-label="Text to find and redact"
              data-testid="redact-search"
            />
            <button class="btn" type="submit">
              Mark all
            </button>
          </form>
          {terms.length > 0 && <p class="hint">Will verify these are gone everywhere: {terms.map((t) => `“${t}”`).join(', ')}</p>}
        </div>
        <div class="panel">
          <h2>
            <Icon name="shield" /> Redaction
          </h2>
          <p class="hint" style={{ marginTop: 0 }}>
            {total} area{total === 1 ? '' : 's'} marked. Pagenoodle deletes the text, image pixels and form data underneath — not just paints over them — then
            re-reads the file to prove it.
          </p>
          <label
            style={{
              display: 'flex',
              gap: 8,
              alignItems: 'center',
              margin: '8px 0',
            }}
          >
            <input type="checkbox" checked={strip} onChange={(e) => setStrip((e.target as HTMLInputElement).checked)} /> Also strip metadata (author, title,
            XMP…)
          </label>
          <button class="btn primary" style={{ width: '100%', justifyContent: 'center' }} onClick={apply} disabled={!total} data-testid="apply-redaction">
            <Icon name="redact" /> Apply redaction
          </button>
          <button
            class="btn ghost"
            style={{ width: '100%', justifyContent: 'center', marginTop: 6 }}
            onClick={() => {
              setBoxes({});
              setTerms([]);
            }}
            disabled={!total}
          >
            Clear all boxes
          </button>
        </div>
      </aside>
      {result && agg && (
        <Dialog
          title={verified ? 'Redaction verified ✅' : 'Redaction not applied ⚠️'}
          onClose={() => setResult(null)}
          actions={
            <button class="btn primary" onClick={() => setResult(null)} autoFocus>
              {verified ? 'Great' : 'OK'}
            </button>
          }
        >
          <div data-testid="redaction-result">
            {verified ? (
              <>
                <div class="check">
                  <span class="ok">✓</span>
                  <div>
                    I re-opened the saved file and checked it two independent ways (the PDF engine and PDF.js): <b>no characters</b> remain under the boxes.
                  </div>
                </div>
                {result.terms.length > 0 && (
                  <div class="check">
                    <span class="ok">✓</span>
                    <div>
                      {result.terms.map((t) => `“${t}”`).join(', ')} {result.terms.length > 1 ? 'were' : 'was'} not found in the page text, annotations, form
                      fields, bookmarks, metadata or any other text in the file (searched byte by byte, including compressed data).
                    </div>
                  </div>
                )}
              </>
            ) : (
              <div class="check">
                <span class="bad">✗</span>
                <div>
                  <b>I undid the redaction, because I couldn't prove it worked</b> — nothing was changed, so you can't accidentally share a half-redacted file.
                  {result.remaining > 0 || result.independent.underBoxes
                    ? ` Text still found under the boxes: “${result.leaked || result.independent.underBoxes}”.`
                    : ''}
                  {result.stillFound.length ? ` Still present: ${result.stillFound.join(', ')}.` : ''}
                  {result.independent.termsFound.length ? ` PDF.js still reads: ${result.independent.termsFound.join(', ')}.` : ''} Try a larger box, or flatten
                  the page to an image first.
                </div>
              </div>
            )}
            {verified && (
              <ul>
                <li>
                  <b>{agg.glyphs}</b> characters removed from the page content
                </li>
                {agg.scrub > 0 && (
                  <li>
                    <b>{agg.scrub}</b> image{agg.scrub > 1 ? 's' : ''} had pixels under the boxes wiped
                  </li>
                )}
                {agg.rm > 0 && (
                  <li>
                    <b>{agg.rm}</b> image{agg.rm > 1 ? 's' : ''} removed entirely (couldn't be partially wiped safely)
                  </li>
                )}
                {agg.annots > 0 && (
                  <li>
                    <b>{agg.annots}</b> annotation/form field
                    {agg.annots > 1 ? 's' : ''} removed
                  </li>
                )}
                {result.stripped && <li>Document metadata stripped</li>}
                <li>The file was fully rewritten — no hidden older versions remain.</li>
                <li>Images and forms shared with other pages are redacted everywhere they appear.</li>
              </ul>
            )}
          </div>
        </Dialog>
      )}
    </div>
  );
}
