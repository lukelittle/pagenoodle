// A single rendered page with an SVG overlay in page points (visual, top-left origin).
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { renderToCanvas, thumbnail } from "../render/pdf";
import type { PageInfo } from "../engine/types";

export function PageView({
  srcKey,
  index,
  info,
  width,
  children,
  overlayProps,
  label,
}: {
  srcKey: string;
  index: number;
  info: PageInfo;
  width: number;
  children?: ComponentChildren;
  overlayProps?: Record<string, unknown>;
  label: string;
}) {
  const host = useRef<HTMLDivElement>(null);
  const [error, setError] = useState(false);
  const height = (width * info.vh) / info.vw;
  useEffect(() => {
    const ac = new AbortController();
    setError(false);
    renderToCanvas(srcKey, index, {
      width,
      signal: ac.signal,
      priority: 10,
      annotations: true,
    })
      .then((c) => {
        const h = host.current;
        if (!h) return;
        h.querySelector("canvas")?.remove();
        c.setAttribute("aria-hidden", "true");
        h.prepend(c);
      })
      .catch((e) => {
        if (
          !(e instanceof DOMException) &&
          e?.name !== "RenderingCancelledException"
        )
          setError(true);
      });
    return () => ac.abort();
  }, [srcKey, index, width]);
  return (
    <div
      class="page-view"
      ref={host}
      style={{ width: `${width}px`, height: `${height}px` }}
      role="group"
      aria-label={label}
    >
      {error && (
        <p class="hint" style={{ padding: 20 }}>
          This page couldn't be drawn, but you can still edit it.
        </p>
      )}
      <svg
        class="overlay"
        viewBox={`0 0 ${info.vw} ${info.vh}`}
        {...overlayProps}
      >
        {children}
      </svg>
    </div>
  );
}

export function MiniPage({
  srcKey,
  index,
  width = 96,
}: {
  srcKey: string;
  index: number;
  width?: number;
}) {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const ac = new AbortController();
    thumbnail(srcKey, index, 0, width * 2, ac.signal)
      .then((b) => {
        const c = ref.current;
        if (!c) return;
        c.width = b.width;
        c.height = b.height;
        c.getContext("2d")!.drawImage(b, 0, 0);
      })
      .catch(() => {});
    return () => ac.abort();
  }, [srcKey, index, width]);
  return (
    <canvas
      ref={ref}
      aria-hidden="true"
      style={{ minHeight: 40, background: "var(--bg-2)" }}
    />
  );
}

/** Convert a pointer event to page points. */
export function pointOf(
  e: PointerEvent | MouseEvent,
  svg: SVGSVGElement,
  info: PageInfo,
): [number, number] {
  const r = svg.getBoundingClientRect();
  return [
    ((e.clientX - r.left) / r.width) * info.vw,
    ((e.clientY - r.top) / r.height) * info.vh,
  ];
}

/** Fit the page into the available stage width. */
export function useStageWidth(
  ref: { current: HTMLElement | null },
  info: PageInfo | undefined,
  zoom: number,
) {
  const [w, setW] = useState(600);
  useEffect(() => {
    const el = ref.current;
    if (!el || !info) return;
    const update = () => {
      const avail = Math.max(240, el.clientWidth - 40);
      const availH = Math.max(300, window.innerHeight - 220);
      const fit = Math.min(avail, ((availH * info.vw) / info.vh) * 1.6, 1100);
      setW(Math.round(fit * zoom));
    };
    update();
    const ro = new ResizeObserver(update);
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref.current, info?.vw, info?.vh, zoom]);
  return w;
}
