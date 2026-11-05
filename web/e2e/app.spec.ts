// End-to-end tests against the production build (vite preview).
import { test, expect, type Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import * as pdfjs from 'pdfjs-dist/legacy/build/pdf.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const FX = join(here, '..', '..', 'engine', 'tests', 'fixtures');
const fx = (n: string) => join(FX, n);

async function pdfText(path: string): Promise<{ pages: number; text: string[] }> {
  const doc = await pdfjs.getDocument({ data: new Uint8Array(readFileSync(path)), useSystemFonts: false }).promise;
  const text: string[] = [];
  for (let i = 1; i <= doc.numPages; i++) {
    const c = await (await doc.getPage(i)).getTextContent();
    text.push(c.items.map((it) => ('str' in it ? it.str : '')).join(' '));
  }
  return { pages: doc.numPages, text };
}

async function open(page: Page, files: string[]) {
  await page.goto('/');
  await page.getByTestId('open-files').click();
  await page.getByTestId('file-input').setInputFiles(files);
  await expect(page.getByTestId('page-thumb').first()).toBeVisible();
}

async function download(page: Page, testInfo: { outputPath: (n: string) => string }) {
  const [dl] = await Promise.all([page.waitForEvent('download'), page.getByTestId('download').click()]);
  const path = testInfo.outputPath(dl.suggestedFilename());
  await dl.saveAs(path);
  return { path, name: dl.suggestedFilename() };
}

test.beforeEach(async ({ page }) => {
  // Fail any test that makes a request to another origin.
  page.on('request', (r) => {
    const u = new URL(r.url());
    if (u.hostname !== 'localhost' && u.hostname !== '127.0.0.1' && !u.hostname.endsWith('.invalid')) throw new Error(`External request: ${r.url()}`);
  });
});

test('privacy: CSP is present and outbound requests are blocked', async ({ page }) => {
  await page.goto('/');
  const csp = await page.locator('meta[http-equiv="Content-Security-Policy"]').getAttribute('content');
  expect(csp).toContain("connect-src 'self'");
  expect(csp).toContain("default-src 'none'");
  const blocked = await page.evaluate(async () => {
    try {
      await fetch('https://example.com/');
      return false;
    } catch {
      return true;
    }
  });
  expect(blocked).toBe(true);
  await page.getByTestId('tab-privacy').click();
  await expect(page.getByText('An outbound request was blocked by the browser')).toBeVisible();
});

test('open, reorder, rotate, merge and download', async ({ page }, testInfo) => {
  await open(page, [fx('text3.pdf'), fx('image.pdf')]);
  await expect(page.getByTestId('page-thumb')).toHaveCount(4);
  // keyboard: move page 3 to the front (Alt+Left twice)
  const third = page.getByTestId('page-thumb').nth(2);
  await third.focus();
  await page.keyboard.press('Alt+ArrowLeft');
  await page.keyboard.press('Alt+ArrowLeft');
  // select first page and rotate
  await page.getByTestId('page-thumb').first().click();
  await page.getByRole('button', { name: 'Rotate right' }).click();
  const { path, name } = await download(page, testInfo);
  expect(name).toBe('text3-merged.pdf');
  const res = await pdfText(path);
  expect(res.pages).toBe(4);
  expect(res.text[0]).toContain('Chapter 3');
  expect(res.text[3]).toContain('Caption under the picture');
  const doc = await pdfjs.getDocument({ data: new Uint8Array(readFileSync(path)) }).promise;
  expect((await doc.getPage(1)).rotate).toBe(90);
});

test('split every page produces a zip', async ({ page }, testInfo) => {
  await open(page, [fx('text3.pdf')]);
  await page.getByRole('button', { name: 'Split…' }).click();
  const [dl] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: /Split & download/ }).click()]);
  expect(dl.suggestedFilename()).toBe('text3-split.zip');
  const p = testInfo.outputPath('split.zip');
  await dl.saveAs(p);
  const zip = readFileSync(p);
  expect(zip.subarray(0, 4).toString('hex')).toBe('504b0304');
  expect(zip.toString('latin1')).toContain('text3-p3.pdf');
});

test('true redaction removes the text and is verified', async ({ page }, testInfo) => {
  await open(page, [fx('text3.pdf')]);
  await page.getByTestId('tab-redact').click();
  await page.getByTestId('redact-search').fill('SECRET-CODE-1234');
  await page.keyboard.press('Enter');
  await page.getByTestId('apply-redaction').click();
  const result = page.getByTestId('redaction-result');
  await expect(result).toContainText('no characters');
  await expect(result).toContainText('was not found');
  await page.keyboard.press('Escape');
  const { path } = await download(page, testInfo);
  const raw = readFileSync(path).toString('latin1');
  expect(raw).not.toContain('SECRET');
  expect(raw).not.toContain('Agent Spaghetti'); // metadata stripped
  const res = await pdfText(path);
  expect(res.text.join(' ')).not.toContain('SECRET');
  expect(res.text[1]).toContain('Jane Doe');
});

test('fill a form and sign with a drawn signature', async ({ page }, testInfo) => {
  await open(page, [fx('form.pdf')]);
  await page.getByTestId('tab-edit').click();
  await page.getByLabel('name').fill('Señor Noodle');
  await page.getByTestId('sign-btn').click();
  const pad = page.getByTestId('sig-pad');
  const b = (await pad.boundingBox())!;
  await page.mouse.move(b.x + 20, b.y + 100);
  await page.mouse.down();
  for (let i = 0; i < 25; i++) await page.mouse.move(b.x + 20 + i * 14, b.y + 90 + Math.sin(i / 2) * 30);
  await page.mouse.up();
  await page.getByTestId('use-signature').click();
  const ov = (await page.getByTestId('page-overlay').boundingBox())!;
  await page.mouse.click(ov.x + ov.width * 0.6, ov.y + ov.height * 0.5);
  await page.getByTestId('apply-edits').click();
  await expect(page.getByText(/Signed, sealed/)).toBeVisible();
  const { path, name } = await download(page, testInfo);
  expect(name).toBe('form-signed.pdf');
  const raw = readFileSync(path).toString('latin1');
  expect(raw).toMatch(/\/Subtype\s*\/Image/);
  const doc = await pdfjs.getDocument({ data: new Uint8Array(readFileSync(path)) }).promise;
  const annots = await (await doc.getPage(1)).getAnnotations();
  expect(annots.some((a: { fieldName?: string; fieldValue?: unknown }) => a.fieldName === 'name' && a.fieldValue === 'Señor Noodle')).toBe(true);
});

test('record a recipe, save it, and replay it on another file', async ({ page }, testInfo) => {
  await open(page, [fx('text3.pdf')]);
  await page.getByTestId('tab-recipes').click();
  await page.getByTestId('start-recording').click();
  await page.getByTestId('tab-tools').click();
  await page.getByTestId('apply-stamp').click();
  await expect(page.getByText('Stamped!')).toBeVisible();
  await page.getByTestId('tab-recipes').click();
  await expect(page.getByTestId('recording-steps')).toContainText('Stamp');
  await page.getByRole('button', { name: /Stop & save/ }).click();
  await page.getByLabel('Name').fill('Numberer');
  await page.getByTestId('save-recipe').click();
  await expect(page.getByTestId('recipe-card')).toContainText('Numberer');
  // new file, replay
  await page.getByRole('button', { name: 'Close all files' }).click({ trial: false }).catch(() => {});
  page.once('dialog', (d) => d.accept());
  await page.getByRole('button', { name: 'Close all files' }).click();
  await page.getByTestId('tab-organize').click();
  await page.getByTestId('open-files').click();
  await page.getByTestId('file-input').setInputFiles([fx('many.pdf')]);
  await expect(page.getByTestId('page-thumb').first()).toBeVisible();
  await page.getByTestId('tab-recipes').click();
  const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 60_000 }), page.getByTestId('run-recipe').first().click()]);
  const p = testInfo.outputPath('recipe.pdf');
  await dl.saveAs(p);
  const res = await pdfText(p);
  expect(res.pages).toBe(300);
  expect(res.text[299]).toContain('Page 300 of 300');
});

test('compare two versions', async ({ page }) => {
  await page.goto('/');
  await page.getByTestId('tab-compare').click();
  await page.getByTestId('compare-a').setInputFiles(fx('text3.pdf'));
  await expect(page.getByText('✓ text3.pdf')).toBeVisible();
  await page.getByTestId('compare-b').setInputFiles(fx('text3_v2.pdf'));
  await expect(page.getByText('✓ text3_v2.pdf')).toBeVisible();
  await page.getByTestId('run-compare').click();
  const r = page.getByTestId('compare-result');
  await expect(r.locator('ins.diff').first()).toContainText('spaghetti');
  await expect(r.locator('del.diff').first()).toContainText('noodle');
  await expect(r).toContainText('A brand new sentence appears here.');
});

test('OCR makes a scanned image searchable', async ({ page }, testInfo) => {
  test.setTimeout(120_000);
  await open(page, [join(here, 'fixtures', 'scan.jpg')]);
  await page.getByTestId('tab-tools').click();
  await page.getByRole('button', { name: 'Run OCR' }).click();
  await expect(page.getByText(/made searchable/)).toBeVisible({ timeout: 90_000 });
  const { path } = await download(page, testInfo);
  const res = await pdfText(path);
  expect(res.text[0]).toMatch(/secret\s+code/i);
});

test('works offline after the first visit', async ({ page, context }) => {
  await page.goto('/');
  await page.evaluate(async () => {
    await navigator.serviceWorker.ready;
  });
  // wait for precache to complete
  await expect
    .poll(async () => page.evaluate(async () => (await caches.keys()).length && (await (await caches.open((await caches.keys())[0])).keys()).length), { timeout: 30_000 })
    .toBeGreaterThan(100);
  await page.reload();
  await context.setOffline(true);
  await page.reload();
  await expect(page.getByTestId('offline-badge')).toContainText('offline');
  await page.getByTestId('open-files').click();
  await page.getByTestId('file-input').setInputFiles([fx('text3.pdf')]);
  await expect(page.getByTestId('page-thumb')).toHaveCount(3);
  await context.setOffline(false);
});

test('accessibility: no serious axe violations on main views', async ({ page }) => {
  await open(page, [fx('text3.pdf')]);
  for (const tab of ['organize', 'tools', 'recipes', 'privacy']) {
    await page.getByTestId(`tab-${tab}`).click();
    await page.waitForTimeout(300);
    const r = await new AxeBuilder({ page }).disableRules(['color-contrast']).analyze();
    const serious = r.violations.filter((v) => v.impact === 'serious' || v.impact === 'critical');
    expect(serious.map((v) => `${tab}: ${v.id} ${v.nodes.map((n) => n.target.join(' ')).join(', ')}`)).toEqual([]);
  }
});

// ---------- regressions from the security review ----------

async function drawBox(page: Page, x0: number, y0: number, x1: number, y1: number, pageW = 612, pageH = 792) {
  // coordinates in PDF user space (unrotated page); converts to the on-screen overlay
  const ov = (await page.getByTestId('redact-overlay').boundingBox())!;
  const sx = ov.width / pageW,
    sy = ov.height / pageH;
  await page.mouse.move(ov.x + x0 * sx, ov.y + (pageH - y1) * sy);
  await page.mouse.down();
  await page.mouse.move(ov.x + x1 * sx, ov.y + (pageH - y0) * sy, { steps: 4 });
  await page.mouse.up();
}

test('find & redact also removes the term from form values, notes, bookmarks and metadata', async ({ page }, testInfo) => {
  await open(page, [fx('elsewhere.pdf')]);
  await page.getByTestId('tab-redact').click();
  await page.getByTestId('redact-search').fill('John Smith');
  await page.keyboard.press('Enter');
  await page.getByTestId('apply-redaction').click();
  await expect(page.getByTestId('redaction-result')).toContainText('not found in the page text, annotations, form fields, bookmarks');
  await page.keyboard.press('Escape');
  const { path } = await download(page, testInfo);
  const doc = await pdfjs.getDocument({ data: new Uint8Array(readFileSync(path)) }).promise;
  const text = (await (await doc.getPage(1)).getTextContent()).items.map((i) => ('str' in i ? i.str : '')).join(' ');
  expect(text).not.toMatch(/John\s*Smith/i);
  expect(text).toContain('Emergency contact');
  const annots = JSON.stringify(await (await doc.getPage(1)).getAnnotations());
  expect(annots).not.toContain('John Smith');
  expect(JSON.stringify((await doc.getOutline()) ?? [])).not.toContain('John Smith');
  expect(JSON.stringify((await doc.getMetadata()).info)).not.toContain('John Smith');
});

for (const [file, secret] of [
  ['rot135.pdf', 'ROTSECRET'],
  ['crop.pdf', 'CROPSECRET'],
  ['nested.pdf', 'TOPSECRET'],
]) {
  test(`a hand-drawn box over visible text really removes it (${file})`, async ({ page }, testInfo) => {
    await open(page, [fx(file)]);
    await page.getByTestId('tab-redact').click();
    await drawBox(page, 60, 490, 450, 530);
    await page.getByTestId('apply-redaction').click();
    await expect(page.getByTestId('redaction-result')).toContainText('no characters');
    await page.keyboard.press('Escape');
    const { path } = await download(page, testInfo);
    const res = await pdfText(path);
    expect(res.text.join(' ')).not.toContain(secret);
    expect(res.text[0]).toContain('Public header');
  });
}

test('malformed imported recipes are rejected and cannot break the Recipes view', async ({ page }, testInfo) => {
  await page.goto('/');
  await page.getByTestId('tab-recipes').click();
  const bad = testInfo.outputPath('bad.json');
  writeFileSync(bad, JSON.stringify({ pagenoodle: 'recipe', name: 'evil', steps: [{ op: 'redactText', terms: 'oops' }] }));
  await page.locator('input[type=file][accept*=json]').setInputFiles(bad);
  await expect(page.getByText(/Invalid “redactText” step/)).toBeVisible();
  await expect(page.getByTestId('recipe-card')).toHaveCount(0);
  await page.reload();
  await page.getByTestId('tab-recipes').click();
  await expect(page.getByTestId('start-recording')).toBeVisible();
});

test('the app refuses to run inside a frame', async ({ page }) => {
  // an attacker page on another origin (no CSP of its own) trying to embed the app
  await page.route('**/framing-test.html', (r) =>
    r.fulfill({ contentType: 'text/html', body: '<iframe src="./" width="800" height="600"></iframe>' }),
  );
  await page.goto('/framing-test.html');
  await expect(page.frameLocator('iframe').locator('#app')).toContainText('cannot run inside a frame');
});

test('redaction removes embedded attachments that could hold the original', async ({ page }, testInfo) => {
  await open(page, [join(FX, '..', 'security', 'attach-zip.pdf')]);
  await page.getByTestId('tab-redact').click();
  await page.getByTestId('redact-search').fill('John Smith');
  await page.keyboard.press('Enter');
  await page.getByTestId('apply-redaction').click();
  await expect(page.getByTestId('redaction-result')).toContainText('Attachments and scripts were removed');
  await page.keyboard.press('Escape');
  const { path } = await download(page, testInfo);
  const doc = await pdfjs.getDocument({ data: new Uint8Array(readFileSync(path)) }).promise;
  expect(await doc.getAttachments()).toBeNull();
  expect(readFileSync(path).toString('latin1')).not.toContain('EmbeddedFile');
});
