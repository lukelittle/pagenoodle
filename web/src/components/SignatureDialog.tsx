// Create a signature by drawing, typing or uploading; optionally save it locally (SQLite) for reuse.
import { useEffect, useRef, useState } from "preact/hooks";
import { Dialog, Icon } from "./ui";
import { db, type SignatureRow } from "../store/db";
import {
  canvasPng,
  knockOutWhite,
  pngToCanvas,
  trimCanvas,
} from "../lib/images";
import { toast } from "../store/app";

export interface SignatureResult {
  canvas: HTMLCanvasElement;
  url: string;
  savedId?: number;
}

type Tab = "draw" | "type" | "upload" | "saved";

export function SignatureDialog({
  onClose,
  onDone,
}: {
  onClose: () => void;
  onDone: (r: SignatureResult) => void;
}) {
  const [tab, setTab] = useState<Tab>("draw");
  const [saved, setSaved] = useState<(SignatureRow & { url: string })[]>([]);
  const [remember, setRemember] = useState(false);
  const [ink, setInk] = useState("#1a237e");
  const [typed, setTyped] = useState("");
  const [font, setFont] = useState<"Caveat" | "Helvetica">("Caveat");
  const [upload, setUpload] = useState<HTMLCanvasElement | null>(null);
  const [knock, setKnock] = useState(true);
  const pad = useRef<HTMLCanvasElement>(null);
  const drawn = useRef(false);

  const loadSaved = async () => {
    try {
      const rows = await db.listSignatures();
      const withUrls = await Promise.all(
        rows.map(async (r) => ({
          ...r,
          url: (await pngToCanvas(r.png)).toDataURL(),
        })),
      );
      setSaved(withUrls);
      if (withUrls.length)
        setTab((t) => (t === "draw" && !drawn.current ? "saved" : t));
    } catch {
      setSaved([]);
    }
  };
  useEffect(() => void loadSaved(), []);

  // drawing pad
  useEffect(() => {
    const c = pad.current;
    if (!c || tab !== "draw") return;
    const dpr = Math.max(2, window.devicePixelRatio || 1);
    const r = c.getBoundingClientRect();
    if (!drawn.current) {
      c.width = r.width * dpr;
      c.height = r.height * dpr;
    }
    const ctx = c.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    let last: [number, number] | null = null;
    let lastMid: [number, number] | null = null;
    const pt = (e: PointerEvent): [number, number] => {
      const b = c.getBoundingClientRect();
      return [e.clientX - b.left, e.clientY - b.top];
    };
    const down = (e: PointerEvent) => {
      c.setPointerCapture(e.pointerId);
      last = pt(e);
      lastMid = last;
      drawn.current = true;
    };
    const move = (e: PointerEvent) => {
      if (!last) return;
      const p = pt(e);
      const mid: [number, number] = [
        (last[0] + p[0]) / 2,
        (last[1] + p[1]) / 2,
      ];
      ctx.strokeStyle = ink;
      ctx.lineWidth =
        2.6 * (e.pressure && e.pointerType === "pen" ? 0.6 + e.pressure : 1);
      ctx.beginPath();
      ctx.moveTo(lastMid![0], lastMid![1]);
      ctx.quadraticCurveTo(last[0], last[1], mid[0], mid[1]);
      ctx.stroke();
      last = p;
      lastMid = mid;
    };
    const up = () => {
      last = null;
    };
    c.addEventListener("pointerdown", down);
    c.addEventListener("pointermove", move);
    c.addEventListener("pointerup", up);
    c.addEventListener("pointercancel", up);
    return () => {
      c.removeEventListener("pointerdown", down);
      c.removeEventListener("pointermove", move);
      c.removeEventListener("pointerup", up);
      c.removeEventListener("pointercancel", up);
    };
  }, [tab, ink]);

  const clearPad = () => {
    const c = pad.current;
    if (c) c.getContext("2d")!.clearRect(0, 0, c.width, c.height);
    drawn.current = false;
  };

  const typedCanvas = async () => {
    if (!typed.trim()) return null;
    const fam = font === "Caveat" ? "Caveat" : "Helvetica, Arial, sans-serif";
    if (font === "Caveat")
      await document.fonts.load("96px Caveat").catch(() => {});
    const c = document.createElement("canvas");
    const ctx = c.getContext("2d")!;
    ctx.font = `96px ${fam}`;
    const w = Math.ceil(ctx.measureText(typed).width) + 40;
    c.width = w;
    c.height = 150;
    const ctx2 = c.getContext("2d")!;
    ctx2.font = `96px ${fam}`;
    ctx2.fillStyle = ink;
    ctx2.textBaseline = "middle";
    ctx2.fillText(typed, 20, 78);
    return c;
  };

  const onUpload = async (f: File) => {
    try {
      const bmp = await createImageBitmap(f);
      const c = document.createElement("canvas");
      const s = Math.min(1, 1200 / bmp.width);
      c.width = Math.round(bmp.width * s);
      c.height = Math.round(bmp.height * s);
      c.getContext("2d")!.drawImage(bmp, 0, 0, c.width, c.height);
      bmp.close();
      setUpload(c);
    } catch {
      toast("I couldn't read that image.", "error");
    }
  };

  const use = async (fromSaved?: SignatureRow & { url: string }) => {
    let c: HTMLCanvasElement | null = null;
    let savedId: number | undefined;
    if (fromSaved) {
      c = await pngToCanvas(fromSaved.png);
      savedId = fromSaved.id;
    } else if (tab === "draw")
      c = pad.current && drawn.current ? trimCanvas(pad.current) : null;
    else if (tab === "type")
      c = await typedCanvas().then((x) => (x ? trimCanvas(x) : null));
    else if (tab === "upload" && upload) {
      const copy = document.createElement("canvas");
      copy.width = upload.width;
      copy.height = upload.height;
      copy.getContext("2d")!.drawImage(upload, 0, 0);
      if (knock) knockOutWhite(copy);
      c = trimCanvas(copy) ?? copy;
    }
    if (!c) return toast("Draw, type or upload a signature first ✍️");
    if (remember && !fromSaved) {
      try {
        const png = await canvasPng(c);
        savedId = await db.saveSignature({
          name: tab === "type" ? typed : "Signature",
          png,
          w: c.width,
          h: c.height,
        });
        toast("Signature saved on this device only 🔒", "success");
      } catch (e) {
        toast(
          `Couldn't save locally: ${e instanceof Error ? e.message : e}`,
          "error",
        );
      }
    }
    onDone({ canvas: c, url: c.toDataURL(), savedId });
  };

  const tabs: [Tab, string][] = [
    ["draw", "Draw"],
    ["type", "Type"],
    ["upload", "Upload"],
    ["saved", `Saved (${saved.length})`],
  ];
  return (
    <Dialog
      title="Your signature"
      onClose={onClose}
      wide
      actions={
        tab !== "saved" ? (
          <>
            <label
              style={{
                marginRight: "auto",
                display: "flex",
                gap: 6,
                alignItems: "center",
              }}
            >
              <input
                type="checkbox"
                checked={remember}
                onChange={(e) =>
                  setRemember((e.target as HTMLInputElement).checked)
                }
              />
              Remember on this device (stored locally, never uploaded)
            </label>
            <button class="btn" onClick={onClose}>
              Cancel
            </button>
            <button
              class="btn primary"
              onClick={() => use()}
              data-testid="use-signature"
            >
              <Icon name="check" /> Use signature
            </button>
          </>
        ) : (
          <button class="btn" onClick={onClose}>
            Close
          </button>
        )
      }
    >
      <div
        class="tabs"
        role="tablist"
        style={{ borderRadius: 12, marginBottom: 12 }}
      >
        {tabs.map(([t, l]) => (
          <button
            key={t}
            class="tab"
            role="tab"
            aria-selected={tab === t}
            onClick={() => setTab(t)}
          >
            {l}
          </button>
        ))}
      </div>
      {tab !== "saved" && tab !== "upload" && (
        <div
          class="row"
          style={{
            display: "flex",
            gap: 8,
            alignItems: "center",
            marginBottom: 8,
          }}
        >
          <span class="hint">Ink:</span>
          {["#111111", "#1a237e", "#0b6623"].map((c) => (
            <button
              key={c}
              class="swatch"
              style={{ background: c }}
              aria-pressed={ink === c}
              aria-label={`Ink ${c}`}
              onClick={() => setInk(c)}
            />
          ))}
        </div>
      )}
      {tab === "draw" && (
        <>
          <canvas
            ref={pad}
            class="sig-pad"
            aria-label="Signature drawing pad. Draw with mouse, finger or pen."
            data-testid="sig-pad"
          />
          <div
            style={{
              display: "flex",
              justifyContent: "space-between",
              marginTop: 6,
            }}
          >
            <span class="hint">
              Sign above ✍️ — keyboard users can use the Type tab.
            </span>
            <button class="btn small" onClick={clearPad}>
              Clear
            </button>
          </div>
        </>
      )}
      {tab === "type" && (
        <>
          <input
            class="text"
            type="text"
            placeholder="Type your name"
            value={typed}
            onInput={(e) => setTyped((e.target as HTMLInputElement).value)}
            aria-label="Your name"
            autoFocus
          />
          <div class="radio-row" style={{ marginTop: 10 }}>
            <label>
              <input
                type="radio"
                checked={font === "Caveat"}
                onChange={() => setFont("Caveat")}
              />
              <span style={{ fontFamily: "Caveat", fontSize: 22 }}>
                Handwritten
              </span>
            </label>
            <label>
              <input
                type="radio"
                checked={font === "Helvetica"}
                onChange={() => setFont("Helvetica")}
              />
              <span>Plain</span>
            </label>
          </div>
          <div
            style={{
              fontFamily: font === "Caveat" ? "Caveat" : "Helvetica, Arial",
              fontSize: 44,
              color: ink,
              minHeight: 64,
              marginTop: 8,
              background: "#fff",
              borderRadius: 10,
              padding: "4px 12px",
            }}
            aria-hidden="true"
          >
            {typed}
          </div>
        </>
      )}
      {tab === "upload" && (
        <>
          <input
            type="file"
            accept="image/*"
            onChange={(e) =>
              (e.target as HTMLInputElement).files?.[0] &&
              onUpload((e.target as HTMLInputElement).files![0])
            }
            aria-label="Signature image"
          />
          <label
            style={{
              display: "flex",
              gap: 6,
              alignItems: "center",
              marginTop: 8,
            }}
          >
            <input
              type="checkbox"
              checked={knock}
              onChange={(e) => setKnock((e.target as HTMLInputElement).checked)}
            />{" "}
            Remove white background
          </label>
          {upload && (
            <img
              src={upload.toDataURL()}
              alt="Uploaded signature preview"
              style={{
                maxWidth: "100%",
                maxHeight: 160,
                marginTop: 8,
                background: "#fff",
              }}
            />
          )}
        </>
      )}
      {tab === "saved" &&
        (saved.length ? (
          <div class="sig-list">
            {saved.map((s) => (
              <div key={s.id} style={{ position: "relative" }}>
                <button
                  class="sig-item"
                  style={{ width: "100%" }}
                  onClick={() => use(s)}
                  aria-label={`Use saved signature ${s.name}`}
                >
                  <img src={s.url} alt="" />
                </button>
                <button
                  class="btn small ghost del"
                  style={{ position: "absolute", top: 2, right: 2 }}
                  aria-label={`Delete saved signature ${s.name}`}
                  onClick={async () => {
                    await db.deleteSignature(s.id);
                    void loadSaved();
                  }}
                >
                  <Icon name="trash" />
                </button>
              </div>
            ))}
          </div>
        ) : (
          <p class="hint">
            No saved signatures yet. Tick “Remember on this device” when you
            make one.
          </p>
        ))}
    </Dialog>
  );
}
