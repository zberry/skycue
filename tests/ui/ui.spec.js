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
// (data: URLs, the radar canvases, are not requests to a host; nor is a
// blob: URL of the page's own origin, a download it made).
let offOrigin;
test.beforeEach(async ({ page, baseURL }) => {
  offOrigin = [];
  const origin = baseURL.replace(/\/$/, '');
  const wsOrigin = origin.replace(/^http/, 'ws');
  page.on('request', (r) => {
    const u = r.url();
    if (!u.startsWith(origin + '/') && !u.startsWith('data:') && !u.startsWith('blob:' + origin + '/')) offOrigin.push(u);
  });
  page.on('websocket', (ws) => {
    if (!ws.url().startsWith(wsOrigin + '/')) offOrigin.push(ws.url());
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
  // The change drops every forwarded track beyond the ring at once, and each
  // drop recolours its marker, so this holds whichever aircraft the replay
  // happens to be observing (some go unobserved for many seconds).
  await expect.poll(async () => {
    const s = await state();
    const ms = await page.evaluate(() => window.skycueApp.markers());
    const bad = [];
    let far = 0;
    for (const m of ms) {
      const t = s.tracks.find((x) => x.hex === m.hex);
      if (!t || t.range_nm === undefined || Math.abs(t.range_nm - 20) < 1.5) continue;
      if (t.range_nm > 20) far++;
      if ((t.range_nm > 20 && m.decision !== 'out_of_range') || COLOR[t.decision] !== m.decision)
        bad.push(`${m.hex}: page ${m.decision}, skycued ${t.decision}`);
    }
    return far >= 5 ? bad : [`only ${far} tracks beyond the ring`];
  }, { timeout: 5000 }).toEqual([]);
  // The same, driven explicitly: a filtered drop alone recolours a marker
  // that sees no further observation.
  await page.evaluate(() => {
    window.skycueApp.injectTestMarker({ hex: 'fff001', addr_type: 'icao', lat: 39.9, lon: -77.0, decision: 'send', track_deg: 0 });
    window.skycueApp.injectFisbItems([{ type: 'drop', hex: 'fff001', addr_type: 'icao', reason: 'filtered', decision: 'out_of_range' }]);
  });
  expect((await markerOf(page, 'fff001')).decision).toBe('out_of_range');
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
  // Back to the map: the Cues tab (a block and its removal make TARGET's rows) and the Config tab.
  await page.locator('#table-toggle').click();
  await expect(page.locator('#table-view')).toBeHidden();
  await page.locator('#tab-cues').click();
  expect(await blockTarget(page, 'add')).toBe(200);
  expect(await blockTarget(page, 'remove')).toBe(200);
  await expect.poll(() => page.locator('#cues-recent tbody tr').count(), { timeout: 15000 }).toBeGreaterThan(1);
  await expect(page.locator('#cues-filters li').first()).toBeVisible();
  await page.screenshot({ path: testInfo.outputPath(`skycue-cues-${testInfo.project.name}.png`), fullPage: true });
  await page.locator('#tab-config').click();
  await expect(page.locator('#pane-config tr[data-key="RECEIVER_LAT"] .cfg-value')).toHaveText('38.9');
  await page.screenshot({ path: testInfo.outputPath(`skycue-config-${testInfo.project.name}.png`), fullPage: true });
});

// Numeric columns are right-aligned, and so are their headers, so each
// value sits under its own header rather than the next one.
test('table numeric headers align with their cells', async ({ page }) => {
  await openPage(page);
  await page.locator('#table-toggle').click();
  await expect(page.locator('#table-view')).toBeVisible();
  const align = (key) => page.locator(`#table-view th[data-key="${key}"]`)
    .evaluate((th) => getComputedStyle(th).textAlign);
  for (const key of ['alt', 'gs_kt', 'range_nm', 'elevation_deg']) expect(await align(key), key).toBe('right');
  for (const key of ['hex', 'callsign', 'registration', 'decision']) expect(await align(key), key).toBe('left');
});

// ---- Cues tab ------------------------------------------------------------------
const net = require('net');
const TARGET_KEY = `${TARGET}|icao`;
// Adds TARGET to or removes it from the block list: a same-origin POST from the page.
const blockTarget = (page, op) => page.evaluate(([o, hex]) => fetch('api/lists/block', { method: 'POST',
  headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ [o]: [hex] }) }).then((r) => r.status), [op, TARGET]);
test('cues_tab_rows_counters_history_clients', async ({ page }) => {
  const errors = watchErrors(page);
  const sock = net.connect(Number(process.env.SKYCUE_CUE_PORT), '127.0.0.1');
  await new Promise((ok) => sock.once('connect', ok));
  try {
    await openPage(page); await waitMarkers(page, 10);
    await page.locator('#wx-toggle').click();
    await page.locator('#tab-cues').click();
    await expect(page.locator('#pane-cues')).toBeVisible();
    // a block makes a filtered drop row with decision "blocked", its removal a send at once (this
    // replay's own sends can be 40 s apart)
    expect(await blockTarget(page, 'add')).toBe(200);
    await expect(page.locator(`#cues-recent tbody tr[data-key="${TARGET_KEY}"]`, { hasText: 'blocked' }).first()).toBeVisible({ timeout: 10000 });
    expect(await blockTarget(page, 'remove')).toBe(200);
    await expect.poll(() => page.locator('#cues-recent tbody tr').count(), { timeout: 15000 }).toBeGreaterThan(1);
    await expect(page.locator('#cues-recent tbody tr').first()).toContainText('sent');
    // the rendered counters match the API within the 1 s stats cadence
    const shown = async (k) => Number(await page.locator(`#cues-counters tr[data-counter="${k}"] td.v`).innerText());
    const st = (await api('/api/stats')).body;
    expect(Math.abs((await shown('sent_updates')) - st.sent_updates)).toBeLessThanOrEqual(st.sent_updates * 0.2 + 50);
    expect(st.drops_blocked).toBeGreaterThan(0);
    for (const k of ['drops_blocked', 'drops_out_of_range', 'drops_below_elevation', 'drops_no_position'])
      await expect.poll(async () => (await shown(k)) === (await api('/api/stats')).body[k], { timeout: 3000 }).toBe(true);
    expect(st.drops_filtered).toBe(st.drops_blocked + st.drops_out_of_range + st.drops_below_elevation + st.drops_no_position);
    await expect(page.locator('#cues-filters li').last()).toContainText('startup');
    // a range change from the page is the newest entry, origin "ui", old → new (a wider range drops nothing)
    const r0 = (await api('/api/filters')).body.max_range_nm;
    const putRange = (v) => page.evaluate((x) => fetch('api/filters', { method: 'PUT',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ max_range_nm: x }) }).then((r) => r.status), v);
    expect(await putRange(r0 + 1)).toBe(200);
    try {
      await expect(page.locator('#cues-filters li').first()).toContainText(`ui: max range ${r0} → ${r0 + 1} nm`);
    } finally { expect(await putRange(null)).toBe(200); }
    // this test's own cue connection is listed
    await expect(page.locator('#cues-clients tr.tcp', { hasText: `127.0.0.1:${sock.localPort}` })).toHaveCount(1, { timeout: 10000 });
    await expect(page.locator('#cues-clients tr.tcp', { hasText: `127.0.0.1:${sock.localPort}` })).toContainText('accepted');
  } finally { sock.destroy(); }
  expect(errors).toEqual([]);
});
test('cues_row_click_selects_track', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.locator('#wx-toggle').click(); await page.locator('#tab-cues').click();
  // TARGET's own sends can be 40 s apart in this replay, so a block and its removal make its row now
  expect(await blockTarget(page, 'add')).toBe(200);
  expect(await blockTarget(page, 'remove')).toBe(200);
  const row = page.locator(`#cues-recent tbody tr[data-key="${TARGET_KEY}"]`).first();
  await expect(row).toBeVisible({ timeout: 15000 });
  await row.click();
  await expect(page.locator('#tab-track')).toHaveAttribute('aria-selected', 'true');
  await expect(page.locator('#details [data-field="hex"]')).toHaveText(TARGET);
});
test('cues_recent_drops_only_kept_in_this_browser', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.locator('#wx-toggle').click(); await page.locator('#tab-cues').click();
  const box = page.locator('#cues-drops-only');
  await expect(box).not.toBeChecked();   // default off: updates and drops
  expect(await blockTarget(page, 'add')).toBe(200);
  expect(await blockTarget(page, 'remove')).toBe(200);
  const rows = page.locator('#cues-recent tbody tr');
  await expect(rows.filter({ hasText: 'sent' }).first()).toBeVisible({ timeout: 15000 });
  await box.check();
  // Drops only: TARGET's filtered drop is there, no update row is.
  await expect(page.locator(`#cues-recent tbody tr[data-key="${TARGET_KEY}"]`, { hasText: 'filtered' }).first()).toBeVisible();
  const reasons = await page.locator('#cues-recent tbody tr td:nth-child(5)').allTextContents();
  expect(reasons.length).toBeGreaterThan(0);
  expect(reasons.filter((r) => r === 'sent')).toEqual([]);
  // Kept across a reload (localStorage), then cleared again.
  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await page.locator('#wx-toggle').click(); await page.locator('#tab-cues').click();
  await expect(page.locator('#cues-drops-only')).toBeChecked();
  await page.locator('#cues-drops-only').uncheck();
  expect(await page.evaluate(() => window.localStorage.getItem('skycue.cues.dropsOnly'))).toBe('0');
});

// The header strip (stats, the four panel buttons, Table, Stats, Alerts with a
// two-digit badge) on one row at 1280 px, with
// live numbers in it (here and, with the FIS-B stats filled, in the 2015
// sample's describe). A plain stat too long for its share is cut with an
// ellipsis by design (style.css), so a cut is not checked here; see
// header_long_stat_is_ellipsised_on_one_row for that.
// headerRowOffsets: how far the vertical middles of the panel buttons, Table, Stats, Alerts and
// every shown stat sit from the brand's (a wrapped control is a whole row, about
// 20 px, off). With `badge`, the Alerts count badge is first shown with that
// text (in the same evaluate, so no live alert can redraw it in between).
async function headerRowOffsets(page, badge) {
  return page.evaluate((badge) => {
    const b = document.getElementById('alerts-badge');
    if (badge !== null && b) {
      b.textContent = badge;
      b.hidden = false;
    }
    const mid = (e) => { const b = e.getBoundingClientRect(); return b.top + b.height / 2; };
    const brand = mid(document.querySelector('#stats .brand'));
    const els = Array.from(document.querySelectorAll('#panel-buttons > button, #table-toggle, #stats-toggle, #alerts-toggle'))
      .concat(Array.from(document.querySelectorAll('#stats > .stat')).filter((e) => !e.hidden && e.offsetParent !== null));
    return els.map((e) => Math.abs(mid(e) - brand));
  }, badge === undefined ? null : badge);
}
test('header_fits_one_row_at_1280', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await openPage(page);
  await waitMarkers(page, 10);
  for (const d of await headerRowOffsets(page, '99')) expect(d).toBeLessThan(4);
  // And at the widths between it and 1920 (where the font and gaps grow).
  for (const width of [1400, 1401, 1600, 1700, 1920]) {
    await page.setViewportSize({ width, height: 800 });
    for (const d of await headerRowOffsets(page, '99')) expect(d, `at ${width} px`).toBeLessThan(4);
  }
});

// A stat value too long for the strip at 1280 px is cut with an ellipsis and
// the header stays on one row. The long text is set and measured in one
// synchronous evaluate, so no live update can land in between.
test('header_long_stat_is_ellipsised_on_one_row', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await openPage(page);
  const r = await page.evaluate(async () => {
    const b = document.getElementById('stat-rec-drops');
    b.textContent = '1234567890 1234567890 1234567890 1234567890 1234567890 1234567890';
    const stat = b.closest('.stat');
    const mid = (e) => { const q = e.getBoundingClientRect(); return q.top + q.height / 2; };
    const brand = mid(document.querySelector('#stats .brand'));
    const els = Array.from(document.querySelectorAll('#panel-buttons > button, #table-toggle, #stats-toggle, #alerts-toggle'))
      .concat(Array.from(document.querySelectorAll('#stats > .stat')).filter((e) => !e.hidden && e.offsetParent !== null));
    const cut = stat.scrollWidth > stat.clientWidth;
    const overflow = getComputedStyle(stat).textOverflow;
    const offsets = els.map((e) => Math.abs(mid(e) - brand));
    // The title follows in a microtask (a MutationObserver); no socket
    // message can run before it.
    await Promise.resolve();
    return { cut, overflow, offsets, title: stat.title, full: stat.textContent.replace(/\s+/g, ' ').trim() };
  });
  expect(r.cut, 'the long stat is cut').toBe(true);
  expect(r.overflow).toBe('ellipsis');
  expect(r.full).toContain('1234567890 1234567890 1234567890 1234567890 1234567890 1234567890');
  expect(r.title, 'the cut stat reads in full on hover').toBe(r.full);
  for (const d of r.offsets) expect(d).toBeLessThan(4);
});

// One header button per side-panel tab (Weather, Receiver, Cues, Config): each
// opens the panel on its tab and is the only one pressed; pressed again it
// closes the panel; Escape closes it and clears all four.
test('header_buttons_open_each_panel_tab', async ({ page }) => {
  await openPage(page);
  const buttons = { weather: '#wx-toggle', receiver: '#rx-toggle', cues: '#cues-toggle', config: '#cfg-toggle' };
  await expect(page.locator('#panel-buttons > button')).toHaveText(['Weather', 'Receiver', 'Cues', 'Config']);
  const panel = page.locator('#details');
  const pressed = () => page.locator('#panel-buttons > button').evaluateAll((bs) =>
    bs.map((b) => b.id + '=' + b.getAttribute('aria-pressed')));
  const expectPressed = async (only) => expect(await pressed()).toEqual(
    Object.values(buttons).map((sel) => sel.slice(1) + '=' + String(sel === only)));
  for (const [tab, sel] of Object.entries(buttons)) {
    await page.locator(sel).click();
    await expect(panel).toBeVisible();
    await expect(page.locator(`#tab-${tab}`)).toHaveAttribute('aria-selected', 'true');
    await expect(page.locator(`#pane-${tab}`)).toBeVisible();
    await expectPressed(sel);
  }
  // The pressed one again closes the panel.
  await page.locator('#cfg-toggle').click();
  await expect(panel).toBeHidden();
  await expectPressed(null);
  // Open on Receiver, then Escape closes it and clears all four.
  await page.locator('#rx-toggle').click();
  await expect(panel).toBeVisible();
  await expectPressed('#rx-toggle');
  await page.keyboard.press('Escape');
  await expect(panel).toBeHidden();
  await expectPressed(null);
  // The inputs counter still opens the Receiver tab, and presses Receiver.
  await page.locator('#stat-inputs').click();
  await expect(page.locator('#tab-receiver')).toHaveAttribute('aria-selected', 'true');
  await expectPressed('#rx-toggle');
});

test('side_panel_tabs_one_row_and_recent_decisions_fit', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.locator('#wx-toggle').click();
  // Five tabs on one row at 13 px in a 340 px panel (both viewports are >= 1280 px).
  const tabs = await page.locator('.tabs [role="tab"]').evaluateAll((ts) => ts.map((t) =>
    [Math.round(t.getBoundingClientRect().top), getComputedStyle(t).fontSize]));
  expect(tabs).toHaveLength(5);
  expect(new Set(tabs.map((t) => t[0])).size).toBe(1);
  expect(new Set(tabs.map((t) => t[1]))).toEqual(new Set(['13px']));
  expect(await page.locator('#details').evaluate((e) => e.getBoundingClientRect().width)).toBe(340);
  // Recent decisions wraps rather than scrolling sideways.
  await page.locator('#tab-cues').click();
  expect(await blockTarget(page, 'add')).toBe(200);
  expect(await blockTarget(page, 'remove')).toBe(200);
  await expect.poll(() => page.locator('#cues-recent tbody tr').count(), { timeout: 15000 }).toBeGreaterThan(1);
  const scroll = page.locator('#pane-cues .cues-scroll');
  expect(await scroll.evaluate((e) => e.scrollWidth - e.clientWidth)).toBeLessThanOrEqual(0);
});

// ---- Alerts ----------------------------------------------------------------------
// The alert strip at the bottom of the main area (alerts_view.js over the
// engine in alerts_pure.js). Each case names the rules it runs with
// (alertsOnly), so the default-on rules on the live replay add no rows.
const ALERT_RULES = ['emergency', 'military_new', 'sent_new', 'input_lost', 'range_enter', 'altitude_below',
  'new_country', 'watch', 'drop_filtered'];
// skycue.alerts before the first load only (a reload keeps what the page stored).
async function alertsOnly(page, on, extra) {
  await page.addInitScript(([on, all, extra]) => {
    if (sessionStorage.getItem('alertsSeeded')) return;
    sessionStorage.setItem('alertsSeeded', '1');
    const rules = {};
    all.forEach((id) => { rules[id] = { enabled: on.includes(id) }; });
    localStorage.setItem('skycue.alerts', JSON.stringify(Object.assign({ version: 1, rules }, extra || {})));
  }, [on, ALERT_RULES, extra || null]);
}
const EMERG = { hex: 'fff7a0', addr_type: 'icao', callsign: 'TEST77', lat: 38.95, lon: -76.9, decision: 'send',
  squawk: '7700', range_nm: 6, alt_baro_ft: 9000, track_deg: 0 };
const inject = (page, o) => page.evaluate((t) => window.skycueApp.injectTestMarker(t), Object.assign({}, EMERG, o));
const alertState = (page) => page.evaluate(() => window.skycueApp.alerts());
const drawerIds = (page) => page.locator('#alerts-log tr[data-alert-id] .al-id').allTextContents();

test('alerts_emergency_raises_strip_and_badge', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  await inject(page);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await expect(page.locator('#alerts')).toHaveClass(/\bhas-emergency\b/);
  await expect(page.locator('#alerts-line .al-kind')).toHaveText('Emergency');
  await expect(page.locator('#alerts-line .al-id')).toHaveText('TEST77');
  await expect(page.locator('#alerts-line .al-detail')).toHaveText('squawk 7700');
  await expect(page.locator('#alerts-line .al-time')).toHaveText(/^\d\d:\d\d:\d\dZ$/);
  await expect(page.locator('#alerts-badge')).toBeVisible();
  await expect(page.locator('#alerts-badge')).toHaveText('1');

  await page.locator('#alerts-ack').click();
  await expect(page.locator('#alerts-count')).toHaveText('0');
  await expect(page.locator('#alerts-badge')).toBeHidden();
  await expect(page.locator('#alerts-line')).toHaveText('No unacknowledged alerts');
  await expect(page.locator('#alerts')).not.toHaveClass(/\bhas-emergency\b/);

  await page.locator('#alerts-expand').click();
  await expect(page.locator('#alerts-drawer')).toBeVisible();
  await expect(page.locator('#alerts-expand')).toHaveAttribute('aria-expanded', 'true');
  await expect(page.locator('#alerts-log tr[data-alert-id]')).toHaveCount(1);
  await expect(page.locator('#alerts-log tr[data-alert-id]')).toHaveClass(/\backed\b/);
});

test('alerts_ack_all_and_drawer_row_ack', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  for (const n of [1, 2, 3]) await inject(page, { hex: `fff7a${n}`, callsign: `T${n}` });
  await expect(page.locator('#alerts-count')).toHaveText('3');
  await page.locator('#alerts-expand').click();
  await expect.poll(() => drawerIds(page)).toEqual(['T3', 'T2', 'T1']);
  // The rows alternate (spec 7); the pointer kept off them (hover has its own background).
  await page.mouse.move(0, 0);
  const rowBg = (i) => page.locator('#alerts-log tr[data-alert-id]').nth(i).evaluate((e) => getComputedStyle(e).backgroundColor);
  expect(await rowBg(1)).not.toBe(await rowBg(0));
  await page.locator('#alerts-log tr[data-alert-id]', { hasText: 'T2' }).locator('.al-ack').click();
  await expect(page.locator('#alerts-count')).toHaveText('2');
  await page.locator('#alerts-ack-all').click();
  await expect(page.locator('#alerts-count')).toHaveText('0');
  await expect(page.locator('#alerts-log tr[data-alert-id]:not(.acked)')).toHaveCount(0);
  await expect(page.locator('#alerts-log tr.acked')).toHaveCount(3);
});

test('alerts_hide_show_and_persist', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  await inject(page);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await page.locator('#alerts-expand').click();
  await page.locator('#alerts-hide').click();
  await expect(page.locator('#alerts')).toBeHidden();
  await expect(page.locator('#alerts-toggle')).toHaveAttribute('aria-pressed', 'false');
  await expect(page.locator('#alerts-badge')).toHaveText('1');

  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await expect(page.locator('#alerts')).toBeHidden();
  await page.locator('#alerts-toggle').click();
  await expect(page.locator('#alerts')).toBeVisible();
  await expect(page.locator('#alerts-toggle')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await expect(page.locator('#alerts-drawer')).toBeVisible();
  await expect(page.locator('#alerts-expand')).toHaveAttribute('aria-expanded', 'true');
  await expect(page.locator('#alerts-log tr[data-alert-id]')).toHaveCount(1);
  expect(await page.evaluate(() => JSON.parse(localStorage.getItem('skycue.alerts')).hidden)).toBe(false);
});

test('alerts_reload_keeps_log_without_refiring', async ({ page }) => {
  await alertsOnly(page, ['watch'], { watch: [TARGET] });
  await openPage(page);
  await expect.poll(async () => (await alertState(page)).rows.length, { timeout: 20000 }).toBe(1);
  const r0 = (await alertState(page)).rows[0];
  expect([r0.rule, r0.key]).toEqual(['watch', `${TARGET}|icao`]);

  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await waitMarkers(page, 10);
  expect((await alertState(page)).rows.length).toBe(1);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await page.locator('#alerts-ack').click();
  await expect(page.locator('#alerts-count')).toHaveText('0');

  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await waitMarkers(page, 10);
  const s = await alertState(page);
  expect(s.rows.length).toBe(1);
  expect(s.unacked).toBe(0);
  expect(s.rows[0].quiet).toBe(false);
  await expect(page.locator('#alerts-count')).toHaveText('0');
});

test('alerts_click_selects_aircraft', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  await inject(page);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await page.locator('#table-toggle').click();
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('table');
  await page.locator('#alerts-line').click();
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe('fff7a0');
});

test('alerts_click_turns_hide_filtered_off', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  try {
    await inject(page, { hex: 'fff7a4', decision: 'out_of_range' });
    await expect(page.locator('#alerts-count')).toHaveText('1');
    await page.evaluate(() => window.skycueApp.setHideFiltered(true));
    await page.locator('#alerts-line').click();
    expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(false);
    await expect(page.locator('#alerts-note')).toHaveText('Hide filtered turned off to show TEST77.');
    expect(await page.evaluate(() => window.skycueApp.selected())).toBe('fff7a4');
  } finally {
    await page.evaluate(() => window.skycueApp.setHideFiltered(false));
  }
});

test('alerts_click_untracked_offers_stats', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await page.route('**/api/stats/aircraft/fff7a5*', (route) => route.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ key: 'fff7a5|icao', hex: 'fff7a5', addr_type: 'icao', first_seen: '2026-11-04T12:00:00.000Z',
      last_seen: '2026-11-04T12:01:00.000Z', observations: 1 }) }));
  await openPage(page);
  await waitMarkers(page, 1);
  await inject(page, { hex: 'fff7a5' });
  await expect(page.locator('#alerts-count')).toHaveText('1');
  await page.evaluate(() => window.skycueApp.removeTestMarker('fff7a5|icao'));
  await page.locator('#alerts-line').click();
  await expect(page.locator('#alerts-note')).toContainText('TEST77 is no longer tracked.');
  await expect(page.locator('#alerts-open-stats')).toBeVisible();
  await expect(page.locator('#alerts-open-stats')).toHaveText('Open in Stats');
  await page.locator('#alerts-open-stats').click();
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('stats');
  await expect.poll(async () => (await statsState(page)).detailKey).toBe('fff7a5|icao');

  await page.locator('#stats-toggle').click();     // back to the map
  await inject(page, { hex: 'fff7a6' });           // no route: the ledger answers 404
  await expect(page.locator('#alerts-count')).toHaveText('2');
  await page.evaluate(() => window.skycueApp.removeTestMarker('fff7a6|icao'));
  const answered = page.waitForResponse((r) => r.url().includes('/api/stats/aircraft/fff7a6'));
  await page.locator('#alerts-line').click();
  await answered;
  await expect(page.locator('#alerts-note')).toContainText('TEST77 is no longer tracked.');
  await expect(page.locator('#alerts-open-stats')).toBeHidden();
});

test('alerts_text_is_not_html', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  const evil = '<img src=x onerror="window.__xss=1">';
  await inject(page, { hex: 'fff7a7', callsign: evil });
  await expect(page.locator('#alerts-line .al-id')).toHaveText(evil);
  await page.locator('#alerts-expand').click();
  await expect(page.locator('#alerts-log tr[data-alert-id] .al-id')).toHaveText(evil);
  expect(await page.locator('#alerts img').count()).toBe(0);
  expect(await page.locator('#alerts-drawer img').count()).toBe(0);
  await page.waitForTimeout(500);
  expect(await page.evaluate(() => window.__xss)).toBeUndefined();
});

test('alerts_strip_leaves_map_controls_clear', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await waitMarkers(page, 1);
  await inject(page);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  const s = await page.locator('#alerts').boundingBox();
  // The scale and the weather control (the main daemon has no tiles, so no
  // attribution control).
  const bottoms = await page.evaluate(() => Array.from(document.querySelectorAll('#map .leaflet-bottom .leaflet-control'))
    .filter((e) => e.offsetParent !== null).map((e) => e.getBoundingClientRect().bottom));
  expect(bottoms.length).toBeGreaterThan(0);
  for (const b of bottoms) expect(b).toBeLessThanOrEqual(s.y + 1);
  const padding = (sel) => page.locator(sel).evaluate((e) => parseFloat(getComputedStyle(e).paddingBottom));
  await page.locator('#table-toggle').click();
  expect(await padding('#table-view')).toBeGreaterThanOrEqual(s.height);
  await page.locator('#stats-toggle').click();
  expect(await padding('#stats-view')).toBeGreaterThanOrEqual(s.height);
  await page.locator('#stats-toggle').click();
});

test('alerts_survive_garbage_storage', async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem('skycue.alerts', '{"rules":7,"hidden":"x"}');
    const rows = [];
    for (let i = 0; i < 5000; i++) {
      if (i % 7 === 0) { rows.push(i % 2 ? null : 'junk'); continue; }
      rows.push({ id: i % 11 === 0 ? 'x' : i, rule: i % 5 === 0 ? 'bogus' : 'emergency',
        time: i % 13 === 0 ? 'never' : new Date(1.7e12 + i * 1000).toISOString(), key: i % 3 ? `ab${i}|icao` : 42,
        label: i % 4 ? `L${i}` : { a: 1 }, detail: 'squawk 7700', acked: i % 2 === 0, quiet: 'yes' });
    }
    localStorage.setItem('skycue.alertLog', JSON.stringify({ version: 1, nextId: 'q', rows,
      pairs: { 'emergency|ab1|icao': 5, 'bogus|x': {} } }));
  });
  const errors = watchErrors(page);
  await openPage(page);
  await expect(page.locator('#alerts')).toBeVisible();
  expect((await alertState(page)).rows.length).toBeLessThanOrEqual(200);
  expect(errors).toEqual([]);
});

// Alerts pressed means "the strip is shown", not "this view is shown", so it
// does not take the pressed look of Table and Stats.
test('alerts_toggle_pressed_differs_from_view_pressed', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await page.locator('#stats-toggle').click();
  await expect(page.locator('#stats-toggle')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#alerts-toggle')).toHaveAttribute('aria-pressed', 'true');
  await page.mouse.move(0, 0);
  const bg = (sel) => page.locator(sel).evaluate((e) => getComputedStyle(e).backgroundColor);
  expect(await bg('#alerts-toggle')).not.toBe(await bg('#stats-toggle'));
  await page.locator('#stats-toggle').click();
});

const storedAlerts = (page) => page.evaluate(() => JSON.parse(localStorage.getItem('skycue.alerts')));

// No AudioContext before the first click anywhere (spec 3.3); then a raised
// row with sound on plays its pattern, and Mute silences it.
test('alerts_sound_waits_for_a_click', async ({ page }) => {
  await page.addInitScript(() => {
    window.__acCount = 0;
    const Real = window.AudioContext;
    window.AudioContext = function () { window.__acCount++; return new Real(); };
  });
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await inject(page);
  await expect(page.locator('#alerts-count')).toHaveText('1');
  expect(await page.evaluate(() => window.__acCount)).toBe(0);
  await expect(page.locator('#alerts-sound-hint')).toBeVisible();
  await expect(page.locator('#alerts-sound-hint')).toContainText('click to enable sound');
  expect((await alertState(page)).tones).toEqual([]);

  await page.locator('#view-heading').click();
  expect(await page.evaluate(() => window.__acCount)).toBe(1);
  await expect(page.locator('#alerts-sound-hint')).toBeHidden();
  expect((await alertState(page)).armed).toBe(true);
  await inject(page, { hex: 'fff7b1' });
  await expect(page.locator('#alerts-count')).toHaveText('2');
  expect((await alertState(page)).tones).toEqual(['double']);

  await page.locator('#alerts-gear').click();
  await page.locator('#alerts-mute').check();
  await inject(page, { hex: 'fff7b2' });
  await expect(page.locator('#alerts-count')).toHaveText('3');
  expect((await alertState(page)).tones).toEqual(['double']);
  expect(await page.evaluate(() => window.__acCount)).toBe(1);
});

test('alerts_config_enables_range_enter', async ({ page }) => {
  await alertsOnly(page, []);
  await openPage(page);
  await page.locator('#alerts-gear').click();
  await expect(page.locator('#alerts-config')).toBeVisible();
  expect((await alertState(page)).configOpen).toBe(true);
  await expect(page.locator('#alerts-rules tr[data-rule]')).toHaveCount(ALERT_RULES.length);
  const row = page.locator('#alerts-rules tr[data-rule="range_enter"]');
  await row.locator('.rule-enabled').check();
  const threshold = row.locator('[data-param="threshold_nm"]');
  await threshold.fill('8');
  await threshold.press('Tab');
  expect((await storedAlerts(page)).rules.range_enter).toEqual({ enabled: true, sound: false, threshold_nm: 8 });
  await inject(page, { hex: 'fff7b3', callsign: 'TESTRG', squawk: '1200', range_nm: 5 });
  await expect(page.locator('#alerts-line .al-kind')).toHaveText('Range');
  await expect(page.locator('#alerts-line .al-detail')).toHaveText('inside 8 nm (5.0 nm)');

  await threshold.fill('-1');
  await expect(threshold).toHaveClass(/\binvalid\b/);
  await threshold.press('Tab');
  expect((await storedAlerts(page)).rules.range_enter.threshold_nm).toBe(8);
});

test('alerts_watch_list_from_config', async ({ page }) => {
  await alertsOnly(page, []);
  await openPage(page);
  await page.locator('#alerts-gear').click();
  await page.locator('#alerts-rules tr[data-rule="watch"] .rule-enabled').check();
  await page.locator('#alerts-watch').fill('testw\nzzz');
  await page.locator('#alerts-watch').blur();
  expect((await storedAlerts(page)).watch).toEqual(['testw', 'zzz']);
  await inject(page, { hex: 'fff7b4', callsign: 'TESTW1', squawk: '1200' });
  await expect(page.locator('#alerts-line .al-kind')).toHaveText('Watch');
  await expect(page.locator('#alerts-line .al-detail')).toHaveText('matches watch entry "testw"');
});

test('alerts_clear_history_and_escape', async ({ page }) => {
  await alertsOnly(page, ['emergency']);
  await openPage(page);
  await inject(page, { hex: 'fff7b5' });
  await inject(page, { hex: 'fff7b6' });
  await expect(page.locator('#alerts-count')).toHaveText('2');
  await page.locator('#alerts-expand').click();
  await expect(page.locator('#alerts-log tr[data-alert-id]')).toHaveCount(2);
  await page.locator('#rx-toggle').click();
  await expect(page.locator('#details')).toBeVisible();
  await page.locator('#alerts-gear').click();
  let question = null;
  page.once('dialog', (d) => { question = d.message(); d.accept(); });
  await page.locator('#alerts-clear').click();
  expect(question).toBe('Clear the alert history? Acknowledged and unacknowledged alerts are removed.');
  await expect(page.locator('#alerts-log tr[data-alert-id]')).toHaveCount(0);
  await expect(page.locator('#alerts-count')).toHaveText('0');
  expect(await page.evaluate(() => JSON.parse(localStorage.getItem('skycue.alertLog')).rows)).toEqual([]);

  await page.keyboard.press('Escape');
  await expect(page.locator('#alerts-config')).toBeHidden();
  expect((await alertState(page)).configOpen).toBe(false);
  await expect(page.locator('#details')).toBeVisible();
});

// The page's own connection lost (a stopped skycued, as in
// stalled_server_detected_and_reconnects): an alert once the banner has been
// up for the hold-off (1 s here), acknowledged with the outage length on
// the reconnect.
test('alerts_daemon_loss_after_banner', async ({ page }) => {
  await alertsOnly(page, ['input_lost']);
  await openPage(page);
  await page.evaluate(() => window.skycueApp.alerts.setHoldOffMs(1000));
  const pid = Number(fs.readFileSync(process.env.SKYCUE_PID_FILE, 'utf8'));
  process.kill(pid, 'SIGSTOP');
  try {
    await expect(page.locator('#conn-banner')).toBeVisible({ timeout: 7000 });
    await expect(page.locator('#alerts-line .al-id')).toHaveText('skycued', { timeout: 5000 });
    await expect(page.locator('#alerts-line .al-detail')).toHaveText('connection lost');
  } finally {
    process.kill(pid, 'SIGCONT');
  }
  await expect(page.locator('#conn-banner')).toBeHidden({ timeout: 15000 });
  const row = (await alertState(page)).rows.find((r) => r.rule === 'input_lost');
  expect(row.input).toBe('skycued');
  expect(row.acked).toBe(true);
  expect(row.recovered).toMatch(/^recovered after \d+ s$/);
});

// ---- Config tab ----------------------------------------------------------------
test('config_tab_shows_effective_values_and_sources', async ({ page }) => {
  const errors = watchErrors(page);
  await page.context().grantPermissions(['clipboard-read', 'clipboard-write'], { origin: BASE });
  const port = new URL(BASE).port;
  await openPage(page);
  await page.locator('#wx-toggle').click();
  await page.locator('#tab-config').click();
  const row = (k) => page.locator(`#pane-config tr[data-key="${k}"]`);
  await expect(row('RECEIVER_LAT').locator('.cfg-value')).toHaveText('38.9');
  await expect(row('RECEIVER_LAT').locator('.cfg-source')).toHaveText('conf');
  await expect(row('WEB_PORT').locator('.cfg-value')).toHaveText(port);
  await expect(row('WEB_PORT').locator('.cfg-restart')).toContainText('restart');
  await expect(row('MIN_INTERVAL_S').locator('.cfg-source')).toHaveText('default');
  await expect(row('MAX_RANGE_NM').locator('.cfg-restart')).not.toContainText('restart');
  await expect(page.locator('#pane-config table[data-group="FIS-B"] tr[data-key="LOCATIONS_FILE"]')).toHaveCount(1);
  await expect(page.locator('#config-daemon')).toContainText('skycued');
  // run.sh's skycued replays the readsb fixture in real time, looped.
  await expect(page.locator('#config-daemon tr', { hasText: 'Runs' })).toContainText(/replay of \S*readsb-sample\.jsonl \(speed 1, looped\)/);
  await expect(page.locator('#config-daemon tr', { hasText: 'Learned locations' })).toContainText(/learned, kept in \S*fisb-locations\.json/);
  await expect(page.locator('#config-daemon tr', { hasText: 'Operator locations' })).toContainText('no LOCATIONS_FILE');
  const text = await page.evaluate(() => window.skycueApp.configText());
  expect(text).toContain(`\n# WEB_PORT: conf, restart\nWEB_PORT=${port}\n`);
  await page.locator('#config-copy').click();
  await expect(page.locator('#config-copy-status')).toHaveText('Copied');
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(text);
  await page.locator('#config-refresh').click();
  await expect(page.locator('#config-copy-status')).toHaveText('');
  // A failed copy shows the text to copy by hand; Refresh clears both.
  await page.evaluate(() => { navigator.clipboard.writeText = () => Promise.reject(new Error('denied')); });
  await page.locator('#config-copy').click();
  await expect(page.locator('#config-copy-status')).toHaveText('Copy failed');
  await expect(page.locator('#config-copy-text')).toBeVisible();
  await page.locator('#config-refresh').click();
  await expect(page.locator('#config-copy-status')).toHaveText('');
  await expect(page.locator('#config-copy-status')).not.toHaveClass(/\berror\b/);
  await expect(page.locator('#config-copy-text')).toBeHidden();
  await expect(row('RECEIVER_LAT').locator('.cfg-value')).toHaveText('38.9');
  expect(errors).toEqual([]);
});

// ---- FIS-B ---------------------------------------------------------------------
async function apiAt(base, path) {
  const r = await fetch(base + path);
  return { status: r.status, body: await r.json() };
}
function watchErrors(page) {
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
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

// Trails, Colour by "signal": an injected aircraft flying east with its
// signal rising from weak to strong, positions 6 s apart (0.03° of
// longitude, about 835 kt: under P.TRAIL_MAX_KT, so the trail joins),
// the last 24 s ago so later steps fit before now.
const SIG_HEX = 'fff0a1';
// A stretch without a value (no signal, no altitude): P.TRAIL_NONE_COLOR.
const NONE_GREY = '#78808a';
async function injectSignalTrack(page) {
  await page.evaluate((hex) => {
    const now = window.skycueApp.dataNow();
    for (let i = 0; i < 12; i++) {
      window.skycueApp.injectTestMarker({ hex, addr_type: 'icao', lat: 39.3, lon: -77.6 + i * 0.03, decision: 'send',
        track_deg: 90, rssi: -34 + i * 2.6, position_time: new Date(now - 24000 - (11 - i) * 6000).toISOString() });
    }
  }, SIG_HEX);
}
test('trail_signal_colours_trail_and_persists', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  await expect(page.locator('#view-trail-color')).toHaveValue('decision');
  await expect(page.locator('#trail-signal-legend')).toBeHidden();
  await injectSignalTrack(page);
  await page.locator(`.ac-marker[data-hex="${SIG_HEX}"] .ac-glyph`).click();
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(SIG_HEX);
  const trail = () => page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX);
  const off = await trail();
  expect(off.points).toBe(12);
  expect(off.runs).toEqual([]);
  expect(off.opacity).toBe(0.6);
  expect(await page.locator('path.trail-run').count()).toBe(0);

  await page.locator('#view-trail-color').selectOption('signal');
  await expect(page.locator('#trail-signal-legend')).toBeVisible();
  await expect(page.locator('#trail-alt-legend')).toBeHidden();
  await expect(page.locator('#trail-signal-legend')).toContainText('faint grey: no signal');
  await expect(page.locator('#trail-signal-legend .sig-scale i')).toHaveCount(6);
  await expect(page.locator('#trail-signal-legend')).toContainText('≤ −35 dBFS');
  await expect(page.locator('#trail-signal-legend')).toContainText('≥ −5 dBFS');
  const on = await trail();
  expect(new Set(on.runs.map((r) => r.color)).size).toBeGreaterThan(1);
  // The colours follow the signal: rising from the weakest step to the strongest.
  const colors = await page.evaluate(() => window.skycuePure.SIGNAL_COLORS);
  expect(on.runs[0].color).toBe(colors[0]);
  expect(on.runs[on.runs.length - 1].color).toBe(colors[colors.length - 1]);
  const steps = on.runs.map((r) => colors.indexOf(r.color));
  expect(steps).toEqual([...steps].sort((a, b) => a - b));
  expect(on.runs.every((r) => !r.dashed && r.opacity === 0.9)).toBe(true);
  // Every segment drawn once, the decision-coloured polyline transparent under the runs.
  expect(on.runs.reduce((n, r) => n + r.n - 1, 0)).toBe(11);
  expect(on.opacity).toBe(0);
  expect(await page.locator('path.trail-run').count()).toBeGreaterThanOrEqual(on.runs.length);
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.view'))).trailColor).toBe('signal');

  // A new position updates the runs in place: one as strong extends the
  // last run (the same layer), a weak one adds a run, one without a signal
  // a run in the decision colour, solid and dim.
  const step = (lon, rssi, ago) => page.evaluate((a) => window.skycueApp.injectTestMarker({ hex: a.hex, addr_type: 'icao',
    lat: 39.3, lon: a.lon, decision: 'send', track_deg: 90, rssi: a.rssi,
    position_time: new Date(window.skycueApp.dataNow() - a.ago).toISOString() }), { hex: SIG_HEX, lon, rssi, ago });
  const lastOf = (t) => t.runs[t.runs.length - 1];
  await step(-77.24, -4, 18000);
  let t = await trail();
  expect(t.runs.length).toBe(on.runs.length);
  expect(lastOf(t).id).toBe(lastOf(on).id);
  expect(lastOf(t).n).toBe(lastOf(on).n + 1);
  expect(t.runs[0].id).toBe(on.runs[0].id);
  await step(-77.21, -40, 12000);
  t = await trail();
  expect(t.runs.length).toBe(on.runs.length + 1);
  expect(lastOf(t)).toMatchObject({ n: 2, color: colors[0], dashed: false });
  await step(-77.18, undefined, 6000);
  t = await trail();
  expect(lastOf(t)).toMatchObject({ n: 2, color: NONE_GREY, dashed: false, opacity: 0.35 });
  expect(t.runs.reduce((n, r) => n + r.n - 1, 0)).toBe(14);
  expect(t.points).toBe(15);
  expect(await page.locator('path.trail-run[stroke-dasharray]').count()).toBe(0);   // trails are never dashed
  await page.locator('#view-trail-color').selectOption('decision');
  const back = await trail();
  expect(back.runs).toEqual([]);
  expect(back.opacity).toBe(0.6);
  expect(await page.locator('path.trail-run').count()).toBe(0);
  await expect(page.locator('#trail-signal-legend')).toBeHidden();

  // Kept in this browser across a reload, then cleared again.
  await page.locator('#view-trail-color').selectOption('signal');
  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await expect(page.locator('#view-trail-color')).toHaveValue('signal');
  await expect(page.locator('#trail-signal-legend')).toBeVisible();
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().trailColor)).toBe('signal');
  // The fixture's own tracks are drawn in runs too (their updates carry rssi).
  await expect.poll(() => page.locator('path.trail-run').count(), { timeout: 20000 }).toBeGreaterThan(0);
  await page.locator('#view-trail-color').selectOption('decision');
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.view'))).trailColor).toBe('decision');
});

// The old "Colour trail by signal" checkbox's setting (skycue.trailSignal)
// becomes Colour by "signal" once, and the old key goes.
test('trail_signal_old_setting_carries_over', async ({ page }) => {
  await page.addInitScript(() => {
    if (sessionStorage.getItem('seeded')) return;
    sessionStorage.setItem('seeded', '1');
    localStorage.setItem('skycue.trailSignal', '1');
    localStorage.setItem('skycue.view', JSON.stringify({ search: '' }));
  });
  await openPage(page);
  await expect(page.locator('#view-trail-color')).toHaveValue('signal');
  expect(await page.evaluate(() => localStorage.getItem('skycue.trailSignal'))).toBe(null);
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.view'))).trailColor).toBe('signal');
  await page.locator('#view-trail-color').selectOption('decision');
});

// Once every position with a signal has aged out of the trail, the whole
// trail is one dim solid run in the decision colour (no signal), in place.
test('trail_signal_dims_when_signal_ages_out', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  await page.evaluate(() => window.skycueApp.setViewFilters({ trailColor: 'signal' }));
  await page.evaluate((hex) => {
    const now = window.skycueApp.dataNow();
    const at = (i, ms, rssi) => window.skycueApp.injectTestMarker({ hex, addr_type: 'icao', lat: 39.3, lon: -77.6 + i * 0.005,
      decision: 'send', track_deg: 90, rssi, position_time: new Date(now - ms).toISOString() });
    for (let i = 0; i < 3; i++) at(i, 118000 - i * 1000, -20);   // just inside the 120 s trail, 1 s apart
    for (let i = 3; i < 8; i++) at(i, (8 - i) * 1000, undefined);   // 5..1 s ago
  }, SIG_HEX);
  const trail = () => page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX);
  let t = await trail();
  expect(t.runs.map((r) => r.opacity)).toEqual([0.9, 0.35]);
  expect(t.runs.every((r) => !r.dashed)).toBe(true);
  const dimId = t.runs[1].id;
  expect(t.opacity).toBe(0);
  // The signal positions leave the 120 s window; the prune trims them.
  await expect.poll(async () => {
    await page.evaluate(() => window.skycueApp.prune());
    return (await trail()).points;
  }, { timeout: 10000 }).toBe(5);
  t = await trail();
  expect(t.runs).toEqual([{ id: dimId, n: 5, color: NONE_GREY, opacity: 0.35, dashed: false }]);
  expect(t.opacity).toBe(0);
  await page.evaluate(() => window.skycueApp.setViewFilters({ trailColor: 'decision' }));
});

// readsb's rssi of -49.5 means "no signal level" (replays, network-fed
// inputs): not the weakest colour but no signal. A trail of only those is
// one dim solid run in the decision colour; a real signal adds a coloured
// run after it, in place. The detail row shows a dash, not -49.5 dBFS.
test('trail_signal_treats_readsb_floor_as_no_signal', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  await page.locator('#view-trail-color').selectOption('signal');
  const hex = 'fff0a2';
  const at = (i, rssi) => page.evaluate((a) => window.skycueApp.injectTestMarker({ hex: a.hex, addr_type: 'icao',
    lat: 39.35, lon: -77.6 + a.i * 0.005, decision: 'send', track_deg: 90, rssi: a.rssi,
    position_time: new Date(window.skycueApp.dataNow() - (10 - a.i) * 1000).toISOString() }), { hex, i, rssi });
  for (let i = 0; i < 6; i++) await at(i, -49.5);
  const trail = () => page.evaluate((h) => window.skycueApp.trail(h), hex);
  let t = await trail();
  expect(t.points).toBe(6);
  expect(t.runs).toEqual([{ id: t.runs[0].id, n: 6, color: NONE_GREY, opacity: 0.35, dashed: false }]);
  expect(t.opacity).toBe(0);
  const dimId = t.runs[0].id;
  await at(6, -20);
  t = await trail();
  expect(t.runs.length).toBe(2);
  expect(t.runs[0]).toMatchObject({ id: dimId, n: 6, color: NONE_GREY, opacity: 0.35, dashed: false });
  expect(t.runs[1]).toMatchObject({ n: 2, opacity: 0.9, dashed: false });
  expect(t.runs[1].color).not.toBe(t.color);
  expect(await page.evaluate(() => window.skycuePure.TRAIL_NONE_COLOR)).toBe(NONE_GREY);
  // As rendered: the dim run's SVG path is grey at the dim opacity, not dashed.
  expect(await page.locator(`path.trail-run[stroke="${NONE_GREY}"][stroke-opacity="0.35"]:not([stroke-dasharray])`).count())
    .toBeGreaterThanOrEqual(1);
  await at(7, -49.5);
  await page.locator(`.ac-marker[data-hex="${hex}"] .ac-glyph`).click();
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(hex);
  const cell = page.locator('#details-fields td[data-field="rssi"]');
  await expect(cell).toHaveText('—');
  await expect(cell).toHaveAttribute('title', 'no signal level from the decoder');
  await page.locator('#view-trail-color').selectOption('decision');
});

// A position the aircraft could not have flown to (over P.TRAIL_MAX_KT
// from the last trail point, as a looped replay jumping back to its start)
// restarts the trail there instead of drawing a line across the map; so
// does one more than 5 s earlier than the last. One up to 5 s earlier is
// left out; a normal move joins.
test('trail_breaks_on_impossible_jump', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  const hex = 'fff0a3';
  const at = (lat, lon, ago) => page.evaluate((a) => window.skycueApp.injectTestMarker({ hex: a.hex, addr_type: 'icao',
    lat: a.lat, lon: a.lon, decision: 'send', track_deg: 0, rssi: -20,
    position_time: new Date(window.skycueApp.dataNow() - a.ago).toISOString() }), { hex, lat, lon, ago });
  const trail = () => page.evaluate((h) => window.skycueApp.trail(h), hex);
  await at(39.0, -77.3, 30000);
  await at(39.02, -77.3, 25000);   // 1.2 nm in 5 s: 864 kt
  await at(39.04, -77.3, 20000);
  expect((await trail()).points).toBe(3);
  await at(39.04 + 200 / 60, -77.3, 15000);   // 200 nm in 5 s
  expect((await trail()).points).toBe(1);
  await at(39.04 + 200 / 60 + 0.02, -77.3, 10000);
  expect((await trail()).points).toBe(2);
  await at(39.04 + 200 / 60 + 0.03, -77.3, 12000);   // 2 s earlier than the last point: left out
  expect((await trail()).points).toBe(2);
  await at(39.04 + 200 / 60 + 0.03, -77.3, 40000);   // 30 s earlier: a break
  expect((await trail()).points).toBe(1);
  // With the signal colouring on, the break drops the runs too; selected,
  // the restarted trail keeps the selected weight.
  await page.evaluate(() => window.skycueApp.setViewFilters({ trailColor: 'signal' }));
  await at(39.04 + 200 / 60 + 0.05, -77.3, 8000);
  let t = await trail();
  expect(t.points).toBe(2);
  expect(t.runs.length).toBe(1);
  await page.evaluate((lat) => window.skycueApp.setView(lat, -77.3, 8), 39.09 + 200 / 60);
  await page.locator(`.ac-marker[data-hex="${hex}"] .ac-glyph`).click();
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(hex);
  expect((await trail()).weight).toBe(3);
  await at(39.0, -77.3, 6000);   // back to the start: a looped replay
  t = await trail();
  expect(t.points).toBe(1);
  expect(t.runs).toEqual([]);
  expect(t.weight).toBe(3);
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(hex);
  await page.evaluate(() => window.skycueApp.setViewFilters({ trailColor: 'decision' }));
});

// Trails, Colour by "altitude": an injected aircraft climbing from the
// ground to 45,000 ft (the same timing as the signal track), then one
// position without an altitude. The runs follow P.ALT_COLORS low to high;
// the legend shows the bounds.
test('trail_altitude_colours_climb', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  const hex = 'fff0a4';
  const alts = [0, 500, 3000, 4500, 8000, 9000, 15000, 25000, 28000, 35000, 42000, 45000];
  await page.evaluate((a) => {
    const now = window.skycueApp.dataNow();
    a.alts.forEach((alt, i) => window.skycueApp.injectTestMarker({ hex: a.hex, addr_type: 'icao', lat: 39.25,
      lon: -77.6 + i * 0.03, decision: 'send', track_deg: 90, gs_kt: 450, rssi: -20, on_ground: i === 0,
      alt_baro_ft: i === 0 || i === 3 ? undefined : alt, alt_geom_ft: i === 3 ? alt : undefined,
      position_time: new Date(now - 30000 - (11 - i) * 6000).toISOString() }));
  }, { hex, alts });
  await page.locator('#view-trail-color').selectOption('altitude');
  const legend = page.locator('#trail-alt-legend');
  await expect(legend).toBeVisible();
  await expect(page.locator('#trail-signal-legend')).toBeHidden();
  await expect(legend.locator('.sig-scale i')).toHaveCount(7);
  await expect(legend).toContainText('ground–1,000 ft');
  await expect(legend).toContainText('> 40,000 ft');
  await expect(legend).toContainText('1k · 5k · 10k · 20k · 30k · 40k ft');
  await expect(legend).toContainText('faint grey: no altitude');
  expect(await legend.locator('.sig-scale i').nth(3).getAttribute('title')).toBe('10,000 to 20,000 ft');
  const trail = () => page.evaluate((h) => window.skycueApp.trail(h), hex);
  const colors = await page.evaluate(() => window.skycuePure.ALT_COLORS);
  let t = await trail();
  expect(t.points).toBe(12);
  expect(t.opacity).toBe(0);
  // ground, 500 | 3000, 4500 (geometric) | 8000, 9000 | 15000 | 25000, 28000 | 35000 | 42000, 45000
  expect(t.runs.map((r) => colors.indexOf(r.color))).toEqual([0, 1, 2, 3, 4, 5, 6]);
  expect(t.runs.map((r) => r.n)).toEqual([2, 3, 3, 2, 3, 2, 3]);
  expect(t.runs.every((r) => !r.dashed && r.opacity === 0.9)).toBe(true);
  // No altitude at all: a dim solid run in the decision colour, appended in place.
  const lastId = t.runs[6].id;
  await page.evaluate((h) => window.skycueApp.injectTestMarker({ hex: h, addr_type: 'icao', lat: 39.25, lon: -77.24,
    decision: 'send', track_deg: 90, gs_kt: 450, rssi: -20,
    position_time: new Date(window.skycueApp.dataNow() - 24000).toISOString() }), hex);
  t = await trail();
  expect(t.runs.length).toBe(8);
  expect(t.runs[6].id).toBe(lastId);
  expect(t.runs[7]).toMatchObject({ n: 2, color: NONE_GREY, opacity: 0.35, dashed: false });
  await page.locator('#view-trail-color').selectOption('decision');
  await expect(legend).toBeHidden();
  expect((await trail()).runs).toEqual([]);
});

// Trails, Show: "selected" leaves only the selected aircraft's polyline on
// the map (the others' removed, not hidden), "off" none, "all" every one
// back; the points are kept throughout, so a trail drawn again has its
// whole history. Remembered in skycue.view; Clear goes back to "all".
async function trailCounts(page) {
  return page.evaluate(() => {
    const ms = window.skycueApp.markers();
    return { dom: document.querySelectorAll('path.trail').length, markers: ms.length,
      withTrail: ms.filter((m) => m.trail > 0).map((m) => m.hex).sort() };
  });
}
test('trails_show_all_selected_off', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 3);
  await expect(page.locator('input[name="view-trail-show"][value="all"]')).toBeChecked();
  await injectSignalTrack(page);
  await page.locator(`.ac-marker[data-hex="${SIG_HEX}"] .ac-glyph`).click();
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(SIG_HEX);
  let c = await trailCounts(page);
  expect(c.withTrail.length).toBeGreaterThan(1);
  expect(c.dom).toBe(c.withTrail.length);

  await page.locator('input[name="view-trail-show"][value="selected"]').check();
  c = await trailCounts(page);
  expect(c.withTrail).toEqual([SIG_HEX]);
  expect(c.dom).toBe(1);
  expect((await page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX)).points).toBe(12);
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.view'))).trailShow).toBe('selected');
  // The selection moves: the trail goes with it, the new one's history at once.
  const other = c.markers > 1 && (await page.evaluate((h) => window.skycueApp.markers().find((m) => m.hex !== h && !m.dimmed).hex, SIG_HEX));
  await page.evaluate((h) => window.skycueApp.selectHex(h), other);
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(other);
  c = await trailCounts(page);
  expect(c.withTrail).toEqual([other]);
  expect(c.dom).toBe(1);
  await page.evaluate((h) => window.skycueApp.selectHex(h), SIG_HEX);
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(SIG_HEX);
  expect((await page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX)).points).toBe(12);

  await page.locator('input[name="view-trail-show"][value="off"]').check();
  c = await trailCounts(page);
  expect(c.withTrail).toEqual([]);
  expect(c.dom).toBe(0);
  expect(await page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX)).toBe(null);
  // A new position while off: kept, drawn when the trails come back.
  await page.evaluate((hex) => window.skycueApp.injectTestMarker({ hex, addr_type: 'icao', lat: 39.3, lon: -77.24,
    decision: 'send', track_deg: 90, rssi: -4, position_time: new Date(window.skycueApp.dataNow() - 18000).toISOString() }), SIG_HEX);
  expect((await trailCounts(page)).dom).toBe(0);
  await expect(page.locator('#view-clear')).toBeEnabled();

  // Kept across a reload.
  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await expect(page.locator('input[name="view-trail-show"][value="off"]')).toBeChecked();
  await waitMarkers(page, 3);
  expect((await trailCounts(page)).dom).toBe(0);
  await injectSignalTrack(page);

  await page.locator('input[name="view-trail-show"][value="all"]').check();
  c = await trailCounts(page);
  expect(c.withTrail.length).toBe(c.markers);
  expect(c.dom).toBe(c.markers);
  expect((await page.evaluate((h) => window.skycueApp.trail(h), SIG_HEX)).points).toBe(12);
  // Clear resets Show to "all".
  await page.locator('input[name="view-trail-show"][value="selected"]').check();
  await page.click('#view-clear');
  await expect(page.locator('input[name="view-trail-show"][value="all"]')).toBeChecked();
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().trailShow)).toBe('all');
  expect((await trailCounts(page)).dom).toBeGreaterThan(1);
});

// Trails, Project ahead: a dashed line from the marker along track_deg for
// gs_kt x the minutes (480 kt for 2 min: 16 nm), in the decision colour;
// moved with the aircraft, gone without a speed or on the ground, following
// Show; "off" removes every one.
test('trail_project_ahead', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 3);
  const hex = 'fff0a5';
  const at = (o) => page.evaluate((a) => window.skycueApp.injectTestMarker(Object.assign({ hex: a.hex, addr_type: 'icao',
    decision: 'send', track_deg: 60, gs_kt: 480, rssi: -20, alt_baro_ft: 12000,
    position_time: new Date(window.skycueApp.dataNow() - a.ago).toISOString() }, a.o)), { hex, o, ago: o.ago || 10000 });
  await at({ lat: 39.2, lon: -77.5, ago: 16000 });
  await at({ lat: 39.21, lon: -77.48, ago: 10000 });
  const ahead = () => page.evaluate((h) => window.skycueApp.ahead(h), hex);
  expect(await ahead()).toBe(null);
  expect(await page.locator('path.trail-ahead').count()).toBe(0);

  await page.locator('#view-trail-ahead').selectOption('2');
  let a = await ahead();
  expect(a.dashed).toBe(true);
  expect(a.points[0]).toEqual(a.marker);
  expect(a.points[0]).toEqual([39.21, -77.48]);
  const far = await page.evaluate((p) => window.skycuePure.rulerMeasure(p[0][0], p[0][1], p[p.length - 1][0], p[p.length - 1][1]), a.points);
  expect(Math.abs(far.nm - 16)).toBeLessThan(0.05);
  expect(Math.abs(far.bearing - 60)).toBeLessThan(0.2);
  expect(a.color).toBe((await page.evaluate((h) => window.skycueApp.trail(h), hex)).color);
  expect(a.weight).toBe(1.5);
  // The fixture's aircraft with a track and a speed get one too: one dashed line per aircraft.
  const total = await page.locator('path.trail-ahead').count();
  expect(total).toBeGreaterThan(1);
  expect(await page.locator('path.trail[stroke-dasharray], path.trail-run[stroke-dasharray]').count()).toBe(0);

  // The aircraft moves: the line starts at the new position; slower, shorter.
  await at({ lat: 39.22, lon: -77.46, gs_kt: 240, ago: 5000 });
  a = await ahead();
  expect(a.points[0]).toEqual([39.22, -77.46]);
  const far2 = await page.evaluate((p) => window.skycuePure.rulerMeasure(p[0][0], p[0][1], p[p.length - 1][0], p[p.length - 1][1]), a.points);
  expect(Math.abs(far2.nm - 8)).toBeLessThan(0.05);
  // A new track: the line turns with it.
  await at({ lat: 39.225, lon: -77.455, gs_kt: 240, track_deg: 150, ago: 4500 });
  a = await ahead();
  const far3 = await page.evaluate((p) => window.skycuePure.rulerMeasure(p[0][0], p[0][1], p[p.length - 1][0], p[p.length - 1][1]), a.points);
  expect(Math.abs(far3.bearing - 150)).toBeLessThan(0.2);
  // No ground speed, or on the ground: no line; back when it has one again.
  await at({ lat: 39.23, lon: -77.44, gs_kt: undefined, ago: 4000 });
  expect(await ahead()).toBe(null);
  await at({ lat: 39.231, lon: -77.439, on_ground: true, ago: 3000 });
  expect(await ahead()).toBe(null);
  await at({ lat: 39.232, lon: -77.438, ago: 2000 });
  expect(await ahead()).not.toBe(null);

  // Show "selected": the selected aircraft's line only; "off": none.
  await page.evaluate((h) => window.skycueApp.selectHex(h), hex);
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(hex);
  expect((await ahead()).weight).toBe(3);
  await page.locator('input[name="view-trail-show"][value="selected"]').check();
  expect(await page.locator('path.trail-ahead').count()).toBe(1);
  expect(await ahead()).not.toBe(null);
  // Nothing selected: no trail and no projection at all.
  await page.keyboard.press('Escape');
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(null);
  expect(await page.locator('path.trail').count()).toBe(0);
  expect(await page.locator('path.trail-ahead').count()).toBe(0);
  expect(await ahead()).toBe(null);
  await page.evaluate((h) => window.skycueApp.selectHex(h), hex);
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBe(hex);
  expect(await page.locator('path.trail-ahead').count()).toBe(1);
  await page.locator('input[name="view-trail-show"][value="off"]').check();
  expect(await page.locator('path.trail-ahead').count()).toBe(0);
  await page.locator('input[name="view-trail-show"][value="all"]').check();
  expect(await page.locator('path.trail-ahead').count()).toBeGreaterThan(1);

  await page.locator('#view-trail-ahead').selectOption('0');
  expect(await page.locator('path.trail-ahead').count()).toBe(0);
  expect(await ahead()).toBe(null);
});

// A filter set saved before the Trails group (no trail fields) gets the
// Trails settings the page loads with, once, so pressing its number key
// keeps them instead of resetting Trails.
test('trails_old_sets_keep_current_trails', async ({ page }) => {
  await page.addInitScript(() => {
    if (sessionStorage.getItem('seeded')) return;
    sessionStorage.setItem('seeded', '1');
    localStorage.setItem('skycue.view', JSON.stringify({ trailShow: 'selected', trailColor: 'altitude', trailAhead: 2 }));
    localStorage.setItem('skycue.viewSets', JSON.stringify({ version: 1, sets: [{ name: 'Old', view: { search: 'ual' }, hideFiltered: false }] }));
  });
  await openPage(page);
  const set = JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.viewSets'))).sets[0].view;
  expect([set.search, set.trailShow, set.trailColor, set.trailAhead]).toEqual(['ual', 'selected', 'altitude', 2]);
  await page.locator('body').press('1');
  const v = await page.evaluate(() => window.skycueApp.getViewFilters());
  expect([v.search, v.trailShow, v.trailColor, v.trailAhead]).toEqual(['ual', 'selected', 'altitude', 2]);
  await page.locator('body').press('0');
});

// The three Trails settings in a saved filter set (the number keys too),
// cleared by 0, and kept in skycue.view across a reload.
test('trails_settings_in_sets_and_reload', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 1);
  await page.locator('input[name="view-trail-show"][value="selected"]').check();
  await page.locator('#view-trail-color').selectOption('altitude');
  await page.locator('#view-trail-ahead').selectOption('5');
  await expect(page.locator('#view-clear')).toBeEnabled();
  page.once('dialog', (d) => d.accept('Trails'));
  await page.click('#view-set-save');
  const saved = JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.viewSets'))).sets[0].view;
  expect([saved.trailShow, saved.trailColor, saved.trailAhead]).toEqual(['selected', 'altitude', 5]);

  await page.locator('body').press('0');
  await expect(page.locator('input[name="view-trail-show"][value="all"]')).toBeChecked();
  await expect(page.locator('#view-trail-color')).toHaveValue('decision');
  await expect(page.locator('#view-trail-ahead')).toHaveValue('0');
  await expect(page.locator('#trail-alt-legend')).toBeHidden();
  await expect(page.locator('#view-clear')).toBeDisabled();

  await page.locator('body').press('1');
  await expect(page.locator('input[name="view-trail-show"][value="selected"]')).toBeChecked();
  await expect(page.locator('#view-trail-color')).toHaveValue('altitude');
  await expect(page.locator('#view-trail-ahead')).toHaveValue('5');
  await expect(page.locator('#trail-alt-legend')).toBeVisible();
  expect(await setSelectedText(page)).toBe('Trails');
  // A Trails change alone makes the set "(modified)".
  await page.locator('#view-trail-ahead').selectOption('1');
  expect(await setSelectedText(page)).toBe('Trails (modified)');
  await page.locator('#view-trail-ahead').selectOption('5');
  expect(await setSelectedText(page)).toBe('Trails');

  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await expect(page.locator('input[name="view-trail-show"][value="selected"]')).toBeChecked();
  await expect(page.locator('#view-trail-color')).toHaveValue('altitude');
  await expect(page.locator('#view-trail-ahead')).toHaveValue('5');
  await expect(page.locator('#trail-alt-legend')).toBeVisible();
  const v = await page.evaluate(() => window.skycueApp.getViewFilters());
  expect([v.trailShow, v.trailColor, v.trailAhead]).toEqual(['selected', 'altitude', 5]);
  await page.locator('body').press('0');
});

// The ruler: away from the fixture's aircraft (around 38.9 N 77 W), two
// clicks 300 px apart on one row at zoom 8 near 30 N are 84..88 nm
// (611.5 m/px at the equator x cos(lat)), heading east.
test('ruler_measures_without_touching_selection', async ({ page }) => {
  await openPage(page);
  const panel = await selectTarget(page);
  const toggle = page.locator('#ruler-toggle');
  await expect(toggle).toHaveAttribute('title', 'Measure');
  await expect(toggle).toHaveAttribute('aria-pressed', 'false');
  await page.evaluate(() => window.skycueApp.setView(30, -90, 8));
  const ruler = () => page.evaluate(() => window.skycueApp.ruler());
  await toggle.click();
  await expect(toggle).toHaveAttribute('aria-pressed', 'true');
  const box = await page.locator('#map').boundingBox();
  const cx = box.x + box.width / 2, cy = box.y + box.height / 2;
  await page.mouse.click(cx - 150, cy);
  let r = await ruler();
  expect(r.armed).toBe(true);
  expect(r.start).not.toBeNull();
  expect(r.end).toBeNull();
  await page.mouse.move(cx + 50, cy, { steps: 3 });
  r = await ruler();
  expect(r.fixed).toBe(false);
  expect(r.label).toMatch(/ nm · .* km · \d{3}°T$/);
  await page.mouse.click(cx + 150, cy);
  r = await ruler();
  expect(r.fixed).toBe(true);
  const m = /^(\d+\.\d) nm · (\d+\.\d) km · (\d{3})°T$/.exec(r.label);
  expect(m, r.label).not.toBeNull();
  const nm = Number(m[1]), km = Number(m[2]), brg = Number(m[3]);
  expect(nm).toBeGreaterThanOrEqual(84);
  expect(nm).toBeLessThanOrEqual(88);
  expect(Math.abs(km - nm * 1.852)).toBeLessThan(0.2);
  expect(brg).toBeGreaterThanOrEqual(88);
  expect(brg).toBeLessThanOrEqual(92);
  await expect(page.locator('.ruler-label')).toHaveText(r.label);
  await expect(page.locator('path.ruler-line')).toHaveCount(1);
  // Its label stays on the map near the right edge (flipped to the left).
  await page.mouse.click(cx - 150, cy);
  await page.mouse.move(box.x + box.width - 20, cy, { steps: 3 });
  const lb = await page.locator('.ruler-label span').boundingBox();
  expect(lb.x).toBeGreaterThanOrEqual(box.x);
  expect(lb.x + lb.width).toBeLessThanOrEqual(box.x + box.width);
  await page.mouse.click(cx + 150, cy);
  expect((await ruler()).label).toBe(r.label);
  // The map clicks neither deselected nor closed the panel; the mouse no
  // longer moves a fixed ruler.
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);
  await expect(panel).toBeVisible();
  await page.mouse.move(cx, cy + 100);
  expect((await ruler()).label).toBe(r.label);

  // Escape clears and disarms, the panel stays; an open set menu closes too.
  await page.locator('#view-set-more').click();
  await expect(page.locator('#view-set-menu')).toBeVisible();
  await page.evaluate(() => document.activeElement.blur());   // the menu's own Escape handler is not asked
  await page.keyboard.press('Escape');
  await expect(page.locator('#view-set-menu')).toBeHidden();
  r = await ruler();
  expect(r).toEqual({ armed: false, start: null, end: null, fixed: false, label: '' });
  await expect(toggle).toHaveAttribute('aria-pressed', 'false');
  await expect(page.locator('path.ruler-line')).toHaveCount(0);
  await expect(page.locator('.ruler-label')).toHaveCount(0);
  await expect(panel).toBeVisible();
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);

  // Armed, an aircraft click is an endpoint (its position), not a selection.
  await page.evaluate(() => window.skycueApp.setView(38.9, -77.0, 7));
  const other = (await page.evaluate(() => window.skycueApp.markers())).find((x) => x.hex !== TARGET && !x.dimmed);
  await toggle.click();
  await page.locator(`.ac-marker[data-hex="${other.hex}"] .ac-glyph`).click();
  r = await ruler();
  expect(r.start).not.toBeNull();
  expect(Math.abs(r.start.lat - other.lat)).toBeLessThan(0.05);
  expect(Math.abs(r.start.lon - other.lon)).toBeLessThan(0.05);
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);
  // The button again clears and disarms.
  await toggle.click();
  expect((await ruler()).armed).toBe(false);
  await expect(page.locator('.ruler-dot')).toHaveCount(0);
  // Covering the map with the table clears it too.
  await toggle.click();
  await page.mouse.click(cx - 150, cy);
  expect((await ruler()).start).not.toBeNull();
  await page.locator('#table-toggle').click();
  expect(await ruler()).toEqual({ armed: false, start: null, end: null, fixed: false, label: '' });
  await page.locator('#table-toggle').click();
  // Disarmed, a map click deselects as before.
  await page.evaluate(() => window.skycueApp.setView(30, -90, 8));
  await page.mouse.click(cx - 150, cy);
  await expect.poll(() => page.evaluate(() => window.skycueApp.selected())).toBeNull();
});

// The ruler button keeps focus after a click; Leaflet's own a:focus rule
// once turned it near-white when the pointer left. Idle, focused and
// disarmed it looks like the zoom buttons; armed it does not.
test('ruler_toggle_matches_zoom_buttons_after_disarm', async ({ page }) => {
  await openPage(page);
  const bg = (sel) => page.locator(sel).evaluate((el) => getComputedStyle(el).backgroundColor);
  const zoom = await bg('a.leaflet-control-zoom-in');
  const box = await page.locator('#map').boundingBox();
  const away = () => page.mouse.move(box.x + box.width / 2, box.y + box.height - 40);
  expect(await bg('#ruler-toggle')).toBe(zoom);
  await page.locator('#ruler-toggle').hover();   // hover lightens, as on the zoom buttons
  const hovered = await bg('#ruler-toggle');
  await page.locator('a.leaflet-control-zoom-in').hover();
  expect(await bg('a.leaflet-control-zoom-in')).toBe(hovered);
  expect(hovered).not.toBe(zoom);
  await away();
  await page.locator('#ruler-toggle').click();
  await away();
  await expect(page.locator('#ruler-toggle')).toHaveAttribute('aria-pressed', 'true');
  expect(await bg('#ruler-toggle')).not.toBe(zoom);
  await page.locator('#ruler-toggle').click();
  await away();
  await expect(page.locator('#ruler-toggle')).toHaveAttribute('aria-pressed', 'false');
  expect(await page.evaluate(() => document.activeElement && document.activeElement.id)).toBe('ruler-toggle');
  expect(await bg('#ruler-toggle')).toBe(zoom);
  expect(await bg('a.leaflet-control-zoom-in')).toBe(zoom);
  // At max zoom Leaflet disables zoom-in (#f4f4f4 of its own): dark, muted
  // text, focused or not.
  const fg = (sel) => page.locator(sel).evaluate((el) => getComputedStyle(el).color);
  const idleText = await fg('a.leaflet-control-zoom-out');
  await page.evaluate(() => window.skycueApp.setView(38.9, -77.0, 14));
  await expect(page.locator('a.leaflet-control-zoom-in')).toHaveClass(/leaflet-disabled/);
  expect(await bg('a.leaflet-control-zoom-in')).toBe(zoom);
  expect(await fg('a.leaflet-control-zoom-in')).not.toBe(idleText);
  await page.locator('a.leaflet-control-zoom-in').focus();
  expect(await bg('a.leaflet-control-zoom-in')).toBe(zoom);
  expect(await fg('a.leaflet-control-zoom-in')).not.toBe(idleText);
});

// The main daemon has no radar or raster: the opacity slider has nothing to
// act on, so it is disabled and its row dimmed.
test('radar_opacity_disabled_without_products', async ({ page }) => {
  await openPage(page);
  await expect(page.locator('label[for="radar-opacity"]')).toHaveText('Radar opacity');
  await expect(page.locator('#radar-opacity')).toBeDisabled();
  await expect(page.locator('#radar-opacity-row')).toHaveClass(/\bdisabled\b/);
});

test.describe('fisb radar (tg30)', () => {
  test.skip(!RADAR_BASE, 'FIS-B captures not fetched: run scripts/fetch-test-captures.sh');
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

  // One slider for the NEXRAD layers and the other rasters alike, kept in
  // this browser across a reload.
  test('radar_opacity_reaches_rasters_and_persists', async ({ page }) => {
    await openFisbPage(page);
    const slider = page.locator('#radar-opacity');
    await expect(slider).toBeEnabled();
    await expect(page.locator('#radar-opacity-row')).not.toHaveClass(/\bdisabled\b/);
    await layerToggle(page, 'Other rasters').check();
    await page.locator('#raster-select').selectOption('70');
    await page.locator('#raster-band').selectOption('0');
    const raster = page.locator('img.leaflet-image-layer.fisb-raster');
    await expect(raster).toHaveCount(1);
    const regional = page.locator('img.leaflet-image-layer.fisb-radar-regional');
    await expect(regional).toHaveCount(1);
    const opacity = (loc) => () => loc.evaluate((el) => Number(getComputedStyle(el).opacity));
    await slider.fill('0.4');
    await expect.poll(opacity(raster)).toBeCloseTo(0.4, 2);
    await expect.poll(opacity(regional)).toBeCloseTo(0.4, 2);
    expect(await page.evaluate(() => localStorage.getItem('skycue.radarOpacity'))).toBe('0.4');
    await page.reload();
    await openFisbPage(page);
    await expect(slider).toHaveValue('0.4');
    await expect(regional).toHaveCount(1);
    await expect.poll(opacity(regional)).toBeCloseTo(0.4, 2);
  });

  test('radar_opacity_follows_layer_toggles', async ({ page }) => {
    await openFisbPage(page);
    const slider = page.locator('#radar-opacity');
    await expect(slider).toBeEnabled();
    await layerToggle(page, 'Radar (regional)').uncheck();
    await layerToggle(page, 'Radar (CONUS)').uncheck();
    await expect(slider).toBeDisabled();
    await expect(page.locator('#radar-opacity-row')).toHaveClass(/\bdisabled\b/);
    await layerToggle(page, 'Other rasters').check();
    await page.locator('#raster-select').selectOption('70');
    await page.locator('#raster-band').selectOption('0');
    await expect(page.locator('img.leaflet-image-layer.fisb-raster')).toHaveCount(1);
    await expect(slider).toBeEnabled();
    await layerToggle(page, 'Other rasters').uncheck();
    await expect(slider).toBeDisabled();
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
    await expect.poll(() => sel.locator('option').count()).toBeGreaterThanOrEqual(4);
    await sel.selectOption('70');
    await page.locator('#raster-band').selectOption('0');
    await expect(page.locator('img.leaflet-image-layer.fisb-raster')).toHaveCount(1);
    await expect(page.locator('#raster-legend')).toContainText('Icing');
    await expect(page.locator('#raster-legend')).toContainText('approximate');
    await sel.selectOption('103');
    await expect(page.locator('#raster-legend')).toContainText(/lightning/i);
    await layerToggle(page, 'Other rasters').uncheck();
    await expect(page.locator('img.fisb-raster')).toHaveCount(0);
  });
  test('raster_band_select', async ({ page }) => {
    await openFisbPage(page);
    await layerToggle(page, 'Other rasters').check();
    await page.locator('#raster-select').selectOption('70');
    const band = page.locator('#raster-band');
    await expect(band).toBeVisible();
    const status = (await apiAt(RADAR_BASE, '/api/fisb/status')).body;
    const codes = status.rasters.filter((h) => h.product === 70).map((h) => h.altitude_code).sort((a, b) => a - b);
    await expect(band).toHaveValue(String(codes[0]));
    await expect(page.locator('#raster-legend')).toContainText('≈ ' + (2000 + 2000 * codes[0]).toLocaleString('en-US') + ' ft');
    await page.locator('#raster-select').selectOption('84');
    await expect(band).toBeHidden();
  });
  test('winds_and_raster_picks_survive_items_and_reconnect', async ({ page }) => {
    const errors = watchErrors(page);
    await openFisbPage(page);
    await page.locator('#wx-toggle').click();
    await layerToggle(page, 'Other rasters').check();
    const status = (await apiAt(RADAR_BASE, '/api/fisb/status')).body;
    const codes = status.rasters.filter((h) => h.product === 70).map((h) => h.altitude_code).sort((a, b) => a - b);
    expect(codes.length).toBeGreaterThan(1);
    const top = String(codes[codes.length - 1]);   // not the default (the lowest)
    await page.locator('#raster-select').selectOption('70');
    await page.locator('#raster-band').selectOption(top);
    await expect.poll(() => page.evaluate(() => window.skycueApp.windsPickers().period)).toBe(6);
    await page.locator('#wx-winds-period').selectOption('12');
    await page.locator('#wx-winds-level').selectOption('39000');
    const picks = async () => [await page.locator('#raster-select').inputValue(), await page.locator('#raster-band').inputValue(),
      await page.evaluate(() => { const w = window.skycueApp.windsPickers(); return [w.period, w.level]; })];
    const want = ['70', top, [12, 39000]];
    expect(await picks()).toEqual(want);
    // Unrelated items leave the picks alone.
    const metar = (await apiAt(RADAR_BASE, '/api/fisb/text')).body.records.find((r) => r.kind === 'metar');
    await page.evaluate((m) => window.skycueApp.injectFisbItems([{ type: 'fisb_location', id: 'K2G4', lat: 39.6, lon: -86.0 },
      { type: 'fisb_text', record: m }]), metar);
    await page.waitForTimeout(500);
    expect(await picks()).toEqual(want);
    // A reconnect empties the data set while it reloads (held here for a
    // while): the picks outlive it.
    let release;
    const held = new Promise((ok) => { release = ok; });
    await page.route('**/api/fisb/text*', async (route) => { await held; await route.continue(); });
    const n = await page.evaluate(() => window.skycueApp.snapshots());
    await page.evaluate(() => window.skycueApp.reconnect());
    await expect.poll(() => page.evaluate(() => window.skycueApp.snapshots())).toBeGreaterThan(n);
    await page.waitForTimeout(1500);
    release();
    await page.waitForFunction(() => window.skycueApp.fisbLoaded());
    await expect.poll(picks).toEqual(want);
    await page.unroute('**/api/fisb/text*');
    expect(errors).toEqual([]);
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
    // The Weather tab with the report symbols on the map and one report card
    // open (SIGMET off so no shape lies over the symbol clicked).
    await layerToggle(page, 'SIGMET').uncheck();
    await expect.poll(() => page.locator('.wx-sym.wx-metar').count()).toBeGreaterThan(0);
    await expect.poll(() => page.locator('.wx-sym.wx-winds').count()).toBeGreaterThan(0);
    await page.locator('.wx-sym.wx-metar[data-station="KCVG"]').click();
    await expect(page.locator('#shape-title')).toHaveText('METAR KCVG');
    await page.waitForTimeout(500);
    await page.screenshot({ path: testInfo.outputPath(`fisb-symbols-${testInfo.project.name}.png`), fullPage: true });
  });

  test('metar_taf_symbols_have_category_classes', async ({ page }) => {
    const errors = watchErrors(page);
    await openFisbPage(page);
    const sym = (k, st) => page.locator(`.wx-sym.wx-${k}[data-station="${st}"]`);
    await expect(sym('metar', 'KCVG')).toHaveClass(/\bcat-mvfr\b/);     // OVC019
    await expect(sym('taf', 'KCVG')).toHaveClass(/\bcat-mvfr\b/);       // first group OVC025
    await expect(sym('metar', 'KPRG')).toHaveClass(/\bcat-vfr\b/);      // 10SM CLR, no wind group
    await expect(sym('metar', 'KAID')).toHaveClass(/\bcat-mvfr\b/);     // OVC012
    const s = await page.evaluate(() => window.skycueApp.wxSymbols());
    expect(s.find((x) => x.key === 'metar:KPRG').barb).toBeNull();
    expect(s.find((x) => x.key === 'metar:KCVG').barb).toEqual({ dir: 330, kt: 11 });
    const pos = await page.evaluate(() => window.skycueApp.wxPositioned());
    expect(pos.n).toBeGreaterThanOrEqual(40); expect(pos.n).toBeLessThan(pos.m);
    await page.locator('#wx-toggle').click();
    await expect(page.locator('#wx-positioned')).toHaveText(`${pos.n} of ${pos.m} positioned`);
    await expect(page.locator('#wx-list li.wx-row[data-key="metar:KCVG"] .wx-show')).toHaveCount(1);
    const unplotted = page.locator('#wx-list li.wx-row[data-key="metar:K2G4"]');   // no NOTAM point names K2G4 in tg30
    await expect(unplotted.locator('.wx-nopos')).toHaveText('no position');
    await expect(sym('metar', 'K2G4')).toHaveCount(0);
    expect(errors).toEqual([]);
  });
  test('symbol_click_opens_report_card', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('.wx-sym.wx-metar[data-station="KCVG"]').click();
    await expect(page.locator('#shape-details')).toBeVisible();
    await expect(page.locator('#shape-title')).toHaveText('METAR KCVG');
    await expect(page.locator('#shape-details .shape-text')).toContainText('METAR KCVG 300852Z');
    await expect(page.locator('#shape-fields')).toContainText('MVFR');
  });
  // While the ruler is armed a weather symbol or an airspace shape is a
  // ruler point, not a click that opens the Weather tab.
  test('ruler_takes_symbol_and_shape_clicks', async ({ page }) => {
    await openFisbPage(page);
    const sym = page.locator('.wx-sym.wx-metar[data-station="KCVG"]');
    await expect(sym).toBeVisible();
    const pos = (await page.evaluate(() => window.skycueApp.wxSymbols())).find((x) => x.kind === 'metar' && x.station === 'KCVG');
    const ruler = () => page.evaluate(() => window.skycueApp.ruler());
    await page.locator('#ruler-toggle').click();
    await sym.click();
    let r = await ruler();
    expect(Math.abs(r.start.lat - pos.lat)).toBeLessThan(1e-6);
    expect(Math.abs(r.start.lon - pos.lon)).toBeLessThan(1e-6);
    await expect(page.locator('#details')).toBeHidden();
    const key = '8-20-997-ZID-1';
    const sq = [[pos.lon + 0.6, pos.lat - 0.3], [pos.lon + 1.2, pos.lat - 0.3], [pos.lon + 1.2, pos.lat + 0.3],
      [pos.lon + 0.6, pos.lat + 0.3], [pos.lon + 0.6, pos.lat - 0.3]];
    await page.evaluate((it) => window.skycueApp.injectFisbItems([it]), {
      type: 'fisb_shape', product: 8, key, features: [{ type: 'Feature', id: key + '/0',
        geometry: { type: 'Polygon', coordinates: [sq] },
        properties: { key, product: 8, name: 'NOTAM', text: 'RULER TEST', geom: 'polygon', cancelled: false, agl: false, shape: 0 } }]
    });
    await layerToggle(page, 'NOTAM / TFR').check();
    const shape = page.locator(`path.fisb-tfr[data-key="${key}"]`);
    await expect(shape).toHaveCount(1);
    await shape.click();
    r = await ruler();
    expect(r.fixed).toBe(true);
    expect(r.label).toMatch(/ nm · .* km · \d{3}°T$/);
    await expect(page.locator('#details')).toBeHidden();
    await page.keyboard.press('Escape');
    expect((await ruler()).armed).toBe(false);
    // Disarmed, the same click opens the shape again.
    await shape.click();
    await expect(page.locator('#shape-details')).toBeVisible();
    await layerToggle(page, 'NOTAM / TFR').uncheck();
  });
  test('reports_toggle_and_filter_apply_to_symbols', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('#wx-toggle').click();
    await page.locator('#wx-show-metar').uncheck();
    await expect(page.locator('.wx-sym.wx-metar')).toHaveCount(0);
    await expect(page.locator('#wx-list li.wx-row[data-kind="metar"]')).toHaveCount(0);
    await expect(page.locator('.wx-sym.wx-taf').first()).toBeVisible();
    await page.locator('#wx-show-metar').check();
    await expect(page.locator('.wx-sym.wx-metar[data-station="KCVG"]')).toHaveCount(1);
    await page.locator('#wx-filter').fill('KCV');
    await expect.poll(() => page.locator('.wx-sym.wx-metar').count(), { timeout: 3000 }).toBe(1);
    await page.locator('#wx-filter').fill('');
    await expect.poll(() => page.locator('.wx-sym.wx-metar').count(), { timeout: 3000 }).toBeGreaterThanOrEqual(40);
  });
  test('late_location_adds_symbol', async ({ page }) => {
    await openFisbPage(page);
    // Inside the view (the receiver's 150 nm ring around central Indiana).
    await page.evaluate(() => window.skycueApp.injectFisbItems([{ type: 'fisb_location', id: 'K2G4', lat: 39.6, lon: -86.0 }]));
    await expect(page.locator('.wx-sym.wx-metar[data-station="K2G4"]')).toHaveClass(/\bcat-lifr\b/, { timeout: 3000 });
    await page.locator('#wx-toggle').click();
    await expect(page.locator('#wx-list li.wx-row[data-key="metar:K2G4"] .wx-nopos')).toHaveCount(0);
  });
  test('symbols_culled_to_the_view', async ({ page }) => {
    await openFisbPage(page);
    const n0 = (await page.evaluate(() => window.skycueApp.wxPositioned())).n;
    // Far east of the view (off the New England coast): positioned, not drawn.
    await page.evaluate(() => window.skycueApp.injectFisbItems([{ type: 'fisb_location', id: 'K2G4', lat: 39.0, lon: -72.0 }]));
    await expect.poll(() => page.evaluate(() => window.skycueApp.wxPositioned().n)).toBe(n0 + 1);
    await page.waitForTimeout(1500);   // past the 1 s redraw
    const sym = page.locator('.wx-sym.wx-metar[data-station="K2G4"]');
    await expect(sym).toHaveCount(0);
    await expect(page.locator('.wx-sym.wx-metar[data-station="KCVG"]')).toHaveCount(1);
    await page.evaluate(() => window.skycueApp.setView(39.0, -72.0, 8));
    await expect(sym).toHaveClass(/\bcat-lifr\b/, { timeout: 3000 });
    await expect(page.locator('.wx-sym.wx-metar[data-station="KCVG"]')).toHaveCount(0);
  });
  test('winds_without_a_symbol_say_no_barb', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('#wx-toggle').click();
    // CVG has 6 h and 12 h records: the 6 h one (3000 ft) is drawn, the 12 h one is not.
    await expect(page.locator('#wx-list li.wx-row[data-key="winds:CVG:6"] .wx-show')).toHaveCount(1);
    await expect(page.locator('#wx-list li.wx-row[data-key="winds:CVG:12"] .wx-nobarb')).toHaveText('no barb');
    await expect(page.locator('#wx-list li.wx-row[data-key="winds:CVG:12"] .wx-show')).toHaveCount(0);
    await expect(page.locator('.wx-sym.wx-winds[data-station="CVG"]')).toHaveCount(1);
    const s = await page.evaluate(() => window.skycueApp.wxSymbols());
    const pos = await page.evaluate(() => window.skycueApp.wxPositioned());
    expect(pos.n).toBe(s.length);   // every positioned report is drawn (all are in view in tg30)
  });
  test('winds_pickers_change_barbs', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('#wx-toggle').click();
    const barbs = () => page.evaluate(() => window.skycueApp.wxSymbols().filter((s) => s.kind === 'winds')
      .map((s) => [s.station, s.barb && s.barb.dir, s.barb && s.barb.kt]).sort());
    await expect.poll(() => page.evaluate(() => window.skycueApp.windsPickers().period)).toBe(6);
    await expect(page.locator('#wx-winds-level')).toHaveValue('3000');
    await expect.poll(barbs).toEqual([['CVG', 350, 17], ['IND', 350, 16]]);   // KCVG, KIND learned; FWA has no 6 h record
    await page.locator('#wx-winds-level').selectOption('39000');
    await expect.poll(barbs).toEqual([['CVG', 290, 92], ['IND', 300, 99]]);
    await page.locator('#wx-winds-period').selectOption('12');
    await expect.poll(barbs).toEqual([['CVG', 280, 85], ['FWA', 300, 100], ['IND', 290, 83]]);
    const levels = await page.locator('#wx-winds-level option').evaluateAll((os) => os.map((o) => Number(o.value)));
    expect(levels).toEqual([3000, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000]);
  });
  test('report_card_follows_its_own_record', async ({ page }) => {
    await openFisbPage(page);
    const rec = (await apiAt(RADAR_BASE, '/api/fisb/text')).body.records.find((r) => r.key === 'metar:KAID');
    const card = page.locator('#shape-details');
    const inject = (items) => page.evaluate((it) => window.skycueApp.injectFisbItems(it), items);
    // A report, then an airspace shape: the report's expiry leaves the shape card alone.
    await page.locator('.wx-sym.wx-metar[data-station="KCVG"]').click();
    await expect(page.locator('#shape-title')).toHaveText('METAR KCVG');
    await layerToggle(page, 'NOTAM / TFR').check();
    await expect.poll(() => page.locator('path.fisb-tfr').count()).toBeGreaterThan(0);
    const i = await clickableIndex(page, 'path.fisb-tfr');
    expect(i).toBeGreaterThanOrEqual(0);
    await page.locator('path.fisb-tfr').nth(i).click();
    await expect(page.locator('#shape-title')).not.toHaveText('METAR KCVG');
    const shapeTitle = await page.locator('#shape-title').textContent();
    await inject([{ type: 'fisb_expired', kind: 'text', key: 'metar:KCVG' }]);
    await page.waitForTimeout(300);
    await expect(card).toBeVisible();
    await expect(page.locator('#shape-title')).toHaveText(shapeTitle);
    await layerToggle(page, 'NOTAM / TFR').uncheck();
    // A report card is re-rendered when its record is updated, kept on another's
    // expiry and closed on its own.
    await page.locator('.wx-sym.wx-metar[data-station="KAID"]').click();
    await expect(page.locator('#shape-title')).toHaveText('METAR KAID');
    const text = rec.text.replace(/ OVC012 /, ' OVC003 ');
    await inject([{ type: 'fisb_text', record: Object.assign({}, rec, { text }) }]);
    await expect(card.locator('.shape-text')).toContainText('OVC003');
    await expect(page.locator('#shape-fields [data-field="category"]')).toHaveText('LIFR');
    await inject([{ type: 'fisb_expired', kind: 'text', key: 'metar:KPRG' }]);
    await page.waitForTimeout(300);
    await expect(card).toBeVisible();
    await inject([{ type: 'fisb_expired', kind: 'text', key: 'metar:KAID' }]);
    await expect(card).toBeHidden();
  });
  test('symbols_not_duplicated_after_reset', async ({ page }) => {
    await openFisbPage(page);
    await expect.poll(() => page.locator('.wx-sym.wx-metar[data-station="KCVG"]').count()).toBe(1);
    const n0 = await page.evaluate(() => window.skycueApp.snapshots());
    await page.evaluate(() => window.skycueApp.reconnect());
    await page.waitForFunction((n) => window.skycueApp.snapshots() > n && window.skycueApp.fisbLoaded(), n0, { timeout: 15000 });
    // One marker per symbol key and nothing left over from before the reset,
    // whatever arrived or expired meanwhile.
    await expect.poll(async () => {
      const keys = await page.evaluate(() => window.skycueApp.wxSymbols().map((s) => s.key));
      const dom = await page.locator('.wx-sym').count();
      return keys.length > 0 && new Set(keys).size === keys.length && dom === keys.length;
    }).toBe(true);
    await expect(page.locator('.wx-sym.wx-metar[data-station="KCVG"]')).toHaveCount(1);
  });
});

test.describe('fisb text and airspace (2015 sample)', () => {
  test.skip(!FISB_BASE, 'FIS-B captures not fetched: run scripts/fetch-test-captures.sh');
  test.use({ baseURL: FISB_BASE });

  test('header_fits_one_row_at_1280_with_fisb_stats', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await openFisbPage(page);
    await expect(page.locator('#stat-fisb-stations')).not.toHaveText('–');
    for (const d of await headerRowOffsets(page, '99')) expect(d).toBeLessThan(4);
    for (const width of [1401, 1600, 1700]) {
      await page.setViewportSize({ width, height: 800 });
      for (const d of await headerRowOffsets(page, '99')) expect(d, `at ${width} px`).toBeLessThan(4);
    }
  });

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
// meta.json says zoom 0..6: 2/1/1 in four coloured quadrants, one native
// zoom-6 tile, 6/18/24 (the receiver's), transparent in its west half, on
// the edge of its region, and its parent 5/9/12, half transparent and in no
// region. Every other zoom-3..6 tile is missing.
const TILES_BASE = process.env.SKYCUE_TILES_URL;

test.describe('map tiles fallback', () => {
  test.use({ baseURL: TILES_BASE });

  test('missing_tile_shows_lower_zoom_fallback', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    expect((await apiAt(TILES_BASE, '/api/tiles-available')).body).toEqual({ tiles: true, minZoom: 0, maxZoom: 6, overlays: true });
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
    let fallbacks = 0, siblings = 0;
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
      } else if (x >> 1 === 9 && y >> 1 === 12) {
        // A sibling: enlarged 5/9/12, itself an edge tile, over 2/1/1.
        expect(t.fallback, t.tile).toBe('5');
        expect(t.underlay, t.tile).toBe('2');
        expect(t.imgs.length).toBe(2);
        expect(t.imgs[0], t.tile).toMatchObject(Object.assign({ underlay: true }, parent));
        expect(t.imgs[1], t.tile).toMatchObject({ src: 'tiles/5/9/12.png', natural: 256, width: px(512),
          left: px(-(x % 2) * 256), top: px(-(y % 2) * 256), underlay: false });
        siblings++;
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
    expect(siblings).toBeGreaterThanOrEqual(1);
    expect(offsets.size).toBe(fallbacks); // each at its own place in the parent
    const counts = await page.evaluate(() => window.skycueApp.tileCounts());
    expect(counts.underlay).toBeGreaterThanOrEqual(1 + siblings);
    // The first view's tiles that Leaflet pruned before they loaded are not failures.
    expect(counts.failed).toBe(0);
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

// ---- Receiver tab ----------------------------------------------------------------
// Against the "receivers" skycued run.sh starts: no built-in decoder inputs,
// one pingstation_json input ps-sim that skycue-pingsim feeds (a device
// status every 2 s at 38.9 N 77.0 W). Every case leaves only ps-sim, enabled.
const RX_BASE = process.env.SKYCUE_RECEIVERS_URL;
const dgram = require('dgram');

async function rxApi(method, path, body) {
  const init = { method, headers: {} };
  if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }
  const r = await fetch(RX_BASE + path, init);
  const text = await r.text();
  return { status: r.status, body: text ? JSON.parse(text) : null };
}
const rxInputs = async () => (await rxApi('GET', '/api/inputs')).body.inputs;
// ps-sim's definition as run.sh wrote it (a case may remove and re-add it).
let psSimDef = null;
function definition(input) {
  const def = Object.assign({}, input);
  delete def.state;
  delete def.builtin;
  return def;
}
async function rxRestore() {
  for (const i of await rxInputs()) {
    if (i.id !== 'ps-sim') await rxApi('DELETE', '/api/inputs/' + i.id);
    else if (JSON.stringify(definition(i)) !== JSON.stringify(psSimDef)) await rxApi('PUT', '/api/inputs/ps-sim', psSimDef);
  }
  if (!(await rxInputs()).some((i) => i.id === 'ps-sim')) await rxApi('POST', '/api/inputs', psSimDef);
}
// A UDP port nothing listens on now.
function freeUdpPort() {
  return new Promise((resolve, reject) => {
    const s = dgram.createSocket('udp4');
    s.on('error', reject);
    s.bind(0, '127.0.0.1', () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
}
async function openReceiverTab(page) {
  await openPage(page);
  await page.locator('#stat-inputs').click();
  await expect(page.locator('#pane-receiver')).toBeVisible();
}
const inputCard = (page, id) => page.locator(`#inputs-list .input-card[data-id="${id}"]`);
const isPost = (r) => r.method() === 'POST' && /\/api\/inputs$/.test(r.url());

test.describe('receiver tab (pingsim)', () => {
  test.use({ baseURL: RX_BASE });
  test.beforeAll(async () => {
    if (!psSimDef) psSimDef = definition((await rxInputs()).find((i) => i.id === 'ps-sim'));
  });
  test.afterEach(rxRestore);

  test('inputs_header_opens_receiver_tab', async ({ page }) => {
    await openPage(page);
    await expect(page.locator('#stat-inputs')).toHaveText('1/1');
    await expect(page.locator('#pane-receiver')).toBeHidden();
    await page.locator('#stat-inputs').click();
    await expect(page.locator('#pane-receiver')).toBeVisible();
    await expect(page.locator('#tab-receiver')).toHaveAttribute('aria-selected', 'true');
    await expect(page.locator('#pane-track')).toBeHidden();
    await expect(page.locator('#pane-weather')).toBeHidden();
    // The tabs switch; the header button closes the panel again.
    await page.locator('#tab-weather').click();
    await expect(page.locator('#pane-weather')).toBeVisible();
    await expect(page.locator('#pane-receiver')).toBeHidden();
    await page.locator('#tab-receiver').click();
    await expect(page.locator('#pane-receiver')).toBeVisible();
    await page.locator('#stat-inputs').click();
    await expect(page.locator('#details')).toBeHidden();
  });

  test('receiver_tab_lists_inputs_with_live_counts', async ({ page }) => {
    const errors = watchErrors(page);
    await openReceiverTab(page);
    const card = inputCard(page, 'ps-sim');
    await expect(card.locator('.dot')).toHaveClass(/\bdot-green\b/, { timeout: 10000 });
    await expect(card.locator('.input-name')).toHaveText('pingsim');
    await expect(card.locator('.input-endpoint')).toHaveText(/^udp 127\.0\.0\.1:\d+$/);
    await expect.poll(async () => Number(await card.locator('.input-rates').getAttribute('data-obs-s')),
      { timeout: 10000 }).toBeGreaterThan(0);
    await expect(card.locator('.input-rates')).toHaveText(/^obs\/s \d+\.\d · msg\/s \d+\.\d$/);
    await expect(card.locator('.input-rejected')).toHaveText('rejected 0');
    await expect(card.locator('.input-age')).toHaveText(/^data \d+ s ago$/);
    const device = card.locator('.input-device');
    await expect(device).toContainText('GPS 3D fix · receiver normal');
    await expect(device).toContainText('firmware 1.0.0');
    await expect(device).not.toHaveClass(/\bstale\b/);
    // Not a built-in: every button.
    for (const act of ['toggle', 'edit', 'remove', 'show']) await expect(card.locator(`[data-act=${act}]`)).toBeVisible();
    // The device's position is a marker of its own, which "show" pans to.
    await expect(page.locator('.device-marker')).toHaveCount(1);
    await expect(page.locator('.rx-marker')).toHaveCount(1);
    // Show pans and keeps the zoom; from far out (below zoom 6, the device
    // out of view) it zooms in to 6.
    await page.evaluate(() => window.skycueApp.setView(30, -100, 7));
    await card.locator('[data-act=show]').click();
    await expect.poll(() => page.evaluate(() => window.skycueApp.center())).toEqual({ lat: 38.9, lon: -77 });
    expect(await page.evaluate(() => window.skycueApp.zoom())).toBe(7);
    await page.evaluate(() => window.skycueApp.setView(-30, 100, 3));
    await card.locator('[data-act=show]').click();
    await expect.poll(() => page.evaluate(() => window.skycueApp.center())).toEqual({ lat: 38.9, lon: -77 });
    expect(await page.evaluate(() => window.skycueApp.zoom())).toBe(6);
    expect(errors).toEqual([]);
  });

  test('add_input_form_validates_and_shows_server_error', async ({ page }) => {
    await openReceiverTab(page);
    const posts = [];
    page.on('request', (r) => { if (isPost(r)) posts.push(r.url()); });
    const form = page.locator('#input-form');
    await form.locator('select[name=type]').selectOption('pingstation_json');
    await expect(form.locator('[name=transport]')).toBeHidden();
    await expect(form.locator('[name=host]')).toBeHidden();
    await form.locator('[name=id]').fill('PS');
    await form.locator('[name=port]').fill('0');
    await form.locator('button[type=submit]').click();
    await expect(form.locator('.field-error[data-for=id]')).toHaveText('id must be 1 to 32 characters of a-z, 0-9 and -');
    await expect(form.locator('[name=id]')).toHaveClass(/\binvalid\b/);
    await expect(form.locator('.field-error[data-for=port]')).toHaveText('port must be an integer from 1 to 65535');
    // Fixing a field clears its mark as it is typed.
    await form.locator('[name=id]').fill('ps-clash');
    await expect(form.locator('[name=id]')).not.toHaveClass(/\binvalid\b/);
    await expect(form.locator('.field-error[data-for=id]')).toHaveText('');
    expect(posts).toEqual([]);

    // A port ps-sim listens on: skycued's 400, word for word.
    const ps = (await rxInputs()).find((i) => i.id === 'ps-sim');
    await form.locator('[name=port]').fill(String(ps.port));
    const answer = page.waitForResponse((r) => isPost(r.request()));
    await form.locator('button[type=submit]').click();
    const res = await answer;
    expect(res.status()).toBe(400);
    const message = (await res.json()).error;
    expect(message).toContain('ps-sim');
    await expect(form.locator('.form-error')).toHaveText(message);
    expect(posts.length).toBe(1);
    expect((await rxInputs()).map((i) => i.id)).toEqual(['ps-sim']);
  });

  test('add_edit_disable_remove_input', async ({ page }) => {
    await openReceiverTab(page);
    const port = await freeUdpPort();
    const form = page.locator('#input-form');
    await form.locator('select[name=type]').selectOption('pingstation_cat021');
    await form.locator('select[name=transport]').selectOption('tcp_connect');
    await expect(form.locator('[name=host]')).toBeVisible();
    await expect(form.locator('[name=bind]')).toBeHidden();
    await form.locator('select[name=transport]').selectOption('udp');
    await expect(form.locator('[name=host]')).toBeHidden();
    await expect(form.locator('[name=bind]')).toBeVisible();
    await expect(form.locator('[name=edition]')).toHaveValue('2.5');
    await form.locator('[name=id]').fill('ps-two');
    await form.locator('[name=name]').fill('Second');
    await form.locator('[name=bind]').fill('127.0.0.1');
    await form.locator('[name=port]').fill(String(port));
    await form.locator('button[type=submit]').click();
    const card = inputCard(page, 'ps-two');
    await expect(card.locator('.input-name')).toHaveText('Second');
    await expect(card.locator('.input-endpoint')).toHaveText(`udp 127.0.0.1:${port}`);
    await expect(form.locator('[name=id]')).toHaveValue(''); // the form is reset for the next one
    let two = (await rxInputs()).find((i) => i.id === 'ps-two');
    expect(two).toMatchObject({ type: 'pingstation_cat021', transport: 'udp', bind: '127.0.0.1', port, edition: '2.5',
      enabled: true });
    // Bound, nothing arriving: connected but not receiving.
    await expect(card.locator('.dot')).toHaveClass(/\bdot-amber\b/);

    await card.locator('[data-act=edit]').click();
    await expect(form.locator('[name=id]')).toHaveValue('ps-two');
    await expect(form.locator('[name=id]')).toHaveAttribute('readonly', '');
    await expect(form.locator('[name=port]')).toHaveValue(String(port));
    await form.locator('[name=name]').fill('Second mast');
    await form.locator('button[type=submit]').click();
    await expect(card.locator('.input-name')).toHaveText('Second mast');
    two = (await rxInputs()).find((i) => i.id === 'ps-two');
    expect(two).toMatchObject({ name: 'Second mast', transport: 'udp', port });

    // Disabled from its card while the form edits it: Save keeps it disabled.
    await card.locator('[data-act=edit]').click();
    await expect(form.locator('[name=enabled]')).toBeChecked();
    await card.locator('[data-act=toggle]').click();
    await expect(card.locator('.dot')).toHaveClass(/\bdot-grey\b/);
    expect((await rxInputs()).find((i) => i.id === 'ps-two').enabled).toBe(false);
    await expect(form.locator('[name=enabled]')).not.toBeChecked();
    await form.locator('[name=name]').fill('Second mast, off');
    await form.locator('button[type=submit]').click();
    await expect(card.locator('.input-name')).toHaveText('Second mast, off');
    expect((await rxInputs()).find((i) => i.id === 'ps-two')).toMatchObject({ name: 'Second mast, off', enabled: false });
    await expect(card.locator('.dot')).toHaveClass(/\bdot-grey\b/);
    await expect(page.locator('#stat-inputs')).toHaveText('1/1');

    // Remove asks first: dismissed, nothing happens.
    page.once('dialog', (d) => d.dismiss());
    await card.locator('[data-act=remove]').click();
    await page.waitForTimeout(500);
    await expect(card).toHaveCount(1);
    let asked = '';
    page.once('dialog', (d) => { asked = d.type(); d.accept(); });
    await card.locator('[data-act=remove]').click();
    await expect(card).toHaveCount(0);
    expect(asked).toBe('confirm');
    expect((await rxInputs()).map((i) => i.id)).toEqual(['ps-sim']);
  });

  // Removed and added again under the same id (on a port pingsim does not
  // send to): a new input, with none of the old one's data age or rates.
  test('removed_id_added_again_starts_fresh', async ({ page }) => {
    await openReceiverTab(page);
    const card = inputCard(page, 'ps-sim');
    await expect(card.locator('.input-age')).toHaveText(/^data \d+ s ago$/, { timeout: 10000 });
    await expect.poll(async () => Number(await card.locator('.input-rates').getAttribute('data-obs-s')),
      { timeout: 10000 }).toBeGreaterThan(0);
    expect((await rxApi('DELETE', '/api/inputs/ps-sim')).status).toBe(204);
    await expect(card).toHaveCount(0);
    const port = await freeUdpPort();
    expect((await rxApi('POST', '/api/inputs', Object.assign({}, psSimDef, { port }))).status).toBe(201);
    await expect(card).toHaveCount(1);
    await expect(card.locator('.input-age')).toHaveText('no data yet');
    await page.waitForTimeout(2500); // stats items and a poll go by
    await expect(card.locator('.input-age')).toHaveText('no data yet');
    await expect(card.locator('.input-rates')).toHaveText(/^obs\/s (—|0\.0) · msg\/s (—|0\.0)$/);
  });

  test('input_name_is_text_not_html', async ({ page }) => {
    const name = '<img src=x onerror="window.__xss=1">';
    const port = await freeUdpPort();
    expect((await rxApi('POST', '/api/inputs',
      { id: 'ps-xss', name, type: 'pingstation_json', bind: '127.0.0.1', port })).status).toBe(201);
    await openReceiverTab(page);
    const card = inputCard(page, 'ps-xss');
    await expect(card.locator('.input-name')).toHaveText(name);
    expect(await card.textContent()).toContain('<img');
    expect(await card.locator('img').count()).toBe(0);
    // A server error with markup in it is text too.
    const markup = '<img src=x onerror="window.__xss=2"> is bad';
    await page.route('**/api/inputs', (route) => (route.request().method() === 'POST' ?
      route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: markup }) }) :
      route.continue()));
    const form = page.locator('#input-form');
    await form.locator('select[name=type]').selectOption('pingstation_json');
    await form.locator('[name=id]').fill('ps-markup');
    await form.locator('[name=port]').fill(String(await freeUdpPort()));
    await form.locator('button[type=submit]').click();
    await expect(form.locator('.form-error')).toHaveText(markup);
    expect(await form.locator('img').count()).toBe(0);
    await page.waitForTimeout(500);
    expect(await page.evaluate(() => window.__xss)).toBeUndefined();
  });

  // A UDP input stays connected while its socket is bound, so the case adds
  // a tcp_listen input (connected while a peer is) and is that peer itself.
  test('alerts_input_lost_after_hold_off_and_recovers', async ({ page }) => {
    const port = await new Promise((resolve, reject) => {
      const s = net.createServer();
      s.on('error', reject);
      s.listen(0, '127.0.0.1', () => {
        const p = s.address().port;
        s.close(() => resolve(p));
      });
    });
    expect((await rxApi('POST', '/api/inputs', { id: 'ps-tcp', name: 'tcp mast', type: 'pingstation_cat021',
      transport: 'tcp_listen', bind: '127.0.0.1', port, edition: '2.5' })).status).toBe(201);
    await alertsOnly(page, ['input_lost']);
    await openPage(page);
    const tcpConnected = async () => {
      const i = (await rxInputs()).find((x) => x.id === 'ps-tcp');
      return !!(i && i.state && i.state.connected);
    };
    const socks = [];
    const connect = () => { const s = net.connect(port, '127.0.0.1'); s.on('error', () => {}); socks.push(s); return s; };
    try {
      const sock = connect();
      await expect.poll(tcpConnected, { timeout: 10000 }).toBe(true);
      // The page has seen it connected (ps-sim and ps-tcp).
      await expect(page.locator('#stat-inputs')).toHaveText('2/2', { timeout: 10000 });
      sock.destroy();
      const t0 = Date.now();
      await page.waitForTimeout(5000);
      expect((await alertState(page)).rows.filter((r) => r.rule === 'input_lost')).toEqual([]);
      await expect(page.locator('#alerts-line .al-kind')).toHaveText('Input lost', { timeout: 20000 });
      expect(Date.now() - t0).toBeGreaterThanOrEqual(10000);
      await expect(page.locator('#alerts-line .al-id')).toHaveText('tcp mast');
      await expect(page.locator('#alerts-line .al-detail')).toHaveText('disconnected');

      connect();
      const lost = async () => (await alertState(page)).rows.find((r) => r.rule === 'input_lost') || null;
      await expect.poll(async () => { const r = await lost(); return r && r.acked; }, { timeout: 15000 }).toBe(true);
      expect((await lost()).recovered).toMatch(/^recovered after \d+ s$/);
      await expect(page.locator('#alerts-count')).toHaveText('0');
    } finally {
      socks.forEach((s) => s.destroy());
    }
  });
});

test.describe('receiver tab (FIS-B, 2015 sample)', () => {
  test.skip(!FISB_BASE, 'FIS-B captures not fetched: run scripts/fetch-test-captures.sh');
  test.use({ baseURL: FISB_BASE });

  // Clicking the station marker deselects, like a map click; while the
  // ruler is armed it is a ruler point at the station instead.
  test('station_marker_click_deselects_unless_ruler_armed', async ({ page }) => {
    await openFisbPage(page);
    const st = (await apiAt(FISB_BASE, '/api/fisb/status')).body.stations[0];
    const station = page.locator('.station-marker');
    await expect(station).toHaveCount(1);
    await page.evaluate((p) => window.skycueApp.injectTestMarker({ hex: 'fff0b1', addr_type: 'icao', lat: p.lat + 0.3,
      lon: p.lon + 0.3, decision: 'send', track_deg: 0 }), st);
    const glyph = page.locator('.ac-marker[data-hex="fff0b1"] .ac-glyph');
    const selected = () => page.evaluate(() => window.skycueApp.selected());
    await glyph.click();
    await expect.poll(selected).toBe('fff0b1');
    await station.click();
    await expect.poll(selected).toBeNull();
    await glyph.click();
    await expect.poll(selected).toBe('fff0b1');
    await page.locator('#ruler-toggle').click();
    await station.click();
    const r = await page.evaluate(() => window.skycueApp.ruler());
    expect(Math.abs(r.start.lat - st.lat)).toBeLessThan(1e-6);
    expect(Math.abs(r.start.lon - st.lon)).toBeLessThan(1e-6);
    expect(await selected()).toBe('fff0b1');
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.skycueApp.removeTestMarker('fff0b1|icao'));
  });

  test('ground_stations_listed', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('#stat-inputs').click();
    const status = (await apiAt(FISB_BASE, '/api/fisb/status')).body;
    expect(status.stations.length).toBe(1);
    const row = page.locator('#stations-list li');
    await expect(row).toHaveCount(1);
    await expect(row).toContainText(status.stations[0].site_id);
    await expect(row).toContainText('37.3227, -121.7550');
    await expect(row).toContainText(`${status.stations[0].count} products`);
    await expect(page.locator('.station-marker')).toHaveCount(1);
  });

  test('decoder_health_table', async ({ page }) => {
    await openFisbPage(page);
    await page.locator('#stat-inputs').click();
    const status = (await apiAt(FISB_BASE, '/api/fisb/status')).body;
    const cell = (k) => page.locator(`#decoder-health tr[data-key="${k}"] td`);
    await expect(cell('uplink_duplicates')).toHaveText('0');
    await expect(cell('bad_frames')).toHaveText(String(status.bad_frames));
    await expect(cell('mosaic_blocks')).toHaveText(String(status.mosaic_blocks));
    await expect(cell('segments_open')).toHaveText(String(status.segments_open));
    await expect(cell('metar')).toHaveText(String(status.counts.metar));
    await expect(cell('last_uplink_time')).toHaveText('2015-01-24 04:23:43Z');
    await expect(cell('recorder_dropped')).toHaveText('0 / 0');
  });
});

test('no_overlay_meta_request_without_overlays', async ({ page }) => {
  const asked = [];
  page.on('request', (r) => { if (/\/overlays\/meta\.json$/.test(r.url())) asked.push(r.url()); });
  expect((await api('/api/tiles-available')).body.overlays).toBe(false);
  await openPage(page);
  expect(asked).toEqual([]);
});

// The left panel: Output filters (what is sent to cue clients, with the allow
// and block lists), then Display filters (this browser only), then the legend.
// It scrolls on its own and nothing in it is wider than the panel.
test('left_panel_groups_output_then_display_filters', async ({ page }, testInfo) => {
  await openPage(page);
  await page.locator('#filters').screenshot({ path: testInfo.outputPath(`skycue-panel-${testInfo.project.name}.png`) });
  const sections = await page.$$eval('#filters > section', (ss) => ss.map((s) => ({
    id: s.id, cls: s.className,
    h2: s.querySelector(':scope > h2') ? s.querySelector(':scope > h2').firstChild.textContent.trim() : null,
  })));
  expect(sections).toEqual([
    { id: 'output-filters', cls: '', h2: 'Output filters' },
    { id: 'view', cls: '', h2: 'Display filters' },
    { id: '', cls: 'legend', h2: null },
  ]);
  await expect(page.locator('#output-filters form.list-add[data-list="allow"]')).toHaveCount(1);
  await expect(page.locator('#output-filters form.list-add[data-list="block"]')).toHaveCount(1);
  await expect(page.locator('#output-filters #save-baseline')).toHaveCount(1);
  await expect(page.locator('#view #hide-filtered')).toHaveCount(1);
  await expect(page.locator('#view #hidden-count')).toHaveCount(1);
  const fit = await page.$eval('#filters', (f) => ({
    overflowX: f.scrollWidth - f.clientWidth,
    scrollable: f.scrollHeight > f.clientHeight ? getComputedStyle(f).overflowY : 'fits',
  }));
  expect(fit.overflowX).toBeLessThanOrEqual(0);
  expect(['auto', 'scroll', 'fits']).toContain(fit.scrollable);
  // Scrolled to the end, the legend comes into view.
  await page.$eval('#filters', (f) => { f.scrollTop = f.scrollHeight; });
  await expect(page.locator('#filters .legend')).toBeInViewport();
  await page.locator('#filters').screenshot({ path: testInfo.outputPath(`skycue-panel-end-${testInfo.project.name}.png`) });
});

// ---- View section (this browser only): narrows the table ---------------------
// Fixture facts (tests/fixtures/readsb-sample.jsonl, looped every ~122 s):
// 12 aircraft, all at US addresses except the TIS-B track ~2c4a1f, which
// skycued reports as hex 2c4a1f, addr_type non_icao (no country). RPA4521 (ac5e81) is heard only in the first
// 59 s of each loop and times out (60 s) for a few seconds before the loop
// restarts, so exact counts that would include it are avoided.
async function tableRows(page) {
  return page.$$eval('#table-view tbody tr', (trs) => trs.map((tr) => tr.getAttribute('data-key')));
}
async function openTable(page) {
  await openPage(page); await waitMarkers(page, 10);
  await page.click('#table-toggle');
  await expect.poll(() => page.$$eval('#table-view tbody tr', (r) => r.length)).toBeGreaterThanOrEqual(10);
}
const TISB = '2c4a1f|non_icao';
const hexes = (keys) => keys.map((k) => k.split('|')[0]).sort();
test('view_search_narrows_table_and_shows_count', async ({ page }) => {
  await openTable(page);
  await page.fill('#view-search', 'ual');
  await expect.poll(async () => (await tableRows(page)).length).toBe(2);           // UAL1423, UAL455
  // M (all tracks) moves while RPA4521 times out, so it is not pinned here.
  await expect(page.locator('#view-count')).toHaveText(/^showing 2 of \d+$/);
  await expect(page.locator('#view-badge')).toHaveText('· 1');
  await expect(page.locator('#view-clear')).toBeEnabled();
  await page.click('#view-clear');
  await expect.poll(() => page.evaluate(() => document.querySelectorAll('#table-view tbody tr').length === window.skycueApp.shownCount())).toBe(true);
  expect(await page.evaluate(() => window.skycueApp.shownCount())).toBeGreaterThanOrEqual(10);
  await expect(page.locator('#view-count')).toHaveText('');
  await expect(page.locator('#view-badge')).toHaveText('');
  await expect(page.locator('#view-clear')).toBeDisabled();
  await expect(page.locator('#view-search')).toHaveValue('');
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_count_stays_live_with_the_table_closed', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.fill('#view-search', 'ual');
  await expect(page.locator('#view-count')).toHaveText(/^showing 2 of \d+$/);
  expect(await page.evaluate(() => window.skycueApp.shownCount())).toBe(2);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_altitude_band_and_ground', async ({ page }) => {
  await openTable(page);
  await page.fill('#view-alt-min', '10000');
  // At or above 10,000 ft in the fixture: a2f7b9 FDX1520 24,000, a4f2c1
  // UAL1423 36,000, a6e9d2 JBU1187 38,000, a8c3e5 DAL872 28,000, ab12f0
  // AAL2291 12,000-15,600, and ac5e81 RPA4521 18,000: 6 (5 while RPA4521
  // is timed out, see above).
  const ALWAYS = ['a2f7b9', 'a4f2c1', 'a6e9d2', 'a8c3e5', 'ab12f0'];
  const WITH_RPA = ALWAYS.concat(['ac5e81']).sort();
  await expect.poll(async () => {
    const h = hexes(await tableRows(page)).join();
    return h === ALWAYS.join() || h === WITH_RPA.join();
  }).toBe(true);
  await page.fill('#view-alt-min', '');
  await page.uncheck('#view-ground');
  // The fixture's on-ground aircraft is UAL455 (a7d3e6); inject one more.
  await page.evaluate(() => window.skycueApp.injectTestMarker({ hex: 'abcdef', decision: 'send', on_ground: true, alt_baro_ft: 0, lat: 38.9, lon: -77.0, position_time: new Date().toISOString() }));
  await expect.poll(async () => hexes(await tableRows(page)).filter((h) => h === 'abcdef' || h === 'a7d3e6')).toEqual([]);
  await expect(page.locator('#view-badge')).toHaveText('· 1');
  await page.check('#view-ground');
  await expect.poll(async () => (await tableRows(page)).some((k) => k.indexOf('abcdef') >= 0)).toBe(true);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_class_emergency_category_country', async ({ page }) => {
  await openTable(page);
  await page.click('#view-classes button[data-class="airline"]');          // off
  await expect(page.locator('#view-classes button[data-class="airline"]')).toHaveAttribute('aria-pressed', 'false');
  await expect.poll(async () => (await tableRows(page)).some((k) => k.indexOf(TARGET) >= 0)).toBe(false);
  await page.click('#view-classes button[data-class="airline"]');          // on again
  await expect(page.locator('#view-classes button[data-class="airline"]')).toHaveAttribute('aria-pressed', 'true');
  await page.selectOption('#view-category', 'A1');
  await expect.poll(async () => (await tableRows(page)).length).toBe(2);   // N512RG, N3157K
  await page.selectOption('#view-category', '');
  await page.selectOption('#view-country', 'Ireland');
  await expect.poll(async () => (await tableRows(page)).length).toBe(0);
  await page.selectOption('#view-country', 'United States');
  // All 11 ICAO-addressed aircraft (10 while RPA4521 is timed out); never
  // the TIS-B track, which has no country.
  await expect.poll(async () => (await tableRows(page)).length).toBeGreaterThanOrEqual(10);
  expect(await tableRows(page)).not.toContain(TISB);
  await page.selectOption('#view-country', 'other');
  await expect.poll(async () => await tableRows(page)).toEqual([TISB]);
  await page.selectOption('#view-country', '');
  await page.evaluate(() => window.skycueApp.injectTestMarker({ hex: 'ae0001', decision: 'send', squawk: '7700', alt_baro_ft: 5000, lat: 38.9, lon: -77.0, position_time: new Date().toISOString() }));
  await page.check('#view-emergency');
  await expect.poll(async () => hexes(await tableRows(page))).toEqual(['ae0001']);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_decision_pill_narrows_table', async ({ page }) => {
  await openTable(page);
  // A known non-sent track, so the check below cannot pass on an empty table.
  await page.evaluate(() => window.skycueApp.injectTestMarker({ hex: 'abc001', decision: 'blocked', alt_baro_ft: 5000, lat: 38.9, lon: -77.0, position_time: new Date().toISOString() }));
  const pill = page.locator('#view-decisions button[data-decision="sent"]');
  await expect(pill).toHaveAttribute('aria-pressed', 'true');
  await pill.click();
  await expect(pill).toHaveAttribute('aria-pressed', 'false');
  await expect.poll(() => page.$$eval('#table-view tbody tr', (trs) => ({
    rows: trs.length,
    sent: trs.filter((tr) => tr.querySelector('td[data-key="decision"] .decision-sent')).length,
    blocked: trs.some((tr) => tr.getAttribute('data-key').indexOf('abc001') === 0)
  }))).toEqual({ rows: expect.any(Number), sent: 0, blocked: true });
  expect((await tableRows(page)).length).toBeGreaterThan(0);
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().decisions)).not.toContain('sent');
  await page.evaluate(() => window.skycueApp.resetViewFilters());
  await expect(pill).toHaveAttribute('aria-pressed', 'true');
});
test('view_persists_across_reload_and_slash_focuses_search', async ({ page }) => {
  await openTable(page);
  await page.fill('#view-search', 'dal');
  await page.check('input[name="view-map-mode"][value="dim"]');
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.view')))).toMatchObject({ search: 'dal', mapMode: 'dim' });
  await page.reload(); await openPage(page); await waitMarkers(page, 1);
  await expect(page.locator('#view-search')).toHaveValue('dal');
  await expect(page.locator('input[name="view-map-mode"][value="dim"]')).toBeChecked();
  await expect(page.locator('#view-badge')).toHaveText('· 1');
  await page.locator('body').press('/');
  await expect(page.locator('#view-search')).toBeFocused();
  await expect(page.locator('#view-search')).toHaveValue('dal');            // the '/' is not typed
  await page.keyboard.press('Escape');
  await expect(page.locator('#view-search')).toHaveValue('');
  await expect(page.locator('#view-search')).not.toBeFocused();
  await expect(page.locator('#view-badge')).toHaveText('');
  // Only dim left: not counted in the badge, but Clear still resets it.
  await expect(page.locator('#view-clear')).toBeEnabled();
  await page.click('#view-clear');
  await expect(page.locator('input[name="view-map-mode"][value="hide"]')).toBeChecked();
  await expect(page.locator('#view-clear')).toBeDisabled();
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_section_fits_panel_and_details_show_class_and_country', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.evaluate(() => window.skycueApp.setViewFilters({ search: 'abcdefghij', altMin: 10000, altMax: 45000, gsMin: 100, gsMax: 600, rangeMin: 10, rangeMax: 250 }));
  const over = await page.evaluate(() => { const f = document.getElementById('filters'); return f.scrollWidth - f.clientWidth; });
  expect(over).toBeLessThanOrEqual(0);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
  const panel = await selectTarget(page);
  await expect(panel.locator('[data-field="class"]')).toHaveText('airline');
  await expect(panel.locator('[data-field="country"]')).toHaveText('United States');
});

// Saved filter sets (spec §4): named Display-filter states in this browser,
// recalled from the select or with the number keys.
const setOptionTexts = (page) => page.$$eval('#view-set-select option', (os) => os.map((o) => o.text));
const setSelectedText = (page) => page.$eval('#view-set-select', (s) => s.options[s.selectedIndex].text);
const activeViewSet = (page) => page.evaluate(() => window.skycueApp.activeViewSet());
async function viewSetMenu(page, act) {
  await page.click('#view-set-more');
  await expect(page.locator('#view-set-menu')).toBeVisible();
  await page.click(`#view-set-menu [data-act="${act}"]`);
}
test('view_sets_save_apply_modified_rename_delete_keys', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 1);
  await expect(page.locator('#view-set-delete')).toBeDisabled();
  await page.evaluate(() => window.skycueApp.setViewFilters({ search: 'ual' }));
  let question = null;
  page.once('dialog', (d) => { question = d.message(); d.accept('Airlines'); });
  await page.click('#view-set-save');
  expect(question).toBe('Name for this filter set');
  expect(await setSelectedText(page)).toBe('Airlines');
  await expect(page.locator('#view-set-delete')).toBeEnabled();
  const ual = await page.evaluate(() => window.skycuePure.normalizeView({ search: 'ual' }));
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.viewSets'))))
    .toEqual({ version: 1, sets: [{ name: 'Airlines', view: ual, hideFiltered: false }] });
  await page.evaluate(() => window.skycueApp.setViewFilters({ search: 'dal' }));
  expect(await setSelectedText(page)).toBe('Airlines (modified)');
  await page.evaluate(() => {
    window.skycueApp.resetViewFilters();
    window.skycueApp.setViewFilters({ emergency: true });
    window.skycueApp.setHideFiltered(true);
  });
  page.once('dialog', (d) => { question = d.defaultValue(); d.accept('Emerg'); });
  await page.click('#view-set-save');
  expect(question).toBe('Airlines');                                     // the prompt offers the active set's name
  expect(await setSelectedText(page)).toBe('Emerg');
  expect(await activeViewSet(page)).toBe(1);

  await page.locator('body').press('1');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().search)).toBe('ual');
  expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(false);
  expect(await setSelectedText(page)).toBe('Airlines');
  await page.locator('body').press('2');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().emergency)).toBe(true);
  expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(true);
  await expect(page.locator('#hide-filtered')).toBeChecked();
  await page.locator('body').press('0');
  const defaults = await page.evaluate(() => window.skycuePure.VIEW_DEFAULTS);
  expect(await page.evaluate(() => window.skycueApp.getViewFilters())).toEqual(defaults);
  await expect(page.locator('#view-set-select')).toHaveValue('');
  expect(await activeViewSet(page)).toBe(null);
  await page.locator('body').press('9');                                   // no ninth set: nothing
  expect(await activeViewSet(page)).toBe(null);

  // A digit typed into a text field is text, never a set.
  await page.locator('#view-search').focus();
  await page.keyboard.type('1');
  await expect(page.locator('#view-search')).toHaveValue('1');
  expect(await activeViewSet(page)).toBe(null);
  await page.fill('#view-search', '');
  await page.locator('#alerts-gear').click();
  await expect(page.locator('#alerts-config')).toBeVisible();
  const watch = page.locator('#alerts-watch');
  const watchBefore = await watch.inputValue();
  await watch.fill('');
  await watch.focus();
  await page.keyboard.type('1');
  await expect(watch).toHaveValue('1');
  expect(await activeViewSet(page)).toBe(null);
  await watch.fill(watchBefore);
  await watch.blur();
  await page.click('#alerts-config-close');
  await expect(page.locator('#alerts-config')).toBeHidden();
  await page.locator('#stats-toggle').click();
  await expect(page.locator('#stats-aircraft-search')).toBeVisible();
  await page.locator('#stats-aircraft-search').focus();
  await page.keyboard.type('1');
  await expect(page.locator('#stats-aircraft-search')).toHaveValue('1');
  expect(await activeViewSet(page)).toBe(null);
  await page.fill('#stats-aircraft-search', '');
  await page.evaluate(() => window.skycueApp.showMain('map'));

  await page.selectOption('#view-set-select', { label: 'Airlines' });
  expect(await activeViewSet(page)).toBe(0);
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().search)).toBe('ual');
  // Choosing the active set again re-applies it when the state drifted.
  await page.evaluate(() => window.skycueApp.setViewFilters({ search: 'dal' }));
  expect(await setSelectedText(page)).toBe('Airlines (modified)');
  await page.selectOption('#view-set-select', { label: 'Airlines' });
  expect(await page.evaluate(() => window.skycueApp.getViewFilters())).toEqual(ual);
  expect(await setSelectedText(page)).toBe('Airlines');
  expect(await activeViewSet(page)).toBe(0);
  await page.click('#view-set-more');
  await expect(page.locator('#view-set-more')).toHaveAttribute('aria-expanded', 'true');
  await expect(page.locator('#view-set-menu [data-act="up"]')).toBeDisabled();
  await expect(page.locator('#view-set-menu [data-act="down"]')).toBeEnabled();
  await page.locator('#view-set-menu [data-act="rename"]').focus();
  await page.keyboard.press('Escape');
  await expect(page.locator('#view-set-menu')).toBeHidden();
  await expect(page.locator('#view-set-more')).toHaveAttribute('aria-expanded', 'false');
  page.once('dialog', (d) => { question = d.defaultValue(); d.accept('Emerg'); });
  await viewSetMenu(page, 'rename');
  expect(question).toBe('Airlines');
  await expect(page.locator('#view-set-msg')).toHaveText('A set named "Emerg" already exists.');
  await expect(page.locator('#view-set-msg')).toHaveClass(/error/);
  page.once('dialog', (d) => d.accept('Airliners'));
  await viewSetMenu(page, 'rename');
  expect(await setOptionTexts(page)).toEqual(['— current —', 'Airliners', 'Emerg']);
  await viewSetMenu(page, 'down');
  expect(await setOptionTexts(page)).toEqual(['— current —', 'Emerg', 'Airliners']);
  expect(await setSelectedText(page)).toBe('Airliners');
  page.once('dialog', (d) => { question = d.message(); d.accept(); });
  await page.click('#view-set-delete');
  expect(question).toBe('Delete the filter set "Airliners"?');
  expect(await setOptionTexts(page)).toEqual(['— current —', 'Emerg']);
  await expect(page.locator('#view-set-select')).toHaveValue('');
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.viewSets'))).sets.map((x) => x.name)).toEqual(['Emerg']);
  await page.evaluate(() => { window.skycueApp.resetViewFilters(); window.skycueApp.setHideFiltered(false); });
});
test('view_sets_row_fits_panel', async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem('skycue.viewSets', JSON.stringify({ version: 1, sets: [{ name: 'W'.repeat(40), view: {}, hideFiltered: false }] }));
  });
  await openPage(page);
  expect(await setOptionTexts(page)).toEqual(['— current —', 'W'.repeat(40)]);
  await page.selectOption('#view-set-select', '0');
  const tops = await page.evaluate(() => ['view-set-select', 'view-set-save', 'view-set-delete', 'view-set-more']
    .map((id) => document.getElementById(id).getBoundingClientRect().top));
  tops.forEach((t) => expect(Math.abs(t - tops[0])).toBeLessThanOrEqual(2));
  expect(await page.evaluate(() => { const f = document.getElementById('filters'); return f.scrollWidth - f.clientWidth; })).toBe(0);
  await page.click('#view-set-more');
  await expect(page.locator('#view-set-menu')).toBeVisible();
  expect(await page.evaluate(() => { const f = document.getElementById('filters'); return f.scrollWidth - f.clientWidth; })).toBe(0);
  await page.keyboard.press('Escape');
});

// Settings export and import (spec §4.4, §11 "Import merge"): one JSON file
// with the filter sets and the alert config, replaced or merged; never sent
// to skycued.
async function saveSetAs(page, name) {
  page.once('dialog', (d) => d.accept(name));
  await page.click('#view-set-save');
}
async function deleteSets(page, names) {
  for (const name of names) {
    await page.selectOption('#view-set-select', { label: name });
    page.once('dialog', (d) => d.accept());
    await page.click('#view-set-delete');
  }
  expect(await setOptionTexts(page)).toEqual(['— current —']);
  await page.evaluate(() => { window.skycueApp.resetViewFilters(); window.skycueApp.setHideFiltered(false); });
}
async function importFile(page, file) {
  const [chooser] = await Promise.all([page.waitForEvent('filechooser'), viewSetMenu(page, 'import')]);
  expect(chooser.isMultiple()).toBe(false);
  await page.setInputFiles('#settings-file', file);
}
test('settings_export_then_import_restores_sets', async ({ page }) => {
  const writes = [];                                         // import never touches skycued
  page.on('request', (r) => { if (r.method() !== 'GET') writes.push(r.method() + ' ' + r.url()); });
  await openPage(page);
  await page.evaluate(() => window.skycueApp.setViewFilters({ search: 'ual' }));
  await saveSetAs(page, 'A');
  await page.evaluate(() => { window.skycueApp.resetViewFilters(); window.skycueApp.setViewFilters({ emergency: true }); });
  await saveSetAs(page, 'B');
  const [download] = await Promise.all([page.waitForEvent('download'), viewSetMenu(page, 'export')]);
  expect(download.suggestedFilename()).toMatch(/^skycue-settings-\d{4}-\d{2}-\d{2}\.json$/);
  const file = await download.path();
  const data = JSON.parse(fs.readFileSync(file, 'utf8'));
  expect(data.skycue_settings).toBe(1);
  expect(data.sets.map((s) => s.name)).toEqual(['A', 'B']);
  expect(data.alerts.rules.emergency.enabled).toBe(true);
  expect(Object.prototype.hasOwnProperty.call(data.alerts, 'hidden')).toBe(false);
  const ual = await page.evaluate(() => window.skycuePure.normalizeView({ search: 'ual' }));
  expect(data.sets[0]).toEqual({ name: 'A', view: ual, hideFiltered: false });

  await deleteSets(page, ['A', 'B']);
  await importFile(page, file);
  await expect(page.locator('#settings-import')).toBeVisible();
  await expect(page.locator('#settings-import-text')).toContainText('2 filter sets and the alert settings');
  await expect(page.locator('#settings-import-text')).not.toContainText('Merge replaces');
  await page.click('#settings-import-replace');
  await expect(page.locator('#settings-import')).toBeHidden();
  await expect(page.locator('#view-set-msg')).toHaveText('Imported 2 filter sets.');
  expect(await setOptionTexts(page)).toEqual(['— current —', 'A', 'B']);
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.viewSets'))).sets.map((x) => x.name)).toEqual(['A', 'B']);
  expect(JSON.parse(await page.evaluate(() => localStorage.getItem('skycue.alerts'))).rules.emergency.enabled).toBe(true);

  await page.selectOption('#view-set-select', { label: 'A' });
  page.once('dialog', (d) => d.accept('C'));
  await viewSetMenu(page, 'rename');
  await importFile(page, file);
  await expect(page.locator('#settings-import-text')).toContainText('Merge replaces: B');
  await page.click('#settings-import-merge');
  expect(await setOptionTexts(page)).toEqual(['— current —', 'C', 'B', 'A']);
  await expect(page.locator('#view-set-msg')).toHaveText('Imported 2 filter sets.');
  expect(writes).toEqual([]);
  await deleteSets(page, ['C', 'B', 'A']);
});
test('settings_import_bad_and_hostile_files', async ({ page }) => {
  await openPage(page);
  const msg = page.locator('#view-set-msg');
  await page.setInputFiles('#settings-file', { name: 'x.json', mimeType: 'application/json', buffer: Buffer.from('{"x":1}') });
  await expect(msg).toHaveText('Cannot import: not a skycue settings file.');
  await expect(msg).toHaveClass(/error/);
  await expect(page.locator('#settings-import')).toBeHidden();
  await page.setInputFiles('#settings-file', { name: 'big.json', mimeType: 'application/json',
    buffer: Buffer.alloc(1536 * 1024, ' ') });
  await expect(msg).toHaveText('File too large (over 1 MB).');
  await expect(page.locator('#settings-import')).toBeHidden();
  const hostile = '<img src=x onerror="window.__xss=3">';
  await page.setInputFiles('#settings-file', { name: '<b>h</b>.json', mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify({ skycue_settings: 1, sets: [{ name: hostile, view: { search: hostile } }] })) });
  await expect(page.locator('#settings-import-text')).toHaveText('<b>h</b>.json: 1 filter sets. Replace yours, or merge?');
  await page.click('#settings-import-replace');
  expect(await setOptionTexts(page)).toEqual(['— current —', hostile]);
  await page.selectOption('#view-set-select', { label: hostile });
  await expect(page.locator('#view-search')).toHaveValue(hostile);
  expect(await page.locator('#view img').count()).toBe(0);
  expect(await page.locator('#settings-import b').count()).toBe(0);
  expect(await page.evaluate(() => window.__xss)).toBeUndefined();
  await deleteSets(page, [hostile]);
});

// The map (spec §5). RPA4521 (ac5e81) times out for ~3 s of each ~122 s
// fixture loop, so marker totals are compared with a tolerance of one.
const near = (a, b) => Math.abs(a - b) <= 1;
test('view_hide_mode_removes_non_matching_markers', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  const total = await page.evaluate(() => window.skycueApp.markers().length);
  await page.fill('#view-search', TARGET);
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().map((m) => m.hex))).toEqual([TARGET]);
  // No marker and no trail for the others (dropLayers), in the DOM too.
  expect(await page.evaluate(() => ({ markers: document.querySelectorAll('.ac-marker').length,
    trails: document.querySelectorAll('path.trail').length }))).toEqual({ markers: 1, trails: 1 });
  await page.click('#view-clear');
  await expect.poll(async () => near(await page.evaluate(() => window.skycueApp.markers().length), total)).toBe(true);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_dim_mode_ghosts_non_matches', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  const total = await page.evaluate(() => window.skycueApp.markers().length);
  const trailsBefore = await page.evaluate(() => window.skycueApp.markers().filter((m) => m.trail > 0).map((m) => m.hex));
  expect(trailsBefore.length).toBeGreaterThan(1);
  await page.check('input[name="view-map-mode"][value="dim"]');
  await page.fill('#view-search', TARGET);
  // Every marker stays; all but the target are dimmed and have no trail.
  await expect.poll(() => page.evaluate((n) => {
    const ms = window.skycueApp.markers();
    return { count: Math.abs(ms.length - n) <= 1, undimmed: ms.filter((m) => !m.dimmed).map((m) => m.hex),
      dimmedTrails: ms.filter((m) => m.dimmed && m.trail > 0).length };
  }, total)).toEqual({ count: true, undimmed: [TARGET], dimmedTrails: 0 });
  const dimmed = page.locator('.ac-marker.dimmed').first();
  await expect(dimmed).toHaveCSS('opacity', '0.25');
  await expect(dimmed).toHaveCSS('pointer-events', 'none');
  await expect(dimmed.locator('.ac-glyph')).toHaveCSS('pointer-events', 'none');
  await expect(dimmed.locator('.ac-label')).toBeHidden();
  await expect(page.locator(`.ac-marker[data-hex="${TARGET}"]`)).not.toHaveClass(/dimmed/);
  await expect(page.locator(`.ac-marker[data-hex="${TARGET}"] .ac-label`)).toBeVisible();
  expect((await markerOf(page, TARGET)).trail).toBeGreaterThan(0);      // the target's trail stays
  // A dimmed marker keeps its place and heading (the glyph's rotation).
  const pos = await page.evaluate((h) => {
    const m = window.skycueApp.markers().find((x) => x.dimmed && x.hex !== h);
    const el = document.querySelector(`.ac-marker[data-hex="${m.hex}"] .ac-glyph`);
    return { lat: typeof m.lat, rotate: /rotate\(/.test(el.style.transform) };
  }, TARGET);
  expect(pos).toEqual({ lat: 'number', rotate: true });
  // A trail point expiring (the 2-s prune) does not refill a dimmed trail.
  // abc0d1 gets a point 1 s short of the 120 s trail age and a current one.
  await page.evaluate(() => {
    const now = window.skycueApp.dataNow();
    [now - 119000, now].forEach((ms) => window.skycueApp.injectTestMarker({ hex: 'abc0d1', decision: 'send',
      alt_baro_ft: 5000, lat: 38.95, lon: -77.05, position_time: new Date(ms).toISOString() }));
  });
  await expect.poll(async () => (await markerOf(page, 'abc0d1') || {}).dimmed).toBe(true);
  await page.waitForTimeout(1500);
  const pruned = await page.evaluate(() => {
    window.skycueApp.prune();
    return window.skycueApp.markers().filter((m) => m.dimmed && m.trail > 0).length;
  });
  expect(pruned).toBe(0);
  // Un-dimming gives the trails back at once (not only on the next update).
  const back = await page.evaluate(() => {
    window.skycueApp.setViewFilters({ search: '' });
    window.skycueApp.renderAll();
    return window.skycueApp.markers().filter((m) => m.trail > 0).map((m) => m.hex);
  });
  // abc0d1's old point did expire: one point left once it is un-dimmed.
  expect((await markerOf(page, 'abc0d1')).trail).toBe(1);
  // At most one missing: RPA4521 (ac5e81) may have timed out meanwhile.
  expect(trailsBefore.filter((h) => back.indexOf(h) < 0).length).toBeLessThanOrEqual(1);
  await page.fill('#view-search', TARGET);
  await page.check('input[name="view-map-mode"][value="hide"]');
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().length)).toBe(1);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_selected_track_survives_filter', async ({ page }) => {
  await openPage(page);
  await selectTarget(page);
  await page.fill('#view-search', 'zzz');                       // matches nothing
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().map((m) => m.hex))).toEqual([TARGET]);
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);
  await page.click('#table-toggle');
  await expect.poll(async () => hexes(await tableRows(page))).toEqual([TARGET]);
  await expect(page.locator('#view-count')).toHaveText(/^showing 1 of \d+$/);
  await page.keyboard.press('Escape');                          // deselect: now nothing shows
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().length)).toBe(0);
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(null);
  await expect.poll(async () => (await tableRows(page)).length).toBe(0);
  await expect(page.locator('#view-count')).toHaveText(/^showing 0 of \d+$/);
  // In dim mode the same filter ghosts every marker instead.
  await page.check('input[name="view-map-mode"][value="dim"]');
  await expect.poll(() => page.evaluate(() => {
    const ms = window.skycueApp.markers();
    return ms.length >= 10 && ms.every((m) => m.dimmed);
  })).toBe(true);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
});
test('view_composes_with_hide_filtered', async ({ page }) => {
  await openPage(page); await waitMarkers(page, 10);
  await page.evaluate(() => window.skycueApp.setHideFiltered(true));
  const hiddenOnly = await page.evaluate(() => window.skycueApp.markers().length);
  const hiddenText = await page.locator('#hidden-count').textContent();
  await page.fill('#view-search', 'ual');
  // UAL1423 (sent) and UAL455 (sent or filtered, as the fixture has it).
  await expect.poll(() => page.evaluate(() => {
    const ms = window.skycueApp.markers();
    return ms.length >= 1 && ms.length <= 2 && ms.every((m) => m.decision === 'sent' || m.decision === 'allow_listed');
  })).toBe(true);
  // hidden-count keeps meaning what "Hide filtered" hides.
  expect(near(parseInt(await page.locator('#hidden-count').textContent(), 10), parseInt(hiddenText, 10))).toBe(true);
  await page.click('#view-clear');
  await expect.poll(async () => near(await page.evaluate(() => window.skycueApp.markers().length), hiddenOnly)).toBe(true);
  expect(await page.evaluate(() => window.skycueApp.getHideFiltered())).toBe(true);
  await page.evaluate(() => window.skycueApp.resetViewFilters());
  await page.evaluate(() => window.skycueApp.setHideFiltered(false));
});

// ---- Stats view ------------------------------------------------------------------
// The third main view (spec 6): /api/stats/* polled while it is shown.
const statsState = (page) => page.evaluate(() => window.skycueApp.stats());
async function openStats(page) {
  await openPage(page);
  await page.locator('#stats-toggle').click();
  await expect(page.locator('#stats-view')).toBeVisible();
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 10000 }).toBeGreaterThan(0);
}
const tileText = (page, id) => page.locator(`#stats-tiles .tile[data-tile="${id}"] .tile-value`).textContent();
const tileNumber = async (page, id) => Number((await tileText(page, id)).replace(/,/g, ''));

// Exactly one of map, table and stats; Table and Stats read "Map" while
// their view is shown, and pressing the active one goes back to the map.
test('stats_toggle_switches_main_view', async ({ page }) => {
  await openPage(page);
  const tv = page.locator('#table-view'), sv = page.locator('#stats-view');
  const tt = page.locator('#table-toggle'), stg = page.locator('#stats-toggle');
  const expectMain = async (which) => {
    expect(await page.evaluate(() => window.skycueApp.mainView())).toBe(which);
    await (which === 'table' ? expect(tv).toBeVisible() : expect(tv).toBeHidden());
    await (which === 'stats' ? expect(sv).toBeVisible() : expect(sv).toBeHidden());
    await expect(tt).toHaveText(which === 'table' ? 'Map' : 'Table');
    await expect(tt).toHaveAttribute('aria-pressed', String(which === 'table'));
    await expect(stg).toHaveText(which === 'stats' ? 'Map' : 'Stats');
    await expect(stg).toHaveAttribute('aria-pressed', String(which === 'stats'));
    expect((await statsState(page)).open).toBe(which === 'stats');
  };
  await expectMain('map');
  await stg.click();
  await expectMain('stats');
  // The view covers the map.
  const covered = await page.evaluate(() => {
    const b = document.getElementById('map').getBoundingClientRect();
    const e = document.elementFromPoint(b.left + b.width / 2, b.top + b.height / 2);
    return !!e && !!e.closest('#stats-view');
  });
  expect(covered).toBe(true);
  await tt.click();
  await expectMain('table');
  await stg.click();
  await expectMain('stats');
  await stg.click();
  await expectMain('map');
  await page.evaluate(() => window.skycueApp.showMain('stats'));
  await expectMain('stats');
  await tt.click();
  await tt.click();
  await expectMain('map');
});

// The range buttons: one pressed, minute series up to 24 h and hour series
// beyond, the choice kept in this browser.
test('stats_range_switches_resolution_and_persists', async ({ page }) => {
  const seriesUrls = [];
  page.on('request', (r) => { if (r.url().includes('/api/stats/series?')) seriesUrls.push(r.url()); });
  await openStats(page);
  const pressed = () => page.locator('#stats-range button').evaluateAll((bs) =>
    bs.filter((b) => b.getAttribute('aria-pressed') === 'true').map((b) => b.getAttribute('data-range')));
  let s = await statsState(page);
  expect(s.range).toBe('24h');
  expect(s.res).toBe('minute');
  expect(await pressed()).toEqual(['24h']);
  // On open: the range's minute series and the whole hour series.
  expect(seriesUrls.some((u) => /series\?res=minute&from=/.test(u))).toBe(true);
  expect(seriesUrls.some((u) => /series\?res=hour$/.test(u))).toBe(true);
  const n = (await statsState(page)).refreshes;
  await page.locator('#stats-range button[data-range="7d"]').click();
  await expect.poll(async () => (await statsState(page)).refreshes).toBeGreaterThan(n);
  s = await statsState(page);
  expect(s.range).toBe('7d');
  expect(s.res).toBe('hour');
  expect(await pressed()).toEqual(['7d']);
  expect(seriesUrls.some((u) => /series\?res=hour&from=/.test(u))).toBe(true);
  await page.reload();
  await page.waitForFunction(() => window.skycueApp && window.skycueApp.connected());
  await page.locator('#stats-toggle').click();
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 10000 }).toBeGreaterThan(0);
  s = await statsState(page);
  expect(s.range).toBe('7d');
  expect(s.res).toBe('hour');
  await page.locator('#stats-range button[data-range="1h"]').click();
  await expect.poll(async () => (await statsState(page)).res).toBe('minute');
  expect(await pressed()).toEqual(['1h']);
});

// Six headline tiles with numbers and a drawn sparkline each; a refresh
// moves the observations as the replay goes on.
test('stats_tiles_show_totals_with_sparklines', async ({ page }) => {
  await openStats(page);
  const s = await statsState(page);
  expect(Object.keys(s.tiles)).toEqual(['aircraft', 'observations', 'sent', 'drops', 'max_range', 'emergencies']);
  await expect.poll(() => tileNumber(page, 'observations'), { timeout: 15000 }).toBeGreaterThan(0);
  // Aircraft = ledger entries last seen in the range, which ends at the
  // daemon's data clock. (Not "all 12": after the reconnect case restarts
  // skycued the replay's clock starts over behind the reloaded ledger.)
  await expect.poll(() => tileNumber(page, 'aircraft'), { timeout: 15000 }).toBeGreaterThan(0);
  for (const id of ['aircraft', 'observations', 'sent', 'drops', 'emergencies']) {
    expect(await tileText(page, id), id).toMatch(/^\d{1,3}(,\d{3})*$/);
  }
  await expect.poll(() => tileText(page, 'max_range'), { timeout: 15000 }).toMatch(/^\d+\.\d nm$/);
  await expect(page.locator('#stats-tiles .tile[data-tile="max_range"] .tile-sub')).toHaveText(/^[0-9a-f]{6}$/);
  await expect(page.locator('#stats-since')).toHaveText(/^since \d{4}-\d\d-\d\d \d\d:\d\dZ$/);
  await expect.poll(async () => (await statsState(page)).sparks.filter((x) => x.canvas).length).toBe(6);
  expect((await statsState(page)).sparks.map((x) => x.id)).toEqual(Object.keys(s.tiles));
  // The tiles follow the daemon's range totals.
  const before = await tileNumber(page, 'observations');
  await expect.poll(async () => {
    await page.evaluate(() => window.skycueApp.statsRefresh());
    return tileNumber(page, 'observations');
  }, { timeout: 15000 }).toBeGreaterThan(before);
  expect((await statsState(page)).tiles.observations).toBe(await tileText(page, 'observations'));
});

// Live: a refresh every 10 s while the view is shown; once it is hidden,
// or live is off, no timer is left and nothing is requested.
test('stats_live_polls_only_while_shown', async ({ page }) => {
  await openStats(page);
  await expect(page.locator('#stats-live')).toBeChecked();
  const r0 = (await statsState(page)).refreshes;
  const obs0 = (await statsState(page)).tiles.observations;
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 15000 }).toBeGreaterThan(r0);
  await expect.poll(async () => (await statsState(page)).liveTimer).toBe(true);
  // A headline tile moves on the live refreshes alone (no statsRefresh()).
  await expect.poll(() => tileText(page, 'observations'), { timeout: 25000 }).not.toBe(obs0);
  expect(Number((await tileText(page, 'observations')).replace(/,/g, ''))).toBeGreaterThan(Number(obs0.replace(/,/g, '')));
  let statsRequests = 0;
  page.on('request', (r) => { if (r.url().includes('/api/stats/')) statsRequests++; });
  await page.locator('#stats-toggle').click();
  await expect(page.locator('#stats-view')).toBeHidden();
  const r1 = (await statsState(page)).refreshes;
  expect((await statsState(page)).liveTimer).toBe(false);
  await page.waitForTimeout(2000);
  expect((await statsState(page)).refreshes).toBe(r1);
  expect(statsRequests).toBe(0);
  // Shown again with live off: one refresh, no timer.
  await page.locator('#stats-toggle').click();
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 10000 }).toBeGreaterThan(r1);
  await page.locator('#stats-live').uncheck();
  await expect.poll(async () => (await statsState(page)).liveTimer).toBe(false);
});

// Reset asks first: dismissed, nothing happens; accepted, the statistics
// start again: a newer "since" and fewer observations in the range (from
// the new since) than before. (Not fewer aircraft: the looping replay hears
// all 12 again within seconds.)
test('stats_reset_asks_first_and_empties', async ({ page }) => {
  await openStats(page);
  const s0 = (await api('/api/stats/summary')).body;
  expect(s0.range.observations).toBeGreaterThan(0);
  let question = null;
  page.once('dialog', (d) => { question = d.message(); d.dismiss(); });
  await page.locator('#stats-reset').click();
  await expect.poll(() => question).not.toBe(null);
  expect(question).toMatch(/^Reset all statistics\?/);
  await page.waitForTimeout(500);
  const kept = (await api('/api/stats/summary')).body;
  expect(kept.since).toBe(s0.since);
  expect(kept.range.observations).toBeGreaterThanOrEqual(s0.range.observations);
  // The page's own tile, freshly drawn, is the baseline for the reset.
  await page.evaluate(() => window.skycueApp.statsRefresh());
  const t0 = await tileNumber(page, 'observations');
  expect(t0).toBeGreaterThan(0);
  const before = kept.range.observations;
  page.once('dialog', (d) => d.accept());
  await page.locator('#stats-reset').click();
  await expect(page.locator('#stats-msg')).toHaveText('Statistics reset.', { timeout: 10000 });
  const s1 = (await api('/api/stats/summary')).body;
  expect(Date.parse(s1.since)).toBeGreaterThan(Date.parse(s0.since));
  expect(s1.range.observations).toBeLessThan(before);
  // The page drew the emptied totals too.
  await expect.poll(() => tileNumber(page, 'observations'), { timeout: 10000 }).toBeLessThan(t0);
});

// Input health lives in the Receiver tab (spec 2): the toolbar opens it
// beside the view.
test('stats_receiver_link_opens_receiver_tab', async ({ page }) => {
  await openStats(page);
  await page.locator('#stats-receiver-link').click();
  await expect(page.locator('#details')).toBeVisible();
  await expect(page.locator('#tab-receiver')).toHaveAttribute('aria-selected', 'true');
  await expect(page.locator('#stats-receiver-link')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#stats-view')).toBeVisible();
  await page.locator('#stats-receiver-link').click();
  await expect(page.locator('#details')).toBeHidden();
});

// ---- Stats: Traffic and Weather (spec 6 items 2 and 6) ------------------------
// A card's live ECharts option (null before its chart is made).
const cardOption = (page, id) => page.evaluate((cid) => {
  const el = document.querySelector('[data-card="' + cid + '"] .card-chart');
  const inst = el && window.echarts.getInstanceByDom(el);
  return inst ? JSON.parse(JSON.stringify(inst.getOption())) : null;
}, id);
const cardCaption = (page, id) => page.locator(`[data-card="${id}"] .card-caption`).textContent();
const isPairs = (data) => Array.isArray(data) && data.every((p) => Array.isArray(p) && p.length === 2 &&
  typeof p[0] === 'number' && typeof p[1] === 'number');
const sumPairs = (data) => data.reduce((b, p) => b + p[1], 0);
const TRAFFIC_CARDS = ['traffic-inputs', 'traffic-aircraft', 'traffic-sent', 'traffic-week'];
const sectionOf = (page, grid) => page.locator(grid).locator('xpath=ancestor::section[contains(@class,"stats-section")]');

async function openTraffic(page) {
  await openStats(page);
  await expect.poll(async () => (await statsState(page)).cards.filter((c) => TRAFFIC_CARDS.includes(c.id) && c.canvas)
    .map((c) => c.id), { timeout: 15000 }).toEqual(TRAFFIC_CARDS);
}

test('stats_traffic_inputs_stacked_by_input_with_zoom', async ({ page }, testInfo) => {
  await openTraffic(page);
  await expect.poll(async () => (await cardOption(page, 'traffic-inputs')).series
    .reduce((a, s) => a + sumPairs(s.data), 0), { timeout: 15000 }).toBeGreaterThan(0);
  const o = await cardOption(page, 'traffic-inputs');
  const inputs = Object.keys((await api('/api/stats/series')).body.by_input);
  expect(inputs.length).toBeGreaterThan(0);
  // One stacked area per input, ordered by total with 'other' last.
  expect(o.series.map((s) => s.name).sort()).toEqual(inputs.slice().sort());
  const sums = o.series.map((s) => sumPairs(s.data));
  for (let i = 1; i < sums.length; i++) {
    if (o.series[i].name !== 'other') expect(sums[i - 1]).toBeGreaterThanOrEqual(sums[i]);
  }
  if (inputs.includes('other')) expect(o.series[o.series.length - 1].name).toBe('other');
  for (const s of o.series) {
    expect(s.type).toBe('line');
    expect(s.stack).toBe('obs');
    expect(s.areaStyle).toBeTruthy();
    expect(isPairs(s.data), s.name).toBe(true);
  }
  expect(o.xAxis[0].type).toBe('time');
  // The axis spans the buckets drawn, half a bucket either side, in UTC.
  const ts = o.series[0].data.map((p) => p[0]);
  expect(o.xAxis[0].min).toBe(ts[0] - 30000);
  expect(o.xAxis[0].max).toBe(ts[ts.length - 1] + 30000);
  expect(o.useUTC).toBe(true);
  expect(o.dataZoom.map((z) => z.type).sort()).toEqual(['inside', 'slider']);
  expect(await cardCaption(page, 'traffic-inputs'))
    .toMatch(new RegExp('^\\d{1,3}(,\\d{3})* observations · ' + inputs.length + ' inputs?$'));
  // The window the user zoomed to survives a refresh (setOption merges).
  const zoom = (start, end) => page.evaluate(([s, e]) => {
    const el = document.querySelector('[data-card="traffic-inputs"] .card-chart');
    window.echarts.getInstanceByDom(el).dispatchAction({ type: 'dataZoom', start: s, end: e });
  }, [start, end]);
  await zoom(40, 90);
  const r0 = (await statsState(page)).refreshes;
  await page.evaluate(() => window.skycueApp.statsRefresh());
  expect((await statsState(page)).refreshes).toBeGreaterThan(r0);
  for (const dz of (await cardOption(page, 'traffic-inputs')).dataZoom) {
    expect(dz.start).toBe(40);
    expect(dz.end).toBe(90);
  }
  await zoom(0, 100);
  const sec = sectionOf(page, '#stats-traffic');
  await sec.scrollIntoViewIfNeeded();
  await sec.screenshot({ path: testInfo.outputPath(`skycue-stats-traffic-${testInfo.project.name}.png`) });
});

test('stats_traffic_aircraft_and_sent_vs_drops', async ({ page }) => {
  await openTraffic(page);
  const a = await cardOption(page, 'traffic-aircraft');
  expect(a.series.map((s) => [s.id, s.type])).toEqual([['distinct', 'bar'], ['new', 'line']]);
  a.series.forEach((s) => expect(isPairs(s.data), s.id).toBe(true));
  await expect.poll(async () => sumPairs((await cardOption(page, 'traffic-aircraft')).series[0].data),
    { timeout: 15000 }).toBeGreaterThan(0);
  expect(await cardCaption(page, 'traffic-aircraft')).toMatch(/^\d{1,3}(,\d{3})* aircraft · \d{1,3}(,\d{3})* new$/);
  const o = await cardOption(page, 'traffic-sent');
  expect(o.series.map((s) => s.id))
    .toEqual(['sent', 'timeout', 'source_lost', 'blocked', 'out_of_range', 'below_elevation', 'no_position']);
  expect(o.series.map((s) => s.name))
    .toEqual(['sent', 'timeout', 'source lost', 'blocked', 'out of range', 'below elevation', 'no position']);
  expect(o.series.map((s) => s.stack)).toEqual(['sent', 'drops', 'drops', 'drops', 'drops', 'drops', 'drops']);
  // A plain legend (wrapping), not a paged one: all seven entries on view.
  expect(o.legend[0].type).toBe('plain');
  o.series.forEach((s) => { expect(s.type).toBe('bar'); expect(isPairs(s.data), s.id).toBe(true); });
  // The decision series in the page's decision colours (style.css tokens).
  const col = {};
  o.series.forEach((s) => { col[s.id] = s.itemStyle.color; });
  expect(col).toMatchObject({ sent: '#3fb950', blocked: '#f85149', out_of_range: '#8b949e',
    below_elevation: '#f0883e', no_position: '#d2a8ff' });
  // The replay sends updates: the sent bars are not all zero.
  await expect.poll(async () => sumPairs((await cardOption(page, 'traffic-sent')).series[0].data),
    { timeout: 15000 }).toBeGreaterThan(0);
  expect(await cardCaption(page, 'traffic-sent')).toMatch(/^\d{1,3}(,\d{3})* sent · \d{1,3}(,\d{3})* drops$/);
});

test('stats_traffic_hour_of_week_heatmap_utc', async ({ page }) => {
  await openTraffic(page);
  await expect.poll(async () => (await cardOption(page, 'traffic-week')).series[0].data
    .reduce((b, c) => b + c[2], 0), { timeout: 15000 }).toBeGreaterThan(0);
  const o = await cardOption(page, 'traffic-week');
  const cells = o.series[0].data;
  expect(o.series[0].type).toBe('heatmap');
  expect(cells.length).toBe(168);
  expect(o.xAxis[0].data).toEqual(Array.from({ length: 24 }, (_, h) => String(h).padStart(2, '0')));
  expect(o.yAxis[0].data).toEqual(['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']);
  // Built from the hour series: every non-zero cell is the UTC weekday and
  // hour of one of its buckets.
  const hours = (await api('/api/stats/series?res=hour')).body;
  const slots = new Set(hours.t.map((t) => { const d = new Date(t); return d.getUTCHours() + ',' + (d.getUTCDay() + 6) % 7; }));
  expect(slots.size).toBeGreaterThan(0);
  for (const c of cells) if (c[2] > 0) expect(slots.has(c[0] + ',' + c[1]), c.join(',')).toBe(true);
  const total = cells.reduce((b, c) => b + c[2], 0);
  const caption = await cardCaption(page, 'traffic-week');
  expect(caption).toMatch(/ · busiest (Mon|Tue|Wed|Thu|Fri|Sat|Sun) \d\d:00 · UTC$/);
  // From 1700 px (three columns) the heatmap takes the whole row; below,
  // one of the two columns.
  const w = await page.evaluate(() => ({
    grid: document.getElementById('stats-traffic').getBoundingClientRect().width,
    card: document.querySelector('[data-card="traffic-week"]').getBoundingClientRect().width,
    vw: window.innerWidth }));
  if (w.vw >= 1700) expect(w.card).toBeCloseTo(w.grid, 0);
  else expect(w.card).toBeLessThan(w.grid * 0.6);
  expect(Number(caption.split(' ')[0].replace(/,/g, ''))).toBe(total);
});

// A failed slow fetch (the hour series here) gives null: the heatmap keeps
// what it drew, and nothing throws.
test('stats_traffic_failed_hour_fetch_keeps_heatmap', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  await openTraffic(page);
  await expect.poll(async () => (await cardOption(page, 'traffic-week')).series[0].data
    .reduce((b, c) => b + c[2], 0), { timeout: 15000 }).toBeGreaterThan(0);
  const before = (await cardOption(page, 'traffic-week')).series[0].data;
  const capBefore = await cardCaption(page, 'traffic-week');
  let failedHour = 0;
  await page.route(/\/api\/stats\/series\?res=hour$/, (route) => {
    failedHour++;
    route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"test"}' });
  });
  // Closing and opening makes the next refresh a slow one.
  const r0 = (await statsState(page)).refreshes;
  await page.locator('#stats-toggle').click();
  await page.locator('#stats-toggle').click();
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 10000 }).toBeGreaterThan(r0);
  await page.evaluate(() => window.skycueApp.statsRefresh());
  expect(failedHour).toBeGreaterThan(0);
  expect((await cardOption(page, 'traffic-week')).series[0].data).toEqual(before);
  expect(await cardCaption(page, 'traffic-week')).toBe(capBefore);
  await expect(page.locator('#stats-msg')).not.toHaveClass(/error/);
  expect(errors).toEqual([]);
  await page.unroute(/\/api\/stats\/series\?res=hour$/);
});

// Weather: the main daemon's section follows its summary.uat_seen.
test('stats_weather_follows_uat_seen', async ({ page }) => {
  await openTraffic(page);
  const seen = (await api('/api/stats/summary')).body.uat_seen;
  const sec = sectionOf(page, '#stats-weather');
  if (seen) await expect(sec).toBeVisible();
  else {
    await expect(sec).toBeHidden();
    expect((await statsState(page)).cards.map((c) => c.id)).not.toContain('weather-uat');
  }
});

test.describe('stats weather (FIS-B daemon)', () => {
  test.skip(!FISB_BASE, 'FIS-B captures not fetched: run scripts/fetch-test-captures.sh');
  test.use({ baseURL: FISB_BASE });
  test('stats_weather_uat_lines_shown_then_hidden', async ({ page }) => {
    expect((await apiAt(FISB_BASE, '/api/stats/summary')).body.uat_seen).toBe(true);
    await openStats(page);
    const sec = sectionOf(page, '#stats-weather');
    await expect(sec).toBeVisible();
    await expect.poll(async () => (await statsState(page)).cards.filter((c) => c.id === 'weather-uat' && c.canvas).length,
      { timeout: 15000 }).toBe(1);
    // One point per bucket of the range's series, from its uat_uplink,
    // uat_downlink and fisb_products columns; the caption gives their sums.
    // The decoded products are counted (the unpaced replay decodes every
    // uplink before the store's first tick); the UAT frame columns are the
    // dump978 input's counters, which a replay has not, so they read zero.
    // The chart and the API are read apart, so a minute bucket can land in
    // between: read both again (after a refresh) until they agree.
    let o, se, cap;
    await expect.poll(async () => {
      o = await cardOption(page, 'weather-uat');
      cap = await cardCaption(page, 'weather-uat');
      se = (await apiAt(FISB_BASE, '/api/stats/series')).body;
      if (o.series.every((s) => s.data.length === se.t.length)) return true;
      await page.evaluate(() => window.skycueApp.statsRefresh());
      return false;
    }, { timeout: 15000 }).toBe(true);
    expect(o.series.map((s) => [s.id, s.type])).toEqual([['uplink', 'line'], ['downlink', 'line'], ['products', 'line']]);
    o.series.forEach((s) => expect(isPairs(s.data), s.id).toBe(true));
    const sums = o.series.map((s) => sumPairs(s.data));
    expect(sums[2]).toBeGreaterThan(0);
    expect(sums[2]).toBe(se.fisb_products.reduce((a, b) => a + b, 0));
    const th = (n) => n.toLocaleString('en-US');
    expect(cap).toBe(th(sums[0]) + ' uplink · ' + th(sums[1]) + ' downlink frames · ' + th(sums[2]) + ' products');
    // A summary without UAT activity (as after a reset) hides the section.
    await page.route(/\/api\/stats\/summary\?/, async (route) => {
      const r = await route.fetch();
      const body = await r.json();
      body.uat_seen = false;
      await route.fulfill({ response: r, json: body });
    });
    await page.evaluate(() => window.skycueApp.statsRefresh());
    await expect(sec).toBeHidden();
    await page.unroute(/\/api\/stats\/summary\?/);
    await page.evaluate(() => window.skycueApp.statsRefresh());
    await expect(sec).toBeVisible();
  });
});

// ---- Stats: Aircraft (spec 6 item 7) -------------------------------------------
// The ledger table (GET /api/stats/aircraft), its search, sort and pager,
// and the detail card with its two ways back to the map.
const acState = async (page) => (await statsState(page)).aircraft;
const acRows = (page) => page.locator('#stats-aircraft-table tbody tr');
async function openAircraftSection(page) {
  await openStats(page);
  await expect(page.locator('#stats-aircraft-table')).toBeVisible();
  await expect.poll(() => acRows(page).count(), { timeout: 15000 }).toBeGreaterThan(0);
  await page.locator('#stats-aircraft-table').scrollIntoViewIfNeeded();
}
async function searchAircraft(page, text) {
  await page.fill('#stats-aircraft-search', text);
  await expect.poll(async () => (await acState(page)).q).toBe(text);
  await expect.poll(async () => (await acState(page)).rows).toEqual([TARGET_KEY]);
}
async function restoreMap(page) {
  await page.evaluate(() => { window.skycueApp.resetViewFilters(); window.skycueApp.showMain('map'); });
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().search)).toBe('');
}

test('stats_aircraft_table_search_and_pager', async ({ page }, testInfo) => {
  await openAircraftSection(page);
  const ledger = (await api('/api/stats/aircraft?limit=1000')).body;
  // The table's look is the track table's: the same header row styles,
  // numeric headers and cells right-aligned, the default sort marked.
  const heads = await page.$$eval('#stats-aircraft-table thead th', (ths) => ths.map((th) => ({
    key: th.getAttribute('data-key'), sort: th.getAttribute('data-sort'), num: th.classList.contains('num'),
    align: getComputedStyle(th).textAlign, pos: getComputedStyle(th).position })));
  expect(heads.map((h) => h.key)).toEqual(['hex', 'callsigns', 'registration', 'category', 'class', 'country',
    'first_seen', 'last_seen', 'sessions', 'observations', 'sent_updates', 'max_range_nm', 'max_alt_ft', 'max_gs_kt']);
  heads.forEach((h) => {
    expect(h.align, h.key).toBe(h.num ? 'right' : 'left');
    expect(h.pos, h.key).toBe('sticky');
  });
  await expect(page.locator('#stats-aircraft-table th[data-sort="last_seen"]')).toHaveClass(/sorted desc/);
  const tdAlign = await page.$eval('#stats-aircraft-table tbody td[data-key="observations"]', (td) => getComputedStyle(td).textAlign);
  expect(tdAlign).toBe('right');
  // Class and country as the API gives them.
  const first = (await acState(page)).rows[0];
  const row = ledger.rows.find((r) => r.key === first);
  const tr = page.locator(`#stats-aircraft-table tbody tr[data-key="${first}"]`);
  await expect(tr.locator('td[data-key="class"]')).toHaveText(row.class);
  await expect(tr.locator('td[data-key="country"]')).toHaveText(row.country || '—');
  // At most 12 aircraft: one page, both pager buttons disabled.
  expect(ledger.total).toBeLessThanOrEqual(12);
  await expect(page.locator('#stats-aircraft-pager .stats-pager-text'))
    .toHaveText(new RegExp('^1–' + (await acRows(page).count()) + ' of \\d+$'));
  await expect(page.locator('#stats-aircraft-pager button[data-page="prev"]')).toBeDisabled();
  await expect(page.locator('#stats-aircraft-pager button[data-page="next"]')).toBeDisabled();
  // Below 1700 px first seen and sessions give way and the measures fit;
  // the time cells carry no year (the full time is in their title).
  const narrow = testInfo.project.use.viewport.width < 1700;
  for (const k of ['first_seen', 'sessions']) {
    await (narrow ? expect(page.locator(`#stats-aircraft-table th[data-key="${k}"]`)).toBeHidden()
      : expect(page.locator(`#stats-aircraft-table th[data-key="${k}"]`)).toBeVisible());
  }
  const fits = await page.$eval('.stats-table-wrap', (w) => w.scrollWidth <= w.clientWidth);
  expect(fits).toBe(true);
  await expect(tr.locator('td[data-key="last_seen"]')).toHaveText(/^\d\d-\d\d \d\d:\d\dZ$/);
  await expect(tr.locator('td[data-key="last_seen"]')).toHaveAttribute('title', /^\d{4}-\d\d-\d\dT/);
  // No evicted entries here: the line is hidden.
  expect(ledger.evicted).toBe(0);
  await expect(page.locator('#stats-evicted')).toBeHidden();
  const sec = sectionOf(page, '#stats-aircraft');
  await sec.scrollIntoViewIfNeeded();
  await sec.screenshot({ path: testInfo.outputPath(`skycue-stats-aircraft-${testInfo.project.name}.png`) });
  // The search (callsign, either case) finds the target only, 300 ms
  // after the last key; one request for the whole word.
  // Live paused, so no refresh lands in the window (one in flight could
  // still repeat the whole word: deduped; a partial word never goes out).
  await page.locator('#stats-live').uncheck();
  const asked = [];
  page.on('request', (r) => { if (r.url().includes('/api/stats/aircraft?')) asked.push(new URL(r.url()).searchParams.get('q')); });
  await page.locator('#stats-aircraft-search').pressSequentially('ual1423', { delay: 40 });
  await expect.poll(async () => (await acState(page)).rows, { timeout: 5000 }).toEqual([TARGET_KEY]);
  expect(Array.from(new Set(asked.filter((q) => q !== null)))).toEqual(['ual1423']);
  await expect(page.locator('#stats-aircraft-pager .stats-pager-text')).toHaveText('1–1 of 1');
  await expect(page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"] td[data-key="callsigns"]`))
    .toContainText('UAL1423');
  // The hex finds it too; a miss shows none.
  await page.fill('#stats-aircraft-search', 'A4F2');
  await expect.poll(async () => (await acState(page)).rows).toEqual([TARGET_KEY]);
  await page.fill('#stats-aircraft-search', 'zzzzzz');
  await expect.poll(async () => (await acState(page)).total).toBe(0);
  await expect(page.locator('#stats-aircraft-pager .stats-pager-text')).toHaveText('no aircraft');
  await restoreMap(page);
});

// Sorted by max range, which differs across the fixture's aircraft (the
// observation counts can all tie early on, and then both orders are the key
// order). Each order is read from the table in one go: the row keys and the
// column, so the two always belong to the same render.
test('stats_aircraft_sort_by_numeric_header', async ({ page }) => {
  await openAircraftSection(page);
  const KEY = 'max_range_nm';
  const th = page.locator(`#stats-aircraft-table th[data-sort="${KEY}"]`);
  const snapshot = () => page.$$eval('#stats-aircraft-table tbody tr[data-key]', (trs, key) => trs.map((tr) => {
    const t = tr.querySelector(`td[data-key="${key}"]`).textContent;
    return { key: tr.getAttribute('data-key'), v: t === '\u2014' ? null : Number(t.replace(/,/g, '')) };
  }), KEY);
  // Non-increasing (dir < 0) or non-decreasing (dir > 0) over the known values.
  const sorted = (rows, dir) => {
    const xs = rows.map((r) => r.v).filter((v) => v !== null);
    return xs.length > 0 && xs.every((v, i) => i === 0 || (dir < 0 ? xs[i - 1] >= v : xs[i - 1] <= v));
  };
  const settled = async (dir) => {
    let rows = [];
    await expect.poll(async () => {
      const s = await acState(page);
      rows = await snapshot();
      return s.sort === KEY && s.dir === (dir < 0 ? 'desc' : 'asc') &&
        rows.map((r) => r.key).join() === s.rows.join() && sorted(rows, dir);
    }).toBe(true);
    return rows;
  };
  await th.click();
  await expect(th).toHaveClass(/sorted desc/);
  await expect(page.locator('#stats-aircraft-table th[data-sort="last_seen"]')).not.toHaveClass(/sorted/);
  const desc = await settled(-1);
  // Again: low to high.
  await th.click();
  await expect(th).toHaveClass(/sorted/);
  await expect(th).not.toHaveClass(/desc/);
  const asc = await settled(1);
  expect(asc.length).toBe(desc.length);
  // The orders differ whenever the values do (all equal: both are key order).
  const known = desc.map((r) => r.v).filter((v) => v !== null);
  if (new Set(known).size > 1) expect(asc.map((r) => r.key)).not.toEqual(desc.map((r) => r.key));
  // A text column is not sortable (the API cannot): nothing changes.
  await page.locator('#stats-aircraft-table th[data-key="callsigns"]').click();
  expect((await acState(page)).sort).toBe(KEY);
  await restoreMap(page);
});

test('stats_aircraft_detail_fields_and_polar', async ({ page }, testInfo) => {
  await openAircraftSection(page);
  await searchAircraft(page, TARGET);
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`).click();
  const det = page.locator('#stats-detail');
  await expect(det).toBeVisible();
  expect((await statsState(page)).detailKey).toBe(TARGET_KEY);
  await expect(page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`)).toHaveClass(/selected/);
  await expect(det.locator('.card-title')).toContainText(TARGET);
  const row = (await api('/api/stats/aircraft/' + TARGET + '?addr_type=icao')).body;
  // Every ledger field once.
  const fields = await det.locator('.stats-detail-fields [data-field]').evaluateAll((els) => els.map((e) => e.getAttribute('data-field')));
  expect(fields.slice().sort()).toEqual(['addr_type', 'callsigns', 'category', 'class', 'country', 'drops', 'emergencies',
    'first_seen', 'hex', 'inputs', 'key', 'last_seen', 'max_alt_ft', 'max_gs_kt', 'max_range_bearing_deg', 'max_range_lat',
    'max_range_lon', 'max_range_nm', 'max_range_time', 'max_rssi', 'min_alt_ft', 'min_rssi', 'observations', 'positions',
    'registration', 'sent_updates', 'sessions', 'source', 'squawks']);
  const dd = (f) => det.locator(`[data-field="${f}"] dd`);
  await expect(dd('hex')).toHaveText(TARGET);
  await expect(dd('registration')).toHaveText(row.registration);
  await expect(dd('class')).toHaveText(row.class);
  await expect(dd('callsigns')).toContainText('UAL1423');
  await expect(dd('first_seen')).toHaveText(row.first_seen);
  await expect(dd('max_range_nm')).toHaveText(/^\d+\.\d nm$/);
  await expect(dd('max_alt_ft')).toHaveText(/ ft/);
  await expect(dd('max_gs_kt')).toHaveText(/ kt$/);
  // The mini polar: its max-range point at the row's bearing and range.
  await expect.poll(async () => (await statsState(page)).cards.find((c) => c.id === 'aircraft-detail').canvas).toBe(true);
  const o = await cardOption(page, 'aircraft-detail');
  expect(o.polar).toBeTruthy();
  const pt = o.series.find((s) => s.id === 'max');
  expect(pt.type).toBe('scatter');
  expect(pt.coordinateSystem).toBe('polar');
  expect(pt.data.length).toBe(1);
  const cur = (await api('/api/stats/aircraft/' + TARGET)).body;
  expect(Math.abs(pt.data[0][1] - cur.max_range_bearing_deg)).toBeLessThan(5);
  expect(pt.data[0][0]).toBeGreaterThan(0);
  expect(pt.data[0][0]).toBeLessThanOrEqual(o.radiusAxis[0].max);
  await det.scrollIntoViewIfNeeded();
  await det.screenshot({ path: testInfo.outputPath(`skycue-stats-aircraft-detail-${testInfo.project.name}.png`) });
  // Close hides it.
  await page.locator('#stats-detail-close').click();
  await expect(det).toBeHidden();
  expect((await statsState(page)).detailKey).toBe(null);
  await restoreMap(page);
});

test('stats_aircraft_show_on_map_selects_it', async ({ page }) => {
  await openPage(page);
  await waitMarkers(page, 10);
  await page.locator('#stats-toggle').click();
  await expect.poll(() => acRows(page).count(), { timeout: 15000 }).toBeGreaterThan(0);
  await searchAircraft(page, 'ual1423');
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`).click();
  // Live in the track table: the button shows.
  const btn = page.locator('#stats-show-on-map');
  await expect(btn).toBeVisible();
  await btn.click();
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  await expect(page.locator('#stats-view')).toBeHidden();
  expect(await page.evaluate(() => window.skycueApp.selected())).toBe(TARGET);
  await expect(page.locator('#details [data-field="hex"]')).toHaveText(TARGET);
  // An entry with no live track (a ledger row routed in): no button, and
  // the detail shows the row.
  await page.route(/\/api\/stats\/aircraft\/abcdef/, (route) => route.fulfill({ json: {
    key: 'abcdef|icao', hex: 'abcdef', addr_type: 'icao', first_seen: '2026-11-03T12:00:00.000Z',
    last_seen: '2026-11-03T12:05:00.000Z', observations: 3, positions: 0, sessions: 1, sent_updates: 0, drops: 0,
    emergencies: 0, callsigns: ['<b>X</b>'], squawks: [], inputs: [], source: 'adsb_1090', class: 'unknown' } }));
  await page.evaluate(() => window.skycueApp.showMain('stats'));
  await page.evaluate(() => window.skycueApp.stats.openAircraft('abcdef|icao'));
  await expect(page.locator('#stats-detail [data-field="hex"] dd')).toHaveText('abcdef');
  // API strings are text, never markup.
  await expect(page.locator('#stats-detail [data-field="callsigns"] dd')).toHaveText('<b>X</b>');
  await expect(page.locator('#stats-detail b')).toHaveCount(0);
  await expect(btn).toBeHidden();
  await expect(page.locator('#stats-filter-map')).toBeVisible();
  await page.unroute(/\/api\/stats\/aircraft\/abcdef/);
  await restoreMap(page);
});

test('stats_aircraft_filter_the_map_to_it', async ({ page }) => {
  await openAircraftSection(page);
  await searchAircraft(page, TARGET);
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`).click();
  await expect(page.locator('#stats-detail')).toBeVisible();
  await page.locator('#stats-filter-map').click();
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().search)).toBe(TARGET);
  await expect(page.locator('#view-search')).toHaveValue(TARGET);
  await expect.poll(() => page.evaluate(() => window.skycueApp.markers().map((m) => m.hex)), { timeout: 10000 })
    .toEqual([TARGET]);
  await restoreMap(page);
});

// An entry gone from the ledger (404, as after a reset) closes the detail
// on the next refresh.
test('stats_aircraft_detail_closes_on_404', async ({ page }) => {
  await openAircraftSection(page);
  await searchAircraft(page, TARGET);
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`).click();
  await expect(page.locator('#stats-detail')).toBeVisible();
  const entry = new RegExp('/api/stats/aircraft/' + TARGET);
  await page.route(entry, (route) => route.fulfill({ status: 404, json: { error: 'no such aircraft' } }));
  await page.evaluate(() => window.skycueApp.statsRefresh());
  await expect(page.locator('#stats-detail')).toBeHidden();
  expect((await statsState(page)).detailKey).toBe(null);
  await expect(page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`)).not.toHaveClass(/selected/);
  await page.unroute(entry);
  await restoreMap(page);
});

// Keyboard focus on a row survives the live rebuild; a search still in its
// debounce when the view is hidden is kept for the next open.
test('stats_aircraft_focus_and_pending_search', async ({ page }) => {
  await openAircraftSection(page);
  const key = (await acState(page)).rows[0];
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${key}"]`).focus();
  await page.evaluate(() => window.skycueApp.statsRefresh());
  await page.evaluate(() => window.skycueApp.statsRefresh());
  expect(await page.evaluate(() => document.activeElement && document.activeElement.getAttribute('data-key')))
    .toBe(key);
  await page.fill('#stats-aircraft-search', 'ual1423');
  await page.evaluate(() => window.skycueApp.showMain('map'));   // within the 300 ms
  expect((await acState(page)).q).toBe('ual1423');
  await page.evaluate(() => window.skycueApp.showMain('stats'));
  await expect.poll(async () => (await acState(page)).rows, { timeout: 10000 }).toEqual([TARGET_KEY]);
  await restoreMap(page);
});

// ---- Stats: Coverage, Fleet and Signal (spec 6 items 3-5) ----------------------
const COVERAGE_CARDS = ['coverage-polar', 'coverage-range', 'coverage-elevation', 'coverage-density'];
const FLEET_CARDS = ['fleet-altitude', 'fleet-speed', 'fleet-category', 'fleet-class', 'fleet-country',
  'fleet-source', 'fleet-addr-type'];
const SIGNAL_CARDS = ['signal-rssi-range', 'signal-rssi'];
const BAND_COLOURS = ['#ff7b72', '#f0883e', '#e3b341', '#7ee787', '#56d4dd', '#58a6ff', '#d2a8ff', '#6e7681'];
async function openCards(page, ids) {
  await openStats(page);
  await expect.poll(async () => (await statsState(page)).cards.filter((c) => ids.includes(c.id) && c.canvas)
    .map((c) => c.id), { timeout: 15000 }).toEqual(ids);
}
// The page point of a series' data item: the middle of a bar, or a point
// at `frac` of a sector's radius (a pie slice or a polar bar).
const itemPoint = (page, card, seriesId, index, frac) => page.evaluate(([cid, sid, i, f]) => {
  const el = document.querySelector('[data-card="' + cid + '"] .card-chart');
  const inst = window.echarts.getInstanceByDom(el);
  const model = inst.getModel().getSeries().find((s) => s.id === sid);
  const lay = model.getData().getItemLayout(i);
  const box = el.getBoundingClientRect();
  if (lay.r !== undefined) {
    const a = (lay.startAngle + lay.endAngle) / 2, r = lay.r0 + (lay.r - lay.r0) * f;
    return { x: box.left + lay.cx + r * Math.cos(a), y: box.top + lay.cy + r * Math.sin(a) };
  }
  return { x: box.left + lay.x + lay.width * f, y: box.top + lay.y + lay.height / 2 };
}, [card, seriesId, index, frac]);
async function clickItem(page, card, seriesId, index, frac) {
  await page.locator(`[data-card="${card}"]`).scrollIntoViewIfNeeded();
  const p = await itemPoint(page, card, seriesId, index, frac);
  await page.mouse.click(p.x, p.y);
}
// Hidden and shown again: the next refresh fetches coverage and rssi anew
// (open() makes the slow fetches due). Live off, so nothing changes after.
async function refetchSlow(page) {
  await page.locator('#stats-live').uncheck();
  const r0 = (await statsState(page)).refreshes;
  await page.evaluate(() => { window.skycueApp.showMain('map'); window.skycueApp.showMain('stats'); });
  await expect.poll(async () => (await statsState(page)).refreshes, { timeout: 10000 }).toBeGreaterThan(r0);
  await page.evaluate(() => window.skycueApp.statsRefresh());   // the open's refresh has drawn
}
// A shot of the box around some cards (scrolled to the first).
async function shootCards(page, ids, file) {
  await page.locator(`[data-card="${ids[0]}"]`).scrollIntoViewIfNeeded();
  await page.locator(`[data-card="${ids[0]}"]`).evaluate((el) => el.scrollIntoView({ block: 'start' }));
  const boxes = [];
  for (const id of ids) boxes.push(await page.locator(`[data-card="${id}"]`).boundingBox());
  const x = Math.min(...boxes.map((b) => b.x)), y = Math.min(...boxes.map((b) => b.y));
  const r = Math.max(...boxes.map((b) => b.x + b.width)), b = Math.max(...boxes.map((q) => q.y + q.height));
  await page.screenshot({ path: file, clip: { x, y, width: r - x, height: b - y } });
}
const pure = (page, fn, arg) => page.evaluate(([f, a]) => window.skycueStatsPure[f](a), [fn, arg]);

test('stats_coverage_polar_histograms_and_density', async ({ page }, testInfo) => {
  await openCards(page, COVERAGE_CARDS);
  // The whole-history maxima only grow while the replay runs: the chart
  // drawn between two reads lies between them.
  const cov = (await api('/api/stats/coverage')).body;
  await refetchSlow(page);
  const p = await cardOption(page, 'coverage-polar');
  const d = await cardOption(page, 'coverage-density');
  const cov2 = (await api('/api/stats/coverage')).body;
  const sum = (await api('/api/stats/summary')).body;
  expect(sum.receiver).toBeTruthy();
  expect(cov.cells.length).toBeGreaterThan(0);
  // Polar: one bar series per band, in the map's band colours, 72 sectors.
  expect(p.series.length).toBe(8);
  const byId = {};
  p.series.forEach((s) => { byId[s.id] = s; });
  for (let i = 0; i < 8; i++) {
    const s = byId['band:' + i];
    expect(s.type).toBe('bar');
    expect(s.coordinateSystem).toBe('polar');
    expect(s.name).toBe(cov.bands[i]);
    expect(s.itemStyle.color).toBe(BAND_COLOURS[i]);
    expect(s.data.length).toBe(72);
  }
  expect(p.angleAxis[0].type).toBe('category');
  expect(p.angleAxis[0].data.length).toBe(72);
  expect(p.legend[0].data).toEqual(cov.bands);
  // Each cell's largest range is its band's sector value.
  const later = {};
  cov2.cells.forEach((c) => { later[c.sector + ':' + c.band] = c.max_range_nm; });
  for (const c of cov.cells) {
    const v = byId['band:' + c.band].data[c.sector];
    expect(v).toBeGreaterThanOrEqual(c.max_range_nm - 1e-6);
    expect(v).toBeLessThanOrEqual(later[c.sector + ':' + c.band] + 1e-6);
  }
  expect(await cardCaption(page, 'coverage-polar')).toMatch(/^farthest \d+(\.\d)? nm · [0-9a-f]{6} · since /);
  // Range and elevation: one bar series each, labels "<start>–<end>".
  for (const id of ['coverage-range', 'coverage-elevation']) {
    const o = await cardOption(page, id);
    expect(o.series.length, id).toBe(1);
    expect(o.series[0].type).toBe('bar');
    expect(o.xAxis[0].data.length).toBe(o.series[0].data.length);
    // "<end>" first (it holds the values below), "<start>+" last.
    o.xAxis[0].data.forEach((l) => expect(l).toMatch(/^(<-?\d+|-?\d+–-?\d+|-?\d+\+)$/));
    expect(o.series[0].data.reduce((a, v) => a + v, 0)).toBeGreaterThan(0);
    expect(await cardCaption(page, id)).toMatch(/^\d{1,3}(,\d{3})* observations · since /);
  }
  // Density: cells, rings and the receiver on a square plot.
  expect(d.series.map((s) => [s.id, s.type])).toEqual([['cells', 'custom'], ['rings', 'line'], ['receiver', 'scatter']]);
  expect(d.series[0].data.length).toBeGreaterThanOrEqual(cov.grid.cells.length);
  expect(d.series[0].data.length).toBeLessThanOrEqual(cov2.grid.cells.length);
  expect(d.series[2].data).toEqual([[cov.receiver.lon, cov.receiver.lat]]);
  expect(d.series[1].data.filter((x) => x !== null).length % 73).toBe(0);
  expect(d.series[1].data.length).toBeGreaterThan(0);
  expect(d.grid[0].width).toBe(d.grid[0].height);
  const xa = d.xAxis[0], ya = d.yAxis[0];
  expect((xa.max - xa.min) * Math.cos(cov.receiver.lat * Math.PI / 180)).toBeCloseTo(ya.max - ya.min, 6);
  expect(await cardCaption(page, 'coverage-density')).toMatch(/^\d{1,3}(,\d{3})* cells · \d{1,3}(,\d{3})* positions/);
  const sec = sectionOf(page, '#stats-coverage');
  await sec.scrollIntoViewIfNeeded();
  await sec.screenshot({ path: testInfo.outputPath(`skycue-stats-coverage-${testInfo.project.name}.png`) });
});

// The "no receiver position" caption needs a daemon without RECEIVER_LAT/LON;
// every UI daemon here has one (the radar, FIS-B and receivers daemons too),
// so it is not tested in the browser.
test.skip('stats_coverage_caption_without_receiver', async () => {});

test('stats_coverage_polar_click_opens_record_holder', async ({ page }) => {
  await openCards(page, COVERAGE_CARDS);
  await page.locator('#stats-live').uncheck();
  const cov = (await api('/api/stats/coverage')).body;
  const far = cov.cells.reduce((m, c) => (!m || c.max_range_nm > m.max_range_nm ? c : m), null);
  // Near the tip of the farthest bar: no other bar of that sector reaches it.
  // Hovering shows the record holder's address and range.
  await page.locator('[data-card="coverage-polar"]').scrollIntoViewIfNeeded();
  const pt = await itemPoint(page, 'coverage-polar', 'band:' + far.band, far.sector, 0.95);
  await page.mouse.move(pt.x, pt.y);
  await expect(page.locator('[data-card="coverage-polar"] .card-chart'))
    .toContainText(far.key.split('|')[0] + '', { timeout: 5000 });
  await clickItem(page, 'coverage-polar', 'band:' + far.band, far.sector, 0.95);
  await expect(page.locator('#stats-detail')).toBeVisible();
  expect((await statsState(page)).detailKey).toBe(far.key);
  await expect(page.locator('#stats-detail .card-title')).toContainText(far.key.split('|')[0]);
  await page.locator('#stats-detail-close').click();
});

test('stats_fleet_histograms_donuts_and_countries', async ({ page }, testInfo) => {
  await openCards(page, FLEET_CARDS);
  const sum = (await api('/api/stats/summary')).body;
  for (const id of ['fleet-altitude', 'fleet-speed']) {
    const o = await cardOption(page, id);
    expect(o.series.map((s) => s.type), id).toEqual(['bar']);
    expect(o.xAxis[0].data.length).toBe(o.series[0].data.length);
    expect(o.series[0].data.reduce((a, v) => a + v, 0)).toBeGreaterThan(0);
  }
  for (const id of ['fleet-category', 'fleet-class', 'fleet-source', 'fleet-addr-type']) {
    const o = await cardOption(page, id);
    expect(o.series.map((s) => s.type), id).toEqual(['pie']);
    expect(o.series[0].data.length, id).toBeGreaterThan(0);
    const v = o.series[0].data.map((x) => x.value);
    for (let i = 1; i < v.length; i++) expect(v[i - 1]).toBeGreaterThanOrEqual(v[i]);
  }
  const names = async (id) => (await cardOption(page, id)).series[0].data.map((x) => x.name).sort();
  expect(await names('fleet-category')).toEqual(Object.keys(sum.counts.by_category).sort());
  expect(await names('fleet-source')).toEqual(Object.keys(sum.counts.by_source).sort());
  expect(await names('fleet-addr-type')).toEqual(Object.keys(sum.counts.by_addr_type).sort());
  for (const n of await names('fleet-class')) expect(['airline', 'private', 'military', 'unknown']).toContain(n);
  const c = await cardOption(page, 'fleet-country');
  expect(c.series.map((s) => s.type)).toEqual(['bar']);
  expect(c.yAxis[0].data.length).toBeGreaterThan(0);
  expect(c.yAxis[0].data.length).toBeLessThanOrEqual(16);
  expect(c.yAxis[0].data.length).toBe(c.series[0].data.length);
  expect(await cardCaption(page, 'fleet-class')).toMatch(/^\d{1,3}(,\d{3})* aircraft in the range$/);
  expect(await cardCaption(page, 'fleet-category')).toMatch(/observations · \d+ categor(y|ies) · since /);
  // The section is taller than the view: two shots, the first cards and the rest.
  await shootCards(page, FLEET_CARDS.slice(0, 4), testInfo.outputPath(`skycue-stats-fleet-${testInfo.project.name}.png`));
  await shootCards(page, FLEET_CARDS.slice(4), testInfo.outputPath(`skycue-stats-fleet-2-${testInfo.project.name}.png`));
});

test('stats_fleet_category_and_class_clicks_filter_the_map', async ({ page }) => {
  await openCards(page, FLEET_CARDS);
  await page.locator('#stats-live').uncheck();
  const code = (await cardOption(page, 'fleet-category')).series[0].data[0].name;
  const want = (await pure(page, 'categoryFilter', code)).category;
  await clickItem(page, 'fleet-category', 'donut', 0, 0.5);
  await expect.poll(() => page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().category)).toBe(want);
  await restoreMap(page);
  await page.evaluate(() => window.skycueApp.showMain('stats'));
  await expect.poll(async () => (await statsState(page)).cards.find((x) => x.id === 'fleet-class').canvas,
    { timeout: 15000 }).toBe(true);
  await page.locator('#stats-live').uncheck();
  const cls = (await cardOption(page, 'fleet-class')).series[0].data[0].name;
  await clickItem(page, 'fleet-class', 'donut', 0, 0.5);
  await expect.poll(() => page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().classes)).toEqual([cls]);
  await restoreMap(page);
});

test('stats_fleet_country_click_filters_the_map', async ({ page }) => {
  await openCards(page, FLEET_CARDS);
  await page.locator('#stats-live').uncheck();
  const list = (await cardOption(page, 'fleet-country')).yAxis[0].data;
  const i = list.findIndex((n) => n !== 'other');
  expect(i).toBeGreaterThanOrEqual(0);
  const want = (await pure(page, 'countryFilter', list[i])).country;
  await clickItem(page, 'fleet-country', 'countries', i, 0.5);
  await expect.poll(() => page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().country)).toBe(want);
  await restoreMap(page);
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().country)).toBe('');
});

test('stats_signal_rssi_vs_range_and_histogram', async ({ page }, testInfo) => {
  await openCards(page, SIGNAL_CARDS);
  const rssi = (await api('/api/stats/rssi')).body;
  await refetchSlow(page);
  const o = await cardOption(page, 'signal-rssi-range');
  const rssi2 = (await api('/api/stats/rssi')).body;
  expect(o.series.map((s) => [s.id, s.type])).toEqual([['cells', 'heatmap'], ['median', 'line']]);
  // The range axis ends a bin past the farthest column with counts.
  const lastCol = rssi2.counts.reduce((a, row, r) => (row.some((n) => n > 0) ? r : a), -1);
  expect(o.xAxis[0].data.length).toBeGreaterThanOrEqual(Math.min(10, rssi.counts.length));
  expect(o.xAxis[0].data.length).toBeLessThanOrEqual(Math.min(rssi.counts.length, Math.max(10, lastCol + 2)));
  o.series[0].data.forEach((c) => expect(c[0]).toBeLessThan(o.xAxis[0].data.length));
  // The signal axis: the levels with counts and one bin either side.
  const levels = (m) => {
    let lo = Infinity, hi = -1;
    m.counts.forEach((row) => row.forEach((v, b) => { if (v > 0) { lo = Math.min(lo, b); hi = Math.max(hi, b); } }));
    return Math.min(m.counts[0].length - 1, hi + 1) - Math.max(0, lo - 1) + 1;
  };
  expect(o.yAxis[0].data.length).toBeGreaterThanOrEqual(levels(rssi));
  expect(o.yAxis[0].data.length).toBeLessThanOrEqual(levels(rssi2));
  expect(o.yAxis[0].data.length).toBeLessThan(rssi.counts[0].length);
  // Cells only appear while the replay runs: the drawing lies between the two reads.
  const cols = (m) => m.counts.filter((row) => row.some((n) => n > 0)).length;
  const nonZero = (m) => m.counts.reduce((a, row) => a + row.filter((n) => n > 0).length, 0);
  expect(cols(rssi)).toBeGreaterThan(0);
  expect(o.series[1].data.length).toBeGreaterThanOrEqual(cols(rssi));
  expect(o.series[1].data.length).toBeLessThanOrEqual(cols(rssi2));
  expect(o.series[0].data.length).toBeGreaterThanOrEqual(nonZero(rssi));
  expect(o.series[0].data.length).toBeLessThanOrEqual(nonZero(rssi2));
  // Each median point sits on a bin of the rssi axis.
  o.series[1].data.forEach((m) => {
    expect(Number.isInteger(m[1])).toBe(true);
    expect(m[1]).toBeGreaterThanOrEqual(0);
    expect(m[1]).toBeLessThan(o.yAxis[0].data.length);
  });
  o.series[0].data.forEach((c) => expect(c[1]).toBeLessThan(o.yAxis[0].data.length));
  const h = await cardOption(page, 'signal-rssi');
  expect(h.series.map((s) => s.type)).toEqual(['bar']);
  expect(h.series[0].data.reduce((a, v) => a + v, 0)).toBeGreaterThan(0);
  expect(await cardCaption(page, 'signal-rssi-range')).toMatch(/observations with a range and signal · since /);
  const sec = sectionOf(page, '#stats-signal');
  await sec.scrollIntoViewIfNeeded();
  await sec.screenshot({ path: testInfo.outputPath(`skycue-stats-signal-${testInfo.project.name}.png`) });
});

// The remainder bar ('other', many countries) is muted, shows no pointer
// and its click changes nothing. The fixture has two countries, so the
// summary is answered with 17 and a remainder.
test('stats_fleet_country_other_bar_is_not_a_filter', async ({ page }) => {
  await openCards(page, FLEET_CARDS);
  await page.locator('#stats-live').uncheck();
  await page.route('**/api/stats/summary*', async (route) => {
    const res = await route.fetch();
    const body = await res.json();
    const by = [];
    for (let i = 0; i < 17; i++) by.push({ name: 'Canada', n: 1 });
    body.range.by_country = [{ name: 'United States', n: 30 }].concat(by.map((c, i) => ({ name: 'X' + i, n: 1 })),
      [{ name: 'other', n: 5 }]);
    await route.fulfill({ response: res, json: body });
  });
  await page.evaluate(() => window.skycueApp.statsRefresh());
  const o = await cardOption(page, 'fleet-country');
  const list = o.yAxis[0].data;
  expect(list.length).toBe(16);
  expect(list[15]).toBe('other');
  expect(o.series[0].data[15].cursor).toBe('default');
  const before = await page.evaluate(() => JSON.stringify(window.skycueApp.getViewFilters()));
  await clickItem(page, 'fleet-country', 'countries', 15, 0.5);
  await page.waitForTimeout(300);
  expect(await page.evaluate(() => window.skycueApp.mainView())).toBe('stats');
  expect(await page.evaluate(() => JSON.stringify(window.skycueApp.getViewFilters()))).toBe(before);
  await page.unroute('**/api/stats/summary*');
});

// From 1700 px (three columns) the wide cards take two columns, so every
// row of Coverage, Fleet and Signal is full; below, two columns, no spans.
test('stats_coverage_fleet_signal_rows_are_full', async ({ page }) => {
  await openCards(page, COVERAGE_CARDS.concat(FLEET_CARDS, SIGNAL_CARDS));
  const r = await page.evaluate(() => ['stats-coverage', 'stats-fleet', 'stats-signal'].map((id) => {
    const g = document.getElementById(id).getBoundingClientRect();
    return { id, gw: g.width, gr: g.right, cards: Array.from(document.querySelectorAll('#' + id + ' .card')).map((c) => {
      const b = c.getBoundingClientRect();
      return { id: c.getAttribute('data-card'), w: b.width, right: b.right, top: b.top };
    }) };
  }));
  const vw = await page.evaluate(() => window.innerWidth);
  const WIDE = ['coverage-range', 'coverage-elevation', 'fleet-altitude', 'fleet-addr-type', 'signal-rssi-range'];
  for (const sec of r) {
    for (const c of sec.cards) {
      if (vw >= 1700 && WIDE.includes(c.id)) {
        expect(c.w / sec.gw, c.id).toBeGreaterThan(0.6);
        expect(c.w / sec.gw, c.id).toBeLessThan(0.7);
      } else if (vw >= 1700) expect(c.w / sec.gw, c.id).toBeLessThan(0.35);
      else expect(c.w / sec.gw, c.id).toBeLessThan(0.55);
    }
    // The last card of each row ends at the grid's right edge (no orphan).
    if (vw >= 1700) {
      const rows = {};
      sec.cards.forEach((c) => { const k = Math.round(c.top); rows[k] = Math.max(rows[k] || 0, c.right); });
      Object.keys(rows).forEach((k) => expect(rows[k], sec.id + ' row ' + k).toBeCloseTo(sec.gr, 0));
    }
  }
});

// ---- Stats: the whole view, chart disposal, the ledger across a restart -------
// The chart cards a view on `summary` draws: Weather only once UAT was
// seen (the aircraft detail card, made empty, draws once a row is opened).
const allCards = (summary) => TRAFFIC_CARDS.concat(COVERAGE_CARDS, FLEET_CARDS, SIGNAL_CARDS,
  summary.uat_seen ? ['weather-uat'] : []).sort();
const drawnCards = async (page) => (await statsState(page)).cards.filter((c) => c.canvas).map((c) => c.id).sort();

// Every card and sparkline draws a canvas of some size, with ECharts from
// skycued itself (the afterEach hook fails on any request to another host).
test('stats_every_card_renders_canvas', async ({ page }, testInfo) => {
  await openAircraftSection(page);
  // The detail card too: open the fixture's target.
  await searchAircraft(page, TARGET);
  await page.locator(`#stats-aircraft-table tbody tr[data-key="${TARGET_KEY}"]`).click();
  await expect(page.locator('#stats-detail')).toBeVisible();
  await page.evaluate(() => window.skycueApp.statsRefresh());
  const want = allCards((await api('/api/stats/summary')).body).concat(['aircraft-detail']).sort();
  await expect.poll(() => drawnCards(page), { timeout: 15000 }).toEqual(want);
  const s = await statsState(page);
  expect(s.cards.map((c) => c.id).sort()).toEqual(want);
  expect(s.sparks.filter((x) => x.canvas).length).toBe(6);
  const sizes = await page.$$eval('#stats-view .card[data-card] .card-chart', (els) => els.map((el) => {
    const c = el.querySelector('canvas');
    return { id: el.closest('.card').getAttribute('data-card'), w: c ? c.width : 0, h: c ? c.height : 0 };
  }));
  expect(sizes.map((x) => x.id).sort()).toEqual(want);
  for (const x of sizes) {
    expect(x.w, x.id).toBeGreaterThan(100);
    expect(x.h, x.id).toBeGreaterThan(100);
  }
  const echartsFrom = await page.evaluate(() => performance.getEntriesByType('resource').map((e) => e.name)
    .filter((n) => /echarts/.test(n)).map((n) => n.startsWith(location.origin + '/')));
  expect(echartsFrom).toEqual([true]);
  // The shot shows the whole view with every card drawn (the search and
  // detail closed again).
  await page.locator('#stats-detail-close').click();
  await expect(page.locator('#stats-detail')).toBeHidden();
  await page.fill('#stats-aircraft-search', '');
  await expect.poll(async () => (await acState(page)).q).toBe('');
  await page.evaluate(() => document.getElementById('stats-view').scrollTo(0, 0));
  await page.screenshot({ path: testInfo.outputPath(`skycue-stats-${testInfo.project.name}.png`), fullPage: true });
  await restoreMap(page);
});

// Hidden past the dispose delay (shortened here from 60 s), every chart is
// disposed; shown again, each is made anew, drawn, and a Fleet click
// handler is bound to the new chart.
test('stats_charts_disposed_when_hidden_and_recreated', async ({ page }) => {
  await openStats(page);
  const want = allCards((await api('/api/stats/summary')).body);
  await expect.poll(() => drawnCards(page), { timeout: 15000 }).toEqual(want);
  const instances = () => page.evaluate(() => Array.from(document.querySelectorAll('#stats-view .card-chart, #stats-view .tile-spark'))
    .filter((el) => window.echarts.getInstanceByDom(el)).length);
  expect(await instances()).toBe(want.length + 6);
  await page.evaluate(() => window.skycueApp.stats.setDisposeMs(300));
  await page.evaluate(() => window.skycueApp.showMain('map'));
  await expect.poll(async () => {
    const st = await statsState(page);
    return st.cards.concat(st.sparks).filter((c) => c.canvas).length;
  }, { timeout: 5000 }).toBe(0);
  expect(await instances()).toBe(0);
  await page.evaluate(() => window.skycueApp.stats.setDisposeMs(-1));
  await page.evaluate(() => window.skycueApp.showMain('stats'));
  await expect.poll(() => drawnCards(page), { timeout: 15000 }).toEqual(want);
  await expect.poll(async () => (await statsState(page)).sparks.filter((x) => x.canvas).length).toBe(6);
  expect(await instances()).toBe(want.length + 6);
  await page.locator('#stats-live').uncheck();
  await expect.poll(async () => {
    const o = await cardOption(page, 'fleet-category');
    return o && o.series[0].data.length;
  }, { timeout: 10000 }).toBeGreaterThan(0);
  const code = (await cardOption(page, 'fleet-category')).series[0].data[0].name;
  const filter = (await pure(page, 'categoryFilter', code)).category;
  await clickItem(page, 'fleet-category', 'donut', 0, 0.5);
  await expect.poll(() => page.evaluate(() => window.skycueApp.mainView())).toBe('map');
  expect(await page.evaluate(() => window.skycueApp.getViewFilters().category)).toBe(filter);
  await restoreMap(page);
});

// ---- Stats: CSV and PNG export (operator tools spec 5) -------------------------
// A click that downloads a file: its name, text and CRLF records (the last
// element, after the final CRLF, is the empty string).
async function csvDownload(page, selector) {
  const [dl] = await Promise.all([page.waitForEvent('download'), page.locator(selector).click()]);
  const text = fs.readFileSync(await dl.path(), 'utf8');
  const lines = text.split('\r\n');
  return { name: dl.suggestedFilename(), text, lines, records: lines.length - 2,
    keys: lines.slice(1, -1).map((l) => l.split(',')[0]) };
}
const LEDGER_CSV_HEADER = 'key,hex,addr_type,first_seen,last_seen,observations,positions,sessions,sent_updates,drops,' +
  'emergencies,max_range_nm,max_range_time,max_range_lat_deg,max_range_lon_deg,max_range_bearing_deg,min_alt_ft,' +
  'max_alt_ft,max_gs_kt,min_rssi_dbfs,max_rssi_dbfs,callsigns,squawks,inputs,registration,category,source,class,country';

test('stats_csv_downloads_three_cards', async ({ page }) => {
  await openAircraftSection(page);
  await page.locator('#stats-live').uncheck();
  const AC = '#stats-aircraft-csv';
  await expect(page.locator(AC)).toHaveText('CSV');
  await expect(page.locator(AC)).toBeEnabled();
  // The whole ledger: the header with units, one record per entry.
  const before = (await api('/api/stats/aircraft?limit=1000')).body.total;
  const all = await csvDownload(page, AC);
  const after = (await api('/api/stats/aircraft?limit=1000')).body.total;
  expect(all.name).toMatch(/^skycue-aircraft-\d{4}-\d{2}-\d{2}\.csv$/);
  expect(all.lines[0]).toBe(LEDGER_CSV_HEADER);
  expect(all.text.charCodeAt(0)).not.toBe(0xfeff);                 // no BOM
  // The 20x replay keeps adding entries: the export lands between the reads.
  expect(before <= all.records && all.records <= after,
    'CSV records ' + all.records + ' between the ledger totals ' + before + ' and ' + after).toBe(true);
  expect(new Set(all.keys).size).toBe(all.keys.length);
  await expect(page.locator('#stats-aircraft-caption')).not.toHaveText(/Exporting/);
  // In the table's sort: the export and an API read taken at once agree
  // (live data can reorder between them: the pair is read again).
  await page.locator('#stats-aircraft-table th[data-sort="max_range_nm"]').click();
  await expect.poll(async () => (await acState(page)).sort).toBe('max_range_nm');
  await expect.poll(async () => {
    const f = await csvDownload(page, AC);
    const a = (await api('/api/stats/aircraft?sort=max_range_nm&dir=desc&limit=12')).body.rows.map((r) => r.key);
    return f.keys.slice(0, 12).join() === a.join();
  }, { timeout: 20000 }).toBe(true);
  // And the search: one record, the target's.
  await page.fill('#stats-aircraft-search', 'a4f2');
  await expect.poll(async () => (await acState(page)).rows).toEqual([TARGET_KEY]);
  const one = await csvDownload(page, AC);
  expect(one.records).toBe(1);
  expect(one.lines[1].startsWith(TARGET_KEY + ',')).toBe(true);

  // The range's series: a record per bucket.
  await page.evaluate(() => window.skycueApp.statsRefresh());
  const TI = '[data-card="traffic-inputs"] .card-action';
  await expect(page.locator(TI)).toHaveText('CSV');
  await expect(page.locator(TI)).toBeEnabled();
  const ser = await csvDownload(page, TI);
  expect(ser.name).toMatch(/^skycue-observations-by-input-\d{4}-\d{2}-\d{2}\.csv$/);
  expect(ser.lines[0].startsWith('time,observations,positions,aircraft')).toBe(true);
  expect(ser.records).toBe((await statsState(page)).seriesBuckets);
  expect(ser.records).toBeGreaterThan(0);

  // The coverage table: 72 sectors x 8 bands.
  const CP = '[data-card="coverage-polar"] .card-action';
  await expect(page.locator(CP)).toHaveText('CSV');
  await expect(page.locator(CP)).toBeEnabled({ timeout: 15000 });
  const cov = await csvDownload(page, CP);
  expect(cov.name).toMatch(/^skycue-coverage-\d{4}-\d{2}-\d{2}\.csv$/);
  expect(cov.lines[0]).toBe('sector_start_deg,band,max_range_nm,holder,time,n');
  expect(cov.records).toBe(72 * 8);                                // 577 lines with the header
  await restoreMap(page);
});

// Every card chart has the toolbox's PNG save on the page's dark
// background, named after its card; its icon sits at the chart's top right.
test('stats_cards_save_as_image', async ({ page }) => {
  await openStats(page);
  const want = allCards((await api('/api/stats/summary')).body);
  await expect.poll(() => drawnCards(page), { timeout: 15000 }).toEqual(want);
  const saves = await page.evaluate(() => Array.from(document.querySelectorAll('#stats-view .card .card-chart'))
    .map((el) => {
      const inst = window.echarts.getInstanceByDom(el);
      if (!inst) return null;
      const f = inst.getOption().toolbox[0].feature.saveAsImage;
      return { id: el.closest('.card').getAttribute('data-card'), bg: f.backgroundColor, name: f.name };
    }).filter((x) => x));
  expect(saves.map((s) => s.id).sort()).toEqual(want);
  for (const s of saves) {
    expect(s.bg, s.id).toBe('#0d1117');
    expect(s.name.startsWith('skycue-' + s.id + '-'), s.id).toBe(true);
  }
  await page.locator('#stats-live').uncheck();
  const chart = page.locator('[data-card="traffic-inputs"] .card-chart');
  await chart.scrollIntoViewIfNeeded();
  const box = await chart.boundingBox();
  const [dl] = await Promise.all([page.waitForEvent('download'),
    page.mouse.click(box.x + box.width - 10, box.y + 10)]);
  expect(dl.suggestedFilename()).toMatch(/^skycue-traffic-inputs-\d{4}-\d{2}-\d{2}\.png$/);
  await restoreMap(page);
});

// The "stats" skycued (run.sh) replays the fixture at 20x and saves its
// statistics to stats.json. Stopped and started again, it reloads the
// ledger (every entry kept, `since` the same), while the replay's clock
// starts over behind the reloaded series, which therefore restart (shorter
// than before) with an INFO line in its log.
const STATS_BASE = process.env.SKYCUE_STATS_URL;
const nodePath = require('path');
async function statsDaemonUp() {
  try {
    await fetch(STATS_BASE + '/api/stats');
    return true;
  } catch (e) {
    return false;
  }
}
// Exited (no /proc entry, or a zombie its parent has not reaped yet).
function processGone(pid) {
  try {
    const st = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
    return st.slice(st.lastIndexOf(')') + 2, st.lastIndexOf(')') + 3) === 'Z';
  } catch (e) {
    return true;
  }
}
const restartLines = () => (fs.readFileSync(process.env.SKYCUE_STATS_LOG, 'utf8')
  .match(/INFO {2}stats: the data clock moved backwards to .*; the series restart/g) || []).length;

test('stats_ledger_survives_restart', async () => {
  // A few minutes of series first (a minute bucket every 3 s at 20x).
  const minutes = async () => (await apiAt(STATS_BASE, '/api/stats/series')).body.t.length;
  await expect.poll(minutes, { timeout: 60000 }).toBeGreaterThanOrEqual(6);
  const s0 = (await apiAt(STATS_BASE, '/api/stats/summary')).body;
  const m0 = await minutes();
  const rows0 = (await apiAt(STATS_BASE, '/api/stats/aircraft?sort=observations&limit=1000')).body.rows;
  expect(s0.aircraft_total).toBeGreaterThanOrEqual(12);   // the fixture's 12, at least
  expect(rows0.length).toBe(s0.aircraft_total);
  const k = rows0[0];
  const lines0 = restartLines();
  const file = nodePath.join(nodePath.dirname(process.env.SKYCUE_STATS_LOG), 'stats.json');

  const pid = Number(fs.readFileSync(process.env.SKYCUE_STATS_PID_FILE, 'utf8'));
  process.kill(pid, 'SIGTERM');
  await expect.poll(statsDaemonUp, { timeout: 10000 }).toBe(false);
  await expect.poll(() => processGone(pid), { timeout: 10000 }).toBe(true);
  // Saved on the way out: the file holds the ledger.
  expect(fs.readFileSync(file, 'utf8')).toContain(JSON.stringify(k.key));
  const child = childProcess.spawn('bash', ['-c', process.env.SKYCUE_STATS_START_CMD], { detached: true, stdio: 'ignore' });
  child.unref();
  await expect.poll(statsDaemonUp, { timeout: 10000 }).toBe(true);

  // The series restarted: shorter than before (it takes ~3 s per minute to grow again).
  await expect.poll(minutes, { timeout: 15000 }).toBeLessThan(m0);
  await expect.poll(restartLines, { timeout: 15000 }).toBeGreaterThan(lines0);
  // The ledger survived.
  const s1 = (await apiAt(STATS_BASE, '/api/stats/summary')).body;
  expect(s1.since).toBe(s0.since);
  expect(s1.aircraft_total).toBeGreaterThanOrEqual(s0.aircraft_total);
  const r = await apiAt(STATS_BASE, `/api/stats/aircraft/${k.hex}?addr_type=${k.addr_type}`);
  expect(r.status).toBe(200);
  expect(r.body.key).toBe(k.key);
  expect(r.body.first_seen).toBe(k.first_seen);
  expect(r.body.observations).toBeGreaterThanOrEqual(k.observations);
});
