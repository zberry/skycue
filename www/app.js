// skycue web UI: the live map, filters, lists, selection panel, stats and
// table. Plain ES2017, no framework; Leaflet (vendor/leaflet) and
// app_pure.js (window.skycuePure) are loaded before this file. Every
// request goes to the page's own origin. The API is docs/web-api.md.
(function () {
  'use strict';
  var P = window.skycuePure;
  var FT = 0.3048;
  var NM = P.METRES_PER_NM;
  var RING_ALTS_FT = [5000, 10000, 15000, 20000, 25000, 30000, 35000, 40000, 45000];
  var DEFAULT_RING_FT = 35000;
  var SENT_FLASH_MS = 700;

  // ---- state ---------------------------------------------------------------
  var cfg = {
    receiver: null,
    filters: null,
    lists: { allow: [], block: [], overlay_active: false },
    trailSeconds: 120
  };
  var clockOffsetMs = 0;            // skycued's data clock minus the browser's
  var tracks = new Map();           // trackKey -> entry {key, t, pts, marker, trail, seenWall}
  var selectedKey = null;
  var map = null, rxMarker = null, rangeRing = null, elevRing = null, graticule = null;
  var haveTiles = false, fitted = false;
  var ws = null, connected = false, attempt = 0, reconnectTimer = null;
  var prevStats = null, prevStatsWall = 0;
  var tableSort = { key: 'hex', dir: 1 };
  var tableDirty = true;
  var putTimers = {};
  var actionsSig = '';

  var $ = function (id) { return document.getElementById(id); };
  function dataNow() { return Date.now() + clockOffsetMs; }
  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function showMsg(el, text, isError) {
    el.textContent = text || '';
    el.classList.toggle('error', !!isError);
  }

  // ---- HTTP ------------------------------------------------------------------
  function request(method, path, body) {
    var init = { method: method, headers: {} };
    if (body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body);
    }
    return fetch(path, init).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (j) {
        if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
        return j;
      });
    });
  }

  // ---- map -------------------------------------------------------------------
  var PLANE_SVG = '<svg viewBox="-12 -12 24 24" aria-hidden="true"><path d="M0,-11 L2,-4 L10,1.5 L10,3.5 L2,1.5 ' +
    'L1.4,7 L4,9 L4,10.6 L0,9.6 L-4,10.6 L-4,9 L-1.4,7 L-2,1.5 L-10,3.5 L-10,1.5 L-2,-4 Z"/></svg>';
  var RX_SVG = '<svg width="18" height="18" viewBox="-9 -9 18 18" aria-hidden="true">' +
    '<circle r="7" fill="none" stroke="currentColor" stroke-width="1.5"/><circle r="2" fill="currentColor"/></svg>';

  function initMap(tiles) {
    haveTiles = !!(tiles && tiles.tiles);
    map = L.map('map', {
      zoomControl: true,
      attributionControl: haveTiles,
      minZoom: haveTiles && typeof tiles.minZoom === 'number' ? tiles.minZoom : 2,
      maxZoom: 14,
      worldCopyJump: false
    }).setView([0, 0], 3);
    if (haveTiles) {
      L.tileLayer('tiles/{z}/{x}/{y}.png', {
        minZoom: typeof tiles.minZoom === 'number' ? tiles.minZoom : 0,
        maxNativeZoom: typeof tiles.maxZoom === 'number' ? tiles.maxZoom : 10,
        maxZoom: 14,
        attribution: 'Made with Natural Earth' // scripts/make-tiles.sh
      }).addTo(map);
    } else {
      graticule = L.layerGroup().addTo(map);
      map.on('zoomend moveend', drawGraticule);
      drawGraticule();
    }
    L.control.scale({ position: 'bottomleft' }).addTo(map);
    map.on('click', function () { deselect(); });
  }

  // Plain-background fallback: a lat/lon grid every 5° (zoomed out) or 1°.
  function drawGraticule() {
    if (!graticule) return;
    graticule.clearLayers();
    var step = P.graticuleStep(map.getZoom());
    var b = map.getBounds().pad(0.5);
    var s = Math.max(-85, Math.floor(b.getSouth() / step) * step);
    var n = Math.min(85, Math.ceil(b.getNorth() / step) * step);
    var w = Math.max(-180, Math.floor(b.getWest() / step) * step);
    var e = Math.min(180, Math.ceil(b.getEast() / step) * step);
    var opts = { className: 'graticule', color: '#22364a', weight: 1, interactive: false };
    for (var lat = s; lat <= n; lat += step) graticule.addLayer(L.polyline([[lat, w], [lat, e]], opts));
    for (var lon = w; lon <= e; lon += step) graticule.addLayer(L.polyline([[s, lon], [n, lon]], opts));
  }

  function rxAltM() {
    var r = cfg.receiver;
    return (r.alt_m || 0) + (r.antenna_m || 0);
  }

  function updateReceiver() {
    var r = cfg.receiver;
    if (!r || typeof r.lat !== 'number') return;
    var ll = [r.lat, r.lon];
    if (!rxMarker) {
      rxMarker = L.marker(ll, {
        icon: L.divIcon({ className: 'rx-marker', html: RX_SVG, iconSize: [18, 18], iconAnchor: [9, 9] }),
        interactive: false, keyboard: false, zIndexOffset: -1000
      }).addTo(map);
    } else {
      rxMarker.setLatLng(ll);
    }
    updateRings();
    if (!fitted && rangeRing) {
      map.fitBounds(rangeRing.getBounds(), { padding: [10, 10] });
      fitted = true;
    }
  }

  function updateRings() {
    var r = cfg.receiver, f = cfg.filters;
    if (!r || !f || !map) return;
    var ll = [r.lat, r.lon];
    if (!rangeRing) {
      rangeRing = L.circle(ll, {
        radius: f.max_range_nm * NM, className: 'range-ring', color: '#58a6ff', weight: 1.5,
        opacity: 0.8, fill: false, dashArray: '6 6', interactive: false
      }).addTo(map);
    } else {
      rangeRing.setLatLng(ll);
      rangeRing.setRadius(f.max_range_nm * NM);
    }
    var altFt = Number($('ring-alt').value) || DEFAULT_RING_FT;
    var nm = P.elevationRingNm(r.lat, r.lon, rxAltM(), altFt * FT, f.min_elevation_deg, 1000);
    if (nm === null) {
      if (elevRing) { map.removeLayer(elevRing); elevRing = null; }
      return;
    }
    if (!elevRing) {
      elevRing = L.circle(ll, {
        radius: nm * NM, className: 'elevation-ring', color: '#f0883e', weight: 1.2,
        opacity: 0.7, fill: false, dashArray: '2 6', interactive: false
      }).addTo(map);
    } else {
      elevRing.setLatLng(ll);
      elevRing.setRadius(nm * NM);
    }
  }

  // ---- aircraft --------------------------------------------------------------
  function markerEl(e) { return e.marker ? e.marker.getElement() : null; }

  function styleMarker(e) {
    var el = markerEl(e);
    if (!el) return;
    var t = e.t;
    var cls = P.decisionColor(t.decision) || 'unknown';
    el.className = el.className.replace(/\bdecision-\S+/g, '').replace(/\s+/g, ' ').trim() + ' decision-' + cls;
    el.classList.toggle('selected', e.key === selectedKey);
    el.setAttribute('data-hex', t.hex);
    el.setAttribute('data-decision', cls);
    var glyph = el.querySelector('.ac-glyph');
    glyph.style.transform = 'rotate(' + (typeof t.track_deg === 'number' ? t.track_deg : 0) + 'deg)';
    glyph.setAttribute('title', P.markerLabel(t) + ' (' + t.decision + ')');
    var label = el.querySelector('.ac-label');
    var text = P.markerLabel(t);
    if (label.textContent !== text) label.textContent = text;
    if (e.trail) e.trail.setStyle({ color: P.DECISION_COLORS[cls], weight: e.key === selectedKey ? 3 : 1.5 });
  }

  function ensureMarker(e) {
    var t = e.t;
    if (e.marker) {
      e.marker.setLatLng([t.lat, t.lon]);
      return;
    }
    e.marker = L.marker([t.lat, t.lon], {
      icon: L.divIcon({
        className: 'ac-marker',
        html: '<span class="ac-glyph">' + PLANE_SVG + '</span><span class="ac-label"></span>',
        iconSize: [22, 22], iconAnchor: [11, 11]
      }),
      keyboard: false, riseOnHover: true
    });
    var key = e.key;
    e.marker.on('click', function () { select(key); });
    e.marker.addTo(map);
    e.trail = L.polyline([], { className: 'trail', weight: 1.5, opacity: 0.6, interactive: false }).addTo(map);
  }

  function pruneTrail(e) {
    var cutoff = dataNow() - cfg.trailSeconds * 1000;
    var i = 0;
    while (i < e.pts.length && e.pts[i].ts < cutoff) i++;
    if (i) e.pts.splice(0, i);
  }

  function removeEntry(key) {
    var e = tracks.get(key);
    if (!e) return;
    if (e.marker) map.removeLayer(e.marker);
    if (e.trail) map.removeLayer(e.trail);
    tracks.delete(key);
    if (key === selectedKey) deselect();
    tableDirty = true;
  }

  function clearAll() {
    Array.from(tracks.keys()).forEach(function (k) {
      var e = tracks.get(k);
      if (e.marker) map.removeLayer(e.marker);
      if (e.trail) map.removeLayer(e.trail);
    });
    tracks.clear();
    tableDirty = true;
  }

  // A track object from the snapshot, an `observed` or an `update` item.
  function onTrack(t, observed) {
    var key = P.trackKey(t);
    var e = tracks.get(key);
    if (!e) {
      e = { key: key, t: t, pts: [], marker: null, trail: null, seenWall: Date.now() };
      tracks.set(key, e);
    }
    e.t = t;
    if (observed) e.seenWall = Date.now();
    var ls = Date.parse(t.last_seen);
    if (ls > dataNow()) clockOffsetMs = ls - Date.now();
    var drawable = typeof t.lat === 'number' && typeof t.lon === 'number' && P.decisionColor(t.decision) !== null;
    if (drawable && map) {
      ensureMarker(e);
      if (observed) {
        var ts = Date.parse(t.position_time);
        e.pts.push({ ts: isNaN(ts) ? dataNow() : ts, ll: [t.lat, t.lon] });
        pruneTrail(e);
        e.trail.setLatLngs(e.pts.map(function (p) { return p.ll; }));
      }
      styleMarker(e);
    }
    if (key === selectedKey) renderDetails();
    tableDirty = true;
    return e;
  }

  function flashSent(e) {
    var el = markerEl(e);
    if (!el) return;
    el.classList.add('sent-flash');
    clearTimeout(e.flashTimer);
    e.flashTimer = setTimeout(function () { el.classList.remove('sent-flash'); }, SENT_FLASH_MS);
  }

  function onDrop(d) {
    var key = P.trackKey(d);
    var e = tracks.get(key);
    if (!e) return;
    if (P.dropRemovesMarker(d.reason)) {
      removeEntry(key);
      return;
    }
    // filtered: no longer forwarded, still observed (recoloured by the next `observed`).
    e.t = Object.assign({}, e.t, { forwarded: false });
    if (key === selectedKey) renderDetails();
    tableDirty = true;
  }

  // Safety net: drop what has not been observed for 2 x TRAIL_SECONDS, and
  // let trails age out between observations.
  function prune() {
    var limit = 2 * cfg.trailSeconds * 1000, now = Date.now();
    Array.from(tracks.values()).forEach(function (e) {
      if (now - e.seenWall > limit) {
        removeEntry(e.key);
        return;
      }
      if (e.trail && e.pts.length) {
        var n = e.pts.length;
        pruneTrail(e);
        if (e.pts.length !== n) e.trail.setLatLngs(e.pts.map(function (p) { return p.ll; }));
      }
    });
  }

  // ---- selection panel ---------------------------------------------------------
  function select(key) {
    var prev = selectedKey;
    selectedKey = key;
    [prev, key].forEach(function (k) {
      var e = k && tracks.get(k);
      if (e) styleMarker(e);
    });
    actionsSig = '';
    var wasHidden = $('details').hidden;
    $('details').hidden = false;
    renderDetails();
    if (wasHidden && map) map.invalidateSize({ pan: false });
    $('details').focus({ preventScroll: true });
    tableDirty = true;
  }

  function deselect() {
    if (selectedKey === null) return;
    var e = tracks.get(selectedKey);
    selectedKey = null;
    if (e) styleMarker(e);
    $('details').hidden = true;
    if (map) map.invalidateSize({ pan: false });
    tableDirty = true;
  }

  function fmtTime(iso) {
    var ms = Date.parse(iso);
    if (isNaN(ms)) return String(iso);
    return new Date(ms).toISOString().substr(11, 8) + 'Z · ' + P.formatAge(ms, dataNow());
  }
  function yesNo(v) { return v ? 'yes' : 'no'; }
  function signed(v) { return (v > 0 ? '+' : v < 0 ? '−' : '') + P.thousands(Math.abs(v)); }

  // Every key of the track object, in this order, then any others as sent.
  var FIELDS = [
    ['hex', 'Hex'], ['addr_type', 'Address type'], ['source', 'Source'], ['callsign', 'Callsign'],
    ['registration', 'Registration'], ['squawk', 'Squawk'], ['category', 'Category'], ['emergency', 'Emergency'],
    ['lat', 'Latitude', function (v) { return v.toFixed(5) + '°'; }],
    ['lon', 'Longitude', function (v) { return v.toFixed(5) + '°'; }],
    ['position_time', 'Position time', fmtTime],
    ['alt_baro_ft', 'Altitude (baro)', function (v) { return P.thousands(v) + ' ft'; }],
    ['alt_geom_ft', 'Altitude (geom)', function (v) { return P.thousands(v) + ' ft'; }],
    ['alt_is_geom', 'Elevation uses geom alt', yesNo],
    ['on_ground', 'On ground', yesNo],
    ['gs_kt', 'Ground speed', function (v) { return v.toFixed(0) + ' kt'; }],
    ['track_deg', 'Track', function (v) { return v.toFixed(0) + '°'; }],
    ['vr_fpm', 'Vertical rate', function (v) { return signed(v) + ' ft/min'; }],
    ['range_nm', 'Range', function (v) { return v.toFixed(1) + ' nm'; }],
    ['bearing', 'Bearing', function (v) { return v.toFixed(0) + '°'; }],
    ['elevation_deg', 'Elevation', function (v) { return v.toFixed(2) + '°'; }],
    ['decision', 'Decision'], ['forwarded', 'Forwarded', yesNo],
    ['nic', 'NIC'], ['nac_p', 'NACp'],
    ['rssi', 'RSSI', function (v) { return v.toFixed(1) + ' dBFS'; }],
    ['first_seen', 'First seen', fmtTime], ['last_seen', 'Last seen', fmtTime]
  ];

  function renderDetails() {
    var e = selectedKey && tracks.get(selectedKey);
    if (!e) return;
    var t = Object.assign({}, e.t);
    var r = cfg.receiver;
    if (r && typeof t.lat === 'number') t.bearing = P.bearingDeg(r.lat, r.lon, t.lat, t.lon);
    $('details-title').textContent = (t.callsign || t.hex) + (t.registration ? ' · ' + t.registration : '');
    var known = {};
    var rows = [];
    FIELDS.forEach(function (f) {
      known[f[0]] = true;
      var v = t[f[0]];
      if (v === undefined || v === null) return;
      rows.push([f[0], f[1], f[2] ? f[2](v) : String(v)]);
    });
    Object.keys(t).forEach(function (k) {
      if (known[k] || k === 'type') return;
      var v = t[k];
      rows.push([k, k, typeof v === 'object' ? JSON.stringify(v) : String(v)]);
    });
    $('details-fields').tBodies[0].innerHTML = rows.map(function (row) {
      return '<tr><th scope="row">' + esc(row[1]) + '</th><td data-field="' + esc(row[0]) + '">' + esc(row[2]) + '</td></tr>';
    }).join('');
    renderActions(t);
  }

  // Add/remove buttons; rebuilt only when what they show changes, so a
  // click is never lost to a live re-render.
  function renderActions(t) {
    var entries = [{ entry: t.hex, label: '' }];
    if (t.registration) entries.push({ entry: t.registration, label: t.registration + ' ' });
    var buttons = [];
    entries.forEach(function (x) {
      ['allow', 'block'].forEach(function (list) {
        var listed = P.listHas(cfg.lists[list], x.entry);
        buttons.push({ list: list, entry: x.entry, op: listed ? 'remove' : 'add',
          text: (listed ? 'Remove ' + x.label + 'from ' : 'Add ' + x.label + 'to ') + list + ' list' });
      });
    });
    var sig = JSON.stringify(buttons);
    if (sig === actionsSig) return;
    actionsSig = sig;
    var box = $('details-actions');
    box.innerHTML = '';
    buttons.forEach(function (b) {
      var el = document.createElement('button');
      el.type = 'button';
      el.textContent = b.text;
      el.className = b.op === 'remove' ? 'listed' : '';
      el.setAttribute('data-action', b.op + '-' + b.list);
      el.addEventListener('click', function () { changeList(b.list, b.op, b.entry); });
      box.appendChild(el);
    });
  }

  // ---- filters, lists, baseline -------------------------------------------------
  var FILTER_INPUTS = { max_range_nm: 'max-range', min_elevation_deg: 'min-elevation' };

  function applyFilters(f) {
    cfg.filters = f;
    Object.keys(FILTER_INPUTS).forEach(function (k) {
      if (putTimers[k]) return; // the user's pending edit wins
      var el = $(FILTER_INPUTS[k]);
      if (typeof f[k] !== 'number') return;
      if (el.value === '' || Number(el.value) !== f[k]) el.value = String(f[k]);
    });
    updateRings();
    updateOverlay();
  }

  function applyLists(l) {
    cfg.lists = l;
    ['allow', 'block'].forEach(function (list) {
      var ul = $('list-' + list);
      var entries = l[list] || [];
      ul.innerHTML = entries.length ? '' : '<li class="empty">(empty)</li>';
      entries.forEach(function (entry) {
        var li = document.createElement('li');
        var span = document.createElement('span');
        span.textContent = entry;
        var btn = document.createElement('button');
        btn.type = 'button';
        btn.textContent = '×';
        btn.setAttribute('aria-label', 'Remove ' + entry + ' from the ' + list + ' list');
        btn.addEventListener('click', function () { changeList(list, 'remove', entry); });
        li.appendChild(span);
        li.appendChild(btn);
        ul.appendChild(li);
      });
    });
    updateOverlay();
    if (selectedKey) renderDetails();
  }

  function updateOverlay() {
    var active = !!((cfg.filters && cfg.filters.overlay_active) || (cfg.lists && cfg.lists.overlay_active));
    $('overlay-indicator').hidden = !active;
    $('save-baseline').disabled = !active;
  }

  function changeList(list, op, entry) {
    var body = {};
    body[op] = [entry];
    return request('POST', 'api/lists/' + list, body).then(function (l) {
      applyLists(l);
      showMsg($('filter-msg'), '');
    }).catch(function (err) {
      showMsg($('filter-msg'), list + ' list: ' + err.message, true);
    });
  }

  function putFilter(key, value) {
    var body = {};
    body[key] = value;
    return request('PUT', 'api/filters', body).then(function (f) {
      applyFilters(f);
      showMsg($('filter-msg'), '');
    }).catch(function (err) {
      showMsg($('filter-msg'), err.message, true);
    });
  }

  function onFilterInput(key) {
    clearTimeout(putTimers[key]);
    putTimers[key] = setTimeout(function () {
      putTimers[key] = null;
      var raw = $(FILTER_INPUTS[key]).value.trim();
      var v = Number(raw);
      var ok = raw !== '' && isFinite(v) && (key === 'max_range_nm' ? v > 0 : v >= -90 && v <= 90);
      if (!ok) {
        showMsg($('filter-msg'), key === 'max_range_nm' ? 'Max range must be a number > 0' :
          'Min elevation must be a number from -90 to 90', true);
        return;
      }
      if (cfg.filters && cfg.filters[key] === v) return;
      putFilter(key, v);
    }, 300);
  }

  function saveBaseline() {
    if (!window.confirm('Save the running filters and lists as the baseline?\n\n' +
        'This rewrites MAX_RANGE_NM / MIN_ELEVATION_DEG in skycue.conf and the allow/block list files.')) return;
    request('POST', 'api/filters/save-baseline').then(function (res) {
      showMsg($('save-msg'), res.unsaved ? 'Not saved:\n' + res.unsaved : 'Saved as baseline.', !!res.unsaved);
      return Promise.all([request('GET', 'api/filters'), request('GET', 'api/lists')]);
    }).then(function (r) {
      applyFilters(r[0]);
      applyLists(r[1]);
    }).catch(function (err) {
      showMsg($('save-msg'), 'Save failed: ' + err.message, true);
    });
  }

  // ---- stats -------------------------------------------------------------------
  function setStat(id, text, cls) {
    var el = $(id);
    el.textContent = text;
    if (cls !== undefined) {
      el.classList.toggle('on', cls === 'on');
      el.classList.toggle('off', cls === 'off');
    }
  }
  function applyStats(s) {
    var now = Date.now();
    setStat('stat-tracks-total', String(s.tracks_total));
    setStat('stat-tracks-forwarded', String(s.tracks_forwarded));
    setStat('stat-readsb', s.readsb_connected ? '● up' : '○ down', s.readsb_connected ? 'on' : 'off');
    setStat('stat-dump978', s.dump978_connected ? '● up' : '○ down', s.dump978_connected ? 'on' : 'off');
    setStat('stat-tcp', String(s.tcp_clients));
    setStat('stat-udp', String(s.udp_targets));
    setStat('stat-rec-drops', String((s.recorder_dropped_received || 0) + (s.recorder_dropped_sent || 0)));
    setStat('stat-web', String(s.web_clients));
    if (prevStats && now > prevStatsWall) {
      var dt = (now - prevStatsWall) / 1000;
      var rate = Math.max(0, s.observations - prevStats.observations) / dt;
      setStat('stat-obs-rate', rate.toFixed(1));
      var dl = Math.max(0, s.uat_downlink - prevStats.uat_downlink) * 60 / dt;
      var ul = Math.max(0, s.uat_uplink - prevStats.uat_uplink) * 60 / dt;
      setStat('stat-uat', dl.toFixed(0) + ' / ' + ul.toFixed(0));
    }
    prevStats = s;
    prevStatsWall = now;
  }

  // ---- table -------------------------------------------------------------------
  var TABLE_COLS = ['hex', 'callsign', 'registration', 'alt', 'gs_kt', 'range_nm', 'elevation_deg', 'decision'];
  function renderTable() {
    if ($('table-view').hidden) return;
    tableDirty = false;
    var rows = Array.from(tracks.values()).map(function (e) {
      var t = e.t;
      var alt = t.on_ground ? 0 : (typeof t.alt_baro_ft === 'number' ? t.alt_baro_ft : t.alt_geom_ft);
      return { key: e.key, t: t, hex: t.hex, callsign: t.callsign, registration: t.registration, alt: alt,
        gs_kt: t.gs_kt, range_nm: t.range_nm, elevation_deg: t.elevation_deg, decision: t.decision };
    });
    rows = P.sortTracks(rows, tableSort.key, tableSort.dir);
    var ths = document.querySelectorAll('#table-view th');
    Array.prototype.forEach.call(ths, function (th) {
      var on = th.getAttribute('data-key') === tableSort.key;
      th.classList.toggle('sorted', on);
      th.classList.toggle('desc', on && tableSort.dir < 0);
    });
    $('table-view').querySelector('tbody').innerHTML = rows.map(function (r) {
      var cls = P.decisionColor(r.decision) || 'no_position';
      var cells = {
        hex: esc(r.hex), callsign: esc(r.callsign || ''), registration: esc(r.registration || ''),
        alt: r.t.on_ground ? 'ground' : (typeof r.alt === 'number' ? P.thousands(r.alt) : '—'),
        gs_kt: P.formatNum(r.gs_kt, 0), range_nm: P.formatNum(r.range_nm, 1),
        elevation_deg: P.formatNum(r.elevation_deg, 2),
        decision: '<i class="sw decision-' + cls + '"></i>' + esc(r.decision)
      };
      return '<tr data-key="' + esc(r.key) + '"' + (r.key === selectedKey ? ' class="selected"' : '') + '>' +
        TABLE_COLS.map(function (c) {
          var num = c === 'alt' || c === 'gs_kt' || c === 'range_nm' || c === 'elevation_deg';
          return '<td data-key="' + c + '"' + (num ? ' class="num"' : '') + '>' + cells[c] + '</td>';
        }).join('') + '</tr>';
    }).join('');
  }
  function sortBy(key) {
    tableSort = { key: key, dir: tableSort.key === key ? -tableSort.dir : 1 };
    renderTable();
  }
  function toggleTable() {
    var tv = $('table-view');
    tv.hidden = !tv.hidden;
    $('table-toggle').setAttribute('aria-pressed', String(!tv.hidden));
    $('table-toggle').textContent = tv.hidden ? 'Table' : 'Map';
    if (tv.hidden) map.invalidateSize({ pan: false });
    else renderTable();
  }

  // ---- WebSocket -----------------------------------------------------------------
  function setConnected(on) {
    connected = on;
    $('conn-banner').hidden = on;
  }

  function onSnapshot(s) {
    clearAll();
    clockOffsetMs = Date.parse(s.now) - Date.now();
    if (isNaN(clockOffsetMs)) clockOffsetMs = 0;
    cfg.receiver = s.receiver;
    cfg.trailSeconds = s.trail_seconds || 120;
    prevStats = null;
    applyStats(s.stats);
    applyLists(s.lists);
    applyFilters(s.filters);
    updateReceiver();
    (s.tracks || []).forEach(function (t) { onTrack(t, true); });
    if (selectedKey && !tracks.has(selectedKey)) deselect();
    else if (selectedKey) renderDetails();
    attempt = 0;
    setConnected(true);
  }

  function onItem(it) {
    switch (it.type) {
      case 'observed': onTrack(it, true); break;
      case 'update': flashSent(onTrack(it, false)); break;
      case 'drop': onDrop(it); break;
      case 'stats': applyStats(it); break;
      case 'filters': applyFilters(it); break;
      case 'lists': applyLists(it); break;
      default: break;
    }
  }

  function connect() {
    reconnectTimer = null;
    var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
    var sock;
    try {
      sock = new WebSocket(url);
    } catch (err) {
      scheduleReconnect();
      return;
    }
    ws = sock;
    sock.onmessage = function (ev) {
      var m;
      try { m = JSON.parse(ev.data); } catch (err) { return; }
      if (m.type === 'snapshot') onSnapshot(m);
      else if (m.type === 'events' && Array.isArray(m.items)) m.items.forEach(onItem);
    };
    sock.onclose = function () {
      if (ws !== sock) return;
      ws = null;
      setConnected(false);
      scheduleReconnect();
    };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(connect, P.backoffMs(attempt++));
  }

  // ---- wiring -------------------------------------------------------------------
  function wire() {
    var sel = $('ring-alt');
    RING_ALTS_FT.forEach(function (ft) {
      var o = document.createElement('option');
      o.value = String(ft);
      o.textContent = P.thousands(ft) + ' ft';
      if (ft === DEFAULT_RING_FT) o.selected = true;
      sel.appendChild(o);
    });
    sel.addEventListener('change', updateRings);
    Object.keys(FILTER_INPUTS).forEach(function (k) {
      $(FILTER_INPUTS[k]).addEventListener('input', function () { onFilterInput(k); });
    });
    Array.prototype.forEach.call(document.querySelectorAll('button[data-reset]'), function (b) {
      b.addEventListener('click', function () {
        var k = b.getAttribute('data-reset');
        clearTimeout(putTimers[k]);
        putTimers[k] = null;
        putFilter(k, null);
      });
    });
    Array.prototype.forEach.call(document.querySelectorAll('form.list-add'), function (form) {
      form.addEventListener('submit', function (ev) {
        ev.preventDefault();
        var input = form.querySelector('input');
        var entry = input.value.trim();
        if (!entry) return;
        changeList(form.getAttribute('data-list'), 'add', entry).then(function () {
          if (!$('filter-msg').classList.contains('error')) input.value = '';
        });
      });
    });
    $('save-baseline').addEventListener('click', saveBaseline);
    $('details-close').addEventListener('click', deselect);
    $('table-toggle').addEventListener('click', toggleTable);
    Array.prototype.forEach.call(document.querySelectorAll('#table-view th'), function (th) {
      th.addEventListener('click', function () { sortBy(th.getAttribute('data-key')); });
      th.addEventListener('keydown', function (ev) {
        if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); sortBy(th.getAttribute('data-key')); }
      });
    });
    $('table-view').querySelector('tbody').addEventListener('click', function (ev) {
      var tr = ev.target.closest('tr');
      if (tr) select(tr.getAttribute('data-key'));
    });
    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape') deselect();
    });
    setInterval(prune, 2000);
    setInterval(function () {
      if (selectedKey) renderDetails(); // ages
      if (tableDirty) renderTable();
    }, 1000);
  }

  // Test hooks (tests/ui/ui.spec.js); read-only views of the page state.
  window.skycueApp = {
    markers: function () {
      var out = [];
      tracks.forEach(function (e) {
        if (!e.marker) return;
        var ll = e.marker.getLatLng();
        out.push({ hex: e.t.hex, addr_type: e.t.addr_type, decision: P.decisionColor(e.t.decision) || 'unknown',
          lat: ll.lat, lon: ll.lng });
      });
      return out;
    },
    rangeRingRadius: function () { return rangeRing ? rangeRing.getRadius() : null; },
    elevationRingRadius: function () { return elevRing ? elevRing.getRadius() : null; },
    selected: function () {
      var e = selectedKey && tracks.get(selectedKey);
      return e ? e.t.hex : null;
    },
    connected: function () { return connected; },
    // Adds a marker skycued does not know (the reconnect test checks it goes).
    injectTestMarker: function (t) { onTrack(t, true); }
  };

  wire();
  request('GET', 'api/tiles-available').catch(function () { return { tiles: false }; }).then(function (tiles) {
    initMap(tiles);
    connect();
  });
})();
