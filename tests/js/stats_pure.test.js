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
  assert.strictEqual(by.aircraft.sub, '3 new');
  assert.strictEqual(by.drops.sub, 'timeout/filter/lost');
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

test('tiles: aircraft sub without a count larger than the tile', () => {
  const sm = summary();
  sm.range.aircraft = 1;
  sm.range.new_aircraft = 12;
  const t = S.tiles(sm, null).find((x) => x.id === 'aircraft');
  assert.strictEqual(t.text, '1');
  assert.strictEqual(t.sub, 'in range');
  sm.range.new_aircraft = 1;
  assert.strictEqual(S.tiles(sm, null).find((x) => x.id === 'aircraft').sub, '1 new');
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

// ---- Traffic and Weather (Task 8) -------------------------------------------
const M0 = Date.parse(T0);
const M1 = Date.parse(T1);
const M2 = Date.parse(T2);
const M3 = Date.parse(T3);

test('hourOfWeek: 168 cells, UTC Monday first', () => {
  // 2026-11-02 is a Monday; 2026-11-08 a Sunday; 2026-11-09 the next Monday.
  const hours = {
    res: 'hour', bucket_s: 3600,
    t: ['2026-11-02T00:00:00.000Z', '2026-11-08T23:00:00.000Z', '2026-11-09T00:00:00.000Z'],
    observations: [5, 7, 1]
  };
  const cells = S.hourOfWeek(hours);
  assert.strictEqual(cells.length, 168);
  const at = (h, d) => cells.filter((c) => c[0] === h && c[1] === d);
  for (let d = 0; d < 7; d++) {
    for (let h = 0; h < 24; h++) {
      const c = at(h, d);
      assert.strictEqual(c.length, 1, 'one cell for ' + h + ',' + d);
      const want = h === 0 && d === 0 ? 6 : (h === 23 && d === 6 ? 7 : 0);
      assert.strictEqual(c[0][2], want, 'cell ' + h + ',' + d);
    }
  }
  // Ordered day by day, hour by hour.
  assert.deepStrictEqual(cells[0], [0, 0, 6]);
  assert.deepStrictEqual(cells[167], [23, 6, 7]);
  assert.deepStrictEqual(cells[25], [1, 1, 0]);
  // No series: still 168 zero cells.
  const empty = S.hourOfWeek(null);
  assert.strictEqual(empty.length, 168);
  assert.strictEqual(empty.reduce((a, c) => a + c[2], 0), 0);
  assert.strictEqual(S.hourOfWeekMax(cells), 7);
  assert.strictEqual(S.hourOfWeekMax(empty), 0);
});

test('inputSeries: order by total, other last', () => {
  const s = series4();
  s.by_input = {
    other: [50, 50, 50, 50],        // the largest, still last
    uat: [0, 5, 10, 15],            // 30
    readsb: [10, 15, 20, 25],       // 70
    beast2: [1, 1, 1, 1],           // 4
    beast1: [1, 1, 1, 1]            // 4, tie: by name
  };
  const out = S.inputSeries(s);
  assert.deepStrictEqual(out.map((x) => x.name), ['readsb', 'uat', 'beast1', 'beast2', 'other']);
  assert.deepStrictEqual(out[0].data, [[M0, 10], [M1, 15], [M2, 20], [M3, 25]]);
  assert.deepStrictEqual(out[4].data, [[M0, 50], [M1, 50], [M2, 50], [M3, 50]]);
  assert.deepStrictEqual(S.inputSeries(null), []);
  assert.deepStrictEqual(S.inputSeries({ t: [T0], observations: [1] }), []);
});

test('aircraftSeries: distinct and new per bucket', () => {
  const s = series4();
  s.new_aircraft = [1, 0, 2, 0];
  const out = S.aircraftSeries(s);
  assert.deepStrictEqual(out.distinct, [[M0, 1], [M1, 2], [M2, 3], [M3, 4]]);
  assert.deepStrictEqual(out.fresh, [[M0, 1], [M1, 0], [M2, 2], [M3, 0]]);
  assert.deepStrictEqual(S.aircraftSeries(null), { distinct: [], fresh: [] });
});

test('sentDropSeries: names and sources', () => {
  const s = series4();
  s.drops_out_of_range = [0, 2, 0, 1];
  // no drops_below_elevation / drops_no_position columns: zeros
  const out = S.sentDropSeries(s);
  assert.deepStrictEqual(out.map((x) => x.name),
    ['sent', 'timeout', 'source_lost', 'blocked', 'out_of_range', 'below_elevation', 'no_position']);
  assert.deepStrictEqual(out[0].data, [[M0, 0], [M1, 1], [M2, 2], [M3, 3]]);       // sent_updates
  assert.deepStrictEqual(out[1].data, [[M0, 1], [M1, 0], [M2, 0], [M3, 2]]);       // drops_timeout
  assert.deepStrictEqual(out[2].data, [[M0, 0], [M1, 0], [M2, 5], [M3, 0]]);       // drops_source_lost
  assert.deepStrictEqual(out[3].data, [[M0, 9], [M1, 9], [M2, 9], [M3, 9]]);       // drops_blocked
  assert.deepStrictEqual(out[4].data, [[M0, 0], [M1, 2], [M2, 0], [M3, 1]]);       // drops_out_of_range
  assert.deepStrictEqual(out[5].data, [[M0, 0], [M1, 0], [M2, 0], [M3, 0]]);
  assert.deepStrictEqual(out[6].data, [[M0, 0], [M1, 0], [M2, 0], [M3, 0]]);
  assert.deepStrictEqual(S.sentDropSeries(null).map((x) => x.data), [[], [], [], [], [], [], []]);
});

test('uatSeries: three lines', () => {
  const s = series4();
  s.uat_downlink = [3, 0, 4, 1];
  s.uat_uplink = [60, 61, 59, 60];
  s.fisb_products = [12, 0, 7, 9];
  const out = S.uatSeries(s);
  assert.deepStrictEqual(Object.keys(out), ['downlink', 'uplink', 'products']);
  assert.deepStrictEqual(out.downlink, [[M0, 3], [M1, 0], [M2, 4], [M3, 1]]);
  assert.deepStrictEqual(out.uplink, [[M0, 60], [M1, 61], [M2, 59], [M3, 60]]);
  assert.deepStrictEqual(out.products, [[M0, 12], [M1, 0], [M2, 7], [M3, 9]]);
  assert.deepStrictEqual(S.uatSeries(null), { downlink: [], uplink: [], products: [] });
});

test('spark and inputSeries: a bucket with an unparsable time is left out', () => {
  const s = series4();
  s.t = [T0, 'garbage', T2, T3];
  assert.deepStrictEqual(S.spark(s, 'observations'), [[M0, 10], [M2, 30], [M3, 40]]);
  const ins = S.inputSeries(s);
  assert.deepStrictEqual(ins.map((x) => x.name), ['readsb', 'uat']);
  assert.deepStrictEqual(ins[0].data, [[M0, 10], [M2, 20], [M3, 25]]);
  assert.deepStrictEqual(ins[1].data, [[M0, 0], [M2, 10], [M3, 15]]);
  assert.strictEqual(S.total(s, 'observations'), 80);
  assert.deepStrictEqual(S.uatSeries(s).uplink, [[M0, 0], [M2, 0], [M3, 0]]);
});

test('total: a column summed', () => {
  assert.strictEqual(S.total(series4(), 'observations'), 100);
  assert.strictEqual(S.total(series4(), ['drops_timeout', 'drops_source_lost']), 8);
  assert.strictEqual(S.total(series4(), 'uat_uplink'), 0);
  assert.strictEqual(S.total(null, 'observations'), 0);
});

test('uatShown: from summary.uat_seen, null without a summary', () => {
  assert.strictEqual(S.uatShown({ uat_seen: true }), true);
  assert.strictEqual(S.uatShown({ uat_seen: false }), false);
  assert.strictEqual(S.uatShown({}), false);
  assert.strictEqual(S.uatShown(null), null);
});

// ---- Aircraft (spec 6 item 7) ----------------------------------------------
// The sort names GET /api/stats/aircraft accepts (docs/web-api.md, `sort`).
const API_SORTS = ['first_seen', 'last_seen', 'observations', 'positions', 'sessions', 'sent_updates', 'drops',
  'emergencies', 'max_range_nm', 'max_range_time', 'max_range_lat', 'max_range_lon', 'max_range_bearing_deg',
  'min_alt_ft', 'max_alt_ft', 'max_gs_kt', 'min_rssi', 'max_rssi'];
// Every field of a ledger row (docs/web-api.md, the row table).
const ROW_FIELDS = ['key', 'hex', 'addr_type', 'first_seen', 'last_seen', 'observations', 'positions', 'sessions',
  'sent_updates', 'drops', 'emergencies', 'max_range_nm', 'max_range_time', 'max_range_lat', 'max_range_lon',
  'max_range_bearing_deg', 'min_alt_ft', 'max_alt_ft', 'max_gs_kt', 'min_rssi', 'max_rssi', 'callsigns', 'squawks',
  'inputs', 'registration', 'category', 'source', 'class', 'country'];
// The docs' example row.
function ledgerRow() {
  return { key: 'a1b2c3|icao', hex: 'a1b2c3', addr_type: 'icao', first_seen: '2026-11-03T12:01:07.000Z',
    last_seen: '2026-11-03T14:09:58.000Z', observations: 5120, positions: 4980, sessions: 2, sent_updates: 1022,
    drops: 2, emergencies: 0, max_range_nm: 143.8, max_range_time: '2026-11-03T13:02:11.000Z',
    max_range_lat: 40.81, max_range_lon: -75.62, max_range_bearing_deg: 28.6, min_alt_ft: 1200,
    max_alt_ft: 36000, max_gs_kt: 481.5, min_rssi: -31.2, max_rssi: -4.5, callsigns: ['UAL123', 'UAL9'],
    squawks: ['3301'], inputs: ['readsb'], registration: 'N123AB', category: 'A3', source: 'adsb_1090',
    class: 'airline', country: 'United States' };
}

test('aircraftQuery: paging, sort, search encoding', () => {
  assert.strictEqual(S.aircraftQuery({ q: '', sort: 'last_seen', dir: 'desc', page: 0 }),
    'api/stats/aircraft?sort=last_seen&dir=desc&limit=100&offset=0');
  assert.ok(S.aircraftQuery({ q: 'n 1', sort: 'observations', dir: 'asc', page: 2 }).endsWith('&offset=200&q=n%201'));
  assert.strictEqual(S.aircraftQuery({ q: '  ual&1 ', sort: 'max_gs_kt', dir: 'asc', page: 1 }),
    'api/stats/aircraft?sort=max_gs_kt&dir=asc&limit=100&offset=100&q=ual%261');
  // Blank search left out; defaults for a missing sort, dir or page.
  assert.strictEqual(S.aircraftQuery({ q: '   ' }), 'api/stats/aircraft?sort=last_seen&dir=desc&limit=100&offset=0');
  assert.strictEqual(S.aircraftQuery({ sort: 'bogus', dir: 'up', page: -3 }),
    'api/stats/aircraft?sort=last_seen&dir=desc&limit=100&offset=0');
  assert.strictEqual(S.AIRCRAFT_PAGE, 100);
});

test('AIRCRAFT_COLS: sortable columns use API names', () => {
  assert.deepStrictEqual(S.AIRCRAFT_COLS.map((c) => c.key), ['hex', 'callsigns', 'registration', 'category', 'class',
    'country', 'first_seen', 'last_seen', 'sessions', 'observations', 'sent_updates', 'max_range_nm', 'max_alt_ft',
    'max_gs_kt']);
  const sortable = S.AIRCRAFT_COLS.filter((c) => c.sort !== null).map((c) => c.sort);
  sortable.forEach((n) => assert.ok(API_SORTS.indexOf(n) >= 0, n + ' is not an API sort name'));
  assert.deepStrictEqual(sortable, ['first_seen', 'last_seen', 'sessions', 'observations', 'sent_updates',
    'max_range_nm', 'max_alt_ft', 'max_gs_kt']);
  // The text columns cannot be sorted by the API.
  ['hex', 'callsigns', 'registration', 'category', 'class', 'country'].forEach((k) => {
    assert.strictEqual(S.AIRCRAFT_COLS.find((c) => c.key === k).sort, null, k);
  });
  // Right-aligned: the counts and measures.
  assert.deepStrictEqual(S.AIRCRAFT_COLS.filter((c) => c.num).map((c) => c.key),
    ['sessions', 'observations', 'sent_updates', 'max_range_nm', 'max_alt_ft', 'max_gs_kt']);
  S.AIRCRAFT_COLS.forEach((c) => assert.ok(typeof c.label === 'string' && c.label.length > 0, c.key));
});

test('aircraftCell: table text per column, dash when absent', () => {
  const r = ledgerRow();
  const cell = (k) => S.aircraftCell(r, k);
  assert.strictEqual(cell('hex'), 'a1b2c3');
  assert.strictEqual(cell('callsigns'), 'UAL123 UAL9');
  assert.strictEqual(cell('registration'), 'N123AB');
  assert.strictEqual(cell('category'), 'A3');
  assert.strictEqual(cell('class'), 'airline');
  assert.strictEqual(cell('country'), 'United States');
  assert.strictEqual(cell('first_seen'), '11-03 12:01Z');   // no year: the measures fit at 1280
  assert.strictEqual(cell('last_seen'), '11-03 14:09Z');
  assert.strictEqual(cell('sessions'), '2');
  assert.strictEqual(cell('observations'), '5,120');
  assert.strictEqual(cell('sent_updates'), '1,022');
  assert.strictEqual(cell('max_range_nm'), '143.8');
  assert.strictEqual(cell('max_alt_ft'), '36,000');
  assert.strictEqual(cell('max_gs_kt'), '482');
  const bare = { key: 'abcdef|non_icao', hex: 'abcdef', addr_type: 'non_icao', callsigns: [], class: 'unknown',
    first_seen: 'garbage' };
  ['callsigns', 'registration', 'category', 'country', 'first_seen', 'last_seen', 'sessions', 'max_range_nm',
    'max_alt_ft', 'max_gs_kt'].forEach((k) => assert.strictEqual(S.aircraftCell(bare, k), '—', k));
  assert.strictEqual(S.aircraftCell(bare, 'class'), 'unknown');
  assert.strictEqual(S.aircraftCell(null, 'hex'), '—');
});

test('aircraftPager: N–M of total, prev/next', () => {
  assert.deepStrictEqual(S.aircraftPager(12, 0, 12), { text: '1–12 of 12', prev: false, next: false });
  assert.deepStrictEqual(S.aircraftPager(1234, 0, 100), { text: '1–100 of 1,234', prev: false, next: true });
  assert.deepStrictEqual(S.aircraftPager(1234, 12, 34), { text: '1,201–1,234 of 1,234', prev: true, next: false });
  assert.deepStrictEqual(S.aircraftPager(250, 1, 100), { text: '101–200 of 250', prev: true, next: true });
  assert.deepStrictEqual(S.aircraftPager(0, 0, 0), { text: 'no aircraft', prev: false, next: false });
  // A page past the end (the ledger shrank): no rows, back is possible.
  assert.deepStrictEqual(S.aircraftPager(5, 3, 0), { text: '0 of 5', prev: true, next: false });
  assert.strictEqual(S.aircraftLastPage(0), 0);
  assert.strictEqual(S.aircraftLastPage(100), 0);
  assert.strictEqual(S.aircraftLastPage(101), 1);
});

test('detailRows: every field once, units', () => {
  const rows = S.detailRows(ledgerRow());
  rows.forEach((r) => assert.ok(Array.isArray(r) && typeof r[0] === 'string' && typeof r[1] === 'string', r[0]));
  // [label, text, field]: each ledger field exactly once.
  const fields = rows.map((r) => r[2]);
  assert.deepStrictEqual(fields.slice().sort(), ROW_FIELDS.slice().sort());
  assert.strictEqual(new Set(rows.map((r) => r[0])).size, rows.length, 'labels unique');
  const by = {};
  rows.forEach((r) => { by[r[2]] = r[1]; });
  assert.strictEqual(by.key, 'a1b2c3|icao');
  assert.strictEqual(by.hex, 'a1b2c3');
  assert.strictEqual(by.addr_type, 'icao');
  assert.strictEqual(by.first_seen, '2026-11-03T12:01:07.000Z');
  assert.strictEqual(by.last_seen, '2026-11-03T14:09:58.000Z');
  assert.strictEqual(by.max_range_time, '2026-11-03T13:02:11.000Z');
  assert.strictEqual(by.observations, '5,120');
  assert.strictEqual(by.positions, '4,980');
  assert.strictEqual(by.sessions, '2');
  assert.strictEqual(by.sent_updates, '1,022');
  assert.strictEqual(by.drops, '2');
  assert.strictEqual(by.emergencies, '0');
  assert.strictEqual(by.max_range_nm, '143.8 nm');
  assert.strictEqual(by.max_range_lat, '40.8100°');
  assert.strictEqual(by.max_range_lon, '-75.6200°');
  assert.strictEqual(by.max_range_bearing_deg, '28.6°');
  assert.strictEqual(by.min_alt_ft, '1,200 ft');
  assert.strictEqual(by.max_alt_ft, '36,000 ft');
  assert.strictEqual(by.max_gs_kt, '481.5 kt');
  assert.strictEqual(by.min_rssi, '-31.2 dBFS');
  assert.strictEqual(by.max_rssi, '-4.5 dBFS');
  assert.strictEqual(by.callsigns, 'UAL123, UAL9');
  assert.strictEqual(by.squawks, '3301');
  assert.strictEqual(by.inputs, 'readsb');
  assert.strictEqual(by.registration, 'N123AB');
  assert.strictEqual(by.category, 'A3');
  assert.strictEqual(by.source, 'adsb_1090');
  assert.strictEqual(by.class, 'airline');
  assert.strictEqual(by.country, 'United States');
  // Absent values: the field still listed once, as a dash; an altitude of 0 is "ground".
  const bare = S.detailRows({ key: 'abcdef|other', hex: 'abcdef', addr_type: 'other', callsigns: [], squawks: [],
    inputs: [], min_alt_ft: 0 });
  assert.deepStrictEqual(bare.map((r) => r[2]).sort(), ROW_FIELDS.slice().sort());
  const b = {};
  bare.forEach((r) => { b[r[2]] = r[1]; });
  ['first_seen', 'max_range_nm', 'max_range_time', 'max_rssi', 'callsigns', 'registration', 'country', 'observations']
    .forEach((k) => assert.strictEqual(b[k], '—', k));
  assert.strictEqual(b.min_alt_ft, '0 ft (ground)');
  assert.deepStrictEqual(S.detailRows(null), []);
});

test('detailPolar: the max-range point, null without one', () => {
  assert.deepStrictEqual(S.detailPolar(ledgerRow()), { range_nm: 143.8, bearing_deg: 28.6, max_nm: 150 });
  const r = ledgerRow();
  r.max_range_nm = 8.2;
  r.max_range_bearing_deg = 359.9;
  assert.deepStrictEqual(S.detailPolar(r), { range_nm: 8.2, bearing_deg: 359.9, max_nm: 10 });
  r.max_range_nm = 260;
  assert.strictEqual(S.detailPolar(r).max_nm, 300);
  assert.strictEqual(S.detailPolar({ max_range_nm: 12 }), null);
  assert.strictEqual(S.detailPolar(null), null);
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
