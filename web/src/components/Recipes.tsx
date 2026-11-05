// Recipes: record a sequence of steps, save locally (SQLite), replay in one click, export/import JSON.
import { useEffect, useState } from "preact/hooks";
import { useApp, setState, withBusy, toast, getState } from "../store/app";
import { db, type RecipeRow } from "../store/db";
import { describeStep, runStep, type Step } from "../pipeline/steps";
import { Icon, Dialog, Field } from "./ui";
import { downloadBlob, safeName } from "../lib/util";
import { downloadParts } from "./Organize";
import { downloadCurrent } from "./actions";

const KNOWN: Step["op"][] = [
  "merge",
  "rotate",
  "delete",
  "keep",
  "reverse",
  "split",
  "compress",
  "stripMetadata",
  "stamp",
  "redactText",
  "sign",
  "flattenForms",
  "ocr",
];

export function validateRecipe(obj: unknown): { name: string; steps: Step[] } {
  const o = obj as { pagenoodle?: string; name?: unknown; steps?: unknown };
  if (!o || typeof o !== "object" || o.pagenoodle !== "recipe")
    throw new Error("That file isn't a Pagenoodle recipe.");
  if (!Array.isArray(o.steps) || !o.steps.length)
    throw new Error("This recipe has no steps.");
  for (const s of o.steps)
    if (!s || typeof s !== "object" || !KNOWN.includes((s as Step).op))
      throw new Error(`Unknown step: ${JSON.stringify(s).slice(0, 60)}`);
  return {
    name:
      typeof o.name === "string" && o.name.trim()
        ? o.name.trim().slice(0, 80)
        : "Imported recipe",
    steps: o.steps as Step[],
  };
}

export function recipeJson(name: string, steps: Step[]) {
  return JSON.stringify(
    { pagenoodle: "recipe", version: 1, name, steps },
    null,
    2,
  );
}

export async function runRecipe(name: string, steps: Step[]) {
  if (!getState().doc.pages.length)
    return toast("Open the files to cook first, then run the recipe.");
  let parts: Awaited<ReturnType<typeof runStep>>["parts"];
  const ok = await withBusy(`Cooking “${name}”…`, async (ctx) => {
    for (let i = 0; i < steps.length; i++) {
      setState((s) => ({
        busy: s.busy
          ? {
              ...s.busy,
              label: `Step ${i + 1}/${steps.length}: ${describeStep(steps[i])}`,
            }
          : null,
      }));
      const out = await runStep(steps[i], {
        signal: ctx.signal,
        onProgress: ctx.onProgress,
      });
      if (out.parts) parts = out.parts;
    }
    if (parts) await downloadParts(parts, ctx);
    return true;
  });
  if (ok && !parts) await downloadCurrent();
}

const TEMPLATES: { label: string; step: Step }[] = [
  {
    label: "Rotate all pages 90°",
    step: { op: "rotate", pages: "all", angle: 90 },
  },
  { label: "Delete first page", step: { op: "delete", pages: "1" } },
  { label: "Reverse page order", step: { op: "reverse" } },
  {
    label: "Add page numbers",
    step: {
      op: "stamp",
      template: "Page {n} of {total}",
      position: 4,
      size: 10,
      margin: 24,
      start: 1,
      pages: "all",
      font: "Helvetica",
      color: 0x333333,
    },
  },
  {
    label: "Stamp “CONFIDENTIAL” header",
    step: {
      op: "stamp",
      template: "CONFIDENTIAL",
      position: 1,
      size: 12,
      margin: 20,
      start: 1,
      pages: "all",
      font: "Helvetica-Bold",
      color: 0xc8381f,
    },
  },
  { label: "Compress (balanced)", step: { op: "compress", level: "balanced" } },
  { label: "Strip metadata", step: { op: "stripMetadata" } },
  { label: "OCR scanned pages", step: { op: "ocr", skipText: true } },
  { label: "Flatten forms", step: { op: "flattenForms" } },
  { label: "Split every page", step: { op: "split", every: 1 } },
];

export function Recipes() {
  const st = useApp();
  const [rows, setRows] = useState<RecipeRow[]>([]);
  const [mode, setMode] = useState("");
  const [naming, setNaming] = useState<Step[] | null>(null);
  const [name, setName] = useState("My recipe");
  const [editing, setEditing] = useState<RecipeRow | null>(null);
  const load = () =>
    db
      .listRecipes()
      .then(setRows)
      .catch((e) => setMode(`unavailable: ${e.message}`));
  useEffect(() => {
    db.mode().then(setMode);
    void load();
  }, []);

  const save = async (steps: Step[], nm: string, id?: number) => {
    await db.saveRecipe({ id, name: nm, steps: JSON.stringify(steps) });
    toast(`Recipe “${nm}” saved on this device 📒`, "success");
    void load();
  };
  const importFile = async (f: File) => {
    try {
      const r = validateRecipe(JSON.parse(await f.text()));
      await save(r.steps, r.name);
    } catch (e) {
      toast(e instanceof Error ? e.message : String(e), "error");
    }
  };

  return (
    <div class="view">
      <div
        class="cards"
        style={{ gridTemplateColumns: "repeat(auto-fill, minmax(340px, 1fr))" }}
      >
        <section class="card" aria-labelledby="rec-h">
          <h2 id="rec-h">
            <Icon name="record" /> Record a recipe
          </h2>
          <p>
            Hit record, then do your usual routine (merge, compress, number,
            sign…). Pagenoodle writes down each step so you can replay it on new
            files in one click.
          </p>
          {st.recording ? (
            <>
              <ol aria-live="polite" data-testid="recording-steps">
                {st.recording.map((s, i) => (
                  <li key={i}>{describeStep(s)}</li>
                ))}
                {!st.recording.length && (
                  <li class="hint">Waiting for your first step…</li>
                )}
              </ol>
              <div class="actions">
                <button
                  class="btn primary"
                  onClick={() => {
                    const steps = st.recording ?? [];
                    setState({ recording: null });
                    if (steps.length) setNaming(steps);
                    else toast("Nothing recorded.");
                  }}
                >
                  <Icon name="stop" /> Stop &amp; save
                </button>
                <button
                  class="btn"
                  onClick={() => setState({ recording: null })}
                >
                  Discard
                </button>
              </div>
            </>
          ) : (
            <div class="actions">
              <button
                class="btn primary"
                onClick={() => setState({ recording: [] })}
                data-testid="start-recording"
              >
                <Icon name="record" /> Start recording
              </button>
              <button class="btn" onClick={() => setNaming([])}>
                <Icon name="plus" /> Build by hand
              </button>
              <label class="btn">
                Import .json
                <input
                  type="file"
                  accept="application/json,.json"
                  class="sr-only"
                  onChange={(e) => {
                    const f = (e.target as HTMLInputElement).files?.[0];
                    (e.target as HTMLInputElement).value = "";
                    if (f) void importFile(f);
                  }}
                />
              </label>
            </div>
          )}
          <p class="hint">
            Stored in{" "}
            {mode === "opfs"
              ? "SQLite on your device (OPFS)"
              : mode === "indexeddb"
                ? "SQLite, saved to IndexedDB on your device"
                : mode || "…"}{" "}
            — never uploaded.
          </p>
        </section>
        {rows.map((r) => {
          let steps: Step[] = [];
          try {
            steps = JSON.parse(r.steps);
          } catch {
            /* ignore */
          }
          return (
            <section
              class="card"
              key={r.id}
              aria-label={`Recipe ${r.name}`}
              data-testid="recipe-card"
            >
              <h2>📒 {r.name}</h2>
              <ol style={{ margin: "4px 0", paddingLeft: 20 }}>
                {steps.map((s, i) => (
                  <li key={i}>{describeStep(s)}</li>
                ))}
              </ol>
              <div class="actions">
                <button
                  class="btn primary"
                  onClick={() => runRecipe(r.name, steps)}
                  disabled={!st.doc.pages.length}
                  data-testid="run-recipe"
                >
                  <Icon name="play" /> Run on open files
                </button>
                <button
                  class="btn"
                  onClick={() =>
                    downloadBlob(
                      new TextEncoder().encode(recipeJson(r.name, steps)),
                      `${safeName(r.name)}.pagenoodle-recipe.json`,
                      "application/json",
                    )
                  }
                >
                  <Icon name="download" /> Export
                </button>
                <button class="btn" onClick={() => setEditing(r)}>
                  Edit
                </button>
                <button
                  class="btn danger"
                  aria-label={`Delete recipe ${r.name}`}
                  onClick={async () => {
                    if (!confirm(`Delete recipe “${r.name}”?`)) return;
                    await db.deleteRecipe(r.id);
                    void load();
                  }}
                >
                  <Icon name="trash" />
                </button>
              </div>
            </section>
          );
        })}
      </div>
      {naming && (
        <RecipeEditor
          initialName={name}
          initialSteps={naming}
          onClose={() => setNaming(null)}
          onSave={async (nm, steps) => {
            setName(nm);
            setNaming(null);
            await save(steps, nm);
          }}
        />
      )}
      {editing && (
        <RecipeEditor
          initialName={editing.name}
          initialSteps={JSON.parse(editing.steps)}
          onClose={() => setEditing(null)}
          onSave={async (nm, steps) => {
            const id = editing.id;
            setEditing(null);
            await save(steps, nm, id);
          }}
        />
      )}
    </div>
  );
}

function RecipeEditor({
  initialName,
  initialSteps,
  onClose,
  onSave,
}: {
  initialName: string;
  initialSteps: Step[];
  onClose: () => void;
  onSave: (name: string, steps: Step[]) => void;
}) {
  const [name, setName] = useState(initialName);
  const [steps, setSteps] = useState<Step[]>(initialSteps);
  const [json, setJson] = useState<string | null>(null);
  const move = (i: number, d: number) => {
    const s = [...steps];
    const [x] = s.splice(i, 1);
    s.splice(Math.max(0, Math.min(s.length, i + d)), 0, x);
    setSteps(s);
  };
  return (
    <Dialog
      title="Save recipe"
      onClose={onClose}
      wide
      actions={
        <>
          <button class="btn" onClick={onClose}>
            Cancel
          </button>
          <button
            class="btn primary"
            data-testid="save-recipe"
            onClick={() => {
              try {
                const finalSteps =
                  json !== null
                    ? validateRecipe({
                        pagenoodle: "recipe",
                        steps: JSON.parse(json),
                      }).steps
                    : steps;
                if (!finalSteps.length) return toast("Add at least one step.");
                onSave(name.trim() || "My recipe", finalSteps);
              } catch (e) {
                toast(e instanceof Error ? e.message : String(e), "error");
              }
            }}
          >
            <Icon name="check" /> Save recipe
          </button>
        </>
      }
    >
      <Field label="Name">
        <input
          type="text"
          value={name}
          onInput={(e) => setName((e.target as HTMLInputElement).value)}
          autoFocus
        />
      </Field>
      {json === null ? (
        <>
          <ol style={{ paddingLeft: 20 }}>
            {steps.map((s, i) => (
              <li
                key={i}
                style={{
                  display: "flex",
                  gap: 6,
                  alignItems: "center",
                  margin: "4px 0",
                }}
              >
                <span style={{ flex: 1 }}>{describeStep(s)}</span>
                <button
                  class="btn small"
                  aria-label="Move up"
                  onClick={() => move(i, -1)}
                  disabled={i === 0}
                >
                  ↑
                </button>
                <button
                  class="btn small"
                  aria-label="Move down"
                  onClick={() => move(i, 1)}
                  disabled={i === steps.length - 1}
                >
                  ↓
                </button>
                <button
                  class="btn small danger"
                  aria-label="Remove step"
                  onClick={() => setSteps(steps.filter((_, k) => k !== i))}
                >
                  <Icon name="x" />
                </button>
              </li>
            ))}
          </ol>
          <div
            class="row"
            style={{ display: "flex", gap: 6, flexWrap: "wrap" }}
          >
            <select
              class="btn"
              aria-label="Add a step"
              value=""
              onChange={(e) => {
                const t = TEMPLATES[+(e.target as HTMLSelectElement).value];
                if (t) setSteps([...steps, structuredClone(t.step)]);
              }}
            >
              <option value="">＋ Add a step…</option>
              {TEMPLATES.map((t, i) => (
                <option key={i} value={i}>
                  {t.label}
                </option>
              ))}
            </select>
            <button
              class="btn"
              onClick={() => setJson(JSON.stringify(steps, null, 2))}
            >
              Edit as JSON
            </button>
          </div>
        </>
      ) : (
        <Field label="Steps (JSON)">
          <textarea
            rows={14}
            style={{ fontFamily: "monospace", fontSize: 12 }}
            value={json}
            onInput={(e) => setJson((e.target as HTMLTextAreaElement).value)}
          />
        </Field>
      )}
    </Dialog>
  );
}
