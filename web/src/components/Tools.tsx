// Tools: page numbers / headers / footers, compress, metadata, export images, OCR, flatten, images→PDF.
import { useState } from 'preact/hooks';
import { useApp, withBusy, record, toast, getState, addFiles, exportBytes } from '../store/app';
import { runStep, type Step } from '../pipeline/steps';
import { Icon, Field } from './ui';
import { renderToCanvas, loadPdf, unloadPdf } from '../render/pdf';
import { downloadBlob, formatBytes, makeZip, parseRanges, safeName } from '../lib/util';
import { celebrate, setSuffix } from './actions';
import { hexToRgb } from '../lib/overlay';
import type { PageSizeMode } from '../engine/types';

const POS = ['Top left', 'Top center', 'Top right', 'Bottom left', 'Bottom center', 'Bottom right'];

async function go(step: Step, label: string, after?: (r: Awaited<ReturnType<typeof runStep>>) => void) {
  await withBusy(label, async (ctx) => {
    const r = await runStep(step, ctx);
    record(step);
    after?.(r);
  });
}

function StampCard() {
  const [template, setTemplate] = useState('Page {n} of {total}');
  const [position, setPosition] = useState(4);
  const [size, setSize] = useState(10);
  const [margin, setMargin] = useState(24);
  const [start, setStart] = useState(1);
  const [pages, setPages] = useState('all');
  const [color, setColor] = useState('#333333');
  const presets = ['Page {n} of {total}', '{n}', '- {n} -', 'Confidential', getState().baseName];
  return (
    <section class="card" aria-labelledby="stamp-h">
      <h2 id="stamp-h">
        <Icon name="hash" /> Page numbers, headers &amp; footers
      </h2>
      <p>
        Use <code>{'{n}'}</code> for the page number and <code>{'{total}'}</code> for the count.
      </p>
      <Field label="Text">
        <input type="text" value={template} onInput={(e) => setTemplate((e.target as HTMLInputElement).value)} list="stamp-presets" />
      </Field>
      <datalist id="stamp-presets">
        {presets.map((p) => (
          <option key={p} value={p} />
        ))}
      </datalist>
      <div class="lbl" style={{ fontWeight: 600, fontSize: 13 }} id="pos-lbl">
        Position
      </div>
      <div class="positions" role="radiogroup" aria-labelledby="pos-lbl">
        {POS.map((p, i) => (
          <button key={p} class="btn small" role="radio" aria-checked={position === i} onClick={() => setPosition(i)}>
            {p}
          </button>
        ))}
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 8 }}>
        <Field label="Size (pt)">
          <input type="number" min={5} max={72} value={size} onInput={(e) => setSize(+(e.target as HTMLInputElement).value)} />
        </Field>
        <Field label="Margin (pt)">
          <input type="number" min={0} max={200} value={margin} onInput={(e) => setMargin(+(e.target as HTMLInputElement).value)} />
        </Field>
        <Field label="Start at">
          <input type="number" min={0} value={start} onInput={(e) => setStart(+(e.target as HTMLInputElement).value)} />
        </Field>
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: '2fr 1fr', gap: 8 }}>
        <Field label="Pages" hint="all, odd, even, 2-, 1-3,7">
          <input type="text" value={pages} onInput={(e) => setPages((e.target as HTMLInputElement).value)} />
        </Field>
        <Field label="Color">
          <input type="color" value={color} onInput={(e) => setColor((e.target as HTMLInputElement).value)} style={{ height: 36 }} />
        </Field>
      </div>
      <div class="actions">
        <button
          class="btn primary"
          data-testid="apply-stamp"
          onClick={() => {
            const [r, g, b] = hexToRgb(color).map((v) => Math.round(v * 255));
            void go(
              {
                op: 'stamp',
                template,
                position,
                size,
                margin,
                start,
                pages,
                font: 'Helvetica',
                color: (r << 16) | (g << 8) | b,
              },
              'Numbering noodles…',
              () => {
                setSuffix('numbered');
                toast('Stamped! 🔢', 'success');
              },
            );
          }}
        >
          <Icon name="check" /> Add to pages
        </button>
      </div>
    </section>
  );
}

function CompressCard() {
  const [level, setLevel] = useState<'lossless' | 'balanced' | 'strong'>('balanced');
  const [last, setLast] = useState('');
  const levels: [typeof level, string, string][] = [
    ['lossless', 'Lossless', 'Repack & deduplicate. Pixel-perfect.'],
    ['balanced', 'Balanced', 'Re-encode big images (JPEG 72%, ≤2200px).'],
    ['strong', 'Tiny', 'Squeeze hard (JPEG 50%, ≤1400px). Great for email.'],
  ];
  return (
    <section class="card" aria-labelledby="cmp-h">
      <h2 id="cmp-h">
        <Icon name="compress" /> Compress
      </h2>
      <div role="radiogroup" aria-labelledby="cmp-h">
        {levels.map(([k, l, d]) => (
          <label
            key={k}
            style={{
              display: 'flex',
              gap: 8,
              alignItems: 'flex-start',
              margin: '6px 0',
            }}
          >
            <input type="radio" name="level" checked={level === k} onChange={() => setLevel(k)} />
            <span>
              <b>{l}</b> — <span class="hint">{d}</span>
            </span>
          </label>
        ))}
      </div>
      {last && <p role="status">{last}</p>}
      <div class="actions">
        <button
          class="btn primary"
          data-testid="apply-compress"
          onClick={() =>
            go({ op: 'compress', level }, 'Squishing your PDF…', (r) => {
              const [a, b] = (r.report ?? '0→0').split('→').map(Number);
              const pct = a ? Math.round((1 - b / a) * 100) : 0;
              setLast(b < a ? `${formatBytes(a)} → ${formatBytes(b)} (${pct}% smaller)` : `${formatBytes(a)} → ${formatBytes(b)} — already pretty lean!`);
              setSuffix('compressed');
            })
          }
        >
          <Icon name="compress" /> Compress
        </button>
      </div>
    </section>
  );
}

function MetadataCard() {
  const st = useApp();
  const infos = Object.values(st.doc.sources).map((s) => s.info);
  const entries = infos.flatMap((i) => Object.entries(i.info).filter(([, v]) => v));
  const xmp = infos.some((i) => i.hasXmp);
  return (
    <section class="card" aria-labelledby="meta-h">
      <h2 id="meta-h">
        <Icon name="eye" /> Metadata
      </h2>
      {entries.length || xmp ? (
        <>
          <p>This PDF carries hidden details:</p>
          <ul style={{ margin: '4px 0', paddingLeft: 18, fontSize: 13 }}>
            {entries.slice(0, 8).map(([k, v]) => (
              <li key={k}>
                <b>{k}:</b> {v}
              </li>
            ))}
            {xmp && <li>XMP metadata packet</li>}
          </ul>
        </>
      ) : (
        <p>No document metadata found. Clean as a fresh noodle. ✨</p>
      )}
      <div class="actions">
        <button class="btn primary" onClick={() => go({ op: 'stripMetadata' }, 'Scrubbing metadata…', () => toast('Metadata stripped 🧽', 'success'))}>
          <Icon name="shield" /> Strip metadata
        </button>
      </div>
    </section>
  );
}

function ExportImagesCard() {
  const [fmt, setFmt] = useState<'png' | 'jpeg'>('png');
  const [dpi, setDpi] = useState(150);
  const [pages, setPages] = useState('all');
  const run = async () => {
    const st = getState();
    await withBusy('Painting pictures of your pages…', async ({ onProgress, signal }) => {
      const bytes = await exportBytes({ signal });
      const key = `export-${Date.now()}`;
      await loadPdf(key, bytes);
      const idx = parseRanges(pages, st.doc.pages.length);
      const files: { name: string; data: Uint8Array }[] = [];
      try {
        for (let k = 0; k < idx.length; k++) {
          onProgress(k, idx.length);
          const c = await renderToCanvas(key, idx[k], {
            scale: dpi / 72,
            dpr: 1,
            signal,
            priority: 5,
          });
          const blob: Blob = await new Promise((res, rej) =>
            c.toBlob((b) => (b ? res(b) : rej(new Error('encode failed'))), fmt === 'png' ? 'image/png' : 'image/jpeg', 0.9),
          );
          files.push({
            name: `${safeName(st.baseName)}-page-${idx[k] + 1}.${fmt === 'png' ? 'png' : 'jpg'}`,
            data: new Uint8Array(await blob.arrayBuffer()),
          });
        }
      } finally {
        unloadPdf(key);
      }
      if (files.length === 1) downloadBlob(files[0].data, files[0].name, fmt === 'png' ? 'image/png' : 'image/jpeg');
      else downloadBlob(makeZip(files), `${safeName(st.baseName)}-images.zip`, 'application/zip');
      celebrate();
    });
  };
  return (
    <section class="card" aria-labelledby="img-h">
      <h2 id="img-h">
        <Icon name="image" /> Pages → PNG / JPG
      </h2>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 8 }}>
        <Field label="Format">
          <select value={fmt} onChange={(e) => setFmt((e.target as HTMLSelectElement).value as 'png' | 'jpeg')}>
            <option value="png">PNG</option>
            <option value="jpeg">JPG</option>
          </select>
        </Field>
        <Field label="Quality">
          <select value={dpi} onChange={(e) => setDpi(+(e.target as HTMLSelectElement).value)}>
            <option value={72}>Screen (72 dpi)</option>
            <option value={150}>Good (150 dpi)</option>
            <option value={300}>Print (300 dpi)</option>
          </select>
        </Field>
        <Field label="Pages">
          <input type="text" value={pages} onInput={(e) => setPages((e.target as HTMLInputElement).value)} />
        </Field>
      </div>
      <div class="actions">
        <button class="btn primary" onClick={run}>
          <Icon name="download" /> Export images
        </button>
      </div>
    </section>
  );
}

function OcrCard() {
  const [skip, setSkip] = useState(true);
  const [msg, setMsg] = useState('');
  return (
    <section class="card" aria-labelledby="ocr-h">
      <h2 id="ocr-h">
        <Icon name="search" /> Make scans searchable (OCR)
      </h2>
      <p>Reads scanned pages on your device and adds an invisible text layer, so you can search, select and copy. English model included — works offline.</p>
      <label style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
        <input type="checkbox" checked={skip} onChange={(e) => setSkip((e.target as HTMLInputElement).checked)} /> Skip pages that already have text
      </label>
      {msg && <p role="status">{msg}</p>}
      <div class="actions">
        <button
          class="btn primary"
          onClick={() =>
            go({ op: 'ocr', skipText: skip }, 'Reading your scans (this can take a moment)…', (r) => {
              setMsg(r.report ?? 'Done!');
              setSuffix('searchable');
            })
          }
        >
          <Icon name="eye" /> Run OCR
        </button>
      </div>
    </section>
  );
}

function FormsCard() {
  const st = useApp();
  const has = Object.values(st.doc.sources).some((s) => s.info.hasForm);
  return (
    <section class="card" aria-labelledby="flat-h">
      <h2 id="flat-h">
        <Icon name="form" /> Lock form answers
      </h2>
      <p>{has ? 'Bake filled-in form fields into the page so they can no longer be edited.' : 'This document has no interactive form fields.'}</p>
      <div class="actions">
        <button class="btn" disabled={!has} onClick={() => go({ op: 'flattenForms' }, 'Flattening form…', () => toast('Form flattened 🥞', 'success'))}>
          Flatten forms
        </button>
      </div>
    </section>
  );
}

function ImagesCard() {
  const [mode, setMode] = useState<PageSizeMode>('fit-a4');
  return (
    <section class="card" aria-labelledby="i2p-h">
      <h2 id="i2p-h">
        <Icon name="image" /> Images → PDF
      </h2>
      <p>Add photos or scans (PNG, JPG, WebP) as new pages. JPEGs are embedded without recompression.</p>
      <Field label="Page size">
        <select value={mode} onChange={(e) => setMode((e.target as HTMLSelectElement).value as PageSizeMode)}>
          <option value="fit-a4">Fit to A4</option>
          <option value="fit-letter">Fit to US Letter</option>
          <option value="original">Original size</option>
        </select>
      </Field>
      <div class="actions">
        <label class="btn">
          <Icon name="plus" /> Choose images
          <input
            type="file"
            accept="image/*"
            multiple
            class="sr-only"
            onChange={(e) => {
              const files = [...((e.target as HTMLInputElement).files ?? [])];
              (e.target as HTMLInputElement).value = '';
              if (files.length) void addFiles(files, mode);
            }}
          />
        </label>
      </div>
    </section>
  );
}

export function Tools() {
  return (
    <div class="view">
      <div class="cards">
        <StampCard />
        <CompressCard />
        <MetadataCard />
        <ExportImagesCard />
        <OcrCard />
        <FormsCard />
        <ImagesCard />
      </div>
    </div>
  );
}
