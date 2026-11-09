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

// ---- Coverage, Fleet and Signal (Task 10) ------------------------------------
const BAND_NAMES = ['0-2.5k', '2.5-5k', '5-10k', '10-20k', '20-30k', '30-40k', '40k+', 'unknown'];
function coverageTable() {
  return {
    sector_deg: 5, bands: BAND_NAMES.slice(),
    cells: [
      { sector: 0, band: 0, max_range_nm: 12.5, key: 'aaaaaa|icao', time: '2026-11-03T12:00:00.000Z', n: 3 },
      { sector: 0, band: 4, max_range_nm: 88, key: 'bbbbbb|icao', time: '2026-11-03T12:01:00.000Z', n: 40 },
      { sector: 71, band: 7, max_range_nm: 120, key: 'c0ffee|tisb_trackfile', time: '2026-11-03T12:02:00.000Z', n: 2 }
    ],
    grid: { cell_deg: 0.05, cap: 40000, full: false,
      cells: [[38.925, -76.975, 311], [40.025, -77.025, 9], [38.875, -78.475, 1]] },
    receiver: { lat: 38.9, lon: -77 }
  };
}

test('polarSeries: 8 bands x 72, holder lookup', () => {
  const p = S.polarSeries(coverageTable());
  assert.strictEqual(p.angles.length, 72);
  assert.strictEqual(p.angles[0], 0);
  assert.strictEqual(p.angles[1], 5);
  assert.strictEqual(p.angles[71], 355);
  assert.deepStrictEqual(p.bands.map((b) => b.name), BAND_NAMES);
  p.bands.forEach((b) => assert.strictEqual(b.data.length, 72));
  assert.strictEqual(p.bands[7].data[71], 120);
  assert.strictEqual(p.bands[0].data[0], 12.5);
  assert.strictEqual(p.bands[4].data[0], 88);
  // Every other value is 0.
  const nonZero = [];
  p.bands.forEach((b, bi) => b.data.forEach((v, si) => { if (v !== 0) nonZero.push([bi, si]); }));
  assert.deepStrictEqual(nonZero, [[0, 0], [4, 0], [7, 71]]);
  assert.strictEqual(p.holder(71, 7).key, 'c0ffee|tisb_trackfile');
  assert.strictEqual(p.holder(71, 7).time, '2026-11-03T12:02:00.000Z');
  assert.strictEqual(p.holder(0, 4).max_range_nm, 88);
  assert.strictEqual(p.holder(70, 7), null);
  assert.strictEqual(p.holder(0, 1), null);
  // The record over all cells.
  assert.strictEqual(p.max.key, 'c0ffee|tisb_trackfile');
  // Null or empty: 8 zero bands, no holders.
  const e = S.polarSeries(null);
  assert.strictEqual(e.bands.length, 8);
  assert.strictEqual(e.bands[3].data.reduce((a, v) => a + v, 0), 0);
  assert.strictEqual(e.holder(0, 0), null);
  assert.strictEqual(e.max, null);
});

test('histBars: labels and values', () => {
  // The first bin also holds the values below it, the last those above.
  assert.deepStrictEqual(S.histBars({ start: -8000, bin: 500, counts: [1, 2, 3] }),
    { labels: ['<-7500', '-7500–-7000', '-7000+'], values: [1, 2, 3] });
  assert.deepStrictEqual(S.histBars({ start: 0, bin: 10, counts: [0, 4, 0, 5, 0, 0] }),
    { labels: ['<10', '10–20', '20–30', '30–40', '40–50', '50+'], values: [0, 4, 0, 5, 0, 0] });
  assert.deepStrictEqual(S.histBars({ start: -50, bin: 1, counts: [2, 0, 7] }, true),
    { labels: ['<-49', '-49–-48', '-48+'], values: [2, 0, 7] });
  // trim: the empty bins before the first and after the last count go.
  assert.deepStrictEqual(S.histBars({ start: 0, bin: 10, counts: [0, 4, 0, 5, 0, 0] }, true),
    { labels: ['10–20', '20–30', '30–40'], values: [4, 0, 5] });
  assert.deepStrictEqual(S.histBars({ start: -50, bin: 1, counts: [0, 0, 0] }, true), { labels: [], values: [] });
  assert.deepStrictEqual(S.histBars(null), { labels: [], values: [] });
  assert.deepStrictEqual(S.histBars({ start: 0, bin: 2 }), { labels: [], values: [] });
});

test('rssiHeatmap: non-zero cells and median', () => {
  const counts = [];
  for (let r = 0; r < 50; r++) { const row = []; for (let b = 0; b < 25; b++) row.push(0); counts.push(row); }
  counts[3][5] = 1;
  counts[3][6] = 2;
  counts[10][0] = 4;
  counts[10][24] = 4;    // even: the lower middle bin
  const h = S.rssiHeatmap({ range_bin_nm: 10, rssi_bin_db: 2, rssi_start: -50, counts: counts });
  assert.deepStrictEqual(h.data, [[3, 5, 1], [3, 6, 2], [10, 0, 4], [10, 24, 4]]);
  // Column 3: 3 observations, the 2nd is in bin 6 -> its centre -50 + 6*2 + 1.
  assert.deepStrictEqual(h.median, [[3, -50 + 6 * 2 + 1], [10, -49]]);
  assert.strictEqual(h.ranges, 50);
  assert.strictEqual(h.levels, 25);
  assert.deepStrictEqual(S.rssiHeatmap(null), { data: [], median: [], ranges: 0, levels: 0 });
});

test('topCountries: top n plus other', () => {
  const by = [];
  for (let i = 0; i < 17; i++) by.push({ name: 'C' + String(i).padStart(2, '0'), n: 100 - i });
  by.push({ name: 'other', n: 4 });
  const t = S.topCountries(by, 15);
  assert.strictEqual(t.length, 16);
  assert.deepStrictEqual(t[0], { name: 'C00', value: 100 });
  assert.deepStrictEqual(t[14], { name: 'C14', value: 86 });
  // The two past the 15th (85 + 84) plus the daemon's own other (4).
  assert.deepStrictEqual(t[15], { name: 'other', value: 85 + 84 + 4 });
  // Few enough: as given, no other.
  assert.deepStrictEqual(S.topCountries([{ name: 'Canada', n: 2 }, { name: 'unknown', n: 5 }], 15),
    [{ name: 'unknown', value: 5 }, { name: 'Canada', value: 2 }]);
  // A lone daemon other stays last.
  assert.deepStrictEqual(S.topCountries([{ name: 'other', n: 9 }, { name: 'Canada', n: 2 }], 15),
    [{ name: 'Canada', value: 2 }, { name: 'other', value: 9 }]);
  assert.deepStrictEqual(S.topCountries(null, 15), []);
});

test('gridView: square around the receiver', () => {
  const v = S.gridView(coverageTable());
  // Square on the ground: the longitude span is the latitude span over cos(lat).
  const cos = Math.cos(38.9 * Math.PI / 180);
  assert.ok(Math.abs((v.xMax - v.xMin) * cos - (v.yMax - v.yMin)) < 1e-9);
  // Centred on the receiver.
  assert.ok(Math.abs((v.xMax + v.xMin) / 2 - -77) < 1e-9);
  assert.ok(Math.abs((v.yMax + v.yMin) / 2 - 38.9) < 1e-9);
  // Every cell inside (with its half cell).
  [[38.925, -76.975], [40.025, -77.025], [38.875, -78.475]].forEach((c) => {
    assert.ok(c[0] - 0.025 >= v.yMin && c[0] + 0.025 <= v.yMax, 'lat ' + c[0]);
    assert.ok(c[1] - 0.025 >= v.xMin && c[1] + 0.025 <= v.xMax, 'lon ' + c[1]);
  });
  // The westmost cell sets the span: |-78.5 - -77| lon at cos(38.9).
  assert.ok(Math.abs(v.xMin - -78.5) < 1e-9);
  assert.deepStrictEqual(v.data, [[-76.975, 38.925, 311], [-77.025, 40.025, 9], [-78.475, 38.875, 1]]);
  assert.strictEqual(v.cell, 0.05);
  assert.deepStrictEqual(v.centre, [-77, 38.9]);
  assert.strictEqual(v.receiver, true);
  // At least one 50 nm ring fits: a half span of 50 nm (5/6 deg of latitude) at the least.
  const c = coverageTable();
  c.grid.cells = [[38.925, -76.975, 1]];
  const s = S.gridView(c);
  assert.ok(Math.abs((s.yMax - s.yMin) - 100 / 60) < 1e-9);
  // No receiver: centred on the cells' bounds.
  c.receiver = undefined;
  c.grid.cells = [[10.025, 20.025, 1], [12.025, 20.025, 1]];
  const n = S.gridView(c);
  assert.strictEqual(n.receiver, false);
  assert.ok(Math.abs((n.yMax + n.yMin) / 2 - 11.025) < 1e-9);
  assert.ok(Math.abs((n.yMax - n.yMin) - (2 + 0.05)) < 1e-9);
  assert.strictEqual(S.gridView({ grid: { cells: [] } }), null);
  assert.strictEqual(S.gridView(null), null);
});

test('rangeRing: 72 points at 50 nm', () => {
  const ring = S.rangeRing({ lat: 38.9, lon: -77 }, 50);
  assert.strictEqual(ring.length, 72);
  // Bearing 0: due north, 50 nm = 50/60 deg of latitude on the page's sphere (within 0.1 %).
  assert.ok(Math.abs(ring[0][0] - -77) < 1e-9);
  assert.ok(Math.abs(ring[0][1] - (38.9 + 50 / 60)) < 0.001);
  // Bearing 90 (index 18): east, nearly the same latitude.
  assert.ok(ring[18][0] > -77 + 50 / 60);
  assert.ok(Math.abs(ring[18][1] - 38.9) < 0.02);
  // Bearing 180 (index 36): due south.
  assert.ok(Math.abs(ring[36][0] - -77) < 1e-9);
  assert.ok(Math.abs(ring[36][1] - (38.9 - 50 / 60)) < 0.001);
  assert.deepStrictEqual(S.rangeRing(null, 50), []);
  // The rings of a view: every 50 nm out to its corners.
  assert.deepStrictEqual(S.ringRadii(120), [50, 100, 150]);
  assert.deepStrictEqual(S.ringRadii(30), [50]);
});

test('donut: descending, zeros dropped', () => {
  assert.deepStrictEqual(S.donut({ A3: 5, none: 9, A1: 0, B2: 5 }),
    [{ name: 'none', value: 9 }, { name: 'A3', value: 5 }, { name: 'B2', value: 5 }]);
  assert.deepStrictEqual(S.donut(null), []);
  assert.deepStrictEqual(S.donut({}), []);
});

test('click-through mapping: category, class, country', () => {
  assert.deepStrictEqual(S.categoryFilter('A1'), { category: 'A1' });
  assert.deepStrictEqual(S.categoryFilter('A7'), { category: 'A7' });
  assert.deepStrictEqual(S.categoryFilter('none'), { category: 'none' });
  assert.deepStrictEqual(S.categoryFilter('A0'), { category: 'other' });
  assert.deepStrictEqual(S.categoryFilter('B2'), { category: 'other' });
  assert.deepStrictEqual(S.categoryFilter('D7'), { category: 'other' });
  assert.deepStrictEqual(S.classFilter('military'), { classes: ['military'] });
  assert.deepStrictEqual(S.classFilter('unknown'), { classes: ['unknown'] });
  assert.strictEqual(S.classFilter('bogus'), null);
  assert.deepStrictEqual(S.countryFilter('Canada'), { country: 'Canada' });
  // The daemon's 'unknown' (no country) is the view filter's 'other'.
  assert.deepStrictEqual(S.countryFilter('unknown'), { country: 'other' });
  // The remainder bar (many countries) is not a filter.
  assert.strictEqual(S.countryFilter('other'), null);
  assert.strictEqual(S.countryFilter(''), null);
});

test('keyHex: the hex of a ledger key', () => {
  assert.strictEqual(S.keyHex('a1b2c3|icao'), 'a1b2c3');
  assert.strictEqual(S.keyHex('c0ffee'), 'c0ffee');
  assert.strictEqual(S.keyHex(null), '');
});

// ---- CSV export ------------------------------------------------------------------
test('csvText: RFC 4180 quoting', () => {
  assert.strictEqual(S.csvText([['a', 'b,c', 'd"e', 'f\ng', null, 3, ''], ['x']]), 'a,"b,c","d""e","f\ng",,3,\r\nx\r\n');
  assert.strictEqual(S.csvText([['a\rb', undefined]]), '"a\rb",\r\n');
  assert.strictEqual(S.csvText([]), '');
});

const LEDGER_HEADER = 'key,hex,addr_type,first_seen,last_seen,observations,positions,sessions,sent_updates,drops,' +
  'emergencies,max_range_nm,max_range_time,max_range_lat_deg,max_range_lon_deg,max_range_bearing_deg,min_alt_ft,' +
  'max_alt_ft,max_gs_kt,min_rssi_dbfs,max_rssi_dbfs,callsigns,squawks,inputs,registration,category,source,class,country';

// Review Focus 5: the ledger grows while the export pages through it, so a
// row can come twice; the file holds it once.
test('ledgerCsvRows: header, cells, repeated keys dropped', () => {
  const r1 = ledgerRow();
  const r2 = { key: 'c0ffee|icao', hex: 'c0ffee', addr_type: 'icao', observations: 1 };
  const out = S.ledgerCsvRows([r1, r2, ledgerRow()]);
  assert.strictEqual(out[0].join(','), LEDGER_HEADER);
  assert.deepStrictEqual(S.LEDGER_CSV_COLS.map((c) => c.key), ROW_FIELDS);
  assert.strictEqual(out.length, 3);
  const col = (name) => out[0].indexOf(name);
  assert.strictEqual(out[1][col('callsigns')], 'UAL123 UAL9');
  assert.strictEqual(out[1][col('max_range_nm')], 143.8);
  assert.strictEqual(out[1][col('country')], 'United States');
  assert.strictEqual(out[1][col('max_range_time')], '2026-11-03T13:02:11.000Z');
  assert.strictEqual(out[1][col('min_rssi_dbfs')], -31.2);
  assert.strictEqual(out[2][col('key')], 'c0ffee|icao');
  assert.strictEqual(out[2][col('max_range_nm')], '');
  assert.strictEqual(out[2][col('callsigns')], '');
  assert.strictEqual(out[2][col('observations')], 1);
  assert.strictEqual(S.ledgerCsvRows([]).length, 1);
});

test('ledgerExportQuery: the table sort and search, 1000 a page', () => {
  assert.strictEqual(S.ledgerExportQuery({ q: 'ual', sort: 'max_range_nm', dir: 'asc' }, 2000),
    'api/stats/aircraft?sort=max_range_nm&dir=asc&limit=1000&offset=2000&q=ual');
  assert.strictEqual(S.ledgerExportQuery({ q: '', sort: 'bogus', dir: 'x', page: 3 }, 0),
    'api/stats/aircraft?sort=last_seen&dir=desc&limit=1000&offset=0');
  assert.strictEqual(S.LEDGER_EXPORT_PAGE, 1000);
});

test('seriesCsvRows: every column, one row per bucket', () => {
  const out = S.seriesCsvRows(series4());
  assert.deepStrictEqual(out[0], ['time', 'observations', 'aircraft', 'sent_updates', 'drops_timeout', 'drops_filtered',
    'drops_source_lost', 'drops_blocked', 'emergencies', 'max_range_nm', 'max_range_key', 'obs_readsb', 'obs_uat']);
  assert.strictEqual(out.length, 5);
  assert.deepStrictEqual(out[2], [T1, 20, 2, 1, 0, 3, 0, 9, 0, 51.5, 'a4f2c1|icao', 15, 5]);
  const s = series4();
  s.zz_new = [1, 2, 3, 4];
  s.short = [1, 2];          // not a bucket column
  const h = S.seriesCsvRows(s)[0];
  assert.strictEqual(h.indexOf('zz_new'), h.indexOf('obs_readsb') - 1);
  assert.strictEqual(h.indexOf('short'), -1);
  assert.strictEqual(h.indexOf('t'), -1);
  assert.deepStrictEqual(S.seriesCsvRows(null), [['time']]);
});

test('coverageCsvRows: 72 x 8 rows', () => {
  const cov = { sector_deg: 5, bands: BAND_NAMES.slice(),
    cells: [{ sector: 1, band: 4, max_range_nm: 143.8, key: 'a1b2c3|icao', time: T0, n: 2210 }] };
  const out = S.coverageCsvRows(cov);
  assert.deepStrictEqual(out[0], ['sector_start_deg', 'band', 'max_range_nm', 'holder', 'time', 'n']);
  assert.strictEqual(out.length, 577);
  assert.deepStrictEqual(out[1], [0, '0-2.5k', '', '', '', 0]);
  assert.deepStrictEqual(out[1 + 8 + 4], [5, '20-30k', 143.8, 'a1b2c3|icao', T0, 2210]);
  assert.deepStrictEqual(out[576], [355, 'unknown', '', '', '', 0]);
});

test('csvFileName: skycue-<what>-<UTC date>.csv', () => {
  assert.strictEqual(S.csvFileName('coverage', Date.parse('2026-11-04T08:00:00Z')), 'skycue-coverage-2026-11-04.csv');
  assert.strictEqual(S.csvFileName('aircraft', Date.parse('2026-11-04T23:59:59Z')), 'skycue-aircraft-2026-11-04.csv');
});

// ---- Flow model ----
const FLOW_THEME = {
  tokens: { accent: '#58a6ff', uat: '#d2a8ff', muted: '#8b949e', text: '#e6edf3' },
  decision: { sent: '#3fb950', allow: '#58a6ff', range: '#aaaaaa', elev: '#f0883e', blocked: '#f85149', unknown: '#d2a8ff' }
};
const FLOW_T0 = Date.parse('2026-11-08T12:00:00Z');
// Snapshot i is 5 s * i after FLOW_T0; every counter grows linearly.
function flowSnap(i, o) {
  o = o || {};
  const stats = {
    observations: 1000 * i, sent_updates: 400 * i, sent_drops: 75 * i, drops_blocked: 250 * i,
    drops_out_of_range: 1200 * i, drops_below_elevation: 500 * i, drops_no_position: 50 * i, tracks_total: 40,
    inputs: [
      { id: 'readsb', connected: true, messages: 7100 * (i + 1), observations: 100 * i, uplinks: 0, rejected: 1300 * i, crc_fail: 0, frames: 0 },
      { id: 'dump978', connected: true, messages: 190 * (i + 1), observations: 10 * i, uplinks: 20 * i, rejected: 0, crc_fail: 40 * i, frames: 0 }
    ]
  };
  const inputs = [
    { id: 'readsb', name: 'Local readsb', type: 'readsb', state: { connected: true, receiving: true, last_data_age_s: 1, messages: 7100 * (i + 1) } },
    { id: 'dump978', name: 'UAT', type: 'dump978', state: { connected: true, receiving: true, last_data_age_s: 2, messages: 190 * (i + 1), uplinks: 20 * i } }
  ];
  const clients = {
    tcp: [
      { endpoint: '192.0.2.5:40001', direction: 'accepted', connected_since: '2026-11-08T11:00:00Z', bytes_sent: 3000 * i, output: 'cue-tcp' },
      { endpoint: '192.0.2.9:5001', direction: 'dialled', connected_since: '2026-11-08T11:00:00Z', bytes_sent: 1000 * i, output: 'cue-connect-0' }
    ],
    udp: [{ endpoint: '192.0.2.20:6000', datagrams_sent: 50 * i, output: 'cue-udp-0' }]
  };
  const files = [
    { name: 'skycue-received-2026-11-08.jsonl', stream: 'received', bytes: 900000 * i },
    { name: 'skycue-sent-2026-11-08.jsonl', stream: 'sent', bytes: 1000 * i }
  ];
  const outputs = [{ id: 'beast-out', kind: 'beast', input: 'readsb', state: { up: true, peers: 2, bytes: 5, messages: 5 } },
                   { id: 'cue-tcp', kind: 'cue', builtin: true, state: { up: true, peers: 2 } }];
  return Object.assign({ t: FLOW_T0 + 5000 * i, stats, inputs, clients, files, outputs }, o);
}
function flowWin(n) {
  const w = S.flowWindow();
  for (let i = 0; i < n; i++) S.flowPush(w, flowSnap(i));
  return w;
}
function flowGraphOf(snap, win, width) {
  const w = win || flowWin(3);
  const r = S.flowRates(w);
  return S.flowGraph(r, snap || w.samples[w.samples.length - 1], { width: width || 1000, theme: FLOW_THEME, now: FLOW_T0 + 10000 });
}
function flowZero() {
  const w = S.flowWindow();
  const a = flowSnap(0), b = flowSnap(0); b.t += 10000;
  S.flowPush(w, a); S.flowPush(w, b);
  return w;
}

test('flowWindow keeps twelve samples and reports the span', () => {
  const w = flowWin(14);
  assert.strictEqual(w.samples.length, 12);
  assert.strictEqual(S.flowRates(w).span_s, 55);
  assert.strictEqual(S.flowRates(S.flowWindow()), null);
  assert.strictEqual(S.flowRates(flowWin(1)), null);
});

test('flowWindow drops a sample after a counter reset', () => {
  const w = flowWin(3);
  const s = flowSnap(3); s.stats.sent_updates = 5;
  S.flowPush(w, s);
  assert.strictEqual(w.samples.length, 1);
  assert.strictEqual(S.flowRates(w), null);
  S.flowPush(w, flowSnap(4));
  assert.ok(S.flowRates(w));
});

test('flowRates per-second values from the fixture', () => {
  const r = S.flowRates(flowWin(3));
  assert.strictEqual(r.span_s, 10);
  assert.strictEqual(r.inputs.readsb.msg_s, 1420);
  assert.strictEqual(r.inputs.dump978.msg_s, 38);
  assert.strictEqual(r.inputs.dump978.uplinks_s, 4);
  assert.strictEqual(r.inputs.readsb.rejected_s, 260);
  assert.strictEqual(r.inputs.dump978.rejected_s, 8);
  assert.strictEqual(r.decoder.in_s, 1458);
  assert.strictEqual(r.decoder.rejected_s, 268);
  assert.strictEqual(r.tracks_s, 1190);
  assert.deepStrictEqual(r.decisions, { sent: 790, range: 240, elev: 100, blocked: 50, nopos: 10 });
  assert.strictEqual(r.cues_s, 95);
  assert.deepStrictEqual(r.clients[0], { key: 'tcp:192.0.2.5:40001', kind: 'tcp', endpoint: '192.0.2.5:40001', direction: 'accepted', per_min_bytes: 36000 });
  assert.strictEqual(r.clients[1].per_min_bytes, 12000);
  assert.strictEqual(r.clients[2].kind, 'udp');
  assert.strictEqual(r.clients[2].per_s_datagrams, 10);
  assert.deepStrictEqual(r.files[0], { name: 'skycue-received-2026-11-08.jsonl', stream: 'received', bytes: 1800000, bytes_per_s: 180000 });
});

test('flowGraph node order, colours and copy bands', () => {
  const r = S.flowRates(flowWin(3));
  const g = flowGraphOf();
  assert.deepStrictEqual(g.nodes.map(n => n.name), ['readsb', 'dump978', 'decode', 'rejected', 'tracks',
    'sent', 'range', 'elev', 'blocked', 'nopos', 'sched', 'tcp:192.0.2.5:40001', 'tcp:192.0.2.9:5001', 'udp:192.0.2.20:6000']);
  const col = n => g.nodes.find(x => x.name === n).color;
  assert.strictEqual(col('dump978'), FLOW_THEME.tokens.uat);
  assert.strictEqual(col('readsb'), FLOW_THEME.tokens.accent);
  assert.strictEqual(col('rejected'), FLOW_THEME.tokens.muted);
  assert.strictEqual(col('nopos'), FLOW_THEME.decision.unknown);
  const link = (s, t) => g.links.find(l => l.source === s && l.target === t);
  assert.strictEqual(link('tracks', 'range').color, FLOW_THEME.decision.range);
  assert.strictEqual(link('tracks', 'blocked').color, FLOW_THEME.decision.blocked);
  assert.strictEqual(link('tracks', 'sent').color, FLOW_THEME.decision.sent);
  assert.strictEqual(link('dump978', 'decode').color, FLOW_THEME.tokens.uat);
  ['tcp:192.0.2.5:40001', 'tcp:192.0.2.9:5001', 'udp:192.0.2.20:6000'].forEach(k => assert.strictEqual(link('sched', k).value, r.cues_s));
  assert.strictEqual(link('readsb', 'decode').value, 1420);
});

test('flowGraph labels obey two lines and one column', () => {
  [1000, 600, 1400].forEach(width => {
    const g = flowGraphOf(null, null, width);
    const maxCh = Math.floor((width - 230) / 6 / 6.6);
    g.nodes.forEach(n => {
      const lines = n.label.split('\n');
      assert.ok(lines.length <= (n.bandPx >= 45 ? 3 : 2), n.name + ' has ' + lines.length + ' lines');
      lines.forEach(l => assert.ok(l.length <= maxCh, n.name + ' line too long: ' + l));
      assert.ok(n.label.indexOf('.jsonl') < 0);
    });
  });
  assert.ok(flowGraphOf().nodes.some(n => n.bandPx >= 45 && n.label.split('\n').length === 3));
});

test('flowGraph tags: received on tracks, sent on sched, fisb on the uplink input, raw output on its input', () => {
  const g = flowGraphOf();
  const tag = n => g.tags.filter(t => t.node === n);
  assert.strictEqual(tag('tracks').length, 1);
  assert.ok(g.nodes.find(n => n.name === 'tracks').tall);
  assert.strictEqual(tag('tracks')[0].text, '⏺ recording · 1.8 MB');
  assert.strictEqual(tag('sched').length, 1);
  // a thin tracks band (nearly everything rejected) gets the bare mark
  const thin = S.flowWindow();
  for (let i = 0; i < 3; i++) { const s = flowSnap(i); s.stats.inputs[0].rejected = 7090 * i; s.stats.drops_out_of_range = s.stats.drops_below_elevation = s.stats.drops_blocked = s.stats.drops_no_position = 0; S.flowPush(thin, s); }
  const gt = flowGraphOf(null, thin);
  assert.ok(!gt.nodes.find(n => n.name === 'tracks').tall);
  assert.strictEqual(gt.tags.filter(t => t.node === 'tracks')[0].text, '⏺');
  assert.ok(tag('tracks')[0].hover.indexOf('skycue-received-2026-11-08.jsonl') >= 0);
  assert.ok(tag('tracks')[0].hover.indexOf('180 kB/s') >= 0);
  assert.strictEqual(tag('sched').length, 1);
  assert.strictEqual(tag('readsb').length, 1);
  assert.strictEqual(tag('readsb')[0].text.indexOf('↗ raw output · 2 peers') >= 0, true);
  const snap = flowSnap(2);
  snap.files.push({ name: 'skycue-fisb-2026-11-08.jsonl', stream: 'fisb', bytes: 500 });
  const w = flowWin(2); S.flowPush(w, snap);
  const g2 = flowGraphOf(snap, w);
  assert.strictEqual(g2.tags.filter(t => t.node === 'dump978' && t.text.indexOf('⏺') === 0).length, 1);
  g2.tags.forEach(t => assert.ok(t.text.indexOf('.jsonl') < 0));
});

test('flowGraph at zero traffic keeps every node', () => {
  const w = flowZero();
  const g = flowGraphOf(null, w);
  assert.strictEqual(g.nodes.length, 14);
  g.links.forEach(l => assert.strictEqual(l.value, 0.01));
  g.nodes.forEach(n => assert.strictEqual(n.idle, true));
  g.nodes.forEach(n => {
    const second = n.label.split('\n').slice(-1)[0];
    if (n.name === 'readsb' || n.name === 'dump978') assert.ok(/^no data \d+ (s|min)$/.test(second), n.label);
    else assert.strictEqual(second, '0/s', n.name);
  });
  assert.ok(/no data 2 s$/.test(g.nodes.find(n => n.name === 'dump978').label));
  assert.ok(flowGraphOf().nodes.every(n => n.idle === false));
  assert.ok(g.height >= 360);
});

test('flowGraph forgets an input that left', () => {
  const w = flowWin(3);
  const latest = flowSnap(2);
  latest.inputs = latest.inputs.filter(i => i.id !== 'dump978');
  const g = flowGraphOf(latest, w);
  assert.ok(!g.nodes.some(n => n.name === 'dump978'));
  assert.ok(g.links.every(l => l.source !== 'dump978' && l.target !== 'dump978'));
});

function flowWinClients(k, mult) {
  const w = S.flowWindow();
  for (let i = 0; i < 3; i++) {
    const s = flowSnap(i);
    s.clients.tcp = [];
    for (let c = 0; c < k; c++) s.clients.tcp.push({ endpoint: 'h' + c + ':1', direction: 'accepted', bytes_sent: 100 * i, output: 'cue-tcp' });
    s.clients.udp = [];
    s.stats.observations *= mult; s.stats.sent_updates *= mult; s.stats.drops_out_of_range *= mult;
    s.stats.inputs.forEach(x => { x.messages *= mult; x.rejected *= mult; });
    S.flowPush(w, s);
  }
  return w;
}

test('flowGraph drawing height is capped for few nodes', () => {
  const z = flowGraphOf(null, flowZero());
  assert.strictEqual(z.height, 360);
  assert.strictEqual(z.top, 20); assert.strictEqual(z.bottom, z.top);   // five decisions: drawH 344
  const g12 = flowGraphOf(null, flowWinClients(12, 1));
  assert.strictEqual(g12.height, 504); assert.strictEqual(g12.top, 20);
});

test('flowGraph height depends on the node set only', () => {
  assert.strictEqual(flowGraphOf().height, 360);
  assert.strictEqual(flowGraphOf(null, flowWinClients(8, 1)).height, 360);
  assert.strictEqual(flowGraphOf(null, flowWinClients(12, 1)).height, 504);
  assert.strictEqual(flowGraphOf(null, flowWinClients(12, 10)).height, 504);
  assert.strictEqual(flowGraphOf(null, flowZero()).height, 360);
});

test('flowGraph raw output tags every input of the kind, else the decoder', () => {
  const snap = flowSnap(2);
  snap.inputs.push({ id: 'readsb2', name: 'Second', type: 'readsb', state: { last_data_age_s: 1 } });
  snap.stats.inputs.push({ id: 'readsb2', messages: 5, rejected: 0 });
  snap.outputs = [{ id: 'j', kind: 'json', state: { up: true, peers: 3 } }];
  const w = flowWin(2); S.flowPush(w, snap);
  const g = flowGraphOf(snap, w);
  const raw = g.tags.filter(t => t.text.indexOf('raw output') >= 0).map(t => t.node).sort();
  assert.deepStrictEqual(raw, ['readsb', 'readsb2']);
  snap.outputs = [{ id: 'c', kind: 'cat021', state: { up: true, peers: 1 } }];
  const g2 = flowGraphOf(snap, w);
  const t = g2.tags.filter(x => x.text.indexOf('raw output') >= 0);
  assert.deepStrictEqual(t.map(x => x.node), ['decode']);
  assert.strictEqual(t[0].text, '\u2197 raw output \u00b7 1 peer');
});

test('flowGraph caption names the span when short', () => {
  assert.ok(/^messages per second, last 60 s/.test(flowGraphOf(null, flowWin(14)).caption));
  assert.ok(/ · after the scheduler every output receives every cue, so those bands are copies · ⏺ marks a recorder; hover for the file/.test(flowGraphOf().caption));
  assert.ok(/last 0 s/.test(S.flowGraph(null, flowSnap(0), { width: 1000, theme: FLOW_THEME }).caption));
  assert.ok(/^messages per second, last 5 s/.test(flowGraphOf(null, flowWin(2)).caption));
});

test('flowCsvRows two sections', () => {
  const rows = S.flowCsvRows(S.flowRates(flowWin(3)));
  assert.deepStrictEqual(rows[0], ['source', 'target', 'rate_per_s']);
  const i = rows.findIndex(r => r[0] === 'file');
  assert.ok(i > 1);
  assert.deepStrictEqual(rows[i], ['file', 'stream', 'bytes', 'bytes_per_s']);
  assert.deepStrictEqual(rows[i + 1], ['skycue-received-2026-11-08.jsonl', 'received', 1800000, 180000]);
  assert.deepStrictEqual(rows[1], ['readsb', 'decode', 1420]);
  assert.ok(rows.slice(1, i).every(r => Math.round(r[2] * 100) === r[2] * 100));
  assert.strictEqual(S.flowCsvRows(null).length, 1);
});

test('flowTruncate cuts with an ellipsis', () => {
  assert.strictEqual(S.flowTruncate('abcdef', 100, 6.6), 'abcdef');
  const t = S.flowTruncate('abcdefghijklmnopqrstuvwxyz', 66, 6.6);
  assert.strictEqual(t, 'abcdefghi…');
  assert.ok(t.length * 6.6 <= 66.0001);
});

test('formatBytes units', () => {
  assert.strictEqual(S.formatBytes(12), '12 B');
  assert.strictEqual(S.formatBytes(210000), '210 kB');
  assert.strictEqual(S.formatBytes(31000000), '31 MB');
  assert.strictEqual(S.formatBytes(1200000000), '1.2 GB');
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
