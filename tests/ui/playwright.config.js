// Headless Chromium against a live skycued started by run.sh, which sets
// SKYCUE_BASE_URL (and SKYCUE_UI_OUT for the results). Both projects share
// one skycued, so the tests run one at a time.
// PLAYWRIGHT_BROWSERS_PATH is honoured by Playwright itself.
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: __dirname,
  testMatch: 'ui.spec.js',
  outputDir: process.env.SKYCUE_UI_OUT || 'test-results',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 90000,
  expect: { timeout: 10000 },
  reporter: [['list']],
  use: {
    baseURL: process.env.SKYCUE_BASE_URL,
    browserName: 'chromium',
    headless: true,
    screenshot: 'on',
    trace: 'off',
  },
  projects: [
    { name: 'desktop-1280', use: { viewport: { width: 1280, height: 720 } } },
    { name: 'desktop-1920', use: { viewport: { width: 1920, height: 1080 } } },
  ],
});
