// skycue web UI: pure helpers (no DOM, no Leaflet), shared by the page
// (window.skycuePure) and the node test (module.exports). Plain ES2017.
(function (root) {
  'use strict';

  // Geometry: must match skycued/src/geo.cpp (and geo.hpp's constants):
  // haversine on a sphere of radius 6371008.8 m, 1852 m per nm, and the
  // elevation's curvature term on a 4/3 effective earth radius.
  var EARTH_RADIUS_M = 6371008.8;
  var METRES_PER_NM = 1852;
  var EFFECTIVE_RADIUS_M = 4 / 3 * EARTH_RADIUS_M;
  var DEG = Math.PI / 180;

  function centralAngleRad(lat1, lon1, lat2, lon2) {
    var p1 = lat1 * DEG, p2 = lat2 * DEG;
    var s1 = Math.sin((lat2 - lat1) * DEG / 2), s2 = Math.sin((lon2 - lon1) * DEG / 2);
    var a = s1 * s1 + Math.cos(p1) * Math.cos(p2) * s2 * s2;
    a = Math.min(1, Math.max(0, a));
    return 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  }

  function rangeNm(lat1, lon1, lat2, lon2) {
    return centralAngleRad(lat1, lon1, lat2, lon2) * EARTH_RADIUS_M / METRES_PER_NM;
  }

  function bearingDeg(lat1, lon1, lat2, lon2) {
    var p1 = lat1 * DEG, p2 = lat2 * DEG, dl = (lon2 - lon1) * DEG;
    var y = Math.sin(dl) * Math.cos(p2);
    var x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
    if (x === 0 && y === 0) return 0;
    var b = Math.atan2(y, x) / DEG;
    if (b < 0) b += 360;
    return b >= 360 ? 0 : b;
  }

  // Elevation in degrees of a target seen from the receiver; altitudes in
  // metres (the receiver's is RECEIVER_ALT_M + ANTENNA_HEIGHT_M).
  function elevationAtDistance(dM, dhM) {
    return Math.atan2(dhM - dM * dM / (2 * EFFECTIVE_RADIUS_M), dM) / DEG;
  }
  function elevationDeg(rxLat, rxLon, rxAltM, lat, lon, altM) {
    var d = centralAngleRad(rxLat, rxLon, lat, lon) * EARTH_RADIUS_M;
    return elevationAtDistance(d, altM - rxAltM);
  }

  // Distance (nm) at which a target at tgtAltM sits exactly at maskDeg:
  // inside it the target is above the mask. Elevation falls monotonically
  // with distance, so bisection; null when even a target next to the
  // receiver is below the mask, maxNm when the whole range is above it.
  function elevationRingNm(rxLat, rxLon, rxAltM, tgtAltM, maskDeg, maxNm) {
    var dh = tgtAltM - rxAltM;
    var lo = 1, hi = maxNm * METRES_PER_NM; // metres
    if (elevationAtDistance(lo, dh) < maskDeg) return null;
    if (elevationAtDistance(hi, dh) >= maskDeg) return maxNm;
    for (var i = 0; i < 60; i++) {
      var mid = (lo + hi) / 2;
      if (elevationAtDistance(mid, dh) >= maskDeg) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2 / METRES_PER_NM;
  }

  // Decision -> colour class (CSS .decision-<class>); null: not drawn.
  var DECISION_CLASS = {
    send: 'sent', allow_listed: 'allow_listed', out_of_range: 'out_of_range',
    below_elevation: 'below_elevation', blocked: 'blocked', no_position: null
  };
  var DECISION_COLORS = {
    sent: '#3fb950', allow_listed: '#58a6ff', out_of_range: '#8b949e',
    below_elevation: '#f0883e', blocked: '#f85149', unknown: '#d2a8ff'
  };
  function decisionColor(decision) {
    return Object.prototype.hasOwnProperty.call(DECISION_CLASS, decision) ? DECISION_CLASS[decision] : 'unknown';
  }

  function thousands(n) {
    return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }
  function formatAlt(t) {
    if (t.on_ground) return 'ground';
    if (typeof t.alt_baro_ft === 'number') return thousands(t.alt_baro_ft) + ' ft';
    if (typeof t.alt_geom_ft === 'number') return thousands(t.alt_geom_ft) + ' ft (geom)';
    return '—';
  }
  function formatNum(v, digits, unit) {
    if (typeof v !== 'number' || !isFinite(v)) return '—';
    return v.toFixed(digits) + (unit || '');
  }
  function formatAge(fromMs, nowMs) {
    var s = Math.max(0, Math.round((nowMs - fromMs) / 1000));
    if (s < 60) return s + ' s ago';
    var m = Math.floor(s / 60);
    if (m < 60) return m + ' min ' + (s % 60) + ' s ago';
    return Math.floor(m / 60) + ' h ' + (m % 60) + ' min ago';
  }

  function trackKey(t) { return t.hex + '|' + t.addr_type; }
  function markerLabel(t) { return t.callsign || t.hex; }
  function listHas(list, entry) {
    if (!list || !entry) return false;
    var e = String(entry).toLowerCase();
    for (var i = 0; i < list.length; i++) if (String(list[i]).toLowerCase() === e) return true;
    return false;
  }

  // Stable sort by one key; tracks without the key always go last.
  function sortTracks(tracks, key, dir) {
    return tracks.map(function (t, i) { return [t, i]; }).sort(function (a, b) {
      var x = a[0][key], y = b[0][key];
      var mx = x === undefined || x === null, my = y === undefined || y === null;
      if (mx || my) return mx === my ? a[1] - b[1] : (mx ? 1 : -1);
      var c = typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y));
      return c !== 0 ? c * dir : a[1] - b[1];
    }).map(function (p) { return p[0]; });
  }

  function graticuleStep(zoom) { return zoom < 6 ? 5 : 1; }

  // A drop for `filtered` only stops forwarding: the aircraft is still
  // observed (and shown, recoloured). Any other reason ends the track.
  function dropRemovesMarker(reason) { return reason !== 'filtered'; }

  function backoffMs(attempt) { return Math.min(5000, 500 * Math.pow(2, attempt)); }

  // skycued's data clock minus the browser's clock, after seeing the
  // data-clock time `dataMs` (an observed item's last_seen) at browser time
  // `wallMs`. It only moves forward (items reach the page late, never
  // early), except after a jump back of more than CLOCK_RESET_MS (a replay
  // that looped, or skycued's clock set back), which resets it.
  var CLOCK_RESET_MS = 5000;
  function nextClockOffset(offsetMs, dataMs, wallMs) {
    if (typeof dataMs !== 'number' || !isFinite(dataMs)) return offsetMs;
    var seen = dataMs - wallMs;
    return seen > offsetMs || offsetMs - seen > CLOCK_RESET_MS ? seen : offsetMs;
  }

  // ---- FIS-B (docs/web-api.md, FIS-B) ------------------------------------
  var BINS_PER_BLOCK = 128;

  // A mosaic block's `rle` ([[value, count], ...]) as its 128 bins, W->E
  // then N->S in 4 rows of 32. Throws when the runs are malformed or do
  // not total 128 bins.
  function expandRle(rle) {
    if (!Array.isArray(rle)) throw new Error('rle is not an array');
    var out = new Uint8Array(BINS_PER_BLOCK);
    var n = 0;
    for (var i = 0; i < rle.length; i++) {
      var run = rle[i];
      if (!Array.isArray(run) || run.length !== 2) throw new Error('bad run ' + JSON.stringify(run));
      var v = run[0], c = run[1];
      if (!(v >= 0 && v <= 255 && Math.floor(v) === v) || !(c >= 0 && Math.floor(c) === c)) {
        throw new Error('bad run ' + JSON.stringify(run));
      }
      if (n + c > BINS_PER_BLOCK) throw new Error('rle totals more than 128 bins');
      out.fill(v, n, n + c);
      n += c;
    }
    if (n !== BINS_PER_BLOCK) throw new Error('rle totals ' + n + ' bins, not 128');
    return out;
  }

  // The palette's CSS colour for a bin value; 'transparent' for a level the
  // palette marks so, or does not have.
  function dbzColor(level, palette) {
    if (!Array.isArray(palette)) return 'transparent';
    var e = palette[level];
    if (!e || e.level !== level) {
      e = null;
      for (var i = 0; i < palette.length; i++) if (palette[i] && palette[i].level === level) { e = palette[i]; break; }
    }
    return e && e.color ? e.color : 'transparent';
  }

  // TWGO shape families: one map layer each.
  var SHAPE_FAMILY = { 8: 'tfr', 16: 'tfr', 17: 'tfr', 12: 'sigmet', 11: 'airmet', 14: 'airmet', 15: 'cwa', 13: 'sua' };
  var SHAPE_COLORS = { tfr: '#f85149', sigmet: '#bc8cff', airmet: '#e3b341', cwa: '#f0883e', sua: '#8b949e' };
  function shapeFamily(product) {
    return Object.prototype.hasOwnProperty.call(SHAPE_FAMILY, product) ? SHAPE_FAMILY[product] : null;
  }
  // Leaflet path options for a shape of `product`; cancelled ones dashed.
  function shapeStyle(product, cancelled) {
    var fam = shapeFamily(product) || 'other';
    var color = SHAPE_COLORS[fam] || '#d2a8ff';
    var s = {
      className: 'fisb-shape fisb-' + fam + (cancelled ? ' cancelled' : ''),
      color: color, weight: 2, opacity: 0.9, fillColor: color, fillOpacity: fam === 'tfr' ? 0.18 : fam === 'airmet' ? 0.04 : 0.08
    };
    if (cancelled) {
      s.dashArray = '6 6';
      s.fillOpacity = 0.03;
    }
    return s;
  }

  // Age colouring of a text report: fresh < 30 min, aging < 90 min, else old
  // (and old when the time is missing or unreadable).
  var AGE_FRESH_MS = 30 * 60000, AGE_AGING_MS = 90 * 60000;
  function ageClass(issuedIso, nowMs) {
    var t = typeof issuedIso === 'string' ? Date.parse(issuedIso) : NaN;
    if (isNaN(t)) return 'old';
    var age = nowMs - t;
    if (age < AGE_FRESH_MS) return 'fresh';
    if (age < AGE_AGING_MS) return 'aging';
    return 'old';
  }

  // The bounding box [[south, west], [north, east]] of mosaic blocks (each
  // NW corner lat0/lon0, 4 rows of dlat by 32 columns of dlon); null for none.
  function mosaicBounds(blocks) {
    if (!blocks || !blocks.length) return null;
    var s = Infinity, w = Infinity, n = -Infinity, e = -Infinity;
    for (var i = 0; i < blocks.length; i++) {
      var b = blocks[i];
      s = Math.min(s, b.lat0 - 4 * b.dlat);
      n = Math.max(n, b.lat0);
      w = Math.min(w, b.lon0);
      e = Math.max(e, b.lon0 + 32 * b.dlon);
    }
    return [[s, w], [n, e]];
  }

  // Web Mercator y of a latitude, in degrees (as Leaflet stretches an image
  // overlay: linear in this, not in latitude).
  function mercY(lat) {
    if (lat === 0) return 0;
    return Math.log(Math.tan(Math.PI / 4 + lat * DEG / 2)) / DEG;
  }

  // Canvas size for a mosaic over `bounds`, `pxPerBin` pixels per smallest
  // bin (dlat, dlon), scaled down to fit `cap` x `cap`.
  function mosaicCanvasSize(bounds, dlat, dlon, pxPerBin, cap) {
    var s = bounds[0][0], w = bounds[0][1], n = bounds[1][0], e = bounds[1][1];
    var binY = Math.min(mercY(Math.min(s + dlat, n)) - mercY(s), mercY(n) - mercY(Math.max(n - dlat, s)));
    var width = (e - w) / dlon * pxPerBin;
    var height = (mercY(n) - mercY(s)) / binY * pxPerBin;
    var k = Math.min(1, cap / width, cap / height);
    return { width: Math.max(1, Math.round(width * k)), height: Math.max(1, Math.round(height * k)) };
  }

  function stationMatches(station, prefix) {
    if (!prefix) return true;
    return String(station || '').toUpperCase().indexOf(String(prefix).toUpperCase()) === 0;
  }

  var api = {
    EARTH_RADIUS_M: EARTH_RADIUS_M, METRES_PER_NM: METRES_PER_NM, DECISION_COLORS: DECISION_COLORS,
    rangeNm: rangeNm, bearingDeg: bearingDeg, elevationDeg: elevationDeg, elevationRingNm: elevationRingNm,
    decisionColor: decisionColor, formatAlt: formatAlt, formatNum: formatNum, formatAge: formatAge,
    thousands: thousands, trackKey: trackKey, markerLabel: markerLabel, listHas: listHas,
    sortTracks: sortTracks, graticuleStep: graticuleStep, dropRemovesMarker: dropRemovesMarker,
    backoffMs: backoffMs, CLOCK_RESET_MS: CLOCK_RESET_MS, nextClockOffset: nextClockOffset,
    BINS_PER_BLOCK: BINS_PER_BLOCK, expandRle: expandRle, dbzColor: dbzColor, SHAPE_COLORS: SHAPE_COLORS,
    shapeFamily: shapeFamily, shapeStyle: shapeStyle, ageClass: ageClass, mosaicBounds: mosaicBounds,
    mercY: mercY, mosaicCanvasSize: mosaicCanvasSize, stationMatches: stationMatches
  };
  root.skycuePure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
