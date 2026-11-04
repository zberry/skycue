'use strict';
// Tests for the alert strip's pure logic (www/alerts_pure.js): the rules
// engine, its dedupe and quiet period, the input hold-off and the storage
// normalisers. Plain node, no test framework (the same harness as
// stats_pure.test.js).
// usage: node alerts_pure.test.js [path/to/alerts_pure.js]
// alerts_pure.js loads app_pure.js from its own directory under node.
const path = require('path');
const assert = require('assert');

const A = require(path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'www', 'alerts_pure.js')));

let failed = 0;
let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log('ok   ' + name);
  } catch (e) {
    failed++;
    console.log('FAIL ' + name + ': ' + (e && e.message));
  }
}

const T0 = Date.parse('2026-11-04T12:00:00.000Z'), W0 = 1000000;
const tr = (o) => Object.assign({ hex: 'a1b2c3', addr_type: 'icao', callsign: 'UAL123', lat: 39, lon: -77,
  decision: 'send', range_nm: 40, alt_baro_ft: 30000, on_ground: false, squawk: '1200', emergency: 'none' }, o);
const cfg = (rules, watch) => { const c = A.defaultConfig(); Object.keys(c.rules).forEach((id) => { c.rules[id].enabled = false; });
  Object.keys(rules || {}).forEach((id) => Object.assign(c.rules[id], rules[id])); c.watch = watch || []; return c; };
const fresh = (rules, watch) => { const e = A.createEngine(cfg(rules, watch), null); e.beginBaseline(); e.endBaseline([]); return e; };
const inp = (c) => [{ id: 'ps-1', name: 'North', enabled: true, state: { connected: c } }];

// The fields of `row` named in `want` equal `want`.
function like(row, want) {
  const got = {};
  Object.keys(want).forEach((k) => { got[k] = row[k]; });
  assert.deepStrictEqual(got, want);
}
const ruleIds = (rows) => rows.map((r) => r.rule);
// The single row a call raised.
function one(rows) {
  assert.strictEqual(rows.length, 1, 'want one row, got ' + JSON.stringify(rows));
  return rows[0];
}

test('normalizeConfig: defaults', () => {
  assert.deepStrictEqual(A.RULES.map((r) => [r.id, r.title, r.enabled, r.sound, r.pattern, r.params]), [
    ['emergency', 'Emergency', true, true, 'double', {}],
    ['military_new', 'Military', true, false, 'single', {}],
    ['sent_new', 'Sent', true, false, 'single', {}],
    ['input_lost', 'Input lost', true, true, 'double', {}],
    ['range_enter', 'Range', false, false, 'single', { threshold_nm: 25 }],
    ['altitude_below', 'Low altitude', false, false, 'single', { altitude_ft: 1000, range_nm: 10 }],
    ['new_country', 'New country', false, false, 'single', {}],
    ['watch', 'Watch', false, false, 'single', {}],
    ['drop_filtered', 'Filtered drop', false, false, 'single', {}]
  ]);
  assert.deepStrictEqual(A.PARAM_LIMITS, { threshold_nm: [0.1, 500], altitude_ft: [0, 60000], range_nm: [0.1, 500] });
  assert.deepStrictEqual([A.QUIET_MS, A.HOLD_OFF_MS, A.LOG_MAX, A.PAIRS_MAX, A.WATCH_MAX, A.WATCH_LEN, A.DAEMON_ID],
    [300000, 10000, 200, 1000, 200, 16, 'skycued']);
  const d = A.defaultConfig();
  assert.deepStrictEqual(A.normalizeConfig(null), d);
  assert.deepStrictEqual(Object.keys(d.rules), A.RULES.map((r) => r.id));
  assert.deepStrictEqual(d.rules.altitude_below, { enabled: false, sound: false, altitude_ft: 1000, range_nm: 10 });
  assert.deepStrictEqual(d.rules.range_enter, { enabled: false, sound: false, threshold_nm: 25 });
  assert.deepStrictEqual(d.rules.emergency, { enabled: true, sound: true });
  assert.strictEqual(d.version, 1);
  assert.strictEqual(d.mute, false);
  assert.strictEqual(d.hidden, false);
  assert.strictEqual(d.drawer, false);
  assert.deepStrictEqual(d.watch, []);
  assert.deepStrictEqual(A.normalizeConfig('not json'), d);
  // Fresh copies: changing one default does not change the next.
  d.rules.emergency.enabled = false;
  assert.strictEqual(A.defaultConfig().rules.emergency.enabled, true);
});

test('normalizeConfig: field by field', () => {
  const c = A.normalizeConfig('{"mute":1,"rules":{"range_enter":{"enabled":true,"threshold_nm":-3},' +
    '"emergency":{"sound":"yes"},"bogus":{}},"watch":[" ual ","UAL","n123","",7]}');
  assert.strictEqual(c.mute, true);
  assert.strictEqual(c.hidden, false);
  assert.deepStrictEqual(c.rules.range_enter, { enabled: true, sound: false, threshold_nm: 25 });
  assert.deepStrictEqual(c.rules.emergency, { enabled: true, sound: true });
  assert.strictEqual(Object.prototype.hasOwnProperty.call(c.rules, 'bogus'), false);
  assert.deepStrictEqual(Object.keys(c.rules), A.RULES.map((r) => r.id));
  assert.deepStrictEqual(c.watch, ['ual', 'n123']);
  // A parameter inside its limits is kept; outside or not a number falls back.
  const p = A.normalizeConfig({ rules: { altitude_below: { altitude_ft: 60000, range_nm: '5' }, range_enter: { threshold_nm: 0.1 } } });
  assert.deepStrictEqual(p.rules.altitude_below, { enabled: false, sound: false, altitude_ft: 60000, range_nm: 10 });
  assert.strictEqual(p.rules.range_enter.threshold_nm, 0.1);
  assert.strictEqual(A.normalizeConfig({ rules: { range_enter: { threshold_nm: 500.5 } } }).rules.range_enter.threshold_nm, 25);
});

test('normalizeWatch: lines, length and count caps', () => {
  assert.deepStrictEqual(A.normalizeWatch('a\n b \n\nA\n' + 'x'.repeat(20)), ['a', 'b', 'xxxxxxxxxxxxxxxx']);
  const many = [];
  for (let i = 0; i < 250; i++) many.push('e' + i);
  const w = A.normalizeWatch(many);
  assert.strictEqual(w.length, 200);
  assert.strictEqual(w[199], 'e199');
  assert.deepStrictEqual(A.normalizeWatch(['N1', null, 3, ' n1 ', 'dal']), ['N1', 'dal']);
  assert.deepStrictEqual(A.normalizeWatch(null), []);
});

test('watchMatch: prefix over hex, callsign, registration', () => {
  const watch = ['ual', 'A1B2', 'n37'];
  assert.strictEqual(A.watchMatch(tr({ callsign: 'UAL1423 ' }), watch), 'ual');
  assert.strictEqual(A.watchMatch(tr({ callsign: 'DAL1' }), watch), 'A1B2');
  assert.strictEqual(A.watchMatch(tr({ callsign: 'X', hex: 'ffffff', registration: 'N37502' }), watch), 'n37');
  assert.strictEqual(A.watchMatch(tr({ callsign: 'XUAL', hex: 'ffffff' }), watch), null);
  assert.strictEqual(A.watchMatch(tr(), []), null);
});

test('emergency: once until ack', () => {
  const e = fresh({ emergency: { enabled: true } });
  const r = one(e.track(tr({ squawk: '7700' }), T0));
  like(r, { rule: 'emergency', title: 'Emergency', label: 'UAL123', detail: 'squawk 7700', key: 'a1b2c3|icao',
    input: null, acked: false, ackTime: null, quiet: false, recovered: null, pattern: 'double', sound: true,
    time: '2026-11-04T12:00:00.000Z' });
  assert.strictEqual(e.hasUnackedEmergency(), true);
  assert.deepStrictEqual(e.track(tr({ squawk: '7700' }), T0 + 1000), []);
  assert.deepStrictEqual(e.track(tr({ squawk: '1200' }), T0 + 2000), []);
  assert.deepStrictEqual(e.track(tr({ squawk: '7700' }), T0 + 3000), []);
  assert.strictEqual(e.rows().length, 1);
  assert.strictEqual(e.ack(r.id, T0 + 4000), true);
  assert.strictEqual(e.ack(r.id, T0 + 5000), false);
  assert.strictEqual(e.hasUnackedEmergency(), false);
  like(e.rows()[0], { acked: true, ackTime: '2026-11-04T12:00:04.000Z' });
});

test('emergency: detail text', () => {
  const e = fresh({ emergency: { enabled: true } });
  assert.strictEqual(one(e.track(tr({ hex: 'b00001', emergency: 'general' }), T0)).detail, 'emergency general');
  assert.strictEqual(one(e.track(tr({ hex: 'b00002', emergency: 'lifeguard_medical' }), T0)).detail,
    'emergency lifeguard medical');
  assert.strictEqual(one(e.track(tr({ hex: 'b00003', squawk: '7600' }), T0)).detail, 'squawk 7600');
  assert.strictEqual(one(e.track(tr({ hex: 'b00004', squawk: '7500', emergency: 'unlawful_interference' }), T0)).detail,
    'squawk 7500');
});

test('quiet period after acknowledgement', () => {
  const e = fresh({ emergency: { enabled: true } });
  const r = one(e.track(tr({ squawk: '7700' }), T0));
  e.ack(r.id, T0 + 1000);
  assert.deepStrictEqual(e.track(tr(), T0 + 2000), []);
  assert.deepStrictEqual(e.track(tr({ squawk: '7700' }), T0 + 60000), []);
  assert.strictEqual(e.rows().length, 2);
  like(e.rows()[0], { rule: 'emergency', quiet: true, acked: true, ackTime: '2026-11-04T12:01:00.000Z' });
  assert.strictEqual(e.unacked().length, 0);
  assert.deepStrictEqual(e.track(tr(), T0 + 70000), []);
  const again = one(e.track(tr({ squawk: '7700' }), T0 + 1000 + 300000));
  like(again, { quiet: false, acked: false, ackTime: null });
  assert.strictEqual(e.unacked().length, 1);
});

test('baseline: state rules fire, new rules seed', () => {
  const all = {};
  A.RULES.forEach((r) => { all[r.id] = { enabled: true }; });
  const e = A.createEngine(cfg(all), null);
  e.beginBaseline();
  assert.deepStrictEqual(ruleIds(e.track(tr({ hex: 'ae0001', squawk: '7700' }), T0)), ['emergency']);
  e.endBaseline(['ae0001|icao']);
  assert.deepStrictEqual(e.track(tr({ hex: 'ae0001', squawk: '7700' }), T0 + 1000), []);
  const r = e.track(tr({ hex: 'ae0002' }), T0 + 2000);
  assert.deepStrictEqual(ruleIds(r), ['military_new', 'sent_new']);
  like(r[0], { label: 'UAL123', detail: 'first seen military', key: 'ae0002|icao', pattern: 'single', sound: false });
});

test('military_new: once per engine life', () => {
  const e = fresh({ military_new: { enabled: true } });
  const t = tr({ hex: 'ae0002' });
  const r = one(e.track(t, T0));
  like(r, { rule: 'military_new', title: 'Military', detail: 'first seen military', key: 'ae0002|icao' });
  e.ack(r.id, T0 + 500);
  e.beginBaseline();
  assert.deepStrictEqual(e.track(t, T0 + 1000), []);
  e.endBaseline(['ae0002|icao']);
  e.trackGone('ae0002|icao');
  assert.deepStrictEqual(e.track(t, T0 + 2000), []);
  assert.deepStrictEqual(e.track(t, T0 + 2000 + 600000), []);
  // Not a quiet row either: the seen set, not the dedupe, stops it.
  assert.strictEqual(e.rows().length, 1);
  // A non-military track never fires it.
  assert.deepStrictEqual(e.track(tr({ hex: 'b00001' }), T0 + 3000), []);
});

test('sent_new: send and allow_listed', () => {
  const e = fresh({ sent_new: { enabled: true } });
  assert.deepStrictEqual(e.track(tr({ decision: 'out_of_range' }), T0), []);
  like(one(e.track(tr(), T0 + 1000)), { rule: 'sent_new', title: 'Sent', detail: 'sent to cue clients', key: 'a1b2c3|icao' });
  assert.deepStrictEqual(e.track(tr({ decision: 'out_of_range' }), T0 + 2000), []);
  assert.deepStrictEqual(e.track(tr(), T0 + 3000), []);
  like(one(e.track(tr({ hex: 'b00001', decision: 'allow_listed' }), T0 + 4000)),
    { detail: 'allow listed, sent', key: 'b00001|icao' });
});

test('range_enter: threshold, edge, enable never fires', () => {
  const e = fresh({ range_enter: { enabled: true } });
  assert.deepStrictEqual(e.track(tr({ range_nm: 30 }), T0), []);
  assert.deepStrictEqual(e.track(tr({ range_nm: 25 }), T0 + 1000), []);
  like(one(e.track(tr({ range_nm: 24.5 }), T0 + 2000)), { rule: 'range_enter', title: 'Range', detail: 'inside 25 nm (24.5 nm)' });
  assert.deepStrictEqual(e.track(tr({ range_nm: 20 }), T0 + 3000), []);
  const off = fresh({});
  assert.deepStrictEqual(off.track(tr({ range_nm: 30 }), T0), []);
  assert.deepStrictEqual(off.track(tr({ range_nm: 24.5 }), T0 + 1000), []);
  off.setConfig(cfg({ range_enter: { enabled: true } }), [tr({ range_nm: 24.5 })]);
  assert.deepStrictEqual(off.track(tr({ range_nm: 24 }), T0 + 2000), []);
  assert.strictEqual(off.rows().length, 0);
  assert.strictEqual(off.config().rules.range_enter.enabled, true);
});

test('altitude_below: airborne, low, near', () => {
  const e = fresh({ altitude_below: { enabled: true } });
  assert.deepStrictEqual(e.track(tr({ alt_baro_ft: 800, range_nm: 12 }), T0), []);
  like(one(e.track(tr({ alt_baro_ft: 800, range_nm: 8 }), T0 + 1000)),
    { rule: 'altitude_below', title: 'Low altitude', detail: 'below 1,000 ft within 10 nm (800 ft, 8.0 nm)' });
  assert.deepStrictEqual(e.track(tr({ hex: 'c00001', on_ground: true, range_nm: 5 }), T0), []);
  assert.deepStrictEqual(e.track(tr({ hex: 'c00002', alt_baro_ft: 1000, range_nm: 5 }), T0), []);
  assert.strictEqual(one(e.track(tr({ hex: 'c00003', alt_baro_ft: 999, range_nm: 10 }), T0)).detail,
    'below 1,000 ft within 10 nm (999 ft, 10.0 nm)');
});

test('new_country: first per engine', () => {
  const e = fresh({ new_country: { enabled: true } });
  like(one(e.track(tr({ hex: 'a00001' }), T0)),
    { rule: 'new_country', title: 'New country', detail: 'first aircraft from United States', key: 'a00001|icao' });
  assert.deepStrictEqual(e.track(tr({ hex: 'a00002' }), T0 + 1000), []);
  assert.deepStrictEqual(e.track(tr({ hex: 'b00001', addr_type: 'non_icao' }), T0 + 2000), []);
  assert.deepStrictEqual(e.track(tr({ hex: 'fff701' }), T0 + 3000), []);
});

test('watch: from the snapshot and on a callsign change', () => {
  const e = A.createEngine(cfg({ watch: { enabled: true } }, ['ual']), null);
  e.beginBaseline();
  like(one(e.track(tr({ callsign: 'UAL9' }), T0)),
    { rule: 'watch', title: 'Watch', label: 'UAL9', detail: 'matches watch entry "ual"' });
  e.endBaseline(['a1b2c3|icao']);
  assert.deepStrictEqual(e.track(tr({ hex: 'b00001', callsign: 'DAL1' }), T0 + 1000), []);
  like(one(e.track(tr({ hex: 'b00001', callsign: 'UAL5' }), T0 + 2000)), { label: 'UAL5', key: 'b00001|icao' });
});

test('drop_filtered: filtered drops only', () => {
  const e = fresh({ drop_filtered: { enabled: true } });
  assert.deepStrictEqual(e.drop({ hex: 'a1b2c3', addr_type: 'icao', reason: 'timeout' }, T0), []);
  const d = { hex: 'a1b2c3', addr_type: 'icao', reason: 'filtered', decision: 'out_of_range', callsign: 'UAL1' };
  const r = one(e.drop(d, T0 + 1000));
  like(r, { rule: 'drop_filtered', title: 'Filtered drop', label: 'UAL1', detail: 'dropped: out of range', key: 'a1b2c3|icao' });
  e.ack(r.id, T0 + 1000);
  // Every filtered drop is a transition: after the quiet period it fires again.
  assert.strictEqual(one(e.drop(d, T0 + 1000 + 300000)).label, 'UAL1');
  assert.strictEqual(one(e.drop({ hex: 'b00001', addr_type: 'icao', reason: 'filtered', decision: 'blocked' }, T0)).label,
    'b00001');
});

test('setConfig: a parameter change re-seeds silently', () => {
  const e = fresh({ range_enter: { enabled: true } });
  assert.deepStrictEqual(e.track(tr({ range_nm: 40 }), T0), []);
  e.setConfig(cfg({ range_enter: { enabled: true, threshold_nm: 50 } }), [tr({ range_nm: 40 })]);
  assert.strictEqual(e.rows().length, 0);
  assert.deepStrictEqual(e.track(tr({ range_nm: 40 }), T0 + 1000), []);
  assert.strictEqual(one(e.track(tr({ hex: 'b00002', range_nm: 45 }), T0 + 2000)).detail, 'inside 50 nm (45.0 nm)');
  // A watch list change re-seeds the watch pairs the same way.
  const w = fresh({ watch: { enabled: true } });
  assert.deepStrictEqual(w.track(tr(), T0), []);
  w.setConfig(cfg({ watch: { enabled: true } }, ['ual']), [tr()]);
  assert.deepStrictEqual(w.track(tr(), T0 + 1000), []);
  assert.strictEqual(w.rows().length, 0);
});

test('endBaseline: a pair whose track is gone is cleared', () => {
  const e = fresh({ emergency: { enabled: true } });
  e.ack(one(e.track(tr({ squawk: '7700' }), T0)).id, T0);
  e.beginBaseline();
  e.endBaseline([]);
  assert.strictEqual(e.track(tr({ squawk: '7700' }), T0 + 360000).length, 1);
  const c = fresh({ emergency: { enabled: true } });
  c.ack(one(c.track(tr({ squawk: '7700' }), T0)).id, T0);
  c.beginBaseline();
  c.endBaseline(['a1b2c3|icao']);
  assert.deepStrictEqual(c.track(tr({ squawk: '7700' }), T0 + 360000), []);
});

test('trackGone: clears the condition', () => {
  const e = fresh({ emergency: { enabled: true } });
  e.ack(one(e.track(tr({ squawk: '7700' }), T0)).id, T0);
  e.trackGone('a1b2c3|icao');
  assert.strictEqual(e.track(tr({ squawk: '7700' }), T0 + 300000).length, 1);
});

test('input_lost: seed, hold-off, recovery', () => {
  const e = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
  e.beginBaseline();
  assert.deepStrictEqual(e.inputs(inp(false), T0, W0), []);
  assert.deepStrictEqual(e.inputs(inp(true), T0 + 1000, W0 + 1000), []);
  e.endBaseline([]);
  assert.deepStrictEqual(e.inputs(inp(false), T0 + 2000, W0 + 2000), []);
  assert.deepStrictEqual(e.tick(T0 + 11999, W0 + 11999), []);
  const r = e.tick(T0 + 12000, W0 + 12000);
  assert.strictEqual(r.length, 1);
  like(r[0], { rule: 'input_lost', title: 'Input lost', label: 'North', detail: 'disconnected', input: 'ps-1',
    key: 'input:ps-1', pattern: 'double', sound: true, acked: false });
  assert.deepStrictEqual(e.tick(T0 + 13000, W0 + 13000), []);
  assert.deepStrictEqual(e.inputs(inp(true), T0 + 74000, W0 + 74000), []);
  like(e.rows()[0], { id: r[0].id, acked: true, ackTime: new Date(T0 + 74000).toISOString(),
    recovered: 'recovered after 1 min 12 s' });
  assert.strictEqual(e.unacked().length, 0);
  // A seed call that sees an input already down starts no hold-off.
  const s = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
  s.beginBaseline();
  s.inputs(inp(false), T0, W0);
  s.endBaseline([]);
  assert.deepStrictEqual(s.inputs(inp(false), T0 + 60000, W0 + 60000), []);
  assert.deepStrictEqual(s.tick(T0 + 120000, W0 + 120000), []);
  // A seed call re-records a known input too: one down there (here inside
  // its hold-off) starts none.
  const k = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
  k.inputs(inp(true), T0, W0);
  k.inputs(inp(false), T0 + 1000, W0 + 1000);
  k.beginBaseline();
  k.inputs(inp(false), T0 + 2000, W0 + 2000);
  k.endBaseline([]);
  assert.deepStrictEqual(k.tick(T0 + 60000, W0 + 60000), []);
  assert.deepStrictEqual(k.inputs(inp(false), T0 + 61000, W0 + 61000), []);
});

test('input_lost: back early, disabled, removed', () => {
  const seeded = () => {
    const e = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
    e.beginBaseline();
    e.inputs(inp(true), T0, W0);
    e.endBaseline([]);
    return e;
  };
  const early = seeded();
  assert.deepStrictEqual(early.inputs(inp(false), T0 + 1000, W0 + 1000), []);
  assert.deepStrictEqual(early.inputs(inp(true), T0 + 6000, W0 + 6000), []);
  assert.deepStrictEqual(early.tick(T0 + 30000, W0 + 30000), []);
  assert.strictEqual(early.rows().length, 0);

  const disabled = seeded();
  disabled.inputs(inp(false), T0 + 1000, W0 + 1000);
  const r = one(disabled.inputs(inp(false), T0 + 21000, W0 + 21000));
  const off = inp(false);
  off[0].enabled = false;
  assert.deepStrictEqual(disabled.inputs(off, T0 + 22000, W0 + 22000), []);
  like(disabled.rows()[0], { id: r.id, acked: true, recovered: 'input disabled' });
  assert.deepStrictEqual(disabled.tick(T0 + 60000, W0 + 60000), []);
  assert.strictEqual(disabled.rows().length, 1);

  const removed = seeded();
  removed.inputs(inp(false), T0 + 1000, W0 + 1000);
  one(removed.tick(T0 + 11000, W0 + 11000));
  assert.deepStrictEqual(removed.inputs([], T0 + 12000, W0 + 12000), []);
  like(removed.rows()[0], { acked: true, recovered: 'input removed' });
  assert.deepStrictEqual(removed.tick(T0 + 60000, W0 + 60000), []);
});

test('daemon loss after the hold-off', () => {
  const e = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
  assert.deepStrictEqual(e.daemon(false, T0, W0), []);
  assert.deepStrictEqual(e.daemon(false, T0 + 5000, W0 + 5000), []);
  assert.deepStrictEqual(e.tick(T0 + 9999, W0 + 9999), []);
  const r = one(e.tick(T0 + 10000, W0 + 10000));
  like(r, { rule: 'input_lost', input: 'skycued', key: 'input:skycued', label: 'skycued', detail: 'connection lost' });
  assert.deepStrictEqual(e.daemon(true, T0 + 15000, W0 + 15000), []);
  like(e.rows()[0], { id: r.id, recovered: 'recovered after 15 s', acked: true });
  const h = A.createEngine(cfg({ input_lost: { enabled: true } }), null);
  h.setHoldOffMs(1000);
  h.daemon(false, T0, W0);
  assert.deepStrictEqual(h.tick(T0 + 999, W0 + 999), []);
  assert.strictEqual(one(h.tick(T0 + 1000, W0 + 1000)).input, 'skycued');
});

test('log cap: oldest acknowledged first', () => {
  const e = fresh({ emergency: { enabled: true } });
  const hex = (i) => ('00000' + i.toString(16)).slice(-6);
  const first = [];
  for (let i = 0; i < 3; i++) first.push(one(e.track(tr({ hex: hex(i), squawk: '7700' }), T0 + i)).id);
  first.forEach((id) => { e.ack(id, T0 + 10); });
  for (let i = 3; i < 205; i++) one(e.track(tr({ hex: hex(i), squawk: '7700' }), T0 + i));
  const rows = e.rows();
  assert.strictEqual(rows.length, 200);
  first.forEach((id) => { assert.strictEqual(rows.some((r) => r.id === id), false); });
  assert.strictEqual(e.unacked().length, 200);
  assert.strictEqual(rows[0].key, hex(204) + '|icao');
  assert.strictEqual(rows[199].key, hex(5) + '|icao');
  // An acknowledged row goes before older unacknowledged ones.
  const m = fresh({ emergency: { enabled: true } });
  for (let i = 0; i < 200; i++) {
    const id = one(m.track(tr({ hex: hex(i), squawk: '7700' }), T0 + i)).id;
    if (i === 2) m.ack(id, T0 + i);
  }
  one(m.track(tr({ hex: hex(200), squawk: '7700' }), T0 + 200));
  assert.strictEqual(m.rows().length, 200);
  assert.strictEqual(m.rows().some((r) => r.key === hex(2) + '|icao'), false);
  assert.strictEqual(m.rows()[199].key, hex(0) + '|icao');
});

test('normalizeLog: round trip', () => {
  const c = cfg({ emergency: { enabled: true }, input_lost: { enabled: true }, range_enter: { enabled: true } });
  const e = A.createEngine(c, null);
  e.beginBaseline();
  e.inputs(inp(true), T0, W0);
  e.endBaseline([]);
  one(e.track(tr({ squawk: '7700' }), T0));
  const range = one(e.track(tr({ hex: 'b00001', range_nm: 10 }), T0 + 1000));
  e.ack(range.id, T0 + 2000);
  e.inputs(inp(false), T0 + 3000, W0 + 3000);
  one(e.tick(T0 + 13000, W0 + 13000));
  const stored = e.toStore();
  assert.deepStrictEqual(A.normalizeLog(stored), stored);
  const e2 = A.createEngine(c, A.normalizeLog(JSON.stringify(stored)));
  assert.deepStrictEqual(e2.rows(), e.rows());
  assert.deepStrictEqual(e2.unacked(), e.unacked());
  assert.strictEqual(e2.unacked().length, 2);
  assert.deepStrictEqual(e2.track(tr({ squawk: '7700' }), T0 + 20000), []);
  assert.deepStrictEqual(e2.track(tr({ hex: 'b00001', range_nm: 10 }), T0 + 20000), []);
  assert.strictEqual(e2.rows().length, 3);
  // Ids continue past the restored rows.
  const next = one(e2.track(tr({ hex: 'b00002', squawk: '7700' }), T0 + 21000));
  assert.strictEqual(next.id, stored.nextId);
});

test('normalizeLog: caps and garbage', () => {
  const rows = [];
  for (let i = 1; i <= 5000; i++) {
    rows.push({ id: i % 10 === 0 ? 'x' : i, time: i % 10 === 4 ? 'nope' : new Date(T0 + i * 1000).toISOString(),
      rule: i % 2 ? 'bogus' : 'emergency', title: 'Emergency', label: 'L', detail: 'D', key: 'k' + i + '|icao',
      input: null, acked: true, ackTime: null, quiet: false, recovered: null, pattern: 'double', sound: true });
  }
  const pairs = {};
  for (let i = 0; i < 3000; i++) pairs['emergency|k' + i + '|icao'] = { active: true, fired: T0 + i, ack: null, open: null };
  const l = A.normalizeLog(JSON.stringify({ version: 1, nextId: 3, rows: rows, pairs: pairs }));
  assert.strictEqual(l.rows.length, 200);
  assert.strictEqual(l.rows.every((r) => r.rule === 'emergency' && Number.isInteger(r.id)), true);
  const maxId = Math.max.apply(null, l.rows.map((r) => r.id));
  assert.strictEqual(maxId, 4998);
  assert.ok(l.nextId > maxId);
  assert.strictEqual(Object.keys(l.pairs).length, 1000);
  assert.deepStrictEqual(A.normalizeLog('[]'), { version: 1, nextId: 1, rows: [], pairs: {} });
  assert.deepStrictEqual(A.normalizeLog(null), { version: 1, nextId: 1, rows: [], pairs: {} });
  assert.deepStrictEqual(A.normalizeLog('nope'), { version: 1, nextId: 1, rows: [], pairs: {} });
});

test('ackAll: acknowledges every open row', () => {
  const e = fresh({ emergency: { enabled: true } });
  one(e.track(tr({ squawk: '7700' }), T0));
  one(e.track(tr({ hex: 'b00001', squawk: '7700' }), T0));
  assert.strictEqual(e.ackAll(T0 + 1000), 2);
  assert.strictEqual(e.unacked().length, 0);
  assert.strictEqual(e.ackAll(T0 + 2000), 0);
  e.clear();
  assert.deepStrictEqual(e.rows(), []);
});

test('durationText', () => {
  assert.strictEqual(A.durationText(0), '0 s');
  assert.strictEqual(A.durationText(59999), '59 s');
  assert.strictEqual(A.durationText(72000), '1 min 12 s');
  assert.strictEqual(A.durationText(3720000), '1 h 2 min');
});

test('tonePlan', () => {
  assert.deepStrictEqual(A.tonePlan('single'), [{ at: 0, dur: 0.15, freq: 880 }]);
  assert.deepStrictEqual(A.tonePlan('double'), [{ at: 0, dur: 0.15, freq: 880 }, { at: 0.25, dur: 0.15, freq: 880 }]);
  assert.deepStrictEqual(A.tonePlan('siren'), [{ at: 0, dur: 0.15, freq: 880 }]);
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
