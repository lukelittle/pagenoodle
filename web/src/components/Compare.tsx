// Compare two versions: side-by-side text with additions/deletions highlighted + per-page summary.
import { useState } from 'preact/hooks';
import { useApp, withBusy, newId, toast } from '../store/app';
import { engine } from '../engine/client';
import { compareDocs, type CompareResult } from '../lib/diff';
import { Icon } from './ui';

interface Pick {
  name: string;
  key: string;
}

export function Compare() {
  const st = useApp();
  const sources = Object.values(st.doc.sources);
  const [a, setA] = useState<Pick | null>(null);
  const [b, setB] = useState<Pick | null>(null);
  const [res, setRes] = useState<CompareResult | null>(null);
  const [onlyChanged, setOnlyChanged] = useState(true);

  const pickFile = async (f: File, set: (p: Pick) => void) => {
    await withBusy(`Reading ${f.name}…`, async () => {
      const key = newId('cmp');
      await engine.open(key, new Uint8Array(await f.arrayBuffer()));
      set({ name: f.name, key });
    });
  };
  const run = async () => {
    if (!a || !b) return;
    await withBusy('Spotting the differences…', async (ctx) => {
      const [ta, tb] = [await engine.textAll(a.key, ctx), await engine.textAll(b.key, ctx)];
      const r = compareDocs(ta, tb);
      setRes(r);
      if (!ta.join('').trim() && !tb.join('').trim()) toast('Neither file has text — try OCR first for scans.', 'info');
    });
  };

  const Picker = ({ label, value, set }: { label: string; value: Pick | null; set: (p: Pick) => void }) => (
    <section class="card" aria-label={label}>
      <h2>{label}</h2>
      {sources.length > 0 && (
        <select
          class="btn"
          value={value && sources.some((s) => s.key === value.key) ? value.key : ''}
          onChange={(e) => {
            const s = sources.find((x) => x.key === (e.target as HTMLSelectElement).value);
            if (s) set({ name: s.name, key: s.key });
          }}
          aria-label={`${label}: choose an open file`}
        >
          <option value="">Choose an open file…</option>
          {sources.map((s) => (
            <option key={s.key} value={s.key}>
              {s.name}
            </option>
          ))}
        </select>
      )}
      <label class="btn">
        <Icon name="open" /> {sources.length ? 'or pick a file' : 'Pick a PDF'}
        <input
          type="file"
          accept="application/pdf,.pdf"
          class="sr-only"
          data-testid={`compare-${label === 'Original' ? 'a' : 'b'}`}
          onChange={(e) => {
            const f = (e.target as HTMLInputElement).files?.[0];
            (e.target as HTMLInputElement).value = '';
            if (f) void pickFile(f, set);
          }}
        />
      </label>
      <p class="hint">{value ? `✓ ${value.name}` : 'Nothing chosen yet'}</p>
    </section>
  );

  return (
    <div class="view">
      <div class="cards" style={{ gridTemplateColumns: 'repeat(auto-fill, minmax(260px, 1fr))' }}>
        <Picker label="Original" value={a} set={setA} />
        <Picker label="Changed" value={b} set={setB} />
        <section class="card">
          <h2>
            <Icon name="diff" /> Compare
          </h2>
          <p>Shows text added and removed, page by page. Everything is computed on your device.</p>
          <div class="actions">
            <button class="btn primary" disabled={!a || !b} onClick={run} data-testid="run-compare">
              Compare versions
            </button>
          </div>
        </section>
      </div>
      {res && (
        <div style={{ marginTop: 18 }} data-testid="compare-result">
          <section class="panel" aria-labelledby="sum-h" style={{ marginBottom: 14 }}>
            <h2 id="sum-h">
              {res.identical
                ? '🎉 No text differences — identical twins!'
                : `Summary: ${res.added} word${res.added === 1 ? '' : 's'} added, ${res.removed} removed`}
            </h2>
            <table class="summary">
              <thead>
                <tr>
                  <th scope="col">Page</th>
                  <th scope="col">Added</th>
                  <th scope="col">Removed</th>
                  <th scope="col">Status</th>
                </tr>
              </thead>
              <tbody>
                {res.summary.map((s, i) => (
                  <tr key={i}>
                    <td>{i + 1}</td>
                    <td>{s.added || '—'}</td>
                    <td>{s.removed || '—'}</td>
                    <td>{s.pageA === null ? 'Page added' : s.pageB === null ? 'Page removed' : s.added || s.removed ? 'Changed' : 'Unchanged'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <label
              style={{
                display: 'flex',
                gap: 8,
                alignItems: 'center',
                marginTop: 10,
              }}
            >
              <input type="checkbox" checked={onlyChanged} onChange={(e) => setOnlyChanged((e.target as HTMLInputElement).checked)} /> Only show changed pages
            </label>
          </section>
          {Array.from({ length: Math.max(res.left.length, res.right.length) }, (_, p) => {
            const s = res.summary[p];
            if (onlyChanged && s && !s.added && !s.removed && s.pageA !== null && s.pageB !== null) return null;
            return (
              <div class="compare-cols" key={p}>
                <div class="compare-page" aria-label={`Original page ${p + 1}`}>
                  <h3>Original · page {p + 1}</h3>
                  {res.left[p] ? (
                    res.left[p]
                      .map((seg, i) =>
                        seg.type === 'del' ? (
                          <del class="diff" key={i}>
                            {seg.text}
                          </del>
                        ) : (
                          <span key={i}>{seg.text}</span>
                        ),
                      )
                      .flatMap((x, i) => (i ? [' ', x] : [x]))
                  ) : (
                    <em class="hint">(no such page)</em>
                  )}
                </div>
                <div class="compare-page" aria-label={`Changed page ${p + 1}`}>
                  <h3>Changed · page {p + 1}</h3>
                  {res.right[p] ? (
                    res.right[p]
                      .map((seg, i) =>
                        seg.type === 'ins' ? (
                          <ins class="diff" key={i}>
                            {seg.text}
                          </ins>
                        ) : (
                          <span key={i}>{seg.text}</span>
                        ),
                      )
                      .flatMap((x, i) => (i ? [' ', x] : [x]))
                  ) : (
                    <em class="hint">(no such page)</em>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
