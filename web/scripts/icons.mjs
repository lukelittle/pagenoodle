// Renders public/icons/icon.svg to PNG app icons with the preinstalled Chromium (dev-time only).
import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
const svg = readFileSync(new URL('../public/icons/icon.svg', import.meta.url), 'utf8');
const browser = await chromium.launch();
for (const size of [192, 512]) {
  const page = await browser.newPage({ viewport: { width: size, height: size } });
  await page.setContent(`<style>html,body{margin:0;background:transparent}</style>${svg.replace('<svg ', `<svg width="${size}" height="${size}" `)}`);
  await page.screenshot({ path: new URL(`../public/icons/icon-${size}.png`, import.meta.url).pathname, omitBackground: true });
  const mask = await browser.newPage({ viewport: { width: size, height: size } });
  await mask.setContent(
    `<style>html,body{margin:0;background:#fff4dc}</style><div style="padding:${size * 0.1}px">${svg.replace('<svg ', `<svg width="${size * 0.8}" height="${size * 0.8}" `)}</div>`,
  );
  await mask.screenshot({ path: new URL(`../public/icons/maskable-${size}.png`, import.meta.url).pathname });
}
await browser.close();
