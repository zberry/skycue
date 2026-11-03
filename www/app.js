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
  var STATS_TIMEOUT_MS = 5000;      // no stats for this long: the connection is dead

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
  var ws = null, connected = false, attempt = 0, reconnectTimer = null, lastStatsWall = 0;
  var prevStats = null, prevStatsWall = 0;
  var tableSort = { key: 'hex', dir: 1 };
  var tableDirty = true;
  var editing = {};                 // filter key -> the input holds an uncommitted edit
  var actionsSig = '';
  var hideFiltered = false;         // view option, kept in localStorage; never sent to skycued

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
  // A PUT or POST always carries a JSON body and Content-Type: skycued
  // answers 415 without it (docs/web-api.md, Security). A DELETE carries
  // neither; its 204 (no body) resolves to null.
  function request(method, path, body) {
    var init = { method: method, headers: {} };
    if (method === 'PUT' || method === 'POST') {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body === undefined ? {} : body);
    }
    return fetch(path, init).then(function (r) {
      if (r.status === 204) return null;
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

  // Offline tiles may come from several make-tiles.sh regions with different
  // zoom ranges (a coarse whole world, finer areas). Where the zoom shown
  // has no tile, this layer shows the nearest lower-zoom ancestor that
  // exists (at most `fallbackLevels` up), enlarged and cropped to the square
  // (P.parentTile), instead of a blank one. A tile that exists but lies on
  // a region's edge (P.tileInsideRegions, from meta.json's regions) may be
  // partly transparent, so its nearest ancestor is drawn underneath it.
  // Each tile is a div clipping its img(s). URLs that 404 are remembered
  // for TILE_MISSING_MS, so later tiles go straight to a level that has them.
  var TILE_MISSING_MS = 60000;
  var tileLayer = null;
  var tileMissing = new Map();      // URL -> Date.now() when it failed
  var tileCounts = { loaded: 0, fallback: 0, underlay: 0, failed: 0 };
  function tileKnownMissing(url) {
    var t = tileMissing.get(url);
    if (t === undefined) return false;
    if (Date.now() - t < TILE_MISSING_MS) return true;
    tileMissing.delete(url);
    return false;
  }
  var FallbackTileLayer = L.TileLayer.extend({
    options: { fallbackMinZoom: 0, fallbackLevels: 4, regions: null },
    // Leaflet's getTileUrl for tile p (any zoom): it takes the zoom from
    // _getZoomForUrl, which follows _urlZoom while this runs. ({-y} would
    // use the shown zoom's range; the URL here has none.)
    ancestorUrl: function (p) {
      var c = L.point(p.x, p.y);
      c.z = p.z;
      this._urlZoom = p.z;
      try { return this.getTileUrl(c); } finally { this._urlZoom = null; }
    },
    _getZoomForUrl: function () {
      if (this._urlZoom === null || this._urlZoom === undefined) return L.TileLayer.prototype._getZoomForUrl.call(this);
      var z = this.options.zoomReverse ? this.options.maxZoom - this._urlZoom : this._urlZoom;
      return z + this.options.zoomOffset;
    },
    createTile: function (coords, done) {
      var layer = this, o = this.options, size = this.getTileSize();
      var tile = document.createElement('div');
      tile.style.overflow = 'hidden';
      tile.complete = false; // Leaflet's _abortLoading drops tiles not yet complete
      tile.setAttribute('data-tile', coords.z + '/' + coords.x + '/' + coords.y);
      // Leaflet takes an aborted or pruned tile out of the DOM: stop there.
      function dropped() { return !tile.parentNode; }
      function newImg() {
        var img = document.createElement('img');
        img.alt = '';
        img.setAttribute('role', 'presentation');
        img.style.position = 'absolute';
        return img;
      }
      // load(img, n, cb): img shows the first tile from n levels up that
      // exists; cb(p) once it has loaded, cb(null) when none does (maybe
      // synchronously).
      function load(img, n, cb) {
        var p = n === 0 ? { z: coords.z, x: coords.x, y: coords.y, scale: 1, offsetX: 0, offsetY: 0 }
          : P.parentTile(coords.z, coords.x, coords.y, n);
        if (!p || p.z < o.fallbackMinZoom || n > o.fallbackLevels) { cb(null); return; }
        var url = layer.ancestorUrl(p);
        if (tileKnownMissing(url)) { load(img, n + 1, cb); return; }
        img.style.width = size.x * p.scale + 'px';
        img.style.height = size.y * p.scale + 'px';
        img.style.left = -p.offsetX * size.x + 'px';
        img.style.top = -p.offsetY * size.y + 'px';
        img.onload = function () { if (!dropped()) cb(p); };
        img.onerror = function () {
          tileMissing.set(url, Date.now());
          if (!dropped()) load(img, n + 1, cb);
        };
        img.src = url;
      }
      var img = newImg();
      tile.appendChild(img);
      load(img, 0, function (p) {
        if (!p) {
          // Asynchronously: Leaflet registers the tile after createTile returns.
          setTimeout(function () {
            tile.complete = true;
            tileCounts.failed++;
            done(new Error('no tile for ' + coords.z + '/' + coords.x + '/' + coords.y), tile);
          }, 0);
          return;
        }
        tile.complete = true;
        if (p.scale > 1) {
          tileCounts.fallback++;
          tile.classList.add('tile-fallback');
          tile.setAttribute('data-fallback-zoom', String(p.z));
        } else {
          tileCounts.loaded++;
          if (o.regions && !P.tileInsideRegions(coords.z, coords.x, coords.y, o.regions)) {
            var under = newImg();
            under.className = 'tile-underlay';
            tile.insertBefore(under, img);
            load(under, 1, function (q) {
              if (q) {
                tileCounts.underlay++;
                tile.setAttribute('data-underlay-zoom', String(q.z));
              } else if (under.parentNode) {
                under.parentNode.removeChild(under);
              }
            });
          }
        }
        done(null, tile);
      });
      return tile;
    }
  });

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
      var tileMinZoom = typeof tiles.minZoom === 'number' ? tiles.minZoom : 0;
      tileLayer = new FallbackTileLayer('tiles/{z}/{x}/{y}.png', {
        minZoom: tileMinZoom,
        fallbackMinZoom: tileMinZoom,
        regions: Array.isArray(tiles.regions) ? tiles.regions : null,
        // The overall top zoom of all regions; elsewhere FallbackTileLayer
        // enlarges the region tiles below it.
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
    initFisbLayers();
    initMapView();
  }

  // ---- map style and overlays ------------------------------------------------
  // View options kept in localStorage (never sent to skycued): the map style
  // (Light: the tiles as baked; Dark: P.tileFilter on the tile pane only)
  // and two overlays from scripts/make-overlays.sh, Borders and Cities,
  // offered only when overlays/meta.json lists their files. Without tiles
  // the background is dark, so the overlays use the dark style.
  var VIEW_KEYS = { style: 'skycue.mapStyle', borders: 'skycue.overlay.borders', cities: 'skycue.overlay.cities' };
  var CITY_REDRAW_MS = 150;
  var view = { style: 'light', borders: false, cities: false };
  var overlayMeta = null;           // overlays/meta.json, or null when there is none
  var bordersLayer = null, bordersLoading = null;
  var citiesLayer = null, citiesData = null, citiesLoading = null, citiesFailed = false, cityTimer = null, cityShown = [];
  function storeGet(k) { try { return window.localStorage.getItem(k); } catch (err) { return null; } }
  function storeSet(k, v) { try { window.localStorage.setItem(k, v); } catch (err) { /* private window */ } }
  function overlayStyle() { return haveTiles ? view.style : 'dark'; }
  function overlayFile(name) {
    var f = overlayMeta && overlayMeta.files && overlayMeta.files[name];
    var file = f && typeof f.file === 'string' && /^[A-Za-z0-9_.-]+$/.test(f.file) ? f.file : name + '.json';
    return 'overlays/' + file;
  }

  function initMapView() {
    map.createPane('borders');
    map.getPane('borders').style.zIndex = 360;      // over the radar (350), under the shapes (400)
    map.getPane('borders').style.pointerEvents = 'none';
    map.createPane('cityLabels');
    map.getPane('cityLabels').style.zIndex = 450;   // over the shapes, under the aircraft (markerPane, 600)
    map.getPane('cityLabels').style.pointerEvents = 'none';
    var offer = P.overlayOffer(overlayMeta);
    var ViewControl = L.Control.extend({
      options: { position: 'topleft' },
      onAdd: function () {
        // The style only changes tiles: without them it is left out, and
        // the box too when there are no overlays either.
        var div = L.DomUtil.create('div', 'view-control');
        div.hidden = !haveTiles && !offer.borders && !offer.cities;
        div.innerHTML =
          '<div id="map-style-row"' + (haveTiles ? '' : ' hidden') + '>' +
          '<label for="map-style">Map</label> <select id="map-style">' +
          '<option value="light">Light</option><option value="dark">Dark</option></select></div>' +
          '<label class="check" for="ov-borders"' + (offer.borders ? '' : ' hidden') + '>' +
          '<input id="ov-borders" type="checkbox"> Borders</label>' +
          '<label class="check" for="ov-cities"' + (offer.cities ? '' : ' hidden') + '>' +
          '<input id="ov-cities" type="checkbox"> Cities</label>';
        L.DomEvent.disableClickPropagation(div);
        L.DomEvent.disableScrollPropagation(div);
        return div;
      }
    });
    new ViewControl().addTo(map);
    $('map-style').addEventListener('change', function () { setMapStyle($('map-style').value); });
    $('ov-borders').addEventListener('change', function () { setOverlay('borders', $('ov-borders').checked); });
    $('ov-cities').addEventListener('change', function () { setOverlay('cities', $('ov-cities').checked); });
    map.on('zoomend', function () { if (bordersLayer && map.hasLayer(bordersLayer)) restyleBorders(); });
    map.on('zoomend moveend', scheduleCities);
    setMapStyle(storeGet(VIEW_KEYS.style), true);
    setOverlay('borders', offer.borders && storeGet(VIEW_KEYS.borders) === '1', true);
    setOverlay('cities', offer.cities && storeGet(VIEW_KEYS.cities) === '1', true);
  }

  function setMapStyle(style, initial) {
    view.style = P.mapStyle(style);
    $('map-style').value = view.style;
    if (!initial) storeSet(VIEW_KEYS.style, view.style);
    var pane = map.getPane('tilePane');
    pane.style.filter = P.tileFilter(view.style);
    pane.classList.toggle('tiles-dark', view.style === 'dark');
    map.getContainer().classList.toggle('overlays-dark', overlayStyle() === 'dark');
    if (bordersLayer) restyleBorders();
  }

  function setOverlay(name, on, initial) {
    view[name] = !!on;
    $('ov-' + name).checked = view[name];
    if (!initial) storeSet(VIEW_KEYS[name], view[name] ? '1' : '0');
    if (name === 'borders') showBorders();
    else { if (!initial) citiesFailed = false; drawCities(); }
  }

  function fetchJson(url) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error(url + ': HTTP ' + r.status);
      return r.json();
    });
  }

  // Borders: one GeoJSON layer of admin0 and admin1 lines, each feature
  // tagged with its kind; admin1 is drawn transparent below zoom 5.
  function showBorders() {
    if (!view.borders) {
      if (bordersLayer) map.removeLayer(bordersLayer);
      return;
    }
    if (bordersLayer) { bordersLayer.addTo(map); restyleBorders(); return; }
    if (bordersLoading) return;
    var files = overlayMeta && overlayMeta.files ? overlayMeta.files : {};
    var kinds = ['admin0', 'admin1'].filter(function (k) { return files[k]; });
    bordersLoading = Promise.all(kinds.map(function (k) {
      return fetchJson(overlayFile(k)).then(function (g) {
        var feats = g && Array.isArray(g.features) ? g.features : [];
        feats.forEach(function (f) { f.properties = { kind: k }; });
        return feats;
      });
    })).then(function (lists) {
      bordersLayer = L.geoJSON({ type: 'FeatureCollection', features: [].concat.apply([], lists) }, {
        pane: 'borders', interactive: false,
        style: function (f) { return P.borderStyle(f.properties.kind, overlayStyle(), map.getZoom()); }
      });
      showBorders();
    }).catch(function (err) {
      console.warn('borders overlay: ' + err.message);
    }).then(function () { bordersLoading = null; });
  }
  function restyleBorders() {
    var style = overlayStyle(), z = map.getZoom();
    bordersLayer.setStyle(function (f) { return P.borderStyle(f.properties.kind, style, z); });
  }

  // Cities: labels for P.cullCities' choice in the view, redrawn (debounced)
  // after the map moves or zooms.
  function scheduleCities() {
    if (!view.cities) return;
    clearTimeout(cityTimer);
    cityTimer = setTimeout(drawCities, CITY_REDRAW_MS);
  }
  function viewBounds() {
    var b = map.getBounds().pad(0.1);
    var w = b.getWest(), e = b.getEast();
    if (e - w >= 360) { w = -180; e = 180; } else {
      w = ((w + 180) % 360 + 360) % 360 - 180;
      e = ((e + 180) % 360 + 360) % 360 - 180;
    }
    return { south: b.getSouth(), west: w, north: b.getNorth(), east: e };
  }
  function drawCities() {
    clearTimeout(cityTimer);
    if (!view.cities) {
      if (citiesLayer) citiesLayer.clearLayers();
      cityShown = [];
      return;
    }
    if (!citiesData) {
      if (citiesLoading || citiesFailed) return;   // a failed load is retried when the box is ticked again
      citiesLoading = fetchJson(overlayFile('cities')).then(function (g) {
        citiesData = (g && Array.isArray(g.features) ? g.features : []).map(function (f) {
          var p = f.properties || {}, c = f.geometry && f.geometry.coordinates;
          return { name: String(p.name || ''), rank: Number(p.rank), pop: Number(p.pop) || 0,
            lat: c ? Number(c[1]) : NaN, lon: c ? Number(c[0]) : NaN };
        }).filter(function (c) { return c.name && isFinite(c.lat) && isFinite(c.lon) && isFinite(c.rank); });
        drawCities();
      }).catch(function (err) {
        citiesFailed = true;
        console.warn('cities overlay: ' + err.message);
      }).then(function () { citiesLoading = null; });
      return;
    }
    if (!citiesLayer) citiesLayer = L.layerGroup().addTo(map);
    citiesLayer.clearLayers();
    cityShown = P.cullCities(citiesData, viewBounds(), map.getZoom());
    cityShown.forEach(function (c) {
      citiesLayer.addLayer(L.marker([c.lat, c.lon], {
        pane: 'cityLabels', interactive: false, keyboard: false,
        icon: L.divIcon({ className: 'city-label', iconSize: null,
          html: '<i class="city-dot"></i><span class="city-name">' + esc(c.name) + '</span>' })
      }));
    });
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
      map.fitBounds(rangeRing.getBounds(), { padding: [10, 10], animate: false });
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

  function dropLayers(e) {
    if (e.marker) map.removeLayer(e.marker);
    if (e.trail) map.removeLayer(e.trail);
    e.marker = null;
    e.trail = null;
  }

  // Draws or removes one entry's marker and trail according to its drawability
  // and the "Hide filtered" option. `observed`: add the position to the trail.
  function renderEntry(e, observed) {
    var t = e.t;
    var drawable = typeof t.lat === 'number' && typeof t.lon === 'number' && P.decisionColor(t.decision) !== null;
    if (!drawable || !map) return;
    if (observed) {
      var ts = Date.parse(t.position_time);
      e.pts.push({ ts: isNaN(ts) ? dataNow() : ts, ll: [t.lat, t.lon] });
      pruneTrail(e);
    }
    if (!P.trackVisible(t.decision, hideFiltered)) {
      dropLayers(e);
      if (e.key === selectedKey) deselect();
      return;
    }
    var fresh = !e.marker;
    ensureMarker(e);
    if (observed || fresh) e.trail.setLatLngs(e.pts.map(function (p) { return p.ll; }));
    styleMarker(e);
  }

  function updateHiddenCount() {
    var n = P.hiddenCount(Array.from(tracks.values()).map(function (e) { return e.t; }), hideFiltered);
    $('hidden-count').textContent = hideFiltered && n > 0 ? n + ' hidden' : '';
    return n;
  }

  function setHideFiltered(on) {
    hideFiltered = !!on;
    $('hide-filtered').checked = hideFiltered;
    try { window.localStorage.setItem('skycue.hideFiltered', hideFiltered ? '1' : '0'); } catch (err) { /* private window */ }
    tracks.forEach(function (e) { renderEntry(e, false); });
    updateHiddenCount();
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
    renderEntry(e, observed);
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
  // The side panel: a Track tab (the selected aircraft), a Weather tab
  // (FIS-B text reports and the clicked airspace shape) and a Receiver tab
  // (inputs, ground stations, decoder health). The tabs are the panel's
  // [role=tab] buttons, tab-<name> showing pane-<name>; a header button with
  // data-opens-tab="<name>" toggles the panel on that tab. TAB_SHOWN[name]
  // draws a tab when it is shown. A new tab: its markup and one entry here.
  var TAB_SHOWN = {
    weather: function () { renderWx(true); },
    receiver: function () { renderReceiver(); }
  };
  var activeTab = 'track';
  function eachEl(selector, fn) { Array.prototype.forEach.call(document.querySelectorAll(selector), fn); }
  function tabShown(tab) { return activeTab === tab && !$('details').hidden; }
  function syncTabToggles() {
    eachEl('[data-opens-tab]', function (b) {
      b.setAttribute('aria-pressed', String(tabShown(b.getAttribute('data-opens-tab'))));
    });
  }
  function setTab(tab) {
    activeTab = tab;
    eachEl('#details [role=tab]', function (b) {
      var t = b.id.replace(/^tab-/, '');
      b.setAttribute('aria-selected', String(t === tab));
      $('pane-' + t).hidden = t !== tab;
    });
    syncTabToggles();
    if (TAB_SHOWN[tab]) TAB_SHOWN[tab]();
  }
  function openPanel(tab) {
    var wasHidden = $('details').hidden;
    $('details').hidden = false;
    setTab(tab);
    if (wasHidden && map) map.invalidateSize({ pan: false });
  }
  function closePanel() {
    deselect();
    if ($('details').hidden) return;
    $('details').hidden = true;
    syncTabToggles();
    if (map) map.invalidateSize({ pan: false });
  }

  function select(key) {
    var te = tracks.get(key);
    if (te && !P.trackVisible(te.t.decision, hideFiltered)) return; // no marker to select
    var prev = selectedKey;
    selectedKey = key;
    [prev, key].forEach(function (k) {
      var e = k && tracks.get(k);
      if (e) styleMarker(e);
    });
    actionsSig = '';
    openPanel('track');
    renderDetails();
    $('details').focus({ preventScroll: true });
    tableDirty = true;
  }

  // Clears the aircraft selection; the panel closes with it when it shows
  // the Track tab (the Weather tab stays open).
  function deselect() {
    if (selectedKey === null) return;
    var e = tracks.get(selectedKey);
    selectedKey = null;
    if (e) styleMarker(e);
    renderDetails();
    if (activeTab === 'track' && !$('details').hidden) {
      $('details').hidden = true;
      if (map) map.invalidateSize({ pan: false });
    }
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
    $('track-empty').hidden = !!e;
    $('details-actions').hidden = !e;
    $('details-fields').hidden = !e;
    if (!e) {
      $('details-title').textContent = 'Track';
      return;
    }
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
      if (editing[k]) return; // the user's uncommitted edit wins
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

  // The input's value as a valid filter value, or null (marked invalid,
  // with the reason shown).
  function checkFilterInput(key) {
    var el = $(FILTER_INPUTS[key]);
    var raw = el.value.trim();
    var v = Number(raw);
    var ok = raw !== '' && isFinite(v) && (key === 'max_range_nm' ? v > 0 : v >= -90 && v <= 90);
    el.classList.toggle('invalid', !ok);
    if (!ok) {
      showMsg($('filter-msg'), key === 'max_range_nm' ? 'Max range must be a number > 0' :
        'Min elevation must be a number from -90 to 90', true);
      return null;
    }
    if ($('filter-msg').classList.contains('error')) showMsg($('filter-msg'), '');
    return v;
  }

  // While typing: only validate (and keep live updates off the field).
  function onFilterInput(key) {
    editing[key] = true;
    checkFilterInput(key);
  }

  // Committed (Enter or leaving the field): apply.
  function onFilterChange(key) {
    var v = checkFilterInput(key);
    if (v === null) return;
    editing[key] = false;
    if (cfg.filters && cfg.filters[key] === v) return;
    putFilter(key, v);
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
    applyInputLines(s.inputs, now);
    rx.stats = s;
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

  // ---- FIS-B: radar, rasters, airspace, weather ---------------------------------
  // docs/web-api.md, FIS-B. The snapshot carries only the status object; the
  // products come from /api/fisb/* after it, then the WebSocket items keep
  // them current (radar/raster items are notifications: GET the mosaic).
  var PX_PER_BIN = 4;               // canvas pixels per smallest bin
  var CANVAS_CAP = 4096;            // canvas width/height limit
  var FISB_POLL_MS = 5000;          // /api/fisb/status for the strip's rates
  var WX_DEBOUNCE_MS = 200;
  var WX_KINDS = [['metar', 'METAR'], ['taf', 'TAF'], ['pirep', 'PIREP'], ['winds', 'Winds aloft'],
    ['twgo', 'NOTAM / SIGMET text']];
  var SHAPE_LAYERS = [
    ['tfr', 'NOTAM / TFR'], ['sigmet', 'SIGMET / convective'], ['airmet', 'AIRMET / G-AIRMET'], ['cwa', 'CWA'], ['sua', 'SUA']
  ];
  var RADAR_NAMES = { 63: 'regional', 64: 'conus' };

  var fisb = {
    gen: 0,                         // bumped per snapshot: older responses are dropped
    loading: false, loaded: false, queue: [],
    status: null, prevStatus: null, prevStatusWall: 0,
    text: new Map(),                // key -> text record (METAR, TAF, PIREP, winds)
    shapes: new Map(),              // properties.key -> {fam, layers, props}
    shapeKey: null,                 // the shape shown in the panel
    rasterHeads: new Map(),         // raster layer key -> mosaic head
    rasterSel: null, raster: null,  // the chosen raster layer and its drawing
    wxDirty: true, wxFilter: '', wxTimer: null, wxExpanded: {}
  };
  var radar = {
    regional: { product: 63, label: 'Radar (regional)', head: null, drawn: null, fetching: false, again: false },
    conus: { product: 64, label: 'Radar (CONUS)', head: null, drawn: null, fetching: false, again: false }
  };
  var radarGroups = {}, shapeGroups = {}, rasterGroup = null;

  function swatchName(color, name) {
    return '<i class="sw" style="color:' + color + '"></i>' + esc(name);
  }

  function initFisbLayers() {
    map.createPane('fisbRaster');
    map.getPane('fisbRaster').style.zIndex = 350;   // under the shapes (overlayPane, 400)
    map.getPane('fisbRaster').style.pointerEvents = 'none';
    var overlays = {};
    ['conus', 'regional'].forEach(function (w) {
      radarGroups[w] = L.layerGroup().addTo(map);    // radar: on by default
    });
    overlays[swatchName('#02fd02', radar.regional.label)] = radarGroups.regional;
    overlays[swatchName('#008e00', radar.conus.label)] = radarGroups.conus;
    rasterGroup = L.layerGroup();
    overlays[swatchName('#54278f', 'Other rasters')] = rasterGroup;
    SHAPE_LAYERS.forEach(function (s) {
      shapeGroups[s[0]] = L.layerGroup();
      overlays[swatchName(P.SHAPE_COLORS[s[0]], s[1])] = shapeGroups[s[0]];
    });
    L.control.layers(null, overlays, { collapsed: false, position: 'topright' }).addTo(map);
    rasterGroup.on('add', function () { $('raster-box').hidden = false; loadRaster(); });
    rasterGroup.on('remove', function () { $('raster-box').hidden = true; });

    var WxControl = L.Control.extend({
      options: { position: 'bottomright' },
      onAdd: function () {
        var div = L.DomUtil.create('div', 'wx-control');
        div.innerHTML =
          '<div id="radar-age">radar –</div>' +
          '<label for="radar-opacity">Opacity</label> ' +
          '<input id="radar-opacity" type="range" min="0.2" max="1" step="0.05" value="0.7">' +
          '<div id="radar-legend" class="legend-row"></div>' +
          '<div id="shape-legend" class="legend-note">Airspace: <span class="ln-pending">dotted, dim</span>: not yet in force · ' +
          '<span class="ln-cancelled">dashed</span>: cancelled</div>' +
          '<div id="raster-box" hidden>' +
          '<label for="raster-select">Raster</label> <select id="raster-select"></select>' +
          '<div id="raster-legend"></div></div>';
        L.DomEvent.disableClickPropagation(div);
        L.DomEvent.disableScrollPropagation(div);
        return div;
      }
    });
    new WxControl().addTo(map);
    $('radar-opacity').addEventListener('input', applyRadarOpacity);
    $('raster-select').addEventListener('change', function () {
      fisb.rasterSel = $('raster-select').value || null;
      loadRaster();
    });
  }

  // ---- mosaics ----
  function rasterValueLevel(legend) {
    // Icing packs three fields into a value; its palette is indexed by one.
    if (!legend || !legend.palette_field || !Array.isArray(legend.fields)) return null;
    for (var i = 0; i < legend.fields.length; i++) {
      var f = legend.fields[i];
      if (f.name === legend.palette_field) return function (v) { return (v >> f.shift) & f.mask; };
    }
    return null;
  }

  // Paints a mosaic onto one canvas over its blocks' box (rows spaced in Web
  // Mercator, as the image overlay is stretched). Bad blocks are skipped.
  function paintMosaic(m) {
    var blocks = m.blocks || [];
    var bounds = P.mosaicBounds(blocks);
    if (!bounds) return null;
    var dlat = Infinity, dlon = Infinity;
    blocks.forEach(function (b) {
      dlat = Math.min(dlat, b.dlat);
      dlon = Math.min(dlon, b.dlon);
    });
    var size = P.mosaicCanvasSize(bounds, dlat, dlon, PX_PER_BIN, CANVAS_CAP);
    var canvas = document.createElement('canvas');
    canvas.width = size.width;
    canvas.height = size.height;
    var ctx = canvas.getContext('2d');
    var w = bounds[0][1], yN = P.mercY(bounds[1][0]);
    var kx = size.width / (bounds[1][1] - w), ky = size.height / (yN - P.mercY(bounds[0][0]));
    var toLevel = rasterValueLevel(m.legend);
    var colors = {};
    function colorOf(v) {
      if (!(v in colors)) colors[v] = P.dbzColor(toLevel ? toLevel(v) : v, m.palette);
      return colors[v];
    }
    var bins = 0, bad = 0;
    blocks.forEach(function (b) {
      var v;
      try { v = P.expandRle(b.rle); } catch (err) { bad++; return; }
      for (var r = 0; r < 4; r++) {
        var top = b.lat0 - r * b.dlat;
        var y0 = Math.round((yN - P.mercY(top)) * ky);
        var y1 = Math.max(y0 + 1, Math.round((yN - P.mercY(top - b.dlat)) * ky));
        var c = 0;
        while (c < 32) {
          var val = v[r * 32 + c], c1 = c + 1;
          while (c1 < 32 && v[r * 32 + c1] === val) c1++;
          var col = colorOf(val);
          if (col !== 'transparent') {
            var x0 = Math.round((b.lon0 + c * b.dlon - w) * kx);
            var x1 = Math.max(x0 + 1, Math.round((b.lon0 + c1 * b.dlon - w) * kx));
            ctx.fillStyle = col;
            ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
            bins += c1 - c;
          }
          c = c1;
        }
      }
    });
    return { canvas: canvas, bounds: bounds, blocks: blocks.length, bins: bins, bad: bad };
  }

  function canvasPainted(canvas) {
    if (!canvas) return 0;
    var d = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    var n = 0;
    for (var i = 3; i < d.length; i += 4) if (d[i]) n++;
    return n;
  }

  function mosaicOverlay(group, drawing, className) {
    var o = L.imageOverlay(drawing.canvas.toDataURL('image/png'), drawing.bounds, {
      pane: 'fisbRaster', className: className, interactive: false
    });
    group.clearLayers();
    group.addLayer(o);
    return o;
  }

  function radarOpacity() { return Number($('radar-opacity').value) || 0.7; }
  function productAgeMs(head) {
    var t = head && Date.parse(head.product_time);
    return t ? dataNow() - t : NaN;
  }
  function applyRadarOpacity() {
    var base = radarOpacity();
    Object.keys(radar).forEach(function (w) {
      var d = radar[w].drawn;
      if (!d) return;
      var age = productAgeMs(d.head);
      var faded = P.radarFaded(age, radar[w].product);
      d.overlay.setOpacity(P.radarOpacity(age, radar[w].product, base));
      var el = d.overlay.getElement();
      if (el) el.classList.toggle('faded', faded);
    });
    if (fisb.raster) fisb.raster.overlay.setOpacity(base);
  }

  function loadRadar(which) {
    var r = radar[which];
    if (r.fetching) { r.again = true; return; }
    r.fetching = true;
    var gen = fisb.gen;
    request('GET', 'api/fisb/radar?product=' + which).then(function (m) {
      if (gen !== fisb.gen) return;
      var d = paintMosaic(m);
      if (!d) { clearRadar(which); return; }
      d.head = m;
      d.overlay = mosaicOverlay(radarGroups[which], d, 'fisb-radar fisb-radar-' + which);
      r.drawn = d;
      if (which === 'regional' || !radar.regional.drawn) renderLegend($('radar-legend'), m, false);
      radarAgeTick();
    }).catch(function () {
      if (gen === fisb.gen) clearRadar(which);   // gone (404) or skycued away: the next status says
    }).then(function () {
      r.fetching = false;
      if (r.again) { r.again = false; loadRadar(which); }
    });
  }
  function clearRadar(which) {
    radar[which].drawn = null;
    radar[which].head = null;
    radarGroups[which].clearLayers();
  }

  function renderLegend(el, m, withTitle) {
    var lg = m.legend || {};
    var labels = {};
    (lg.levels || []).forEach(function (l) { labels[l.level] = l.label + (l.approximate ? ' (approximate)' : ''); });
    var items = (m.palette || []).filter(function (p) { return p.color && p.color !== 'transparent'; });
    el.innerHTML = (withTitle ? '<div class="legend-title">' + esc(lg.title || m.name || '') +
      (lg.approximate ? ' <span class="approx">(approximate)</span>' : '') +
      (typeof lg.altitude_ft === 'number' ? ' · ' + (lg.altitude_approximate ? '≈ ' : '') + P.thousands(lg.altitude_ft) + ' ft' : '') +
      '</div>' : '') +
      items.map(function (p) {
        var label = labels[p.level] !== undefined ? labels[p.level] : 'level ' + p.level;
        return '<span class="legend-item" title="' + esc(label) + '"><i class="sw" style="color:' + esc(p.color) + '"></i>' +
          (withTitle ? esc(label) : esc(String(p.level))) + '</span>';
      }).join('');
  }

  function rasterLabel(h) {
    var alt = '';
    if (typeof h.altitude_code === 'number') {
      var ft = (h.product === 71 || h.product === 91 ? 18000 : 2000) + h.altitude_code * 2000;
      alt = ' · ≈ ' + P.thousands(ft) + ' ft';
    }
    return (h.name || 'product ' + h.product) + alt;
  }
  function renderRasterSelect() {
    var sel = $('raster-select');
    var heads = Array.from(fisb.rasterHeads.values()).sort(function (a, b) {
      return a.product - b.product || (a.altitude_code || 0) - (b.altitude_code || 0);
    });
    var sig = heads.map(function (h) { return h.key; }).join(',');
    if (!fisb.rasterSel || !fisb.rasterHeads.has(fisb.rasterSel)) fisb.rasterSel = heads.length ? heads[0].key : null;
    if (sel.getAttribute('data-sig') !== sig) {
      sel.setAttribute('data-sig', sig);
      sel.innerHTML = heads.map(function (h) {
        return '<option value="' + esc(h.key) + '">' + esc(rasterLabel(h)) + '</option>';
      }).join('');
    }
    sel.disabled = !heads.length;
    if (fisb.rasterSel) sel.value = fisb.rasterSel;
  }
  function loadRaster() {
    if (!map.hasLayer(rasterGroup)) return;
    var h = fisb.rasterSel && fisb.rasterHeads.get(fisb.rasterSel);
    if (!h) { clearRaster(); return; }
    var gen = fisb.gen, key = h.key;
    var q = 'api/fisb/raster?product=' + h.product + (typeof h.altitude_code === 'number' ? '&altitude=' + h.altitude_code : '');
    request('GET', q).then(function (m) {
      if (gen !== fisb.gen || key !== fisb.rasterSel || !map.hasLayer(rasterGroup)) return;
      var d = paintMosaic(m);
      if (!d) { clearRaster(); return; }
      d.key = key;
      d.overlay = mosaicOverlay(rasterGroup, d, 'fisb-raster fisb-raster-' + key);
      fisb.raster = d;
      renderLegend($('raster-legend'), m, true);
      applyRadarOpacity();
    }).catch(function () { if (key === fisb.rasterSel) clearRaster(); });
  }
  function clearRaster() {
    fisb.raster = null;
    rasterGroup.clearLayers();
    $('raster-legend').innerHTML = fisb.rasterHeads.size ? '' : '<span class="msg">No raster products.</span>';
  }

  // ---- airspace shapes ----
  function shapeLayer(f, key) {
    var p = f.properties || {}, g = f.geometry;
    if (!g || !g.coordinates) return null;
    var style = P.shapeStyle(p.product, !!p.cancelled, !!p.pending);
    var layer, kind;
    if (g.type === 'Point') {
      var ll = [g.coordinates[1], g.coordinates[0]];
      if (p.geom === 'circle' && p.radius_nm > 0) {
        layer = L.circle(ll, Object.assign({}, style, { radius: p.radius_nm * NM }));
        kind = 'circle';
      } else {
        layer = L.circleMarker(ll, Object.assign({}, style, { radius: 5, fillOpacity: 0.5 }));
        kind = 'point';
      }
    } else {
      layer = L.geoJSON(f, { style: function () { return style; } });
      kind = g.type === 'LineString' ? 'polyline' : 'polygon';
    }
    var leaves = layer instanceof L.LayerGroup ? layer.getLayers() : [layer];
    leaves.forEach(function (l) {
      l.on('add', function () {
        var el = l.getElement();
        if (el) el.setAttribute('data-key', key);
      });
    });
    layer.on('click', function (ev) {
      L.DomEvent.stopPropagation(ev);
      showShape(key);
    });
    layer.fisbKind = kind;
    return layer;
  }
  function removeShapes(key) {
    var s = fisb.shapes.get(key);
    if (!s) return;
    s.layers.forEach(function (l) { shapeGroups[s.fam].removeLayer(l); });
    fisb.shapes.delete(key);
    if (fisb.shapeKey === key) hideShape();
  }
  function setShapes(key, features) {
    var shown = fisb.shapeKey === key;
    var s = fisb.shapes.get(key);
    if (s) {
      s.layers.forEach(function (l) { shapeGroups[s.fam].removeLayer(l); });
      fisb.shapes.delete(key);
    }
    var fam = features && features.length ? P.shapeFamily(features[0].properties.product) : null;
    if (!fam) {
      if (shown) hideShape();
      return;
    }
    var layers = [];
    features.forEach(function (f) {
      var l = shapeLayer(f, key);
      if (l) {
        layers.push(l);
        shapeGroups[fam].addLayer(l);
      }
    });
    fisb.shapes.set(key, { fam: fam, layers: layers, props: features[0].properties });
    if (shown) showShape(key);
  }

  // A TWGO time: date and time, and how far from the data clock (NOTAM
  // ends are often days ahead).
  function fmtWhen(iso) {
    var ms = Date.parse(iso);
    if (isNaN(ms)) return String(iso);
    var d = ms - dataNow(), a = Math.abs(d);
    var rel = a < 60000 ? Math.round(a / 1000) + ' s' : a < 3600000 ? Math.round(a / 60000) + ' min' :
      a < 172800000 ? Math.floor(a / 3600000) + ' h ' + Math.round(a % 3600000 / 60000) + ' min' :
      Math.round(a / 86400000) + ' d';
    return new Date(ms).toISOString().substr(0, 19).replace('T', ' ') + 'Z · ' + (d > 0 ? 'in ' + rel : rel + ' ago');
  }
  function altText(p) {
    var lo = p.alt_low_ft, hi = p.alt_high_ft;
    if (typeof lo !== 'number' && typeof hi !== 'number') return null;
    if (!lo && !hi) return null;   // no altitudes given (point NOTAMs)
    var range = typeof lo === 'number' && typeof hi === 'number' ? P.thousands(lo) + ' – ' + P.thousands(hi) :
      typeof lo === 'number' ? 'from ' + P.thousands(lo) : 'up to ' + P.thousands(hi);
    return range + ' ft ' + (p.agl ? 'AGL' : 'MSL');
  }
  function showShape(key) {
    var s = fisb.shapes.get(key);
    if (!s) return;
    var p = s.props;
    fisb.shapeKey = key;
    var card = $('shape-details');
    card.setAttribute('data-key', key);
    $('shape-title').textContent = (p.name || 'product ' + p.product) + (p.label || p.location ? ' · ' + (p.label || p.location) : '');
    var rows = [
      ['product', 'Product', (p.name || '') + ' (' + p.product + ')'],
      ['key', 'Key', key],
      ['location', 'Location', p.location || null],
      ['start', 'Start', p.start ? fmtWhen(p.start) : null],
      ['pending', 'In force', p.pending ? 'not yet (dashed, dimmed until its start)' : null],
      ['text_end', 'Text end', p.text_end ? fmtWhen(p.text_end) : null],
      ['graphics_end', 'Graphics end', p.graphics_end ? fmtWhen(p.graphics_end) : null],
      ['alt', 'Altitudes', altText(p)],
      ['cancelled', 'Cancelled', yesNo(p.cancelled)]
    ];
    $('shape-fields').innerHTML = rows.filter(function (r) { return r[2] !== null && r[2] !== undefined; }).map(function (r) {
      return '<tr><th scope="row">' + esc(r[1]) + '</th><td data-field="' + r[0] + '">' + esc(r[2]) + '</td></tr>';
    }).join('');
    var pre = card.querySelector('pre.shape-text');
    pre.textContent = p.text || '';
    pre.hidden = !p.text;
    card.hidden = false;
    openPanel('weather');
    card.scrollIntoView({ block: 'nearest' });
  }
  function hideShape() {
    fisb.shapeKey = null;
    $('shape-details').hidden = true;
  }

  // ---- text reports ----
  function upsertText(rec) {
    if (!rec || !rec.key || !rec.kind) return;
    if (rec.kind === 'twgo') {
      if (rec.has_graphics) return;   // drawn (fisb_shape), not listed
      // A TWGO record without graphics: new, or amended (the store dropped
      // the old graphics with the old text), so no shape of it stays drawn.
      setShapes(rec.key, []);
      rec = Object.assign({}, rec, { station: rec.location || rec.label || '', issued: rec.last_received || rec.received });
    }
    fisb.text.set(rec.key, rec);
    fisb.wxDirty = true;
  }
  function dropText(key) {
    if (fisb.text.delete(key)) fisb.wxDirty = true;
  }
  function hhmm(iso) {
    var ms = Date.parse(iso);
    return isNaN(ms) ? '—' : new Date(ms).toISOString().substr(11, 5) + 'Z';
  }
  function shortAge(ms, now) { return P.formatAge(ms, now).replace(/ ago$/, ''); }

  function renderWx(force) {
    if (!force && (!fisb.wxDirty || $('pane-weather').hidden || $('details').hidden)) return;
    fisb.wxDirty = false;
    var now = dataNow(), prefix = fisb.wxFilter;
    var groups = {};
    WX_KINDS.forEach(function (k) { groups[k[0]] = []; });
    var total = 0;
    fisb.text.forEach(function (r) {
      if (!groups[r.kind]) return;
      total++;
      if (P.stationMatches(r.station, prefix)) groups[r.kind].push(r);
    });
    var shown = 0;
    $('wx-list').innerHTML = WX_KINDS.map(function (k) {
      var list = groups[k[0]].sort(function (a, b) {
        return a.station < b.station ? -1 : a.station > b.station ? 1 : Date.parse(a.issued) - Date.parse(b.issued);
      });
      shown += list.length;
      return '<section class="wx-group" data-kind="' + k[0] + '"><h3>' + k[1] + ' <span class="n">' + list.length +
        '</span></h3>' + (list.length ? '<ul>' + list.map(function (r) {
        var open = !!fisb.wxExpanded[r.key];
        var t = Date.parse(r.issued);
        return '<li class="wx-row age-' + P.ageClass(r.issued, now) + '" data-key="' + esc(r.key) + '" data-kind="' + r.kind +
          '" data-station="' + esc(r.station) + '"><button type="button" class="wx-head" aria-expanded="' + open + '">' +
          '<span class="wx-st">' + esc(r.station || '—') + '</span>' +
          (r.kind === 'twgo' ? '<span class="wx-name">' + esc(r.name || 'product ' + r.product) + '</span>' : '') +
          '<span class="wx-issued">' + hhmm(r.issued) + '</span>' +
          (r.valid ? '<span class="wx-valid">valid ' + hhmm(r.valid) + '</span>' : '') +
          '<span class="wx-age">' + (isNaN(t) ? '' : shortAge(t, now)) + '</span></button>' +
          '<pre class="wx-text"' + (open ? '' : ' hidden') + '>' + esc(r.text || '') + '</pre></li>';
      }).join('') + '</ul>' : '<p class="msg">None' + (prefix ? ' matching' : '') + '.</p>') + '</section>';
    }).join('');
    $('wx-summary').textContent = prefix ? shown + ' of ' + total + ' reports match "' + prefix.toUpperCase() + '"' :
      total + ' reports' + (fisb.loaded ? '' : ' (loading…)');
  }

  // ---- status strip ----
  function applyFisbStatus(st) {
    if (!st) return;
    var now = Date.now();
    fisb.status = st;
    updateStationMarkers(st.stations || []);
    setStat('stat-fisb-stations', String((st.stations || []).length));
    var prev = fisb.prevStatus;
    if (prev && now > fisb.prevStatusWall) {
      var perMin = 60000 / (now - fisb.prevStatusWall);
      setStat('stat-fisb-uplinks', (Math.max(0, (st.uplinks || 0) - (prev.uplinks || 0)) * perMin).toFixed(0));
      setStat('stat-fisb-products', (Math.max(0, (st.products || 0) - (prev.products || 0)) * perMin).toFixed(0));
    }
    fisb.prevStatus = st;
    fisb.prevStatusWall = now;
    if (!st.enabled) return;
    // A publish whose notification was missed: fetch it. Not a mosaic
    // still filling (its block count moves between publishes).
    Object.keys(radar).forEach(function (w) {
      var h = st.radar && st.radar[w];
      if (!h) {
        if (radar[w].drawn && fisb.loaded) clearRadar(w);
        return;
      }
      radar[w].head = h;
      var d = radar[w].drawn;
      if (P.radarNeedsFetch(d && d.head, h) && !radar[w].fetching) loadRadar(w);
    });
    var skew = P.skewWarning(st.clock_skew_s), sk = $('stat-fisb-skew');
    sk.hidden = !skew;
    $('stat-fisb-skew-text').textContent = skew || '';
    var keys = {};
    (st.rasters || []).forEach(function (h) { keys[h.key] = true; fisb.rasterHeads.set(h.key, h); });
    Array.from(fisb.rasterHeads.keys()).forEach(function (k) { if (!keys[k]) fisb.rasterHeads.delete(k); });
    renderRasterSelect();
    if (fisb.raster && !fisb.rasterHeads.has(fisb.raster.key)) clearRaster();
  }
  function pollFisb() {
    if (!connected) return;
    var gen = fisb.gen;
    request('GET', 'api/fisb/status').then(function (st) {
      if (gen === fisb.gen) applyFisbStatus(st);
    }).catch(function () { /* the WebSocket watchdog reports a dead skycued */ });
  }
  function radarAgeTick() {
    var parts = [], newest = NaN;
    Object.keys(radar).forEach(function (w) {
      var d = radar[w].drawn;
      if (!d) return;
      var age = productAgeMs(d.head);
      if (!(newest <= age)) newest = age;
      parts.push((w === 'conus' ? 'CONUS' : 'regional') + ' ' + hhmm(d.head.product_time) + ' · ' +
        shortAge(Date.parse(d.head.product_time), dataNow()) + (P.radarFaded(age, radar[w].product) ? ' (stale)' : ''));
    });
    $('radar-age').textContent = parts.length ? 'Radar ' + parts.join(', ') : 'No radar';
    setStat('stat-radar-age', isNaN(newest) ? '–' : shortAge(dataNow() - newest, dataNow()));
    applyRadarOpacity();
  }

  // ---- snapshot and items ----
  function fisbReset(st) {
    fisb.gen++;
    var gen = fisb.gen;
    fisb.text.clear();
    Array.from(fisb.shapes.keys()).forEach(removeShapes);
    hideShape();
    Object.keys(radar).forEach(clearRadar);
    fisb.rasterHeads.clear();
    clearRaster();
    fisb.prevStatus = null;
    fisb.loaded = false;
    fisb.queue = [];
    fisb.wxDirty = true;
    if (!st || !st.enabled) {
      fisb.loading = false;
      fisb.loaded = true;
      applyFisbStatus(st);
      renderWx();
      return;
    }
    fisb.loading = true;
    applyFisbStatus(st);
    Promise.all([request('GET', 'api/fisb/text'), request('GET', 'api/fisb/shapes'),
      request('GET', 'api/fisb/text?type=twgo')]).then(function (r) {
      if (gen !== fisb.gen) return;
      (r[0].records || []).forEach(upsertText);
      (r[2].records || []).forEach(upsertText);
      var byKey = new Map();
      (r[1].features || []).forEach(function (f) {
        var k = f.properties && f.properties.key;
        if (!k) return;
        if (!byKey.has(k)) byKey.set(k, []);
        byKey.get(k).push(f);
      });
      byKey.forEach(function (fs, k) { setShapes(k, fs); });
    }).catch(function () { /* reloaded with the next snapshot */ }).then(function () {
      if (gen !== fisb.gen) return;
      fisb.loading = false;
      var q = fisb.queue;
      fisb.queue = [];
      q.forEach(onFisbItem);
      fisb.loaded = true;
      fisb.wxDirty = true;
      renderWx();
    });
  }

  function onFisbItem(it) {
    if (fisb.loading) { fisb.queue.push(it); return; }
    switch (it.type) {
      case 'fisb_text': upsertText(it.record); break;
      case 'fisb_shape':
        dropText(it.key);   // a text-only TWGO record that now has graphics
        setShapes(it.key, it.features || []);
        break;
      case 'fisb_radar': {
        var w = RADAR_NAMES[it.product];
        if (w) { radar[w].head = it; loadRadar(w); }
        break;
      }
      case 'fisb_raster':
        fisb.rasterHeads.set(it.key, it);
        renderRasterSelect();
        if (it.key === fisb.rasterSel) loadRaster();
        break;
      case 'fisb_expired':
        if (it.kind === 'text') {
          dropText(it.key);
        } else if (it.kind === 'twgo') {
          removeShapes(it.key);
          dropText(it.key);
        } else if (it.kind === 'radar') {
          var which = RADAR_NAMES[it.product] || (it.key === '64' ? 'conus' : it.key === '63' ? 'regional' : null);
          if (which) clearRadar(which);
        } else if (it.kind === 'raster') {
          fisb.rasterHeads.delete(it.key);
          renderRasterSelect();
          if (fisb.raster && fisb.raster.key === it.key) clearRaster();
        }
        break;
      default: break;
    }
  }

  // ---- Receiver tab ------------------------------------------------------------------
  // docs/web-api.md, Inputs. The list comes with the snapshot, the `inputs`
  // items (a change) and GET /api/inputs every INPUTS_POLL_MS (device status,
  // status poll failures and data ages are not changes); the live counters
  // and `connected` with every stats item. Everything an input's definition
  // or the device says is shown with textContent, never as markup.
  var INPUTS_POLL_MS = 5000;
  var NUMERIC_FIELDS = { port: true, sac: true, sic: true, status_interval_s: true };
  var DOT_TITLES = { green: 'connected, receiving', amber: 'connected, no data for 30 s',
    red: 'disconnected, or the status poll failed 3 times', grey: 'disabled' };
  var ACT_LABELS = { toggle: 'Disable', edit: 'Edit', remove: 'Remove', show: 'Show' };
  var DEVICE_SVG = '<svg width="16" height="16" viewBox="-8 -8 16 16" aria-hidden="true">' +
    '<path d="M0,-7 L7,0 L0,7 L-7,0 Z" fill="currentColor" stroke="#0d1117" stroke-width="1.5"/></svg>';
  var STATION_SVG = '<svg width="16" height="16" viewBox="-8 -8 16 16" aria-hidden="true">' +
    '<path d="M0,-7 L7,6 L-7,6 Z" fill="currentColor" stroke="#0d1117" stroke-width="1.5"/></svg>';
  var HEALTH_ROWS = [
    ['metar', 'METAR held'], ['taf', 'TAF held'], ['pirep', 'PIREP held'], ['winds', 'Winds aloft held'],
    ['other', 'Other text held'], ['twgo', 'NOTAM / SIGMET held'], ['cancelled', 'Cancelled (30 min)'],
    ['bad_frames', 'Bad frames'], ['segments_open', 'Segments open'], ['mosaic_blocks', 'Mosaic blocks'],
    ['last_uplink_time', 'Last uplink'], ['uplink_duplicates', 'Uplink duplicates'],
    ['recorder_dropped', 'Recorder drops (received / sent)']
  ];
  var rx = {
    inputs: [],                     // input objects, as GET /api/inputs lists them
    wall: 0,                        // Date.now() when they arrived (for the data ages)
    rates: {},                      // id -> {obs_s, msg_s}
    dataWall: {},                   // id -> Date.now() of the stats item its message count last grew in
    prevLines: null, prevWall: 0,   // the previous stats.inputs
    stats: null,                    // the latest stats object (recorder drops)
    cards: new Map(),               // id -> card element
    devices: new Map(),             // id -> device marker
    stations: new Map(),            // site_id -> station marker
    editing: null                   // the id of the input the form edits; null: it adds one
  };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function inputById(id) {
    for (var i = 0; i < rx.inputs.length; i++) if (rx.inputs[i].id === id) return rx.inputs[i];
    return null;
  }
  function devicePos(inp) {
    var ds = inp.state && inp.state.device_status;
    return ds && typeof ds.lat === 'number' && typeof ds.lon === 'number' ? [ds.lat, ds.lon] : null;
  }

  function applyInputs(list) {
    rx.inputs = Array.isArray(list) ? list : [];
    rx.wall = Date.now();
    updateInputsHeader();
    updateDeviceMarkers();
    if (tabShown('receiver')) renderInputs();
  }
  // stats.inputs: the rates, and each input's counters and `connected`.
  function applyInputLines(lines, now) {
    if (!Array.isArray(lines)) return;
    rx.rates = P.inputRates(rx.prevLines, lines, rx.prevLines ? (now - rx.prevWall) / 1000 : 0);
    lines.forEach(function (l) { if (rx.rates[l.id].msg_s > 0) rx.dataWall[l.id] = now; });
    rx.prevLines = lines;
    rx.prevWall = now;
    lines.forEach(function (l) {
      var inp = inputById(l.id);
      if (!inp || !inp.state) return;
      ['connected', 'messages', 'observations', 'uplinks', 'rejected'].forEach(function (k) { inp.state[k] = l[k]; });
    });
    updateInputsHeader();
  }
  function updateInputsHeader() {
    var n = P.inputsSummary(rx.inputs);
    setStat('stat-inputs', n.connected + '/' + n.enabled, n.enabled > 0 && n.connected === n.enabled ? 'on' : 'off');
  }
  function refreshInputs() {
    return request('GET', 'api/inputs').then(function (j) { applyInputs(j.inputs); });
  }
  function pollInputs() {
    if (connected) refreshInputs().catch(function () { /* the WebSocket watchdog reports a dead skycued */ });
  }

  function makeCard(inp) {
    var card = el('div', 'input-card');
    card.setAttribute('data-id', inp.id);
    var head = el('div', 'input-head');
    head.appendChild(el('span', 'dot'));
    head.appendChild(el('span', 'input-name'));
    head.appendChild(el('span', 'input-type'));
    card.appendChild(head);
    card.appendChild(el('div', 'input-endpoint'));
    var line = el('div', 'input-stats');
    ['input-age', 'input-rates', 'input-rejected'].forEach(function (c) { line.appendChild(el('span', c)); });
    card.appendChild(line);
    card.appendChild(el('div', 'input-device'));
    card.appendChild(el('div', 'input-error'));
    var acts = el('div', 'input-actions');
    (inp.builtin ? ['toggle'] : ['toggle', 'edit', 'remove', 'show']).forEach(function (a) {
      var b = el('button', null, ACT_LABELS[a]);
      b.type = 'button';
      b.setAttribute('data-act', a);
      acts.appendChild(b);
    });
    card.appendChild(acts);
    return card;
  }
  function setText(card, cls, text) {
    var e = card.querySelector('.' + cls);
    if (e.textContent !== text) e.textContent = text;
    return e;
  }
  function deviceLine(inp, now) {
    var ds = inp.state && inp.state.device_status;
    if (!ds) return { text: 'no device status yet', stale: false };
    var parts = [];
    if (ds.gps_status !== undefined) parts.push('GPS ' + ds.gps_status);
    if (ds.receiver_status !== undefined) parts.push('receiver ' + ds.receiver_status);
    if (ds.version) parts.push('firmware ' + ds.version);
    var stale = P.statusStale(ds, inp.status_interval_s || 30, now);
    var at = Date.parse(ds.received_at);
    if (!isNaN(at)) parts.push('status ' + P.formatAge(at, now) + (stale ? ' (stale)' : ''));
    return { text: parts.join(' · '), stale: stale };
  }
  function fillCard(card, inp, now) {
    var st = inp.state || {}, color = P.inputDot(inp, now);
    var dot = card.querySelector('.dot');
    dot.className = 'dot dot-' + color;
    dot.title = DOT_TITLES[color];
    setText(card, 'input-name', inp.name || inp.id);
    setText(card, 'input-type', (inp.name ? inp.id + ' · ' : '') + inp.type);
    setText(card, 'input-endpoint', P.inputEndpoint(inp));
    // The last data: from the list's age, or newer from the stats counters.
    var dataAt = typeof st.last_data_age_s === 'number' ? rx.wall - st.last_data_age_s * 1000 : NaN;
    var counted = rx.dataWall[inp.id];
    if (counted && !(counted <= dataAt)) dataAt = counted;
    setText(card, 'input-age', isNaN(dataAt) ? 'no data yet' : 'data ' + P.formatAge(dataAt, Date.now()));
    var r = rx.rates[inp.id] || { obs_s: null, msg_s: null };
    var rates = setText(card, 'input-rates', 'obs/s ' + P.formatNum(r.obs_s, 1) + ' · msg/s ' + P.formatNum(r.msg_s, 1));
    rates.setAttribute('data-obs-s', r.obs_s === null ? '' : String(r.obs_s));
    setText(card, 'input-rejected', 'rejected ' + (st.rejected || 0));
    var ping = inp.type === 'pingstation_json' || inp.type === 'pingstation_cat021';
    var dev = card.querySelector('.input-device');
    dev.hidden = !ping;
    if (ping) {
      var d = deviceLine(inp, now);
      setText(card, 'input-device', d.text);
      dev.classList.toggle('stale', d.stale);
    }
    setText(card, 'input-error', st.last_error ? 'last error: ' + st.last_error : '').hidden = !st.last_error;
    card.querySelector('[data-act=toggle]').textContent = inp.enabled ? 'Disable' : 'Enable';
    var show = card.querySelector('[data-act=show]');
    if (show) show.hidden = !devicePos(inp);
  }
  function renderInputs() {
    var list = $('inputs-list'), now = dataNow(), seen = {};
    rx.inputs.forEach(function (inp, i) {
      seen[inp.id] = true;
      var card = rx.cards.get(inp.id);
      if (!card) {
        card = makeCard(inp);
        rx.cards.set(inp.id, card);
      }
      if (list.children[i] !== card) list.insertBefore(card, list.children[i] || null);
      fillCard(card, inp, now);
    });
    rx.cards.forEach(function (card, id) {
      if (seen[id]) return;
      card.remove();
      rx.cards.delete(id);
    });
    list.classList.toggle('empty', !rx.inputs.length);
  }
  function renderReceiver() {
    renderInputs();
    renderStations();
    renderHealth();
  }

  function updateDeviceMarkers() {
    if (!map) return;
    var seen = {};
    rx.inputs.forEach(function (inp) {
      var ll = devicePos(inp);
      if (!ll) return;
      seen[inp.id] = true;
      var title = (inp.name || inp.id) + ': device position';
      var m = rx.devices.get(inp.id);
      if (!m) {
        m = L.marker(ll, {
          icon: L.divIcon({ className: 'device-marker', html: DEVICE_SVG, iconSize: [16, 16], iconAnchor: [8, 8] }),
          title: title, keyboard: false, zIndexOffset: -500
        }).addTo(map);
        rx.devices.set(inp.id, m);
      } else {
        m.setLatLng(ll);
      }
      var e = m.getElement();
      if (e && e.title !== title) e.title = title;
    });
    rx.devices.forEach(function (m, id) {
      if (seen[id]) return;
      m.remove();
      rx.devices.delete(id);
    });
  }
  function updateStationMarkers(stations) {
    if (!map) return;
    var seen = {};
    stations.forEach(function (s) {
      if (typeof s.lat !== 'number' || typeof s.lon !== 'number') return;
      seen[s.site_id] = true;
      var m = rx.stations.get(s.site_id);
      if (!m) {
        m = L.marker([s.lat, s.lon], {
          icon: L.divIcon({ className: 'station-marker', html: STATION_SVG, iconSize: [16, 16], iconAnchor: [8, 8] }),
          title: 'FIS-B ground station ' + s.site_id, keyboard: false, zIndexOffset: -600
        }).addTo(map);
        rx.stations.set(s.site_id, m);
      } else {
        m.setLatLng([s.lat, s.lon]);
      }
    });
    rx.stations.forEach(function (m, id) {
      if (seen[id]) return;
      m.remove();
      rx.stations.delete(id);
    });
    if (tabShown('receiver')) renderStations();
  }
  function renderStations() {
    var ul = $('stations-list'), stations = (fisb.status && fisb.status.stations) || [], now = dataNow();
    ul.textContent = '';
    if (!stations.length) ul.appendChild(el('li', 'empty', 'None heard in the last 60 min.'));
    stations.forEach(function (s) {
      var li = el('li');
      li.appendChild(el('span', 'st-id', s.site_id));
      li.appendChild(el('span', 'st-pos', typeof s.lat === 'number' ? s.lat.toFixed(4) + ', ' + s.lon.toFixed(4) : '—'));
      li.appendChild(el('span', 'st-count', s.count + ' products'));
      var t = Date.parse(s.last_seen);
      li.appendChild(el('span', 'st-age', isNaN(t) ? '' : 'heard ' + P.formatAge(t, now)));
      ul.appendChild(li);
    });
  }
  function uplinkTime(iso) {
    var ms = Date.parse(iso);
    if (isNaN(ms)) return '—';
    var t = new Date(ms).toISOString();
    return t.substr(0, 10) + ' ' + t.substr(11, 8) + 'Z';
  }
  function renderHealth() {
    var st = fisb.status || {}, counts = st.counts || {}, s = rx.stats || {};
    var body = $('decoder-health').tBodies[0];
    body.textContent = '';
    HEALTH_ROWS.forEach(function (row) {
      var k = row[0], v;
      if (k === 'last_uplink_time') v = uplinkTime(st.last_uplink_time);
      else if (k === 'recorder_dropped') v = (s.recorder_dropped_received || 0) + ' / ' + (s.recorder_dropped_sent || 0);
      else if (k in counts) v = String(counts[k]);
      else v = String(st[k] || 0);
      var tr = el('tr');
      tr.setAttribute('data-key', k);
      tr.appendChild(el('th', null, row[1]));
      tr.appendChild(el('td', null, v));
      body.appendChild(tr);
    });
  }

  // ---- the add / edit form ----
  function formEl(name) { return $('input-form').elements[name]; }
  function formFields() { return P.inputFields(formEl('type').value, formEl('transport').value); }
  function syncFormFields() {
    var shown = formFields();
    eachEl('#input-form [data-field]', function (l) {
      var f = l.getAttribute('data-field');
      l.hidden = f !== 'type' && shown.indexOf(f) < 0;
    });
  }
  // The definition the form holds; a field left empty is not sent, so
  // skycued's default applies.
  function formDef() {
    var def = { type: formEl('type').value };
    formFields().forEach(function (f) {
      var e = formEl(f);
      if (e.type === 'checkbox') {
        def[f] = e.checked;
        return;
      }
      var v = f === 'name' ? e.value : e.value.trim();
      if (v !== '') def[f] = NUMERIC_FIELDS[f] ? Number(v) : v;
    });
    return def;
  }
  function showFieldErrors(errors) {
    eachEl('#input-form .field-error', function (e) {
      var f = e.getAttribute('data-for'), msg = errors[f] || '';
      e.textContent = msg;
      var input = formEl(f);
      input.classList.toggle('invalid', !!msg);
      if (msg) input.setAttribute('aria-invalid', 'true'); else input.removeAttribute('aria-invalid');
    });
  }
  function showFormError(text) { $('input-form').querySelector('.form-error').textContent = text || ''; }
  function resetForm() {
    var form = $('input-form');
    form.reset();
    rx.editing = null;
    formEl('id').readOnly = false;
    formEl('type').disabled = false;
    $('input-form-title').textContent = 'Add input';
    form.querySelector('button[type=submit]').textContent = 'Add';
    form.querySelector('[data-act=cancel]').hidden = true;
    showFieldErrors({});
    showFormError('');
    syncFormFields();
  }
  // A built-in's card has no Edit: its enable/disable button is all it takes.
  function editInput(inp) {
    resetForm();
    rx.editing = inp.id;
    var form = $('input-form');
    formEl('type').value = inp.type;
    formEl('type').disabled = true;
    if (inp.transport) formEl('transport').value = inp.transport;
    P.inputFields(inp.type, inp.transport).forEach(function (f) {
      var e = formEl(f), v = inp[f];
      if (e.type === 'checkbox') e.checked = !!v;
      else e.value = v === undefined || v === null ? '' : String(v);
    });
    formEl('id').readOnly = true;
    $('input-form-title').textContent = 'Edit ' + inp.id;
    form.querySelector('button[type=submit]').textContent = 'Save';
    form.querySelector('[data-act=cancel]').hidden = false;
    syncFormFields();
    form.scrollIntoView({ block: 'nearest' });
  }
  function submitInput(ev) {
    ev.preventDefault();
    var def = formDef(), id = rx.editing;
    var check = P.validateInputDef(def);
    showFieldErrors(check.errors);
    showFormError('');
    if (!check.ok) return;
    var submit = $('input-form').querySelector('button[type=submit]');
    submit.disabled = true;
    var req = id === null ? request('POST', 'api/inputs', def) :
      request('PUT', 'api/inputs/' + encodeURIComponent(id), def);
    req.then(function () {
      resetForm();
      return refreshInputs();
    }).catch(function (err) {
      showFormError(err.message); // skycued's message, word for word
    }).then(function () { submit.disabled = false; });
  }
  function onFormInput(ev) {
    var name = ev.target.name;
    if (name === 'type' || name === 'transport') syncFormFields();
    // Once marked, the marks follow the typing.
    if ($('input-form').querySelector('.invalid')) showFieldErrors(P.validateInputDef(formDef()).errors);
  }
  function onCardClick(ev) {
    var b = ev.target.closest('button[data-act]');
    var card = b && b.closest('.input-card');
    var inp = card && inputById(card.getAttribute('data-id'));
    if (!inp) return;
    var act = b.getAttribute('data-act'), path = 'api/inputs/' + encodeURIComponent(inp.id);
    var done = function () { showMsg($('inputs-msg'), ''); return refreshInputs(); };
    var failed = function (err) { showMsg($('inputs-msg'), err.message, true); };
    if (act === 'toggle') {
      request('POST', path + (inp.enabled ? '/disable' : '/enable'), {}).then(done).catch(failed);
    } else if (act === 'edit') {
      editInput(inp);
    } else if (act === 'remove') {
      if (!window.confirm('Remove input ' + inp.id + '?')) return;
      if (rx.editing === inp.id) resetForm();
      request('DELETE', path).then(done).catch(failed);
    } else if (act === 'show') {
      var ll = devicePos(inp);
      if (ll && map) map.setView(ll, Math.max(map.getZoom(), 9));
    }
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
    rx.prevLines = null;
    rx.dataWall = {};
    applyStats(s.stats);
    applyInputs(s.inputs);
    applyLists(s.lists);
    applyFilters(s.filters);
    updateReceiver();
    (s.tracks || []).forEach(function (t) { onTrack(t, true); });
    if (selectedKey && !tracks.has(selectedKey)) deselect();
    else if (selectedKey) renderDetails();
    attempt = 0;
    lastStatsWall = Date.now();
    setConnected(true);
    if (map) fisbReset(s.fisb);
  }

  function onItem(it) {
    switch (it.type) {
      case 'observed':
        // Fresh by definition (unlike a snapshot's or an update's last_seen).
        clockOffsetMs = P.nextClockOffset(clockOffsetMs, Date.parse(it.last_seen), Date.now());
        onTrack(it, true);
        break;
      case 'update': flashSent(onTrack(it, false)); break;
      case 'drop': onDrop(it); break;
      case 'expired': removeEntry(P.trackKey(it)); break; // never forwarded, gone from skycued's table
      case 'stats': lastStatsWall = Date.now(); applyStats(it); break;
      case 'filters': applyFilters(it); break;
      case 'lists': applyLists(it); break;
      case 'inputs': applyInputs(it.inputs); break;
      case 'fisb_text': case 'fisb_shape': case 'fisb_radar': case 'fisb_raster': case 'fisb_expired':
        onFisbItem(it);
        break;
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
    lastStatsWall = Date.now(); // the attempt gets STATS_TIMEOUT_MS too
    sock.onmessage = function (ev) {
      if (ws !== sock) return; // abandoned by the watchdog
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

  // A half-open connection (skycued stopped or the network gone without a
  // close) never fires onclose: skycued sends stats every second, so after
  // STATS_TIMEOUT_MS without any (or an attempt that never opened) the
  // socket is abandoned and a new one started.
  function watchdog() {
    if (!ws || Date.now() - lastStatsWall <= STATS_TIMEOUT_MS) return;
    var sock = ws;
    ws = null;
    try { sock.close(); } catch (err) { /* already closing */ }
    setConnected(false);
    scheduleReconnect();
  }

  // ---- wiring -------------------------------------------------------------------
  function wire() {
    var stored = null;
    try { stored = window.localStorage.getItem('skycue.hideFiltered'); } catch (err) { /* no storage */ }
    if (stored === '1') setHideFiltered(true);
    $('hide-filtered').addEventListener('change', function () { setHideFiltered($('hide-filtered').checked); });
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
      $(FILTER_INPUTS[k]).addEventListener('change', function () { onFilterChange(k); });
    });
    Array.prototype.forEach.call(document.querySelectorAll('button[data-reset]'), function (b) {
      b.addEventListener('click', function () {
        var k = b.getAttribute('data-reset');
        editing[k] = false;
        $(FILTER_INPUTS[k]).classList.remove('invalid');
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
    $('details-close').addEventListener('click', closePanel);
    eachEl('#details [role=tab]', function (b) {
      b.addEventListener('click', function () { setTab(b.id.replace(/^tab-/, '')); });
    });
    eachEl('[data-opens-tab]', function (b) {
      b.addEventListener('click', function () {
        var tab = b.getAttribute('data-opens-tab');
        if (tabShown(tab)) closePanel();
        else openPanel(tab);
      });
    });
    $('input-form').addEventListener('submit', submitInput);
    $('input-form').addEventListener('input', onFormInput);
    $('input-form').addEventListener('change', onFormInput);
    $('input-form').querySelector('[data-act=cancel]').addEventListener('click', resetForm);
    $('inputs-list').addEventListener('click', onCardClick);
    syncFormFields();
    $('shape-close').addEventListener('click', hideShape);
    $('wx-filter').addEventListener('input', function () {
      clearTimeout(fisb.wxTimer);
      fisb.wxTimer = setTimeout(function () {
        fisb.wxFilter = $('wx-filter').value.trim();
        renderWx(true);
      }, WX_DEBOUNCE_MS);
    });
    $('wx-list').addEventListener('click', function (ev) {
      var head = ev.target.closest('.wx-head');
      if (!head) return;
      var li = head.closest('.wx-row');
      var key = li.getAttribute('data-key');
      var open = !fisb.wxExpanded[key];
      if (open) fisb.wxExpanded[key] = true; else delete fisb.wxExpanded[key];
      head.setAttribute('aria-expanded', String(open));
      li.querySelector('.wx-text').hidden = !open;
    });
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
      if (ev.key === 'Escape') closePanel();
    });
    setInterval(prune, 2000);
    setInterval(watchdog, 1000);
    setInterval(pollFisb, FISB_POLL_MS);
    setInterval(pollInputs, INPUTS_POLL_MS);
    var ticks = 0;
    setInterval(function () {
      if (selectedKey) renderDetails(); // ages
      updateHiddenCount();
      if (tableDirty) renderTable();
      if (map) radarAgeTick();
      if (tabShown('receiver')) renderReceiver(); // ages, rates
      if (++ticks % 15 === 0) fisb.wxDirty = true; // ages and age classes
      renderWx();
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
    // The map centre, to 4 decimals (the Receiver tab's "show").
    center: function () {
      var c = map.getCenter();
      return { lat: Math.round(c.lat * 1e4) / 1e4, lon: Math.round(c.lng * 1e4) / 1e4 };
    },
    elevationRingRadius: function () { return elevRing ? elevRing.getRadius() : null; },
    selected: function () {
      var e = selectedKey && tracks.get(selectedKey);
      return e ? e.t.hex : null;
    },
    connected: function () { return connected; },
    setHideFiltered: setHideFiltered,
    getHideFiltered: function () { return hideFiltered; },
    hiddenCount: updateHiddenCount,
    // Adds a marker skycued does not know (the reconnect test checks it goes).
    injectTestMarker: function (t) { onTrack(t, true); },
    dataNow: dataNow,
    // FIS-B: the initial /api/fisb/* load is done; WebSocket items through
    // the page's own handler; what the radar canvases hold.
    fisbLoaded: function () { return fisb.loaded; },
    injectFisbItems: function (items) { items.forEach(onItem); },
    radarStats: function () {
      var out = { blocks: 0, painted: 0, products: {} };
      Object.keys(radar).forEach(function (w) {
        var d = radar[w].drawn;
        var p = { blocks: d ? d.blocks : 0, painted: d ? canvasPainted(d.canvas) : 0,
          width: d ? d.canvas.width : 0, height: d ? d.canvas.height : 0 };
        out.products[w] = p;
        out.blocks += p.blocks;
        out.painted += p.painted;
      });
      return out;
    },
    radarBounds: function (w) { return radar[w] && radar[w].drawn ? radar[w].drawn.bounds : null; },
    // Map tiles: how many showed their own image, an enlarged ancestor
    // (fallback) or nothing; null without tiles.
    tileCounts: function () {
      return tileLayer ? { loaded: tileCounts.loaded, fallback: tileCounts.fallback, underlay: tileCounts.underlay,
        failed: tileCounts.failed, missing: tileMissing.size } : null;
    },
    // Moves the page's data clock to `iso` (until skycued's next newer time)
    // and redraws the radar ages; returns each drawing's opacity and fade.
    setDataClock: function (iso) {
      clockOffsetMs = Date.parse(iso) - Date.now();
      radarAgeTick();
      var out = {};
      Object.keys(radar).forEach(function (w) {
        var d = radar[w].drawn, el = d && d.overlay.getElement();
        out[w] = el ? { opacity: Number(el.style.opacity), faded: el.classList.contains('faded') } : null;
      });
      out.label = $('radar-age').textContent;
      return out;
    },
    // Map style and overlays: move the map; what is shown.
    setView: function (lat, lon, zoom) { map.setView([lat, lon], zoom, { animate: false }); },
    zoom: function () { return map.getZoom(); },
    mapView: function () {
      return { style: view.style, borders: view.borders, cities: view.cities,
        bordersLoaded: !!bordersLayer, citiesLoaded: !!citiesData };
    },
    cityLabels: function () { return cityShown.map(function (c) { return c.name; }); },
    shapeLayers: function (key) {
      var s = fisb.shapes.get(key);
      return s ? s.layers.map(function (l) {
        return l.fisbKind === 'circle' ? { kind: 'circle', radius_m: l.getRadius() } : { kind: l.fisbKind };
      }) : null;
    }
  };

  wire();
  var tilesReady = request('GET', 'api/tiles-available').catch(function () { return { tiles: false }; }).then(function (tiles) {
    if (!tiles.tiles) return tiles;
    // The regions (scripts/make-tiles.sh), for the tiles on their edges.
    return request('GET', 'tiles/meta.json').then(function (m) {
      tiles.regions = m && Array.isArray(m.regions) ? m.regions : null;
      return tiles;
    }, function () { return tiles; });
  });
  // The overlays (scripts/make-overlays.sh) are optional like the tiles:
  // without overlays/meta.json (404) the page offers no overlay toggles.
  var overlaysReady = fetchJson('overlays/meta.json').then(function (m) {
    overlayMeta = m && typeof m === 'object' ? m : null;
  }, function () { overlayMeta = null; });
  Promise.all([tilesReady, overlaysReady]).then(function (r) {
    var tiles = r[0];
    initMap(tiles);
    connect();
  });
})();
