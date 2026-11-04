'use strict';
// Tests for the Stats view's pure helpers (www/stats_pure.js), plain node,
// no test framework (the same harness as app_pure.test.js).
// usage: node stats_pure.test.js [path/to/stats_pure.js]
// stats_pure.js loads app_pure.js from its own directory under node.
const path = require('path');
const assert = require('assert');

const file = process.argv[2] || path.join(__dirname, '..', '..', 'www', 'stats_pure.js');
const S = require(path.resolve(file));

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

const T0 = '2026-11-03T12:00:00.000Z';
const T1 = '2026-11-03T12:01:00.000Z';
const T2 = '2026-11-03T12:02:00.000Z';
const T3 = '2026-11-03T12:03:00.000Z';
// A four-bucket minute series in the shape of GET /api/stats/series.
function series4() {
  return {
    res: 'minute', bucket_s: 60, t: [T0, T1, T2, T3],
    observations: [10, 20, 30, 40], aircraft: [1, 2, 3, 4], sent_updates: [0, 1, 2, 3],
    drops_timeout: [1, 0, 0, 2], drops_filtered: [0, 3, 0, 1], drops_source_lost: [0, 0, 5, 0],
    drops_blocked: [9, 9, 9, 9], emergencies: [0, 0, 1, 0],
    max_range_nm: [0, 51.5, 80.25, 12], max_range_key: [null, 'a4f2c1|icao', 'abc123|icao', 'a4f2c1|icao'],
    by_input: { readsb: [10, 15, 20, 25], uat: [0, 5, 10, 15] }
  };
}

test('RANGES: keys, lengths and resolutions in order', () => {
  assert.deepStrictEqual(S.RANGES.map((r) => r.key), ['1h', '6h', '24h', '7d', '30d']);
  assert.deepStrictEqual(S.RANGES.map((r) => r.ms),
    [3600000, 6 * 3600000, 24 * 3600000, 7 * 24 * 3600000, 30 * 24 * 3600000]);
  assert.deepStrictEqual(S.RANGES.map((r) => r.res), ['minute', 'minute', 'minute', 'hour', 'hour']);
});

test('rangeQuery: resolution and bounds', () => {
  const q = S.rangeQuery('6h', Date.parse('2026-11-03T12:00:00Z'));
  assert.deepStrictEqual(q, { from: '2026-11-03T06:00:00.000Z', to: '2026-11-03T12:00:00.000Z', res: 'minute' });
  assert.strictEqual(S.rangeQuery('7d', 0).res, 'hour');
  assert.strictEqual(S.rangeQuery('30d', 0).res, 'hour');
  assert.strictEqual(S.rangeQuery('24h', 0).res, 'minute');
  // Unknown key: 24 h.
  const b = S.rangeQuery('bogus', Date.parse(T0));
  assert.strictEqual(b.res, 'minute');
  assert.strictEqual(b.from, '2026-11-02T12:00:00.000Z');
  assert.strictEqual(S.rangeKey('bogus'), '24h');
  assert.strictEqual(S.rangeKey('7d'), '7d');
  assert.strictEqual(S.rangeKey(null), '24h');
});

test('seriesTimes: ISO to ms, empty for no series', () => {
  assert.deepStrictEqual(S.seriesTimes(series4()), [T0, T1, T2, T3].map(Date.parse));
  assert.deepStrictEqual(S.seriesTimes(null), []);
  assert.deepStrictEqual(S.seriesTimes({}), []);
});

test('sliceSeries: cuts every column and by_input', () => {
  const s = series4();
  const c = S.sliceSeries(s, Date.parse(T1), Date.parse(T2));
  assert.deepStrictEqual(c.t, [T1, T2]);
  assert.deepStrictEqual(c.observations, [20, 30]);
  assert.deepStrictEqual(c.max_range_key, ['a4f2c1|icao', 'abc123|icao']);
  assert.deepStrictEqual(c.by_input.readsb, [15, 20]);
  assert.deepStrictEqual(c.by_input.uat, [5, 10]);
  assert.strictEqual(c.res, 'minute');
  assert.strictEqual(c.bucket_s, 60);
  // The input is untouched.
  assert.strictEqual(s.t.length, 4);
  assert.strictEqual(s.by_input.readsb.length, 4);
  // Bounds are inclusive; a range past the end is empty.
  assert.deepStrictEqual(S.sliceSeries(s, Date.parse(T0), Date.parse(T3)).observations, [10, 20, 30, 40]);
  const none = S.sliceSeries(s, Date.parse(T3) + 1, Date.parse(T3) + 60000);
  assert.deepStrictEqual(none.t, []);
  assert.deepStrictEqual(none.by_input.uat, []);
  assert.deepStrictEqual(S.sliceSeries(null, 0, 1).t, []);
});

test('spark: pairs of ms and value', () => {
  const s = series4();
  assert.deepStrictEqual(S.spark(s, 'observations'),
    [[Date.parse(T0), 10], [Date.parse(T1), 20], [Date.parse(T2), 30], [Date.parse(T3), 40]]);
  // A missing field: zeros.
  assert.deepStrictEqual(S.spark(s, 'nope').map((p) => p[1]), [0, 0, 0, 0]);
  // Several fields: their sum per bucket.
  assert.deepStrictEqual(S.spark(s, ['drops_timeout', 'drops_filtered', 'drops_source_lost']).map((p) => p[1]),
    [1, 3, 5, 3]);
  assert.deepStrictEqual(S.spark(null, 'observations'), []);
});

function summary() {
  return {
    since: '2026-11-03T10:00:00.000Z', now: T3, aircraft_total: 40, evicted: 0, uat_seen: false,
    range: { from: T0, to: T3, res: 'minute', observations: 12345, positions: 9000, new_aircraft: 3,
      sent_updates: 1500, drops: 11, emergencies: 1, max_range_nm: 80.25, max_range_key: 'abc123|icao',
      aircraft: 12, by_class: { airline: 8, military: 1, private: 2, unknown: 1 }, by_country: [] }
  };
}

test('tiles: ids, values, max range sub', () => {
  const tiles = S.tiles(summary(), series4());
  assert.deepStrictEqual(tiles.map((t) => t.id), ['aircraft', 'observations', 'sent', 'drops', 'max_range', 'emergencies']);
  const by = {};
  tiles.forEach((t) => { by[t.id] = t; });
  assert.strictEqual(by.aircraft.value, 12);
  assert.strictEqual(by.observations.value, 12345);
  assert.strictEqual(by.observations.text, '12,345');
  assert.strictEqual(by.sent.value, 1500);
  assert.strictEqual(by.sent.text, '1,500');
  assert.strictEqual(by.drops.value, 11);
  assert.strictEqual(by.max_range.value, 80.25);
  assert.strictEqual(by.max_range.text, '80.3 nm');
  assert.strictEqual(by.max_range.sub, 'abc123');
  assert.strictEqual(by.emergencies.value, 1);
  tiles.forEach((t) => {
    assert.strictEqual(typeof t.label, 'string', t.id + ' label');
    assert.ok(t.label.length > 0, t.id + ' label');
    assert.strictEqual(typeof t.sub, 'string', t.id + ' sub');
  });
});

test('tiles: each has a spark; drops = timeout + filtered + source lost', () => {
  const ms = [T0, T1, T2, T3].map(Date.parse);
  const by = {};
  S.tiles(summary(), series4()).forEach((t) => { by[t.id] = t; });
  const vals = (id) => by[id].spark.map((p) => p[1]);
  by.drops.spark.forEach((p, i) => assert.strictEqual(p[0], ms[i]));
  assert.deepStrictEqual(vals('drops'), [1, 3, 5, 3]);   // drops_blocked is not a drop of this kind
  assert.deepStrictEqual(vals('aircraft'), [1, 2, 3, 4]);
  assert.deepStrictEqual(vals('observations'), [10, 20, 30, 40]);
  assert.deepStrictEqual(vals('sent'), [0, 1, 2, 3]);
  assert.deepStrictEqual(vals('max_range'), [0, 51.5, 80.25, 12]);
  assert.deepStrictEqual(vals('emergencies'), [0, 0, 1, 0]);
});

test('tiles: no data yet', () => {
  const tiles = S.tiles({ now: T0, range: { observations: 0, positions: 0, new_aircraft: 0, sent_updates: 0,
    drops: 0, emergencies: 0, aircraft: 0 } }, null);
  const by = {};
  tiles.forEach((t) => { by[t.id] = t; });
  assert.strictEqual(by.observations.text, '0');
  assert.strictEqual(by.max_range.value, null);
  assert.strictEqual(by.max_range.text, '—');
  assert.strictEqual(by.max_range.sub, '');
  tiles.forEach((t) => assert.deepStrictEqual(t.spark, [], t.id));
  // No summary at all: six tiles, all unknown.
  const empty = S.tiles(null, null);
  assert.strictEqual(empty.length, 6);
  empty.forEach((t) => assert.strictEqual(t.text, '—', t.id));
});

test('sinceText: UTC date and time to the minute', () => {
  assert.strictEqual(S.sinceText('2026-11-03T10:04:59.500Z'), 'since 2026-11-03 10:04Z');
  assert.strictEqual(S.sinceText(undefined), '');
  assert.strictEqual(S.sinceText('garbage'), '');
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
