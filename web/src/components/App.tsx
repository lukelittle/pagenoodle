import { useEffect, useState } from 'preact/hooks';
import { useApp, setState, undo, redo, addFiles, clearAll, type Mode } from '../store/app';
import { Icon, Mascot, BusyOverlay, Toasts, Celebrate } from './ui';
import { Organize } from './Organize';
import { Editor } from './Editor';
import { Redact } from './Redact';
import { Tools } from './Tools';
import { Recipes } from './Recipes';
import { Compare } from './Compare';
import { Privacy } from './Privacy';
import { downloadCurrent, openFilePicker } from './actions';

const TABS: [Mode, string, string, boolean][] = [
  ['organize', 'Organize', 'layers', true],
  ['edit', 'Edit & Sign', 'sign', true],
  ['redact', 'Redact', 'redact', true],
  ['tools', 'Tools', 'hash', true],
  ['recipes', 'Recipes', 'play', false],
  ['compare', 'Compare', 'diff', false],
  ['privacy', 'Privacy', 'shield', false],
];

function useOfflineStatus() {
  const [online, setOnline] = useState(navigator.onLine);
  const [ready, setReady] = useState(false);
  useEffect(() => {
    const on = () => setOnline(navigator.onLine);
    window.addEventListener('online', on);
    window.addEventListener('offline', on);
    if ('serviceWorker' in navigator) navigator.serviceWorker.ready.then(() => setReady(true)).catch(() => {});
    return () => {
      window.removeEventListener('online', on);
      window.removeEventListener('offline', on);
    };
  }, []);
  return { online, ready };
}

function EmptyState() {
  return (
    <div class="empty">
      <div class="drop">
        <Mascot size={150} />
        <h1>Hi! I'm Pagenoodle.</h1>
        <p>
          Drop PDFs or images anywhere and I'll help you merge, split, sign, fill, redact, compress and OCR them —<b> right here on your device.</b> Nothing
          gets uploaded. Ever.
        </p>
        <div class="row">
          <button class="btn primary" onClick={openFilePicker} data-testid="open-files">
            <Icon name="open" /> Choose files
          </button>
          <button class="btn" onClick={() => setState({ mode: 'privacy' })}>
            <Icon name="wifi" /> How is this private?
          </button>
        </div>
        <div class="features">
          <div>
            <b>🧩 Organize</b>Reorder, rotate, merge &amp; split
          </div>
          <div>
            <b>✍️ Sign &amp; fill</b>Draw, type or upload
          </div>
          <div>
            <b>⬛ True redaction</b>Text really removed, then verified
          </div>
          <div>
            <b>📒 Recipes</b>Replay routines in one click
          </div>
          <div>
            <b>🔍 OCR</b>Make scans searchable
          </div>
          <div>
            <b>🆚 Compare</b>Spot changes between versions
          </div>
        </div>
      </div>
    </div>
  );
}

export function App() {
  const st = useApp();
  const { online, ready } = useOfflineStatus();
  const [dragging, setDragging] = useState(false);
  const hasDoc = st.doc.pages.length > 0;

  useEffect(() => {
    let depth = 0;
    const hasFiles = (e: DragEvent) => [...(e.dataTransfer?.types ?? [])].includes('Files');
    const enter = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      depth++;
      setDragging(true);
    };
    const leave = () => {
      depth = Math.max(0, depth - 1);
      if (!depth) setDragging(false);
    };
    const over = (e: DragEvent) => {
      if (hasFiles(e)) e.preventDefault();
    };
    const drop = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      depth = 0;
      setDragging(false);
      const files = [...(e.dataTransfer?.files ?? [])];
      if (files.length) void addFiles(files);
    };
    window.addEventListener('dragenter', enter);
    window.addEventListener('dragleave', leave);
    window.addEventListener('dragover', over);
    window.addEventListener('drop', drop);
    const key = (e: KeyboardEvent) => {
      const mod = e.ctrlKey || e.metaKey;
      const t = e.target as HTMLElement;
      const typing = t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable;
      if (mod && e.key.toLowerCase() === 'o') {
        e.preventDefault();
        openFilePicker();
      } else if (mod && e.key.toLowerCase() === 's') {
        e.preventDefault();
        void downloadCurrent();
      } else if (!typing && mod && e.key.toLowerCase() === 'z') {
        e.preventDefault();
        if (e.shiftKey) redo();
        else undo();
      } else if (!typing && mod && e.key.toLowerCase() === 'y') {
        e.preventDefault();
        redo();
      }
    };
    window.addEventListener('keydown', key);
    // PWA file handling ("Open with Pagenoodle")
    const lq = (
      window as unknown as {
        launchQueue?: {
          setConsumer: (cb: (p: { files: FileSystemFileHandle[] }) => void) => void;
        };
      }
    ).launchQueue;
    lq?.setConsumer(async (p) => {
      const files = await Promise.all(p.files.map((h) => h.getFile()));
      if (files.length) void addFiles(files);
    });
    return () => {
      window.removeEventListener('dragenter', enter);
      window.removeEventListener('dragleave', leave);
      window.removeEventListener('dragover', over);
      window.removeEventListener('drop', drop);
      window.removeEventListener('keydown', key);
    };
  }, []);

  useEffect(() => {
    document.body.classList.toggle('dragging-over', dragging);
  }, [dragging]);

  const needsDoc = TABS.find((t) => t[0] === st.mode)?.[3] ?? true;
  let body;
  if (needsDoc && !hasDoc) body = <EmptyState />;
  else
    body = {
      organize: <Organize />,
      edit: <Editor />,
      redact: <Redact />,
      tools: <Tools />,
      recipes: <Recipes />,
      compare: <Compare />,
      privacy: <Privacy />,
    }[st.mode];

  return (
    <>
      <a class="skip-link" href="#main">
        Skip to content
      </a>
      <header class="header">
        <button class="brand" onClick={() => setState({ mode: 'organize' })}>
          <Mascot size={48} wiggle={!!st.busy} />
          <span>
            Pagenoodle
            <small>PDF tools that never leave your device</small>
          </span>
        </button>
        <button
          class={`badge${online ? '' : ' offline'}`}
          onClick={() => setState({ mode: 'privacy' })}
          title="How Pagenoodle keeps your files private"
          data-testid="offline-badge"
        >
          <Icon name={online ? 'wifi' : 'check'} />
          {online ? (ready ? 'Works with Wi-Fi off' : 'Private · no uploads') : "You're offline — still works!"}
        </button>
        {st.recording && (
          <span class="badge offline" role="status">
            <Icon name="record" /> Recording recipe · {st.recording.length} step
            {st.recording.length === 1 ? '' : 's'}
          </span>
        )}
        <span class="spacer" />
        <button class="btn ghost" onClick={undo} disabled={!st.past.length} aria-label="Undo" title="Undo (Ctrl+Z)">
          <Icon name="undo" />
        </button>
        <button class="btn ghost" onClick={redo} disabled={!st.future.length} aria-label="Redo" title="Redo (Ctrl+Shift+Z)">
          <Icon name="redo" />
        </button>
        <button class="btn" onClick={openFilePicker} title="Open (Ctrl+O)">
          <Icon name="open" /> Open
        </button>
        <button class="btn primary" onClick={downloadCurrent} disabled={!hasDoc} title="Download (Ctrl+S)" data-testid="download">
          <Icon name="download" /> Download
        </button>
        {hasDoc && (
          <button
            class="btn ghost"
            aria-label="Close all files"
            title="Close all files"
            onClick={() => {
              if (confirm('Close all files? Unsaved changes will be lost (nothing was uploaded, so nothing to clean up elsewhere).')) clearAll();
            }}
          >
            <Icon name="x" />
          </button>
        )}
      </header>
      <nav class="tabs" role="tablist" aria-label="Sections">
        {TABS.map(([m, label, icon]) => (
          <button
            key={m}
            class="tab"
            role="tab"
            aria-selected={st.mode === m}
            aria-controls="main"
            onClick={() => setState({ mode: m })}
            data-testid={`tab-${m}`}
          >
            <Icon name={icon} /> {label}
          </button>
        ))}
      </nav>
      <main id="main" tabIndex={-1}>
        {body}
      </main>
      <BusyOverlay />
      <Toasts />
      <Celebrate />
    </>
  );
}
