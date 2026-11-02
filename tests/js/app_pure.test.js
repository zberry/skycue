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

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
