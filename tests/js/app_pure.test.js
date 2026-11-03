'use strict';
// Tests for the web UI's pure helpers (www/app_pure.js), plain node, no
// test framework. usage: node app_pure.test.js [path/to/app_pure.js]
// The geometry values are the ones tests/test_geo.cpp checks geo.cpp with:
// the page's copy of elevationDeg must agree with skycued's.
const fs = require('fs');
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

test('parentTile: one level up halves x/y, scale 2, offset the child quadrant', () => {
  assert.deepStrictEqual(P.parentTile(9, 150, 187, 1), { z: 8, x: 75, y: 93, scale: 2, offsetX: 0, offsetY: 1 });
  assert.deepStrictEqual(P.parentTile(1, 1, 1, 1), { z: 0, x: 0, y: 0, scale: 2, offsetX: 1, offsetY: 1 });
});
test('parentTile: several levels up, down to z0', () => {
  assert.deepStrictEqual(P.parentTile(9, 150, 187, 4), { z: 5, x: 9, y: 11, scale: 16, offsetX: 6, offsetY: 11 });
  assert.deepStrictEqual(P.parentTile(3, 7, 5, 3), { z: 0, x: 0, y: 0, scale: 8, offsetX: 7, offsetY: 5 });
  // The parent's square, scaled, contains the child's exactly.
  const c = { z: 7, x: 37, y: 101 };
  for (let n = 1; n <= 7; n++) {
    const p = P.parentTile(c.z, c.x, c.y, n);
    assert.strictEqual(p.x * p.scale + p.offsetX, c.x);
    assert.strictEqual(p.y * p.scale + p.offsetY, c.y);
    assert.ok(p.offsetX >= 0 && p.offsetX < p.scale && p.offsetY >= 0 && p.offsetY < p.scale);
  }
});
test('tileBounds: z0 is the Mercator world, z1/0/0 its north-west quarter', () => {
  const w = P.tileBounds(0, 0, 0);
  assert.deepStrictEqual([w.west, w.east, w.south], [-180, 180, -w.north]);
  between(w.north, 85.05, 85.06, 'north');
  const q = P.tileBounds(1, 0, 0);
  assert.deepStrictEqual([q.west, q.east, q.south], [-180, 0, 0]);
});
test('tileInsideRegions: wholly inside a region with that zoom only', () => {
  const regions = [{ name: 'conus', bbox: [-125, 24, -66, 50], minZoom: 6, maxZoom: 8 },
    { name: 'bad' }, null, { bbox: [1, 2], minZoom: 0, maxZoom: 9 }];
  // 6/18/24 is -78.75..-73.125, 38.82..40.98: inside conus.
  assert.strictEqual(P.tileInsideRegions(6, 18, 24, regions), true);
  assert.strictEqual(P.tileInsideRegions(9, 144, 192, regions), false);  // zoom 9: no region
  assert.strictEqual(P.tileInsideRegions(6, 20, 24, regions), false);   // -67.5..-61.875: on the -66 edge
  assert.strictEqual(P.tileInsideRegions(6, 18, 24, []), false);
  assert.strictEqual(P.tileInsideRegions(6, 18, 24, undefined), false);
});
test('parentTile: x wraps around the world, y off the world and bad n give null', () => {
  assert.deepStrictEqual(P.parentTile(2, -1, 1, 1), { z: 1, x: 1, y: 0, scale: 2, offsetX: 1, offsetY: 1 });
  assert.deepStrictEqual(P.parentTile(2, 5, 1, 1), { z: 1, x: 0, y: 0, scale: 2, offsetX: 1, offsetY: 1 });
  assert.strictEqual(P.parentTile(2, 1, -1, 1), null);
  assert.strictEqual(P.parentTile(2, 1, 4, 1), null);
  assert.strictEqual(P.parentTile(2, 1, 1, 0), null);
  assert.strictEqual(P.parentTile(2, 1, 1, 3), null);
  assert.strictEqual(P.parentTile(2, 1, 1, 1.5), null);
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

// ---- FIS-B (docs/web-api.md, FIS-B) ------------------------------------------
test('expandRle expands runs to exactly 128 bins', () => {
  const bins = P.expandRle([[0, 100], [3, 20], [7, 8]]);
  assert.ok(bins instanceof Uint8Array);
  assert.strictEqual(bins.length, 128);
  assert.strictEqual(bins[0], 0);
  assert.strictEqual(bins[99], 0);
  assert.strictEqual(bins[100], 3);
  assert.strictEqual(bins[119], 3);
  assert.strictEqual(bins[120], 7);
  assert.strictEqual(bins[127], 7);
  assert.deepStrictEqual(Array.from(P.expandRle([[0, 128]])), new Array(128).fill(0));
});
test('expandRle rejects a bad sum, a bad run and a non-array', () => {
  assert.throws(() => P.expandRle([[0, 127]]), /128/);
  assert.throws(() => P.expandRle([[0, 100], [1, 29]]), /128/);
  assert.throws(() => P.expandRle([[0, -1], [1, 129]]));
  assert.throws(() => P.expandRle([[0, 64], [256, 64]]));
  assert.throws(() => P.expandRle([[0]]));
  assert.throws(() => P.expandRle(null));
});

const REGIONAL = [
  { level: 0, dbz_max: 5, color: 'transparent' }, { level: 1, dbz_min: 5, dbz_max: 20, color: '#04e9e7' },
  { level: 2, color: '#02fd02' }, { level: 3, color: '#008e00' }, { level: 4, color: '#fdf802' },
  { level: 5, color: '#fd9500' }, { level: 6, color: '#fd0000' }, { level: 7, color: '#f800fd' }
];
const CONUS = REGIONAL.map((e) => Object.assign({}, e, e.level === 1 ? { color: 'transparent' } : {}));
test('dbzColor maps each level to its palette colour', () => {
  assert.strictEqual(P.dbzColor(0, REGIONAL), 'transparent');
  assert.strictEqual(P.dbzColor(1, REGIONAL), '#04e9e7');
  assert.strictEqual(P.dbzColor(4, REGIONAL), '#fdf802');
  assert.strictEqual(P.dbzColor(7, REGIONAL), '#f800fd');
  assert.strictEqual(P.dbzColor(0, CONUS), 'transparent');
  assert.strictEqual(P.dbzColor(1, CONUS), 'transparent');   // every empty CONUS block is level 1
  assert.strictEqual(P.dbzColor(2, CONUS), '#02fd02');
});
test('dbzColor: an unknown level or no palette is transparent', () => {
  assert.strictEqual(P.dbzColor(8, REGIONAL), 'transparent');
  assert.strictEqual(P.dbzColor(-1, REGIONAL), 'transparent');
  assert.strictEqual(P.dbzColor(2, null), 'transparent');
  // Entries are matched on `level`, not only on the index.
  assert.strictEqual(P.dbzColor(5, [{ level: 5, color: '#123456' }]), '#123456');
});

test('shapeStyle colours by family', () => {
  const fam = (p) => P.shapeStyle(p, false);
  for (const p of [8, 16, 17]) {
    assert.strictEqual(fam(p).color, P.SHAPE_COLORS.tfr, 'product ' + p);
    assert.match(fam(p).className, /\bfisb-tfr\b/);
  }
  assert.match(fam(12).className, /\bfisb-sigmet\b/);
  assert.match(fam(11).className, /\bfisb-airmet\b/);
  assert.match(fam(14).className, /\bfisb-airmet\b/);
  assert.match(fam(15).className, /\bfisb-cwa\b/);
  assert.match(fam(13).className, /\bfisb-sua\b/);
  assert.strictEqual(P.SHAPE_COLORS.tfr, '#f85149');       // red
  assert.strictEqual(P.SHAPE_COLORS.sigmet, '#bc8cff');    // purple
  assert.strictEqual(P.SHAPE_COLORS.airmet, '#e3b341');    // yellow
  assert.strictEqual(P.SHAPE_COLORS.cwa, '#f0883e');       // orange
  assert.strictEqual(P.SHAPE_COLORS.sua, '#8b949e');       // grey
  assert.strictEqual(fam(12).color, P.SHAPE_COLORS.sigmet);
  assert.strictEqual(fam(15).color, P.SHAPE_COLORS.cwa);
  assert.strictEqual(P.shapeFamily(99), null);
  assert.strictEqual(P.shapeFamily(16), 'tfr');
  assert.strictEqual(fam(8).dashArray, undefined);
});
test('shapeStyle dashes a cancelled shape', () => {
  const s = P.shapeStyle(8, true);
  assert.ok(typeof s.dashArray === 'string' && s.dashArray.length > 0);
  assert.match(s.className, /\bcancelled\b/);
  assert.doesNotMatch(P.shapeStyle(8, false).className, /\bcancelled\b/);
});
test('shapeStyle: a pending shape (not yet in force) is dashed and dimmed', () => {
  const active = P.shapeStyle(12, false, false), pending = P.shapeStyle(12, false, true);
  assert.match(pending.className, /\bpending\b/);
  assert.doesNotMatch(active.className, /\bpending\b/);
  assert.ok(typeof pending.dashArray === 'string' && pending.dashArray.length > 0);
  assert.strictEqual(active.dashArray, undefined);
  assert.ok(pending.opacity < active.opacity);
  assert.ok(pending.fillOpacity < active.fillOpacity);
  assert.strictEqual(pending.color, active.color);
  // Cancelled wins over pending.
  const both = P.shapeStyle(12, true, true);
  assert.strictEqual(both.dashArray, P.shapeStyle(12, true).dashArray);
  assert.doesNotMatch(both.className, /\bpending\b/);
});

test('radarOpacity: half past 15 min (regional) / 30 min (CONUS)', () => {
  const min = 60000;
  assert.strictEqual(P.radarOpacity(0, 63, 0.7), 0.7);
  assert.strictEqual(P.radarOpacity(15 * min, 63, 0.7), 0.7);
  assert.strictEqual(P.radarOpacity(15 * min + 1, 63, 0.7), 0.35);
  assert.strictEqual(P.radarOpacity(29 * min, 64, 0.8), 0.8);
  assert.strictEqual(P.radarOpacity(30 * min + 1, 64, 0.8), 0.4);
  assert.strictEqual(P.radarOpacity(70 * min, 64, 1), 0.5);
  assert.strictEqual(P.radarOpacity(NaN, 63, 0.7), 0.7);   // no product time: as is
  assert.strictEqual(P.radarFaded(16 * min, 63), true);
  assert.strictEqual(P.radarFaded(16 * min, 64), false);
  assert.strictEqual(P.RADAR_FADE_MS[63], 15 * min);
  assert.strictEqual(P.RADAR_FADE_MS[64], 30 * min);
});

test('skewWarning: only past 10 min ahead', () => {
  assert.strictEqual(P.skewWarning(undefined), null);
  assert.strictEqual(P.skewWarning(0), null);
  assert.strictEqual(P.skewWarning(-3600), null);
  assert.strictEqual(P.skewWarning(600), null);
  assert.strictEqual(P.skewWarning(601), 'product times 10 min ahead of the data clock');
  assert.strictEqual(P.skewWarning(3 * 3600), 'product times 3 h ahead of the data clock');
});

test('radarNeedsFetch: a missed publish only, never a mosaic still filling', () => {
  const pub = { published: '2020-10-30T08:56:31.128Z', blocks: 861 };
  assert.strictEqual(P.radarNeedsFetch(null, pub), true);                // nothing drawn
  assert.strictEqual(P.radarNeedsFetch(pub, pub), false);                // drawn
  assert.strictEqual(P.radarNeedsFetch(pub, Object.assign({}, pub, { blocks: 900 })), false);   // filling
  assert.strictEqual(P.radarNeedsFetch(pub, Object.assign({}, pub, { published: '2020-10-30T08:58:25.165Z' })), true);
  assert.strictEqual(P.radarNeedsFetch(null, { blocks: 12 }), false);    // never published
  assert.strictEqual(P.radarNeedsFetch(pub, null), false);
});

test('ageClass boundaries: fresh < 30 min, aging < 90 min, else old', () => {
  const issued = '2015-01-24T03:50:00.000Z';
  const t = Date.parse(issued);
  const min = 60000;
  assert.strictEqual(P.ageClass(issued, t), 'fresh');
  assert.strictEqual(P.ageClass(issued, t + 30 * min - 1), 'fresh');
  assert.strictEqual(P.ageClass(issued, t + 30 * min), 'aging');
  assert.strictEqual(P.ageClass(issued, t + 90 * min - 1), 'aging');
  assert.strictEqual(P.ageClass(issued, t + 90 * min), 'old');
  assert.strictEqual(P.ageClass(issued, t - 5 * min), 'fresh');   // issued "in the future": fresh
  assert.strictEqual(P.ageClass('not a time', t), 'old');
  assert.strictEqual(P.ageClass(undefined, t), 'old');
});

test('mosaicBounds of two blocks', () => {
  const b = P.mosaicBounds([
    { block: 1, lat0: 34, lon0: -121.6, dlat: 0.016667, dlon: 0.025, rle: [[0, 128]] },
    { block: 2, lat0: 35, lon0: -122.4, dlat: 0.016667, dlon: 0.025, rle: [[0, 128]] }
  ]);
  // NW corners; each block is 4 rows of dlat and 32 columns of dlon.
  between(b[0][0], 34 - 4 * 0.016667 - 1e-9, 34 - 4 * 0.016667 + 1e-9, 'south');
  between(b[0][1], -122.4 - 1e-9, -122.4 + 1e-9, 'west');
  between(b[1][0], 35 - 1e-9, 35 + 1e-9, 'north');
  between(b[1][1], -121.6 + 32 * 0.025 - 1e-9, -121.6 + 32 * 0.025 + 1e-9, 'east');
  assert.strictEqual(P.mosaicBounds([]), null);
});

test('mercY is 0 at the equator and grows with latitude', () => {
  assert.strictEqual(P.mercY(0), 0);
  assert.ok(P.mercY(60) - P.mercY(50) > P.mercY(10) - P.mercY(0));
  between(P.mercY(45), 50.49, 50.51, 'mercY(45) in degrees');
});

test('mosaicCanvasSize caps at 4096 and keeps the aspect', () => {
  const s = P.mosaicCanvasSize([[30, -100], [40, -90]], 0.025, 0.025, 4, 4096);
  assert.strictEqual(s.width, 1600);          // 10 deg / 0.025 * 4
  assert.ok(s.height > 1600 && s.height <= 4096);
  const big = P.mosaicCanvasSize([[20, -132], [53, -60]], 0.016667, 0.025, 4, 4096);
  assert.ok(big.width <= 4096 && big.height <= 4096);
  assert.ok(Math.max(big.width, big.height) === 4096);
  const uncapped = P.mosaicCanvasSize([[20, -132], [53, -60]], 0.016667, 0.025, 4, 1e9);
  assert.strictEqual(uncapped.width, 11520);  // 72 deg / 0.025 * 4
  between(big.width / big.height, uncapped.width / uncapped.height * 0.99,
    uncapped.width / uncapped.height * 1.01, 'aspect kept');
});

test('stationMatches: case-insensitive prefix', () => {
  assert.strictEqual(P.stationMatches('KSFO', 'ks'), true);
  assert.strictEqual(P.stationMatches('KSFO', 'KSF'), true);
  assert.strictEqual(P.stationMatches('KSFO', 'SFO'), false);
  assert.strictEqual(P.stationMatches('KSFO', ''), true);
  assert.strictEqual(P.stationMatches('', 'K'), false);
});

test('trackVisible: only sent and allow-listed survive hide filtered', () => {
  const all = ['send', 'allow_listed', 'out_of_range', 'below_elevation', 'blocked', 'no_position', 'weird', undefined];
  const keep = new Set(['send', 'allow_listed']);
  all.forEach((d) => {
    assert.strictEqual(P.trackVisible(d, false), true, 'off shows ' + d);
    assert.strictEqual(P.trackVisible(d, true), keep.has(d), 'on: ' + d);
  });
  assert.strictEqual(P.trackVisible('sent', true), true);   // colour-class spelling
  assert.strictEqual(P.trackVisible('allow', true), true);
});

test('hiddenCount counts drawn-but-filtered tracks only when on', () => {
  const ts = ['send', 'allow_listed', 'out_of_range', 'below_elevation', 'blocked', 'no_position', 'weird']
    .map((decision) => ({ decision, lat: 37, lon: -122 }));
  assert.strictEqual(P.hiddenCount(ts, false), 0);
  assert.strictEqual(P.hiddenCount(ts, true), 4);           // no_position is never drawn
  assert.strictEqual(P.hiddenCount([], true), 0);
  assert.strictEqual(P.hiddenCount([{ decision: 'blocked' }, { decision: 'blocked', lat: 1 }], true), 0); // no position
  assert.strictEqual(P.hiddenCount(ts.slice(0, 2), true), 0);
});

test('mapStyle and tileFilter: dark filters, anything else is light', () => {
  assert.deepStrictEqual(P.MAP_STYLES, ['light', 'dark']);
  assert.strictEqual(P.mapStyle('dark'), 'dark');
  [null, undefined, '', 'light', 'DARK', 'sepia', 1].forEach((s) => assert.strictEqual(P.mapStyle(s), 'light', String(s)));
  assert.strictEqual(P.tileFilter('light'), '');
  assert.strictEqual(P.tileFilter(null), '');
  const f = P.tileFilter('dark');
  assert.strictEqual(f, P.DARK_TILE_FILTER);
  assert.ok(/^invert\(1\) hue-rotate\(180deg\)/.test(f), f);
});

test('borders: admin0 always, admin1 from zoom 5; styled per map style', () => {
  [0, 2, 4, 5, 9, 14].forEach((z) => assert.strictEqual(P.borderVisible('admin0', z), true));
  assert.strictEqual(P.borderVisible('admin1', 4), false);
  assert.strictEqual(P.borderVisible('admin1', 4.9), false);
  assert.strictEqual(P.borderVisible('admin1', 5), true);
  assert.strictEqual(P.borderVisible('other', 9), false);
  assert.strictEqual(P.borderStyle('admin1', 'light', 4).opacity, 0);
  assert.ok(P.borderStyle('admin1', 'light', 6).opacity > 0);
  assert.ok(P.borderStyle('admin0', 'dark', 2).opacity > 0);
  const lum = (hex) => [1, 3, 5].reduce((s, i) => s + parseInt(hex.slice(i, i + 2), 16), 0);
  ['admin0', 'admin1'].forEach((k) => {
    const light = P.borderStyle(k, 'light', 8), dark = P.borderStyle(k, 'dark', 8);
    assert.ok(lum(light.color) < 3 * 128, k + ' light style draws dark lines');
    assert.ok(lum(dark.color) > 3 * 128, k + ' dark style draws light lines');
    assert.strictEqual(light.interactive, false);
    assert.strictEqual(light.fill, false);
  });
  assert.ok(P.borderStyle('admin1', 'light', 8).weight < P.borderStyle('admin0', 'light', 8).weight);
});

test('cityRankLimit: 2 to zoom 4, 4 at 5-6, everything from 7', () => {
  assert.strictEqual(P.cityRankLimit(0), 2);
  assert.strictEqual(P.cityRankLimit(4), 2);
  assert.strictEqual(P.cityRankLimit(5), 4);
  assert.strictEqual(P.cityRankLimit(6), 4);
  assert.strictEqual(P.cityRankLimit(7), Infinity);
  assert.strictEqual(P.cityRankLimit(12), Infinity);
});

test('cullCities: rank by zoom, viewport, order, cap', () => {
  const cities = [
    { name: 'A', rank: 0, pop: 100, lat: 40, lon: -75 },
    { name: 'B', rank: 3, pop: 900, lat: 41, lon: -74 },
    { name: 'C', rank: 7, pop: 50, lat: 39, lon: -76 },
    { name: 'D', rank: 0, pop: 500, lat: 41, lon: -73 },
    { name: 'Far', rank: 0, pop: 1e7, lat: 10, lon: 100 },
    { name: 'Bad', rank: 0, pop: 1 }
  ];
  const b = { south: 35, west: -80, north: 45, east: -70 };
  assert.deepStrictEqual(P.cullCities(cities, b, 3).map((c) => c.name), ['D', 'A']);
  assert.deepStrictEqual(P.cullCities(cities, b, 5).map((c) => c.name), ['D', 'A', 'B']);
  assert.deepStrictEqual(P.cullCities(cities, b, 8).map((c) => c.name), ['D', 'A', 'B', 'C']);
  assert.deepStrictEqual(P.cullCities(cities, b, 8, 2).map((c) => c.name), ['D', 'A']);
  // Crossing 180°: west > east.
  const pac = [{ name: 'E', rank: 1, pop: 1, lat: 0, lon: 179 }, { name: 'W', rank: 1, pop: 2, lat: 0, lon: -179 },
    { name: 'Mid', rank: 1, pop: 3, lat: 0, lon: 0 }];
  assert.deepStrictEqual(P.cullCities(pac, { south: -10, west: 170, north: 10, east: -170 }, 3).map((c) => c.name), ['W', 'E']);
  const many = [];
  for (let i = 0; i < 1000; i++) many.push({ name: 'n' + i, rank: i % 3, pop: i, lat: 40, lon: -75 });
  const out = P.cullCities(many, b, 9);
  assert.strictEqual(out.length, P.MAX_CITY_LABELS);
  assert.ok(out.every((c) => c.rank === 0), 'the most important kept');
  assert.strictEqual(out[0].name, 'n999');
});

test('overlayOffer: toggles from overlays/meta.json', () => {
  assert.deepStrictEqual(P.overlayOffer(null), { borders: false, cities: false });
  assert.deepStrictEqual(P.overlayOffer({}), { borders: false, cities: false });
  assert.deepStrictEqual(P.overlayOffer({ files: { admin0: { file: 'admin0.json' } } }), { borders: true, cities: false });
  assert.deepStrictEqual(P.overlayOffer({ files: { admin1: {}, cities: {} } }), { borders: true, cities: true });
  assert.deepStrictEqual(P.overlayOffer({ files: 'x' }), { borders: false, cities: false });
});

// ---- Receiver tab -------------------------------------------------------------
// The messages are parseInputDef's (skycued/src/inputs/input_def.cpp).
const CAT021_UDP = { id: 'ps-north', name: 'North mast', type: 'pingstation_cat021', enabled: true, transport: 'udp',
  bind: '0.0.0.0', port: 4001, peer: '', edition: '2.5', sac: null, sic: null, status_url: 'http://192.0.2.10/api/v1/status',
  status_interval_s: 30, time_mode: 'utc', capture: false };

test('validateInputDef accepts a pingstation_cat021 udp definition', () => {
  assert.deepStrictEqual(P.validateInputDef(CAT021_UDP), { ok: true, errors: {} });
  assert.deepStrictEqual(P.validateInputDef({ id: 'rx-2', type: 'readsb', host: 'pi.local', port: 30047 }),
    { ok: true, errors: {} });
});
test('validateInputDef rejects id "PS"', () => {
  const r = P.validateInputDef(Object.assign({}, CAT021_UDP, { id: 'PS' }));
  assert.strictEqual(r.ok, false);
  assert.deepStrictEqual(r.errors, { id: 'id must be 1 to 32 characters of a-z, 0-9 and -' });
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { id: 'readsb' })).errors.id, "id 'readsb' is reserved");
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { id: '' })).errors.id, 'id is required');
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { id: 'a'.repeat(33) })).ok, false);
});
test('validateInputDef rejects port 0 and 65536', () => {
  const msg = 'port must be an integer from 1 to 65535';
  [0, 65536, 1.5, NaN].forEach((port) => {
    assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { port })).errors.port, msg, String(port));
  });
  const noPort = Object.assign({}, CAT021_UDP);
  delete noPort.port;
  assert.strictEqual(P.validateInputDef(noPort).errors.port, 'port is required');
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { port: 65535 })).ok, true);
});
test('validateInputDef rejects https status_url', () => {
  const msg = 'status_url must be an http:// URL without userinfo, with a port of 1 to 65535';
  ['https://192.0.2.10/status', 'http://user@192.0.2.10/', 'http://192.0.2.10:0/', 'http://bad host/'].forEach((u) => {
    assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { status_url: u })).errors.status_url, msg, u);
  });
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { status_url: 'http://ps.local:8080?x' })).ok, true);
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { status_url: '' })).ok, true);
});
test('validateInputDef rejects unknown edition', () => {
  assert.strictEqual(P.validateInputDef(Object.assign({}, CAT021_UDP, { edition: '3.0' })).errors.edition,
    'edition must be one of 2.2, 2.3, 2.4, 2.5, 2.6, 2.7');
  const r = P.validateInputDef(Object.assign({}, CAT021_UDP, { bind: '256.0.0.1', sac: 256, status_interval_s: 4,
    time_mode: 'tai', name: 'x'.repeat(65) }));
  assert.deepStrictEqual(r.errors, {
    bind: 'bind must be an IPv4 address', sac: 'sac must be null or an integer from 0 to 255',
    status_interval_s: 'status_interval_s must be an integer from 5 to 3600', time_mode: 'time_mode must be utc or gps',
    name: 'name must be at most 64 printable characters' });
  const noTransport = Object.assign({}, CAT021_UDP);
  delete noTransport.transport;
  assert.strictEqual(P.validateInputDef(noTransport).errors.transport,
    'transport is required for pingstation_cat021 (udp, tcp_connect or tcp_listen)');
  const tcp = { id: 'ps-c', type: 'pingstation_cat021', transport: 'tcp_connect', port: 4000 };
  assert.strictEqual(P.validateInputDef(tcp).errors.host, 'host is required');
  ['-bad.example', '999.1.1.1', 'a..b'].forEach((host) => {
    assert.strictEqual(P.validateInputDef(Object.assign({}, tcp, { host })).errors.host,
      'host must be an IPv4 address or a hostname of at most 253 characters', host);
  });
  assert.strictEqual(P.validateInputDef({ id: 'x', type: 'replay', port: 1 }).errors.type,
    'type must be one of readsb, dump978, pingstation_json, pingstation_cat021');
});

test('inputFields for cat021 tcp_connect has host not bind', () => {
  const f = P.inputFields('pingstation_cat021', 'tcp_connect');
  assert.ok(f.indexOf('host') >= 0 && f.indexOf('bind') < 0 && f.indexOf('peer') < 0, f.join());
  assert.deepStrictEqual(P.inputFields('pingstation_cat021', 'udp'), ['id', 'name', 'transport', 'bind', 'port', 'peer',
    'edition', 'sac', 'sic', 'status_url', 'status_interval_s', 'time_mode', 'capture', 'enabled']);
  assert.deepStrictEqual(P.inputFields('pingstation_json'), ['id', 'name', 'bind', 'port', 'peer',
    'status_url', 'status_interval_s', 'time_mode', 'capture', 'enabled']);
  assert.deepStrictEqual(P.inputFields('readsb'), ['id', 'name', 'host', 'port', 'enabled']);
  // Before a transport is chosen the fields are udp's, the add form's preselected value.
  assert.deepStrictEqual(P.inputFields('pingstation_cat021'), P.inputFields('pingstation_cat021', 'udp'));
});

const live = (state, extra) => Object.assign({ id: 'ps', type: 'pingstation_json', enabled: true,
  state: Object.assign({ connected: true, receiving: true, status_fail_streak: 0 }, state) }, extra);
test('inputDot grey when disabled', () => {
  assert.strictEqual(P.inputDot(live({}, { enabled: false }), 0), 'grey');
  assert.strictEqual(P.inputDot(live({ connected: false }, { enabled: false }), 0), 'grey');
});
test('inputDot red when status failed 3 times', () => {
  assert.strictEqual(P.inputDot(live({ status_fail_streak: 3 }), 0), 'red');
  assert.strictEqual(P.inputDot(live({ status_fail_streak: 2 }), 0), 'green');
  assert.strictEqual(P.inputDot(live({ connected: false }), 0), 'red');
});
test('inputDot amber when connected but not receiving', () => {
  assert.strictEqual(P.inputDot(live({ receiving: false }), 0), 'amber');
});
test('inputDot green', () => {
  assert.strictEqual(P.inputDot(live({}), 0), 'green');
});

test('inputRates per second and null after a counter reset', () => {
  const prev = [{ id: 'a', messages: 100, observations: 90 }, { id: 'b', messages: 50, observations: 50 }];
  const cur = [{ id: 'a', messages: 120, observations: 100 }, { id: 'b', messages: 10, observations: 60 },
    { id: 'c', messages: 5, observations: 5 }];
  assert.deepStrictEqual(P.inputRates(prev, cur, 2), {
    a: { obs_s: 5, msg_s: 10 }, b: { obs_s: null, msg_s: null }, c: { obs_s: null, msg_s: null } });
  assert.deepStrictEqual(P.inputRates(prev, cur.slice(0, 1), 0), { a: { obs_s: null, msg_s: null } });
  assert.deepStrictEqual(P.inputRates(null, cur.slice(0, 1), 1), { a: { obs_s: null, msg_s: null } });
});

test('statusStale after 3 intervals', () => {
  const at = Date.parse('2026-11-02T12:00:00.000Z');
  const ds = { received_at: '2026-11-02T12:00:00.000Z' };
  assert.strictEqual(P.statusStale(ds, 30, at + 90000), false);
  assert.strictEqual(P.statusStale(ds, 30, at + 90001), true);
  assert.strictEqual(P.statusStale(ds, 5, at + 14000), false);
  assert.strictEqual(P.statusStale(ds, 5, at + 16000), true);
  assert.strictEqual(P.statusStale(null, 30, at), false);
  assert.strictEqual(P.statusStale({ received_at: 'x' }, 30, at), true);
});

test('inputEndpoint forms', () => {
  assert.strictEqual(P.inputEndpoint({ type: 'pingstation_json', bind: '0.0.0.0', port: 4000 }), 'udp 0.0.0.0:4000');
  assert.strictEqual(P.inputEndpoint({ type: 'pingstation_cat021', transport: 'udp', bind: '0.0.0.0', port: 4000 }),
    'udp 0.0.0.0:4000');
  assert.strictEqual(P.inputEndpoint({ type: 'pingstation_cat021', transport: 'tcp_connect', host: '192.0.2.10', port: 4000 }),
    'tcp → 192.0.2.10:4000');
  assert.strictEqual(P.inputEndpoint({ type: 'pingstation_cat021', transport: 'tcp_listen', bind: '0.0.0.0', port: 4000 }),
    'tcp listen 0.0.0.0:4000');
  assert.strictEqual(P.inputEndpoint({ type: 'readsb', host: '127.0.0.1', port: 30047 }), 'readsb 127.0.0.1:30047');
  assert.strictEqual(P.inputEndpoint({ type: 'dump978', host: '127.0.0.1', port: 30978 }), 'dump978 127.0.0.1:30978');
  assert.strictEqual(P.inputEndpoint({ type: 'replay' }), 'replay');
});

test('rejectedText shows rejected_peer only when non-zero', () => {
  assert.strictEqual(P.rejectedText({ rejected: 3, rejected_peer: 0 }), 'rejected 3');
  assert.strictEqual(P.rejectedText({}), 'rejected 0');
  assert.strictEqual(P.rejectedText({ rejected: 0, rejected_peer: 7 }), 'rejected 0 · rejected_peer 7');
});

test('inputsSummary counts', () => {
  assert.deepStrictEqual(P.inputsSummary([]), { connected: 0, enabled: 0 });
  assert.deepStrictEqual(P.inputsSummary(null), { connected: 0, enabled: 0 });
  assert.deepStrictEqual(P.inputsSummary([live({}), live({ connected: false }), live({}, { enabled: false }),
    live({ connected: true, receiving: false })]), { connected: 2, enabled: 3 });
});

test('gpsStatusText and receiverStatusText: the ICD codes, else the number', () => {
  assert.deepStrictEqual([0, 1, 2, 3, 4].map(P.gpsStatusText), ['no GPS', 'no lock', '2D fix', '3D fix', 'DGPS']);
  assert.strictEqual(P.gpsStatusText(7), '7');
  assert.deepStrictEqual([0, 1, 2].map(P.receiverStatusText), ['normal', 'comm errors', 'not transmitting']);
  assert.strictEqual(P.receiverStatusText(5), '5');
});

const FIX = {
 "metar:K2G4": "METAR K2G4 300855Z AUTO 35005KT 1/2SM OVC001 03/03 A2972 RMK AO1=",
 "metar:K0A9": "METAR K0A9 300835Z AUTO VRB05G13KT 1 1/2SM BR FEW004 BKN011 OVC024 11/10\n     A2986 RMK AO2=",
 "metar:KTVK": "METAR KTVK 300835Z AUTO 00000KT 10SM CLR M03/M03 A3022 RMK AO2=",
 "metar:KPVE": "METAR KPVE 300835Z AUTO 33009KT 280V360 10SM OVC020 08/06 A3012 RMK\n     A01=",
 "metar:KFKL": "SPECI KFKL 300814Z AUTO 02004KT 10SM OVC008 05/04 A2982 RMK AO2 CIG\n     007V011=",
 "metar:K6L4": "METAR K6L4 300835Z AUTO 2SM BR BKN006 OVC010 08/08 A2986 RMK AO2=",
 "metar:KCVG": "METAR KCVG 300852Z 33011KT 10SM OVC019 07/04 A3001 RMK AO2 SLP161\n     T00670039 51022=",
 "metar:KDAY": "SPECI KDAY 300833Z 35009KT 10SM FEW009 BKN014 OVC020 06/06 A3000 RMK AO2\n     RAE23 CIG 010 RWY 6L P0001 T00610056 $=",
 "metar:KCEY": "METAR KCEY 300835Z AUTO 29004KT 10SM 04/03 A3014 RMK AO1=",
 "metar:KUKF": "METAR KUKF 300835Z AUTO 31012G19KT 10SM CLR 15/08 A2974 RMK AO2=",
 "metar:KM08": "METAR KM08 300835Z AUTO VRB04KT 10SM OVC019 08/05 A3014 RMK A01=",
 "metar:KPRG": "METAR KPRG 300815Z 10SM CLR 02/00 A3014 RMK AO2 T00201005=",
 "taf:KCVG": "TAF.AMD KCVG 300857Z 3009/3112 33012KT P6SM OVC025\n      TEMPO 3009/3010 OVC019\n     FM301400 33011KT P6SM OVC035\n     FM310000 02004KT P6SM SCT250=",
 "taf:KHOP": "TAF KHOP 300800Z 3008/3114 33006KT 9999 SCT020 510063 QNH3007INS\n     BECMG 3010/3011 32006KT 9000 BR BKN007 OVC014 QNH3015INS\n     BECMG 3015/3016 34009KT 9999 NSW SKC QNH3022INS TX10/3020Z\n     TN03/3011Z=",
 "taf:KLOZ": "TAF KLOZ 300524Z 3006/3106 33008G15KT 6SM -DZ BR OVC008\n      TEMPO 3006/3010 P6SM NSW OVC015\n     FM301600 33007KT P6SM BKN015\n     FM302100 VRB04KT P6SM SCT030=",
 "winds:TUL:12": "WINDS TUL 301800Z  FT 3000 6000      9000   12000       18000   24000   30000    34000  39000\n   2808 9900+08 3509+05 2818+02 2835-10 2746-22 274738 274548 275656",
 "winds:ACK:6": "WINDS ACK 301200Z  FT 3000 6000      9000   12000       18000   24000   30000    34000  39000\n   0544 0817+00 2634+04 2535-01 2392-12 7327-21 724633 735141 735851",
 "winds:FWA:12": "WINDS FWA 301800Z  FT 3000 6000      9000   12000       18000   24000   30000    34000  39000\n   3309 3224-07 3134-05 3041-11 3186-13 3094-24 309638 800348 800057",
 "winds:GLD:6": "WINDS GLD 301200Z  FT      6000      9000   12000       18000   24000   30000    34000  39000\n        3107    3114+05 3117+00 2737-12 2648-25 275439 275547 274855",
 "metar:KMER": "METAR KMER 240355Z AUTO 10005KT 1 3/4SM OVC002 08/08 A3031 RMK AO1=",
 "taf:KBAB": "TAF KBAB 2403/2509 VRB06KT 9999 SKC QNH3022INS\n     BECMG 2405/2406 VRB06KT 4800 BR SCT010 QNH3018INS\n     BECMG 2410/2411 VRB06KT 0800 FG VV001 QNH3030INS\n     BECMG 2418/2419 VRB06KT 4800 BR SCT010 QNH3037INS\n     BECMG 2421/2422 33009KT 9999 NSW SKC QNH3028INS TX19/2421Z\n     TN06/2413Z=",
 "taf:KSTS": "TAF KSTS 232339Z 2400/2424 00000KT P6SM SKC\n     FM240700 00000KT 1/4SM FG VV002\n     FM241700 35003KT P6SM FEW002=",
 "winds:BCE:24": "WINDS BCE 250000Z  FT                       12000       18000   24000   30000    34000  39000\n                        3522+00 3631-13 0141-27 035046 046656 365258",
 "winds:RKS:24": "WINDS RKS 250000Z  FT                9000   12000       18000   24000   30000    34000  39000\n                3233    3221-05 3349-15 3461-28 356446 018956 335960",
 "metar:KJXN": "METAR KJXN 062056Z 24006KT 10SM CLR 30/19 A3009 RMK AO2 SLP182 6////\n    T03000189 58013="
};

const M = (k) => P.parseMetar(FIX[k]);
test('metar_categories_from_fixture_text', () => {
  const want = { 'metar:K2G4': 'LIFR', 'metar:K0A9': 'IFR', 'metar:KMER': 'LIFR', 'metar:KTVK': 'VFR', 'metar:KPVE': 'MVFR',
    'metar:KFKL': 'IFR', 'metar:K6L4': 'IFR', 'metar:KCVG': 'MVFR', 'metar:KDAY': 'MVFR' };
  for (const k of Object.keys(want)) assert.strictEqual(M(k).category, want[k], k);
  assert.strictEqual(M('metar:K0A9').visSm, 1.5); assert.strictEqual(M('metar:K0A9').ceilFt, 1100);
  assert.strictEqual(M('metar:KDAY').ceilFt, 1400);   // FEW009 is not a ceiling
  assert.strictEqual(M('metar:K2G4').station, 'K2G4');
});
test('metar_no_sky_group_is_unknown', () => {
  assert.strictEqual(M('metar:KCEY').ceilFt, null); assert.strictEqual(M('metar:KCEY').category, 'unknown');
});
test('metar_remarks_ignored', () => { assert.strictEqual(M('metar:KJXN').category, 'VFR'); });
test('metar_wind_forms', () => {
  assert.deepStrictEqual(M('metar:KTVK').wind, { dir: null, kt: 0, gust: null, calm: true, variable: false });
  assert.deepStrictEqual(M('metar:KUKF').wind, { dir: 310, kt: 12, gust: 19, calm: false, variable: false });
  assert.deepStrictEqual(M('metar:KM08').wind, { dir: null, kt: 4, gust: null, calm: false, variable: true });
  assert.deepStrictEqual(M('metar:KCVG').wind, { dir: 330, kt: 11, gust: null, calm: false, variable: false });
  assert.strictEqual(M('metar:KPRG').wind, null); assert.strictEqual(M('metar:KPRG').category, 'VFR');
});
test('flight_category_threshold_edges', () => {
  const F = P.flightCategory;
  assert.strictEqual(F(10, 499), 'LIFR'); assert.strictEqual(F(10, 500), 'IFR');
  assert.strictEqual(F(10, 999), 'IFR');  assert.strictEqual(F(10, 1000), 'MVFR');
  assert.strictEqual(F(10, 3000), 'MVFR'); assert.strictEqual(F(10, 3100), 'VFR');
  assert.strictEqual(F(0.75, Infinity), 'LIFR'); assert.strictEqual(F(1, Infinity), 'IFR');
  assert.strictEqual(F(2.75, Infinity), 'IFR'); assert.strictEqual(F(3, Infinity), 'MVFR');
  assert.strictEqual(F(5, Infinity), 'MVFR'); assert.strictEqual(F(6, Infinity), 'VFR');
  assert.strictEqual(F(6, 800), 'IFR');
  assert.strictEqual(F(null, Infinity), 'unknown'); assert.strictEqual(F(10, null), 'unknown');
});
test('visibility_forms', () => {   // synthetic token lists
  const V = (s) => P.visibilitySm(s.split(' '));
  assert.strictEqual(V('P6SM'), 6); assert.ok(V('M1/4SM') < 0.25); assert.strictEqual(V('1 1/2SM'), 1.5);
  assert.strictEqual(V('3/4SM'), 0.75); assert.ok(V('9999') > 5); assert.ok(Math.abs(V('4800') - 4800 / 1609.344) < 1e-9);
  assert.ok(Math.abs(V('0800') - 800 / 1609.344) < 1e-9); assert.strictEqual(V('////SM'), null);
  assert.strictEqual(P.ceilingFt(['CAVOK']), Infinity); assert.ok(P.visibilitySm(['CAVOK']) > 5);
  assert.strictEqual(P.ceilingFt(['BKN///']), null); assert.strictEqual(P.ceilingFt(['VV002']), 200);
});
test('taf_first_group_categories', () => {
  const T = (k) => P.parseTafFirst(FIX[k]);
  assert.strictEqual(T('taf:KCVG').station, 'KCVG'); assert.strictEqual(T('taf:KCVG').category, 'MVFR');
  assert.strictEqual(T('taf:KCVG').wind.dir, 330);
  assert.strictEqual(T('taf:KBAB').station, 'KBAB'); assert.strictEqual(T('taf:KBAB').category, 'VFR');   // no issue time; 9999
  assert.strictEqual(T('taf:KHOP').category, 'VFR');   // 9999 SCT020; later BECMG ignored
  assert.strictEqual(T('taf:KSTS').category, 'VFR');   // FM240700 1/4SM VV002 not in the first group
  assert.strictEqual(T('taf:KLOZ').category, 'IFR');   // OVC008
  // synthetic first groups built from the KBAB BECMG groups (metric visibility)
  assert.strictEqual(P.parseTafFirst('TAF KBAB 2405/2406 VRB06KT 4800 BR SCT010 QNH3018INS=').category, 'IFR');
  assert.strictEqual(P.parseTafFirst('TAF KBAB 2410/2411 VRB06KT 0800 FG VV001 QNH3030INS=').category, 'LIFR');
});
const W = (k) => P.parseWinds(FIX[k]);
test('winds_record_tul', () => {
  const w = W('winds:TUL:12');
  assert.strictEqual(w.station, 'TUL'); assert.strictEqual(w.valid, '301800');
  assert.deepStrictEqual(w.levels.map((l) => l.ft), P.WINDS_LEVELS_FT);
  assert.deepStrictEqual(P.windsLevel(w, 3000), { ft: 3000, dir: 280, kt: 8, temp: null, lv: false });
  assert.deepStrictEqual(P.windsLevel(w, 6000), { ft: 6000, dir: null, kt: 0, temp: 8, lv: true });
  assert.deepStrictEqual(P.windsLevel(w, 30000), { ft: 30000, dir: 270, kt: 47, temp: -38, lv: false });
  assert.deepStrictEqual(P.windsLevel(w, 39000), { ft: 39000, dir: 270, kt: 56, temp: -56, lv: false });
});
test('winds_100kt_encodings', () => {
  const ack = W('winds:ACK:6');
  assert.deepStrictEqual(P.windsLevel(ack, 18000), { ft: 18000, dir: 230, kt: 92, temp: -12, lv: false });
  assert.deepStrictEqual(P.windsLevel(ack, 24000), { ft: 24000, dir: 230, kt: 127, temp: -21, lv: false });
  assert.deepStrictEqual(P.windsLevel(ack, 30000), { ft: 30000, dir: 220, kt: 146, temp: -33, lv: false });
  assert.deepStrictEqual(P.windsLevel(W('winds:FWA:12'), 39000), { ft: 39000, dir: 300, kt: 100, temp: -57, lv: false });
  const lv = P.parseWinds('WINDS XYZ 301800Z  FT 39000\n   990042');   // synthetic
  assert.deepStrictEqual(P.windsLevel(lv, 39000), { ft: 39000, dir: null, kt: 0, temp: -42, lv: true });
});
test('winds_records_starting_above_3000', () => {
  const gld = W('winds:GLD:6');
  assert.strictEqual(gld.levels.length, 8); assert.strictEqual(P.windsLevel(gld, 3000), null);
  assert.deepStrictEqual(P.windsLevel(gld, 6000), { ft: 6000, dir: 310, kt: 7, temp: null, lv: false });
  const bce = W('winds:BCE:24');
  assert.strictEqual(bce.levels[0].ft, 12000);
  assert.deepStrictEqual(P.windsLevel(bce, 12000), { ft: 12000, dir: 350, kt: 22, temp: 0, lv: false });
  assert.deepStrictEqual(P.windsLevel(bce, 39000), { ft: 39000, dir: 360, kt: 52, temp: -58, lv: false });
  assert.deepStrictEqual(P.windsLevel(W('winds:RKS:24'), 9000), { ft: 9000, dir: 320, kt: 33, temp: null, lv: false });
});
test('winds_column_count_mismatch_is_null', () => {
  assert.strictEqual(P.parseWinds(FIX['winds:TUL:12'].replace(' 275656', '')), null);
  assert.strictEqual(P.parseWinds(FIX['winds:TUL:12'].replace('2808', '28X8')), null);
});
test('nearest_winds_period', () => {
  const recs = [{ period_h: 6, valid: '2020-10-30T12:00:00.000Z' }, { period_h: 12, valid: '2020-10-30T18:00:00.000Z' },
    { period_h: 24, valid: '2020-10-31T06:00:00.000Z' }];
  assert.strictEqual(P.nearestWindsPeriod(recs, Date.parse('2020-10-30T09:00:17Z')), 6);
  assert.strictEqual(P.nearestWindsPeriod(recs, Date.parse('2020-10-30T16:00:00Z')), 12);
  assert.strictEqual(P.nearestWindsPeriod([], 0), null);
});
test('pirep_ov_forms_from_fixture', () => {
  const lines = fs.readFileSync(path.join(__dirname, '..', 'fixtures', 'fisb', 'pireps.txt'), 'utf8')
    .split('\n').filter((l) => l && l[0] !== '#');
  assert.strictEqual(lines.length, 19);
  const parsed = lines.map(P.parsePirep);
  assert.ok(parsed.every((p) => p && p.urgent === false));
  assert.strictEqual(parsed.filter((p) => p.ov).length, 17);
  assert.deepStrictEqual(parsed[0], { station: 'ROD', urgent: false, ov: { id: 'ROD', radial: 40, nm: 30 } });
  const by = (s) => P.parsePirep(lines.find((l) => l.indexOf(s) >= 0));
  assert.deepStrictEqual(by('/OV DKK062005').ov, { id: 'DKK', radial: 62, nm: 5 });
  assert.deepStrictEqual(by('/OV YXU 315020').ov, { id: 'YXU', radial: 315, nm: 20 });
  assert.deepStrictEqual(by('/OV KCWA/').ov, { id: 'KCWA', radial: null, nm: null });
  assert.strictEqual(by('/OV SJX-GRB').ov, null);
  assert.strictEqual(by('/OV 4 W CVG').ov, null);
  assert.strictEqual(P.parsePirep(lines[0].replace(' UA ', ' UUA ')).urgent, true);
});
test('station_resolution_rules', () => {   // synthetic index
  const idx = { KIND: [39.717, -86.294], CYXU: [43.03, -81.15], '2XG': [30.3, -78.5], KMEM: [35.04, -89.98] };
  assert.deepStrictEqual(P.stationCandidates('winds', 'IND'), ['KIND', 'PIND', 'CIND', 'IND']);
  assert.deepStrictEqual(P.stationCandidates('metar', 'KIND'), ['KIND']);
  assert.deepStrictEqual(P.resolveStation('winds', 'IND', idx), [39.717, -86.294]);
  assert.deepStrictEqual(P.resolveStation('winds', 'ANC', Object.assign({ PANC: [61.174, -149.996] }, idx)), [61.174, -149.996]);
  assert.deepStrictEqual(P.resolveStation('winds', 'YXU', idx), [43.03, -81.15]);
  assert.deepStrictEqual(P.resolveStation('winds', '2XG', idx), [30.3, -78.5]);
  assert.strictEqual(P.resolveStation('metar', 'IND', idx), null);
  assert.strictEqual(P.resolveStation('taf', 'KXYZ', idx), null);
  assert.deepStrictEqual(P.pirepPosition({ ov: { id: 'KMEM', radial: null, nm: null } }, idx), [35.04, -89.98]);
  assert.strictEqual(P.pirepPosition({ ov: null }, idx), null);
  const d = P.pirepPosition({ ov: { id: 'MEM', radial: 360, nm: 2 } }, idx);
  between(P.rangeNm(35.04, -89.98, d[0], d[1]), 1.99, 2.01, 'distance'); assert.ok(d[0] > 35.04);
});
test('destination_point_geodesic', () => {
  const p = P.destinationPoint(0, 0, 90, 60);
  between(p[1], 0.999, 1.001, 'lon'); between(p[0], -1e-9, 1e-9, 'lat');
  between(P.bearingDeg(39.7, -86.3, ...P.destinationPoint(39.7, -86.3, 40, 30)), 39.9, 40.1, 'bearing');
});
test('barb_parts', () => {
  assert.deepStrictEqual(P.barbParts(0), { calm: true, pennants: 0, full: 0, half: 0 });
  assert.deepStrictEqual(P.barbParts(2), { calm: true, pennants: 0, full: 0, half: 0 });
  assert.deepStrictEqual(P.barbParts(16), { calm: false, pennants: 0, full: 1, half: 1 });
  assert.deepStrictEqual(P.barbParts(65), { calm: false, pennants: 1, full: 1, half: 1 });
  assert.deepStrictEqual(P.barbParts(127), { calm: false, pennants: 2, full: 2, half: 1 });
});

test('flight_category_just_above_edges', () => {
  const F = P.flightCategory;
  assert.strictEqual(F(10, 3001), 'VFR');
  assert.strictEqual(F(0.99, Infinity), 'LIFR'); assert.strictEqual(F(2.99, Infinity), 'IFR');
  assert.strictEqual(F(5.01, Infinity), 'VFR');
});
test('metar_trend_groups_ignored', () => {   // synthetic
  const m = P.parseMetar('METAR KXXX 300855Z 9999 BKN010 TEMPO BKN003=');
  assert.strictEqual(m.ceilFt, 1000); assert.strictEqual(m.category, 'MVFR');
  assert.strictEqual(P.parseMetar('METAR KXXX 300855Z 10SM CLR BECMG 1200 OVC002=').category, 'VFR');
  assert.strictEqual(P.parseMetar('METAR KXXX 300855Z 10SM CLR NOSIG=').category, 'VFR');
});
test('non_taf_text_and_zero_denominator', () => {
  assert.strictEqual(P.parseTafFirst('garbage text'), null);
  assert.strictEqual(P.parseTafFirst(FIX['metar:KCVG']), null);
  assert.strictEqual(P.visibilitySm(['1/0SM']), null);
});
test('winds_direction_code_out_of_range_is_null', () => {   // synthetic
  assert.strictEqual(P.parseWinds('WINDS XYZ 301800Z  FT 3000\n   9912'), null);
  assert.strictEqual(P.parseWinds('WINDS XYZ 301800Z  FT 3000\n   4012'), null);
  assert.strictEqual(P.parseWinds('WINDS XYZ 301800Z  FT 3000\n   8712'), null);
  assert.strictEqual(P.parseWinds('WINDS XYZ 301800Z  FT 3000\n   0012'), null);
  assert.deepStrictEqual(P.windsLevel(P.parseWinds('WINDS XYZ 301800Z  FT 3000\n   3612'), 3000),
    { ft: 3000, dir: 360, kt: 12, temp: null, lv: false });
  assert.deepStrictEqual(P.windsLevel(P.parseWinds('WINDS XYZ 301800Z  FT 39000\n   860560'), 39000),
    { ft: 39000, dir: 360, kt: 105, temp: -60, lv: false });
});
test('raster_products_and_bands', () => {
  const heads = [{ key: '70-0', product: 70, name: 'icing low', altitude_code: 0 }, { key: '70-3', product: 70, name: 'icing low', altitude_code: 3 },
    { key: '84', product: 84, name: 'cloud tops' }, { key: '91-1', product: 91, name: 'turbulence high', altitude_code: 1 }];
  const r = P.rasterProducts(heads);
  assert.deepStrictEqual(r.map((e) => e.product), [70, 84, 91]);
  assert.strictEqual(r[0].bands.length, 8); assert.strictEqual(r[0].bands[3].key, '70-3'); assert.strictEqual(r[0].bands[1].key, null);
  assert.strictEqual(r[1].key, '84'); assert.deepStrictEqual(r[1].bands, []);
  assert.strictEqual(P.defaultBand(r[2]), 1); assert.strictEqual(P.rasterBandFt(91, 1), 20000); assert.strictEqual(P.rasterBandFt(70, 0), 2000);
});
test('winds_periods_and_levels', () => {   // synthetic records
  const rec = (st, p, levels) => ({ kind: 'winds', station: st, period_h: p, text: 'WINDS ' + st + ' 301200Z  FT ' + levels.join(' ') + '\n   ' + levels.map(() => '3516').join(' ') });
  const rs = [rec('IND', 6, [3000, 6000]), rec('GLD', 6, [6000, 9000]), rec('IND', 12, [3000])];
  assert.deepStrictEqual(P.windsPeriods(rs), [6, 12]);
  assert.deepStrictEqual(P.windsLevelsFor(rs, 6), [3000, 6000, 9000]);
  assert.deepStrictEqual(P.windsLevelsFor(rs, 24), []);
});
test('ring_and_decision_rows', () => {
  const r = [];
  for (let i = 0; i < 205; i++) P.pushRing(r, i, 200);
  assert.strictEqual(r.length, 200); assert.strictEqual(r[0], 5);
  assert.deepStrictEqual(P.decisionRow({ type: 'drop', hex: 'a1b2c3', addr_type: 'icao', reason: 'filtered', decision: 'blocked',
    callsign: 'UAL1423', time: '2020-10-30T09:00:17.000Z' }),
    { key: 'a1b2c3|icao', time: '2020-10-30T09:00:17.000Z', hex: 'a1b2c3', callsign: 'UAL1423', decision: 'blocked', reason: 'filtered' });
  assert.strictEqual(P.decisionRow({ type: 'update', hex: 'a1b2c3', addr_type: 'icao', decision: 'send', last_seen: 'T' }).reason, 'sent');
  assert.strictEqual(P.decisionRow({ type: 'drop', hex: 'a1b2c3', addr_type: 'icao', reason: 'timeout', time: 'T' }).decision, '');
});
test('filter_history_rows_diff_consecutive', () => {
  const rows = P.filterHistoryRows([
    { time: 'T0', origin: 'startup', filters: { max_range_nm: 150, min_elevation_deg: 0 } },
    { time: 'T1', origin: 'ui', filters: { max_range_nm: 20, min_elevation_deg: 0 } },
    { time: 'T2', origin: 'file:skycue.conf', filters: { max_range_nm: 20, min_elevation_deg: 2 } }]);
  assert.deepStrictEqual(rows.map((r) => r.origin), ['file:skycue.conf', 'ui', 'startup']);
  assert.deepStrictEqual(rows[0].diffs, [{ field: 'min_elevation_deg', from: 0, to: 2 }]);
  assert.deepStrictEqual(rows[1].diffs, [{ field: 'max_range_nm', from: 150, to: 20 }]);
  assert.deepStrictEqual(rows[2].diffs, [{ field: 'max_range_nm', from: null, to: 150 }, { field: 'min_elevation_deg', from: null, to: 0 }]);
  assert.deepStrictEqual(P.filterHistoryRows([]), []);
});
const CFG = { daemon: { version: 'skycued 0.1.0', started: '2026-01-01T00:00:00.000Z' }, keys: [
  { key: 'RECEIVER_LAT', value: '38.9', source: 'conf', restart: true }, { key: 'WEB_BIND', value: '127.0.0.1', source: 'default', restart: true },
  { key: 'CUE_UDP_TARGETS', value: '127.0.0.1:30501 10.0.0.2:30502', source: 'conf', restart: true },
  { key: 'MAX_RANGE_NM', value: '20', source: 'overlay', restart: false }, { key: 'RECORD_DIR', value: '', source: 'default', restart: true },
  { key: 'NEW_KEY', value: 'x', source: 'conf', restart: true }] };
test('config_grouping', () => {
  const g = P.groupConfig(CFG.keys);
  assert.deepStrictEqual(g.map((x) => x.group), ['Receiver site', 'Cue outputs', 'Recording', 'Filters', 'Web', 'Other']);
  assert.deepStrictEqual(g[5].rows.map((r) => r.key), ['NEW_KEY']);
});
test('config_text', () => {
  const t = P.configText(CFG);
  assert.ok(t.startsWith('# skycued 0.1.0, started 2026-01-01T00:00:00.000Z\n'));
  assert.ok(t.includes('# Receiver site\nRECEIVER_LAT=38.9  # conf, restart\n'));
  assert.ok(t.includes('CUE_UDP_TARGETS="127.0.0.1:30501 10.0.0.2:30502"  # conf, restart\n'));
  assert.ok(t.includes('MAX_RANGE_NM=20  # overlay\n'));
  assert.ok(t.includes('RECORD_DIR=""  # default, restart\n'));
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
