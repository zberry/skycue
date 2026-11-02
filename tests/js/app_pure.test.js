'use strict';
// Tests for the web UI's pure helpers (www/app_pure.js), plain node, no
// test framework. usage: node app_pure.test.js [path/to/app_pure.js]
// The geometry values are the ones tests/test_geo.cpp checks geo.cpp with:
// the page's copy of elevationDeg must agree with skycued's.
const path = require('path');
const assert = require('assert');

const file = process.argv[2] || path.join(__dirname, '..', '..', 'www', 'app_pure.js');
const P = require(path.resolve(file));

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
function between(v, lo, hi, what) {
  assert.ok(typeof v === 'number' && v >= lo && v <= hi, what + ' = ' + v + ', want ' + lo + '..' + hi);
}

const FT = 0.3048;
const nmPerDegLat = P.rangeNm(0, 0, 1, 0);

test('rangeNm DC to NYC is 175..176 nm', () => {
  between(P.rangeNm(38.9, -77.0, 40.7, -74.0), 175, 176, 'range');
});
test('rangeNm of a point to itself is 0', () => {
  assert.strictEqual(P.rangeNm(38.9, -77.0, 38.9, -77.0), 0);
});
test('elevationDeg overhead is 89.9..90', () => {
  between(P.elevationDeg(38.9, -77.0, 0, 38.9, -77.0, 10000), 89.9, 90, 'elevation');
});
test('elevationDeg is negative at 200 nm and 1000 ft', () => {
  const e = P.elevationDeg(38.0, -77.0, 0, 38.0 + 200 / nmPerDegLat, -77.0, 1000 * FT);
  assert.ok(e < 0, 'elevation = ' + e);
});
test('elevationDeg at 30 nm and 35000 ft is 10..12', () => {
  between(P.elevationDeg(38.0, -77.0, 0, 38.0 + 30 / nmPerDegLat, -77.0, 35000 * FT), 10, 12, 'elevation');
});
test('elevationDeg: a higher antenna sees the same target lower', () => {
  let prev = 91;
  for (const rx of [0, 10, 100, 500, 1000]) {
    const e = P.elevationDeg(38.0, -77.0, rx, 38.0 + 30 / nmPerDegLat, -77.0, 10000 * FT);
    assert.ok(e < prev, rx + ' m: ' + e + ' >= ' + prev);
    prev = e;
  }
});
test('elevationDeg: the fixture C172 at 40 nm, 3000 ft is about 0.4 deg', () => {
  between(P.elevationDeg(38.9, -77.0, 0, 38.9 - 40 / nmPerDegLat, -77.0, 3000 * FT), 0.2, 0.6, 'elevation');
});

test('decisionColor maps every decision to its class', () => {
  assert.strictEqual(P.decisionColor('send'), 'sent');
  assert.strictEqual(P.decisionColor('allow_listed'), 'allow_listed');
  assert.strictEqual(P.decisionColor('out_of_range'), 'out_of_range');
  assert.strictEqual(P.decisionColor('below_elevation'), 'below_elevation');
  assert.strictEqual(P.decisionColor('blocked'), 'blocked');
  assert.strictEqual(P.decisionColor('no_position'), null);
  assert.strictEqual(P.decisionColor('something_new'), 'unknown');
});
test('decisionColors gives the documented colours', () => {
  assert.deepStrictEqual(Object.keys(P.DECISION_COLORS).sort(),
    ['allow_listed', 'below_elevation', 'blocked', 'out_of_range', 'sent', 'unknown']);
  for (const k of Object.keys(P.DECISION_COLORS)) assert.ok(/^#[0-9a-f]{6}$/.test(P.DECISION_COLORS[k]), k);
});

test('formatAlt', () => {
  assert.strictEqual(P.formatAlt({ alt_baro_ft: 36000 }), '36,000 ft');
  assert.strictEqual(P.formatAlt({ alt_baro_ft: 2500, alt_geom_ft: 2650 }), '2,500 ft');
  assert.strictEqual(P.formatAlt({ alt_geom_ft: 2650 }), '2,650 ft (geom)');
  assert.strictEqual(P.formatAlt({ on_ground: true }), 'ground');
  assert.strictEqual(P.formatAlt({}), '—');
});

test('bearingDeg: north, east, south, west, same point', () => {
  between(P.bearingDeg(38.9, -77.0, 39.9, -77.0), 0, 1e-9, 'north');
  between(P.bearingDeg(0, 0, 0, 1), 90 - 1e-9, 90 + 1e-9, 'east');
  between(P.bearingDeg(39.9, -77.0, 38.9, -77.0), 180 - 1e-9, 180 + 1e-9, 'south');
  between(P.bearingDeg(0, 1, 0, 0), 270 - 1e-9, 270 + 1e-9, 'west');
  assert.strictEqual(P.bearingDeg(1, 1, 1, 1), 0);
});

test('elevationRingNm: elevation at the ring equals the mask', () => {
  for (const [altFt, mask] of [[5000, 0], [35000, 2], [10000, 5], [45000, -0.5]]) {
    const r = P.elevationRingNm(38.9, -77.0, 0, altFt * FT, mask, 400);
    assert.ok(r > 0 && r < 400, altFt + ' ft / ' + mask + ' deg: ring ' + r);
    const e = P.elevationDeg(38.9, -77.0, 0, 38.9 + r / nmPerDegLat, -77.0, altFt * FT);
    between(e, mask - 0.01, mask + 0.01, altFt + ' ft ring elevation');
  }
});
test('elevationRingNm: higher altitude, wider ring', () => {
  const a = P.elevationRingNm(38.9, -77.0, 0, 5000 * FT, 2, 400);
  const b = P.elevationRingNm(38.9, -77.0, 0, 35000 * FT, 2, 400);
  assert.ok(b > a, a + ' >= ' + b);
});
test('elevationRingNm: null when even overhead is below the mask', () => {
  assert.strictEqual(P.elevationRingNm(38.9, -77.0, 3000, 1000 * FT, 5, 400), null);
});

test('trackKey and markerLabel', () => {
  assert.strictEqual(P.trackKey({ hex: 'a4f2c1', addr_type: 'icao' }), 'a4f2c1|icao');
  assert.strictEqual(P.markerLabel({ hex: 'a4f2c1', callsign: 'UAL1423' }), 'UAL1423');
  assert.strictEqual(P.markerLabel({ hex: '2c4a1f' }), '2c4a1f');
});

test('listHas ignores case', () => {
  assert.strictEqual(P.listHas(['A4F2C1', 'n37502'], 'a4f2c1'), true);
  assert.strictEqual(P.listHas(['A4F2C1', 'n37502'], 'N37502'), true);
  assert.strictEqual(P.listHas([], 'x'), false);
  assert.strictEqual(P.listHas(undefined, 'x'), false);
});

test('sortTracks by a numeric and a text column, missing last', () => {
  const t = [{ hex: 'b', range_nm: 5 }, { hex: 'a' }, { hex: 'c', range_nm: 1 }];
  assert.deepStrictEqual(P.sortTracks(t, 'range_nm', 1).map(x => x.hex), ['c', 'b', 'a']);
  assert.deepStrictEqual(P.sortTracks(t, 'range_nm', -1).map(x => x.hex), ['b', 'c', 'a']);
  assert.deepStrictEqual(P.sortTracks(t, 'hex', 1).map(x => x.hex), ['a', 'b', 'c']);
});

test('graticuleStep: 5 deg below zoom 6, 1 deg from 6', () => {
  assert.strictEqual(P.graticuleStep(3), 5);
  assert.strictEqual(P.graticuleStep(5), 5);
  assert.strictEqual(P.graticuleStep(6), 1);
  assert.strictEqual(P.graticuleStep(12), 1);
});

test('dropRemovesMarker: only filtered drops keep the marker', () => {
  assert.strictEqual(P.dropRemovesMarker('filtered'), false);
  for (const r of ['timeout', 'source_lost', 'shutdown', 'stale']) assert.strictEqual(P.dropRemovesMarker(r), true, r);
});

test('backoffMs grows and is capped', () => {
  assert.strictEqual(P.backoffMs(0), 500);
  assert.ok(P.backoffMs(1) > P.backoffMs(0));
  assert.strictEqual(P.backoffMs(20), 5000);
});

test('nextClockOffset: forward only, small jitter back ignored', () => {
  const wall = 1000000;
  assert.strictEqual(P.nextClockOffset(0, wall + 200, wall), 200);       // ahead: follows
  assert.strictEqual(P.nextClockOffset(200, wall + 100, wall), 200);     // a late item: kept
  assert.strictEqual(P.nextClockOffset(200, wall - 4000, wall), 200);    // 4.2 s back: still kept
  assert.strictEqual(P.nextClockOffset(200, NaN, wall), 200);            // no time: kept
  assert.strictEqual(P.nextClockOffset(200, undefined, wall), 200);
});
test('nextClockOffset: a jump back of more than 5 s resets (replay loop wrap)', () => {
  const wall = 1000000;
  // The replay looped: the data clock is 10 minutes earlier now.
  assert.strictEqual(P.nextClockOffset(0, wall - 600000, wall), -600000);
  assert.strictEqual(P.nextClockOffset(1000, wall + 1000 - P.CLOCK_RESET_MS - 1, wall), -P.CLOCK_RESET_MS - 1 + 1000);
  assert.strictEqual(P.nextClockOffset(1000, wall + 1000 - P.CLOCK_RESET_MS, wall), 1000);  // exactly 5 s: kept
  // And forward again from there.
  assert.strictEqual(P.nextClockOffset(-600000, wall - 599000, wall), -599000);
});

test('formatAge', () => {
  assert.strictEqual(P.formatAge(Date.parse('2026-11-01T12:00:00.000Z'), Date.parse('2026-11-01T12:00:05.400Z')), '5 s ago');
  assert.strictEqual(P.formatAge(Date.parse('2026-11-01T12:00:00.000Z'), Date.parse('2026-11-01T12:03:05.000Z')), '3 min 5 s ago');
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
