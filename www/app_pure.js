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

  // "Hide filtered": only sent and allow-listed tracks stay drawn. Takes skycued's
  // decision strings (send, allow_listed) or the page's colour classes.
  function trackVisible(decision, hideFiltered) {
    return !hideFiltered || decision === 'send' || decision === 'allow_listed' ||
      decision === 'sent' || decision === 'allow';
  }
  // How many tracks the toggle hides: filtered ones that have a position (so
  // would have had a marker). no_position tracks are never drawn. `tracks` is an array of track objects.
  function hiddenCount(tracks, hideFiltered) {
    if (!hideFiltered) return 0;
    var n = 0;
    tracks.forEach(function (t) {
      if (t.decision !== 'no_position' && typeof t.lat === 'number' && typeof t.lon === 'number' &&
          !trackVisible(t.decision, true)) n++;
    });
    return n;
  }

  // Map style (a view option kept in localStorage): 'light' shows the tiles
  // as baked, 'dark' puts this CSS filter on the tile pane only (markers,
  // shapes, radar canvases and labels sit in other panes and keep their
  // colours). Inverting and turning the hue back keeps sea blue-ish and land
  // brown-grey, darker and duller.
  var MAP_STYLES = ['light', 'dark'];
  var DARK_TILE_FILTER = 'invert(1) hue-rotate(180deg) brightness(.8) contrast(.85) saturate(.55)';
  function mapStyle(stored) { return stored === 'dark' ? 'dark' : 'light'; }
  function tileFilter(style) { return mapStyle(style) === 'dark' ? DARK_TILE_FILTER : ''; }

  // Overlays (scripts/maps/make-overlays.sh). Borders: admin0 (countries) at
  // every zoom, admin1 (states, provinces) from zoom 5. Style per map style:
  // dark lines on the light tiles, light ones on the dark.
  var ADMIN1_MIN_ZOOM = 5;
  function borderVisible(kind, zoom) { return kind === 'admin1' ? zoom >= ADMIN1_MIN_ZOOM : kind === 'admin0'; }
  function borderStyle(kind, style, zoom) {
    var dark = mapStyle(style) === 'dark';
    var a1 = kind === 'admin1';
    return {
      color: dark ? (a1 ? '#a9b4c2' : '#dfe6ee') : (a1 ? '#5b5266' : '#3b3340'),
      weight: a1 ? 0.8 : 1.2,
      opacity: borderVisible(kind, zoom) ? (a1 ? 0.6 : 0.8) : 0,
      dashArray: a1 ? '4 3' : null,
      fill: false,
      interactive: false
    };
  }
  // Cities: the largest scalerank shown at a zoom (0 is the most important).
  function cityRankLimit(zoom) { return zoom < 5 ? 2 : zoom < 7 ? 4 : Infinity; }
  var MAX_CITY_LABELS = 300;
  // The cities to label: rank within the zoom's limit, inside `bounds`
  // ({south, west, north, east}, degrees; west > east crosses 180°), most
  // important first (rank, then population), at most `max` (default
  // MAX_CITY_LABELS). `cities` is an array of {name, rank, pop, lat, lon}.
  function cullCities(cities, bounds, zoom, max) {
    var limit = cityRankLimit(zoom);
    var cap = typeof max === 'number' ? max : MAX_CITY_LABELS;
    var wrap = bounds.west > bounds.east;
    var out = cities.filter(function (c) {
      if (!(c.rank <= limit) || !(c.lat >= bounds.south && c.lat <= bounds.north)) return false;
      return wrap ? (c.lon >= bounds.west || c.lon <= bounds.east) : (c.lon >= bounds.west && c.lon <= bounds.east);
    });
    out.sort(function (a, b) { return a.rank - b.rank || (b.pop || 0) - (a.pop || 0); });
    return out.length > cap ? out.slice(0, cap) : out;
  }
  // overlays/meta.json -> which toggles to offer: {borders, cities}.
  function overlayOffer(meta) {
    var f = meta && typeof meta === 'object' && meta.files && typeof meta.files === 'object' ? meta.files : {};
    return { borders: !!(f.admin0 || f.admin1), cities: !!f.cities };
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

  // Map tiles: the ancestor `n` levels up of XYZ tile z/x/y (n >= 1, at
  // most z), and where the child sits in it. Drawn `scale` times its size
  // and shifted by (-offsetX, -offsetY) child tiles, the parent covers
  // exactly the child's square. null when there is no such ancestor.
  // x wraps around the world as Leaflet's tile coordinates may not.
  function parentTile(z, x, y, n) {
    if (!(n >= 1) || n > z || n !== Math.floor(n) || z !== Math.floor(z)) return null;
    var size = Math.pow(2, z);
    if (y < 0 || y >= size) return null;
    x = ((x % size) + size) % size;
    var scale = Math.pow(2, n);
    var px = Math.floor(x / scale), py = Math.floor(y / scale);
    return { z: z - n, x: px, y: py, scale: scale, offsetX: x - px * scale, offsetY: y - py * scale };
  }

  // The lon/lat box of XYZ tile z/x/y (Web Mercator).
  function tileBounds(z, x, y) {
    var size = Math.pow(2, z);
    var lat = function (yy) { return Math.atan(Math.sinh(Math.PI * (1 - 2 * yy / size))) / DEG; };
    return { west: x / size * 360 - 180, east: (x + 1) / size * 360 - 180, north: lat(y), south: lat(y + 1) };
  }

  // True when tile z/x/y lies wholly inside the bbox of a meta.json region
  // ({bbox: [w, s, e, n], minZoom, maxZoom}) that has zoom z: such a tile is
  // opaque. A tile only partly inside every region with zoom z sits on a
  // region's edge and may be partly transparent there. Entries that are not
  // well-formed regions are ignored.
  function tileInsideRegions(z, x, y, regions) {
    var t = tileBounds(z, x, y);
    var eps = 1e-9;
    return (regions || []).some(function (r) {
      var b = r && r.bbox;
      if (!b || b.length !== 4 || !(r.minZoom <= z && z <= r.maxZoom)) return false;
      return b[0] <= t.west + eps && t.east <= b[2] + eps && b[1] <= t.south + eps && t.north <= b[3] + eps;
    });
  }

  // The smallest n, from <= n <= maxLevels, whose ancestor n levels up of
  // tile z/x/y lies wholly inside a region (tileInsideRegions on the
  // ancestor), or null: the first ancestor that is opaque everywhere, for an
  // underlay below a region-edge tile.
  function nearestInsideAncestor(z, x, y, from, maxLevels, regions) {
    for (var n = Math.max(1, from); n <= maxLevels; n++) {
      var p = parentTile(z, x, y, n);
      if (!p) return null;
      if (tileInsideRegions(p.z, p.x, p.y, regions)) return n;
    }
    return null;
  }

  // The level an underlay tries after level n's image failed to load: the
  // next ancestor inside a region (nearestInsideAncestor from n + 1), or null.
  // Never the plain n + 1, which may be a region-edge tile, transparent in
  // the very place the underlay is for.
  function underlayNextLevel(z, x, y, n, maxLevels, regions) {
    return nearestInsideAncestor(z, x, y, n + 1, maxLevels, regions);
  }

  // The weather card closes: no shape or report is shown, and the report
  // "show" kept through the symbol cap (showKey) is let go, so the cap
  // is recomputed without it (symDirty) when one was kept.
  function closeReportCard(fisb) {
    if (fisb.showKey !== null && fisb.showKey !== undefined) fisb.symDirty = true;
    fisb.shapeKey = null;
    fisb.reportRec = null;
    fisb.showKey = null;
  }

  // At most `max` of `symbols` ({key, rank, lat, lon}), as a new array: the
  // one whose key is keepKey first (the report "show" panned to stays on the
  // map), then the rest by rank, then nearest `centre` ({lat, lng}) first.
  function capSymbols(symbols, centre, max, keepKey) {
    var kept = null, rest = [];
    symbols.forEach(function (s) {
      if (kept === null && keepKey !== null && keepKey !== undefined && s.key === keepKey) kept = s;
      else rest.push({ s: s, d: rangeNm(centre.lat, centre.lng, s.lat, s.lon) });
    });
    rest.sort(function (a, b) { return a.s.rank - b.s.rank || a.d - b.d; });
    var out = kept ? [kept] : [];
    for (var i = 0; i < rest.length && out.length < max; i++) out.push(rest[i].s);
    if (out.length > max) out.length = max;
    return out;
  }

  // A drop for `filtered` only stops forwarding: the aircraft is still
  // observed (and shown, recoloured). Any other reason ends the track.
  function dropRemovesMarker(reason) { return reason !== 'filtered'; }

  // ---- Cues tab ----
  // The stats counters the tab shows, in this order (docs/web-api.md, the
  // stats object): what was sent, then the drops by reason and the
  // filtered drops by the decision that dropped them.
  var CUE_COUNTERS = [
    ['sent_updates', 'Updates sent'], ['sent_drops', 'Drops sent'],
    ['drops_timeout', 'Dropped: timeout'], ['drops_filtered', 'Dropped: filtered'],
    ['drops_blocked', 'Filtered: blocked'], ['drops_out_of_range', 'Filtered: out of range'],
    ['drops_below_elevation', 'Filtered: below elevation'], ['drops_no_position', 'Filtered: no position'],
    ['drops_source_lost', 'Dropped: source lost']
  ];
  var DECISIONS_RING = 200;         // update / drop items kept for the Recent decisions table

  // Appends `item`, drops the oldest past `cap`; the same array.
  function pushRing(ring, item, cap) {
    ring.push(item);
    if (ring.length > cap) ring.splice(0, ring.length - cap);
    return ring;
  }

  // One Recent decisions row from an `update` or `drop` ws item.
  function decisionRow(it) {
    var drop = it.type === 'drop';
    return {
      key: trackKey(it), time: drop ? it.time : it.last_seen, hex: it.hex, callsign: it.callsign || '',
      decision: drop ? (it.decision || '') : it.decision, reason: drop ? it.reason : 'sent'
    };
  }

  // GET /api/filters/history's changes (oldest first, each the new state)
  // as rows newest first, each with what changed from the entry before it;
  // the first (startup) entry has every field, from null.
  function filterHistoryRows(changes) {
    var rows = [];
    changes.forEach(function (c, i) {
      var prev = i ? changes[i - 1].filters || {} : null, cur = c.filters || {}, diffs = [];
      Object.keys(cur).forEach(function (f) {
        var from = prev && f in prev ? prev[f] : null;
        if (!prev || from !== cur[f]) diffs.push({ field: f, from: from, to: cur[f] });
      });
      rows.unshift({ time: c.time, origin: c.origin, diffs: diffs });
    });
    return rows;
  }

  // ---- Config tab ----
  // GET /api/config's keys in the tab's tables, in this order; a key no
  // group names (one added to skycued later) goes to a trailing Other.
  var CONFIG_GROUPS = [
    ['Receiver site', ['RECEIVER_LAT', 'RECEIVER_LON', 'RECEIVER_ALT_M', 'ANTENNA_HEIGHT_M']],
    ['Decoders', ['SDR_1090_SERIAL', 'SDR_978_SERIAL', 'GAIN_1090', 'GAIN_978', 'DUMP978_RAW_PORT', 'DUMP978_JSON_PORT', 'READSB_JSON_PORT', 'INPUTS_FILE']],
    ['Cue outputs', ['CUE_TCP_PORT', 'CUE_TCP_CONNECT', 'CUE_UDP_TARGETS', 'MIN_INTERVAL_S', 'MAX_INTERVAL_S', 'TRACK_TIMEOUT_S', 'KEYFRAME_INTERVAL_S']],
    ['Recording', ['RECORD_DIR', 'RECORD_FORMATS', 'RECORD_MAX_GB', 'RECORD_RAW']],
    ['Filters', ['MAX_RANGE_NM', 'MIN_ELEVATION_DEG', 'ALLOW_LIST_FILE', 'BLOCK_LIST_FILE']],
    ['Web', ['WEB_BIND', 'WEB_PORT', 'TRAIL_SECONDS']],
    ['Statistics', ['STATS_FILE', 'STATS_AIRCRAFT_MAX', 'STATS_SAVE_S']],
    ['FIS-B', ['FISB_ENABLE', 'FISB_EXPERIMENTAL', 'LOCATIONS_FILE']]];

  // [{group, rows}] in CONFIG_GROUPS order, each group's rows in its key
  // order; groups with no key are left out.
  function groupConfig(keys) {
    var byKey = {}, named = {};
    keys.forEach(function (k) { byKey[k.key] = k; });
    var out = [];
    CONFIG_GROUPS.forEach(function (g) {
      var rows = [];
      g[1].forEach(function (name) {
        named[name] = true;
        if (byKey[name]) rows.push(byKey[name]);
      });
      if (rows.length) out.push({ group: g[0], rows: rows });
    });
    var other = keys.filter(function (k) { return !named[k.key]; });
    if (other.length) out.push({ group: 'Other', rows: other });
    return out;
  }

  // GET /api/config's daemon facts as the Config tab's Daemon rows: how it
  // runs (a replay, or live and which built-in decoders it connects to) and
  // the report locations it holds.
  function daemonModeText(d) {
    if (d.mode === 'replay' && d.replay) {
      var r = d.replay;
      return 'replay of ' + r.file + ' (' + (r.speed === 0 ? 'unpaced' : 'speed ' + r.speed) +
        (r.loop ? ', looped' : '') + ')';
    }
    return 'live: ' + (d.readsb === false ? 'no readsb (--no-readsb)' : 'readsb') + ', ' +
      (d.dump978 === false ? 'no dump978 (--no-uat)' : 'dump978');
  }
  function daemonLocationsText(d) {
    var l = d.locations || {};
    var learned = (l.learned || 0) + ' learned, ' + (l.learned_path ? 'kept in ' + l.learned_path : 'not saved this run');
    var op = !l.operator_path ? 'no LOCATIONS_FILE'
      : (l.operator || 0) + ' from ' + l.operator_path +
        (l.operator_rejected ? ', ' + l.operator_rejected + ' lines rejected' : '') +
        (l.operator_overflow ? ', ' + l.operator_overflow + ' over the cap' : '');
    return [learned, op];
  }

  // A value as skycue.conf spells it, the same rules as conf_rewrite.cpp
  // quoteValue: bare when every character is one of the grammar's unquoted
  // ones, else "..." when it has none of " $ ` \, else '...' when it has no
  // '; null when the grammar cannot hold it (both kinds, or a line break).
  function confValue(v) {
    if (/[\r\n]/.test(v)) return null;
    if (/^[A-Za-z0-9_.,:/+@%=-]*$/.test(v)) return v;
    if (!/["$`\\]/.test(v)) return '"' + v + '"';
    if (v.indexOf("'") < 0) return "'" + v + "'";
    return null;
  }

  // The whole configuration as skycue.conf text (the Config tab's copy):
  // a `# <group>` line before each group, and before each key a comment
  // with its source and whether it needs a restart, so every line is one
  // parseConfigText and skycued accept. A value the grammar cannot
  // hold is left as a comment saying so.
  function configText(cfg) {
    var d = cfg.daemon || {}, out = '# ' + d.version + ', started ' + d.started + '\n';
    groupConfig(cfg.keys || []).forEach(function (g) {
      out += '# ' + g.group + '\n';
      g.rows.forEach(function (r) {
        var v = String(r.value), q = confValue(v);
        out += '# ' + r.key + ': ' + r.source + (r.restart ? ', restart' : '') + '\n';
        out += q === null
          ? '# ' + r.key + ' cannot be written in skycue.conf (it holds both \' and one of " $ ` \\, or a line break): ' +
            v.replace(/[\r\n]+/g, ' ') + '\n'
          : r.key + '=' + q + '\n';
      });
    });
    return out;
  }

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
  // Leaflet path options for a shape of `product`: cancelled ones dashed;
  // pending ones (not yet in force) dashed finer and dimmed.
  function shapeStyle(product, cancelled, pending) {
    var fam = shapeFamily(product) || 'other';
    var color = SHAPE_COLORS[fam] || '#d2a8ff';
    var s = {
      className: 'fisb-shape fisb-' + fam + (cancelled ? ' cancelled' : '') + (pending && !cancelled ? ' pending' : ''),
      color: color, weight: 2, opacity: 0.9, fillColor: color, fillOpacity: fam === 'tfr' ? 0.18 : fam === 'airmet' ? 0.04 : 0.08
    };
    if (cancelled) {
      s.dashArray = '6 6';
      s.fillOpacity = 0.03;
    } else if (pending) {
      s.dashArray = '2 5';
      s.opacity = 0.45;
      s.fillOpacity = s.fillOpacity / 3;
    }
    return s;
  }

  // Radar fades to half its opacity past these ages (a regional mosaic is
  // re-sent every few minutes, CONUS less often); it expires at 75 min.
  var RADAR_FADE_MS = { 63: 15 * 60000, 64: 30 * 60000 };
  function radarFaded(ageMs, product) {
    var fade = RADAR_FADE_MS[product] || RADAR_FADE_MS[63];
    return ageMs > fade;
  }
  // The opacity to draw a radar mosaic `ageMs` old at, `base` the slider's.
  function radarOpacity(ageMs, product, base) {
    return radarFaded(ageMs, product) ? base * 0.5 : base;
  }
  // The slider's base opacity from its stored value (skycue.radarOpacity):
  // a number in the slider's range [0.2, 1], else the default 0.7.
  function radarOpacityValue(stored) {
    var v = stored === null || stored === undefined || stored === '' ? NaN : Number(stored);
    return isFinite(v) && v >= 0.2 && v <= 1 ? v : 0.7;
  }

  // Seconds the newest product time runs ahead of the data clock, when that
  // is more than the 10 min a receiver clock or a ground station can be
  // off by: a warning for the status strip; null otherwise.
  var CLOCK_SKEW_WARN_S = 600;
  function skewWarning(skewS) {
    if (typeof skewS !== 'number' || !(skewS > CLOCK_SKEW_WARN_S)) return null;
    var min = Math.round(skewS / 60);
    return 'product times ' + (min < 120 ? min + ' min' : Math.round(min / 60) + ' h') + ' ahead of the data clock';
  }

  // Whether the status poll should fetch a radar mosaic: `head` is the
  // status's mosaic head, `drawnHead` the head of the mosaic drawn (null for
  // none). Only a published mosaic is fetched, and only when its publish is
  // not the one drawn (a missed fisb_radar item). A mosaic still filling
  // (unpublished, or its block count moving between publishes) is not.
  function radarNeedsFetch(drawnHead, head) {
    if (!head || !head.published) return false;
    return !drawnHead || drawnHead.published !== head.published;
  }

  // Raster layers by product. Icing and turbulence (70/71/90/91) come per
  // 3-bit altitude code; a band's altitude is approximate (docs/web-api.md).
  var RASTER_BAND_PRODUCTS = [70, 71, 90, 91];
  function rasterBandFt(product, code) {
    return (product === 71 || product === 91 ? 18000 : 2000) + 2000 * code;
  }
  // heads (status.rasters[]) -> one entry per product id, sorted: a band
  // product lists codes 0..7 (key null: no layer at that code), any other
  // has its head's key and no bands.
  function rasterProducts(heads) {
    var by = {};
    (heads || []).forEach(function (h) {
      var e = by[h.product];
      if (!e) {
        var band = RASTER_BAND_PRODUCTS.indexOf(h.product) >= 0, bands = [];
        for (var c = 0; band && c < 8; c++) bands.push({ code: c, ft: rasterBandFt(h.product, c), key: null });
        e = by[h.product] = { product: h.product, name: h.name || 'product ' + h.product, key: null, bands: bands };
      }
      if (!e.bands.length) e.key = h.key;
      else if (typeof h.altitude_code === 'number' && e.bands[h.altitude_code]) e.bands[h.altitude_code].key = h.key;
    });
    return Object.keys(by).map(function (k) { return by[k]; }).sort(function (a, b) { return a.product - b.product; });
  }
  function defaultBand(entry) {
    var bands = (entry && entry.bands) || [];
    for (var i = 0; i < bands.length; i++) if (bands[i].key) return bands[i].code;
    return null;
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

  // ---- Receiver tab: input definitions (docs/web-api.md, Inputs) ----
  // The rules and messages of skycued's parseInputDef
  // (skycued/src/inputs/input_def.cpp), for the fields the add form has.
  var INPUT_TYPES = ['readsb', 'dump978', 'pingstation_json', 'pingstation_cat021'];
  var TRANSPORTS = ['udp', 'tcp_connect', 'tcp_listen'];
  var EDITIONS = ['2.2', '2.3', '2.4', '2.5', '2.6', '2.7'];
  var RESERVED_IDS = ['readsb', 'dump978', 'replay'];
  function isPingStation(type) { return type === 'pingstation_json' || type === 'pingstation_cat021'; }
  function inputTransport(type, transport) {
    if (type === 'pingstation_json') return 'udp';
    // cat021 has no default in skycued (transport is required); the add form
    // preselects udp, so the fields shown before one is chosen are udp's.
    if (type === 'pingstation_cat021') return TRANSPORTS.indexOf(transport) >= 0 ? transport : 'udp';
    return 'tcp_connect';
  }
  // Dotted decimal, four parts of 0..255 without leading zeros.
  function isIpv4(s) {
    return typeof s === 'string' && /^(0|[1-9]\d{0,2})(\.(0|[1-9]\d{0,2})){3}$/.test(s) &&
      s.split('.').every(function (p) { return Number(p) <= 255; });
  }
  // An IPv4 address or a hostname of at most 253 characters (a trailing dot
  // not counted), labels of 1..63 letters, digits and '-' not at either end,
  // the last label not all digits.
  function isHost(s) {
    if (typeof s !== 'string') return false;
    if (isIpv4(s)) return true;
    var name = s.charAt(s.length - 1) === '.' ? s.slice(0, -1) : s;
    if (!name || name.length > 253) return false;
    var labels = name.split('.');
    var ok = labels.every(function (l) { return /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(l); });
    return ok && !/^\d+$/.test(labels[labels.length - 1]);
  }
  function isHttpUrl(url) {
    if (url.indexOf('http://') !== 0 || /[\x00-\x20\x7f-￿#]/.test(url)) return false;
    var rest = url.slice(7), end = rest.search(/[/?]/);
    var authority = end < 0 ? rest : rest.slice(0, end);
    if (authority.indexOf('@') >= 0) return false;
    var host = authority, colon = authority.lastIndexOf(':');
    if (colon >= 0) {
      host = authority.slice(0, colon);
      var port = authority.slice(colon + 1);
      if (!/^\d{1,5}$/.test(port) || Number(port) < 1 || Number(port) > 65535) return false;
    }
    return isHost(host);
  }
  function isIntIn(v, lo, hi) { return typeof v === 'number' && Math.floor(v) === v && v >= lo && v <= hi; }
  function unset(v) { return v === undefined || v === null || v === ''; }

  // The form fields for an input of `type` over `transport`, in order.
  function inputFields(type, transport) {
    var listens = inputTransport(type, transport) !== 'tcp_connect';
    var f = ['id', 'name'];
    if (type === 'pingstation_cat021') f.push('transport');
    f.push(listens ? 'bind' : 'host', 'port');
    if (listens) f.push('peer');
    if (type === 'pingstation_cat021') f.push('edition', 'sac', 'sic');
    if (isPingStation(type)) f.push('status_url', 'status_interval_s', 'time_mode', 'capture');
    f.push('enabled');
    return f;
  }

  // {ok, errors: {field: message}} for a definition as the form sends it
  // (numbers as numbers; an optional text left empty is unset).
  function validateInputDef(def) {
    var errors = {};
    def = def || {};
    var id = def.id;
    if (unset(id)) errors.id = 'id is required';
    else if (typeof id !== 'string' || !/^[a-z0-9-]{1,32}$/.test(id)) errors.id = 'id must be 1 to 32 characters of a-z, 0-9 and -';
    else if (RESERVED_IDS.indexOf(id) >= 0) errors.id = "id '" + id + "' is reserved";
    if (unset(def.type)) errors.type = 'type is required';
    else if (INPUT_TYPES.indexOf(def.type) < 0) errors.type = 'type must be one of ' + INPUT_TYPES.join(', ');
    if (def.type === 'pingstation_cat021' && unset(def.transport))
      errors.transport = 'transport is required for pingstation_cat021 (udp, tcp_connect or tcp_listen)';
    else if (def.type === 'pingstation_cat021' && TRANSPORTS.indexOf(def.transport) < 0)
      errors.transport = 'transport must be udp, tcp_connect or tcp_listen';
    var fields = inputFields(def.type, def.transport);
    var has = function (k) { return fields.indexOf(k) >= 0; };
    var name = def.name;
    if (!unset(name) && (typeof name !== 'string' || /[\x00-\x1f\x7f]/.test(name) || Array.from(name).length > 64))
      errors.name = 'name must be at most 64 printable characters';
    if (def.port === undefined || def.port === null) errors.port = 'port is required';
    else if (!isIntIn(def.port, 1, 65535)) errors.port = 'port must be an integer from 1 to 65535';
    if (has('bind') && !unset(def.bind) && !isIpv4(def.bind)) errors.bind = 'bind must be an IPv4 address';
    if (has('peer') && !unset(def.peer) && !isIpv4(def.peer)) errors.peer = 'peer must be an IPv4 address';
    if (has('host')) {
      if (unset(def.host)) errors.host = 'host is required';
      else if (!isHost(def.host)) errors.host = 'host must be an IPv4 address or a hostname of at most 253 characters';
    }
    if (has('edition') && !unset(def.edition) && EDITIONS.indexOf(def.edition) < 0)
      errors.edition = 'edition must be one of ' + EDITIONS.join(', ');
    ['sac', 'sic'].forEach(function (k) {
      if (has(k) && !unset(def[k]) && !isIntIn(def[k], 0, 255)) errors[k] = k + ' must be null or an integer from 0 to 255';
    });
    if (has('status_url') && !unset(def.status_url) && !(typeof def.status_url === 'string' && isHttpUrl(def.status_url)))
      errors.status_url = 'status_url must be an http:// URL without userinfo, with a port of 1 to 65535';
    if (has('status_interval_s') && def.status_interval_s !== undefined && !isIntIn(def.status_interval_s, 5, 3600))
      errors.status_interval_s = 'status_interval_s must be an integer from 5 to 3600';
    if (has('time_mode') && def.time_mode !== undefined && def.time_mode !== 'utc' && def.time_mode !== 'gps')
      errors.time_mode = 'time_mode must be utc or gps';
    ['capture', 'enabled'].forEach(function (k) {
      if (has(k) && def[k] !== undefined && typeof def[k] !== 'boolean') errors[k] = k + ' must be true or false';
    });
    return { ok: Object.keys(errors).length === 0, errors: errors };
  }

  // The card's status dot: grey disabled; red disconnected or the status
  // poll failed 3 times in a row; amber connected but no data for 30 s
  // (skycued's `receiving`); else green. The state carries all of it, so
  // `nowMs` is not needed today.
  function inputDot(input, nowMs) {
    var s = (input && input.state) || {};
    if (!input || !input.enabled) return 'grey';
    if (!s.connected || (s.status_fail_streak || 0) >= 3) return 'red';
    if (!s.receiving) return 'amber';
    return 'green';
  }

  // Per-input observations and messages per second between two stats.inputs
  // lists `dtS` seconds apart; null where there is no earlier count, a
  // counter went down (the input was removed and added again) or dtS <= 0.
  function inputRates(prevLines, curLines, dtS) {
    var prev = {}, out = {};
    (prevLines || []).forEach(function (l) { prev[l.id] = l; });
    (curLines || []).forEach(function (l) {
      var p = prev[l.id];
      var ok = p && dtS > 0 && l.observations >= p.observations && l.messages >= p.messages;
      out[l.id] = ok ? { obs_s: (l.observations - p.observations) / dtS, msg_s: (l.messages - p.messages) / dtS } :
        { obs_s: null, msg_s: null };
    });
    return out;
  }

  // A device status is stale when it arrived more than 3 status intervals
  // ago (or its time is unreadable); no status is not stale.
  function statusStale(deviceStatus, intervalS, nowMs) {
    if (!deviceStatus) return false;
    var t = Date.parse(deviceStatus.received_at);
    return isNaN(t) || nowMs - t > 3 * intervalS * 1000;
  }

  // Where an input gets its data, for its card.
  function inputEndpoint(def) {
    if (def.type === 'readsb' || def.type === 'dump978') return def.type + ' ' + def.host + ':' + def.port;
    if (!isPingStation(def.type)) return String(def.type);
    var t = inputTransport(def.type, def.transport);
    if (t === 'udp') return 'udp ' + def.bind + ':' + def.port;
    if (t === 'tcp_listen') return 'tcp listen ' + def.bind + ':' + def.port;
    return 'tcp → ' + def.host + ':' + def.port;
  }

  // The card's rejected counter, with the refused-peer count beside it when
  // there is one (data from an address other than the configured peer).
  function rejectedText(state) {
    var st = state || {};
    var text = 'rejected ' + (st.rejected || 0);
    if (st.rejected_peer > 0) text += ' · rejected_peer ' + st.rejected_peer;
    return text;
  }

  // A pingStation's gpsStatus and receiverStatus codes as the ICD names
  // them; a code it does not list stays the number.
  var GPS_STATUS = ['no GPS', 'no lock', '2D fix', '3D fix', 'DGPS'];
  var RECEIVER_STATUS = ['normal', 'comm errors', 'not transmitting'];
  function gpsStatusText(code) { return GPS_STATUS[code] || String(code); }
  function receiverStatusText(code) { return RECEIVER_STATUS[code] || String(code); }

  // The header's `inputs n/m`: enabled inputs connected / enabled inputs.
  function inputsSummary(inputs) {
    var out = { connected: 0, enabled: 0 };
    (inputs || []).forEach(function (i) {
      if (!i.enabled) return;
      out.enabled++;
      if (i.state && i.state.connected) out.connected++;
    });
    return out;
  }

  // ---- FIS-B weather text (spec §3.2-§3.3) ----

  // Flight category from the standard thresholds: ceiling (lowest BKN/OVC/VV
  // layer, ft) and prevailing visibility (statute miles); the worse wins.
  var FLIGHT_CATEGORIES = ['VFR', 'MVFR', 'IFR', 'LIFR', 'unknown'];
  var METRES_PER_SM = 1609.344;

  // A report's whitespace tokens, the trailing '=' removed, up to (not
  // including) the remarks.
  function reportBody(text) {
    var out = [];
    var toks = String(text || '').split(/\s+/);
    for (var i = 0; i < toks.length; i++) {
      var t = toks[i].replace(/=+$/, '');
      if (t === 'RMK') break;
      if (t) out.push(t);
    }
    return out;
  }

  // The first visibility group, in statute miles: 'P6SM' (6), 'M1/4SM' (just
  // under 1/4), '3/4SM', mixed '1 1/2SM', a 4-digit metric group (metres;
  // 9999 is 10 km or more), CAVOK (10 km or more). '////SM' is null.
  function visibilitySm(tokens) {
    for (var i = 0; i < tokens.length; i++) {
      var t = tokens[i], m;
      if (t === 'CAVOK') return 9999 / METRES_PER_SM;
      if (/^\d{4}$/.test(t)) return Number(t) / METRES_PER_SM;
      if (/^\/+SM$/.test(t)) return null;
      m = /^([PM])?(?:(\d+)|(\d+)\/(\d+))SM$/.exec(t);
      if (!m) continue;
      if (m[4] !== undefined && Number(m[4]) === 0) return null;   // '1/0SM'
      var v = m[2] !== undefined ? Number(m[2]) : Number(m[3]) / Number(m[4]);
      if (m[2] === undefined && i > 0 && /^\d$/.test(tokens[i - 1])) v += Number(tokens[i - 1]);
      return m[1] === 'M' ? v - 0.01 : v;
    }
    return null;
  }

  // The lowest BKN/OVC/VV height in ft; Infinity when there are sky groups
  // (or CAVOK) but no ceiling; null when there is no sky group or a ceiling
  // layer's height is '///'.
  function ceilingFt(tokens) {
    var sky = false, ceil = Infinity;
    for (var i = 0; i < tokens.length; i++) {
      var t = tokens[i];
      if (/^(CLR|SKC|NSC|NCD|CAVOK)$/.test(t)) { sky = true; continue; }
      var m = /^(FEW|SCT|BKN|OVC|VV)(\d{3}|\/\/\/)(CB|TCU|\/\/\/)?$/.exec(t);
      if (!m) continue;
      sky = true;
      if (m[1] === 'FEW' || m[1] === 'SCT') continue;
      if (m[2] === '///') return null;
      ceil = Math.min(ceil, Number(m[2]) * 100);
    }
    return sky ? ceil : null;
  }

  function flightCategory(visSm, ceilFt) {
    if (visSm === null || visSm === undefined || ceilFt === null || ceilFt === undefined) return 'unknown';
    if (ceilFt < 500 || visSm < 1) return 'LIFR';
    if (ceilFt < 1000 || visSm < 3) return 'IFR';
    if (ceilFt <= 3000 || visSm <= 5) return 'MVFR';
    return 'VFR';
  }

  // The first wind group (knots): dddssKT, dddssGggKT, VRBssKT, 00000KT.
  function windGroup(tokens) {
    for (var i = 0; i < tokens.length; i++) {
      var m = /^(\d{3}|VRB)(\d{2,3})(?:G(\d{2,3}))?KT$/.exec(tokens[i]);
      if (!m) continue;
      var kt = Number(m[2]);
      var variable = m[1] === 'VRB', calm = !variable && Number(m[1]) === 0 && kt === 0;
      return {
        dir: variable || calm ? null : Number(m[1]), kt: kt,
        gust: m[3] !== undefined ? Number(m[3]) : null, calm: calm, variable: variable
      };
    }
    return null;
  }

  function weatherSummary(station, groups) {
    var vis = visibilitySm(groups), ceil = ceilingFt(groups);
    return { station: station, category: flightCategory(vis, ceil), visSm: vis, ceilFt: ceil, wind: windGroup(groups) };
  }

  // A METAR or SPECI: its groups are searched only after the ddhhmmZ token
  // and before any trend (BECMG, TEMPO, NOSIG; reportBody already stops at RMK).
  var METAR_TREND = /^(BECMG|TEMPO|NOSIG)$/;
  function parseMetar(text) {
    var toks = reportBody(text);
    if (toks[0] !== 'METAR' && toks[0] !== 'SPECI') return null;
    var i = 1;
    if (toks[i] === 'COR') i++;
    var station = toks[i] || null;
    var t = i + 1;
    while (t < toks.length && !/^\d{6}Z$/.test(toks[t])) t++;
    var groups = t < toks.length ? toks.slice(t + 1) : toks.slice(i + 1);
    for (var g = 0; g < groups.length; g++) {
      if (METAR_TREND.test(groups[g])) { groups = groups.slice(0, g); break; }
    }
    return weatherSummary(station, groups);
  }

  var TAF_HEAD = /^(TAF|TAF\.AMD|AMD|COR)$/;
  var TAF_CHANGE = /^(FM\d{6}|TEMPO|BECMG|PROB\d\d)$/;
  function tafStart(toks) {
    var i = 0;
    while (i < toks.length && TAF_HEAD.test(toks[i])) i++;
    return i;
  }

  // A TAF's first group: the tokens after the valid period (dddd/dddd), up
  // to the first change group.
  function tafFirstGroup(text) {
    var toks = reportBody(text);
    var i = tafStart(toks) + 1;
    if (i < toks.length && /^\d{6}Z$/.test(toks[i])) i++;
    if (!(i < toks.length && /^\d{4}\/\d{4}$/.test(toks[i]))) return [];
    var out = [];
    for (i++; i < toks.length && !TAF_CHANGE.test(toks[i]); i++) out.push(toks[i]);
    return out;
  }

  // A TAF's station and first-group summary; null for text that does not
  // start with TAF, TAF.AMD, AMD or COR.
  function parseTafFirst(text) {
    var toks = reportBody(text);
    var s = tafStart(toks);
    if (s === 0) return null;
    return weatherSummary(toks[s] || null, tafFirstGroup(text));
  }

  // The daemon's winds-aloft record: 'WINDS <stn> <ddhhmm>Z FT <level>...'
  // then a line of values matched to the levels by order. Values: 'ddss',
  // 'ddss+tt' / 'ddss-tt', or 'ddsstt' (sign implied negative); '9900' is
  // light and variable; a direction code of 51..86 means speed >= 100 kt.
  // Any other direction code (00, 37..50, 87..98, or 99 with a speed) makes
  // the record null.
  var WINDS_LEVELS_FT = [3000, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000];
  function parseWinds(text) {
    var lines = String(text || '').split('\n');
    var head = lines[0].trim().split(/\s+/);
    var vals = lines.slice(1).join(' ').trim().split(/\s+/).filter(Boolean);
    var tm = /^(\d{6})Z$/.exec(head[2] || '');
    if (head[0] !== 'WINDS' || !head[1] || !tm || head[3] !== 'FT') return null;
    var fts = head.slice(4);
    if (!fts.length || fts.length !== vals.length) return null;
    var levels = [];
    for (var i = 0; i < fts.length; i++) {
      var m = /^(\d{2})(\d{2})([+-]\d{2}|\d{2})?$/.exec(vals[i]);
      if (!/^\d+$/.test(fts[i]) || !m) return null;
      var d = Number(m[1]), s = Number(m[2]);
      var temp = m[3] === undefined ? null : (/^[+-]/.test(m[3]) ? Number(m[3]) : -Number(m[3]));
      var lv = d === 99 && s === 0;
      if (!lv && !(d >= 1 && d <= 36) && !(d >= 51 && d <= 86)) return null;
      if (!lv && d >= 51) { d -= 50; s += 100; }
      levels.push({ ft: Number(fts[i]), dir: lv ? null : d * 10, kt: lv ? 0 : s, temp: temp, lv: lv });
    }
    return { station: head[1], valid: tm[1], levels: levels };
  }

  function windsLevel(parsed, ft) {
    if (!parsed) return null;
    for (var i = 0; i < parsed.levels.length; i++) if (parsed.levels[i].ft === ft) return parsed.levels[i];
    return null;
  }

  // records [{period_h, valid (ISO)}]: the period_h whose valid time is
  // nearest nowMs (the first on a tie).
  function nearestWindsPeriod(records, nowMs) {
    var best = null, bestD = Infinity;
    (records || []).forEach(function (r) {
      var d = Math.abs(Date.parse(r.valid) - nowMs);
      if (d < bestD) { bestD = d; best = r.period_h; }
    });
    return best;
  }

  // The winds pickers' options: the period_h values present, and the levels
  // (ft) of that period's parsed records, both sorted ascending. parse
  // (optional) maps a record to its parseWinds result (the page's cache).
  function windsPeriods(records) {
    var seen = {};
    (records || []).forEach(function (r) { if (typeof r.period_h === 'number') seen[r.period_h] = true; });
    return Object.keys(seen).map(Number).sort(function (a, b) { return a - b; });
  }
  function windsLevelsFor(records, period, parse) {
    var seen = {};
    (records || []).forEach(function (r) {
      if (r.period_h !== period) return;
      var p = parse ? parse(r) : parseWinds(r.text);
      if (p) p.levels.forEach(function (l) { seen[l.ft] = true; });
    });
    return Object.keys(seen).map(Number).sort(function (a, b) { return a - b; });
  }

  // A PIREP: its station, UUA (urgent) or UA, and the /OV group when it is a
  // location id optionally followed by rrrddd (radial, nm); routes and
  // free-text positions ('SJX-GRB', '4 W CVG') give ov null. The radial is
  // magnetic as reported; pirepPosition applies it as a true bearing.
  function parsePirep(text) {
    var s = String(text || '');
    var toks = s.split(/\s+/).filter(Boolean);
    var station = (toks[0] === 'PIREP' ? toks[1] : toks[0]) || null;
    var headToks = s.split('/')[0].split(/\s+/);
    var ov = null;
    var g = /\/OV([^/]*)/.exec(s);
    var m = g && /^([A-Z0-9]{3,4})(?:\s?(\d{3})(\d{3}))?$/.exec(g[1].trim());
    if (m) {
      ov = { id: m[1], radial: m[2] !== undefined ? Number(m[2]) : null, nm: m[3] !== undefined ? Number(m[3]) : null };
    }
    return { station: station, urgent: headToks.indexOf('UUA') >= 0, ov: ov };
  }

  // Station resolution (spec §3.2), first hit wins: METAR/TAF ids verbatim;
  // winds ids and 3-character PIREP /OV ids as K, P, C + id, then verbatim.
  var WINDS_PREFIXES = ['K', 'P', 'C'];
  function stationCandidates(kind, id) {
    var prefixed = kind === 'winds' || (kind === 'pirep' && String(id).length === 3);
    if (!prefixed) return [id];
    return WINDS_PREFIXES.map(function (p) { return p + id; }).concat([id]);
  }

  function resolveStation(kind, id, index) {
    var c = stationCandidates(kind, id);
    for (var i = 0; i < c.length; i++) {
      if (index && Object.prototype.hasOwnProperty.call(index, c[i])) return index[c[i]];
    }
    return null;
  }

  // The point nm along a great circle from lat/lon on the initial bearing,
  // on the same sphere as rangeNm. The bearing is true (no magnetic variation).
  function destinationPoint(lat, lon, brgDeg, nm) {
    var d = nm * METRES_PER_NM / EARTH_RADIUS_M, b = brgDeg * DEG, p1 = lat * DEG, l1 = lon * DEG;
    var sp2 = Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(b);
    var p2 = Math.asin(Math.max(-1, Math.min(1, sp2)));
    var l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * sp2);
    var lonOut = ((l2 / DEG + 540) % 360) - 180;
    return [p2 / DEG, lonOut];
  }

  // A PIREP's position: its /OV id resolved, then moved rrr/ddd when given.
  // Approximation: the magnetic radial is used as a true bearing (magnetic
  // variation ignored), so the point can be off by about 1-2 nm at 30 nm.
  function pirepPosition(parsed, index) {
    if (!parsed || !parsed.ov) return null;
    var pos = resolveStation('pirep', parsed.ov.id, index);
    if (!pos) return null;
    if (parsed.ov.radial === null || parsed.ov.nm === null) return pos;
    return destinationPoint(pos[0], pos[1], parsed.ov.radial, parsed.ov.nm);
  }

  // A wind barb's parts: speed rounded to the nearest 5 kt, then 50 kt
  // pennants, 10 kt full barbs and a 5 kt half barb; calm when it rounds to 0.
  function barbParts(kt) {
    var r = Math.round(kt / 5) * 5;
    if (!(r > 0)) return { calm: true, pennants: 0, full: 0, half: 0 };
    var pennants = Math.floor(r / 50), rem = r - pennants * 50;
    return { calm: false, pennants: pennants, full: Math.floor(rem / 10), half: rem % 10 ? 1 : 0 };
  }

  // The 24-bit aircraft address allocation to States, ICAO Annex 10 Vol. III,
  // Appendix to Chapter 9: a table of facts transcribed by hand. Sorted by
  // first address, [first, last, name]; the ICAO-held blocks are left out.
  var ICAO_BLOCKS = [
    [0x004000, 0x0043FF, 'Zimbabwe'],
    [0x006000, 0x006FFF, 'Mozambique'],
    [0x008000, 0x00FFFF, 'South Africa'],
    [0x010000, 0x017FFF, 'Egypt'],
    [0x018000, 0x01FFFF, 'Libya'],
    [0x020000, 0x027FFF, 'Morocco'],
    [0x028000, 0x02FFFF, 'Tunisia'],
    [0x030000, 0x0303FF, 'Botswana'],
    [0x032000, 0x032FFF, 'Burundi'],
    [0x034000, 0x034FFF, 'Cameroon'],
    [0x035000, 0x0353FF, 'Comoros'],
    [0x036000, 0x036FFF, 'Congo'],
    [0x038000, 0x038FFF, 'Ivory Coast'],
    [0x03E000, 0x03EFFF, 'Gabon'],
    [0x040000, 0x040FFF, 'Ethiopia'],
    [0x042000, 0x042FFF, 'Equatorial Guinea'],
    [0x044000, 0x044FFF, 'Ghana'],
    [0x046000, 0x046FFF, 'Guinea'],
    [0x048000, 0x0483FF, 'Guinea-Bissau'],
    [0x04A000, 0x04A3FF, 'Lesotho'],
    [0x04C000, 0x04CFFF, 'Kenya'],
    [0x050000, 0x050FFF, 'Liberia'],
    [0x054000, 0x054FFF, 'Madagascar'],
    [0x058000, 0x058FFF, 'Malawi'],
    [0x05A000, 0x05A3FF, 'Maldives'],
    [0x05C000, 0x05CFFF, 'Mali'],
    [0x05E000, 0x05E3FF, 'Mauritania'],
    [0x060000, 0x0603FF, 'Mauritius'],
    [0x062000, 0x062FFF, 'Niger'],
    [0x064000, 0x064FFF, 'Nigeria'],
    [0x068000, 0x068FFF, 'Uganda'],
    [0x06A000, 0x06A3FF, 'Qatar'],
    [0x06C000, 0x06CFFF, 'Central African Republic'],
    [0x06E000, 0x06EFFF, 'Rwanda'],
    [0x070000, 0x070FFF, 'Senegal'],
    [0x074000, 0x0743FF, 'Seychelles'],
    [0x076000, 0x0763FF, 'Sierra Leone'],
    [0x078000, 0x078FFF, 'Somalia'],
    [0x07A000, 0x07A3FF, 'Eswatini'],
    [0x07C000, 0x07CFFF, 'Sudan'],
    [0x080000, 0x080FFF, 'Tanzania'],
    [0x084000, 0x084FFF, 'Chad'],
    [0x088000, 0x088FFF, 'Togo'],
    [0x08A000, 0x08AFFF, 'Zambia'],
    [0x08C000, 0x08CFFF, 'DR Congo'],
    [0x090000, 0x090FFF, 'Angola'],
    [0x094000, 0x0943FF, 'Benin'],
    [0x096000, 0x0963FF, 'Cape Verde'],
    [0x098000, 0x0983FF, 'Djibouti'],
    [0x09A000, 0x09AFFF, 'Gambia'],
    [0x09C000, 0x09CFFF, 'Burkina Faso'],
    [0x09E000, 0x09E3FF, 'Sao Tome and Principe'],
    [0x0A0000, 0x0A7FFF, 'Algeria'],
    [0x0A8000, 0x0A8FFF, 'Bahamas'],
    [0x0AA000, 0x0AA3FF, 'Barbados'],
    [0x0AB000, 0x0AB3FF, 'Belize'],
    [0x0AC000, 0x0ACFFF, 'Colombia'],
    [0x0AE000, 0x0AEFFF, 'Costa Rica'],
    [0x0B0000, 0x0B0FFF, 'Cuba'],
    [0x0B2000, 0x0B2FFF, 'El Salvador'],
    [0x0B4000, 0x0B4FFF, 'Guatemala'],
    [0x0B6000, 0x0B6FFF, 'Guyana'],
    [0x0B8000, 0x0B8FFF, 'Haiti'],
    [0x0BA000, 0x0BAFFF, 'Honduras'],
    [0x0BC000, 0x0BC3FF, 'Saint Vincent and the Grenadines'],
    [0x0BE000, 0x0BEFFF, 'Jamaica'],
    [0x0C0000, 0x0C0FFF, 'Nicaragua'],
    [0x0C2000, 0x0C2FFF, 'Panama'],
    [0x0C4000, 0x0C4FFF, 'Dominican Republic'],
    [0x0C6000, 0x0C6FFF, 'Trinidad and Tobago'],
    [0x0C8000, 0x0C8FFF, 'Suriname'],
    [0x0CA000, 0x0CA3FF, 'Antigua and Barbuda'],
    [0x0CC000, 0x0CC3FF, 'Grenada'],
    [0x0D0000, 0x0D7FFF, 'Mexico'],
    [0x0D8000, 0x0DFFFF, 'Venezuela'],
    [0x100000, 0x1FFFFF, 'Russia'],
    [0x201000, 0x2013FF, 'Namibia'],
    [0x202000, 0x2023FF, 'Eritrea'],
    [0x300000, 0x33FFFF, 'Italy'],
    [0x340000, 0x37FFFF, 'Spain'],
    [0x380000, 0x3BFFFF, 'France'],
    [0x3C0000, 0x3FFFFF, 'Germany'],
    [0x400000, 0x43FFFF, 'United Kingdom'],
    [0x440000, 0x447FFF, 'Austria'],
    [0x448000, 0x44FFFF, 'Belgium'],
    [0x450000, 0x457FFF, 'Bulgaria'],
    [0x458000, 0x45FFFF, 'Denmark'],
    [0x460000, 0x467FFF, 'Finland'],
    [0x468000, 0x46FFFF, 'Greece'],
    [0x470000, 0x477FFF, 'Hungary'],
    [0x478000, 0x47FFFF, 'Norway'],
    [0x480000, 0x487FFF, 'Netherlands'],
    [0x488000, 0x48FFFF, 'Poland'],
    [0x490000, 0x497FFF, 'Portugal'],
    [0x498000, 0x49FFFF, 'Czechia'],
    [0x4A0000, 0x4A7FFF, 'Romania'],
    [0x4A8000, 0x4AFFFF, 'Sweden'],
    [0x4B0000, 0x4B7FFF, 'Switzerland'],
    [0x4B8000, 0x4BFFFF, 'Turkey'],
    [0x4C0000, 0x4C7FFF, 'Serbia'],
    [0x4C8000, 0x4C83FF, 'Cyprus'],
    [0x4CA000, 0x4CAFFF, 'Ireland'],
    [0x4CC000, 0x4CCFFF, 'Iceland'],
    [0x4D0000, 0x4D03FF, 'Luxembourg'],
    [0x4D2000, 0x4D23FF, 'Malta'],
    [0x4D4000, 0x4D43FF, 'Monaco'],
    [0x500000, 0x5003FF, 'San Marino'],
    [0x501000, 0x5013FF, 'Albania'],
    [0x501C00, 0x501FFF, 'Croatia'],
    [0x502C00, 0x502FFF, 'Latvia'],
    [0x503C00, 0x503FFF, 'Lithuania'],
    [0x504C00, 0x504FFF, 'Moldova'],
    [0x505C00, 0x505FFF, 'Slovakia'],
    [0x506C00, 0x506FFF, 'Slovenia'],
    [0x507C00, 0x507FFF, 'Uzbekistan'],
    [0x508000, 0x50FFFF, 'Ukraine'],
    [0x510000, 0x5103FF, 'Belarus'],
    [0x511000, 0x5113FF, 'Estonia'],
    [0x512000, 0x5123FF, 'North Macedonia'],
    [0x513000, 0x5133FF, 'Bosnia and Herzegovina'],
    [0x514000, 0x5143FF, 'Georgia'],
    [0x515000, 0x5153FF, 'Tajikistan'],
    [0x516000, 0x5163FF, 'Montenegro'],
    [0x600000, 0x6003FF, 'Armenia'],
    [0x600800, 0x600BFF, 'Azerbaijan'],
    [0x601000, 0x6013FF, 'Kyrgyzstan'],
    [0x601800, 0x601BFF, 'Turkmenistan'],
    [0x680000, 0x6803FF, 'Bhutan'],
    [0x681000, 0x6813FF, 'Micronesia'],
    [0x682000, 0x6823FF, 'Mongolia'],
    [0x683000, 0x6833FF, 'Kazakhstan'],
    [0x684000, 0x6843FF, 'Palau'],
    [0x700000, 0x700FFF, 'Afghanistan'],
    [0x702000, 0x702FFF, 'Bangladesh'],
    [0x704000, 0x704FFF, 'Myanmar'],
    [0x706000, 0x706FFF, 'Kuwait'],
    [0x708000, 0x708FFF, 'Laos'],
    [0x70A000, 0x70AFFF, 'Nepal'],
    [0x70C000, 0x70C3FF, 'Oman'],
    [0x70E000, 0x70EFFF, 'Cambodia'],
    [0x710000, 0x717FFF, 'Saudi Arabia'],
    [0x718000, 0x71FFFF, 'South Korea'],
    [0x720000, 0x727FFF, 'North Korea'],
    [0x728000, 0x72FFFF, 'Iraq'],
    [0x730000, 0x737FFF, 'Iran'],
    [0x738000, 0x73FFFF, 'Israel'],
    [0x740000, 0x747FFF, 'Jordan'],
    [0x748000, 0x74FFFF, 'Lebanon'],
    [0x750000, 0x757FFF, 'Malaysia'],
    [0x758000, 0x75FFFF, 'Philippines'],
    [0x760000, 0x767FFF, 'Pakistan'],
    [0x768000, 0x76FFFF, 'Singapore'],
    [0x770000, 0x777FFF, 'Sri Lanka'],
    [0x778000, 0x77FFFF, 'Syria'],
    [0x780000, 0x7BFFFF, 'China'],
    [0x7C0000, 0x7FFFFF, 'Australia'],
    [0x800000, 0x83FFFF, 'India'],
    [0x840000, 0x87FFFF, 'Japan'],
    [0x880000, 0x887FFF, 'Thailand'],
    [0x888000, 0x88FFFF, 'Vietnam'],
    [0x890000, 0x890FFF, 'Yemen'],
    [0x894000, 0x894FFF, 'Bahrain'],
    [0x895000, 0x8953FF, 'Brunei'],
    [0x896000, 0x896FFF, 'United Arab Emirates'],
    [0x897000, 0x8973FF, 'Solomon Islands'],
    [0x898000, 0x898FFF, 'Papua New Guinea'],
    [0x899000, 0x8993FF, 'Taiwan'],
    [0x8A0000, 0x8A7FFF, 'Indonesia'],
    [0x900000, 0x9003FF, 'Marshall Islands'],
    [0x901000, 0x9013FF, 'Cook Islands'],
    [0x902000, 0x9023FF, 'Samoa'],
    [0xA00000, 0xAFFFFF, 'United States'],
    [0xC00000, 0xC3FFFF, 'Canada'],
    [0xC80000, 0xC87FFF, 'New Zealand'],
    [0xC88000, 0xC88FFF, 'Fiji'],
    [0xC8A000, 0xC8A3FF, 'Nauru'],
    [0xC8C000, 0xC8C3FF, 'Saint Lucia'],
    [0xC8D000, 0xC8D3FF, 'Tonga'],
    [0xC8E000, 0xC8E3FF, 'Kiribati'],
    [0xC90000, 0xC903FF, 'Vanuatu'],
    [0xE00000, 0xE3FFFF, 'Argentina'],
    [0xE40000, 0xE7FFFF, 'Brazil'],
    [0xE80000, 0xE80FFF, 'Chile'],
    [0xE84000, 0xE84FFF, 'Ecuador'],
    [0xE88000, 0xE88FFF, 'Paraguay'],
    [0xE8C000, 0xE8CFFF, 'Peru'],
    [0xE90000, 0xE90FFF, 'Uruguay'],
    [0xE94000, 0xE94FFF, 'Bolivia']
  ];

  // Blocks States reserve for military aircraft, sorted [first, last]. This
  // is a widely published heuristic list, not an ICAO rule: it misses
  // military aircraft outside these ranges and may include some civil ones.
  var MILITARY_BLOCKS = [
    [0x010070, 0x01008F], // Egypt
    [0x0A4000, 0x0A4FFF], // Algeria
    [0x33FF00, 0x33FFFF], // Italy
    [0x350000, 0x37FFFF], // Spain
    [0x3AA000, 0x3AFFFF], // France
    [0x3B7000, 0x3BFFFF], // France
    [0x3EA000, 0x3EBFFF], // Germany
    [0x3F4000, 0x3FBFFF], // Germany
    [0x400000, 0x40003F], // United Kingdom
    [0x43C000, 0x43CFFF], // United Kingdom
    [0x444000, 0x446FFF], // Austria
    [0x44F000, 0x44FFFF], // Belgium
    [0x457000, 0x457FFF], // Bulgaria
    [0x45F400, 0x45F4FF], // Denmark
    [0x468000, 0x4683FF], // Greece
    [0x473C00, 0x473C0F], // Hungary
    [0x478100, 0x4781FF], // Norway
    [0x480000, 0x480FFF], // Netherlands
    [0x48D800, 0x48D87F], // Poland
    [0x497C00, 0x497CFF], // Portugal
    [0x498420, 0x49842F], // Czechia
    [0x4B7000, 0x4B7FFF], // Switzerland
    [0x4B8200, 0x4B82FF], // Turkey
    [0x506F00, 0x506FFF], // Slovenia
    [0x70C070, 0x70C07F], // Oman
    [0x710258, 0x71028F], // Saudi Arabia
    [0x710380, 0x71039F], // Saudi Arabia
    [0x738A00, 0x738AFF], // Israel
    [0x7C822E, 0x7C84FF], // Australia
    [0x7C8800, 0x7C88FF], // Australia
    [0x7C9000, 0x7CBFFF], // Australia
    [0x7D0000, 0x7FFFFF], // Australia
    [0x800200, 0x8002FF], // India
    [0xADF7C8, 0xAFFFFF], // United States
    [0xC20000, 0xC3FFFF], // Canada
    [0xE40000, 0xE41FFF], // Brazil
    [0xE80600, 0xE806FF]  // Chile
  ];

  var ICAO_COUNTRIES = (function () {
    var seen = {}, out = [];
    for (var i = 0; i < ICAO_BLOCKS.length; i++) {
      var n = ICAO_BLOCKS[i][2];
      if (!seen[n]) { seen[n] = true; out.push(n); }
    }
    return out.sort();
  })();

  // An ICAO address as an integer: exactly 6 hex digits, anything else is
  // null. A leading '~' marks a TIS-B track-file id, not an ICAO address, so
  // it is null too (no country, not military).
  function parseHex(hex) {
    if (typeof hex !== 'string') return null;
    return /^[0-9a-f]{6}$/i.test(hex) ? parseInt(hex, 16) : null;
  }

  // The block holding address v in a sorted [first, last, ...] table, or null.
  function findBlock(table, v) {
    var lo = 0, hi = table.length - 1;
    while (lo <= hi) {
      var mid = (lo + hi) >> 1;
      if (v < table[mid][0]) hi = mid - 1;
      else if (v > table[mid][1]) lo = mid + 1;
      else return table[mid];
    }
    return null;
  }

  function icaoCountry(hex) {
    var v = parseHex(hex);
    if (v === null) return null;
    var b = findBlock(ICAO_BLOCKS, v);
    return b ? b[2] : null;
  }

  function icaoMilitary(hex) {
    var v = parseHex(hex);
    return v !== null && findBlock(MILITARY_BLOCKS, v) !== null;
  }

  // Well-known military callsign prefixes; a callsign must have at least one
  // character after the prefix.
  var MILITARY_CALLSIGN_PREFIXES = ['RCH', 'EVAC', 'DUKE', 'KING', 'NAVY', 'CNV', 'RRR', 'ASCOT',
    'GAF', 'FAF', 'BAF', 'IAM', 'CFC', 'NATO'];

  // What a callsign looks like: 'military' (a known military prefix),
  // 'airline' (3-letter designator + 1-4 alphanumerics with a digit),
  // 'registration' (an N-number, or 4-6 letters such as GABCD), else 'other';
  // null for no callsign.
  function callsignKind(callsign) {
    var s = typeof callsign === 'string' ? callsign.trim().toUpperCase() : '';
    if (!s) return null;
    for (var i = 0; i < MILITARY_CALLSIGN_PREFIXES.length; i++) {
      var p = MILITARY_CALLSIGN_PREFIXES[i];
      if (s.length > p.length && s.lastIndexOf(p, 0) === 0) return 'military';
    }
    if (/^[A-Z]{3}[A-Z0-9]{1,4}$/.test(s) && /\d/.test(s.slice(3))) return 'airline';
    if (/^N[1-9]\d{0,4}[A-Z]{0,2}$/.test(s) || /^[A-Z]{4,6}$/.test(s)) return 'registration';
    return 'other';
  }

  var CLASSES = ['airline', 'private', 'military', 'unknown'];
  var CATEGORY_LABELS = { A1: 'light', A2: 'small', A3: 'large', A4: 'high vortex', A5: 'heavy',
    A6: 'high performance', A7: 'rotorcraft' };

  // The address rules (country, military block) apply only to a track whose
  // addr_type is 'icao' or absent: a non-ICAO address (TIS-B track file,
  // non_icao, other) carries no allocation meaning.
  function isIcaoAddressed(t) { return t.addr_type === undefined || t.addr_type === null || t.addr_type === 'icao'; }
  // The country of a track's address (null for non-ICAO addresses).
  function trackCountry(t) { return isIcaoAddressed(t) ? icaoCountry(t.hex) : null; }

  // The estimated class of a track, from its address, callsign, emitter
  // category and registration (spec 3a).
  function trackClass(t) {
    var kind = callsignKind(t.callsign);
    if ((isIcaoAddressed(t) && icaoMilitary(t.hex)) || kind === 'military') return 'military';
    var cat = t.category || '';
    if (kind === 'airline' || (kind !== null && (cat === 'A3' || cat === 'A4' || cat === 'A5'))) return 'airline';
    if (cat === 'A1' || cat === 'A2' || cat === 'A7' || kind === 'registration' ||
        (t.registration && kind === null)) return 'private';
    return 'unknown';
  }

  // An emergency is declared, or the squawk is 7500, 7600 or 7700.
  function isEmergency(t) {
    if (t.emergency && t.emergency !== 'none') return true;
    var sq = t.squawk;
    return sq === '7500' || sq === '7600' || sq === '7700';
  }

  // The View section's settings (spec 3, 6): what the operator narrows the
  // table and map to, kept in this browser only. Numbers are null for "no
  // bound"; decisions are colour classes, classes are trackClass names.
  var VIEW_DECISIONS = ['sent', 'allow_listed', 'out_of_range', 'below_elevation', 'blocked'];
  var VIEW_CATEGORIES = ['A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7'];
  var VIEW_DEFAULTS = Object.freeze({
    search: '', altMin: null, altMax: null, ground: true, gsMin: null, gsMax: null,
    rangeMin: null, rangeMax: null, decisions: Object.freeze(VIEW_DECISIONS.slice()),
    classes: Object.freeze(CLASSES.slice()), emergency: false, category: '', country: '', mapMode: 'hide',
    trailShow: 'all', trailColor: 'decision', trailAhead: 0
  });
  // The Trails group: which aircraft have a trail (every drawn one, only
  // the selected one, none) and what colours it (the decision; the signal
  // or the altitude of each position, trailRunColor) and how many minutes
  // ahead the projection reaches (0: none; projectAhead).
  var TRAIL_SHOWS = ['all', 'selected', 'off'];
  var TRAIL_COLOR_BY = ['decision', 'signal', 'altitude'];
  var TRAIL_AHEAD_MIN = [0, 1, 2, 5];

  function viewNumber(v) {
    if (typeof v === 'string') v = v.trim() === '' ? null : Number(v);
    return typeof v === 'number' && isFinite(v) ? v : null;
  }
  function viewBool(v, dflt) {
    return typeof v === 'boolean' || typeof v === 'number' ? !!v : dflt;
  }
  // The known values of `allowed` that `v` holds, in `allowed`'s order; the
  // default (copied) when v is not an array.
  function viewSet(v, allowed) {
    if (!Array.isArray(v)) return allowed.slice();
    return allowed.filter(function (a) { return v.indexOf(a) >= 0; });
  }

  // Any input (an object, a JSON string, garbage) to a full view; each field
  // falls back to its default on its own. Arrays are fresh copies.
  function normalizeView(obj) {
    if (typeof obj === 'string') {
      try { obj = JSON.parse(obj); } catch (e) { obj = null; }
    }
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) obj = {};
    var cat = obj.category, country = obj.country;
    return {
      search: typeof obj.search === 'string' ? obj.search : '',
      altMin: viewNumber(obj.altMin), altMax: viewNumber(obj.altMax),
      ground: viewBool(obj.ground, true),
      gsMin: viewNumber(obj.gsMin), gsMax: viewNumber(obj.gsMax),
      rangeMin: viewNumber(obj.rangeMin), rangeMax: viewNumber(obj.rangeMax),
      decisions: viewSet(obj.decisions, VIEW_DECISIONS),
      classes: viewSet(obj.classes, CLASSES),
      emergency: viewBool(obj.emergency, false),
      category: cat === 'other' || cat === 'none' || VIEW_CATEGORIES.indexOf(cat) >= 0 ? cat : '',
      country: country === 'other' || ICAO_COUNTRIES.indexOf(country) >= 0 ? country : '',
      mapMode: obj.mapMode === 'dim' ? 'dim' : 'hide',
      trailShow: TRAIL_SHOWS.indexOf(obj.trailShow) >= 0 ? obj.trailShow : VIEW_DEFAULTS.trailShow,
      trailColor: TRAIL_COLOR_BY.indexOf(obj.trailColor) >= 0 ? obj.trailColor : VIEW_DEFAULTS.trailColor,
      trailAhead: TRAIL_AHEAD_MIN.indexOf(viewNumber(obj.trailAhead)) >= 0 ? viewNumber(obj.trailAhead) : VIEW_DEFAULTS.trailAhead
    };
  }
  // Whether the Trails group is at its defaults (Clear resets it; like the
  // map mode it is not counted in the heading badge).
  function viewTrailsDefault(v) {
    return v.trailShow === VIEW_DEFAULTS.trailShow && v.trailColor === VIEW_DEFAULTS.trailColor &&
      v.trailAhead === VIEW_DEFAULTS.trailAhead;
  }

  // Map toolbar, layer set and badges (map chrome spec).
  // The toolbar opens one box at a time; a click on the open box closes it.
  var MAP_BOXES = ['map', 'aircraft', 'layers', 'weather'];
  function mapBoxName(stored) {
    return typeof stored === 'string' && MAP_BOXES.indexOf(stored) >= 0 ? stored : null;
  }
  function mapBoxToggle(open, clicked) {
    return clicked === open ? null : mapBoxName(clicked);
  }
  // The map layer set: which overlays are on. Stored as a JSON object keyed
  // by these ids; unknown keys are ignored.
  var LAYER_IDS = ['regional', 'conus', 'rasters', 'tfr', 'sigmet', 'airmet', 'cwa', 'sua'];
  var LAYER_DEFAULTS = Object.freeze({
    regional: true, conus: true, rasters: false, tfr: false,
    sigmet: false, airmet: false, cwa: false, sua: false
  });
  // The object a layer set arrives as: an object, or a JSON string of one.
  // Anything else (parse failure, array, scalar, null) gives null.
  function layersSource(obj) {
    if (typeof obj === 'string') {
      try { obj = JSON.parse(obj); } catch (e) { return null; }
    }
    return obj && typeof obj === 'object' && !Array.isArray(obj) ? obj : null;
  }
  function normalizeLayers(obj) {
    var src = layersSource(obj);
    var out = {};
    LAYER_IDS.forEach(function (id) {
      out[id] = src && Object.prototype.hasOwnProperty.call(src, id) ? !!src[id] : LAYER_DEFAULTS[id];
    });
    return out;
  }
  function normalizeLayersPartial(obj) {
    var src = layersSource(obj);
    var out = {};
    if (!src) return out;
    LAYER_IDS.forEach(function (id) {
      if (Object.prototype.hasOwnProperty.call(src, id)) out[id] = !!src[id];
    });
    return out;
  }
  function layersOnCount(state) {
    var all = normalizeLayers(state);
    return LAYER_IDS.filter(function (id) { return all[id] === true; }).length;
  }
  // Whether the Weather badge shows a dot: a box that is on but faded, or on
  // and not drawn while FIS-B is present. items: [{on, drawn, faded}].
  function weatherDot(items, fisbPresent) {
    return (items || []).some(function (it) {
      return !!it && ((it.on && it.faded) || (it.on && !it.drawn && !!fisbPresent));
    });
  }

  // Saved views (spec 4): skycue.viewSets is {version: 2, sets:
  // [{name, view, hideFiltered, map?}]}, at most VIEW_SETS_MAX named
  // Display-filter states. The optional map holds the map as saved (style,
  // borders, cities, layers, radarOpacity), each field only when valid; a
  // set saved before version 2 has no map and leaves the map alone. A name
  // is trimmed and cut to VIEW_SET_NAME_MAX; '' is no name.
  var VIEW_SETS_VERSION = 2;
  var VIEW_SETS_MAX = 20;
  var VIEW_SET_NAME_MAX = 40;
  function viewSetName(v) {
    return typeof v === 'string' ? v.trim().slice(0, VIEW_SET_NAME_MAX).trim() : '';
  }
  // A set's map view: undefined when `obj` is not an object, else only the
  // valid fields present (possibly {}).
  function normalizeMapView(obj) {
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return undefined;
    var out = {};
    if (obj.style === 'light' || obj.style === 'dark') out.style = obj.style;
    if (obj.borders !== undefined) out.borders = !!obj.borders;
    if (obj.cities !== undefined) out.cities = !!obj.cities;
    if (obj.layers && typeof obj.layers === 'object' && !Array.isArray(obj.layers)) {
      out.layers = normalizeLayersPartial(obj.layers);
    }
    if (typeof obj.radarOpacity === 'number' && isFinite(obj.radarOpacity) &&
        obj.radarOpacity >= 0.2 && obj.radarOpacity <= 1) out.radarOpacity = obj.radarOpacity;
    return out;
  }
  // Any input (an object, a JSON string, garbage) to a store. Each set's
  // fields fall back on their own; a set without a name, or with the name of
  // an earlier set, is dropped; sets past the cap are dropped.
  function normalizeViewSets(obj) {
    if (typeof obj === 'string') {
      try { obj = JSON.parse(obj); } catch (e) { obj = null; }
    }
    var src = obj && typeof obj === 'object' && Array.isArray(obj.sets) ? obj.sets : [];
    var sets = [], seen = {};
    for (var i = 0; i < src.length && sets.length < VIEW_SETS_MAX; i++) {
      var s = src[i];
      if (!s || typeof s !== 'object') continue;
      var name = viewSetName(s.name);
      if (name === '' || Object.prototype.hasOwnProperty.call(seen, name)) continue;
      seen[name] = true;
      var set = { name: name, view: normalizeView(s.view), hideFiltered: viewBool(s.hideFiltered, false) };
      var m = normalizeMapView(s.map);
      if (m !== undefined) set.map = m;
      sets.push(set);
    }
    return { version: VIEW_SETS_VERSION, sets: sets };
  }
  // Sets saved before the Trails group existed lack its fields; applying
  // one would put Trails back to the defaults. `raw` is the stored store
  // (object or JSON text): each set's view missing one of TRAIL_FIELDS gets
  // it from `view` (the current settings), once. {store (normalised),
  // filled: how many sets were given a field}.
  var TRAIL_FIELDS = ['trailShow', 'trailColor', 'trailAhead'];
  function viewSetsFillTrails(raw, view) {
    if (typeof raw === 'string') {
      try { raw = JSON.parse(raw); } catch (e) { raw = null; }
    }
    var filled = 0, cur = normalizeView(view);
    if (raw && typeof raw === 'object' && Array.isArray(raw.sets)) {
      raw = { version: raw.version, sets: raw.sets.map(function (s) {
        if (!s || typeof s !== 'object') return s;
        var v = s.view && typeof s.view === 'object' && !Array.isArray(s.view) ? s.view : {};
        var missing = TRAIL_FIELDS.filter(function (k) { return !Object.prototype.hasOwnProperty.call(v, k); });
        if (!missing.length) return s;
        var nv = Object.assign({}, v);
        missing.forEach(function (k) { nv[k] = cur[k]; });
        filled++;
        return Object.assign({}, s, { view: nv });
      }) };
    }
    return { store: normalizeViewSets(raw), filled: filled };
  }
  // Whether the map as saved (undefined for an old set) is the current map
  // view: every field the set holds equals the current one. A field the
  // current view lacks (the install cannot honour it) counts as a match, and
  // so does a missing current view.
  function mapViewMatches(stored, current) {
    if (stored === undefined) return true;
    var cur = current && typeof current === 'object' ? current : {};
    var own = Object.prototype.hasOwnProperty;
    var ok = ['style', 'borders', 'cities', 'radarOpacity'].every(function (k) {
      return !own.call(stored, k) || cur[k] === undefined || stored[k] === cur[k];
    });
    if (!ok) return false;
    if (stored.layers && cur.layers && typeof cur.layers === 'object') {
      return LAYER_IDS.every(function (id) {
        return !own.call(stored.layers, id) || cur.layers[id] === undefined || stored.layers[id] === cur.layers[id];
      });
    }
    return true;
  }
  // Whether the current state (a view, the Hide filtered flag and, when
  // given, the map view) is the set's.
  function viewSetMatches(set, view, hideFiltered, mapView) {
    return JSON.stringify(normalizeView(set.view)) === JSON.stringify(normalizeView(view)) &&
      !!set.hideFiltered === !!hideFiltered && mapViewMatches(set.map, mapView);
  }
  // A new store with the set `name` holding this state: a set of the same
  // name is replaced in place, else the set is added at the end. `index` is
  // where it is; {error} when the name is empty or the store is full.
  function putViewSet(store, name, view, hideFiltered, mapView) {
    name = viewSetName(name);
    if (name === '') return { error: 'A filter set needs a name.' };
    var sets = store.sets.slice(), set = { name: name, view: normalizeView(view), hideFiltered: !!hideFiltered };
    var m = normalizeMapView(mapView);
    if (m !== undefined) set.map = m;
    var i = sets.map(function (s) { return s.name; }).indexOf(name);
    if (i < 0 && sets.length >= VIEW_SETS_MAX) return { error: 'At most ' + VIEW_SETS_MAX + ' filter sets: delete one first.' };
    if (i >= 0) sets[i] = set; else sets.push(set);
    return { store: { version: VIEW_SETS_VERSION, sets: sets }, replaced: i >= 0, index: i >= 0 ? i : sets.length - 1 };
  }

  // The UTC calendar date of a time as YYYY-MM-DD (the downloads' file names).
  function utcDate(ms) { return new Date(ms).toISOString().slice(0, 10); }

  // How many controls differ from the default (the heading badge). The
  // altitude band and its ground box count as one; the map mode never counts.
  function viewActiveCount(v) {
    var n = 0;
    if (String(v.search || '').trim() !== '') n++;
    if (v.altMin !== null || v.altMax !== null || !v.ground) n++;
    if (v.gsMin !== null || v.gsMax !== null) n++;
    if (v.rangeMin !== null || v.rangeMax !== null) n++;
    if (v.decisions.length !== VIEW_DECISIONS.length) n++;
    if (v.classes.length !== CLASSES.length) n++;
    if (v.emergency) n++;
    if (v.category) n++;
    if (v.country) n++;
    return n;
  }

  // The table's altitude: 0 on the ground, else barometric, else geometric.
  function trackAlt(t) {
    if (t.on_ground) return 0;
    if (typeof t.alt_baro_ft === 'number') return t.alt_baro_ft;
    if (typeof t.alt_geom_ft === 'number') return t.alt_geom_ft;
    return null;
  }

  // Case-insensitive substring over hex, callsign, registration and squawk.
  function searchMatches(t, text) {
    var q = String(text || '').trim().toLowerCase();
    if (!q) return true;
    var fields = [t.hex, t.callsign, t.registration, t.squawk];
    for (var i = 0; i < fields.length; i++) {
      var f = fields[i];
      if (f !== undefined && f !== null && String(f).trim().toLowerCase().indexOf(q) >= 0) return true;
    }
    return false;
  }

  // A band with a bound set fails an unknown value; min > max fails all.
  function inBand(v, min, max) {
    if (min === null && max === null) return true;
    if (typeof v !== 'number' || !isFinite(v)) return false;
    return (min === null || v >= min) && (max === null || v <= max);
  }

  // Does track t pass every rule of view v (spec 3)? The selected-aircraft
  // exemption is the page's business, not this function's.
  function viewMatches(t, v) {
    if (!searchMatches(t, v.search)) return false;
    if (t.on_ground && !v.ground) return false;
    if (!inBand(trackAlt(t), v.altMin, v.altMax)) return false;
    if (!inBand(t.gs_kt, v.gsMin, v.gsMax)) return false;
    if (!inBand(t.range_nm, v.rangeMin, v.rangeMax)) return false;
    // Only the five toggleable colour classes are subject to the pills:
    // no_position (null) and an unrecognised decision ('unknown') are not.
    var dc = decisionColor(t.decision);
    if (VIEW_DECISIONS.indexOf(dc) >= 0 && v.decisions.indexOf(dc) < 0) return false;
    if (v.classes.indexOf(trackClass(t)) < 0) return false;
    if (v.emergency && !isEmergency(t)) return false;
    if (v.category) {
      var cat = t.category || '';
      if (v.category === 'none' ? cat !== '' :
          v.category === 'other' ? (cat === '' || VIEW_CATEGORIES.indexOf(cat) >= 0) :
          cat !== v.category) return false;
    }
    if (v.country) {
      var c = trackCountry(t);
      if (v.country === 'other' ? c !== null : c !== v.country) return false;
    }
    return true;
  }

  // ---- signal-coloured trail (Trails, Colour by "signal") --------------------
  // A track's RSSI (dBFS, `rssi`) on a fixed scale of SIGNAL_COLORS.length
  // steps of SIGNAL_STEP_DB from SIGNAL_MIN_DB (weak: cool and dim) to
  // SIGNAL_MAX_DB (strong: warm and bright); values outside are clamped to the
  // end steps. Blue through violet and magenta to orange: no green (the
  // Light tiles' land and the `sent` colour) and no pale step, checked on
  // the Light tiles and the Dark style.
  var SIGNAL_MIN_DB = -35;
  var SIGNAL_MAX_DB = -5;
  var SIGNAL_COLORS = ['#4657c0', '#7b45c8', '#b03fb0', '#e0457a', '#f2662f', '#ff9a1f'];
  var SIGNAL_STEP_DB = (SIGNAL_MAX_DB - SIGNAL_MIN_DB) / SIGNAL_COLORS.length;
  // readsb reports -49.5 (its floor) when it has no signal level for the
  // data: network-fed Beast or raw frames, replayed recordings. An rssi at
  // or below it is "no signal", not the weakest step.
  var SIGNAL_NONE_DB = -49.5;

  // A track's rssi as a signal level: the number, or null when it is not a
  // finite number or is readsb's no-signal value (SIGNAL_NONE_DB or lower).
  function signalLevel(rssi) {
    if (typeof rssi !== 'number' || !isFinite(rssi) || rssi <= SIGNAL_NONE_DB) return null;
    return rssi;
  }
  // The step index of a signal, or -1 when it is no signal (signalLevel).
  function signalStep(db) {
    if (signalLevel(db) === null) return -1;
    var i = Math.floor((db - SIGNAL_MIN_DB) / SIGNAL_STEP_DB);
    return Math.max(0, Math.min(SIGNAL_COLORS.length - 1, i));
  }
  function signalColor(db) {
    var i = signalStep(db);
    return i < 0 ? null : SIGNAL_COLORS[i];
  }
  // The legend: one {color, from, to} per step, dB bounds, weak first.
  function signalLegend() {
    return SIGNAL_COLORS.map(function (c, i) {
      return { color: c, from: SIGNAL_MIN_DB + i * SIGNAL_STEP_DB, to: SIGNAL_MIN_DB + (i + 1) * SIGNAL_STEP_DB };
    });
  }
  // ---- coloured trail runs (Trails, "Colour by") -------------------------------
  // [{lat, lon, ...}] (oldest first) as polyline runs [{points: [[lat, lon],
  // ...], color}], `colorOf(point)` giving a point's colour (a step of a
  // fixed scale) or null when the point has no value: the segment into each
  // point takes that point's colour (the signal it was received at, the
  // altitude it was at), consecutive segments of one colour make one run,
  // and adjacent runs share their boundary point so the line is unbroken.
  // A null run is a stretch without a value (the caller draws it dim in
  // the decision colour). One point: one run of one point.
  function trailRuns(pts, colorOf) {
    var out = [];
    if (!Array.isArray(pts) || !pts.length) return out;
    var ll = function (p) { return [p.lat, p.lon]; };
    if (pts.length === 1) return [{ points: [ll(pts[0])], color: colorOf(pts[0]) }];
    for (var i = 1; i < pts.length; i++) {
      var c = colorOf(pts[i]), last = out[out.length - 1];
      if (last && last.color === c) last.points.push(ll(pts[i]));
      else out.push({ points: [ll(pts[i - 1]), ll(pts[i])], color: c });
    }
    return out;
  }

  // The same runs kept up to date as a trail moves, instead of rebuilt:
  // trailRunsAppend adds the segment from `prev` to `pt` to `runs` in
  // place, extending the last run when the colour is the same; it returns
  // true when it extended one, false when it pushed a new run.
  // trailRunsTrim drops the trail's first `n` points from `runs` in place
  // and returns {dropped, trimmed}: how many leading runs went whole, and
  // whether the first remaining run lost points from its front (it needs
  // no colours). After either, `runs` equals trailRuns of the trail's
  // points (2 or more).
  function trailRunsAppend(runs, prev, pt, colorOf) {
    var c = colorOf(pt), last = runs[runs.length - 1];
    if (last && last.color === c) {
      last.points.push([pt.lat, pt.lon]);
      return true;
    }
    runs.push({ points: [[prev.lat, prev.lon], [pt.lat, pt.lon]], color: c });
    return false;
  }

  // The signal runs: trailRuns on [{lat, lon, sig}].
  function sigOf(p) { return signalColor(p.sig); }
  function signalRuns(pts) { return trailRuns(pts, sigOf); }
  function signalRunsAppend(runs, prev, pt) { return trailRunsAppend(runs, prev, pt, sigOf); }
  function trailRunsTrim(runs, n) {
    var dropped = 0, start = 0;
    if (!(n > 0)) return { dropped: 0, trimmed: false };
    while (runs.length) {
      var end = start + runs[0].points.length - 1;   // the index of the run's last point
      if (end > n) break;
      runs.shift();
      dropped++;
      start = end;                                    // the next run starts on that point
    }
    var trimmed = runs.length > 0 && n > start;
    if (trimmed) runs[0].points.splice(0, n - start);
    return { dropped: dropped, trimmed: trimmed };
  }
  var signalRunsTrim = trailRunsTrim;
  // A run without a value (no signal, no altitude) is drawn in this
  // neutral grey (dim), never in a scale or decision colour, so it cannot
  // be read as a step: 3.5:1 on the Light land, 3.6:1 on the Dark style.
  var TRAIL_NONE_COLOR = '#78808a';

  // The altitude scale: a position's altitude (trackAlt: 0 on the ground,
  // else barometric, else geometric; feet) in one of ALT_COLORS.length
  // steps with the upper bounds ALT_BOUNDS_FT (a bound belongs to the step
  // below it: 1000 ft is the first step, 1001 the second). Green (low)
  // through yellow, orange and red to magenta and violet (high): the
  // conventional low-to-high progression, no blue, so nothing like the
  // signal scale's blue-to-orange start. Every step is at least 3.1:1
  // against the Light tiles' land (#f2efe9) and 2.8:1 against the Dark
  // style (#262a30; 3.6:1 against the no-tile background).
  var ALT_BOUNDS_FT = [1000, 5000, 10000, 20000, 30000, 40000];
  var ALT_COLORS = ['#1f8a5a', '#4f8f00', '#a87800', '#e8590c', '#e03131', '#c92a8a', '#8a4fe0'];
  // The step index of an altitude, or -1 when there is none.
  function altitudeStep(ft) {
    if (typeof ft !== 'number' || !isFinite(ft)) return -1;
    var i = 0;
    while (i < ALT_BOUNDS_FT.length && ft > ALT_BOUNDS_FT[i]) i++;
    return i;
  }
  function altitudeColor(ft) {
    var i = altitudeStep(ft);
    return i < 0 ? null : ALT_COLORS[i];
  }
  // The legend: one {color, from, to} per step, feet, low first; from is
  // null on the first step (the ground and below 1000 ft), to null on the last.
  function altitudeLegend() {
    return ALT_COLORS.map(function (c, i) {
      return { color: c, from: i ? ALT_BOUNDS_FT[i - 1] : null, to: i < ALT_BOUNDS_FT.length ? ALT_BOUNDS_FT[i] : null };
    });
  }
  function altOf(p) { return altitudeColor(p.alt); }
  // The altitude runs: trailRuns on [{lat, lon, alt}].
  function altitudeRuns(pts) { return trailRuns(pts, altOf); }

  // A Colour by mode's point colour function for trailRuns and
  // trailRunsAppend, on points {lat, lon, sig, alt}; null for 'decision'
  // (one plain trail in the decision colour, no runs).
  function trailRunColor(mode) {
    return mode === 'signal' ? sigOf : mode === 'altitude' ? altOf : null;
  }

  // ---- projection ahead (Trails, "Project ahead") ------------------------------
  // Dead reckoning: where an aircraft is `minutes` from now if it keeps its
  // track and ground speed, on the great circle (destinationPoint's sphere;
  // the track is true). projectPoint is that point; aheadNm the distance,
  // ground speed (kt) times the time. Straight flight only: a turn or a
  // speed change is not predicted.
  function projectPoint(lat, lon, trackDeg, distanceNm) { return destinationPoint(lat, lon, trackDeg, distanceNm); }
  function aheadNm(gsKt, minutes) { return gsKt * minutes / 60; }
  // The great-circle line from the track's position along its track_deg
  // for aheadNm(gs_kt, minutes), as [[lat, lon], ...] from the position to
  // the far end with a point every AHEAD_STEP_NM at most (Leaflet joins
  // points with straight lines on the map); null without a projection:
  // minutes 0, no position, no track_deg or gs_kt (or not above 0), or on
  // the ground.
  var AHEAD_STEP_NM = 10;
  function projectAhead(t, minutes) {
    var fin = function (v) { return typeof v === 'number' && isFinite(v); };
    if (!(minutes > 0) || !t || t.on_ground) return null;
    if (!fin(t.lat) || !fin(t.lon) || !fin(t.track_deg) || !fin(t.gs_kt) || !(t.gs_kt > 0)) return null;
    var nm = aheadNm(t.gs_kt, minutes), n = Math.max(1, Math.ceil(nm / AHEAD_STEP_NM)), out = [[t.lat, t.lon]];
    for (var i = 1; i <= n; i++) out.push(projectPoint(t.lat, t.lon, t.track_deg, nm * i / n));
    return out;
  }

  // ---- trail breaks -----------------------------------------------------------
  // What a new position does to the trail, from the previous trail point
  // (prev and next are {lat, lon, ts}, ts in ms):
  // - 'break': the trail restarts at it. Its implied ground speed
  //   (great-circle distance over the time between them, at least 1 s so
  //   equal or near-equal times cannot hide a jump or magnify rounding) is
  //   above TRAIL_MAX_KT, or it is more than TRAIL_BACKSTEP_S earlier: a
  //   looped replay jumping back to its start, a bad position, a hex reused
  //   by another aircraft.
  // - 'skip': up to TRAIL_BACKSTEP_S earlier (readsb's position_time
  //   wobbles by its seen_pos rounding; two inputs report out of order): not
  //   added, so the trail stays in time order, and not a break.
  // - 'join': otherwise, and when there is no previous point or no usable time.
  var TRAIL_MAX_KT = 1500;
  var TRAIL_BACKSTEP_S = 5;
  function trailStep(prev, next) {
    if (!prev || !next) return 'join';
    var dt = next.ts - prev.ts;
    if (typeof dt !== 'number' || !isFinite(dt)) return 'join';
    if (dt < -TRAIL_BACKSTEP_S * 1000) return 'break';
    if (dt < 0) return 'skip';
    var nm = rangeNm(prev.lat, prev.lon, next.lat, next.lon);
    return isFinite(nm) && nm / (Math.max(dt, 1000) / 3600000) > TRAIL_MAX_KT ? 'break' : 'join';
  }

  // ---- ruler ("Measure") -----------------------------------------------------
  // Great-circle distance (rangeNm's sphere) and initial true bearing from
  // the first point to the second.
  var KM_PER_NM = METRES_PER_NM / 1000;
  function rulerMeasure(lat1, lon1, lat2, lon2) {
    var nm = rangeNm(lat1, lon1, lat2, lon2);
    return { nm: nm, km: nm * KM_PER_NM, bearing: bearingDeg(lat1, lon1, lat2, lon2) };
  }
  // "175.6 nm · 325.2 km · 050°T": one decimal under 1000 (none from 1000 up,
  // with thousands separators), the bearing in whole degrees, 000..359.
  function rulerLabel(m) {
    if (!m || typeof m.nm !== 'number' || !isFinite(m.nm)) return '';
    var dist = function (v) { return v < 1000 ? v.toFixed(1) : thousands(Math.round(v)); };
    var b = Math.round(m.bearing) % 360;
    var bs = String(b);
    while (bs.length < 3) bs = '0' + bs;
    return dist(m.nm) + ' nm · ' + dist(m.km) + ' km · ' + bs + '°T';
  }

  var api = {
    EARTH_RADIUS_M: EARTH_RADIUS_M, METRES_PER_NM: METRES_PER_NM, DECISION_COLORS: DECISION_COLORS,
    rangeNm: rangeNm, bearingDeg: bearingDeg, elevationDeg: elevationDeg, elevationRingNm: elevationRingNm,
    decisionColor: decisionColor, formatAlt: formatAlt, formatNum: formatNum, formatAge: formatAge,
    thousands: thousands, trackKey: trackKey, markerLabel: markerLabel, listHas: listHas,
    sortTracks: sortTracks, graticuleStep: graticuleStep, parentTile: parentTile,
    tileBounds: tileBounds, tileInsideRegions: tileInsideRegions,
    nearestInsideAncestor: nearestInsideAncestor, underlayNextLevel: underlayNextLevel,
    capSymbols: capSymbols, closeReportCard: closeReportCard,
    dropRemovesMarker: dropRemovesMarker,
    CUE_COUNTERS: CUE_COUNTERS, DECISIONS_RING: DECISIONS_RING, pushRing: pushRing, decisionRow: decisionRow,
    filterHistoryRows: filterHistoryRows,
    CONFIG_GROUPS: CONFIG_GROUPS, groupConfig: groupConfig, configText: configText, confValue: confValue,
    daemonModeText: daemonModeText, daemonLocationsText: daemonLocationsText,
    backoffMs: backoffMs, CLOCK_RESET_MS: CLOCK_RESET_MS, nextClockOffset: nextClockOffset,
    BINS_PER_BLOCK: BINS_PER_BLOCK, expandRle: expandRle, dbzColor: dbzColor, SHAPE_COLORS: SHAPE_COLORS,
    shapeFamily: shapeFamily, shapeStyle: shapeStyle, ageClass: ageClass, mosaicBounds: mosaicBounds,
    RADAR_FADE_MS: RADAR_FADE_MS, radarFaded: radarFaded, radarOpacity: radarOpacity, radarOpacityValue: radarOpacityValue,
    CLOCK_SKEW_WARN_S: CLOCK_SKEW_WARN_S, skewWarning: skewWarning, radarNeedsFetch: radarNeedsFetch,
    mercY: mercY, mosaicCanvasSize: mosaicCanvasSize, stationMatches: stationMatches,
    trackVisible: trackVisible, hiddenCount: hiddenCount,
    MAP_STYLES: MAP_STYLES, DARK_TILE_FILTER: DARK_TILE_FILTER, mapStyle: mapStyle, tileFilter: tileFilter,
    ADMIN1_MIN_ZOOM: ADMIN1_MIN_ZOOM, borderVisible: borderVisible, borderStyle: borderStyle,
    cityRankLimit: cityRankLimit, MAX_CITY_LABELS: MAX_CITY_LABELS, cullCities: cullCities,
    overlayOffer: overlayOffer,
    INPUT_TYPES: INPUT_TYPES, validateInputDef: validateInputDef, inputFields: inputFields, inputDot: inputDot,
    inputRates: inputRates, statusStale: statusStale, inputEndpoint: inputEndpoint, rejectedText: rejectedText,
    inputsSummary: inputsSummary,
    gpsStatusText: gpsStatusText, receiverStatusText: receiverStatusText,
    FLIGHT_CATEGORIES: FLIGHT_CATEGORIES, reportBody: reportBody, visibilitySm: visibilitySm,
    ceilingFt: ceilingFt, flightCategory: flightCategory, windGroup: windGroup, parseMetar: parseMetar,
    tafFirstGroup: tafFirstGroup, parseTafFirst: parseTafFirst,
    WINDS_LEVELS_FT: WINDS_LEVELS_FT, parseWinds: parseWinds, windsLevel: windsLevel,
    nearestWindsPeriod: nearestWindsPeriod, windsPeriods: windsPeriods, windsLevelsFor: windsLevelsFor,
    RASTER_BAND_PRODUCTS: RASTER_BAND_PRODUCTS, rasterBandFt: rasterBandFt, rasterProducts: rasterProducts,
    defaultBand: defaultBand, parsePirep: parsePirep,
    WINDS_PREFIXES: WINDS_PREFIXES, stationCandidates: stationCandidates, resolveStation: resolveStation,
    destinationPoint: destinationPoint, pirepPosition: pirepPosition, barbParts: barbParts,
    ICAO_BLOCKS: ICAO_BLOCKS, MILITARY_BLOCKS: MILITARY_BLOCKS, ICAO_COUNTRIES: ICAO_COUNTRIES,
    icaoCountry: icaoCountry, icaoMilitary: icaoMilitary,
    MILITARY_CALLSIGN_PREFIXES: MILITARY_CALLSIGN_PREFIXES, callsignKind: callsignKind,
    CLASSES: CLASSES, CATEGORY_LABELS: CATEGORY_LABELS, trackClass: trackClass, isEmergency: isEmergency, trackCountry: trackCountry,
    VIEW_DEFAULTS: VIEW_DEFAULTS, normalizeView: normalizeView, viewActiveCount: viewActiveCount,
    TRAIL_SHOWS: TRAIL_SHOWS, TRAIL_COLOR_BY: TRAIL_COLOR_BY, TRAIL_AHEAD_MIN: TRAIL_AHEAD_MIN,
    viewTrailsDefault: viewTrailsDefault,
    MAP_BOXES: MAP_BOXES, mapBoxName: mapBoxName, mapBoxToggle: mapBoxToggle,
    LAYER_IDS: LAYER_IDS, LAYER_DEFAULTS: LAYER_DEFAULTS, normalizeLayers: normalizeLayers,
    VIEW_SETS_VERSION: VIEW_SETS_VERSION, normalizeMapView: normalizeMapView, mapViewMatches: mapViewMatches,
    normalizeLayersPartial: normalizeLayersPartial, layersOnCount: layersOnCount, weatherDot: weatherDot,
    projectPoint: projectPoint, aheadNm: aheadNm, AHEAD_STEP_NM: AHEAD_STEP_NM, projectAhead: projectAhead,
    VIEW_SETS_MAX: VIEW_SETS_MAX, VIEW_SET_NAME_MAX: VIEW_SET_NAME_MAX, viewSetName: viewSetName,
    normalizeViewSets: normalizeViewSets, viewSetMatches: viewSetMatches, putViewSet: putViewSet, utcDate: utcDate,
    trackAlt: trackAlt, searchMatches: searchMatches, viewMatches: viewMatches,
    SIGNAL_MIN_DB: SIGNAL_MIN_DB, SIGNAL_MAX_DB: SIGNAL_MAX_DB, SIGNAL_COLORS: SIGNAL_COLORS,
    SIGNAL_NONE_DB: SIGNAL_NONE_DB, signalLevel: signalLevel,
    signalStep: signalStep, signalColor: signalColor, signalLegend: signalLegend, signalRuns: signalRuns,
    signalRunsAppend: signalRunsAppend, signalRunsTrim: signalRunsTrim,
    TRAIL_NONE_COLOR: TRAIL_NONE_COLOR, TRAIL_FIELDS: TRAIL_FIELDS, viewSetsFillTrails: viewSetsFillTrails,
    trailRuns: trailRuns, trailRunsAppend: trailRunsAppend, trailRunsTrim: trailRunsTrim, trailRunColor: trailRunColor,
    ALT_BOUNDS_FT: ALT_BOUNDS_FT, ALT_COLORS: ALT_COLORS, altitudeStep: altitudeStep, altitudeColor: altitudeColor,
    altitudeLegend: altitudeLegend, altitudeRuns: altitudeRuns,
    TRAIL_MAX_KT: TRAIL_MAX_KT, TRAIL_BACKSTEP_S: TRAIL_BACKSTEP_S, trailStep: trailStep,
    rulerMeasure: rulerMeasure, rulerLabel: rulerLabel
  };
  root.skycuePure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
