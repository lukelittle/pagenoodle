// App-level actions shared across views.
import {
  addFiles,
  exportBytes,
  getState,
  setState,
  toast,
  withBusy,
} from "../store/app";
import { downloadBlob, safeName } from "../lib/util";

export function celebrate() {
  setState((s) => ({ celebrate: s.celebrate + 1 }));
  setTimeout(() => setState({ celebrate: 0 }), 2600);
}

let input: HTMLInputElement | null = null;
export function openFilePicker() {
  if (!input) {
    input = document.createElement("input");
    input.type = "file";
    input.multiple = true;
    input.accept =
      "application/pdf,.pdf,image/png,image/jpeg,image/webp,image/gif,image/bmp";
    input.style.display = "none";
    input.setAttribute("data-testid", "file-input");
    input.onchange = () => {
      const files = [...(input!.files ?? [])];
      input!.value = "";
      if (files.length) void addFiles(files);
    };
    document.body.appendChild(input);
  }
  input.click();
}

export function setSuffix(s: string) {
  setState({ suffix: s });
}

export async function downloadCurrent() {
  const st = getState();
  if (!st.doc.pages.length)
    return toast("Nothing to download yet — drop a PDF in first!");
  await withBusy("Packing your PDF…", async (ctx) => {
    const bytes = await exportBytes(ctx);
    const multi = Object.keys(st.doc.sources).length > 1;
    const suffix = st.suffix || (multi ? "merged" : "noodled");
    downloadBlob(bytes, `${safeName(st.baseName)}-${suffix}.pdf`);
    celebrate();
  });
}
