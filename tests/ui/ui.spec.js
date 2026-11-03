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

// The FIS-B cases run against two more skycueds that run.sh starts, each
// replaying a received recording with uplinks (unpaced, not looped):
// FISB_BASE the 2015 sample (text, NOTAMs, a SIGMET; clear-sky radar),
// RADAR_BASE tg30.978 (real NEXRAD regional and CONUS, rasters, AIRMETs).
const FISB_BASE = process.env.SKYCUE_FISB_URL;
const RADAR_BASE = process.env.SKYCUE_RADAR_URL;

// Every request the page makes goes to the origin it was loaded from
// (data: URLs, the radar canvases, are not requests to a host).
let offOrigin;
test.beforeEach(async ({ page, baseURL }) => {
  offOrigin = [];
  const origin = baseURL.replace(/\/$/, '');
  const wsOrigin = origin.replace(/^http/, 'ws');
  page.on('request', (r) => {
    if (!r.url().startsWith(origin + '/') && !r.url().startsWith('data:')) offOrigin.push(r.url());
  });
  page.on('websocket', (ws) => {
    if (!ws.url().startsWith(wsOrigin + '/')) offOrigin.push(ws.url());
  });
});
test.afterEach(async () => {
  expect(offOrigin, 'requests to other hosts').toEqual([]);
});

// The page asks for overlays/meta.json to learn whether there are overlays
// (scripts/make-overlays.sh); most of these skycueds' www directories have
// none, so the browser's console error for that one 404 is expected.
function overlaysAbsent(m) {
  return /\/overlays\/meta\.json$/.test(m.location().url || '') && /404/.test(m.text());
}

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
  page.on('console', (m) => { if (m.type() === 'error' && !overlaysAbsent(m)) errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(String(e)));
  await openPage(page);
  await expect(page.locator('#map.leaflet-container')).toBeVisible();
  // No overlays here: no overlay toggles; the style select only with tiles.
  await expect(page.locator('#ov-borders')).toBeHidden();
  await expect(page.locator('#ov-cities')).toBeHidden();
  const tiles = (await api('/api/tiles-available')).body;
  if (tiles.tiles) await expect(page.locator('#map-style')).toBeVisible();
  else await expect(page.locator('.view-control')).toBeHidden();
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
  // Typing only validates: nothing is sent until the edit is committed.
  const input = page.locator('#max-range');
  await input.fill('2');
  await page.waitForTimeout(1000);
  expect(puts, 'PUTs while typing').toEqual([]);
  await input.fill('0');
  await expect(input).toHaveClass(/\binvalid\b/);
  await expect(page.locator('#filter-msg')).toHaveClass(/\berror\b/);
  await input.fill('20');
  await expect(input).not.toHaveClass(/\binvalid\b/);
  await page.waitForTimeout(500);
  expect(puts, 'PUTs while typing').toEqual([]);
  expect(await page.evaluate(() => window.skycueApp.rangeRingRadius())).toBeCloseTo(r0, 0);
  await input.press('Enter');
  await expect.poll(() => puts.length, { timeout: 3000 }).toBe(1);
  expect(puts[0]).toEqual({ max_range_nm: 20 });
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
  const res = await api('/api/filters', { method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ max_range_nm: null }) });
  expect(res.status).toBe(200);
  await expect.poll(() => page.evaluate(() => window.skycueApp.rangeRingRadius())).toBeCloseTo(r0, 0);
  await expect(page.locator('#max-range')).toHaveValue(String(r0 / 1852));
});

test('hide_filtered_removes_filtered_markers', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  const snap = async () => JSON.stringify([(await api('/api/filters')).body, (await api('/api/lists')).body]);
  const recStats = async () => {
    const s = (await api('/api/stats')).body;
    return [s.recorder_dropped_received, s.recorder_dropped_sent];
  };
  const filtersBefore = await snap();
  const recBefore = await recStats();
  expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(false);

  // One synchronous evaluate, so the replay cannot move the counts in between.
  const r = await page.evaluate(() => {
    const keep = (m) => m.decision === 'sent' || m.decision === 'allow_listed';
    const before = window.skycueApp.markers();
    window.skycueApp.setHideFiltered(true);
    const after = window.skycueApp.markers();
    return { before: before.length, filtered: before.filter((m) => !keep(m)).length, after: after.length,
      leftover: after.filter((m) => !keep(m)).length, hidden: window.skycueApp.hiddenCount(),
      text: document.getElementById('hidden-count').textContent,
      domFiltered: document.querySelectorAll('.ac-marker:not(.decision-sent):not(.decision-allow_listed)').length,
      domTrails: document.querySelectorAll('path.trail').length };
  });
  expect(r.filtered, 'fixture has filtered-out aircraft').toBeGreaterThan(0);
  expect(r.leftover).toBe(0);
  expect(r.domFiltered).toBe(0);
  expect(r.hidden).toBe(r.filtered);
  expect(r.before - r.after).toBe(r.filtered);
  expect(r.text).toBe(r.filtered + ' hidden');
  expect(r.domTrails).toBe(r.after);
  expect(await page.evaluate(() => localStorage.getItem('skycue.hideFiltered'))).toBe('1');

  // Live updates keep the filtered tracks out (the per-update upsert path).
  const live = () => page.evaluate(() => ({
    dom: document.querySelectorAll('.ac-marker:not(.decision-sent):not(.decision-allow_listed)').length,
    hidden: window.skycueApp.hiddenCount(), text: document.getElementById('hidden-count').textContent }));
  for (let i = 0; i < 4; i++) {
    await page.waitForTimeout(400);
    const l = await live();
    expect(l.dom).toBe(0);
    expect(l.text).toBe(l.hidden > 0 ? l.hidden + ' hidden' : '');
  }
  await page.evaluate(() => window.skycueApp.setHideFiltered(false));

  // A selected track that becomes hidden is deselected and the panel closes.
  await page.evaluate(() => window.skycueApp.setHideFiltered(false));
  const panel = await selectTarget(page);
  await panel.getByRole('button', { name: 'Add to block list', exact: true }).click();
  await expect.poll(async () => (await markerOf(page, TARGET) || {}).decision, { timeout: 3000 }).toBe('blocked');
  await page.evaluate(() => window.skycueApp.setHideFiltered(true));
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(null);
  await expect(page.locator('#details')).toBeHidden();
  await page.evaluate(() => window.skycueApp.setHideFiltered(false));
  await page.locator('#list-block li', { hasText: TARGET }).getByRole('button').click();
  await expect.poll(async () => (await api('/api/lists')).body.block.map((e) => e.toLowerCase()),
    { timeout: 3000 }).not.toContain(TARGET);

  // Off again: the markers return; nothing server-side changed.
  const r2 = await page.evaluate(() => {
    window.skycueApp.setHideFiltered(true);
    const hidden = window.skycueApp.markers().length;
    window.skycueApp.setHideFiltered(false);
    return { hidden, shown: window.skycueApp.markers().length, text: document.getElementById('hidden-count').textContent };
  });
  expect(r2.shown).toBeGreaterThan(r2.hidden);
  const off = await page.evaluate(() => ({ dom: document.querySelectorAll('.ac-marker').length,
    domFiltered: document.querySelectorAll('.ac-marker:not(.decision-sent):not(.decision-allow_listed)').length,
    hook: window.skycueApp.markers().length }));
  expect(off.dom).toBe(off.hook);
  expect(off.domFiltered).toBeGreaterThan(0);
  expect(r2.text).toBe('');
  expect(await page.evaluate(() => localStorage.getItem('skycue.hideFiltered'))).toBe('0');
  await expect.poll(snap, { timeout: 5000 }).toBe(filtersBefore);
  expect(await recStats()).toEqual(recBefore);
  expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(false);
});

test('save_baseline_confirms_and_clears_overlay', async ({ page }) => {
  await openPage(page);
  const cur = (await api('/api/filters')).body.max_range_nm;
  const want = cur === 140 ? 145 : 140;
  await page.locator('#max-range').fill(String(want));
  await page.locator('#max-range').blur(); // committed by leaving the field
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

// A stopped (not exited) skycued keeps the TCP connection open, so no close
// event comes: the page must notice the missing stats by itself.
test('stalled_server_detected_and_reconnects', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  const pid = Number(fs.readFileSync(process.env.SKYCUE_PID_FILE, 'utf8'));
  process.kill(pid, 'SIGSTOP');
  const stopped = Date.now();
  try {
    await expect(page.locator('#conn-banner')).toBeVisible({ timeout: 6900 });
    expect(await page.evaluate(() => window.skycueApp.connected())).toBe(false);
    const left = 7000 - (Date.now() - stopped);
    if (left > 0) await page.waitForTimeout(left);
  } finally {
    process.kill(pid, 'SIGCONT');
  }
  await expect(page.locator('#conn-banner')).toBeHidden({ timeout: 15000 });
  expect(await page.evaluate(() => window.skycueApp.connected())).toBe(true);
  await waitMarkers(page, 1);
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

// ---- FIS-B ---------------------------------------------------------------------
async function apiAt(base, path) {
  const r = await fetch(base + path);
  return { status: r.status, body: await r.json() };
}
function watchErrors(page) {
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error' && !overlaysAbsent(m)) errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(String(e)));
  return errors;
}
async function openFisbPage(page) {
  await openPage(page);
  await page.waitForFunction(() => window.skycueApp.fisbLoaded());
}
function layerToggle(page, name) {
  return page.locator('.leaflet-control-layers-overlays label', { hasText: name }).locator('input');
}
// The index of the first element matching `selector` that is on top at its
// own centre (so a real click lands on it, not on an overlapping shape).
async function clickableIndex(page, selector) {
  return page.evaluate((sel) => {
    const els = Array.from(document.querySelectorAll(sel));
    for (let i = 0; i < els.length; i++) {
      const r = els[i].getBoundingClientRect();
      if (!r.width && !r.height) continue;
      const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
      if (hit === els[i]) return i;
    }
    return -1;
  }, selector);
}

test.describe('fisb radar (tg30)', () => {
  test.use({ baseURL: RADAR_BASE });

  test('radar_canvas_drawn_when_radar_present', async ({ page }) => {
    const errors = watchErrors(page);
    const status = (await apiAt(RADAR_BASE, '/api/fisb/status')).body;
    expect(status.radar.regional.blocks).toBeGreaterThan(0);
    expect(status.radar.conus.blocks).toBeGreaterThan(0);
    await openFisbPage(page);
    // Radar on by default, every other layer off.
    await expect(layerToggle(page, 'Radar (regional)')).toBeChecked();
    await expect(layerToggle(page, 'Radar (CONUS)')).toBeChecked();
    for (const name of ['Other rasters', 'NOTAM / TFR', 'SIGMET', 'AIRMET', 'CWA', 'SUA']) {
      await expect(layerToggle(page, name)).not.toBeChecked();
    }
    await expect(page.locator('img.leaflet-image-layer.fisb-radar-regional')).toHaveCount(1);
    await expect(page.locator('img.leaflet-image-layer.fisb-radar-conus')).toHaveCount(1);
    await expect.poll(() => page.evaluate(() => window.skycueApp.radarStats().products.regional.painted),
      { timeout: 10000 }).toBeGreaterThan(0);
    const st = await page.evaluate(() => window.skycueApp.radarStats());
    expect(st.products.regional.blocks).toBe(status.radar.regional.blocks);
    expect(st.products.conus.blocks).toBe(status.radar.conus.blocks);
    expect(st.products.conus.painted).toBeGreaterThan(0);
    expect(st.blocks).toBe(st.products.regional.blocks + st.products.conus.blocks);
    expect(st.painted).toBeGreaterThan(0);
    // The overlay covers the mosaic's own box.
    const radar = (await apiAt(RADAR_BASE, '/api/fisb/radar?product=regional')).body;
    const b = await page.evaluate(() => window.skycueApp.radarBounds('regional'));
    const north = Math.max(...radar.blocks.map((x) => x.lat0));
    const west = Math.min(...radar.blocks.map((x) => x.lon0));
    expect(b[1][0]).toBeCloseTo(north, 4);
    expect(b[0][1]).toBeCloseTo(west, 4);
    // Radar age from the product time, ticking.
    await expect(page.locator('#radar-age')).toContainText(/\d+ (s|min)/);
    await expect(page.locator('#stat-radar-age')).toHaveText(/\d+ (s|min)/);
    // Toggling the regional layer removes and restores its overlay.
    await layerToggle(page, 'Radar (regional)').uncheck();
    await expect(page.locator('img.fisb-radar-regional')).toHaveCount(0);
    await layerToggle(page, 'Radar (regional)').check();
    await expect(page.locator('img.fisb-radar-regional')).toHaveCount(1);
    expect(errors).toEqual([]);
  });

  test('radar_opacity_slider', async ({ page }) => {
    await openFisbPage(page);
    const img = page.locator('img.leaflet-image-layer.fisb-radar-regional');
    await expect(img).toHaveCount(1);
    const opacity = () => img.evaluate((el) => Number(getComputedStyle(el).opacity));
    await page.locator('#radar-opacity').fill('0.4');
    await expect.poll(opacity).toBeCloseTo(0.4, 2);
    await page.locator('#radar-opacity').fill('1');
    await expect.poll(opacity).toBeCloseTo(1, 2);
    await page.locator('#radar-opacity').fill('0.2');
    await expect.poll(opacity).toBeCloseTo(0.2, 2);
    await expect(page.locator('img.fisb-radar-conus')).toHaveCSS('opacity', '0.2');
  });

  // Regional radar fades to half opacity past 15 min, CONUS past 30 min
  // (the page's data clock moved through the test hook).
  test('radar_fades_with_age', async ({ page }) => {
    const status = (await apiAt(RADAR_BASE, '/api/fisb/status')).body;
    await openFisbPage(page);
    await expect(page.locator('img.leaflet-image-layer.fisb-radar-regional')).toHaveCount(1);
    await expect(page.locator('img.leaflet-image-layer.fisb-radar-conus')).toHaveCount(1);
    const at = (iso) => page.evaluate((t) => window.skycueApp.setDataClock(t), iso);
    const plus = (iso, min) => new Date(Date.parse(iso) + min * 60000).toISOString();
    const reg = status.radar.regional.product_time, conus = status.radar.conus.product_time;
    const base = Number(await page.locator('#radar-opacity').inputValue());
    let o = await at(plus(reg, 14));
    expect(o.regional).toEqual({ opacity: base, faded: false });
    o = await at(plus(reg, 16));
    expect(o.regional.opacity).toBeCloseTo(base / 2, 5);
    expect(o.regional.faded).toBe(true);
    expect(o.label).toMatch(/regional .*\(stale\)/);
    if (Date.parse(plus(conus, 29)) > Date.parse(plus(reg, 16))) {
      o = await at(plus(conus, 29));
      expect(o.conus).toEqual({ opacity: base, faded: false });
    }
    o = await at(plus(conus, 31));
    expect(o.conus.opacity).toBeCloseTo(base / 2, 5);
    expect(o.conus.faded).toBe(true);
    await expect(page.locator('img.fisb-radar-conus')).toHaveClass(/\bfaded\b/);
    // Back on skycued's clock: drawn as before.
    o = await at(status.now);
    expect(o.regional.faded).toBe(false);
  });

  test('other_rasters_layer_and_legend', async ({ page }) => {
    await openFisbPage(page);
    await layerToggle(page, 'Other rasters').check();
    const sel = page.locator('#raster-select');
    await expect.poll(() => sel.locator('option').count()).toBeGreaterThanOrEqual(5);
    await sel.selectOption('70-0');
    await expect(page.locator('img.leaflet-image-layer.fisb-raster')).toHaveCount(1);
    await expect(page.locator('#raster-legend')).toContainText('Icing');
    await expect(page.locator('#raster-legend')).toContainText('approximate');
    await sel.selectOption('103');
    await expect(page.locator('#raster-legend')).toContainText(/lightning/i);
    await layerToggle(page, 'Other rasters').uncheck();
    await expect(page.locator('img.fisb-raster')).toHaveCount(0);
  });

  test('fisb_screenshots_radar', async ({ page }, testInfo) => {
    await openFisbPage(page);
    await expect.poll(() => page.evaluate(() => window.skycueApp.radarStats().painted)).toBeGreaterThan(0);
    await layerToggle(page, 'SIGMET').check();
    await page.locator('#wx-toggle').click();
    await expect(page.locator('#pane-weather')).toBeVisible();
    await expect.poll(() => page.locator('#wx-list li.wx-row').count()).toBeGreaterThan(100);
    await page.waitForTimeout(500);
    await page.screenshot({ path: testInfo.outputPath(`fisb-radar-${testInfo.project.name}.png`), fullPage: true });
  });
});

test.describe('fisb text and airspace (2015 sample)', () => {
  test.use({ baseURL: FISB_BASE });

  test('tfr_polygon_rendered_and_clickable', async ({ page }) => {
    const errors = watchErrors(page);
    const shapes = (await apiAt(FISB_BASE, '/api/fisb/shapes?product=8,16,17')).body.features;
    expect(shapes.length).toBeGreaterThan(0);
    await openFisbPage(page);
    await expect(page.locator('path.fisb-tfr')).toHaveCount(0);   // off by default
    await layerToggle(page, 'NOTAM / TFR').check();
    await expect.poll(() => page.locator('path.fisb-tfr').count()).toBe(shapes.length);
    const i = await clickableIndex(page, 'path.fisb-tfr');
    expect(i).toBeGreaterThanOrEqual(0);
    await page.locator('path.fisb-tfr').nth(i).click();
    const card = page.locator('#shape-details');
    await expect(page.locator('#details')).toBeVisible();
    await expect(card).toBeVisible();
    const key = await card.getAttribute('data-key');
    const f = shapes.find((x) => x.properties.key === key);
    expect(f, 'clicked shape ' + key).toBeTruthy();
    await expect(card.locator('pre.shape-text')).toHaveText(f.properties.text);
    await expect(card.locator('[data-field="product"]')).toContainText('NOTAM');
    await expect(card.locator('[data-field="key"]')).toHaveText(key);
    await expect(card.locator('[data-field="cancelled"]')).toHaveText('no');

    // The fixtures have no TFR polygon (their NOTAMs are points): live items
    // with a polygon, a circle and a cancelled shape, through the page's
    // own WebSocket item handler.
    const tfr = {
      type: 'fisb_shape', product: 8, key: '8-15-999-ZOA-1', features: [
        { type: 'Feature', id: '8-15-999-ZOA-1/0',
          geometry: { type: 'Polygon', coordinates: [[[-122.2, 37.2], [-121.8, 37.2], [-121.8, 37.5], [-122.2, 37.5], [-122.2, 37.2]]] },
          properties: { key: '8-15-999-ZOA-1', product: 8, name: 'NOTAM', text: 'NOTAM-TFR TEST POLYGON', geom: 'polygon',
            cancelled: false, alt_low_ft: 0, alt_high_ft: 3000, agl: true, shape: 0, start: '2015-01-24T04:00:00.000Z',
            text_end: '2015-01-24T08:00:00.000Z' } },
        { type: 'Feature', id: '8-15-999-ZOA-1/1', geometry: { type: 'Point', coordinates: [-121.5, 37.0] },
          properties: { key: '8-15-999-ZOA-1', product: 8, name: 'NOTAM', text: 'NOTAM-TFR TEST POLYGON', geom: 'circle',
            radius_nm: 5, cancelled: false, agl: false, shape: 1 } }]
    };
    const cancelled = {
      type: 'fisb_shape', product: 16, key: '16-15-998-ZOA-1', features: [
        { type: 'Feature', id: '16-15-998-ZOA-1/0',
          geometry: { type: 'Polygon', coordinates: [[[-122.6, 37.6], [-122.4, 37.6], [-122.4, 37.8], [-122.6, 37.6]]] },
          properties: { key: '16-15-998-ZOA-1', product: 16, name: 'NOTAM-TRA', text: 'CANCELLED ONE', geom: 'polygon',
            cancelled: true, agl: false, shape: 0 } }]
    };
    await page.evaluate((items) => window.skycueApp.injectFisbItems(items), [tfr, cancelled]);
    await expect(page.locator('path.fisb-tfr[data-key="8-15-999-ZOA-1"]')).toHaveCount(2);
    expect(await page.evaluate(() => window.skycueApp.shapeLayers('8-15-999-ZOA-1'))).toEqual(
      [{ kind: 'polygon' }, { kind: 'circle', radius_m: 5 * 1852 }]);
    await expect(page.locator('path.fisb-tfr.cancelled')).toHaveCount(1);
    await expect(page.locator('path.fisb-tfr.cancelled')).toHaveAttribute('stroke-dasharray', /\d/);
    // A replaced record replaces its shapes (upsert by key).
    await page.evaluate((it) => window.skycueApp.injectFisbItems([Object.assign({}, it,
      { features: [it.features[0]] })]), tfr);
    await expect(page.locator('path.fisb-tfr[data-key="8-15-999-ZOA-1"]')).toHaveCount(1);
    const j = await clickableIndex(page, 'path.fisb-tfr[data-key="8-15-999-ZOA-1"]');
    expect(j).toBe(0);
    await page.locator('path.fisb-tfr[data-key="8-15-999-ZOA-1"]').click();
    await expect(card).toHaveAttribute('data-key', '8-15-999-ZOA-1');
    await expect(card.locator('pre.shape-text')).toHaveText('NOTAM-TFR TEST POLYGON');
    await expect(card.locator('[data-field="alt"]')).toHaveText('0 – 3,000 ft AGL');
    await expect(card.locator('[data-field="text_end"]')).toContainText('08:00:00Z');
    // Expired (or cancelled): gone from the map and the panel.
    await page.evaluate(() => window.skycueApp.injectFisbItems([
      { type: 'fisb_expired', kind: 'twgo', product: 8, key: '8-15-999-ZOA-1', reason: 'cancelled' },
      { type: 'fisb_expired', kind: 'twgo', product: 16, key: '16-15-998-ZOA-1', reason: 'expired' }]));
    await expect(page.locator('path.fisb-tfr[data-key="8-15-999-ZOA-1"]')).toHaveCount(0);
    await expect(page.locator('path.fisb-tfr.cancelled')).toHaveCount(0);
    await expect(card).toBeHidden();
    await expect(page.locator('path.fisb-tfr')).toHaveCount(shapes.length);
    expect(errors).toEqual([]);
  });

  // A report whose start is still ahead (pending) is drawn dashed and dim,
  // then solid once republished in force.
  test('pending_shape_drawn_dashed_and_dim', async ({ page }) => {
    const errors = watchErrors(page);
    await openFisbPage(page);
    await layerToggle(page, 'SIGMET').check();
    await expect(page.locator('#shape-legend')).toContainText('not yet in force');
    const key = '12-15-996--1';
    const item = (pending) => ({ type: 'fisb_shape', product: 12, key, features: [{ type: 'Feature', id: key + '/0',
      geometry: { type: 'Polygon', coordinates: [[[-120.2, 36.2], [-119.8, 36.2], [-119.8, 36.5], [-120.2, 36.2]]] },
      properties: { key, product: 12, name: 'SIGMET', text: 'SIGMET TEST', geom: 'polygon', cancelled: false,
        pending, start: '2015-01-24T06:00:00.000Z', agl: false, shape: 0 } }] });
    const path = page.locator(`path.fisb-sigmet[data-key="${key}"]`);
    await page.evaluate((it) => window.skycueApp.injectFisbItems([it]), item(true));
    await expect(path).toHaveCount(1);
    await expect(path).toHaveClass(/\bpending\b/);
    await expect(path).toHaveAttribute('stroke-dasharray', /\d/);
    const dim = Number(await path.getAttribute('stroke-opacity'));
    await page.evaluate((it) => window.skycueApp.injectFisbItems([it]), item(false));
    await expect(path).not.toHaveClass(/\bpending\b/);
    expect(await path.getAttribute('stroke-dasharray')).toBeNull();
    expect(Number(await path.getAttribute('stroke-opacity'))).toBeGreaterThan(dim);
    await page.evaluate((k) => window.skycueApp.injectFisbItems([
      { type: 'fisb_expired', kind: 'twgo', product: 12, key: k, reason: 'expired' }]), key);
    await expect(path).toHaveCount(0);
    expect(errors).toEqual([]);
  });

  // An amended TWGO text arrives without graphics (the store dropped the old
  // ones): the page clears the old shape and lists the text instead.
  test('twgo_text_update_clears_old_shape', async ({ page }) => {
    const errors = watchErrors(page);
    await openFisbPage(page);
    await layerToggle(page, 'NOTAM / TFR').check();
    const key = '8-15-997-ZOA-1';
    const path = page.locator(`path.fisb-tfr[data-key="${key}"]`);
    await page.evaluate((k) => window.skycueApp.injectFisbItems([{
      type: 'fisb_shape', product: 8, key: k, features: [{ type: 'Feature', id: k + '/0',
        geometry: { type: 'Polygon', coordinates: [[[-122.2, 37.2], [-121.8, 37.2], [-121.8, 37.5], [-122.2, 37.2]]] },
        properties: { key: k, product: 8, name: 'NOTAM', text: 'NOTAM-TFR ORIGINAL', geom: 'polygon',
          cancelled: false, pending: false, agl: false, shape: 0 } }] }]), key);
    await expect(path).toHaveCount(1);
    await page.evaluate((k) => window.skycueApp.injectFisbItems([{ type: 'fisb_text', product: 8, key: k,
      record: { key: k, kind: 'twgo', product: 8, name: 'NOTAM', report: 997, location: 'ZOA', text: 'NOTAM-TFR AMENDED',
        has_graphics: false, complete: true, cancelled: false, pending: false,
        received: new Date(window.skycueApp.dataNow()).toISOString(),
        last_received: new Date(window.skycueApp.dataNow()).toISOString(), shapes: [] } }]), key);
    await expect(path).toHaveCount(0);
    expect(await page.evaluate((k) => window.skycueApp.shapeLayers(k), key)).toBeNull();
    // Listed with the text-only TWGO reports in the Weather tab.
    await page.locator('#wx-toggle').click();
    const row = page.locator(`#wx-list section[data-kind="twgo"] li.wx-row[data-key="${key}"]`);
    await expect(row).toHaveCount(1);
    await expect(page.locator('#wx-list section[data-kind="twgo"] h3')).toContainText('NOTAM / SIGMET text');
    await row.locator('.wx-head').click();
    await expect(row.locator('.wx-text')).toHaveText('NOTAM-TFR AMENDED');
    // Its graphics again: drawn, no longer listed; then expired: gone.
    await page.evaluate((k) => window.skycueApp.injectFisbItems([{
      type: 'fisb_shape', product: 8, key: k, features: [{ type: 'Feature', id: k + '/0',
        geometry: { type: 'Point', coordinates: [-122.0, 37.3] },
        properties: { key: k, product: 8, name: 'NOTAM', text: 'NOTAM-TFR AMENDED', geom: 'point',
          cancelled: false, pending: false, agl: false, shape: 0 } }] }]), key);
    await expect(path).toHaveCount(1);
    await expect(row).toHaveCount(0);
    await page.evaluate((k) => window.skycueApp.injectFisbItems([
      { type: 'fisb_expired', kind: 'twgo', product: 8, key: k, reason: 'expired' }]), key);
    await expect(path).toHaveCount(0);
    expect(errors).toEqual([]);
  });

  test('weather_panel_lists_metars_and_filters', async ({ page }) => {
    const errors = watchErrors(page);
    const metars = (await apiAt(FISB_BASE, '/api/fisb/text?type=metar')).body.records;
    expect(metars.length).toBeGreaterThanOrEqual(100);
    await openFisbPage(page);
    await page.locator('#wx-toggle').click();
    await expect(page.locator('#details')).toBeVisible();
    await expect(page.locator('#tab-weather')).toHaveAttribute('aria-selected', 'true');
    const rows = page.locator('#wx-list li.wx-row[data-kind="metar"]');
    await expect.poll(() => rows.count()).toBe(metars.length);
    for (const kind of ['taf', 'pirep', 'winds']) {
      await expect.poll(() => page.locator(`#wx-list li.wx-row[data-kind="${kind}"]`).count()).toBeGreaterThan(0);
    }
    const first = rows.first();
    await expect(first.locator('.wx-issued')).toHaveText(/^\d\d:\d\dZ/);
    await expect(first).toHaveClass(/\bage-(fresh|aging|old)\b/);
    await expect(page.locator('#wx-list li.wx-row[data-kind="winds"]').first().locator('.wx-valid')).toHaveText(/valid \d\d:\d\dZ/);
    // Age colouring agrees with the pure function on the data clock.
    const sample = metars[0];
    const want = await page.evaluate((r) => window.skycuePure.ageClass(r.issued, window.skycueApp.dataNow()), sample);
    await expect(page.locator(`#wx-list li.wx-row[data-key="${sample.key}"]`)).toHaveClass(new RegExp('\\bage-' + want + '\\b'));
    // Text expands.
    await first.locator('.wx-head').click();
    await expect(first.locator('.wx-text')).toBeVisible();
    await expect(first.locator('.wx-text')).toContainText('METAR');

    // A 3-letter prefix that some but not all METAR stations have.
    const counts = {};
    for (const r of metars) counts[r.station.slice(0, 3)] = (counts[r.station.slice(0, 3)] || 0) + 1;
    const prefix = Object.keys(counts).sort((a, b) => counts[b] - counts[a])[0];
    expect(counts[prefix]).toBeLessThan(metars.length);
    await page.locator('#wx-filter').fill(prefix.toLowerCase());
    await expect.poll(() => rows.count()).toBe(counts[prefix]);
    for (const st of await rows.evaluateAll((els) => els.map((e) => e.getAttribute('data-station')))) {
      expect(st.startsWith(prefix), st).toBe(true);
    }
    await page.locator('#wx-filter').fill('');
    await expect.poll(() => rows.count()).toBe(metars.length);

    // Live: an upsert adds or replaces by key, an expiry removes.
    await page.evaluate(() => window.skycueApp.injectFisbItems([{ type: 'fisb_text', product: 413, key: 'metar:KZZZ',
      record: { key: 'metar:KZZZ', kind: 'metar', station: 'KZZZ', issued: new Date(window.skycueApp.dataNow()).toISOString(),
        text: 'METAR KZZZ TEST' } }]));
    const zzz = page.locator('#wx-list li.wx-row[data-key="metar:KZZZ"]');
    await expect(zzz).toHaveCount(1);
    await expect(zzz).toHaveClass(/\bage-fresh\b/);
    await expect.poll(() => rows.count()).toBe(metars.length + 1);
    await page.evaluate((k) => window.skycueApp.injectFisbItems([
      { type: 'fisb_expired', kind: 'text', product: 413, key: 'metar:KZZZ', station: 'KZZZ', reason: 'expired' },
      { type: 'fisb_expired', kind: 'text', product: 413, key: k, station: '', reason: 'expired' }]), metars[1].key);
    await expect(zzz).toHaveCount(0);
    await expect.poll(() => rows.count()).toBe(metars.length - 1);
    // The Track tab is still there.
    await page.locator('#tab-track').click();
    await expect(page.locator('#pane-track')).toBeVisible();
    await expect(page.locator('#pane-weather')).toBeHidden();
    expect(errors).toEqual([]);
  });

  test('layer_toggles_hide_show', async ({ page }) => {
    const sig = (await apiAt(FISB_BASE, '/api/fisb/shapes?product=12')).body.features;
    expect(sig.length).toBeGreaterThan(0);
    await openFisbPage(page);
    const paths = page.locator('path.fisb-sigmet');
    await expect(paths).toHaveCount(0);
    await layerToggle(page, 'SIGMET').check();
    await expect(paths).toHaveCount(sig.length);
    await layerToggle(page, 'SIGMET').uncheck();
    await expect(paths).toHaveCount(0);
    await layerToggle(page, 'SIGMET').check();
    await expect(paths).toHaveCount(sig.length);
    await expect(paths.first()).toHaveAttribute('stroke', '#bc8cff');
  });

  test('fisb_status_strip', async ({ page }) => {
    await openFisbPage(page);
    const status = (await apiAt(FISB_BASE, '/api/fisb/status')).body;
    await expect(page.locator('#stats')).toContainText('uplinks/min');
    await expect(page.locator('#stats')).toContainText('products/min');
    await expect(page.locator('#stats')).toContainText('stations');
    await expect(page.locator('#stats')).toContainText('radar age');
    await expect.poll(async () => Number(await page.locator('#stat-fisb-stations').textContent()))
      .toBe(status.stations.length);
    expect(status.stations.length).toBeGreaterThanOrEqual(1);
    await expect(page.locator('#stat-fisb-uplinks')).toHaveText(/^\d+$/, { timeout: 15000 });
    await expect(page.locator('#stat-fisb-products')).toHaveText(/^\d+$/);
    await expect(page.locator('#stat-radar-age')).toHaveText(/\d+ (s|min)/);
    // No product time runs ahead of the fixture's data clock: no skew warning.
    expect(status.clock_skew_s).toBeUndefined();
    await expect(page.locator('#stat-fisb-skew')).toBeHidden();
  });

  test('fisb_screenshots_weather', async ({ page }, testInfo) => {
    await openFisbPage(page);
    await layerToggle(page, 'NOTAM / TFR').check();
    await layerToggle(page, 'SIGMET').check();
    await expect.poll(() => page.locator('path.fisb-sigmet').count()).toBeGreaterThan(0);
    const i = await clickableIndex(page, 'path.fisb-tfr');
    await page.locator('path.fisb-tfr').nth(i).click();
    await expect(page.locator('#shape-details')).toBeVisible();
    await page.locator('#wx-filter').fill('KS');
    await page.locator('#wx-list li.wx-row[data-kind="metar"]').first().locator('.wx-head').click();
    await page.waitForTimeout(500);
    await page.screenshot({ path: testInfo.outputPath(`fisb-weather-${testInfo.project.name}.png`), fullPage: true });
  });
});

// Map tiles, against the skycued run.sh starts with generated tiles while
// meta.json says zoom 0..6: 2/1/1 in four coloured quadrants and one native
// zoom-6 tile, 6/18/24 (the receiver's), transparent in its west half, on
// the edge of its region. Every other zoom-6 tile is missing.
const TILES_BASE = process.env.SKYCUE_TILES_URL;

test.describe('map tiles fallback', () => {
  test.use({ baseURL: TILES_BASE });

  test('missing_tile_shows_lower_zoom_fallback', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    expect((await apiAt(TILES_BASE, '/api/tiles-available')).body).toEqual({ tiles: true, minZoom: 0, maxZoom: 6 });
    await openPage(page);
    await waitMarkers(page, 1);
    // The map fits the range ring (zoom > 6), so it shows zoom-6 tiles;
    // wait until those of the first view (zoom 3) are pruned and all loaded.
    const read = () => page.evaluate(() => Array.from(document.querySelectorAll('.leaflet-tile-pane .leaflet-tile'))
      .map((t) => {
        const imgs = Array.from(t.querySelectorAll('img')).map((i) => ({ src: i.getAttribute('src'),
          natural: i.naturalWidth, width: i.style.width, left: i.style.left, top: i.style.top,
          underlay: i.classList.contains('tile-underlay') }));
        return { tile: t.getAttribute('data-tile'), loaded: t.classList.contains('leaflet-tile-loaded'),
          fallback: t.getAttribute('data-fallback-zoom'), underlay: t.getAttribute('data-underlay-zoom'),
          clip: getComputedStyle(t).overflow, imgs };
      }));
    await expect.poll(async () => (await read()).every((t) => t.tile.startsWith('6/') && t.loaded &&
      t.imgs.every((i) => i.natural === 256)), { timeout: 15000 }).toBe(true);
    const tiles = await read();
    const px = (n) => `${n}px`;
    let fallbacks = 0;
    const offsets = new Set();
    for (const t of tiles) {
      const [, x, y] = t.tile.split('/').map(Number);
      expect(t.clip, t.tile).toBe('hidden');
      // The tile's place in 2/1/1, 16 x 16 tiles: offset (x mod 16, y mod 16).
      const parent = { src: 'tiles/2/1/1.png', natural: 256, width: px(4096),
        left: px(-(x % 16) * 256), top: px(-(y % 16) * 256) };
      if (x === 18 && y === 24) {
        // Native, on its region's edge: the parent underneath, then itself.
        expect(t.fallback, t.tile).toBe(null);
        expect(t.underlay, t.tile).toBe('2');
        expect(t.imgs.length).toBe(2);
        expect(t.imgs[0]).toMatchObject(Object.assign({ underlay: true }, parent));
        expect(t.imgs[1]).toMatchObject({ src: 'tiles/6/18/24.png', width: px(256), left: px(0), top: px(0), underlay: false });
      } else {
        expect(x >> 4, t.tile).toBe(1); // the view is inside 2/1/1
        expect(y >> 4, t.tile).toBe(1);
        expect(t.fallback, t.tile).toBe('2');
        expect(t.imgs.length).toBe(1);
        expect(t.imgs[0], t.tile).toMatchObject(Object.assign({ underlay: false }, parent));
        fallbacks++;
        offsets.add(`${x % 16},${y % 16}`);
      }
    }
    expect(tiles.some((t) => t.tile === '6/18/24'), 'the receiver tile is shown').toBe(true);
    expect(fallbacks).toBeGreaterThanOrEqual(3);
    expect(offsets.size).toBe(fallbacks); // each at its own place in the parent
    const counts = await page.evaluate(() => window.skycueApp.tileCounts());
    expect(counts.underlay).toBeGreaterThanOrEqual(1);
    expect(pageErrors).toEqual([]);
  });

  // Map style and overlays: the same skycued's www has a fixture
  // overlays/ (run.sh): a border line and three cities around the receiver,
  // Alphaville (rank 1), Bravoton (rank 4), Charlie Falls (rank 7).
  test('dark_style_filters_tile_pane_only', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await openPage(page);
    await waitMarkers(page, 1);
    const panes = () => page.evaluate(() => {
      const f = (sel) => getComputedStyle(document.querySelector(sel)).filter;
      return { tileClass: document.querySelector('.leaflet-tile-pane').classList.contains('tiles-dark'),
        tile: f('.leaflet-tile-pane'), marker: f('.leaflet-marker-pane'), overlay: f('.leaflet-overlay-pane'),
        labels: f('.leaflet-cityLabels-pane'), raster: f('.leaflet-fisbRaster-pane') };
    });
    expect(await page.locator('#map-style').inputValue()).toBe('light');
    expect(await panes()).toEqual({ tileClass: false, tile: 'none', marker: 'none', overlay: 'none', labels: 'none', raster: 'none' });
    await page.locator('#map-style').selectOption('dark');
    const dark = await panes();
    expect(dark.tileClass).toBe(true);
    expect(dark.tile).toContain('invert(1)');
    expect(dark).toMatchObject({ marker: 'none', overlay: 'none', labels: 'none', raster: 'none' });
    // Kept across a reload (localStorage).
    await page.reload();
    await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
    expect(await page.locator('#map-style').inputValue()).toBe('dark');
    expect((await panes()).tileClass).toBe(true);
    await page.locator('#map-style').selectOption('light');
    expect((await panes()).tile).toBe('none');
    expect(pageErrors).toEqual([]);
  });

  test('cities_labels_follow_zoom_and_borders_draw', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await openPage(page);
    await waitMarkers(page, 1);   // the map has fitted the range ring: setView below is not undone
    await expect(page.locator('#ov-cities')).toBeVisible();
    await expect(page.locator('#ov-borders')).toBeVisible();
    expect(await page.locator('#ov-cities').isChecked()).toBe(false);
    expect(await page.locator('.city-label').count()).toBe(0);
    await page.locator('#ov-cities').check();
    const labelsAt = async (z) => {
      await page.evaluate((zoom) => window.skycueApp.setView(38.9, -77.0, zoom), z);
      await expect.poll(() => page.evaluate(() => window.skycueApp.zoom())).toBe(z);
      return page.locator('.leaflet-cityLabels-pane .city-name');
    };
    let names = await labelsAt(4);
    await expect(names).toHaveText(['Alphaville']);
    names = await labelsAt(6);
    await expect(names).toHaveText(['Alphaville', 'Bravoton']);
    await expect(page.locator('.city-name', { hasText: 'Charlie Falls' })).toHaveCount(0);
    names = await labelsAt(7);
    await expect(names).toHaveText(['Alphaville', 'Bravoton', 'Charlie Falls']);
    await expect(page.locator('.city-name', { hasText: 'Charlie Falls' })).toBeVisible();
    names = await labelsAt(4);
    await expect(names).toHaveText(['Alphaville']);

    // Borders: the fixture line in its own pane; off again removes it.
    await page.locator('#ov-borders').check();
    await expect.poll(() => page.locator('.leaflet-borders-pane path').count()).toBeGreaterThan(0);
    // Both toggles are kept across a reload.
    await page.reload();
    await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
    expect(await page.locator('#ov-cities').isChecked()).toBe(true);
    expect(await page.locator('#ov-borders').isChecked()).toBe(true);
    await expect.poll(() => page.evaluate(() => window.skycueApp.mapView().citiesLoaded)).toBe(true);
    await page.locator('#ov-borders').uncheck();
    await expect(page.locator('.leaflet-borders-pane path')).toHaveCount(0);
    await page.locator('#ov-cities').uncheck();
    await expect(page.locator('.city-label')).toHaveCount(0);
    expect(pageErrors).toEqual([]);
  });
});

// Overlays without tiles, against the skycued run.sh starts with a www that
// has no tiles/ and an overlays/ holding the border line, while its
// meta.json lists a cities.json that is missing (404).
const OVERLAYS_BASE = process.env.SKYCUE_OVERLAYS_URL;

test.describe('overlays without tiles', () => {
  test.use({ baseURL: OVERLAYS_BASE });

  test('no_tiles_hides_style_and_borders_draw_dark', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    expect((await apiAt(OVERLAYS_BASE, '/api/tiles-available')).body.tiles).toBe(false);
    await openPage(page);
    await waitMarkers(page, 1);
    await expect(page.locator('.view-control')).toBeVisible();
    await expect(page.locator('#map-style')).toBeHidden();
    await expect(page.locator('#ov-borders')).toBeVisible();
    await expect(page.locator('#ov-cities')).toBeVisible();
    await page.locator('#ov-borders').check();
    const path = page.locator('.leaflet-borders-pane path');
    await expect.poll(() => path.count()).toBeGreaterThan(0);
    // On the plain dark background the borders take the dark style's colour.
    const expected = await page.evaluate(() => window.skycuePure.borderStyle('admin0', 'dark', 6).color);
    expect(await path.first().getAttribute('stroke')).toBe(expected);
    expect(pageErrors).toEqual([]);
  });

  test('failed_cities_fetch_draws_no_labels', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await openPage(page);
    await waitMarkers(page, 1);
    await expect(page.locator('#ov-cities')).toBeVisible();
    let asked = 0;
    page.on('request', (r) => { if (r.url().endsWith('/overlays/cities.json')) asked++; });
    const failed = page.waitForResponse((r) => r.url().endsWith('/overlays/cities.json'));
    await page.locator('#ov-cities').check();
    expect((await failed).status()).toBe(404);
    // Moving the map does not ask again (only ticking the box again does).
    await page.evaluate(() => window.skycueApp.setView(38.9, -77.0, 8));
    await page.evaluate(() => window.skycueApp.setView(39.2, -76.5, 7));
    await page.waitForTimeout(500);
    expect(asked).toBe(1);
    expect(await page.locator('#ov-cities').isChecked()).toBe(true);
    expect(await page.locator('.city-label').count()).toBe(0);
    expect(await page.evaluate(() => window.skycueApp.cityLabels())).toEqual([]);
    expect(await page.evaluate(() => window.skycueApp.mapView().citiesLoaded)).toBe(false);
    expect(pageErrors).toEqual([]);
  });
});
