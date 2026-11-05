import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './e2e',
  timeout: 60_000,
  retries: process.env.CI ? 1 : 0,
  use: { baseURL: 'http://localhost:4173', acceptDownloads: true },
  webServer: { command: 'npx vite preview --port 4173 --strictPort', port: 4173, reuseExistingServer: !process.env.CI },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
});
