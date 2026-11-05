// Edit & Sign: add text, shapes, highlights, freehand ink and signatures; fill interactive forms.
import { useEffect, useRef, useState } from "preact/hooks";
import {
  useApp,
  setState,
  commit,
  applyEdits,
  withBusy,
  toast,
  newId,
  getState,
  record,
  type Source,
} from "../store/app";
import { engine } from "../engine/client";
import type { EditOp, FormField } from "../engine/types";
import { annotsToOps, userToVisual, type Annot } from "../lib/overlay";
import { canvasPixels } from "../lib/images";
import { Icon, Swatches, Field } from "./ui";
import { PageView, MiniPage, pointOf, useStageWidth } from "./PageView";
import { SignatureDialog, type SignatureResult } from "./SignatureDialog";
import { setSuffix } from "./actions";

type Tool =
  | "select"
  | "text"
  | "rect"
  | "ellipse"
  | "line"
  | "arrow"
  | "highlight"
  | "ink"
  | "sign";
const TOOLS: [Tool, string, string][] = [
  ["select", "Select", "pointer"],
  ["text", "Text", "text"],
  ["rect", "Box", "rect"],
  ["ellipse", "Oval", "ellipse"],
  ["line", "Line", "line"],
  ["arrow", "Arrow", "arrow"],
  ["highlight", "Highlight", "highlight"],
  ["ink", "Draw", "pen"],
];

export function useCommitted(): Source | null {
  const st = useApp();
  const [src, setSrc] = useState<Source | null>(null);
  const keys = Object.keys(st.doc.sources);
  const normalized =
    keys.length === 1 &&
    st.doc.pages.every((p, i) => p.index === i && p.rot === 0) &&
    st.doc.pages.length === st.doc.sources[keys[0]].info.pages;
  useEffect(() => {
    if (!st.doc.pages.length) return setSrc(null);
    if (normalized) return setSrc(st.doc.sources[keys[0]]);
    void withBusy("Getting the pages ready…", async (ctx) =>
      setSrc(await commit(ctx)),
    );
  }, [st.doc]);
  return src && st.doc.sources[src.key] ? src : null;
}

export function Editor() {
  const st = useApp();
  const src = useCommitted();
  const [tool, setTool] = useState<Tool>("select");
  const [color, setColor] = useState("#e4572e");
  const [width, setWidth] = useState(2);
  const [fontSize, setFontSize] = useState(14);
  const [annots, setAnnots] = useState<Record<number, Annot[]>>({});
  const [selId, setSelId] = useState<string | null>(null);
  const [draft, setDraft] = useState<Annot | null>(null);
  const [editingText, setEditingText] = useState<string | null>(null);
  const [sigOpen, setSigOpen] = useState(false);
  const [pendingSig, setPendingSig] = useState<SignatureResult | null>(null);
  const [zoom, setZoom] = useState(1);
  const [fields, setFields] = useState<FormField[]>([]);
  const [values, setValues] = useState<Record<number, string>>({});
  const stageRef = useRef<HTMLDivElement>(null);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const moveRef = useRef<{
    id: string;
    start: [number, number];
    orig: Annot;
    resize: boolean;
  } | null>(null);

  const page = Math.min(st.editPage, Math.max(0, (src?.info.pages ?? 1) - 1));
  const info = src?.info.pageInfo[page];
  const stageW = useStageWidth(stageRef, info, zoom);
  const list = annots[page] ?? [];
  const dirty =
    Object.values(annots).some((a) => a.length) ||
    Object.keys(values).length > 0;

  useEffect(() => {
    if (!src?.info.hasForm) return setFields([]);
    engine
      .forms(src.key)
      .then(setFields)
      .catch(() => setFields([]));
    setValues({});
  }, [src?.key]);

  // warn before leaving with unapplied work
  useEffect(() => {
    const h = (e: BeforeUnloadEvent) => {
      if (dirty) e.preventDefault();
    };
    window.addEventListener("beforeunload", h);
    return () => window.removeEventListener("beforeunload", h);
  }, [dirty]);

  const update = (id: string, patch: Partial<Annot>) =>
    setAnnots((a) => ({
      ...a,
      [page]: (a[page] ?? []).map((x) =>
        x.id === id ? ({ ...x, ...patch } as Annot) : x,
      ),
    }));
  const add = (an: Annot) =>
    setAnnots((a) => ({ ...a, [page]: [...(a[page] ?? []), an] }));
  const removeAnnot = (id: string) =>
    setAnnots((a) => ({
      ...a,
      [page]: (a[page] ?? []).filter((x) => x.id !== id),
    }));

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement;
      if (
        t.tagName === "INPUT" ||
        t.tagName === "TEXTAREA" ||
        t.tagName === "SELECT"
      )
        return;
      if ((e.key === "Delete" || e.key === "Backspace") && selId) {
        removeAnnot(selId);
        setSelId(null);
      } else if (selId && e.key.startsWith("Arrow")) {
        e.preventDefault();
        const d = e.shiftKey ? 10 : 1;
        const an = list.find((x) => x.id === selId);
        if (!an) return;
        const dx = e.key === "ArrowLeft" ? -d : e.key === "ArrowRight" ? d : 0;
        const dy = e.key === "ArrowUp" ? -d : e.key === "ArrowDown" ? d : 0;
        update(selId, translate(an, dx, dy));
      } else if (e.key === "Escape") {
        setSelId(null);
        setPendingSig(null);
        setTool("select");
      } else if (e.key === "PageDown")
        setState({ editPage: Math.min((src?.info.pages ?? 1) - 1, page + 1) });
      else if (e.key === "PageUp")
        setState({ editPage: Math.max(0, page - 1) });
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  if (!src || !info) return <div class="view hint">Preparing pages…</div>;

  const onDown = (e: PointerEvent) => {
    const svg = svgRef.current!;
    const [x, y] = pointOf(e, svg, info);
    const target = (e.target as Element).closest(
      "[data-annot]",
    ) as Element | null;
    if (pendingSig) {
      const w = info.vw * 0.28;
      const h = (w * pendingSig.canvas.height) / pendingSig.canvas.width;
      const px = canvasPixels(pendingSig.canvas);
      const an: Annot = {
        id: newId("a"),
        kind: "image",
        x: x - w / 2,
        y: y - h / 2,
        w,
        h,
        rgba: px.rgba,
        iw: px.w,
        ih: px.h,
        url: pendingSig.url,
        sigId: pendingSig.savedId,
      };
      add(an);
      setSelId(an.id);
      setPendingSig(null);
      setTool("select");
      return;
    }
    if (tool === "select") {
      if (target) {
        const id = target.getAttribute("data-annot")!;
        const an = list.find((a) => a.id === id)!;
        setSelId(id);
        moveRef.current = {
          id,
          start: [x, y],
          orig: an,
          resize: target.hasAttribute("data-handle"),
        };
        svg.setPointerCapture(e.pointerId);
      } else setSelId(null);
      return;
    }
    if (tool === "text") {
      const an: Annot = {
        id: newId("a"),
        kind: "text",
        x,
        y: y - fontSize * 0.6,
        text: "",
        size: fontSize,
        color,
      };
      add(an);
      setEditingText(an.id);
      setSelId(an.id);
      return;
    }
    svg.setPointerCapture(e.pointerId);
    const id = newId("a");
    if (tool === "rect" || tool === "ellipse")
      setDraft({ id, kind: tool, x, y, w: 0, h: 0, color, width });
    else if (tool === "highlight")
      setDraft({
        id,
        kind: "highlight",
        x,
        y,
        w: 0,
        h: 0,
        color: color === "#e4572e" ? "#ffe600" : color,
      });
    else if (tool === "line" || tool === "arrow")
      setDraft({ id, kind: tool, x1: x, y1: y, x2: x, y2: y, color, width });
    else if (tool === "ink")
      setDraft({ id, kind: "ink", points: [[x, y]], color, width });
    moveRef.current = {
      id,
      start: [x, y],
      orig: null as unknown as Annot,
      resize: false,
    };
  };
  const onMove = (e: PointerEvent) => {
    const m = moveRef.current;
    if (!m) return;
    const [x, y] = pointOf(e, svgRef.current!, info);
    if (draft) {
      const [sx, sy] = m.start;
      if (
        draft.kind === "rect" ||
        draft.kind === "ellipse" ||
        draft.kind === "highlight"
      )
        setDraft({
          ...draft,
          x: Math.min(sx, x),
          y: Math.min(sy, y),
          w: Math.abs(x - sx),
          h: Math.abs(y - sy),
        });
      else if (draft.kind === "line" || draft.kind === "arrow")
        setDraft({ ...draft, x2: x, y2: y });
      else if (draft.kind === "ink")
        setDraft({ ...draft, points: [...draft.points, [x, y]] });
      return;
    }
    const dx = x - m.start[0],
      dy = y - m.start[1];
    if (m.resize && "w" in m.orig) {
      const o = m.orig as Annot & { w: number; h: number };
      if (o.kind === "image") {
        const w = Math.max(10, o.w + dx);
        update(m.id, { w, h: (w * o.h) / o.w });
      } else
        update(m.id, { w: Math.max(4, o.w + dx), h: Math.max(4, o.h + dy) });
    } else update(m.id, translate(m.orig, dx, dy));
  };
  const onUp = () => {
    if (draft) {
      const tiny =
        "w" in draft
          ? draft.w < 3 && draft.h < 3
          : draft.kind === "ink"
            ? draft.points.length < 2
            : draft.kind === "line" || draft.kind === "arrow"
              ? Math.hypot(draft.x2 - draft.x1, draft.y2 - draft.y1) < 3
              : false;
      if (!tiny) add(draft);
      setDraft(null);
    }
    moveRef.current = null;
  };

  const apply = async () => {
    const ops: EditOp[] = [];
    for (const [p, arr] of Object.entries(annots)) {
      const items = arr.filter((a) => a.kind !== "text" || a.text.trim());
      if (!items.length) continue;
      const { ops: o, images } = annotsToOps(items, src.info.pageInfo[+p]);
      ops.push({ type: "overlay", page: +p, ops: o, images });
    }
    for (const [id, v] of Object.entries(values))
      ops.push({ type: "formSet", id: +id, value: v });
    if (!ops.length)
      return toast("Nothing to apply yet — pick a tool and doodle!");
    const signed = Object.values(annots)
      .flat()
      .filter(
        (a): a is Extract<Annot, { kind: "image" }> => a.kind === "image",
      );
    await withBusy("Stirring your changes in…", async (ctx) => {
      await applyEdits(ops, undefined, ctx);
      if (getState().recording) {
        for (const [p, arr] of Object.entries(annots))
          for (const a of arr)
            if (a.kind === "image" && a.sigId) {
              const pi = src.info.pageInfo[+p];
              const pages = +p === src.info.pages - 1 ? "last" : String(+p + 1);
              record({
                op: "sign",
                signatureId: a.sigId,
                pages,
                x: a.x / pi.vw,
                y: a.y / pi.vh,
                w: a.w / pi.vw,
              });
            }
      }
      setAnnots({});
      setValues({});
      setSelId(null);
      setSuffix(
        signed.length
          ? "signed"
          : Object.keys(values).length
            ? "filled"
            : "edited",
      );
      toast(
        signed.length
          ? "Signed, sealed, delivered 🖋️"
          : "Changes baked in! Download whenever you like.",
        "success",
      );
    });
  };

  const sel = list.find((a) => a.id === selId);
  const pageFields = fields.flatMap((f) =>
    f.widgets.filter((w) => w.page === page).map((w) => ({ f, w })),
  );

  return (
    <div class="editor">
      <nav class="pages" aria-label="Pages">
        {Array.from({ length: src.info.pages }, (_, i) => (
          <button
            key={i}
            aria-current={i === page}
            onClick={() => setState({ editPage: i })}
            aria-label={`Page ${i + 1}${(annots[i] ?? []).length ? ", has unapplied edits" : ""}`}
          >
            <MiniPage srcKey={src.key} index={i} />
            <span class="hint">
              {i + 1}
              {(annots[i] ?? []).length ? " •" : ""}
            </span>
          </button>
        ))}
      </nav>
      <section class="stage" ref={stageRef} aria-label="Page editor">
        <div class="stage-toolbar" role="toolbar" aria-label="Drawing tools">
          {TOOLS.map(([t, label, icon]) => (
            <button
              key={t}
              class="btn small"
              aria-pressed={tool === t && !pendingSig}
              onClick={() => {
                setTool(t);
                setPendingSig(null);
              }}
              title={label}
            >
              <Icon name={icon} /> {label}
            </button>
          ))}
          <button
            class="btn small primary"
            onClick={() => setSigOpen(true)}
            aria-pressed={!!pendingSig}
            data-testid="sign-btn"
          >
            <Icon name="sign" /> Sign
          </button>
          <span class="sep" />
          <button
            class="btn small"
            onClick={() => setState({ editPage: Math.max(0, page - 1) })}
            disabled={page === 0}
            aria-label="Previous page"
          >
            <Icon name="chevl" />
          </button>
          <span class="hint">
            {page + 1} / {src.info.pages}
          </span>
          <button
            class="btn small"
            onClick={() =>
              setState({ editPage: Math.min(src.info.pages - 1, page + 1) })
            }
            disabled={page >= src.info.pages - 1}
            aria-label="Next page"
          >
            <Icon name="chevr" />
          </button>
          <select
            class="btn small"
            value={zoom}
            onChange={(e) => setZoom(+(e.target as HTMLSelectElement).value)}
            aria-label="Zoom"
          >
            {[0.5, 0.75, 1, 1.25, 1.5, 2].map((z) => (
              <option key={z} value={z}>
                {Math.round(z * 100)}%
              </option>
            ))}
          </select>
        </div>
        {pendingSig && (
          <p class="hint" role="status">
            Click on the page where your signature should go.
          </p>
        )}
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
            style: {
              cursor:
                tool === "select" && !pendingSig ? "default" : "crosshair",
            },
            "data-testid": "page-overlay",
          }}
        >
          {pageFields.map(({ f, w }) => {
            const r = userToVisual(info, w.rect);
            return (
              <rect
                key={`${f.id}-${w.id}`}
                x={r.x}
                y={r.y}
                width={r.w}
                height={r.h}
                fill="rgba(37,99,166,0.08)"
                stroke="#2563a6"
                stroke-dasharray="3 2"
                stroke-width={0.8}
                onClick={() =>
                  document.getElementById(`field-${f.id}`)?.focus()
                }
              >
                <title>{f.name}</title>
              </rect>
            );
          })}
          {[...list, ...(draft ? [draft] : [])].map((a) => (
            <AnnotShape
              key={a.id}
              a={a}
              selected={a.id === selId}
              editing={editingText === a.id}
            />
          ))}
        </PageView>
        {editingText &&
          (() => {
            const a = list.find((x) => x.id === editingText) as
              | Extract<Annot, { kind: "text" }>
              | undefined;
            if (!a) return null;
            const s = stageW / info.vw;
            const svg = svgRef.current?.getBoundingClientRect();
            const stage = stageRef.current?.getBoundingClientRect();
            if (!svg || !stage) return null;
            return (
              <textarea
                class="text-edit"
                ref={(el) => {
                  if (el && document.activeElement !== el)
                    setTimeout(() => el.focus(), 0);
                }}
                aria-label="Text to add"
                style={{
                  position: "absolute",
                  left: `${svg.left - stage.left + stageRef.current!.scrollLeft + a.x * s}px`,
                  top: `${svg.top - stage.top + stageRef.current!.scrollTop + a.y * s}px`,
                  fontSize: `${a.size * s}px`,
                  color: a.color,
                }}
                value={a.text}
                onInput={(e) =>
                  update(a.id, {
                    text: (e.target as HTMLTextAreaElement).value,
                  })
                }
                onBlur={() => {
                  setEditingText(null);
                  if (!a.text.trim()) removeAnnot(a.id);
                  setTool("select");
                }}
                onKeyDown={(e) => {
                  if (
                    e.key === "Escape" ||
                    (e.key === "Enter" && (e.metaKey || e.ctrlKey))
                  )
                    (e.target as HTMLTextAreaElement).blur();
                }}
              />
            );
          })()}
      </section>
      <aside class="side" aria-label="Properties">
        <div class="panel">
          <h2>Style</h2>
          <Swatches
            value={color}
            onChange={(c) => {
              setColor(c);
              if (sel && "color" in sel) update(sel.id, { color: c });
            }}
          />
          <Field label={`Line width: ${width}pt`}>
            <input
              type="range"
              min={0.5}
              max={12}
              step={0.5}
              value={width}
              onInput={(e) => {
                const v = +(e.target as HTMLInputElement).value;
                setWidth(v);
                if (sel && "width" in sel) update(sel.id, { width: v });
              }}
            />
          </Field>
          <Field label={`Text size: ${fontSize}pt`}>
            <input
              type="range"
              min={6}
              max={72}
              value={fontSize}
              onInput={(e) => {
                const v = +(e.target as HTMLInputElement).value;
                setFontSize(v);
                if (sel?.kind === "text") update(sel.id, { size: v });
              }}
            />
          </Field>
          {sel && (
            <div class="row">
              {sel.kind === "text" && (
                <button
                  class="btn small"
                  onClick={() => setEditingText(sel.id)}
                >
                  Edit text
                </button>
              )}
              <button
                class="btn small danger"
                onClick={() => {
                  removeAnnot(sel.id);
                  setSelId(null);
                }}
              >
                <Icon name="trash" /> Remove
              </button>
            </div>
          )}
        </div>
        {fields.length > 0 && (
          <div class="panel">
            <h2>
              <Icon name="form" /> Form fields
            </h2>
            <FormFields
              fields={fields}
              values={values}
              setValues={setValues}
              onFocusField={(f) =>
                f.widgets[0] &&
                setState({ editPage: Math.max(0, f.widgets[0].page) })
              }
            />
          </div>
        )}
        <div class="panel">
          <button
            class="btn primary"
            style={{ width: "100%", justifyContent: "center" }}
            onClick={apply}
            disabled={!dirty}
            data-testid="apply-edits"
          >
            <Icon name="check" /> Apply changes
          </button>
          <button
            class="btn ghost"
            style={{ width: "100%", justifyContent: "center", marginTop: 6 }}
            onClick={() => {
              setAnnots({});
              setValues({});
            }}
            disabled={!dirty}
          >
            Discard
          </button>
          <p class="hint">
            Edits are flattened into the page so they look the same in every PDF
            reader.
          </p>
        </div>
      </aside>
      {sigOpen && (
        <SignatureDialog
          onClose={() => setSigOpen(false)}
          onDone={(r) => {
            setSigOpen(false);
            setPendingSig(r);
          }}
        />
      )}
    </div>
  );
}

function translate(a: Annot, dx: number, dy: number): Partial<Annot> {
  switch (a.kind) {
    case "line":
    case "arrow":
      return { x1: a.x1 + dx, y1: a.y1 + dy, x2: a.x2 + dx, y2: a.y2 + dy };
    case "ink":
      return {
        points: a.points.map(([x, y]) => [x + dx, y + dy] as [number, number]),
      };
    default:
      return { x: a.x + dx, y: a.y + dy };
  }
}

export function AnnotShape({
  a,
  selected,
  editing,
}: {
  a: Annot;
  selected: boolean;
  editing?: boolean;
}) {
  const common = { "data-annot": a.id, style: { cursor: "move" } } as Record<
    string,
    unknown
  >;
  const sel = selected ? { filter: "drop-shadow(0 0 2px #1d5fd1)" } : {};
  let el = null;
  switch (a.kind) {
    case "text":
      el = editing ? null : (
        <text
          {...common}
          x={a.x}
          y={a.y + a.size * 0.8}
          font-size={a.size}
          fill={a.color}
          font-family="Helvetica, Arial, sans-serif"
          style={{ ...sel, cursor: "move", whiteSpace: "pre" }}
        >
          {a.text.split("\n").map((l, i) => (
            <tspan key={i} x={a.x} dy={i ? a.size * 1.2 : 0}>
              {l || " "}
            </tspan>
          ))}
        </text>
      );
      break;
    case "rect":
      el = (
        <rect
          {...common}
          x={a.x}
          y={a.y}
          width={a.w}
          height={a.h}
          fill={a.fill ?? "transparent"}
          stroke={a.color}
          stroke-width={a.width}
          style={sel}
        />
      );
      break;
    case "ellipse":
      el = (
        <ellipse
          {...common}
          cx={a.x + a.w / 2}
          cy={a.y + a.h / 2}
          rx={a.w / 2}
          ry={a.h / 2}
          fill={a.fill ?? "transparent"}
          stroke={a.color}
          stroke-width={a.width}
          style={sel}
        />
      );
      break;
    case "highlight":
      el = (
        <rect
          {...common}
          x={a.x}
          y={a.y}
          width={a.w}
          height={a.h}
          fill={a.color}
          opacity={0.4}
          style={{ ...sel, mixBlendMode: "multiply" }}
        />
      );
      break;
    case "line":
    case "arrow": {
      const ang = Math.atan2(a.y2 - a.y1, a.x2 - a.x1);
      const L = Math.max(8, a.width * 4);
      el = (
        <g {...common} style={sel}>
          <line
            x1={a.x1}
            y1={a.y1}
            x2={a.x2}
            y2={a.y2}
            stroke={a.color}
            stroke-width={a.width}
            stroke-linecap="round"
          />
          <line
            x1={a.x1}
            y1={a.y1}
            x2={a.x2}
            y2={a.y2}
            stroke="transparent"
            stroke-width={Math.max(10, a.width * 3)}
          />
          {a.kind === "arrow" && (
            <polygon
              points={`${a.x2},${a.y2} ${a.x2 - L * Math.cos(ang - 0.45)},${a.y2 - L * Math.sin(ang - 0.45)} ${a.x2 - L * Math.cos(ang + 0.45)},${a.y2 - L * Math.sin(ang + 0.45)}`}
              fill={a.color}
            />
          )}
        </g>
      );
      break;
    }
    case "ink":
      el = (
        <polyline
          {...common}
          points={a.points.map((p) => p.join(",")).join(" ")}
          fill="none"
          stroke={a.color}
          stroke-width={a.width}
          stroke-linecap="round"
          stroke-linejoin="round"
          style={sel}
        />
      );
      break;
    case "image":
      el = (
        <g>
          <image
            {...common}
            href={a.url}
            x={a.x}
            y={a.y}
            width={a.w}
            height={a.h}
            preserveAspectRatio="none"
            style={sel}
          />
          {selected && (
            <rect
              x={a.x}
              y={a.y}
              width={a.w}
              height={a.h}
              fill="none"
              stroke="#1d5fd1"
              stroke-dasharray="4 3"
              stroke-width={0.8}
              pointer-events="none"
            />
          )}
        </g>
      );
      break;
    case "redact":
      el = (
        <rect
          {...common}
          x={a.x}
          y={a.y}
          width={a.w}
          height={a.h}
          fill="rgba(0,0,0,0.75)"
          stroke="#e4572e"
          stroke-width={1}
          style={sel}
        />
      );
      break;
  }
  const handle =
    selected && "w" in a && a.kind !== "highlight" ? (
      <rect
        data-annot={a.id}
        data-handle="1"
        x={a.x + a.w - 4}
        y={a.y + a.h - 4}
        width={8}
        height={8}
        fill="#1d5fd1"
        style={{ cursor: "nwse-resize" }}
      />
    ) : null;
  return (
    <>
      {el}
      {handle}
    </>
  );
}

function FormFields({
  fields,
  values,
  setValues,
  onFocusField,
}: {
  fields: FormField[];
  values: Record<number, string>;
  setValues: (v: Record<number, string>) => void;
  onFocusField: (f: FormField) => void;
}) {
  const set = (id: number, v: string) => setValues({ ...values, [id]: v });
  return (
    <div>
      {fields
        .filter((f) => f.type !== "button" && f.type !== "signature")
        .map((f) => {
          const v = values[f.id] ?? f.value;
          const id = `field-${f.id}`;
          const label = f.name.replace(/[._]/g, " ");
          const common = {
            id,
            disabled: f.readonly,
            onFocus: () => onFocusField(f),
          };
          if (f.type === "checkbox") {
            const on = f.widgets[0]?.on ?? "Yes";
            return (
              <label
                key={f.id}
                style={{
                  display: "flex",
                  gap: 8,
                  alignItems: "center",
                  margin: "8px 0",
                }}
              >
                <input
                  type="checkbox"
                  {...common}
                  checked={v !== "" && v !== "Off"}
                  onChange={(e) =>
                    set(
                      f.id,
                      (e.target as HTMLInputElement).checked ? on : "Off",
                    )
                  }
                />
                {label}
              </label>
            );
          }
          if (f.type === "radio") {
            return (
              <fieldset
                key={f.id}
                style={{ border: 0, padding: 0, margin: "8px 0" }}
              >
                <legend class="lbl" style={{ fontWeight: 600, fontSize: 13 }}>
                  {label}
                </legend>
                {f.widgets.map((w, i) => (
                  <label
                    key={w.id}
                    style={{ display: "inline-flex", gap: 6, marginRight: 12 }}
                  >
                    <input
                      type="radio"
                      name={id}
                      id={i === 0 ? id : undefined}
                      checked={v === w.on}
                      onChange={() => set(f.id, w.on)}
                      onFocus={() => onFocusField(f)}
                      disabled={f.readonly}
                    />
                    {w.on}
                  </label>
                ))}
              </fieldset>
            );
          }
          if (f.type === "combo" || f.type === "list") {
            return (
              <Field key={f.id} label={label}>
                <select
                  {...common}
                  value={v}
                  onChange={(e) =>
                    set(f.id, (e.target as HTMLSelectElement).value)
                  }
                >
                  {!f.options.includes(v) && (
                    <option value={v}>{v || "—"}</option>
                  )}
                  {f.options.map((o) => (
                    <option key={o} value={o}>
                      {o}
                    </option>
                  ))}
                </select>
              </Field>
            );
          }
          return (
            <Field key={f.id} label={label}>
              {f.multiline ? (
                <textarea
                  {...common}
                  rows={3}
                  value={v}
                  maxLength={f.maxlen || undefined}
                  onInput={(e) =>
                    set(f.id, (e.target as HTMLTextAreaElement).value)
                  }
                />
              ) : (
                <input
                  type="text"
                  {...common}
                  value={v}
                  maxLength={f.maxlen || undefined}
                  onInput={(e) =>
                    set(f.id, (e.target as HTMLInputElement).value)
                  }
                />
              )}
            </Field>
          );
        })}
    </div>
  );
}

export type { Source };
