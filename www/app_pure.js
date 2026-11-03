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

  // Overlays (scripts/make-overlays.sh). Borders: admin0 (countries) at
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

  var api = {
    EARTH_RADIUS_M: EARTH_RADIUS_M, METRES_PER_NM: METRES_PER_NM, DECISION_COLORS: DECISION_COLORS,
    rangeNm: rangeNm, bearingDeg: bearingDeg, elevationDeg: elevationDeg, elevationRingNm: elevationRingNm,
    decisionColor: decisionColor, formatAlt: formatAlt, formatNum: formatNum, formatAge: formatAge,
    thousands: thousands, trackKey: trackKey, markerLabel: markerLabel, listHas: listHas,
    sortTracks: sortTracks, graticuleStep: graticuleStep, parentTile: parentTile,
    tileBounds: tileBounds, tileInsideRegions: tileInsideRegions,
    dropRemovesMarker: dropRemovesMarker,
    backoffMs: backoffMs, CLOCK_RESET_MS: CLOCK_RESET_MS, nextClockOffset: nextClockOffset,
    BINS_PER_BLOCK: BINS_PER_BLOCK, expandRle: expandRle, dbzColor: dbzColor, SHAPE_COLORS: SHAPE_COLORS,
    shapeFamily: shapeFamily, shapeStyle: shapeStyle, ageClass: ageClass, mosaicBounds: mosaicBounds,
    RADAR_FADE_MS: RADAR_FADE_MS, radarFaded: radarFaded, radarOpacity: radarOpacity,
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
    gpsStatusText: gpsStatusText, receiverStatusText: receiverStatusText
  };
  root.skycuePure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
