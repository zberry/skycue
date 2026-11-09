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

test('formatGeomAlt_reported', () => {
  assert.strictEqual(P.formatGeomAlt({ alt_geom_ft: 12300, alt_geom_source: 'reported' }), '12,300 ft');
  assert.strictEqual(P.formatGeomAlt({ alt_geom_ft: 12300 }), '12,300 ft');
});
test('formatGeomAlt_estimated_shows_est_and_plusminus', () => {
  assert.strictEqual(P.formatGeomAlt({ alt_geom_ft: 12300, alt_geom_source: 'estimated', alt_geom_uncertainty_ft: 240 }), '12,300 ft est. ±250');
  assert.strictEqual(P.formatGeomAlt({ alt_geom_ft: 12300, alt_geom_source: 'estimated', alt_geom_uncertainty_ft: 130 }), '12,300 ft est. ±150');
  assert.strictEqual(P.formatGeomAlt({ alt_geom_ft: 12300, alt_geom_source: 'estimated', alt_geom_uncertainty_ft: 10 }), '12,300 ft est. ±50');
});
test('formatGeomAlt_absent', () => {
  assert.strictEqual(P.formatGeomAlt({}), '—');
});
test('formatAlt_geom_fallback_estimated', () => {
  assert.strictEqual(P.formatAlt({ alt_geom_ft: 2650, alt_geom_source: 'estimated' }), '2,650 ft (geom est.)');
  assert.strictEqual(P.formatAlt({ alt_geom_ft: 2650, alt_geom_source: 'reported' }), '2,650 ft (geom)');
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

test('radarOpacityValue: a stored number in [0.2, 1], else 0.7', () => {
  assert.strictEqual(P.radarOpacityValue(null), 0.7);
  assert.strictEqual(P.radarOpacityValue('0.4'), 0.4);
  assert.strictEqual(P.radarOpacityValue('1'), 1);
  assert.strictEqual(P.radarOpacityValue('0.1'), 0.7);
  assert.strictEqual(P.radarOpacityValue('abc'), 0.7);
  assert.strictEqual(P.radarOpacityValue('0.2'), 0.2);
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

// ---- Feeds tab -------------------------------------------------------------
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
    'type must be one of readsb, dump978, pingstation_json, pingstation_cat021, beast_raw, uat_raw');
});

test('inputFields for cat021 tcp_connect has host not bind', () => {
  const f = P.inputFields('pingstation_cat021', 'tcp_connect');
  assert.ok(f.indexOf('host') >= 0 && f.indexOf('bind') < 0 && f.indexOf('peer') < 0, f.join());
  assert.deepStrictEqual(P.inputFields('pingstation_cat021', 'udp'), ['id', 'name', 'transport', 'bind', 'group',
    'interface', 'port', 'peer', 'edition', 'sac', 'sic', 'status_url', 'status_interval_s', 'time_mode', 'capture',
    'enabled']);
  assert.deepStrictEqual(P.inputFields('pingstation_json'), ['id', 'name', 'transport', 'bind', 'group', 'interface',
    'port', 'peer', 'source', 'status_url', 'status_interval_s', 'time_mode', 'capture', 'enabled']);
  assert.deepStrictEqual(P.inputFields('readsb'), ['id', 'name', 'transport', 'host', 'port', 'enabled']);
  // Before a transport is chosen the fields are udp's, the add form's preselected value.
  assert.deepStrictEqual(P.inputFields('pingstation_cat021'), P.inputFields('pingstation_cat021', 'udp'));
});

test('inputFields_per_type_and_transport', () => {
  const f = P.inputFields;
  // The type decides the default transport; any of the three is allowed.
  assert.deepStrictEqual(f('readsb', 'tcp_listen'), ['id', 'name', 'transport', 'bind', 'port', 'peer', 'enabled']);
  assert.deepStrictEqual(f('uat_raw'), ['id', 'name', 'transport', 'host', 'port', 'capture', 'enabled']);
  assert.deepStrictEqual(f('uat_raw', 'udp'), ['id', 'name', 'transport', 'bind', 'group', 'interface', 'port', 'peer',
    'capture', 'enabled']);
  assert.deepStrictEqual(f('beast_raw'), ['id', 'name', 'transport', 'host', 'port', 'format', 'capture', 'enabled']);
  assert.deepStrictEqual(f('pingstation_json', 'tcp_connect'), ['id', 'name', 'transport', 'host', 'port', 'source', 'status_url',
    'status_interval_s', 'time_mode', 'capture', 'enabled']);
  // group and interface only for udp; format only for beast_raw; capture not for readsb.
  ['readsb', 'dump978', 'uat_raw', 'beast_raw', 'pingstation_json', 'pingstation_cat021'].forEach((type) => {
    ['tcp_connect', 'tcp_listen'].forEach((t) => {
      const list = f(type, t);
      assert.ok(list.indexOf('group') < 0 && list.indexOf('interface') < 0, type + ' ' + t);
    });
    assert.ok(f(type, 'udp').indexOf('group') >= 0 && f(type, 'udp').indexOf('interface') >= 0, type);
    assert.strictEqual(f(type, 'udp').indexOf('format') >= 0, type === 'beast_raw', type);
  });
  assert.ok(f('readsb', 'udp').indexOf('capture') < 0);
  // A group binds the group itself: no bind.
  assert.ok(f('readsb', 'udp', '239.1.1.5').indexOf('bind') < 0);
  assert.ok(f('readsb', 'udp', '').indexOf('bind') >= 0);
  assert.strictEqual(P.inputTransport('beast_raw'), 'tcp_connect');
  assert.strictEqual(P.inputTransport('pingstation_json'), 'udp');
  assert.strictEqual(P.inputTransport('readsb', 'udp'), 'udp');
});

// One row per message of Task 1's input_def.cpp that the page can show
// (copy a changed server text here, and the page's helper with it).
test('validateInputDef_matches_server_messages_for_new_keys', () => {
  const rows = [
    [{ id: 'a', type: 'beast_raw', host: '192.0.2.1', port: 1, format: 'hex' }, 'format', 'format must be beast or avr'],
    [{ id: 'a', type: 'uat_raw', host: '192.0.2.1', port: 1, format: 'beast' }, 'format', 'format applies to beast_raw only'],
    [{ id: 'a', type: 'readsb', transport: 'udp', port: 1, group: '10.0.0.1' }, 'group',
      'group must be a multicast address (224.0.0.0 to 239.255.255.255)'],
    [{ id: 'a', type: 'readsb', host: '192.0.2.1', port: 1, group: '239.1.1.1' }, 'group', 'group applies to udp only'],
    [{ id: 'a', type: 'readsb', host: '192.0.2.1', port: 1, interface: '192.0.2.2' }, 'interface',
      'interface applies to udp only'],
    [{ id: 'a', type: 'readsb', transport: 'udp', port: 1, interface: '192.0.2.2' }, 'interface',
      'interface applies to a multicast group only'],
    [{ id: 'a', type: 'readsb', transport: 'udp', port: 1, group: '239.1.1.1', interface: 'eth0' }, 'interface',
      'interface must be an IPv4 address'],
    [{ id: 'a', type: 'readsb', transport: 'udp', port: 1, group: '239.1.1.1', bind: '0.0.0.0' }, 'bind',
      'bind does not apply when group is set'],
    [{ id: 'a', type: 'readsb', transport: 'udp', port: 1, bind: '999.1.1.1' }, 'bind', 'bind must be an IPv4 address'],
    [{ id: 'a', type: 'readsb', transport: 'tcp_listen', port: 1, host: '192.0.2.1' }, 'host',
      'host is not used with tcp_listen'],
    [{ id: 'a', type: 'readsb', transport: 'tcp_connect', port: 1, bind: '0.0.0.0' }, 'bind',
      'bind is not used with tcp_connect'],
    [{ id: 'a', type: 'readsb', transport: 'tcp_connect', port: 1, peer: '192.0.2.1' }, 'peer',
      'peer applies to udp and tcp_listen only'],
    [{ id: 'a', type: 'readsb', host: 'h', port: 1, capture: true }, 'capture',
      'capture applies to beast_raw, uat_raw, pingstation_json and pingstation_cat021 only'],
    [{ id: 'a', type: 'readsb', transport: 'tcp', port: 1 }, 'transport', 'transport must be udp, tcp_connect or tcp_listen'],
    [{ id: 'a', type: 'dump979', host: 'h', port: 1 }, 'type',
      'type must be one of readsb, dump978, pingstation_json, pingstation_cat021, beast_raw, uat_raw'],
    [{ id: 'a', type: 'readsb', port: 1 }, 'host', 'host is required']
  ];
  rows.forEach(([def, field, message]) => {
    assert.strictEqual(P.validateInputDef(def).errors[field], message, JSON.stringify(def));
  });
  const ok = [
    { id: 'a', type: 'beast_raw', host: '192.0.2.1', port: 30005, format: 'avr', capture: true },
    { id: 'a', type: 'uat_raw', transport: 'udp', group: '239.1.1.5', interface: '192.0.2.10', port: 4978 },
    { id: 'a', type: 'readsb', transport: 'tcp_listen', bind: '0.0.0.0', port: 30047, peer: '192.0.2.9' },
    { id: 'a', type: 'pingstation_json', transport: 'udp', group: '239.1.1.5', port: 4001 }
  ];
  ok.forEach((def) => assert.deepStrictEqual(P.validateInputDef(def), { ok: true, errors: {} }, JSON.stringify(def)));
});

test('outputFields_hide_ttl_without_group', () => {
  const f = P.outputFields;
  assert.deepStrictEqual(f('beast', 'tcp_listen'), ['id', 'name', 'kind', 'transport', 'bind', 'port', 'peer', 'enabled']);
  assert.deepStrictEqual(f('beast', 'tcp_connect'), ['id', 'name', 'kind', 'transport', 'host', 'port', 'enabled']);
  assert.deepStrictEqual(f('json', 'udp'), ['id', 'name', 'kind', 'transport', 'host', 'group', 'port', 'enabled']);
  assert.deepStrictEqual(f('json', 'udp', ''), f('json', 'udp'));
  assert.deepStrictEqual(f('json', 'udp', '239.1.1.5'), ['id', 'name', 'kind', 'transport', 'group', 'port',
    'interface', 'ttl', 'loopback', 'enabled']);
  ['tcp_listen', 'tcp_connect'].forEach((t) => {
    ['ttl', 'loopback', 'interface', 'group'].forEach((k) => assert.ok(f('beast', t, '239.1.1.5').indexOf(k) < 0, t + k));
  });
});

test('validateOutputDef_matches_server_messages', () => {
  const rows = [
    [{ kind: 'beast', transport: 'tcp_listen', port: 1 }, 'id', 'id is required'],
    [{ id: 'A', kind: 'beast', transport: 'tcp_listen', port: 1 }, 'id', 'id must be 1 to 32 characters of a-z, 0-9 and -'],
    [{ id: 'cue-tcp', kind: 'beast', transport: 'tcp_listen', port: 1 }, 'id', "id 'cue-tcp' is reserved"],
    [{ id: 'cue-udp-3', kind: 'beast', transport: 'tcp_listen', port: 1 }, 'id', "id 'cue-udp-3' is reserved"],
    [{ id: 'cue-connect-12', kind: 'beast', transport: 'tcp_listen', port: 1 }, 'id', "id 'cue-connect-12' is reserved"],
    [{ id: 'a', transport: 'tcp_listen', port: 1 }, 'kind', 'kind must be one of beast, uat, json, pingstation_json, cat021, cue'],
    [{ id: 'a', kind: 'raw', transport: 'tcp_listen', port: 1 }, 'kind', 'kind must be one of beast, uat, json, pingstation_json, cat021, cue'],
    [{ id: 'a', kind: 'beast', port: 1 }, 'transport', 'transport is required for outputs (tcp_listen, tcp_connect or udp)'],
    [{ id: 'a', kind: 'beast', transport: 'tcp', port: 1 }, 'transport', 'transport must be tcp_listen, tcp_connect or udp'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_listen' }, 'port', 'port is required'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_listen', port: 70000 }, 'port', 'port must be an integer from 1 to 65535'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_listen', port: 1, bind: 'x' }, 'bind', 'bind must be an IPv4 address'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_listen', port: 1, peer: 'x' }, 'peer', 'peer must be an IPv4 address'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_connect', port: 1, bind: '0.0.0.0' }, 'bind', 'bind applies to tcp_listen only'],
    [{ id: 'a', kind: 'beast', transport: 'udp', host: 'h', port: 1, peer: '192.0.2.1' }, 'peer', 'peer applies to tcp_listen only'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_listen', port: 1, host: 'h' }, 'host', 'host is not used with tcp_listen'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_connect', port: 1 }, 'host', 'host is required'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_connect', port: 1, host: '-x' }, 'host',
      'host must be an IPv4 address or a hostname of at most 253 characters'],
    [{ id: 'a', kind: 'beast', transport: 'tcp_connect', host: 'h', port: 1, group: '239.1.1.1' }, 'group', 'group applies to udp only'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, group: '10.0.0.1' }, 'group',
      'group must be a multicast address (224.0.0.0 to 239.255.255.255)'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1 }, 'host', 'udp needs a host (unicast) or a group (multicast)'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, host: 'h', group: '239.1.1.1' }, 'host',
      'host and group cannot both be set'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, host: 'h', ttl: 4 }, 'ttl',
      'ttl applies to a multicast group only'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, host: 'h', loopback: false }, 'loopback',
      'loopback applies to a multicast group only'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, host: 'h', interface: '192.0.2.1' }, 'interface',
      'interface applies to a multicast group only'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, group: '239.1.1.1', interface: 'eth0' }, 'interface',
      'interface must be an IPv4 address'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, group: '239.1.1.1', ttl: 0 }, 'ttl',
      'ttl must be an integer from 1 to 255'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, group: '239.1.1.1', ttl: 256 }, 'ttl',
      'ttl must be an integer from 1 to 255'],
    [{ id: 'a', kind: 'beast', transport: 'udp', port: 1, group: '239.1.1.1', loopback: 'yes' }, 'loopback',
      'loopback must be true or false'],
    [{ id: 'a', name: 'x'.repeat(65), kind: 'beast', transport: 'tcp_listen', port: 1 }, 'name',
      'name must be at most 64 printable characters']
  ];
  rows.forEach(([def, field, message]) => {
    assert.strictEqual(P.validateOutputDef(def).errors[field], message, JSON.stringify(def));
  });
  const ok = [
    { id: 'out-1', kind: 'beast', transport: 'tcp_listen', port: 30005 },
    { id: 'out-2', kind: 'json', transport: 'tcp_connect', host: '192.0.2.30', port: 30500, enabled: false },
    { id: 'out-3', kind: 'cat021', transport: 'udp', group: '239.1.1.5', interface: '192.0.2.10', ttl: 1, loopback: true,
      port: 4978 },
    { id: 'out-4', kind: 'cue', transport: 'udp', host: 'h.example', port: 4978 }
  ];
  ok.forEach((def) => assert.deepStrictEqual(P.validateOutputDef(def), { ok: true, errors: {} }, JSON.stringify(def)));
  assert.deepStrictEqual(P.OUTPUT_KINDS, ['beast', 'uat', 'json', 'pingstation_json', 'cat021', 'cue']);
});

test('outputEndpoint_text', () => {
  assert.strictEqual(P.outputEndpoint({ transport: 'tcp_listen', bind: '0.0.0.0', port: 30005 }), 'tcp_listen 0.0.0.0:30005');
  assert.strictEqual(P.outputEndpoint({ transport: 'tcp_connect', host: '192.0.2.30', port: 30500 }),
    'tcp_connect 192.0.2.30:30500');
  assert.strictEqual(P.outputEndpoint({ transport: 'udp', group: '239.1.1.5', interface: '192.0.2.10', ttl: 1, port: 4978 }),
    'udp 239.1.1.5:4978 via 192.0.2.10 ttl 1');
  assert.strictEqual(P.outputEndpoint({ transport: 'udp', group: '239.1.1.5', interface: null, ttl: 4, port: 4978 }),
    'udp 239.1.1.5:4978 ttl 4');
  assert.strictEqual(P.outputEndpoint({ transport: 'udp', host: '192.0.2.30', port: 4978 }), 'udp 192.0.2.30:4978');
});

test('outputDot_states', () => {
  const o = (enabled, state) => ({ enabled, state });
  assert.strictEqual(P.outputDot(o(false, { up: true, last_error: '' })), 'grey');
  assert.strictEqual(P.outputDot(o(true, { up: false, last_error: '' })), 'red');
  assert.strictEqual(P.outputDot(o(true, { up: false, last_error: 'bind failed' })), 'red');
  assert.strictEqual(P.outputDot(o(true, { up: true, last_error: 'connection reset' })), 'amber');
  assert.strictEqual(P.outputDot(o(true, { up: true, last_error: '' })), 'green');
  assert.strictEqual(P.outputDot({ enabled: true }), 'red');
});

test('inputEndpoint new transports', () => {
  assert.strictEqual(P.inputEndpoint({ type: 'readsb', transport: 'tcp_listen', bind: '0.0.0.0', port: 30047 }),
    'readsb listen 0.0.0.0:30047');
  assert.strictEqual(P.inputEndpoint({ type: 'uat_raw', host: '192.0.2.1', port: 30978 }), 'uat_raw 192.0.2.1:30978');
  assert.strictEqual(P.inputEndpoint({ type: 'beast_raw', transport: 'udp', group: '239.1.1.5', interface: '192.0.2.10',
    port: 4978 }), 'beast_raw udp 239.1.1.5:4978 via 192.0.2.10');
  assert.strictEqual(P.inputEndpoint({ type: 'pingstation_json', transport: 'udp', group: '239.1.1.5', port: 4001 }),
    'udp 239.1.1.5:4001');
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

test('validSource accepts device and decode only', () => {
  assert.strictEqual(P.validSource('device'), true);
  assert.strictEqual(P.validSource('decode'), true);
  ['', 'Decode', 'mlat', null, undefined, 1].forEach((v) => assert.strictEqual(P.validSource(v), false, String(v)));
  const ok = { id: 'ps', type: 'pingstation_json', port: 4000 };
  assert.strictEqual(P.validateInputDef(Object.assign({ source: 'decode' }, ok)).ok, true);
  assert.strictEqual(P.validateInputDef(Object.assign({ source: 'raw' }, ok)).errors.source, 'source must be device or decode');
  // The field belongs to pingstation_json only.
  assert.ok(P.inputFields('pingstation_json').indexOf('source') >= 0);
  assert.ok(P.inputFields('pingstation_cat021', 'udp').indexOf('source') < 0);
});

test('decoderLine_formats_rates_and_percent', () => {
  const prev = { decoding: true, frames: 1000, crc_fail: 100, crc_fixed: 3, cpr_pending: 5, cpr_rejected: 1, unknown_type: 0 };
  const cur = { decoding: true, frames: 1050, crc_fail: 105, crc_fixed: 4, cpr_pending: 7, cpr_rejected: 2, unknown_type: 3 };
  assert.strictEqual(P.decoderLine(cur, prev, 2),
    'frames/s 25.0 · CRC fail 10.0% · fixed 4 · pending 7 · rejected 2 · unknown 3');
  // No earlier item, a counter reset or dt <= 0: no rate; no frames: no percent.
  assert.strictEqual(P.decoderLine(cur, null, 1), 'frames/s — · CRC fail 10.0% · fixed 4 · pending 7 · rejected 2 · unknown 3');
  assert.ok(P.decoderLine(cur, prev, 0).startsWith('frames/s — · '));
  assert.ok(P.decoderLine(prev, cur, 1).startsWith('frames/s — · '));
  assert.strictEqual(P.decoderLine({ decoding: true }, null, 0),
    'frames/s — · CRC fail — · fixed 0 · pending 0 · rejected 0 · unknown 0');
});

test('decoderLine_appends_commb_and_ra_counts_when_non_zero', () => {
  const cur = { decoding: true, frames: 10, crc_fail: 0, commb_bds40: 2, commb_bds50: 3, commb_bds60: 4, acas_ra: 1 };
  assert.ok(P.decoderLine(cur, null, 1).endsWith(' · unknown 0 · Comm-B 4,0 2 · 5,0 3 · 6,0 4 · RA 1'));
  assert.ok(P.decoderLine({ decoding: true, frames: 1, acas_ra: 1 }, null, 1).endsWith('Comm-B 4,0 0 · 5,0 0 · 6,0 0 · RA 1'));
  assert.ok(P.decoderLine({ decoding: true, frames: 1, acas_ra: 1, acas_ra_tc28: 2 }, null, 1).endsWith('RA 3'));
  assert.ok(P.decoderLine({ decoding: true, frames: 1, commb_ambiguous: 5 }, null, 1).indexOf('Comm-B') < 0);
});

test('decoderLine_absent_without_decoding', () => {
  assert.strictEqual(P.decoderLine({ frames: 5 }, null, 1), null);
  assert.strictEqual(P.decoderLine({ decoding: false, frames: 5 }, { frames: 1 }, 1), null);
  assert.strictEqual(P.decoderLine(null, null, 1), null);
  assert.strictEqual(P.decoderLine(undefined, undefined, 1), null);
});

test('inputsSummary counts', () => {
  assert.deepStrictEqual(P.inputsSummary([]), { connected: 0, enabled: 0 });
  assert.deepStrictEqual(P.inputsSummary(null), { connected: 0, enabled: 0 });
  assert.deepStrictEqual(P.inputsSummary([live({}), live({ connected: false }), live({}, { enabled: false }),
    live({ connected: true, receiving: false })]), { connected: 2, enabled: 3 });
});

test('feedsSummary counts and singulars', () => {
  const up = () => live({}), stale = () => live({ connected: true, receiving: false });
  const down = () => live({ connected: false, receiving: false });
  assert.strictEqual(P.feedsSummary([up(), up(), up(), stale()], [{}, {}], { tcp: [{}, {}, {}], udp: [{}] }, [{}, {}]),
    '3 inputs up · 1 stale · 2 outputs · 4 clients · recording 2 files');
  assert.strictEqual(P.feedsSummary([up(), stale()], [{}], { tcp: [{}] }, [{}]),
    '1 input up · 1 stale · 1 output · 1 client · recording 1 file');
  assert.strictEqual(P.feedsSummary([up()], [{}], { udp: [{}] }, [{}]), '1 input up · 1 output · 1 client · recording 1 file');
  assert.strictEqual(P.feedsSummary([], [{}], { tcp: [{}] }, []), 'no feeds');
  assert.strictEqual(P.feedsSummary(null, null, null, null), 'no feeds');
  assert.strictEqual(P.feedsSummary([up(), down()], [], {}, null), '1 input up · 1 down');
  assert.strictEqual(P.feedsSummary([down()], [], null, []), '1 down');
});

test('gpsStatusText and receiverStatusText: the documented codes, else the number', () => {
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
  const lines = fs.readFileSync(path.join(__dirname, '..', 'fixtures', 'fisb', 'pireps-synthetic.txt'), 'utf8')
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
    { time: 'T2', origin: 'file:skycue.toml', filters: { max_range_nm: 20, min_elevation_deg: 2 } }]);
  assert.deepStrictEqual(rows.map((r) => r.origin), ['file:skycue.toml', 'ui', 'startup']);
  assert.deepStrictEqual(rows[0].diffs, [{ field: 'min_elevation_deg', from: 0, to: 2 }]);
  assert.deepStrictEqual(rows[1].diffs, [{ field: 'max_range_nm', from: 150, to: 20 }]);
  assert.deepStrictEqual(rows[2].diffs, [{ field: 'max_range_nm', from: null, to: 150 }, { field: 'min_elevation_deg', from: null, to: 0 }]);
  assert.deepStrictEqual(P.filterHistoryRows([]), []);
});
const SCHEMA = [
  { key: 'receiver.lat', type: 'float', class: 'restart', default: 0, min: -90, max: 90 },
  { key: 'inputs.readsb.host', type: 'string', class: 'restart', default: '127.0.0.1' },
  { key: 'cues.udp_targets', type: 'host_port_list', class: 'restart', default: [] },
  { key: 'cues.min_interval_s', type: 'float', class: 'live', default: 1, min: 0.1, max: 5 },
  { key: 'web.bind', type: 'string', class: 'bootstrap', default: '127.0.0.1' },
  { key: 'web.trail_seconds', type: 'int', class: 'live', default: 300, min: 0, max: 3600 },
  { key: 'recording.formats', type: 'string_set', class: 'restart', default: ['jsonl'], values: ['jsonl', 'csv'] },
  { key: 'recording.dir', type: 'path', class: 'restart', default: '' },
  { key: 'recording.raw', type: 'bool', class: 'restart', default: false },
  { key: 'sdr.mode', type: 'enum', class: 'restart', default: 'skycued', values: ['skycued', 'external'] },
  { key: 'stats.save_s', type: 'int', class: 'external', default: 60 }];
const KEYS = [
  { key: 'web.trail_seconds', value: '300', source: 'env', restart: false, class: 'live', editable: false, default: 300 },
  { key: 'receiver.lat', value: '38.9', source: 'conf', restart: true, class: 'restart', editable: true, default: 0 },
  { key: 'web.bind', value: '127.0.0.1', source: 'default', restart: true, class: 'bootstrap', editable: false, default: '127.0.0.1' },
  { key: 'cues.udp_targets', value: '127.0.0.1:30501 10.0.0.2:30502', source: 'conf', restart: true, class: 'restart', editable: true, default: '' },
  { key: 'cues.min_interval_s', value: '2', source: 'overrides', restart: false, class: 'live', editable: true, file_value: '1.5', default: 1 },
  { key: 'recording.dir', value: '', source: 'default', restart: true, class: 'restart', editable: true, default: '' },
  { key: 'stats.save_s', value: '60', source: 'set', restart: true, class: 'external', editable: false, default: 60 },
  { key: 'new.key', value: 'x', source: 'conf', restart: true, class: 'restart', editable: true, default: '' }];
// Values as GET /api/config sends them: all text (arrays space-separated, bools "true"/"false").
const CFG = { daemon: { version: 'skycued 0.1.0', started: '2026-01-01T00:00:00.000Z' }, schema: SCHEMA.concat([
  { key: 'filters.max_range_nm', type: 'float', class: 'live', default: 0 }, { key: 'sdr.gain_1090', type: 'string', class: 'restart', default: '' }]),
  keys: [
  { key: 'receiver.lat', value: '38.9', source: 'conf', restart: true }, { key: 'web.bind', value: '127.0.0.1', source: 'default', restart: true },
  { key: 'cues.udp_targets', value: '127.0.0.1:30501 10.0.0.2:30502', source: 'conf', restart: true },
  { key: 'filters.max_range_nm', value: '20', source: 'runtime', restart: false }, { key: 'recording.dir', value: '', source: 'default', restart: true },
  { key: 'web.trail_seconds', value: '300', source: 'env', restart: true }, { key: 'stats.save_s', value: '60', source: 'set', restart: true },
  { key: 'recording.raw', value: 'false', source: 'conf', restart: true }, { key: 'sdr.gain_1090', value: '49.6', source: 'conf', restart: true },
  { key: 'new.key', value: 'x', source: 'conf', restart: true }] };
const spec = (k) => SCHEMA.find((x) => x.key === k);
test('configTables groups by TOML table in schema order with bare keys', () => {
  const t = P.configTables(KEYS, SCHEMA);
  assert.deepStrictEqual(t.map((x) => x.table), ['receiver', 'cues', 'web', 'recording', 'stats', 'other']);
  assert.strictEqual(t[0].rows[0].bare, 'lat');
  assert.strictEqual(t[0].rows[0].key, 'receiver.lat');
  assert.strictEqual(t[0].rows[0].cls, 'restart');
  assert.strictEqual(t[0].rows[0].spec, spec('receiver.lat'));
  assert.deepStrictEqual(t[1].rows.map((r) => r.bare), ['udp_targets', 'min_interval_s']);
  assert.strictEqual(t[1].rows[1].file_value, '1.5');
  assert.strictEqual(t[1].rows[1].source, 'overrides');
  assert.strictEqual(t[5].rows[0].key, 'new.key');
  assert.strictEqual(t[5].rows[0].bare, 'new.key');
  assert.strictEqual(t[5].rows[0].spec, null);
  assert.deepStrictEqual(P.configTables([], SCHEMA), []);
});
test('configControl kinds and steps', () => {
  const c = (k) => P.configControl(spec(k));
  assert.deepStrictEqual([c('web.trail_seconds').kind, c('web.trail_seconds').step, c('web.trail_seconds').min, c('web.trail_seconds').max], ['number', 1, 0, 3600]);
  assert.deepStrictEqual([c('receiver.lat').kind, c('receiver.lat').step], ['number', 1]);
  assert.deepStrictEqual([c('cues.min_interval_s').kind, c('cues.min_interval_s').step], ['number', 0.1]);
  assert.strictEqual(P.configControl({ key: 'a.b', type: 'float', min: 0.5 }).step, 0.1);
  assert.strictEqual(P.configControl({ key: 'a.b', type: 'float', min: 0 }).step, 1);
  assert.strictEqual(c('recording.raw').kind, 'checkbox');
  assert.deepStrictEqual([c('sdr.mode').kind, c('sdr.mode').values], ['select', ['skycued', 'external']]);
  assert.deepStrictEqual([c('recording.formats').kind, c('recording.formats').values], ['checkset', ['jsonl', 'csv']]);
  assert.strictEqual(c('cues.udp_targets').kind, 'lines');
  assert.strictEqual(c('recording.dir').kind, 'text');
  assert.strictEqual(c('web.bind').kind, 'text');
});
test('configParse per kind', () => {
  assert.deepStrictEqual(P.configParse(spec('web.trail_seconds'), '300'), { value: 300 });
  assert.deepStrictEqual(P.configParse(spec('web.trail_seconds'), '3.5'), { error: 'trail_seconds: expected an integer' });
  assert.deepStrictEqual(P.configParse(spec('web.trail_seconds'), ''), { error: 'trail_seconds: expected an integer' });
  assert.deepStrictEqual(P.configParse(spec('receiver.lat'), '38.9'), { value: 38.9 });
  assert.deepStrictEqual(P.configParse(spec('receiver.lat'), 'abc'), { error: 'lat: expected a number' });
  assert.deepStrictEqual(P.configParse(spec('cues.udp_targets'), 'a:1\n\nb:2\n'), { value: ['a:1', 'b:2'] });
  assert.deepStrictEqual(P.configParse(spec('cues.udp_targets'), ' a:1 \r\n b:2'), { value: ['a:1', 'b:2'] });
  assert.deepStrictEqual(P.configParse(spec('cues.udp_targets'), ''), { value: [] });
  assert.deepStrictEqual(P.configParse(spec('recording.formats'), ['jsonl']), { value: ['jsonl'] });
  assert.deepStrictEqual(P.configParse(spec('sdr.mode'), 'skycued'), { value: 'skycued' });
  assert.deepStrictEqual(P.configParse(spec('recording.raw'), true), { value: true });
  assert.deepStrictEqual(P.configParse(spec('recording.dir'), '/x y'), { value: '/x y' });
});
test('configStage body and bar text', () => {
  const s = P.configStage({ 'web.trail_seconds': 1, 'cues.min_interval_s': null, 'receiver.lat': 'x' });
  assert.deepStrictEqual(s.body, { set: { 'web.trail_seconds': 1, 'receiver.lat': 'x' }, clear: ['cues.min_interval_s'] });
  assert.strictEqual(s.text, '3 changes');
  assert.deepStrictEqual(s.keys, ['web.trail_seconds', 'cues.min_interval_s', 'receiver.lat']);
  const o = P.configStage({ 'web.trail_seconds': 1, 'cues.min_interval_s': null, 'receiver.lat': 'x' }, SCHEMA);
  assert.deepStrictEqual(o.keys, ['receiver.lat', 'cues.min_interval_s', 'web.trail_seconds']);
  assert.strictEqual(P.configStage({ a: 1 }).text, '1 change');
  assert.deepStrictEqual(P.configStage({}).body, { set: {}, clear: [] });
});
test('pendingRestartText plural', () => {
  assert.strictEqual(P.pendingRestartText([]), '');
  assert.strictEqual(P.pendingRestartText(undefined), '');
  assert.strictEqual(P.pendingRestartText([{ key: 'a.b' }]), '1 setting waits for a restart');
  assert.strictEqual(P.pendingRestartText([{ key: 'a.b' }, { key: 'c.d' }]), '2 settings wait for a restart');
});
test('configRowHint', () => {
  assert.strictEqual(P.configRowHint({ source: 'overrides', file_value: 150 }), 'file: 150');
  assert.strictEqual(P.configRowHint({ source: 'overrides', file_value: 'a' }), 'file: a');
  assert.strictEqual(P.configRowHint({ source: 'overrides' }), '');
  assert.strictEqual(P.configRowHint({ source: 'conf', file_value: 1 }), '');
});
test('configDisabledTitle for set, env and bootstrap', () => {
  assert.strictEqual(P.configDisabledTitle({ key: 'stats.save_s', source: 'set', editable: false }), 'set by --set');
  assert.strictEqual(P.configDisabledTitle({ key: 'web.bind', source: 'env', editable: false }), 'set by SKYCUE_WEB_BIND');
  assert.strictEqual(P.configDisabledTitle({ key: 'inputs.readsb.host', source: 'env', editable: false }), 'set by SKYCUE_INPUTS_READSB_HOST');
  assert.strictEqual(P.configDisabledTitle({ key: 'inputs.readsb.enable', source: 'flag', editable: false }), 'set by a command-line flag');
  assert.strictEqual(P.configDisabledTitle({ key: 'web.bind', source: 'default', cls: 'bootstrap', editable: false }), 'edit skycue.toml and restart');
  assert.strictEqual(P.configDisabledTitle({ key: 'receiver.lat', source: 'conf', cls: 'restart', editable: true }), '');
});
test('config_text groups by table without CONFIG_GROUPS', () => {
  const t = P.configText(CFG);
  assert.strictEqual(P.CONFIG_GROUPS, undefined);
  assert.ok(t.startsWith('# skycued 0.1.0, started 2026-01-01T00:00:00.000Z\n'));
  assert.ok(t.includes('# [receiver]\n[receiver]\n# receiver.lat: conf, restart\nlat = 38.9\n'));
  assert.ok(t.includes('# cues.udp_targets: conf, restart\nudp_targets = ["127.0.0.1:30501", "10.0.0.2:30502"]\n'));
  assert.ok(t.includes('# filters.max_range_nm: runtime\nmax_range_nm = 20\n'));
  assert.ok(t.includes('# recording.dir: default, restart\ndir = ""\n'));
  assert.ok(t.includes('# recording.raw: conf, restart\nraw = false\n'));
  assert.ok(t.includes('gain_1090 = "49.6"\n'));
  assert.ok(t.includes('# new.key = x\n'));
  assert.ok(t.includes('# web.trail_seconds: env, restart\ntrail_seconds = 300\n'));
  assert.ok(t.includes('# stats.save_s: --set, restart\nsave_s = 60\n'));
  assert.ok(t.indexOf('# [receiver]') < t.indexOf('# [web]') && t.indexOf('# [web]') < t.indexOf('# [recording]'));
  assert.strictEqual(t.split('\n[web]\n').length, 2, 'one [web] header');
});
test('pinned filters disable their controls', () => {
  const keys = [{ key: 'filters.max_range_nm', source: 'set' }, { key: 'filters.min_elevation_deg', source: 'env' },
    { key: 'web.port', source: 'env' }];
  assert.deepStrictEqual(P.pinnedFilters(keys), { max_range_nm: 'set', min_elevation_deg: 'env' });
  assert.deepStrictEqual(P.pinnedFilters([{ key: 'filters.max_range_nm', source: 'runtime' },
    { key: 'filters.min_elevation_deg', source: 'overrides' }]), {});
  assert.deepStrictEqual(P.pinnedFilters(undefined), {});
});
test('config panel shows dotted keys and env/--set sources', () => {
  const keys = ['receiver.lat', 'inputs.readsb.host', 'filters.max_range_nm', 'web.trail_seconds', 'stats.save_s', 'web.port'].map((k) => ({ key: k, value: '1', source: 'conf', restart: false }));
  const rows = P.configTables(keys, keys.map((k) => ({ key: k.key }))).reduce((a, g) => a.concat(g.rows.map((r) => r.key)), []);
  assert.deepStrictEqual(rows, ['receiver.lat', 'inputs.readsb.host', 'filters.max_range_nm', 'web.trail_seconds', 'web.port', 'stats.save_s']);
  assert.deepStrictEqual(['conf', 'overrides', 'runtime', 'flag', 'env', 'set', 'default'].map(P.configSourceLabel),
    ['conf', 'overrides', 'runtime', 'flag', 'env', '--set', 'default']);
});
test('daemon_mode_and_locations_text', () => {
  assert.strictEqual(P.daemonModeText({ mode: 'live', readsb: true, dump978: true }), 'live: readsb, dump978');
  assert.strictEqual(P.daemonModeText({ mode: 'live', readsb: true, dump978: false }), 'live: readsb, no dump978 (--no-uat)');
  assert.strictEqual(P.daemonModeText({ mode: 'live', readsb: false, dump978: true }), 'live: no readsb (--no-readsb), dump978');
  assert.strictEqual(P.daemonModeText({ mode: 'replay', replay: { file: '/d/r.jsonl', speed: 0, loop: true } }),
    'replay of /d/r.jsonl (unpaced, looped)');
  assert.strictEqual(P.daemonModeText({ mode: 'replay', replay: { file: 'r.jsonl', speed: 2, loop: false } }), 'replay of r.jsonl (speed 2)');
  assert.deepStrictEqual(P.daemonLocationsText({ locations: { learned_path: '/e/fisb-locations.json', learned: 42,
    operator_path: '', operator: 0 } }), ['42 learned, kept in /e/fisb-locations.json', 'no paths.locations_file']);
  assert.deepStrictEqual(P.daemonLocationsText({ locations: { learned_path: '', learned: 0, operator_path: '/e/l.csv',
    operator: 2, operator_rejected: 1, operator_overflow: 3 } }),
    ['0 learned, not saved this run', '2 from /e/l.csv, 1 lines rejected, 3 over the cap']);
});
test('toml_value_every_branch', () => {
  // The API's value text, written by the schema type.
  const v = (type, text) => P.tomlValue({ key: 'a.b', type }, text);
  assert.strictEqual(v('float', '38.9'), '38.9');
  assert.strictEqual(v('float', '-77'), '-77');
  assert.strictEqual(v('float', ''), null);                // unset: left as a comment
  assert.strictEqual(v('float', 'abc'), null);
  assert.strictEqual(v('int', '8080'), '8080');
  assert.strictEqual(v('int', '80 80'), null);
  assert.strictEqual(v('int', '3.5'), null);
  assert.strictEqual(v('bool', 'true'), 'true');
  assert.strictEqual(v('bool', 'false'), 'false');
  assert.strictEqual(v('bool', 'yes'), null);
  assert.strictEqual(v('host_port_list', ''), '[]');
  assert.strictEqual(v('host_port_list', 'a:1 b:2'), '["a:1", "b:2"]');
  assert.strictEqual(v('string_set', 'jsonl'), '["jsonl"]');
  assert.strictEqual(v('string', '49.6'), '"49.6"');       // numeric-looking text stays a string
  assert.strictEqual(v('string', ''), '""');
  assert.strictEqual(v('path', 'a b'), '"a b"');
  assert.strictEqual(v('path', 'say "hi" \\ $1'), '"say \\"hi\\" \\\\ $1"');
  assert.strictEqual(v('path', 'a\nb\tc\u0001\u007f'), '"a\\nb\\tc\\u0001\\u007F"');
  assert.strictEqual(v('path', 'a\u0000b'), null);
  assert.strictEqual(v('enum', 'readsb'), '"readsb"');
  assert.strictEqual(v('weird', 'x'), null);
});
// A reader for the lines configText writes: a comment, a [table] header, or key = value with a
// string (the escapes \" \\ \n \t \r \uXXXX), a number, a boolean or an array of strings
// (skycued's toml_lite, test_config.cpp config_copy_text_parses, accepts the same text).
function tomlLines(text) {
  const out = {};
  let table = null;
  const str = (v) => /^"((?:[^"\\]|\\.)*)"$/.exec(v) && JSON.parse(v);
  text.split('\n').forEach((l) => {
    if (/^\s*(#|$)/.test(l)) return;
    let m = /^\[([a-z0-9_.]+)\]$/.exec(l);
    if (m) { assert.ok(!(m[1] in out) && table !== m[1], 'table twice'); table = m[1]; return; }
    m = /^([a-z0-9_]+) = (.*)$/.exec(l);
    assert.ok(m && table, 'malformed line: ' + l);
    const v = m[2];
    let val;
    if (str(v) !== null && str(v) !== undefined && str(v) !== false) val = str(v);
    else if (/^\[.*\]$/.test(v)) val = v === '[]' ? '' : v.slice(1, -1).split(', ').map((w) => JSON.parse(w)).join(' ');
    else if (/^(-?\d+(\.\d+)?|true|false)$/.test(v)) val = v;
    else throw new Error('malformed value: ' + l);
    out[table + '.' + m[1]] = val;
  });
  return out;
}
// The same keys and values test_config.cpp (config_copy_text_parses) checks.
const CFG_COPY = { daemon: { version: 'skycued 0.1.0', started: '2026-01-01T00:00:00.000Z' }, schema: [
  { key: 'receiver.lat', type: 'float' }, { key: 'receiver.lon', type: 'float' }, { key: 'sdr.serial_1090', type: 'string' },
  { key: 'sdr.gain_1090', type: 'string' }, { key: 'cues.tcp_port', type: 'int' }, { key: 'cues.udp_targets', type: 'host_port_list' },
  { key: 'recording.dir', type: 'path' }, { key: 'recording.raw', type: 'bool' }, { key: 'filters.max_range_nm', type: 'float' },
  { key: 'filters.allow_list_file', type: 'path' }, { key: 'filters.block_list_file', type: 'path' }, { key: 'paths.locations_file', type: 'path' }], keys: [
  { key: 'receiver.lat', value: '38.9', source: 'conf', restart: true },
  { key: 'receiver.lon', value: '-77.0', source: 'conf', restart: true },
  { key: 'sdr.serial_1090', value: '', source: 'default', restart: true },
  { key: 'sdr.gain_1090', value: '49.6', source: 'conf', restart: true },
  { key: 'cues.tcp_port', value: '30003', source: 'conf', restart: true },
  { key: 'cues.udp_targets', value: '127.0.0.1:30501 10.0.0.2:30502', source: 'conf', restart: true },
  { key: 'recording.dir', value: '/srv/sky cue/rec', source: 'conf', restart: true },
  { key: 'recording.raw', value: 'false', source: 'conf', restart: true },
  { key: 'filters.max_range_nm', value: '20', source: 'runtime', restart: false },
  { key: 'filters.allow_list_file', value: 'allow "main" $1.txt', source: 'conf', restart: false },
  { key: 'filters.block_list_file', value: "it's $HOME.txt", source: 'conf', restart: false },
  { key: 'paths.locations_file', value: "o'hare locations.csv", source: 'conf', restart: true }] };
test('config_text_round_trips_through_toml', () => {
  const t = P.configText(CFG_COPY);
  // test_config.cpp parses this same fixture with parseConfigText.
  assert.strictEqual(t, fs.readFileSync(path.join(__dirname, '..', 'fixtures', 'config-copy.toml'), 'utf8'));
  const want = {};
  CFG_COPY.keys.forEach((k) => { want[k.key] = Array.isArray(k.value) ? k.value.join(' ') : String(k.value); });
  assert.deepStrictEqual(tomlLines(t), want);
});

test('nearestInsideAncestor skips region-edge ancestors', () => {
  // tileInsideRegions reads minZoom/maxZoom (app_pure.js:214-221). "g" spans the whole Web
  // Mercator square (|lat| <= 85.0511), so every z 0..2 tile is inside it.
  const regions = [{ name: 'g', bbox: [-180, -85.06, 180, 85.06], minZoom: 0, maxZoom: 2 },
                   { name: 'spot', bbox: [-78, 38, -76, 40], minZoom: 3, maxZoom: 6 }];
  // 6/18/24 and its z 5..3 ancestors each overlap "spot" without lying inside it.
  for (let n = 0; n <= 3; n++) {
    const p = n ? P.parentTile(6, 18, 24, n) : { z: 6, x: 18, y: 24 };
    const b = P.tileBounds(p.z, p.x, p.y);
    assert.ok(b.west < -76 && b.east > -78 && b.south < 40 && b.north > 38, 'overlaps at z' + p.z);
    assert.strictEqual(P.tileInsideRegions(p.z, p.x, p.y, regions), false, 'edge at z' + p.z);
  }
  // 6/18/24 lies partly outside "spot"; levels 1-3 (z 5..3) are edge tiles, level 4 (z 2) is inside "g".
  assert.strictEqual(P.nearestInsideAncestor(6, 18, 24, 1, 4, regions), 4);
  assert.strictEqual(P.nearestInsideAncestor(6, 18, 24, 1, 3, regions), null);
  // 2/1/1's parent 1/0/0 reaches 85.0511 N, inside "g"'s 85.06 edge (H9).
  assert.strictEqual(P.nearestInsideAncestor(2, 1, 1, 1, 4, regions), 1);
  // Starting further up, and past z 0.
  assert.strictEqual(P.nearestInsideAncestor(6, 18, 24, 5, 9, regions), 5);
  assert.strictEqual(P.nearestInsideAncestor(1, 0, 0, 2, 4, regions), null);
});
test('underlayNextLevel stays on ancestors inside a region', () => {
  // 6/18/24: its z 4 ancestor 4/4/6 is exactly region "r4" (inside), z 3 is in no region
  // (an edge level), z 2 is inside "g". The underlay starts at level 2 (z 4); when that
  // image fails it must go on to level 4 (z 2), never to the edge level 3.
  const b4 = P.tileBounds(4, 4, 6);
  const r4 = { name: 'r4', bbox: [b4.west, b4.south, b4.east, b4.north], minZoom: 4, maxZoom: 4 };
  const g = { name: 'g', bbox: [-180, -85.06, 180, 85.06], minZoom: 0, maxZoom: 2 };
  const regions = [g, r4];
  assert.deepStrictEqual(P.parentTile(6, 18, 24, 2), Object.assign({}, P.parentTile(6, 18, 24, 2), { z: 4, x: 4, y: 6 }));
  assert.strictEqual(P.tileInsideRegions(4, 4, 6, regions), true);
  assert.strictEqual(P.tileInsideRegions(3, 2, 3, regions), false);
  assert.strictEqual(P.nearestInsideAncestor(6, 18, 24, 1, 4, regions), 2);
  assert.strictEqual(P.underlayNextLevel(6, 18, 24, 2, 4, regions), 4);
  // Nothing inside further up: no next level, not an edge level.
  assert.strictEqual(P.underlayNextLevel(6, 18, 24, 2, 4, [r4]), null);
  assert.strictEqual(P.underlayNextLevel(6, 18, 24, 2, 3, regions), null);
  assert.strictEqual(P.underlayNextLevel(6, 18, 24, 4, 9, regions), 5);
});
test('closeReportCard forgets the shown report', () => {
  const fisb = { shapeKey: 'k', reportRec: 'metar:KDCA', showKey: 'metar:KDCA', symDirty: false };
  P.closeReportCard(fisb);
  assert.strictEqual(fisb.shapeKey, null);
  assert.strictEqual(fisb.reportRec, null);
  assert.strictEqual(fisb.showKey, null);
  assert.strictEqual(fisb.symDirty, true);   // the symbol cap is recomputed without it
  const idle = { shapeKey: null, reportRec: null, showKey: null, symDirty: false };
  P.closeReportCard(idle);
  assert.strictEqual(idle.symDirty, false);  // nothing was kept: no redraw
});
test('capSymbols keeps the shown report', () => {
  const s = [];
  for (let i = 0; i < 400; i++) s.push({ key: 'metar:' + i, rank: 0, lat: 38 + i / 100, lon: -77 });
  s.push({ key: 'winds:FAR:06', rank: 3, lat: 45, lon: -100 });
  const out = P.capSymbols(s, { lat: 38, lng: -77 }, 300, 'winds:FAR:06');
  assert.strictEqual(out.length, 300);
  assert.strictEqual(out[0].key, 'winds:FAR:06');
  assert.strictEqual(out[1].key, 'metar:0');
  assert.strictEqual(P.capSymbols(s, { lat: 38, lng: -77 }, 300, null).some((x) => x.rank === 3), false);
  // Under the cap nothing is dropped; the input is not changed.
  assert.strictEqual(P.capSymbols(s.slice(0, 10), { lat: 38, lng: -77 }, 300, 'metar:9')[0].key, 'metar:9');
  assert.strictEqual(P.capSymbols(s.slice(0, 10), { lat: 38, lng: -77 }, 300, null).length, 10);
  assert.strictEqual(s.length, 401);
});

test('icaoCountry: allocation blocks and edges', () => {
  assert.strictEqual(P.icaoCountry('a4f2c1'), 'United States');
  assert.strictEqual(P.icaoCountry('A00000'), 'United States');
  assert.strictEqual(P.icaoCountry('afffff'), 'United States');
  assert.strictEqual(P.icaoCountry('4ca61c'), 'Ireland');
  assert.strictEqual(P.icaoCountry('406a0b'), 'United Kingdom');
  assert.strictEqual(P.icaoCountry('3c6444'), 'Germany');
  assert.strictEqual(P.icaoCountry('484b6e'), 'Netherlands');
  assert.strictEqual(P.icaoCountry('c01234'), 'Canada');
  assert.strictEqual(P.icaoCountry('7c6b2a'), 'Australia');
  assert.strictEqual(P.icaoCountry('~2c4a1f'), null);   // TIS-B track-file id, not an ICAO address
  assert.strictEqual(P.icaoCountry('~a4f2c1'), null);   // even when the digits would be US
  assert.strictEqual(P.icaoMilitary('~ae1234'), false);
  assert.strictEqual(P.icaoCountry('zz'), null);
  assert.strictEqual(P.icaoCountry(undefined), null);
  assert.ok(P.ICAO_COUNTRIES.indexOf('United States') >= 0 && P.ICAO_COUNTRIES.length > 150);
  for (let i = 1; i < P.ICAO_COUNTRIES.length; i++) assert.ok(P.ICAO_COUNTRIES[i - 1] < P.ICAO_COUNTRIES[i], 'sorted');
});
test('icaoMilitary: US and UK military blocks and their neighbours', () => {
  assert.strictEqual(P.icaoMilitary('adf7c8'), true);
  assert.strictEqual(P.icaoMilitary('ae1234'), true);
  assert.strictEqual(P.icaoMilitary('afffff'), true);
  assert.strictEqual(P.icaoMilitary('adf7c7'), false);
  assert.strictEqual(P.icaoMilitary('43c000'), true);
  assert.strictEqual(P.icaoMilitary('43cfff'), true);
  assert.strictEqual(P.icaoMilitary('43d000'), false);
  assert.strictEqual(P.icaoMilitary('a4f2c1'), false);
  assert.strictEqual(P.icaoMilitary(null), false);
});
test('callsignKind', () => {
  assert.strictEqual(P.callsignKind('RYR9085'), 'airline');
  assert.strictEqual(P.callsignKind('BAW99'), 'airline');
  assert.strictEqual(P.callsignKind('UAL1423 '), 'airline');
  assert.strictEqual(P.callsignKind('CTN82R'), 'airline');
  assert.strictEqual(P.callsignKind('N512RG'), 'registration');
  assert.strictEqual(P.callsignKind('N3157K'), 'registration');
  assert.strictEqual(P.callsignKind('GABCD'), 'registration');
  assert.strictEqual(P.callsignKind('PHBXA'), 'registration');
  assert.strictEqual(P.callsignKind('RCH4521'), 'military');
  assert.strictEqual(P.callsignKind('EVAC123'), 'military');
  assert.strictEqual(P.callsignKind('RRR1234'), 'military');
  assert.strictEqual(P.callsignKind('12345'), 'other');
  assert.strictEqual(P.callsignKind(''), null);
  assert.strictEqual(P.callsignKind(undefined), null);
});
test('trackClass', () => {
  assert.strictEqual(P.trackClass({ hex: 'ae1234', callsign: 'ABC123', category: 'A3' }), 'military');
  assert.strictEqual(P.trackClass({ hex: 'a4f2c1', callsign: 'RCH123', category: 'A5' }), 'military');
  assert.strictEqual(P.trackClass({ hex: 'a4f2c1', callsign: 'UAL1423', category: 'A3' }), 'airline');
  assert.strictEqual(P.trackClass({ hex: 'a4f2c1', callsign: 'UAL1423' }), 'airline');
  assert.strictEqual(P.trackClass({ hex: 'a4f2c1', callsign: 'XYZ', category: 'A5' }), 'airline'); // heavy with a callsign
  assert.strictEqual(P.trackClass({ hex: 'a5b06d', callsign: 'N512RG', category: 'A1' }), 'private');
  assert.strictEqual(P.trackClass({ hex: 'a5b06d', category: 'A1' }), 'private');
  assert.strictEqual(P.trackClass({ hex: 'a91c42', callsign: 'N72HX', category: 'A7' }), 'private');
  assert.strictEqual(P.trackClass({ hex: 'a5b06d', registration: 'N512RG' }), 'private');
  assert.strictEqual(P.trackClass({ hex: 'a4f2c1', category: 'A3' }), 'unknown');   // large, no callsign
  assert.strictEqual(P.trackClass({ hex: '4b0de1' }), 'unknown');
});
test('isEmergency', () => {
  assert.strictEqual(P.isEmergency({ squawk: '7700' }), true);
  assert.strictEqual(P.isEmergency({ squawk: '7600', emergency: 'none' }), true);
  assert.strictEqual(P.isEmergency({ squawk: '7500' }), true);
  assert.strictEqual(P.isEmergency({ squawk: '1200', emergency: 'general' }), true);
  assert.strictEqual(P.isEmergency({ squawk: '1200', emergency: 'none' }), false);
  assert.strictEqual(P.isEmergency({}), false);
});
test('address tables: sorted, each first <= last, no overlaps', () => {
  [P.ICAO_BLOCKS, P.MILITARY_BLOCKS].forEach((tbl) => {
    assert.ok(tbl.length > 0);
    for (let i = 0; i < tbl.length; i++) {
      const b = tbl[i];
      assert.ok(b[0] >= 0 && b[1] <= 0xffffff && b[0] <= b[1], 'block ' + i + ' first <= last');
      if (i > 0) assert.ok(tbl[i - 1][1] < b[0], 'block ' + i + ' after block ' + (i - 1));
    }
  });
  // Every military block lies inside a national block.
  P.MILITARY_BLOCKS.forEach((m) => {
    const hex = m[0].toString(16).padStart(6, '0');
    assert.ok(P.icaoCountry(hex) !== null, 'military block ' + hex + ' inside a national block');
  });
});

test('normalizeView: defaults, partial, malformed', () => {
  assert.deepStrictEqual(P.normalizeView(undefined), P.VIEW_DEFAULTS);
  assert.deepStrictEqual(P.normalizeView('{'), P.VIEW_DEFAULTS);
  assert.deepStrictEqual(P.normalizeView(42), P.VIEW_DEFAULTS);
  const v = P.normalizeView({ search: 'ual', altMin: '3000', altMax: 'x', decisions: ['sent', 'bogus'], classes: 7, mapMode: 'DIM', category: 'A3', country: 'Ireland', emergency: 1 });
  assert.strictEqual(v.search, 'ual'); assert.strictEqual(v.altMin, 3000); assert.strictEqual(v.altMax, null);
  assert.deepStrictEqual(v.decisions, ['sent']); assert.deepStrictEqual(v.classes, P.VIEW_DEFAULTS.classes);
  assert.strictEqual(v.mapMode, 'hide'); assert.strictEqual(v.category, 'A3'); assert.strictEqual(v.country, 'Ireland');
  assert.strictEqual(v.emergency, true);
  assert.strictEqual(P.normalizeView({ mapMode: 'dim' }).mapMode, 'dim');
  assert.strictEqual(P.normalizeView(JSON.stringify(v)).altMin, 3000, 'accepts a JSON string');
});
test('normalizeView: the Trails group (show)', () => {
  assert.deepStrictEqual(P.TRAIL_SHOWS, ['all', 'selected', 'off']);
  assert.strictEqual(P.VIEW_DEFAULTS.trailShow, 'all');
  ['all', 'selected', 'off'].forEach((s) => assert.strictEqual(P.normalizeView({ trailShow: s }).trailShow, s));
  ['ALL', 'none', '', 1, null, ['off']].forEach((s) => assert.strictEqual(P.normalizeView({ trailShow: s }).trailShow, 'all', String(s)));
  assert.strictEqual(P.viewTrailsDefault(P.normalizeView(null)), true);
  assert.strictEqual(P.viewTrailsDefault(P.normalizeView({ trailShow: 'off' })), false);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ trailShow: 'off' })), 0, 'not a filter: not in the badge');
});
test('view sets round-trip the Trails group', () => {
  const v = P.normalizeView({ search: 'ual', trailShow: 'selected' });
  const r = P.putViewSet(P.normalizeViewSets(null), 'T', v, false);
  const back = P.normalizeViewSets(JSON.parse(JSON.stringify(r.store))).sets[0];
  assert.deepStrictEqual(back.view, v);
  assert.strictEqual(P.viewSetMatches(back, v, false), true);
  assert.strictEqual(P.viewSetMatches(back, P.normalizeView({ search: 'ual' }), false), false, 'a different Show is a different state');
  // A set saved before the group existed applies the defaults.
  assert.strictEqual(P.normalizeViewSets({ sets: [{ name: 'old', view: { search: 'ual' } }] }).sets[0].view.trailShow, 'all');
});
test('normalizeView: copies arrays, never hands out the defaults', () => {
  assert.ok(Object.isFrozen(P.VIEW_DEFAULTS));
  const v = P.normalizeView(null);
  v.decisions.push('bogus'); v.classes.length = 0;
  assert.strictEqual(P.VIEW_DEFAULTS.decisions.length, 5);
  assert.deepStrictEqual(P.VIEW_DEFAULTS.classes, P.CLASSES);
  assert.deepStrictEqual(P.VIEW_DEFAULTS.decisions, ['sent', 'allow_listed', 'out_of_range', 'below_elevation', 'blocked']);
});
test('viewActiveCount', () => {
  assert.strictEqual(P.viewActiveCount(P.VIEW_DEFAULTS), 0);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ mapMode: 'dim' })), 0);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ search: 'a', altMin: 1, altMax: 2, ground: false, emergency: true })), 3);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ decisions: ['sent'], classes: ['airline'], category: 'A1', country: 'Canada', gsMax: 100, rangeMin: 5 })), 6);
});
test('normalizeViewSets: field by field and caps', () => {
  assert.strictEqual(P.VIEW_SETS_MAX, 20);
  assert.strictEqual(P.VIEW_SET_NAME_MAX, 40);
  assert.deepStrictEqual(P.normalizeViewSets('x'), { version: 2, sets: [] });
  assert.deepStrictEqual(P.normalizeViewSets(null), { version: 2, sets: [] });
  assert.deepStrictEqual(P.normalizeViewSets({ sets: 'nope' }), { version: 2, sets: [] });
  const s = P.normalizeViewSets({ version: 1, sets: [
    { name: ' A ', view: { search: 'ual', mapMode: 'bogus' }, hideFiltered: 1 },
    { name: 'A', view: {} }, { name: '', view: {} }, { name: 7 },
    { name: 'L'.repeat(50), view: null, hideFiltered: 'yes' }
  ] });
  assert.strictEqual(s.version, 2);
  assert.deepStrictEqual(s.sets.map((x) => x.name), ['A', 'L'.repeat(40)]);
  assert.deepStrictEqual(s.sets[0].view, P.normalizeView({ search: 'ual' }));
  assert.strictEqual(s.sets[0].hideFiltered, true);
  assert.deepStrictEqual(s.sets[1].view, P.normalizeView(null));
  assert.strictEqual(s.sets[1].hideFiltered, false);
  assert.strictEqual(P.normalizeViewSets(JSON.stringify(s)).sets[1].name, 'L'.repeat(40), 'accepts a JSON string');
  const many = [];
  for (let i = 0; i < 500; i++) many.push({ name: 'S' + i, view: {}, hideFiltered: false });
  const capped = P.normalizeViewSets({ sets: many });
  assert.strictEqual(capped.sets.length, 20);
  assert.strictEqual(capped.sets[19].name, 'S19');
});
test('viewSetMatches', () => {
  const set = P.normalizeViewSets({ sets: [{ name: 'A', view: { search: 'ual' }, hideFiltered: true }] }).sets[0];
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'ual' }), true), true);
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'dal' }), true), false);
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'ual' }), false), false);
});
test('mapBoxName and mapBoxToggle', () => {
  assert.deepStrictEqual(P.MAP_BOXES, ['map', 'aircraft', 'layers', 'weather']);
  assert.strictEqual(P.mapBoxName('layers'), 'layers');
  assert.strictEqual(P.mapBoxName('Layers'), null);
  assert.strictEqual(P.mapBoxName(''), null);
  assert.strictEqual(P.mapBoxName(null), null);
  assert.strictEqual(P.mapBoxName(7), null);
  assert.strictEqual(P.mapBoxToggle(null, 'map'), 'map');
  assert.strictEqual(P.mapBoxToggle('map', 'map'), null);
  assert.strictEqual(P.mapBoxToggle('map', 'weather'), 'weather');
  assert.strictEqual(P.mapBoxToggle('map', 'bogus'), null);
});
test('normalizeLayers: defaults, strings and garbage', () => {
  assert.deepStrictEqual(P.LAYER_IDS, ['regional', 'conus', 'rasters', 'tfr', 'sigmet', 'airmet', 'cwa', 'sua']);
  assert.deepStrictEqual(P.normalizeLayers(null), P.LAYER_DEFAULTS);
  assert.deepStrictEqual(P.normalizeLayers('not json'), P.LAYER_DEFAULTS);
  assert.deepStrictEqual(P.normalizeLayers([]), P.LAYER_DEFAULTS);
  const n = P.normalizeLayers('{"regional":false,"tfr":"yes","bogus":true}');
  assert.deepStrictEqual(n, { regional: false, conus: true, rasters: false, tfr: true, sigmet: false, airmet: false, cwa: false, sua: false });
  assert.strictEqual(Object.keys(n).length, 8);
});
test('normalizeLayersPartial: garbage', () => {
  assert.deepStrictEqual(P.normalizeLayersPartial(undefined), {});
  assert.deepStrictEqual(P.normalizeLayersPartial('x'), {});
  assert.deepStrictEqual(P.normalizeLayersPartial([]), {});
  assert.deepStrictEqual(P.normalizeLayersPartial({ regional: 'yes', bogus: true, sua: 0 }), { regional: true, sua: false });
});
test('layersOnCount', () => {
  assert.strictEqual(P.layersOnCount(null), 2);
  assert.strictEqual(P.layersOnCount({ regional: false, conus: false }), 0);
  assert.strictEqual(P.layersOnCount({ tfr: true, sua: true }), 4);
});
test('weatherDot: faded, missing, no fisb', () => {
  const ok = { on: true, drawn: true, faded: false };
  assert.strictEqual(P.weatherDot([ok, ok], true), false);
  assert.strictEqual(P.weatherDot([ok, { on: true, drawn: true, faded: true }], true), true);
  assert.strictEqual(P.weatherDot([{ on: false, drawn: true, faded: true }], true), false);
  assert.strictEqual(P.weatherDot([{ on: true, drawn: false, faded: false }], true), true);
  assert.strictEqual(P.weatherDot([{ on: true, drawn: false, faded: false }], false), false);
  assert.strictEqual(P.weatherDot([], true), false);
});
test('normalizeMapView: partial, valid fields only', () => {
  assert.strictEqual(P.normalizeMapView(undefined), undefined);
  assert.strictEqual(P.normalizeMapView('x'), undefined);
  assert.strictEqual(P.normalizeMapView([]), undefined);
  assert.deepStrictEqual(P.normalizeMapView({}), {});
  assert.deepStrictEqual(P.normalizeMapView({ style: 'dark', borders: 1, cities: 0, radarOpacity: 0.4, layers: { tfr: 1, bogus: 1 }, junk: 1 }),
    { style: 'dark', borders: true, cities: false, layers: { tfr: true }, radarOpacity: 0.4 });
  assert.deepStrictEqual(P.normalizeMapView({ style: 'sepia', radarOpacity: 5, layers: 'x' }), {});
  assert.deepStrictEqual(P.normalizeMapView({ radarOpacity: '0.5' }), {});
});
test('normalizeViewSets: version 2, old sets keep no map', () => {
  assert.strictEqual(P.VIEW_SETS_VERSION, 2);
  const s = P.normalizeViewSets({ version: 1, sets: [{ name: 'Old', view: {}, hideFiltered: false }, { name: 'New', view: {}, hideFiltered: true, map: { style: 'dark', layers: { sua: true } } }] });
  assert.strictEqual(s.version, 2);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(s.sets[0], 'map'), false);
  assert.deepStrictEqual(s.sets[1].map, { style: 'dark', layers: { sua: true } });
  assert.deepStrictEqual(P.normalizeViewSets({ sets: [{ name: 'E', view: {}, map: {} }] }).sets[0].map, {});
});
test('mapViewMatches and viewSetMatches with a map', () => {
  const cur = { style: 'light', borders: false, cities: false, layers: P.LAYER_DEFAULTS, radarOpacity: 0.7 };
  assert.strictEqual(P.mapViewMatches(undefined, cur), true);
  assert.strictEqual(P.mapViewMatches({}, cur), true);
  assert.strictEqual(P.mapViewMatches({ style: 'light', layers: { conus: true } }, cur), true);
  assert.strictEqual(P.mapViewMatches({ style: 'dark' }, cur), false);
  assert.strictEqual(P.mapViewMatches({ layers: { conus: false } }, cur), false);
  assert.strictEqual(P.mapViewMatches({ radarOpacity: 0.4 }, cur), false);
  assert.strictEqual(P.mapViewMatches({ style: 'dark' }, { layers: P.LAYER_DEFAULTS, radarOpacity: 0.7 }), true, 'no tiles: style cannot be honoured, so it matches');
  assert.strictEqual(P.mapViewMatches({ style: 'dark', layers: { tfr: true }, radarOpacity: 0.3 }, undefined), true, 'no current map view');
  assert.strictEqual(P.mapViewMatches({ style: 'dark' }, null), true);
  assert.strictEqual(P.mapViewMatches({ style: 'dark' }, 'x'), true);
  const set = P.normalizeViewSets({ sets: [{ name: 'A', view: { search: 'ual' }, hideFiltered: false, map: { style: 'dark' } }] }).sets[0];
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'ual' }), false, cur), false);
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'ual' }), false, Object.assign({}, cur, { style: 'dark' })), true);
  assert.strictEqual(P.viewSetMatches(set, P.normalizeView({ search: 'ual' }), false), true, 'old call shape');
  const old = P.normalizeViewSets({ sets: [{ name: 'B', view: { search: 'ual' } }] }).sets[0];
  assert.strictEqual(P.viewSetMatches(old, P.normalizeView({ search: 'ual' }), false, cur), true);
});
test('putViewSet records the map view', () => {
  const cur = { style: 'light', borders: true, layers: P.LAYER_DEFAULTS, radarOpacity: 0.7 };
  const r = P.putViewSet({ version: 2, sets: [] }, 'A', {}, false, cur);
  assert.strictEqual(r.store.version, 2);
  assert.deepStrictEqual(r.store.sets[0].map, { style: 'light', borders: true, layers: P.LAYER_DEFAULTS, radarOpacity: 0.7 });
  const r2 = P.putViewSet({ version: 2, sets: [] }, 'B', {}, false);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(r2.store.sets[0], 'map'), false);
});
test('putViewSet: add, replace, cap', () => {
  let r = P.putViewSet(P.normalizeViewSets(null), 'A', P.normalizeView({ search: 'ual' }), false);
  assert.strictEqual(r.replaced, false);
  assert.strictEqual(r.store.sets.length, 1);
  assert.strictEqual(r.store.sets[0].name, 'A');
  r = P.putViewSet(r.store, 'B', P.normalizeView(null), true);
  assert.strictEqual(r.replaced, false);
  assert.deepStrictEqual(r.store.sets.map((x) => x.name), ['A', 'B']);
  r = P.putViewSet(r.store, 'A', P.normalizeView({ emergency: true }), true);
  assert.strictEqual(r.replaced, true);
  assert.deepStrictEqual(r.store.sets.map((x) => x.name), ['A', 'B'], 'same index');
  assert.strictEqual(r.store.sets[0].view.emergency, true);
  assert.strictEqual(r.store.sets[0].view.search, '');
  assert.strictEqual(r.store.sets[0].hideFiltered, true);
  const full = { version: 1, sets: [] };
  for (let i = 0; i < 20; i++) full.sets.push({ name: 'S' + i, view: P.normalizeView(null), hideFiltered: false });
  assert.deepStrictEqual(P.putViewSet(full, 'New', P.normalizeView(null), false), { error: 'At most 20 views: delete one first.' });
  assert.strictEqual(P.putViewSet(full, 'S3', P.normalizeView(null), true).replaced, true, 'replacing at the cap is allowed');
  assert.strictEqual(full.sets.length, 20);
});
test('utcDate: the UTC calendar date of a time', () => {
  assert.strictEqual(P.utcDate(Date.parse('2026-11-04T23:59:59Z')), '2026-11-04');
  assert.strictEqual(P.utcDate(Date.parse('2026-11-05T00:00:00Z')), '2026-11-05');
  assert.strictEqual(P.utcDate(Date.parse('2026-01-01T20:04:05-08:00')), '2026-01-02');
});
test('trackAlt', () => {
  assert.strictEqual(P.trackAlt({ on_ground: true, alt_baro_ft: 500 }), 0);
  assert.strictEqual(P.trackAlt({ alt_baro_ft: 500, alt_geom_ft: 600 }), 500);
  assert.strictEqual(P.trackAlt({ alt_geom_ft: 600 }), 600);
  assert.strictEqual(P.trackAlt({}), null);
});
test('searchMatches over hex, callsign, registration, squawk', () => {
  const t = { hex: 'a4f2c1', callsign: 'UAL1423', registration: 'N12345', squawk: '7700' };
  ['', 'A4F2', 'ual', '1423', 'n123', '7700'].forEach((q) => assert.strictEqual(P.searchMatches(t, q), true, q));
  ['zzz', '7701'].forEach((q) => assert.strictEqual(P.searchMatches(t, q), false, q));
  assert.strictEqual(P.searchMatches({ hex: 'abc123' }, 'abc'), true);
});
test('viewMatches: bands, ground, unknown values, min > max', () => {
  const V = (o) => P.normalizeView(o);
  const air = { hex: 'a4f2c1', callsign: 'UAL1423', category: 'A3', alt_baro_ft: 35000, gs_kt: 450, range_nm: 40, decision: 'send' };
  const gnd = { hex: 'a5b06d', callsign: 'N512RG', category: 'A1', on_ground: true, alt_baro_ft: 100, gs_kt: 5, range_nm: 3, decision: 'below_elevation' };
  const nogs = { hex: '4b0de1', alt_baro_ft: 1000, range_nm: 10, decision: 'out_of_range' };
  assert.strictEqual(P.viewMatches(air, V({})), true);
  assert.strictEqual(P.viewMatches(air, V({ altMin: 30000 })), true);
  assert.strictEqual(P.viewMatches(air, V({ altMax: 30000 })), false);
  assert.strictEqual(P.viewMatches(gnd, V({ altMin: 0 })), true);
  assert.strictEqual(P.viewMatches(gnd, V({ ground: false })), false);
  assert.strictEqual(P.viewMatches(gnd, V({ altMin: 0, altMax: 100, ground: false })), false, 'ground off beats the band');
  assert.strictEqual(P.viewMatches(nogs, V({ gsMin: 0 })), false, 'unknown value fails an active band');
  assert.strictEqual(P.viewMatches(nogs, V({ altMin: 500 })), true);
  assert.strictEqual(P.viewMatches(air, V({ gsMin: 500, gsMax: 100 })), false, 'min > max matches nothing');
  assert.strictEqual(P.viewMatches(air, V({ rangeMin: 10, rangeMax: 50 })), true);
  assert.strictEqual(P.viewMatches(air, V({ rangeMax: 39.9 })), false);
});
test('viewMatches: decisions, classes, emergency, category, country, search', () => {
  const V = (o) => P.normalizeView(o);
  const air = { hex: 'a4f2c1', callsign: 'UAL1423', category: 'A3', alt_baro_ft: 35000, gs_kt: 450, range_nm: 40, decision: 'send', squawk: '3341' };
  const nopos = { hex: 'a0d4b7', callsign: 'SWA3310', decision: 'no_position' };
  const mil = { hex: 'ae1234', squawk: '7700', decision: 'blocked', category: 'A3' };
  assert.strictEqual(P.viewMatches(air, V({ decisions: ['sent'] })), true);
  assert.strictEqual(P.viewMatches(air, V({ decisions: ['blocked'] })), false);
  assert.strictEqual(P.viewMatches(nopos, V({ decisions: ['blocked'] })), true, 'no_position ignores decision toggles');
  assert.strictEqual(P.viewMatches(air, V({ classes: ['airline'] })), true);
  assert.strictEqual(P.viewMatches(air, V({ classes: ['military'] })), false);
  assert.strictEqual(P.viewMatches(mil, V({ classes: ['military'] })), true);
  assert.strictEqual(P.viewMatches(air, V({ emergency: true })), false);
  assert.strictEqual(P.viewMatches(mil, V({ emergency: true })), true);
  assert.strictEqual(P.viewMatches(air, V({ category: 'A3' })), true);
  assert.strictEqual(P.viewMatches(air, V({ category: 'A1' })), false);
  assert.strictEqual(P.viewMatches(nopos, V({ category: 'none' })), true);
  assert.strictEqual(P.viewMatches({ hex: 'a4f2c1', category: 'B2', decision: 'send' }, V({ category: 'other' })), true);
  assert.strictEqual(P.viewMatches(air, V({ country: 'United States' })), true);
  assert.strictEqual(P.viewMatches(air, V({ country: 'Ireland' })), false);
  assert.strictEqual(P.viewMatches({ hex: '~2c4a1f', decision: 'send' }, V({ country: 'other' })), true);
  assert.strictEqual(P.viewMatches(air, V({ search: '3341' })), true);
  assert.strictEqual(P.viewMatches(air, V({ search: 'ryr', decisions: ['sent'] })), false, 'every rule must pass');
});
test('trackClass: the address-based military check only for ICAO addresses', () => {
  assert.strictEqual(P.trackClass({ hex: 'ae1234', addr_type: 'tisb_trackfile' }), 'unknown');
  assert.strictEqual(P.trackClass({ hex: 'ae1234', addr_type: 'icao' }), 'military');
  assert.strictEqual(P.trackClass({ hex: 'ae1234' }), 'military', 'no addr_type: taken as ICAO');
  assert.strictEqual(P.trackClass({ hex: 'ae1234', addr_type: 'non_icao', callsign: 'RCH123' }), 'military', 'the callsign still counts');
});
test('viewMatches country: a non-ICAO address has no country', () => {
  const V = (o) => P.normalizeView(o);
  const tisb = { hex: 'ae1234', addr_type: 'tisb_trackfile', decision: 'send' };
  const icao = { hex: 'ae1234', addr_type: 'icao', decision: 'send' };
  assert.strictEqual(P.trackCountry(tisb), null);
  assert.strictEqual(P.trackCountry(icao), 'United States');
  assert.strictEqual(P.viewMatches(tisb, V({ country: 'United States' })), false);
  assert.strictEqual(P.viewMatches(tisb, V({ country: 'other' })), true);
  assert.strictEqual(P.viewMatches(icao, V({ country: 'United States' })), true);
  assert.strictEqual(P.viewMatches(icao, V({ country: 'other' })), false);
  assert.strictEqual(P.viewMatches(tisb, V({ classes: ['military'] })), false);
  assert.strictEqual(P.viewMatches(icao, V({ classes: ['military'] })), true);
});

// The same cases tests/test_icao_class.cpp runs through the daemon's copy of
// these rules (skycued/src/icao_class.cpp): an absent want is null.
test('class cases fixture', () => {
  const C = require(path.join(__dirname, '..', 'fixtures', 'icao_class_cases.json'));
  const want = (c) => (c.want === undefined ? null : c.want);
  const name = (c) => JSON.stringify(c);
  ['country', 'track_country', 'military', 'callsign_kind', 'track_class'].forEach((g) => {
    assert.ok(Array.isArray(C[g]) && C[g].length > 0, 'group ' + g);
  });
  C.country.forEach((c) => assert.strictEqual(P.icaoCountry(c.hex), want(c), name(c)));
  C.track_country.forEach((c) => assert.strictEqual(P.trackCountry(c), want(c), name(c)));
  C.military.forEach((c) => assert.strictEqual(P.icaoMilitary(c.hex), want(c), name(c)));
  C.callsign_kind.forEach((c) => assert.strictEqual(P.callsignKind(c.callsign), want(c), name(c)));
  C.track_class.forEach((c) => assert.strictEqual(P.trackClass(c), want(c), name(c)));
});

// Signal-coloured trail: the fixed scale and the run builder.
test('signalColor: bounds, clamping, steps weak to strong', () => {
  const C = P.SIGNAL_COLORS;
  assert.ok(C.length >= 5 && C.length <= 7, 'steps ' + C.length);
  assert.strictEqual(P.signalColor(P.SIGNAL_MIN_DB), C[0]);
  assert.strictEqual(P.signalColor(-49.4), C[0]);
  assert.strictEqual(P.signalColor(P.SIGNAL_MAX_DB), C[C.length - 1]);
  assert.strictEqual(P.signalColor(0), C[C.length - 1]);
  assert.strictEqual(P.signalColor(P.SIGNAL_MAX_DB - 0.01), C[C.length - 1]);
  const step = (P.SIGNAL_MAX_DB - P.SIGNAL_MIN_DB) / C.length;
  for (let i = 0; i < C.length; i++) {
    assert.strictEqual(P.signalStep(P.SIGNAL_MIN_DB + i * step), i, 'lower edge of step ' + i);
    assert.strictEqual(P.signalStep(P.SIGNAL_MIN_DB + (i + 0.5) * step), i, 'middle of step ' + i);
  }
  assert.strictEqual(new Set(C).size, C.length, 'distinct colours');
});
test('signalColor: no signal is null', () => {
  [undefined, null, NaN, Infinity, '-20', {}].forEach((v) => {
    assert.strictEqual(P.signalColor(v), null, String(v));
    assert.strictEqual(P.signalStep(v), -1, String(v));
  });
});
test('signalLevel: readsb\'s -49.5 and lower are no signal', () => {
  assert.strictEqual(P.SIGNAL_NONE_DB, -49.5);
  [-49.5, -50, -80].forEach((v) => {
    assert.strictEqual(P.signalLevel(v), null, String(v));
    assert.strictEqual(P.signalStep(v), -1, String(v));
    assert.strictEqual(P.signalColor(v), null, String(v));
  });
  [undefined, null, NaN, '-20'].forEach((v) => assert.strictEqual(P.signalLevel(v), null, String(v)));
  assert.strictEqual(P.signalLevel(-49.4), -49.4);
  assert.strictEqual(P.signalColor(-49.4), P.SIGNAL_COLORS[0]);
  assert.strictEqual(P.signalLevel(-20), -20);
  // The trail builder: a -49.5 trail is one run without a colour.
  assert.deepStrictEqual(P.signalRuns([{ lat: 0, lon: 0, sig: -49.5 }, { lat: 1, lon: 1, sig: -49.5 }, { lat: 2, lon: 2, sig: -49.5 }]),
    [{ points: [[0, 0], [1, 1], [2, 2]], color: null }]);
});
test('signalLegend covers the scale contiguously', () => {
  const L = P.signalLegend();
  assert.strictEqual(L.length, P.SIGNAL_COLORS.length);
  assert.strictEqual(L[0].from, P.SIGNAL_MIN_DB);
  assert.strictEqual(L[L.length - 1].to, P.SIGNAL_MAX_DB);
  for (let i = 1; i < L.length; i++) assert.strictEqual(L[i].from, L[i - 1].to);
  L.forEach((s, i) => assert.strictEqual(s.color, P.SIGNAL_COLORS[i]));
});
test('signalRuns: empty, single point, missing signal', () => {
  assert.deepStrictEqual(P.signalRuns([]), []);
  assert.deepStrictEqual(P.signalRuns(null), []);
  assert.deepStrictEqual(P.signalRuns([{ lat: 1, lon: 2, sig: -20 }]),
    [{ points: [[1, 2]], color: P.signalColor(-20) }]);
  assert.deepStrictEqual(P.signalRuns([{ lat: 1, lon: 2 }]), [{ points: [[1, 2]], color: null }]);
  assert.deepStrictEqual(P.signalRuns([{ lat: 0, lon: 0 }, { lat: 1, lon: 1, sig: null }, { lat: 2, lon: 2 }]),
    [{ points: [[0, 0], [1, 1], [2, 2]], color: null }]);
});
test('signalRuns: runs coalesce and share their boundary points', () => {
  const weak = P.signalColor(-34), strong = P.signalColor(-6);
  const pts = [
    { lat: 0, lon: 0, sig: -6 },    // the first point only starts the line
    { lat: 0, lon: 1, sig: -34 }, { lat: 0, lon: 2, sig: -33 },
    { lat: 0, lon: 3, sig: -6 }, { lat: 0, lon: 4, sig: -5 },
    { lat: 0, lon: 5 },
    { lat: 0, lon: 6, sig: -34 }
  ];
  const runs = P.signalRuns(pts);
  assert.deepStrictEqual(runs, [
    { points: [[0, 0], [0, 1], [0, 2]], color: weak },
    { points: [[0, 2], [0, 3], [0, 4]], color: strong },
    { points: [[0, 4], [0, 5]], color: null },
    { points: [[0, 5], [0, 6]], color: weak }
  ]);
  // Every segment is drawn exactly once.
  assert.strictEqual(runs.reduce((n, r) => n + r.points.length - 1, 0), pts.length - 1);
});

test('signalRunsAppend / signalRunsTrim: extend, new run, trim inside a run and across runs', () => {
  const p = (lon, sig) => ({ lat: 0, lon, sig });
  const runs = [];
  assert.strictEqual(P.signalRunsAppend(runs, p(0, -6), p(1, -34)), false);
  assert.strictEqual(P.signalRunsAppend(runs, p(1, -34), p(2, -33)), true);
  assert.strictEqual(P.signalRunsAppend(runs, p(2, -33), p(3, -6)), false);
  assert.strictEqual(P.signalRunsAppend(runs, p(3, -6), p(4)), false);
  assert.deepStrictEqual(runs, P.signalRuns([p(0, -6), p(1, -34), p(2, -33), p(3, -6), p(4)]));
  // Point 0 goes: the first run loses its front.
  assert.deepStrictEqual(P.signalRunsTrim(runs, 1), { dropped: 0, trimmed: true });
  assert.deepStrictEqual(runs, P.signalRuns([p(1, -34), p(2, -33), p(3, -6), p(4)]));
  // Points 1 and 2 go: the weak run and the strong one (it ended on point 3) go whole.
  assert.deepStrictEqual(P.signalRunsTrim(runs, 2), { dropped: 2, trimmed: false });
  assert.deepStrictEqual(runs, P.signalRuns([p(3, -6), p(4)]));
  assert.deepStrictEqual(P.signalRunsTrim(runs, 0), { dropped: 0, trimmed: false });
  assert.deepStrictEqual(P.signalRunsTrim(runs, 5), { dropped: 1, trimmed: false });
  assert.deepStrictEqual(runs, []);
});
test('signalRunsAppend / signalRunsTrim equal signalRuns over a random walk', () => {
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const pts = [];
  let runs = null, lon = 0;
  for (let step = 0; step < 2000; step++) {
    if (rnd() < 0.7 || pts.length < 2) {
      const pt = { lat: 1, lon: lon++, sig: rnd() < 0.1 ? null : -40 + rnd() * 40 };
      pts.push(pt);
      if (pts.length === 2) runs = P.signalRuns(pts);              // the page builds the first time
      else if (pts.length > 2) P.signalRunsAppend(runs, pts[pts.length - 2], pt);
    } else {
      const n = 1 + Math.floor(rnd() * (pts.length - 2));         // leaves 2 or more points
      pts.splice(0, n);
      P.signalRunsTrim(runs, n);
    }
    if (pts.length >= 2) assert.deepStrictEqual(runs, P.signalRuns(pts), 'step ' + step);
  }
});

// Trails, Colour by altitude: the scale and the generalised run builder.
test('altitudeStep / altitudeColor: 7 steps, bounds belong to the step below, no altitude is null', () => {
  assert.deepStrictEqual(P.ALT_BOUNDS_FT, [1000, 5000, 10000, 20000, 30000, 40000]);
  assert.strictEqual(P.ALT_COLORS.length, 7);
  assert.strictEqual(new Set(P.ALT_COLORS).size, 7, 'distinct colours');
  P.ALT_COLORS.forEach((c) => assert.ok(P.SIGNAL_COLORS.indexOf(c) < 0, c + ' is not a signal colour'));
  const cases = [[-200, 0], [0, 0], [1000, 0], [1001, 1], [5000, 1], [5001, 2], [10000, 2], [15000, 3],
    [20000, 3], [25000, 4], [30000, 4], [35000, 5], [40000, 5], [40001, 6], [60000, 6]];
  cases.forEach(([ft, i]) => {
    assert.strictEqual(P.altitudeStep(ft), i, ft + ' ft');
    assert.strictEqual(P.altitudeColor(ft), P.ALT_COLORS[i], ft + ' ft');
  });
  [undefined, null, NaN, Infinity, '35000', {}].forEach((v) => {
    assert.strictEqual(P.altitudeStep(v), -1, String(v));
    assert.strictEqual(P.altitudeColor(v), null, String(v));
  });
});
test('altitudeLegend: the bounds, contiguous, open at both ends', () => {
  const L = P.altitudeLegend();
  assert.strictEqual(L.length, 7);
  assert.deepStrictEqual(L[0], { color: P.ALT_COLORS[0], from: null, to: 1000 });
  assert.deepStrictEqual(L[6], { color: P.ALT_COLORS[6], from: 40000, to: null });
  for (let i = 1; i < L.length; i++) assert.strictEqual(L[i].from, L[i - 1].to);
  assert.deepStrictEqual(L.slice(1, 6).map((s) => [s.from, s.to]),
    [[1000, 5000], [5000, 10000], [10000, 20000], [20000, 30000], [30000, 40000]]);
});
test('altitudeRuns: a climb in steps, missing altitude, coalescing', () => {
  const C = P.ALT_COLORS;
  const p = (lon, alt) => ({ lat: 0, lon, alt });
  assert.deepStrictEqual(P.altitudeRuns([]), []);
  assert.deepStrictEqual(P.altitudeRuns([p(0, 500)]), [{ points: [[0, 0]], color: C[0] }]);
  const climb = [p(0, 0), p(1, 800), p(2, 1000), p(3, 3000), p(4, 4000), p(5), p(6, 12000), p(7, 12500)];
  assert.deepStrictEqual(P.altitudeRuns(climb), [
    { points: [[0, 0], [0, 1], [0, 2]], color: C[0] },          // ground to 1000 ft: one run
    { points: [[0, 2], [0, 3], [0, 4]], color: C[1] },
    { points: [[0, 4], [0, 5]], color: null },                    // no altitude: the caller draws it dim
    { points: [[0, 5], [0, 6], [0, 7]], color: C[3] }
  ]);
  // The same runs through the generic builder and the colour function the page uses.
  assert.deepStrictEqual(P.trailRuns(climb, P.trailRunColor('altitude')), P.altitudeRuns(climb));
  assert.deepStrictEqual(P.trailRuns(climb.map((x) => ({ lat: x.lat, lon: x.lon, sig: -20 })), P.trailRunColor('signal')),
    [{ points: climb.map((x) => [x.lat, x.lon]), color: P.signalColor(-20) }]);
  assert.strictEqual(P.trailRunColor('decision'), null);
  assert.strictEqual(P.trailRunColor('bogus'), null);
});
test('trailRunsAppend / trailRunsTrim with the altitude colours equal trailRuns over a random walk', () => {
  const colorOf = P.trailRunColor('altitude');
  let seed = 11;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const pts = [];
  let runs = null, lon = 0, alt = 0;
  for (let step = 0; step < 2000; step++) {
    if (rnd() < 0.7 || pts.length < 2) {
      alt = Math.max(0, alt + (rnd() - 0.4) * 6000);
      const pt = { lat: 1, lon: lon++, alt: rnd() < 0.1 ? null : alt };
      pts.push(pt);
      if (pts.length === 2) runs = P.trailRuns(pts, colorOf);
      else if (pts.length > 2) P.trailRunsAppend(runs, pts[pts.length - 2], pt, colorOf);
    } else {
      const n = 1 + Math.floor(rnd() * (pts.length - 2));
      pts.splice(0, n);
      P.trailRunsTrim(runs, n);
    }
    if (pts.length >= 2) assert.deepStrictEqual(runs, P.trailRuns(pts, colorOf), 'step ' + step);
  }
  assert.strictEqual(P.signalRunsTrim, P.trailRunsTrim);
});
test('normalizeView: the Trails group (colour by)', () => {
  assert.deepStrictEqual(P.TRAIL_COLOR_BY, ['decision', 'signal', 'altitude']);
  assert.strictEqual(P.VIEW_DEFAULTS.trailColor, 'decision');
  P.TRAIL_COLOR_BY.forEach((c) => assert.strictEqual(P.normalizeView({ trailColor: c }).trailColor, c));
  ['Signal', 'rssi', '', 0, null, true].forEach((c) => assert.strictEqual(P.normalizeView({ trailColor: c }).trailColor, 'decision', String(c)));
  assert.strictEqual(P.viewTrailsDefault(P.normalizeView({ trailColor: 'altitude' })), false);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ trailColor: 'altitude' })), 0);
  const v = P.normalizeView({ trailShow: 'off', trailColor: 'altitude' });
  const back = P.normalizeViewSets(JSON.stringify(P.putViewSet(P.normalizeViewSets(null), 'A', v, true).store)).sets[0];
  assert.deepStrictEqual(back.view, v);
  assert.strictEqual(P.viewSetMatches(back, P.normalizeView({ trailShow: 'off', trailColor: 'signal' }), true), false);
});

// Trails, Project ahead: the point, the length, the line.
test('projectPoint: 60 nm due north is +1 deg of latitude; east on the equator; the ruler agrees', () => {
  // 60 nm is 1 deg of arc to 0.07 % on rangeNm's sphere (1852 m per nm,
  // 1 deg = 111.195 km): 0.99932 deg.
  const n = P.projectPoint(39, -77, 0, 60);
  between(n[0], 39 + 0.9990, 39 + 0.9996, 'north lat');
  between(n[1], -77.000001, -76.999999, 'north lon');
  const e = P.projectPoint(0, 0, 90, 60);
  between(e[0], -1e-9, 1e-9, 'east lat');
  between(e[1], 0.9990, 0.9996, 'east lon');
  const s = P.projectPoint(39, -77, 180, 30);
  between(s[0], 38.5, 38.501, 'south lat');
  const m = P.rulerMeasure(39.7, -86.3, ...P.projectPoint(39.7, -86.3, 237, 16));
  between(m.nm, 15.999, 16.001, 'distance back');
  between(m.bearing, 236.99, 237.01, 'bearing back');
  assert.deepStrictEqual(P.projectPoint(10, 20, 45, 0), [10, 20]);
});
test('aheadNm: ground speed times minutes', () => {
  assert.strictEqual(P.aheadNm(480, 2), 16);
  assert.strictEqual(P.aheadNm(600, 5), 50);
  assert.strictEqual(P.aheadNm(120, 1), 2);
  assert.strictEqual(P.aheadNm(0, 5), 0);
});
test('projectAhead: from the position along the track, great-circle points, null without one', () => {
  const t = { lat: 39, lon: -77, track_deg: 45, gs_kt: 480 };
  const line = P.projectAhead(t, 2);
  assert.deepStrictEqual(line[0], [39, -77], 'starts at the position');
  assert.strictEqual(line.length, 3, '16 nm: two steps of at most 10 nm');
  const far = P.rulerMeasure(39, -77, ...line[line.length - 1]);
  between(far.nm, 15.999, 16.001, 'length');
  between(far.bearing, 44.99, 45.01, 'initial bearing');
  // Every point is on the great circle: the bearing from the start stays the track's.
  const mid = P.rulerMeasure(39, -77, ...line[1]);
  between(mid.nm, 7.999, 8.001, 'mid distance');
  between(mid.bearing, 44.99, 45.01, 'mid bearing');
  assert.strictEqual(P.projectAhead(Object.assign({}, t, { gs_kt: 600 }), 5).length, 6, '50 nm: 5 steps');
  assert.strictEqual(P.projectAhead(Object.assign({}, t, { gs_kt: 60 }), 1).length, 2, '1 nm: one step');
  assert.strictEqual(P.projectAhead(t, 0), null, 'off');
  [{ track_deg: undefined }, { track_deg: null }, { gs_kt: undefined }, { gs_kt: 0 }, { gs_kt: -5 }, { gs_kt: NaN },
    { on_ground: true }, { lat: undefined }, { lon: null }].forEach((o) => {
    assert.strictEqual(P.projectAhead(Object.assign({}, t, o), 2), null, JSON.stringify(o));
  });
  assert.strictEqual(P.projectAhead(null, 2), null);
});
test('normalizeView: the Trails group (project ahead), all three in a set', () => {
  assert.deepStrictEqual(P.TRAIL_AHEAD_MIN, [0, 1, 2, 5]);
  assert.strictEqual(P.VIEW_DEFAULTS.trailAhead, 0);
  [0, 1, 2, 5].forEach((m) => assert.strictEqual(P.normalizeView({ trailAhead: m }).trailAhead, m));
  assert.strictEqual(P.normalizeView({ trailAhead: '5' }).trailAhead, 5, 'the select gives a string');
  [3, 10, -1, 2.5, '', 'off', null, true, [2]].forEach((m) => assert.strictEqual(P.normalizeView({ trailAhead: m }).trailAhead, 0, String(m)));
  assert.strictEqual(P.viewTrailsDefault(P.normalizeView({ trailAhead: 1 })), false);
  assert.strictEqual(P.viewActiveCount(P.normalizeView({ trailAhead: 5 })), 0);
  const v = P.normalizeView({ classes: ['airline'], trailShow: 'selected', trailColor: 'altitude', trailAhead: '2' });
  assert.deepStrictEqual([v.trailShow, v.trailColor, v.trailAhead], ['selected', 'altitude', 2]);
  const store = P.putViewSet(P.normalizeViewSets(null), 'Trails', v, false).store;
  const back = P.normalizeViewSets(JSON.parse(JSON.stringify(store))).sets[0];
  assert.deepStrictEqual(back.view, v);
  assert.strictEqual(P.viewSetMatches(back, v, false), true);
  assert.strictEqual(P.viewSetMatches(back, P.normalizeView(Object.assign({}, v, { trailAhead: 5 })), false), false);
  assert.strictEqual(P.viewTrailsDefault(P.normalizeView(P.VIEW_DEFAULTS)), true);
});

test('viewSetsFillTrails: old sets get the current Trails fields once; newer sets are left alone', () => {
  assert.deepStrictEqual(P.TRAIL_FIELDS, ['trailShow', 'trailColor', 'trailAhead']);
  const cur = P.normalizeView({ trailShow: 'selected', trailColor: 'signal', trailAhead: 5 });
  const raw = { version: 1, sets: [
    { name: 'Old', view: { search: 'ual' }, hideFiltered: true },
    { name: 'Part', view: { trailColor: 'altitude' } },
    { name: 'New', view: { trailShow: 'off', trailColor: 'decision', trailAhead: 0 } },
    { name: 'NoView' }
  ] };
  const r = P.viewSetsFillTrails(JSON.stringify(raw), cur);
  assert.strictEqual(r.filled, 3);
  const t = (s) => [s.view.trailShow, s.view.trailColor, s.view.trailAhead];
  assert.deepStrictEqual(r.store.sets.map((s) => s.name), ['Old', 'Part', 'New', 'NoView']);
  assert.deepStrictEqual(t(r.store.sets[0]), ['selected', 'signal', 5]);
  assert.strictEqual(r.store.sets[0].view.search, 'ual');
  assert.strictEqual(r.store.sets[0].hideFiltered, true);
  assert.deepStrictEqual(t(r.store.sets[1]), ['selected', 'altitude', 5], 'a field the set has is kept');
  assert.deepStrictEqual(t(r.store.sets[2]), ['off', 'decision', 0]);
  assert.deepStrictEqual(t(r.store.sets[3]), ['selected', 'signal', 5]);
  // Once filled (stored again), nothing more to fill: the second load is a no-op.
  const again = P.viewSetsFillTrails(r.store, P.normalizeView(null));
  assert.strictEqual(again.filled, 0);
  assert.deepStrictEqual(again.store, r.store);
  assert.deepStrictEqual(P.viewSetsFillTrails(null, cur), { store: { version: 2, sets: [] }, filled: 0 });
  assert.deepStrictEqual(P.viewSetsFillTrails('{', cur), { store: { version: 2, sets: [] }, filled: 0 });
  assert.strictEqual(raw.sets[0].view.trailShow, undefined, 'the input is not changed');
});
test('TRAIL_NONE_COLOR is neutral: not a scale or a decision colour', () => {
  const c = P.TRAIL_NONE_COLOR;
  assert.ok(/^#[0-9a-f]{6}$/.test(c));
  assert.ok(P.SIGNAL_COLORS.indexOf(c) < 0 && P.ALT_COLORS.indexOf(c) < 0);
  assert.ok(Object.values(P.DECISION_COLORS).indexOf(c) < 0);
  const rgb = [1, 3, 5].map((i) => parseInt(c.substr(i, 2), 16));
  assert.ok(Math.max(...rgb) - Math.min(...rgb) <= 24, 'grey');
});
test('ALT_COLORS: at least 3:1 against the Light tiles\' land', () => {
  const lum = (h) => [1, 3, 5].map((i) => parseInt(h.substr(i, 2), 16) / 255)
    .map((v) => (v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)))
    .reduce((s, v, i) => s + v * [0.2126, 0.7152, 0.0722][i], 0);
  const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  P.ALT_COLORS.forEach((c) => assert.ok(ratio(c, '#f2efe9') >= 3, c + ' ' + ratio(c, '#f2efe9').toFixed(2)));
});

// Ruler: distance, initial bearing, the label.
// Trail steps: join, skip a small step back in time, break on an implied
// speed over TRAIL_MAX_KT (over at least 1 s) or a big step back.
test('trailStep: impossible jumps break, small backsteps skip, normal moves join', () => {
  assert.strictEqual(P.TRAIL_MAX_KT, 1500);
  assert.strictEqual(P.TRAIL_BACKSTEP_S, 5);
  const a = { lat: 39, lon: -77, ts: 1000000 };
  const north = (nm, dtMs) => ({ lat: 39 + nm / 60, lon: -77, ts: a.ts + dtMs });
  // 1 nm in 2 s is 1800 kt, over the limit; 1 nm in 3 s (1200 kt) joins.
  assert.strictEqual(P.trailStep(a, north(1, 2000)), 'break', '1 nm in 2 s: 1800 kt');
  assert.strictEqual(P.trailStep(a, north(1, 3000)), 'join', '1 nm in 3 s: 1200 kt');
  assert.strictEqual(P.trailStep(a, north(0.5, 2000)), 'join', '0.5 nm in 2 s: 900 kt');
  assert.strictEqual(P.trailStep(a, north(100, 2000)), 'break', '100 nm in 2 s');
  assert.strictEqual(P.trailStep(a, north(200, 600000)), 'join', '200 nm in 10 min: 1200 kt');
  assert.strictEqual(P.trailStep(a, north(300, 600000)), 'break', '300 nm in 10 min: 1800 kt');
  // Equal times: the speed over 1 s (0.4 nm is 1440 kt; 1 nm would be 3600 kt).
  assert.strictEqual(P.trailStep(a, north(0, 0)), 'join', 'same time, same place');
  assert.strictEqual(P.trailStep(a, north(0.4, 0)), 'join', 'same time, 0.4 nm');
  assert.strictEqual(P.trailStep(a, north(1, 0)), 'break', 'same time, 1 nm: 3600 kt over 1 s');
  assert.strictEqual(P.trailStep(a, north(200, 0)), 'break', 'same time, 200 nm');
  assert.strictEqual(P.trailStep(a, north(0.1, 100)), 'join', '0.1 nm in 100 ms: speed over 1 s');
  // Back in time: up to 5 s skips (at any distance), more breaks.
  assert.strictEqual(P.trailStep(a, north(0, -50)), 'skip', '50 ms earlier');
  assert.strictEqual(P.trailStep(a, north(0, -2000)), 'skip', '2 s earlier');
  assert.strictEqual(P.trailStep(a, north(0, -5000)), 'skip', '5 s earlier');
  assert.strictEqual(P.trailStep(a, north(0, -10000)), 'break', '10 s earlier');
  assert.strictEqual(P.trailStep(null, a), 'join');
  assert.strictEqual(P.trailStep(a, { lat: 40, lon: -77, ts: NaN }), 'join', 'no usable time joins');
});

test('rulerMeasure LAX to JFK: 2144 nm, initial course 066', () => {
  // The Aviation Formulary's worked example (LAX 33°57'N 118°24'W, JFK
  // 40°38'N 73°47'W): 2144 nm, initial true course 66°. The formulary's
  // nm is one arc minute; on rangeNm's 6371008.8 m sphere it is 2145.
  const m = P.rulerMeasure(33 + 57 / 60, -(118 + 24 / 60), 40 + 38 / 60, -(73 + 47 / 60));
  between(m.nm, 2143, 2146, 'nm');
  between(m.km, 2143 * 1.852, 2146 * 1.852, 'km');
  between(m.bearing, 65.5, 66.5, 'bearing');
  assert.strictEqual(P.rulerLabel(m), '2,145 nm · 3,973 km · 066°T');
});
test('rulerMeasure DC to NYC and back', () => {
  const m = P.rulerMeasure(38.9, -77.0, 40.7, -74.0);
  between(m.nm, 175, 176, 'nm');
  between(m.bearing, 50, 52, 'bearing');
  between(P.rulerMeasure(40.7, -74.0, 38.9, -77.0).bearing, 232, 234, 'reverse bearing');
});
test('rulerLabel formats', () => {
  assert.strictEqual(P.rulerLabel({ nm: 0, km: 0, bearing: 0 }), '0.0 nm · 0.0 km · 000°T');
  assert.strictEqual(P.rulerLabel({ nm: 12.345, km: 22.863, bearing: 7.4 }), '12.3 nm · 22.9 km · 007°T');
  assert.strictEqual(P.rulerLabel({ nm: 600, km: 1111.2, bearing: 359.6 }), '600.0 nm · 1,111 km · 000°T');
  assert.strictEqual(P.rulerLabel(null), '');
  assert.strictEqual(P.rulerLabel({ nm: NaN, km: NaN, bearing: 0 }), '');
});

test('formatNavModes and formatAcasRa', () => {
  assert.strictEqual(P.formatNavModes(['autopilot', 'vnav']), 'autopilot, vnav');
  assert.strictEqual(P.formatNavModes([]), '—');
  assert.strictEqual(P.formatNavModes(null), '—');
  assert.strictEqual(P.formatAcasRa({ active: true, text: 'climb' }), 'RA: climb');
  assert.strictEqual(P.formatAcasRa({ active: false, text: 'clear of conflict' }), 'RA ended: clear of conflict');
  assert.strictEqual(P.formatAcasRa({ active: true }), 'RA');
  assert.strictEqual(P.formatAcasRa(null), '—');
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
