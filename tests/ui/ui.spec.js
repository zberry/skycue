// The skycued web UI in headless Chromium, against the live skycued that
// run.sh starts (fixture replay at real-time speed, looped). Every check is
// on the DOM, the page's test hooks (window.skycueApp) or the HTTP API,
// never on pixels. Both viewport projects run this file against the same
// skycued, one test at a time, and every test leaves the filters and lists
// as it found them (save-baseline aside, which only moves them).
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const childProcess = require('child_process');

const BASE = process.env.SKYCUE_BASE_URL;
const CONF = process.env.SKYCUE_CONF_PATH;
const WS_BASE = BASE.replace(/^http/, 'ws');

// The page's colour classes for skycued's decisions (app_pure.js).
const COLOR = { send: 'sent', allow_listed: 'allow_listed', out_of_range: 'out_of_range',
  below_elevation: 'below_elevation', blocked: 'blocked' };
// A forwarded aircraft well clear of the others (60 nm east, inbound) with a
// registration: the fixture's UAL1423.
const TARGET = 'a4f2c1';

async function api(path, init) {
  const r = await fetch(BASE + path, init);
  const body = await r.json();
  return { status: r.status, body };
}
const state = async () => (await api('/api/state')).body;

let offOrigin;
test.beforeEach(async ({ page }) => {
  offOrigin = [];
  page.on('request', (r) => {
    if (!r.url().startsWith(BASE + '/')) offOrigin.push(r.url());
  });
  page.on('websocket', (ws) => {
    if (!ws.url().startsWith(WS_BASE + '/')) offOrigin.push(ws.url());
  });
});
test.afterEach(async () => {
  expect(offOrigin, 'requests to other hosts').toEqual([]);
});

async function openPage(page) {
  await page.goto('/');
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
}
async function waitMarkers(page, n) {
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().length),
    { timeout: 20000 }).toBeGreaterThanOrEqual(n);
}
async function markerOf(page, hex) {
  return page.evaluate((h) => window.skycueApp.markers().find((m) => m.hex === h) || null, hex);
}
async function selectTarget(page) {
  await waitMarkers(page, 10);
  await page.locator(`.ac-marker[data-hex="${TARGET}"] .ac-glyph`).click();
  const panel = page.locator('#details');
  await expect(panel).toBeVisible();
  await expect(panel.locator('[data-field="hex"]')).toHaveText(TARGET);
  return panel;
}

test('page_loads_without_console_errors', async ({ page }) => {
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(String(e)));
  await openPage(page);
  await expect(page.locator('#map.leaflet-container')).toBeVisible();
  const tiles = (await api('/api/tiles-available')).body;
  if (!tiles.tiles) {
    await expect.poll(() => page.locator('path.graticule').count()).toBeGreaterThan(0);
    await expect(page.locator('.leaflet-control-scale')).toBeVisible();
  }
  await waitMarkers(page, 1);
  await page.waitForTimeout(2000);
  expect(errors).toEqual([]);
});

test('markers_appear_for_all_tracks', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  await expect.poll(async () => Number(await page.locator('#stat-tracks-total').textContent()),
    { timeout: 20000 }).toBeGreaterThanOrEqual(10);
  // Every marker is a positioned track skycued knows.
  const s = await state();
  const known = new Set(s.tracks.filter((t) => t.lat !== undefined).map((t) => t.hex));
  for (const m of await page.evaluate(() => window.skycueApp.markers())) expect(known.has(m.hex), m.hex).toBe(true);
  // Labels: callsign when known, else hex (the TIS-B track has no callsign).
  await expect(page.locator(`.ac-marker[data-hex="${TARGET}"] .ac-label`)).toHaveText('UAL1423');
});

test('click_marker_opens_panel_with_fields', async ({ page }) => {
  await openPage(page);
  const panel = await selectTarget(page);
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);
  await expect(page.locator(`.ac-marker[data-hex="${TARGET}"]`)).toHaveClass(/\bselected\b/);
  await expect(panel.locator('[data-field="callsign"]')).toHaveText('UAL1423');
  await expect(panel.locator('[data-field="registration"]')).toHaveText('N37502');
  await expect(panel.locator('[data-field="lat"]')).toHaveText(/^-?\d+\.\d+°$/);
  await expect(panel.locator('[data-field="lon"]')).toHaveText(/^-?\d+\.\d+°$/);
  await expect(panel.locator('[data-field="alt_baro_ft"]')).toHaveText(/^3\d,\d{3} ft$/);
  await expect(panel.locator('[data-field="gs_kt"]')).toHaveText(/ kt$/);
  await expect(panel.locator('[data-field="range_nm"]')).toHaveText(/^\d+\.\d nm$/);
  await expect(panel.locator('[data-field="elevation_deg"]')).toHaveText(/^-?\d+\.\d+°$/);
  await expect(panel.locator('[data-field="decision"]')).toHaveText('send');
  await expect(panel.locator('[data-field="forwarded"]')).toHaveText('yes');
  await expect(panel.locator('[data-field="first_seen"]')).toContainText('ago');
  await expect(panel.locator('[data-field="last_seen"]')).toContainText('ago');
  // Live: the range changes as the aircraft flies in.
  const r1 = await panel.locator('[data-field="range_nm"]').textContent();
  await expect.poll(() => panel.locator('[data-field="range_nm"]').textContent(), { timeout: 10000 }).not.toBe(r1);
  await page.keyboard.press('Escape');
  await expect(panel).toBeHidden();
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(null);
});

test('add_to_block_list_drops_and_recolours', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  await expect.poll(async () => (await markerOf(page, TARGET) || {}).decision).toBe('sent');
  const panel = await selectTarget(page);
  await panel.getByRole('button', { name: 'Add to block list', exact: true }).click();
  await expect.poll(async () => (await api('/api/lists')).body.block.map((e) => e.toLowerCase()),
    { timeout: 3000 }).toContain(TARGET);
  await expect.poll(async () => (await markerOf(page, TARGET) || {}).decision, { timeout: 3000 }).toBe('blocked');
  await expect(page.locator(`.ac-marker[data-hex="${TARGET}"]`)).toHaveClass(/\bdecision-blocked\b/);
  await expect(panel.getByRole('button', { name: 'Remove from block list', exact: true })).toBeVisible();
  await expect.poll(async () => (await state()).tracks.find((t) => t.hex === TARGET).forwarded,
    { timeout: 3000 }).toBe(false);
  // The filtered aircraft stays on the map and keeps moving.
  const before = await markerOf(page, TARGET);
  await expect.poll(async () => (await markerOf(page, TARGET) || {}).lon, { timeout: 5000 }).not.toBe(before.lon);
  await expect(page.locator('#list-block li', { hasText: TARGET })).toHaveCount(1);

  await panel.getByRole('button', { name: 'Remove from block list', exact: true }).click();
  await expect.poll(async () => (await api('/api/lists')).body.block.map((e) => e.toLowerCase()),
    { timeout: 3000 }).not.toContain(TARGET);
  await expect.poll(async () => (await markerOf(page, TARGET) || {}).decision, { timeout: 5000 }).toBe('sent');
  await expect(panel.getByRole('button', { name: 'Add to block list', exact: true })).toBeVisible();
  await expect.poll(async () => (await state()).tracks.find((t) => t.hex === TARGET).forwarded,
    { timeout: 5000 }).toBe(true);
});

test('range_change_moves_ring_and_drops', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  const r0 = await page.evaluate(() => window.skycueApp.rangeRingRadius());
  const puts = [];
  page.on('request', (r) => {
    if (r.method() === 'PUT' && r.url() === BASE + '/api/filters') puts.push(r.postDataJSON());
  });
  await page.locator('#max-range').fill('20');
  await expect.poll(() => puts.length, { timeout: 3000 }).toBeGreaterThan(0);
  expect(puts[puts.length - 1]).toEqual({ max_range_nm: 20 });
  await expect.poll(() => page.evaluate(() => window.skycueApp.rangeRingRadius())).toBeCloseTo(20 * 1852, 0);
  expect(r0).toBeGreaterThan(20 * 1852);
  await expect(page.locator('#overlay-indicator')).toBeVisible();
  // Aircraft beyond 20 nm turn grey, and the page agrees with skycued
  // (tracks right at the ring may change between the two reads: skipped).
  await expect.poll(async () => {
    const s = await state();
    const ms = await page.evaluate(() => window.skycueApp.markers());
    let bad = 0, far = 0;
    for (const m of ms) {
      const t = s.tracks.find((x) => x.hex === m.hex);
      if (!t || t.range_nm === undefined || Math.abs(t.range_nm - 20) < 1.5) continue;
      if (t.range_nm > 20) {
        far++;
        if (m.decision !== 'out_of_range') bad++;
      }
      if (COLOR[t.decision] !== m.decision) bad++;
    }
    return far >= 5 && bad === 0;
  }, { timeout: 5000 }).toBe(true);
  // Back to the baseline value.
  const res = await api('/api/filters', { method: 'PUT', body: JSON.stringify({ max_range_nm: null }) });
  expect(res.status).toBe(200);
  await expect.poll(() => page.evaluate(() => window.skycueApp.rangeRingRadius())).toBeCloseTo(r0, 0);
  await expect(page.locator('#max-range')).toHaveValue(String(r0 / 1852));
});

test('save_baseline_confirms_and_clears_overlay', async ({ page }) => {
  await openPage(page);
  const cur = (await api('/api/filters')).body.max_range_nm;
  const want = cur === 140 ? 145 : 140;
  await page.locator('#max-range').fill(String(want));
  await expect.poll(async () => (await api('/api/filters')).body.overlay_active, { timeout: 3000 }).toBe(true);
  await expect(page.locator('#overlay-indicator')).toBeVisible();
  let dialogSeen = '';
  page.once('dialog', (d) => { dialogSeen = d.type(); d.accept(); });
  await page.locator('#save-baseline').click();
  await expect.poll(async () => (await api('/api/filters')).body.overlay_active, { timeout: 3000 }).toBe(false);
  expect(dialogSeen).toBe('confirm');
  expect((await api('/api/filters')).body.max_range_nm).toBe(want);
  expect(fs.readFileSync(CONF, 'utf8')).toMatch(new RegExp('^MAX_RANGE_NM=' + want + '\\s*$', 'm'));
  await expect(page.locator('#overlay-indicator')).toBeHidden();
});

async function webUp() {
  try {
    await fetch(BASE + '/api/stats');
    return true;
  } catch (e) {
    return false;
  }
}

test('reconnect_after_restart_converges', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  // A marker for an id the new snapshot will not have: it must go.
  await page.evaluate(() => window.skycueApp.injectTestMarker({ hex: 'fff000', addr_type: 'icao',
    lat: 38.5, lon: -76.5, decision: 'send', track_deg: 0 }));
  expect(await markerOf(page, 'fff000')).not.toBe(null);

  const pid = Number(fs.readFileSync(process.env.SKYCUE_PID_FILE, 'utf8'));
  process.kill(pid, 'SIGTERM');
  await expect.poll(webUp, { timeout: 10000 }).toBe(false);
  await expect(page.locator('#conn-banner')).toBeVisible({ timeout: 10000 });
  expect(await page.evaluate(() => window.skycueApp.connected())).toBe(false);

  const child = childProcess.spawn('bash', ['-c', process.env.SKYCUE_START_CMD], { detached: true, stdio: 'ignore' });
  child.unref();
  await expect.poll(webUp, { timeout: 10000 }).toBe(true);
  await expect(page.locator('#conn-banner')).toBeHidden({ timeout: 15000 });
  await waitMarkers(page, 10);
  expect(await markerOf(page, 'fff000')).toBe(null);
  const s = await state();
  const known = new Set(s.tracks.map((t) => t.hex));
  for (const m of await page.evaluate(() => window.skycueApp.markers())) expect(known.has(m.hex), m.hex).toBe(true);
});

test('screenshots', async ({ page }, testInfo) => {
  await openPage(page);
  await waitMarkers(page, 10);
  await page.waitForTimeout(5000); // some trail
  await selectTarget(page);
  await page.screenshot({ path: testInfo.outputPath(`skycue-map-${testInfo.project.name}.png`), fullPage: true });
  await page.locator('#table-toggle').click();
  await expect(page.locator('#table-view')).toBeVisible();
  await expect.poll(() => page.locator('#table-view tbody tr').count()).toBeGreaterThanOrEqual(10);
  const th = page.locator('#table-view th[data-key="range_nm"]');
  const ranges = async () => (await page.locator('#table-view tbody tr td[data-key="range_nm"]').allTextContents())
    .map(parseFloat).filter((x) => !Number.isNaN(x));
  await th.click();
  await expect(th).toHaveClass(/\bsorted\b/);
  await expect(th).not.toHaveClass(/\bdesc\b/);
  let nums = await ranges();
  expect(nums.length).toBeGreaterThanOrEqual(10);
  expect(nums).toEqual([...nums].sort((a, b) => a - b));
  await th.click();
  await expect(th).toHaveClass(/\bdesc\b/);
  nums = await ranges();
  expect(nums).toEqual([...nums].sort((a, b) => b - a));
  await th.click();
  await page.screenshot({ path: testInfo.outputPath(`skycue-table-${testInfo.project.name}.png`), fullPage: true });
});
