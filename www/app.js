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
  // The View section's filters (P.normalizeView; spec 3, 6): kept in
  // localStorage, never sent to skycued, read once here before the first
  // render. (`view`, further down, is the map style and overlays.)
  var VIEW_FILTER_KEY = 'skycue.view';
  var viewFilter = P.normalizeView(storeGet(VIEW_FILTER_KEY));
  // Saved filter sets (P.normalizeViewSets; spec 4): named View states with
  // their Hide filtered, kept in localStorage. `activeSet` is the index of
  // the set last applied or saved (null: "— current —").
  var VIEW_SETS_KEY = 'skycue.viewSets';
  var viewSets = P.normalizeViewSets(storeGet(VIEW_SETS_KEY));
  var activeSet = null;
  // The alert config's store (alerts_view.js owns it; the settings import
  // writes it only without the strip). AP: alerts_pure.js, may be absent.
  var ALERTS_KEY = 'skycue.alerts';
  var AP = window.skycueAlertsPure || null;

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
        if (!r.ok) {
          var err = new Error(j.error || ('HTTP ' + r.status));
          err.status = r.status;   // e.g. the Stats detail closes on a 404
          throw err;
        }
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
      // load(img, n, cb, next): img shows the first tile from n levels up
      // that exists, trying next(n) after level n (default n + 1; null
      // stops); cb(p) once it has loaded, cb(null) when none does (maybe
      // synchronously).
      function load(img, n, cb, next) {
        var p = n === null ? null
          : n === 0 ? { z: coords.z, x: coords.x, y: coords.y, scale: 1, offsetX: 0, offsetY: 0 }
          : P.parentTile(coords.z, coords.x, coords.y, n);
        if (!p || p.z < o.fallbackMinZoom || n > o.fallbackLevels) { cb(null); return; }
        var after = next ? next(n) : n + 1;
        var url = layer.ancestorUrl(p);
        if (tileKnownMissing(url)) { load(img, after, cb, next); return; }
        img.style.width = size.x * p.scale + 'px';
        img.style.height = size.y * p.scale + 'px';
        img.style.left = -p.offsetX * size.x + 'px';
        img.style.top = -p.offsetY * size.y + 'px';
        img.onload = function () { if (!dropped()) cb(p); };
        img.onerror = function () {
          tileMissing.set(url, Date.now());
          if (!dropped()) load(img, after, cb, next);
        };
        img.src = url;
      }
      var img = newImg();
      tile.appendChild(img);
      load(img, 0, function (p) {
        if (!p) {
          // Asynchronously: Leaflet registers the tile after createTile returns.
          // A tile Leaflet has already dropped (pruned on a zoom) is not a failure.
          setTimeout(function () {
            tile.complete = true;
            if (!dropped()) tileCounts.failed++;
            done(new Error('no tile for ' + coords.z + '/' + coords.x + '/' + coords.y), tile);
          }, 0);
          return;
        }
        tile.complete = true;
        // n: how many levels up the shown tile is (0 native).
        var n = Math.round(Math.log2(p.scale));
        if (n > 0) {
          tileCounts.fallback++;
          tile.classList.add('tile-fallback');
          tile.setAttribute('data-fallback-zoom', String(p.z));
        } else {
          tileCounts.loaded++;
        }
        // A tile on a region's edge may be transparent there: below it goes
        // the nearest ancestor (above the one shown) that lies wholly inside a
        // region, so the underlay is opaque; none, no underlay.
        if (o.regions && !P.tileInsideRegions(p.z, p.x, p.y, o.regions)) {
          var from = P.nearestInsideAncestor(coords.z, coords.x, coords.y, n + 1, o.fallbackLevels, o.regions);
          if (from !== null) {
            var under = newImg();
            under.className = 'tile-underlay';
            tile.insertBefore(under, img);
            // A missing underlay goes on to the next inside ancestor only.
            load(under, from, function (q) {
              if (q) {
                tileCounts.underlay++;
                tile.setAttribute('data-underlay-zoom', String(q.z));
              } else if (under.parentNode) {
                under.parentNode.removeChild(under);
              }
            }, function (m) {
              return P.underlayNextLevel(coords.z, coords.x, coords.y, m, o.fallbackLevels, o.regions);
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
      zoomControl: false,
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
        attribution: 'Made with Natural Earth' // scripts/maps/make-tiles.sh
      }).addTo(map);
    } else {
      graticule = L.layerGroup().addTo(map);
      map.on('zoomend moveend', drawGraticule);
      drawGraticule();
    }
    L.control.scale({ position: 'bottomleft' }).addTo(map);
    map.on('click', function (ev) {
      if (rulerTakes(ev)) return;
      deselect();
    });
    initToolbar();
    initFisbLayers();
    initMapView();
    // Markers for what arrived before the map existed.
    updateDeviceMarkers();
    if (fisb.status) updateStationMarkers(fisb.status.stations || []);
    openBox(P.mapBoxName(storeGet(BOX_KEY)), true);
  }

  // ---- map style and overlays ------------------------------------------------
  // View options kept in localStorage (never sent to skycued): the map style
  // (Light: the tiles as baked; Dark: P.tileFilter on the tile pane only)
  // and two overlays from scripts/maps/make-overlays.sh, Borders and Cities,
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
    // The style only changes tiles: without them its row is left out, and
    // the Map button too when there are no overlays either.
    $('map-style-row').hidden = !haveTiles;
    $('ov-borders').parentNode.hidden = !offer.borders;
    $('ov-cities').parentNode.hidden = !offer.cities;
    $('tb-map').hidden = !haveTiles && !offer.borders && !offer.cities;
    $('map-style').addEventListener('change', function () { setMapStyle($('map-style').value); });
    $('ov-borders').addEventListener('change', function () { setOverlay('borders', $('ov-borders').checked); });
    $('ov-cities').addEventListener('change', function () { setOverlay('cities', $('ov-cities').checked); });
    map.on('zoomend', function () { if (bordersLayer && map.hasLayer(bordersLayer)) restyleBorders(); });
    map.on('zoomend moveend', scheduleCities);
    setMapStyle(storeGet(VIEW_KEYS.style), true);
    setOverlay('borders', offer.borders && storeGet(VIEW_KEYS.borders) === '1', true);
    setOverlay('cities', offer.cities && storeGet(VIEW_KEYS.cities) === '1', true);
  }

  // ---- ruler ("Measure") -------------------------------------------------------
  // A button under the zoom control arms the ruler: the first map click (or
  // aircraft click: its position) sets the start, the mouse drags a dashed
  // line labelled with P.rulerLabel (great-circle distance, initial true
  // bearing), the second click fixes it; a further click starts a new one.
  // Escape or the button again clears and disarms. While armed, map clicks
  // never select or deselect, and double-click zoom is off.
  var RULER_SVG = '<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">' +
    '<path d="M1.5 11.5l10-10 3 3-10 10z" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/>' +
    '<path d="M4.5 8.5l1.5 1.5M6.5 6.5l1 1M8.5 4.5l1.5 1.5M10.5 2.5l1 1" stroke="currentColor" stroke-width="1.1"/></svg>';
  var RULER_COLOR = '#ff4fa3';   // none of the decision colours; reads on Light and Dark
  var ruler = { armed: false, start: null, end: null, fixed: false, button: null, line: null, dot: null, label: null, dblZoom: false };

  // ---- toolbar and its boxes ---------------------------------------------------
  // One bar top left: zoom, measure, then four buttons (Map, Aircraft, Layers,
  // Weather) that fold a box out to the right. One box is open at a time and
  // the choice is kept in this browser. The boxes are in index.html (their
  // controls are wired before the map exists) and move into the bar here.
  var BOX_KEY = 'skycue.mapBox';
  var BOX_SVG = {
    map: '<path d="M1.5 3.5l4-1.5 5 1.5 4-1.5v10l-4 1.5-5-1.5-4 1.5zM5.5 2v10.5M10.5 3.5V14"/>',
    aircraft: '<path d="M8 1.5c.7 0 1 .7 1 1.5v3.5l5.5 3.5v1.5L9 9.5V13l1.5 1.2v1H5.5v-1L7 13V9.5l-5.5 2.5v-1.5L7 6.5V3c0-.8.3-1.5 1-1.5z"/>',
    layers: '<path d="M8 2l6.5 3L8 8 1.5 5zM1.5 8L8 11l6.5-3M1.5 11L8 14l6.5-3"/>',
    weather: '<path d="M4.5 12.5a3 3 0 0 1-.4-5.97A4 4 0 0 1 11.8 5.6 3.5 3.5 0 0 1 11.5 12.5z"/>'
  };
  var toolbar = null, openBoxName = null;

  function initToolbar() {
    var Toolbar = L.Control.Zoom.extend({
      options: { position: 'topleft' },
      onAdd: function (m) {
        var wrap = L.DomUtil.create('div', 'map-toolbar');
        var bar = L.Control.Zoom.prototype.onAdd.call(this, m);
        wrap.appendChild(bar);
        var a = L.DomUtil.create('a', 'ruler-toggle', bar);
        a.href = '#';
        a.id = 'ruler-toggle';
        a.title = 'Measure';
        a.setAttribute('role', 'button');
        a.setAttribute('aria-label', 'Measure');
        a.setAttribute('aria-pressed', 'false');
        a.innerHTML = RULER_SVG;
        L.DomEvent.on(a, 'click', function (ev) {
          L.DomEvent.preventDefault(ev);
          if (ruler.armed) clearRuler(); else armRuler();
        });
        // The fixed measurement, for screen readers (the live one would chatter).
        ruler.live = L.DomUtil.create('span', 'sr-only', bar);
        ruler.live.setAttribute('aria-live', 'polite');
        ruler.button = a;
        var sep = L.DomUtil.create('span', 'tb-sep', bar);
        sep.setAttribute('aria-hidden', 'true');
        toolbar = { bar: bar, buttons: {}, boxes: {} };
        P.MAP_BOXES.forEach(function (name) {
          var label = name.charAt(0).toUpperCase() + name.slice(1);
          var b = L.DomUtil.create('a', 'tb-box', bar);
          b.id = 'tb-' + name;
          b.href = '#';
          b.title = label;
          b.setAttribute('role', 'button');
          b.setAttribute('aria-label', label);
          b.setAttribute('aria-expanded', 'false');
          b.setAttribute('aria-controls', 'box-' + name);
          b.innerHTML = '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" ' +
            'stroke-width="1.3" stroke-linejoin="round" aria-hidden="true">' + BOX_SVG[name] + '</svg>' +
            (name === 'layers' ? '<span id="layers-badge" class="tb-badge" hidden></span>' : '') +
            (name === 'weather' ? '<span id="weather-dot" class="tb-dot" hidden></span>' : '');
          L.DomEvent.on(b, 'click', function (ev) {
            L.DomEvent.preventDefault(ev);
            toggleBox(name);
          });
          toolbar.buttons[name] = b;
          toolbar.boxes[name] = $('box-' + name);
          wrap.appendChild(toolbar.boxes[name]);
        });
        var pen = $('map-boxes');
        if (pen.parentNode) pen.parentNode.removeChild(pen);
        L.DomEvent.disableClickPropagation(wrap);
        L.DomEvent.disableScrollPropagation(wrap);
        return wrap;
      }
    });
    new Toolbar().addTo(map);
    map.on('resize', function () { if (openBoxName) sizeBox(openBoxName); });
    // On the container, not the map: markers do not pass their mousemove on.
    L.DomEvent.on(map.getContainer(), 'mousemove', function (ev) {
      if (!ruler.armed || !ruler.start || ruler.fixed) return;
      if (ev.target.closest && ev.target.closest('.leaflet-control')) return;
      ruler.end = map.mouseEventToLatLng(ev);
      drawRuler();
    });
  }

  function sizeBox(name) {
    var box = toolbar.boxes[name];
    var alerts = $('map-wrap').classList.contains('alerts-on') ? $('alerts').offsetHeight : 0;
    box.style.top = toolbar.buttons[name].offsetTop + 'px';
    box.style.maxHeight = Math.max(0, map.getContainer().clientHeight - box.offsetTop - 10 - alerts) + 'px';
  }

  // name: a box name or null (all folded). A name whose button is hidden is null.
  function openBox(name, initial) {
    if (!toolbar) return;
    if (name && toolbar.buttons[name].hidden) name = null;
    P.MAP_BOXES.forEach(function (n) {
      var on = n === name;
      toolbar.boxes[n].hidden = !on;
      toolbar.buttons[n].setAttribute('aria-expanded', on ? 'true' : 'false');
      toolbar.buttons[n].classList.toggle('active', on);
    });
    openBoxName = name;
    if (name) sizeBox(name);
    if (!initial) storeSet(BOX_KEY, name || '');
  }

  function toggleBox(name) { openBox(P.mapBoxToggle(openBoxName, name)); }

  function armRuler() {
    ruler.armed = true;
    ruler.button.setAttribute('aria-pressed', 'true');
    ruler.button.classList.add('active');
    map.getContainer().classList.add('ruler-armed');
    ruler.dblZoom = map.doubleClickZoom.enabled();
    map.doubleClickZoom.disable();
  }

  function clearRuler() {
    [ruler.line, ruler.dot, ruler.label].forEach(function (l) { if (l) map.removeLayer(l); });
    ruler.line = ruler.dot = ruler.label = null;
    ruler.start = ruler.end = null;
    ruler.fixed = false;
    if (ruler.live) ruler.live.textContent = '';
    if (!ruler.armed) return;
    ruler.armed = false;
    ruler.button.setAttribute('aria-pressed', 'false');
    ruler.button.classList.remove('active');
    map.getContainer().classList.remove('ruler-armed');
    if (ruler.dblZoom) map.doubleClickZoom.enable();
  }

  // Every click handler on the map asks this first: while armed the click
  // is a ruler point (at `ll`, else where it was) and goes no further.
  function rulerTakes(ev, ll) {
    if (!ruler.armed) return false;
    L.DomEvent.stopPropagation(ev);
    rulerClick(ll || ev.latlng);
    return true;
  }

  // A click while armed, at a map point or an aircraft's position.
  function rulerClick(ll) {
    ll = L.latLng(ll);
    if (!ruler.start || ruler.fixed) {
      clearRulerLayers();
      ruler.start = ll;
      ruler.end = null;
      ruler.fixed = false;
    } else {
      ruler.end = ll;
      ruler.fixed = true;
    }
    drawRuler();
  }

  function clearRulerLayers() {
    [ruler.line, ruler.label].forEach(function (l) { if (l) map.removeLayer(l); });
    ruler.line = ruler.label = null;
  }

  function drawRuler() {
    var s = ruler.start, e = ruler.end;
    if (!ruler.dot) {
      ruler.dot = L.circleMarker(s, { className: 'ruler-dot', color: RULER_COLOR, radius: 3, weight: 2, fillOpacity: 1, interactive: false }).addTo(map);
    } else ruler.dot.setLatLng(s);
    if (!e) return;
    if (!ruler.line) {
      ruler.line = L.polyline([s, e], { className: 'ruler-line', color: RULER_COLOR, weight: 2, dashArray: '6 5', interactive: false }).addTo(map);
    } else ruler.line.setLatLngs([s, e]);
    var text = P.rulerLabel(P.rulerMeasure(s.lat, s.lng, e.lat, e.lng));
    if (!ruler.label) {
      ruler.label = L.marker(e, {
        icon: L.divIcon({ className: 'ruler-label', html: '<span></span>', iconSize: [0, 0], iconAnchor: [0, 0] }),
        interactive: false, keyboard: false
      }).addTo(map);
    } else ruler.label.setLatLng(e);
    var box = ruler.label.getElement(), span = box.querySelector('span');
    if (span.textContent !== text) span.textContent = text;
    box.classList.toggle('fixed', ruler.fixed);
    // Right of and above the end, flipped left near the right edge and
    // below near the top so it stays on the map.
    var pt = map.latLngToContainerPoint(e), size = map.getSize();
    box.classList.toggle('flip-x', pt.x + 14 + span.offsetWidth > size.x - 4);
    box.classList.toggle('flip-y', pt.y - 14 - span.offsetHeight < 4);
    if (ruler.fixed && ruler.live) ruler.live.textContent = 'Measured ' + text;
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
    el.classList.toggle('dimmed', !!e.dimmed);
    el.setAttribute('data-hex', t.hex);
    el.setAttribute('data-decision', cls);
    var glyph = el.querySelector('.ac-glyph');
    glyph.style.transform = 'rotate(' + (typeof t.track_deg === 'number' ? t.track_deg : 0) + 'deg)';
    glyph.setAttribute('title', P.markerLabel(t) + ' (' + t.decision + ')');
    var label = el.querySelector('.ac-label');
    var text = P.markerLabel(t);
    if (label.textContent !== text) label.textContent = text;
    styleTrail(e);
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
    e.marker.on('click', function (ev) {
      if (rulerTakes(ev, e.marker.getLatLng())) return;
      select(key);
    });
    e.marker.addTo(map);
  }

  function dropLayers(e) {
    if (e.marker) map.removeLayer(e.marker);
    removeTrail(e);
    e.marker = null;
    e.dimmed = false;
  }

  // ---- trails ----------------------------------------------------------------
  // The Trails group (viewFilter.trailShow, .trailColor): which entries
  // have a trail (trailShown) and what colours it.
  // e.trail is the polyline of e.pts in the decision colour. An entry
  // without a trail has no polyline (e.trail null, removed from the map,
  // not hidden), but its points are kept, so a trail drawn later
  // (selecting it, Show back to "all") has the whole history at once. A
  // dimmed entry has none either.
  // With Colour by "signal" or "altitude" a group of runs, e.runGroup, is
  // drawn over it instead (the polyline is then transparent but kept):
  // each run in the colour of the RSSI its points were received at, or of
  // the altitude they were at (P.trailRuns with P.trailRunColor), a run
  // without a value solid in the decision colour, dim. Every trail is
  // solid: dashed is only the projection ahead. e.runs holds the runs,
  // e.runLayers their polylines in the same order: a new position extends
  // the last run or adds one, a prune trims the first ones
  // (P.trailRunsAppend, P.trailRunsTrim, updateTrail); drawTrail rebuilds
  // only on a Trails change, a (re)creation, an un-dimming or a trail of
  // two points or fewer.
  // With Project ahead, e.ahead is the dashed line from the marker along
  // its track for that many minutes at its ground speed (P.projectAhead),
  // in the decision colour and the trail's weight; only an entry with a
  // trail has one (it follows Show), and updateAhead keeps it with every
  // render.
  var TRAIL_OPACITY = 0.6;          // the plain trail
  var RUN_OPACITY = 0.9;            // a run with a colour of the scale
  var RUN_DIM_OPACITY = 0.35;       // a run without a value (no signal, no altitude)

  function trailColor(e) { return P.DECISION_COLORS[P.decisionColor(e.t.decision) || 'unknown']; }
  function trailWeight(e) { return e.key === selectedKey ? 3 : 1.5; }
  function runPoint(p) { return { lat: p.ll[0], lon: p.ll[1], sig: p.sig, alt: p.alt }; }

  // Restyles the trail only when its colour, weight or mode changed (it is
  // called on every renderEntry through styleMarker).
  function styleTrail(e) {
    if (!e.trail) return;
    var color = trailColor(e), weight = trailWeight(e);
    var key = color + '|' + weight + '|' + (e.runGroup ? 1 : 0);
    if (e.trailStyle === key) return;
    e.trailStyle = key;
    e.trail.setStyle({ color: color, weight: weight, opacity: e.runGroup ? 0 : TRAIL_OPACITY });
    if (e.runLayers) {
      e.runLayers.forEach(function (l) { l.setStyle({ weight: weight }); });
    }
  }

  function runLayer(e, r) {
    var l = L.polyline(r.points, {
      className: 'trail-run', interactive: false, color: r.color || P.TRAIL_NONE_COLOR, weight: trailWeight(e),
      opacity: r.color ? RUN_OPACITY : RUN_DIM_OPACITY
    });
    l.runColor = r.color;
    e.runGroup.addLayer(l);
    return l;
  }

  function dropRuns(e) {
    if (e.runGroup) map.removeLayer(e.runGroup);
    e.runGroup = e.runs = e.runLayers = null;
  }

  function trailShown(e) {
    if (!e.marker || e.dimmed) return false;
    var show = viewFilter.trailShow;
    return show === 'all' || (show === 'selected' && e.key === selectedKey);
  }

  // The whole trail from e.pts: made when the entry should have one,
  // removed when it should not.
  function drawTrail(e) {
    if (!trailShown(e)) { removeTrail(e); return; }
    if (!e.trail) e.trail = L.polyline([], { className: 'trail', weight: 1.5, opacity: TRAIL_OPACITY, interactive: false }).addTo(map);
    var pts = e.pts, colorOf = P.trailRunColor(viewFilter.trailColor);
    e.trail.setLatLngs(pts.map(function (p) { return p.ll; }));
    if (colorOf && pts.length > 1) {
      if (!e.runGroup) e.runGroup = L.layerGroup().addTo(map);
      else e.runGroup.clearLayers();
      e.runs = P.trailRuns(pts.map(runPoint), colorOf);
      e.runLayers = e.runs.map(function (r) { return runLayer(e, r); });
    } else dropRuns(e);
    e.trailStyle = null;
    styleTrail(e);
  }

  // The projection ahead from the current position, made, moved, restyled
  // or removed (no trail, Project ahead off, no track or speed, on the
  // ground); recomputed only when what it depends on changed (e.aheadKey).
  var AHEAD_OPACITY = 0.6;
  function updateAhead(e) {
    var t = e.t, min = viewFilter.trailAhead;
    if (!e.trail || !(min > 0)) { dropAhead(e); return; }
    var key = [t.lat, t.lon, t.track_deg, t.gs_kt, t.on_ground ? 1 : 0, min].join('|');
    var color = trailColor(e), weight = trailWeight(e);
    if (e.ahead && e.aheadKey === key) {
      if (e.ahead.options.color !== color || e.ahead.options.weight !== weight) e.ahead.setStyle({ color: color, weight: weight });
      return;
    }
    var pts = P.projectAhead(t, min);
    if (!pts) { dropAhead(e); return; }
    e.aheadKey = key;
    if (!e.ahead) {
      e.ahead = L.polyline(pts, { className: 'trail-ahead', interactive: false, color: color, weight: weight,
        opacity: AHEAD_OPACITY, dashArray: '6 6' }).addTo(map);
      return;
    }
    e.ahead.setLatLngs(pts);
    if (e.ahead.options.color !== color || e.ahead.options.weight !== weight) e.ahead.setStyle({ color: color, weight: weight });
  }
  function dropAhead(e) {
    if (e.ahead) map.removeLayer(e.ahead);
    e.ahead = e.aheadKey = null;
  }

  // The trail after `pruned` points went from its front and, with
  // `appended`, the newest point was added: the polyline and the runs are
  // updated in place, not rebuilt.
  function updateTrail(e, pruned, appended) {
    if (!e.trail) return;
    var n = e.pts.length, last = e.pts[n - 1];
    if (appended && !pruned) e.trail.addLatLng(last.ll);
    else e.trail.setLatLngs(e.pts.map(function (p) { return p.ll; }));
    var colorOf = P.trailRunColor(viewFilter.trailColor);
    if (!colorOf) return;
    // No runs yet (one point so far), or two points or fewer left: rebuilt
    // (the in-place steps keep runs of two points or more).
    if (!e.runGroup || n <= 2) { drawTrail(e); return; }
    if (pruned) {
      var cut = P.trailRunsTrim(e.runs, pruned);
      e.runLayers.splice(0, cut.dropped).forEach(function (l) { e.runGroup.removeLayer(l); });
      if (cut.trimmed) e.runLayers[0].setLatLngs(e.runs[0].points);
    }
    if (!appended) return;
    if (!e.runs.length) { drawTrail(e); return; }
    if (P.trailRunsAppend(e.runs, runPoint(e.pts[n - 2]), runPoint(last), colorOf)) {
      e.runLayers[e.runLayers.length - 1].addLatLng(last.ll);
    } else e.runLayers.push(runLayer(e, e.runs[e.runs.length - 1]));
  }

  function removeTrail(e) {
    if (e.trail) map.removeLayer(e.trail);
    dropRuns(e);
    dropAhead(e);
    e.trail = null;
    e.trailStyle = null;
  }

  // The Trails group's settings live in the view (skycue.view, the filter
  // sets, Clear); a change shows the right legend and redraws every trail
  // (only the projections when Project ahead alone changed, `aheadOnly`).
  function trailOptionsChanged(a, b) {
    return P.TRAIL_FIELDS.some(function (k) { return a[k] !== b[k]; });
  }
  function applyTrailOptions(aheadOnly) {
    $('trail-signal-legend').hidden = viewFilter.trailColor !== 'signal';
    $('trail-alt-legend').hidden = viewFilter.trailColor !== 'altitude';
    if (!map) return;
    tracks.forEach(function (e) {
      if (!aheadOnly) drawTrail(e);
      updateAhead(e);
    });
  }

  // A legend: the scale's swatches (each titled with its bounds), the two
  // ends, a line of extra text and what a dim stretch means.
  function fillScaleLegend(legend, scale, title, lo, hi, extra, none) {
    var bar = el('div', 'sig-scale');
    scale.forEach(function (s) {
      var sw = el('i', null);
      sw.style.background = s.color;
      sw.title = title(s);
      bar.appendChild(sw);
    });
    var ends = el('div', 'sig-ends');
    ends.appendChild(el('span', null, lo));
    ends.appendChild(el('span', null, hi));
    legend.textContent = '';
    legend.appendChild(bar);
    legend.appendChild(ends);
    if (extra) legend.appendChild(el('div', 'sig-steps', extra));
    var row = el('div', 'sig-none');
    var sw = el('i', 'sig-none-sw');
    sw.style.background = P.TRAIL_NONE_COLOR;
    row.appendChild(sw);
    row.appendChild(document.createTextNode(none));
    legend.appendChild(row);
  }

  // The legends under Colour by; the old "Colour trail by signal"
  // checkbox's setting (localStorage skycue.trailSignal) taken over once;
  // then filter sets saved before the Trails group get the current Trails
  // settings once (P.viewSetsFillTrails), so applying one keeps them.
  var TRAIL_SIGNAL_KEY = 'skycue.trailSignal';
  function initTrails() {
    var db = function (v) { return v < 0 ? '−' + -v : String(v); };   // a minus sign, as the docs write it
    fillScaleLegend($('trail-signal-legend'), P.signalLegend(),
      function (s) { return db(s.from) + ' to ' + db(s.to) + ' dBFS'; },
      '≤ ' + db(P.SIGNAL_MIN_DB) + ' dBFS', '≥ ' + db(P.SIGNAL_MAX_DB) + ' dBFS', null, 'faint grey: no signal');
    var ft = function (v) { return P.thousands(v); };
    var b = P.ALT_BOUNDS_FT;
    fillScaleLegend($('trail-alt-legend'), P.altitudeLegend(),
      function (s) {
        return s.from === null ? 'ground to ' + ft(s.to) + ' ft' : s.to === null ? 'above ' + ft(s.from) + ' ft' :
          ft(s.from) + ' to ' + ft(s.to) + ' ft';
      },
      'ground–' + ft(b[0]) + ' ft', '> ' + ft(b[b.length - 1]) + ' ft',
      b.map(function (v) { return v / 1000 + 'k'; }).join(' · ') + ' ft', 'faint grey: no altitude');
    var old = storeGet(TRAIL_SIGNAL_KEY);
    if (old !== null) {
      try { window.localStorage.removeItem(TRAIL_SIGNAL_KEY); } catch (err) { /* private window */ }
      var had = false;
      try {
        var stored = JSON.parse(storeGet(VIEW_FILTER_KEY));
        had = !!stored && typeof stored === 'object' && 'trailColor' in stored;
      } catch (err) { /* nothing stored, or garbage */ }
      if (old === '1' && !had) setViewFilters({ trailColor: 'signal' });
    }
    var fill = P.viewSetsFillTrails(storeGet(VIEW_SETS_KEY), viewFilter);
    if (fill.filled) {
      viewSets = fill.store;
      saveViewSets();
    }
    applyTrailOptions();
  }

  // Draws or removes one entry's marker and trail according to its drawability,
  // the "Hide filtered" option and the View (spec §5: a non-match has no
  // layers in "hide" mode, a dimmed marker without trail in "dim" mode; the
  // selected entry always matches). `observed`: add the position to the trail.
  function renderEntry(e, observed) {
    var t = e.t, step = 'join';
    var drawable = typeof t.lat === 'number' && typeof t.lon === 'number' && P.decisionColor(t.decision) !== null;
    if (!drawable || !map) return;
    if (observed) {
      var ts = Date.parse(t.position_time);
      var pushed = { ts: isNaN(ts) ? dataNow() : ts, ll: [t.lat, t.lon], sig: P.signalLevel(t.rssi), alt: P.trackAlt(t) };
      // P.trailStep: an impossible jump (a looped replay restarting, a bad
      // position, a reused hex) restarts the trail here instead of joining
      // it; a small step back in time is left out.
      var prev = e.pts[e.pts.length - 1];
      step = prev ? P.trailStep({ lat: prev.ll[0], lon: prev.ll[1], ts: prev.ts },
        { lat: t.lat, lon: t.lon, ts: pushed.ts }) : 'join';
      if (step === 'break') e.pts.length = 0;
      if (step !== 'skip') e.pts.push(pushed);
      var pruned = pruneTrail(e);
    }
    if (!P.trackVisible(t.decision, hideFiltered)) {
      dropLayers(e);
      if (e.key === selectedKey) deselect();
      return;
    }
    var shows = viewShows(e);
    if (!shows && viewFilter.mapMode === 'hide') { dropLayers(e); return; } // never deselects
    var fresh = !e.marker, wasDimmed = !!e.dimmed;
    ensureMarker(e);
    e.dimmed = !shows;
    var inPlace = observed && step !== 'break' && !e.dimmed && !fresh && !wasDimmed;
    // A trail that should come or go (dimming, Show "selected" and the
    // selection moving) is drawn or removed whole; else updated in place.
    if (trailShown(e) !== !!e.trail) drawTrail(e);
    else if (!e.trail) { /* no trail: the points are kept, nothing drawn */ }
    else if (inPlace && step === 'skip') { if (pruned) updateTrail(e, pruned, false); }
    else if (inPlace && e.pts[e.pts.length - 1] === pushed) updateTrail(e, pruned, true);
    else if (observed || fresh || wasDimmed) drawTrail(e);
    styleMarker(e);
    updateAhead(e);
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
    renderAllEntries();
    updateHiddenCount();
    syncViewSetSelect();
  }

  // ---- View section --------------------------------------------------------------
  // Narrows the table and the map to the tracks that match every active
  // control. The selected aircraft is exempt, so a filter change never drops
  // or deselects it.
  function viewShows(e) { return e.key === selectedKey || P.viewMatches(e.t, viewFilter); }
  function renderAllEntries() { tracks.forEach(function (e) { renderEntry(e, false); }); }
  function saveView() { storeSet(VIEW_FILTER_KEY, JSON.stringify(viewFilter)); }
  var VIEW_NUMBERS = { altMin: 'view-alt-min', altMax: 'view-alt-max', gsMin: 'view-gs-min', gsMax: 'view-gs-max',
    rangeMin: 'view-range-min', rangeMax: 'view-range-max' };
  function pillValues(groupId, attr) {
    var out = [];
    eachEl('#' + groupId + ' button[' + attr + ']', function (b) {
      if (b.getAttribute('aria-pressed') === 'true') out.push(b.getAttribute(attr));
    });
    return out;
  }
  function setPills(groupId, attr, on) {
    eachEl('#' + groupId + ' button[' + attr + ']', function (b) {
      b.setAttribute('aria-pressed', String(on.indexOf(b.getAttribute(attr)) >= 0));
    });
  }
  function applyViewToControls() {
    var v = viewFilter;
    $('view-search').value = v.search;
    Object.keys(VIEW_NUMBERS).forEach(function (k) { $(VIEW_NUMBERS[k]).value = v[k] === null ? '' : String(v[k]); });
    $('view-ground').checked = v.ground;
    setPills('view-decisions', 'data-decision', v.decisions);
    setPills('view-classes', 'data-class', v.classes);
    $('view-emergency').checked = v.emergency;
    $('view-category').value = v.category;
    $('view-country').value = v.country;
    eachEl('input[name="view-map-mode"]', function (r) { r.checked = r.value === v.mapMode; });
    eachEl('input[name="view-trail-show"]', function (r) { r.checked = r.value === v.trailShow; });
    $('view-trail-color').value = v.trailColor;
    $('view-trail-ahead').value = String(v.trailAhead);
  }
  function readViewFromControls() {
    var o = {
      search: $('view-search').value, ground: $('view-ground').checked, emergency: $('view-emergency').checked,
      decisions: pillValues('view-decisions', 'data-decision'), classes: pillValues('view-classes', 'data-class'),
      category: $('view-category').value, country: $('view-country').value,
      mapMode: (document.querySelector('input[name="view-map-mode"]:checked') || {}).value,
      trailShow: (document.querySelector('input[name="view-trail-show"]:checked') || {}).value,
      trailColor: $('view-trail-color').value, trailAhead: $('view-trail-ahead').value
    };
    Object.keys(VIEW_NUMBERS).forEach(function (k) { o[k] = $(VIEW_NUMBERS[k]).value; });
    return P.normalizeView(o);
  }
  // N of "showing N of M", from the tracks themselves (live with the table closed).
  function viewShownCount() {
    var n = 0;
    tracks.forEach(function (e) { if (viewShows(e)) n++; });
    return n;
  }
  function updateViewStatus() {
    var active = P.viewActiveCount(viewFilter);
    $('view-count').textContent = active > 0 ? 'showing ' + viewShownCount() + ' of ' + tracks.size : '';
    $('view-badge').textContent = active > 0 ? '· ' + active : '';
    // Clear also resets the map mode and the Trails group, which are not
    // counted in the badge.
    $('view-clear').disabled = active === 0 && viewFilter.mapMode === 'hide' && P.viewTrailsDefault(viewFilter);
  }
  // Merges `partial` into the view, persists and re-renders. `fromControls`:
  // the controls already show it (the operator is typing), leave them be.
  function setViewFilters(partial, fromControls) {
    var before = viewFilter;
    viewFilter = P.normalizeView(Object.assign({}, viewFilter, partial));
    if (trailOptionsChanged(before, viewFilter)) {
      applyTrailOptions(before.trailShow === viewFilter.trailShow && before.trailColor === viewFilter.trailColor);
    }
    if (!fromControls) applyViewToControls();
    saveView();
    tableDirty = true;
    scheduleViewRender();
    syncViewSetSelect();
  }
  // One map/status/table pass per frame however many input events arrive
  // (typing in the search fires one per key).
  var viewRenderPending = false;
  function scheduleViewRender() {
    if (viewRenderPending) return;
    viewRenderPending = true;
    window.requestAnimationFrame(flushViewRender);
  }
  function flushViewRender() {
    if (!viewRenderPending) return;
    viewRenderPending = false;
    renderAllEntries();
    updateViewStatus();
    renderTable();
  }
  function resetViewFilters() { setViewFilters(P.VIEW_DEFAULTS); }
  function togglePill(field, value) {
    var on = viewFilter[field].slice(), i = on.indexOf(value);
    if (i >= 0) on.splice(i, 1); else on.push(value);
    var o = {};
    o[field] = on;
    setViewFilters(o);
  }
  function wireView() {
    var cat = $('view-category');
    var opts = [['', 'any']];
    Object.keys(P.CATEGORY_LABELS).forEach(function (c) { opts.push([c, P.CATEGORY_LABELS[c] + ' (' + c + ')']); });
    opts.push(['other', 'other'], ['none', 'none']);
    opts.forEach(function (o) { cat.appendChild(new Option(o[1], o[0])); });
    var country = $('view-country');
    country.appendChild(new Option('any', ''));
    P.ICAO_COUNTRIES.forEach(function (c) { country.appendChild(new Option(c, c)); });
    country.appendChild(new Option('other / unknown', 'other'));
    applyViewToControls();
    updateViewStatus();
    var fromControls = function () { setViewFilters(readViewFromControls(), true); };
    ['view-search'].concat(Object.keys(VIEW_NUMBERS).map(function (k) { return VIEW_NUMBERS[k]; })).forEach(function (id) {
      $(id).addEventListener('input', fromControls);
    });
    ['view-ground', 'view-emergency', 'view-category', 'view-country', 'view-trail-color', 'view-trail-ahead'].forEach(function (id) {
      $(id).addEventListener('change', fromControls);
    });
    eachEl('input[name="view-map-mode"], input[name="view-trail-show"]', function (r) { r.addEventListener('change', fromControls); });
    eachEl('#view-decisions button[data-decision]', function (b) {
      b.addEventListener('click', function () { togglePill('decisions', b.getAttribute('data-decision')); });
    });
    eachEl('#view-classes button[data-class]', function (b) {
      b.addEventListener('click', function () { togglePill('classes', b.getAttribute('data-class')); });
    });
    $('view-clear').addEventListener('click', resetViewFilters);
    // Escape in the search clears and blurs it; the panel stays open.
    $('view-search').addEventListener('keydown', function (ev) {
      if (ev.key !== 'Escape') return;
      ev.stopPropagation();
      $('view-search').blur();
      if (viewFilter.search !== '') setViewFilters({ search: '' });
    });
  }
  // Whether the key goes to a field that takes typing (a page shortcut
  // must leave it alone).
  function typingTarget(ev) {
    var tag = ev.target && ev.target.tagName;
    return tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || !!(ev.target && ev.target.isContentEditable);
  }
  // `/` focuses the search unless the key goes to a control or has a modifier.
  function slashFocusesSearch(ev) {
    if (ev.key !== '/' || ev.ctrlKey || ev.metaKey || ev.altKey) return false;
    if (typingTarget(ev) || (ev.target && ev.target.tagName === 'BUTTON')) return false;
    ev.preventDefault();
    $('view-search').focus();
    return true;
  }

  // ---- Saved filter sets -------------------------------------------------------
  function saveViewSets() { storeSet(VIEW_SETS_KEY, JSON.stringify(viewSets)); }
  // The select's options, the "(modified)" marker and which buttons apply.
  // A modified active set shows as a hidden "<name> (modified)" option, so
  // choosing the set itself again is a change and re-applies it.
  function syncViewSetSelect() {
    var sel = $('view-set-select');
    if (!sel) return;
    var n = viewSets.sets.length;
    if (activeSet !== null && activeSet >= n) activeSet = null;
    while (sel.options.length) sel.remove(0);
    sel.appendChild(new Option('— current —', ''));
    viewSets.sets.forEach(function (set, i) { sel.appendChild(new Option(set.name, String(i))); });
    var active = activeSet === null ? null : viewSets.sets[activeSet];
    if (active && !P.viewSetMatches(active, viewFilter, hideFiltered)) {
      // Right after the active set's own option, so the arrow keys on the
      // closed select step from the set's place in the list.
      var marker = new Option(active.name + ' (modified)', 'modified');
      marker.hidden = true;
      sel.insertBefore(marker, sel.options[activeSet + 2] || null);
      sel.value = 'modified';
    } else sel.value = activeSet === null ? '' : String(activeSet);
    $('view-set-delete').disabled = activeSet === null;
    var item = function (act) { return document.querySelector('#view-set-menu [data-act="' + act + '"]'); };
    item('rename').disabled = activeSet === null;
    item('up').disabled = activeSet === null || activeSet === 0;
    item('down').disabled = activeSet === null || activeSet === n - 1;
  }
  // The whole set: every View field and Hide filtered.
  function applyViewSet(i) {
    var set = viewSets.sets[i];
    if (!set) return;
    activeSet = i;
    setViewFilters(set.view);
    setHideFiltered(set.hideFiltered);
  }
  function viewSetMsg(text, isError) { showMsg($('view-set-msg'), text, isError); }
  function saveViewSet() {
    var current = activeSet === null ? '' : viewSets.sets[activeSet].name;
    var raw = window.prompt('Name for this filter set', current);
    if (raw === null) return;
    var name = P.viewSetName(raw);
    if (name === '') return;
    var taken = viewSets.sets.some(function (s) { return s.name === name; });
    if (taken && !window.confirm('Replace the filter set "' + name + '"?')) return;
    var r = P.putViewSet(viewSets, name, viewFilter, hideFiltered);
    if (r.error) { viewSetMsg(r.error, true); return; }
    viewSets = r.store;
    activeSet = r.index;
    saveViewSets();
    viewSetMsg('');
    syncViewSetSelect();
  }
  function deleteViewSet() {
    if (activeSet === null) return;
    if (!window.confirm('Delete the filter set "' + viewSets.sets[activeSet].name + '"?')) return;
    viewSets.sets.splice(activeSet, 1);
    activeSet = null;
    saveViewSets();
    viewSetMsg('');
    syncViewSetSelect();
  }
  function renameViewSet() {
    if (activeSet === null) return;
    var set = viewSets.sets[activeSet];
    var raw = window.prompt('Name for this filter set', set.name);
    if (raw === null) return;
    var name = P.viewSetName(raw);
    if (name === '' || name === set.name) return;
    if (viewSets.sets.some(function (s) { return s.name === name; })) {
      viewSetMsg('A set named "' + name + '" already exists.', true);
      return;
    }
    set.name = name;
    saveViewSets();
    viewSetMsg('');
    syncViewSetSelect();
  }
  function moveViewSet(by) {
    var to = activeSet === null ? -1 : activeSet + by;
    if (to < 0 || to >= viewSets.sets.length) return;
    var set = viewSets.sets.splice(activeSet, 1)[0];
    viewSets.sets.splice(to, 0, set);
    activeSet = to;
    saveViewSets();
    syncViewSetSelect();
  }

  // ---- Settings file (spec 4.4): the sets and the alert config -----------------
  // A download of `text` as the file `name`; the object URL goes on the next tick.
  function downloadText(name, text, mime) {
    var url = URL.createObjectURL(new Blob([text], { type: mime }));
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.hidden = true;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 0);
  }
  // The alert config now: the strip's own (with hidden and drawer) or, without
  // the strip, the stored one.
  function currentAlerts() {
    return alertsView ? alertsView.config() : AP.normalizeConfig(storeGet(ALERTS_KEY));
  }
  function exportSettings() {
    downloadText(AP.settingsFileName(Date.now()),
      JSON.stringify(AP.settingsFile(viewSets, currentAlerts()), null, 2), 'application/json');
  }
  // The parsed file waiting for Replace or Merge (null: the panel is hidden).
  var pendingImport = null;
  function setImportPanel(pending) {
    pendingImport = pending;
    $('settings-import').hidden = !pending;
    if (!pending) { $('settings-import-text').textContent = ''; return; }
    var r = pending.parsed;
    var text = pending.name + ': ' + r.sets.length + ' filter sets' +
      (r.alerts ? ' and the alert settings' : '') + '. Replace yours, or merge?';
    var replaced = AP.mergeSettings({ sets: viewSets.sets, alerts: currentAlerts() }, r, 'merge').replacedNames;
    if (replaced.length) text += '\nMerge replaces: ' + replaced.join(', ');
    $('settings-import-text').textContent = text;
  }
  // Reads the chosen file and offers Replace or Merge; never sent to skycued.
  function importSettings(file) {
    setImportPanel(null);
    if (file.size > AP.SETTINGS_MAX_BYTES) {
      $('settings-file').value = '';
      viewSetMsg('File too large (over 1 MB).', true);
      return;
    }
    var reader = new FileReader();
    reader.onload = function () {
      $('settings-file').value = '';
      var r = AP.parseSettings(String(reader.result));
      if (!r.ok) { viewSetMsg('Cannot import: ' + r.error + '.', true); return; }
      viewSetMsg('');
      setImportPanel({ name: file.name, parsed: r });
    };
    reader.onerror = function () {
      $('settings-file').value = '';
      viewSetMsg('Cannot import: the file could not be read.', true);
    };
    reader.readAsText(file);
  }
  function applyImport(mode) {
    if (!pendingImport) return;
    var r = pendingImport.parsed;
    var m = AP.mergeSettings({ sets: viewSets.sets, alerts: currentAlerts() }, r, mode);
    viewSets = P.normalizeViewSets({ version: 1, sets: m.sets });
    if (mode === 'replace') activeSet = null;
    saveViewSets();
    if (alertsView) alertsView.setConfig(m.alerts); // stores skycue.alerts
    else storeSet(ALERTS_KEY, JSON.stringify(m.alerts));
    setImportPanel(null);
    var n = r.sets.length - m.droppedNames.length;
    viewSetMsg('Imported ' + n + ' filter sets.' + (m.droppedNames.length ? '\nNot imported (at most ' +
      P.VIEW_SETS_MAX + ' sets): ' + m.droppedNames.join(', ') : ''));
    syncViewSetSelect();
  }

  function setViewSetMenu(open) {
    $('view-set-menu').hidden = !open;
    $('view-set-more').setAttribute('aria-expanded', String(open));
    if (open) {
      var first = document.querySelector('#view-set-menu button:not([disabled])');
      if (first) first.focus();
    } else if ($('view-set-menu').contains(document.activeElement)) $('view-set-more').focus();
  }
  // Keys 1–9 apply that set, 0 clears to the defaults; not with a modifier
  // or while a field takes typing.
  function digitAppliesSet(ev) {
    if (!/^[0-9]$/.test(ev.key) || ev.ctrlKey || ev.metaKey || ev.altKey || typingTarget(ev)) return false;
    if (ev.key === '0') {
      activeSet = null;
      resetViewFilters();
    } else {
      var i = Number(ev.key) - 1;
      if (i >= viewSets.sets.length) return false;
      applyViewSet(i);
    }
    ev.preventDefault();
    return true;
  }
  function wireViewSets() {
    var sel = $('view-set-select');
    sel.addEventListener('change', function () {
      if (sel.value === '') { activeSet = null; syncViewSetSelect(); } else if (sel.value === 'modified') applyViewSet(activeSet);
      else applyViewSet(Number(sel.value));
    });
    $('view-set-save').addEventListener('click', saveViewSet);
    $('view-set-delete').addEventListener('click', deleteViewSet);
    $('view-set-more').addEventListener('click', function () { setViewSetMenu($('view-set-menu').hidden); });
    var acts = { rename: renameViewSet, up: function () { moveViewSet(-1); }, down: function () { moveViewSet(1); },
      'export': exportSettings, 'import': function () { $('settings-file').click(); } };
    // Without alerts_pure.js there is no settings file.
    ['export', 'import'].forEach(function (act) {
      document.querySelector('#view-set-menu [data-act="' + act + '"]').disabled = !AP;
    });
    $('view-set-menu').addEventListener('click', function (ev) {
      var b = ev.target.closest('button[data-act]');
      if (!b || b.disabled || !acts[b.getAttribute('data-act')]) return;
      setViewSetMenu(false);
      acts[b.getAttribute('data-act')]();
    });
    $('settings-file').addEventListener('change', function () {
      var file = $('settings-file').files[0];
      if (file) importSettings(file);
    });
    $('settings-import-replace').addEventListener('click', function () { applyImport('replace'); });
    $('settings-import-merge').addEventListener('click', function () { applyImport('merge'); });
    $('settings-import-cancel').addEventListener('click', function () { setImportPanel(null); });
    // Escape closes the menu only, not the side panel too.
    $('view-set-menu').addEventListener('keydown', function (ev) {
      if (ev.key !== 'Escape') return;
      ev.stopPropagation();
      setViewSetMenu(false);
    });
    document.addEventListener('click', function (ev) {
      if (!$('view-set-menu').hidden && !ev.target.closest('#view-set-menu, #view-set-more')) setViewSetMenu(false);
    });
    syncViewSetSelect();
  }

  function pruneTrail(e) {
    var cutoff = dataNow() - cfg.trailSeconds * 1000;
    var i = 0;
    while (i < e.pts.length && e.pts[i].ts < cutoff) i++;
    if (i) e.pts.splice(0, i);
    return i;
  }

  function removeEntry(key) {
    var e = tracks.get(key);
    if (!e) return;
    if (e.marker) map.removeLayer(e.marker);
    removeTrail(e);
    tracks.delete(key);
    if (alertsView) alertsView.trackGone(key);
    if (key === selectedKey) deselect();
    tableDirty = true;
  }

  function clearAll() {
    Array.from(tracks.keys()).forEach(function (k) {
      var e = tracks.get(k);
      if (e.marker) map.removeLayer(e.marker);
      removeTrail(e);
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
    if (alertsView) alertsView.track(e.t);
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
    if (alertsView) alertsView.drop(d);
    var key = P.trackKey(d);
    var e = tracks.get(key);
    if (!e) return;
    if (P.dropRemovesMarker(d.reason)) {
      removeEntry(key);
      return;
    }
    // filtered: no longer forwarded, still observed. Recoloured now by the
    // drop's decision when it names a drawn one (a track not observed again
    // soon would otherwise keep its old colour), else by the next `observed`.
    e.t = Object.assign({}, e.t, { forwarded: false });
    var cls = typeof d.decision === 'string' ? P.decisionColor(d.decision) : null;
    if (cls && cls !== 'unknown') {
      e.t.decision = d.decision;
      renderEntry(e, false);
    }
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
      // The points age out with or without a trail drawn (a dimmed entry,
      // Show "selected" or "off"); updateTrail does nothing without one.
      if (e.pts.length) {
        var n = e.pts.length;
        pruneTrail(e);
        if (e.pts.length !== n) updateTrail(e, n - e.pts.length, false);
      }
    });
  }

  // ---- selection panel ---------------------------------------------------------
  // The side panel: a Track tab (the selected aircraft), a Weather tab
  // (FIS-B text reports and the clicked airspace shape), a Receiver tab
  // (inputs, ground stations, decoder health) and a Cues tab (what was sent
  // and dropped, filter changes, cue clients). The tabs are the panel's
  // [role=tab] buttons, tab-<name> showing pane-<name>; a header button with
  // data-opens-tab="<name>" toggles the panel on that tab. TAB_SHOWN[name]
  // draws a tab when it is shown. A new tab: its markup and one entry here.
  var TAB_SHOWN = {
    weather: function () { renderWx(true); },
    receiver: function () { renderReceiver(); },
    cues: showCues,
    config: showConfig
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
    // renderEntry, not only styleMarker: the previous one may no longer
    // match the View (drops or dims), the new one always does.
    [prev, key].forEach(function (k) {
      var e = k && tracks.get(k);
      if (e) renderEntry(e, false);
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
    if (e) renderEntry(e, false); // a non-match now drops (hide) or dims
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
    ['class', 'Class (est.)'], ['country', 'Country'],
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
    ['rssi', 'RSSI', function (v) {
      // readsb's -49.5: no level for the data (P.SIGNAL_NONE_DB), not a reading.
      if (P.signalLevel(v) === null) return { text: '—', title: 'no signal level from the decoder' };
      return v.toFixed(1) + ' dBFS';
    }],
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
    // A shallow copy: the derived rows (class, country) and the bearing are
    // for display only, never stored on the track.
    var t = Object.assign({}, e.t);
    t['class'] = P.trackClass(e.t);
    t.country = P.trackCountry(e.t) || '—';
    var r = cfg.receiver;
    if (r && typeof t.lat === 'number') t.bearing = P.bearingDeg(r.lat, r.lon, t.lat, t.lon);
    $('details-title').textContent = (t.callsign || t.hex) + (t.registration ? ' · ' + t.registration : '');
    var known = {};
    var rows = [];
    FIELDS.forEach(function (f) {
      known[f[0]] = true;
      var v = t[f[0]];
      if (v === undefined || v === null) return;
      var shown = f[2] ? f[2](v) : String(v);   // a string, or {text, title}
      if (typeof shown === 'string') rows.push([f[0], f[1], shown]);
      else rows.push([f[0], f[1], shown.text, shown.title]);
    });
    Object.keys(t).forEach(function (k) {
      if (known[k] || k === 'type') return;
      var v = t[k];
      rows.push([k, k, typeof v === 'object' ? JSON.stringify(v) : String(v)]);
    });
    $('details-fields').tBodies[0].innerHTML = rows.map(function (row) {
      return '<tr><th scope="row">' + esc(row[1]) + '</th><td data-field="' + esc(row[0]) + '"' +
        (row[3] ? ' title="' + esc(row[3]) + '"' : '') + '>' + esc(row[2]) + '</td></tr>';
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
    if (tabShown('cues')) renderCueCounters();
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
    var rows = Array.from(tracks.values()).filter(viewShows).map(function (e) {
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
  // The main area shows exactly one of the map, the table and the Stats
  // view (the last two overlay the map, which stays as it is). Table and
  // Stats are pressed and read "Map" while their view is shown; pressing
  // the active one goes back to the map.
  var mainView = 'map';
  var statsView = null;             // stats_view.js, made in wire()
  var alertsView = null;            // alerts_view.js, made in wire()
  function showMain(which) {
    if (which !== 'table' && (which !== 'stats' || !statsView)) which = 'map';
    var prev = mainView;
    mainView = which;
    if (which !== 'map' && map) clearRuler();   // the table and Stats cover the map
    $('table-view').hidden = which !== 'table';
    $('stats-view').hidden = which !== 'stats';
    [['table-toggle', 'table', 'Table'], ['stats-toggle', 'stats', 'Stats']].forEach(function (b) {
      $(b[0]).setAttribute('aria-pressed', String(which === b[1]));
      $(b[0]).textContent = which === b[1] ? 'Map' : b[2];
    });
    if (which === 'table') renderTable();
    if (statsView) {
      if (which === 'stats') statsView.open();
      else statsView.close();
    }
    if (which === 'map' && prev !== 'map' && map) map.invalidateSize({ pan: false });
  }
  function toggleTable() { showMain(mainView === 'table' ? 'map' : 'table'); }

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
    rasterPick: { product: null, band: null },   // the operator's choice (null: the default)
    wxDirty: true, wxFilter: '', wxTimer: null, wxExpanded: {},
    locations: {},                  // location id -> [lat, lon] (api/fisb/locations, fisb_location)
    show: { metar: true, taf: true, pirep: true, winds: true },   // the Reports toggles
    syms: new Map(),                // symbol key -> {marker, sig, s} (renderWxSymbols)
    symDirty: true,
    windsPick: { period: null, level: null },     // the operator's choice (null: the default)
    winds: { period: null, level: null, periods: [], levels: [] },   // what the barbs use (syncWinds)
    reportRec: null,                // the record key of the report shown in the card
    showKey: null                   // the report last shown from the list (kept through the symbol cap)
  };
  var wxGroup = null;               // the weather symbols, pane fisbWx
  var radar = {
    regional: { product: 63, label: 'Radar (regional)', head: null, drawn: null, fetching: false, again: false },
    conus: { product: 64, label: 'Radar (CONUS)', head: null, drawn: null, fetching: false, again: false }
  };
  var radarGroups = {}, shapeGroups = {}, rasterGroup = null;
  var RADAR_OPACITY_KEY = 'skycue.radarOpacity';   // the opacity slider's value, per browser

  function swatchName(color, name) {
    return '<i class="sw" style="color:' + color + '"></i>' + esc(name);
  }

  function initFisbLayers() {
    map.createPane('fisbRaster');
    map.getPane('fisbRaster').style.zIndex = 350;   // under the shapes (overlayPane, 400)
    map.getPane('fisbRaster').style.pointerEvents = 'none';
    map.createPane('fisbWx');
    map.getPane('fisbWx').style.zIndex = 500;       // over the shapes and city labels, under the aircraft (600)
    wxGroup = L.layerGroup().addTo(map);
    // Culled to the view: redrawn by the 1 s interval after a pan or zoom.
    map.on('moveend zoomend', function () { fisb.symDirty = true; });
    var overlays = {};
    ['conus', 'regional'].forEach(function (w) {
      radarGroups[w] = L.layerGroup();    // on or off by the saved layer set (setLayers, below)
    });
    overlays[swatchName('#02fd02', radar.regional.label)] = radarGroups.regional;
    overlays[swatchName('#008e00', radar.conus.label)] = radarGroups.conus;
    rasterGroup = L.layerGroup();
    overlays[swatchName('#54278f', 'Other rasters')] = rasterGroup;
    SHAPE_LAYERS.forEach(function (s) {
      shapeGroups[s[0]] = L.layerGroup();
      overlays[swatchName(P.SHAPE_COLORS[s[0]], s[1])] = shapeGroups[s[0]];
    });
    var layersCtl = L.control.layers(null, overlays, { collapsed: false, position: 'topright' }).addTo(map);
    $('box-layers').appendChild(layersCtl.getContainer());
    rasterGroup.on('add', function () { $('raster-box').hidden = false; loadRaster(); });
    rasterGroup.on('remove', function () { $('raster-box').hidden = true; });
    [radarGroups.regional, radarGroups.conus, rasterGroup].forEach(function (g) {
      g.on('add remove', syncOpacityControl);
    });

    $('radar-opacity').value = String(P.radarOpacityValue(storeGet(RADAR_OPACITY_KEY)));
    $('radar-opacity').addEventListener('input', function () {
      storeSet(RADAR_OPACITY_KEY, $('radar-opacity').value);
      applyRadarOpacity();
    });
    syncOpacityControl();
    $('raster-select').addEventListener('change', function () {
      fisb.rasterPick = { product: Number($('raster-select').value), band: null };   // its lowest band with data
      renderRasterSelect();
      loadRaster();
    });
    $('raster-band').addEventListener('change', function () {
      fisb.rasterPick.band = Number($('raster-band').value);
      renderRasterSelect();
      loadRaster();
    });
    P.LAYER_IDS.forEach(function (id) { layerGroup(id).on('add remove', onLayerChange); });
    setLayers(P.normalizeLayers(storeGet(LAYERS_KEY)));
  }

  // The layer set is kept per browser: each layer group's add and remove
  // saves it and refreshes the Layers count and the Weather dot.
  var LAYERS_KEY = 'skycue.layers';
  function layerGroup(id) {
    if (id === 'regional' || id === 'conus') return radarGroups[id];
    return id === 'rasters' ? rasterGroup : shapeGroups[id];
  }
  function layersState() {
    var out = {};
    P.LAYER_IDS.forEach(function (id) { out[id] = map.hasLayer(layerGroup(id)); });
    return out;
  }
  function setLayers(partial) {
    var want = P.normalizeLayersPartial(partial);
    Object.keys(want).forEach(function (id) {
      var g = layerGroup(id);
      if (want[id] && !map.hasLayer(g)) g.addTo(map);
      else if (!want[id] && map.hasLayer(g)) map.removeLayer(g);
    });
  }
  function saveLayers() { storeSet(LAYERS_KEY, JSON.stringify(layersState())); }
  function updateLayersBadge() {
    var n = P.layersOnCount(layersState());
    $('layers-badge').textContent = String(n);
    $('layers-badge').hidden = n === 0;
  }
  function updateWeatherDot() {
    var items = ['regional', 'conus'].map(function (w) {
      var d = radar[w].drawn;
      return { on: map.hasLayer(radarGroups[w]), drawn: !!d,
        faded: !!d && P.radarFaded(productAgeMs(d.head), radar[w].product) };
    });
    var n = fisb.status && fisb.status.stations ? fisb.status.stations.length : 0;
    $('weather-dot').hidden = !P.weatherDot(items, !!fisb.loaded && n > 0);
  }
  function onLayerChange() {
    saveLayers();
    updateLayersBadge();
    updateWeatherDot();
    syncViewSetSelect();
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
    syncOpacityControl();
  }
  // The slider is enabled only while something it controls is drawn: a
  // radar mosaic on a layer that is on, or a raster product. Run on the
  // layers' add and remove and from applyRadarOpacity; clearRadar and
  // clearRaster are not hooked, the 1 s tick (radarAgeTick) syncs them.
  function syncOpacityControl() {
    var on = Object.keys(radar).some(function (w) { return radar[w].drawn && map.hasLayer(radarGroups[w]); }) ||
      !!(fisb.raster && map.hasLayer(rasterGroup));
    $('radar-opacity').disabled = !on;
    $('radar-opacity-row').classList.toggle('disabled', !on);
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

  // The product select and, for icing and turbulence, the altitude band
  // select; fisb.rasterSel follows them. A pick stays until its layer goes.
  // Each select is rebuilt only when its options change.
  function renderRasterSelect() {
    var sel = $('raster-select'), bandSel = $('raster-band');
    var products = P.rasterProducts(Array.from(fisb.rasterHeads.values()));
    var pick = fisb.rasterPick;
    var entry = products.filter(function (e) { return e.product === pick.product; })[0];
    // With no rasters held (a reconnect reloading, say) the pick is kept for
    // when they are back; otherwise a product no longer offered is replaced.
    if (!entry) { entry = products[0] || null; if (entry) { pick.product = entry.product; pick.band = null; } }
    var bands = entry ? entry.bands : [];
    if (bands.length && (pick.band === null || !bands[pick.band] || !bands[pick.band].key)) pick.band = P.defaultBand(entry);
    fisb.rasterSel = !entry ? null : bands.length ? (pick.band === null ? null : bands[pick.band].key) : entry.key;
    var sig = products.map(function (e) { return e.product + '=' + e.name; }).join(',');
    if (sel.getAttribute('data-sig') !== sig) {
      sel.setAttribute('data-sig', sig);
      sel.textContent = '';
      products.forEach(function (e) { sel.appendChild(new Option(e.name, String(e.product))); });
    }
    sel.disabled = !products.length;
    if (entry) sel.value = String(entry.product);
    var bsig = (entry ? entry.product : '') + ':' + bands.map(function (b) { return b.key ? 1 : 0; }).join('');
    if (bandSel.getAttribute('data-sig') !== bsig) {
      bandSel.setAttribute('data-sig', bsig);
      bandSel.textContent = '';
      bands.forEach(function (b) {
        var o = new Option('≈ ' + P.thousands(b.ft) + ' ft' + (b.key ? '' : ' (no data)'), String(b.code));
        o.disabled = !b.key;
        bandSel.appendChild(o);
      });
    }
    bandSel.hidden = !bands.length;
    if (pick.band !== null && bands.length) bandSel.value = String(pick.band);
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
      if (rulerTakes(ev)) return;
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
    fisb.reportRec = null;
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
    P.closeReportCard(fisb);
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
    fisb.symDirty = true;
    if (rec.key === fisb.reportRec) fillReport(rec);   // the card shows the new report
  }
  function dropText(key) {
    if (fisb.text.delete(key)) fisb.wxDirty = fisb.symDirty = true;
    if (fisb.reportRec === key) hideShape();
  }
  function hhmm(iso) {
    var ms = Date.parse(iso);
    return isNaN(ms) ? '—' : new Date(ms).toISOString().substr(11, 5) + 'Z';
  }
  function shortAge(ms, now) { return P.formatAge(ms, now).replace(/ ago$/, ''); }

  function renderWx(force) {
    if (!force && (!fisb.wxDirty || $('pane-weather').hidden || $('details').hidden)) return;
    syncWinds();
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
    WX_REPORT_KINDS.forEach(function (k) { $('wx-n-' + k).textContent = String(groups[k].length); });
    var pos = wxPositioned();
    $('wx-positioned').textContent = pos.n + ' of ' + pos.m + ' positioned';
    $('wx-list').innerHTML = WX_KINDS.filter(function (k) { return fisb.show[k[0]] !== false; }).map(function (k) {
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
          (r.kind === 'twgo' ? '' : wxPlotPos(r) ? '<a class="wx-show" href="#">show</a>' :
            r.kind === 'winds' && wxPos(r) ? '<span class="wx-nobarb">no barb</span>' :
            '<span class="wx-nopos">no position</span>') +
          '<pre class="wx-text"' + (open ? '' : ' hidden') + '>' + esc(r.text || '') + '</pre></li>';
      }).join('') + '</ul>' : '<p class="msg">None' + (prefix ? ' matching' : '') + '.</p>') + '</section>';
    }).join('');
    $('wx-summary').textContent = prefix ? shown + ' of ' + total + ' reports match "' + prefix.toUpperCase() + '"' :
      total + ' reports' + (fisb.loaded ? '' : ' (loading…)');
  }

  // ---- weather symbols (spec §3.3) ----
  // One Leaflet marker per symbol in wxGroup, reused (setLatLng, setIcon on
  // a change) and removed when its record goes. Colours come from the
  // classes (style.css); the SVG uses currentColor.
  var WX_REPORT_KINDS = ['metar', 'taf', 'pirep', 'winds'];
  var MAX_WX_SYMBOLS = 300;         // on the map at once: METAR, TAF, PIREP, winds, each nearest the centre first
  var WX_KIND_RANK = { metar: 0, taf: 1, pirep: 2, winds: 3 };
  var WX_VIEW_PAD = 0.25;           // symbols drawn inside the view grown by this fraction per side
  var parsedCache = new WeakMap();  // text record -> its parse (a new record object on every update)
  function parsedOf(r) {
    if (!parsedCache.has(r)) {
      parsedCache.set(r, r.kind === 'metar' ? P.parseMetar(r.text) : r.kind === 'taf' ? P.parseTafFirst(r.text) :
        r.kind === 'pirep' ? P.parsePirep(r.text) : r.kind === 'winds' ? P.parseWinds(r.text) : null);
    }
    return parsedCache.get(r);
  }
  // A report's [lat, lon] (spec §3.2), or null: listed, never plotted.
  function wxPos(r) {
    if (r.kind === 'pirep') return P.pirepPosition(parsedOf(r), fisb.locations);
    return P.resolveStation(r.kind, r.station, fisb.locations);
  }
  // Where a report's symbol goes, or null: no position, or a winds record
  // other than the picked period's (winds:<station>:<period>) or without the
  // picked level.
  function wxPlotPos(r) {
    var w = fisb.winds;
    if (r.kind === 'winds' && (r.period_h !== w.period || w.level === null || !P.windsLevel(parsedOf(r), w.level))) return null;
    return wxPos(r);
  }
  // The winds pickers (spec §3.1): the periods present, defaulting to the
  // one whose valid time is nearest the data clock, and the levels present
  // in that period, defaulting to the lowest. An operator's pick stays until
  // it is no longer offered. Sets fisb.winds; on a change the list and the
  // symbols are redrawn. Each select is rebuilt only when its options change.
  function syncWinds() {
    var recs = [];
    fisb.text.forEach(function (r) { if (r.kind === 'winds') recs.push(r); });
    var pick = fisb.windsPick, periods = P.windsPeriods(recs);
    // No winds held at all (a reconnect reloading, say): the pick is kept
    // for when they are back, not judged against an empty set.
    var empty = !recs.length;
    if (!empty && periods.indexOf(pick.period) < 0) pick.period = null;
    var period = pick.period !== null ? pick.period : P.nearestWindsPeriod(recs, dataNow());
    if (period === null && periods.length) period = periods[0];
    var levels = period === null ? [] : P.windsLevelsFor(recs, period, parsedOf);
    if (!empty && levels.indexOf(pick.level) < 0) pick.level = null;
    var level = pick.level !== null ? pick.level : levels.length ? levels[0] : null;
    var old = fisb.winds;
    fisb.winds = { period: period, level: level, periods: periods, levels: levels };
    fillPicker($('wx-winds-period'), periods, period, function (h) { return h + ' h'; });
    fillPicker($('wx-winds-level'), levels, level, function (ft) { return P.thousands(ft) + ' ft'; });
    $('wx-winds-pick').hidden = !fisb.show.winds;
    if (old.period === period && old.level === level) return;
    fisb.wxDirty = fisb.symDirty = true;
    var shown = fisb.reportRec && fisb.text.get(fisb.reportRec);
    if (shown && shown.kind === 'winds') fillReport(shown);   // the card names the level
  }
  function fillPicker(sel, values, value, label) {
    var sig = values.join(',');
    if (sel.getAttribute('data-sig') !== sig) {
      sel.setAttribute('data-sig', sig);
      sel.textContent = '';
      values.forEach(function (v) { sel.appendChild(new Option(label(v), String(v))); });
    }
    sel.disabled = !values.length;
    if (value !== null) sel.value = String(value);
  }
  // "n of m positioned" over the reports the list shows (toggles, filter):
  // n counts those with a symbol, on screen or not.
  function wxPositioned() {
    var n = 0, m = 0;
    fisb.text.forEach(function (r) {
      if (!fisb.show[r.kind] || !P.stationMatches(r.station, fisb.wxFilter)) return;
      m++;
      if (wxPlotPos(r)) n++;
    });
    return { n: n, m: m };
  }

  // A barb's SVG: the staff from r0 px out to r0 + 17 px toward `dir` (where
  // the wind comes from), 50 kt pennants, 10 kt barbs and a 5 kt half barb
  // from its far end (P.barbParts); drawn twice, a halo under the line.
  function barbSvg(dir, kt, r0) {
    var b = P.barbParts(kt), tip = -(r0 + 17), y = tip, i;
    var staff = 'M0 ' + (-r0) + 'V' + tip, feathers = '', pens = '';
    for (i = 0; i < b.pennants; i++, y += 5) pens += 'M0 ' + y + 'L8 ' + (y + 2) + 'L0 ' + (y + 4) + 'Z';
    if (b.pennants) y += 1;
    for (i = 0; i < b.full; i++, y += 3) feathers += 'M0 ' + y + 'l8 -4';
    if (b.half) feathers += 'M0 ' + (y === tip ? y + 3 : y) + 'l4 -2';
    return '<g class="barb" transform="rotate(' + Number(dir) + ')"><path class="halo" d="' + staff + feathers + pens +
      '"/><path d="' + staff + feathers + '"/>' + (pens ? '<path class="pen" d="' + pens + '"/>' : '') + '</g>';
  }
  function svgBox(size, body) {
    var h = size / 2;
    return '<svg width="' + size + '" height="' + size + '" viewBox="' + -h + ' ' + -h + ' ' + size + ' ' + size +
      '" aria-hidden="true">' + body + '</svg>';
  }
  // The symbol of one record at pos, or null (a winds record without the level).
  function wxSymbolFor(r, pos, now) {
    var p = parsedOf(r), cls = 'wx-sym wx-' + r.kind, barb = null, html, size = 48, anchor = [24, 24];
    var key = r.kind === 'winds' ? 'winds:' + r.station : r.key;
    if (r.kind === 'metar' || r.kind === 'taf') {
      cls += ' cat-' + (p ? p.category : 'unknown').toLowerCase();
      if (r.kind === 'metar') {
        var w = p && p.wind;
        if (w && !w.calm && !w.variable && w.dir !== null) barb = { dir: w.dir, kt: w.kt };
        html = svgBox(48, (barb ? barbSvg(barb.dir, barb.kt, 5) : '') + (w && w.calm ? '<circle class="ring" r="8"/>' : '') +
          '<circle class="wx-core" r="5"/>');
      } else {
        size = 10; anchor = [-7, 5];   // right of the METAR circle
        html = svgBox(10, '<rect class="wx-core" x="-4" y="-4" width="8" height="8"/>');
      }
    } else if (r.kind === 'pirep') {
      var urgent = !!(p && p.urgent);
      cls += urgent ? ' pirep-uua' : ' pirep-ua';
      size = 18; anchor = [9, 9];
      html = svgBox(18, '<path class="wx-core" d="M0 -7L7 6H-7Z"/>' + (urgent ? '<text class="bang" y="4.5">!</text>' : ''));
    } else {
      var lv = fisb.winds.level === null ? null : P.windsLevel(p, fisb.winds.level);
      if (!lv) return null;
      if (!lv.lv) barb = { dir: lv.dir, kt: lv.kt };
      anchor = [24, 8];    // 16 px below the station: clear of the METAR circle and its barb
      html = svgBox(48, (barb ? barbSvg(barb.dir, barb.kt, 3) : '') + '<circle class="wx-core" r="3"/>' +
        (lv.temp !== null ? '<text class="temp" x="5" y="13">' + Number(lv.temp) + '</text>' : ''));
    }
    return { key: key, kind: r.kind, station: r.station, cls: cls + ' age-' + P.ageClass(r.issued, now),
      lat: pos[0], lon: pos[1], barb: barb, html: html, size: size, anchor: anchor, rec: r };
  }
  function wxIcon(s) {
    return L.divIcon({ className: s.cls, html: s.html, iconSize: [s.size, s.size], iconAnchor: s.anchor });
  }
  function clearWxSymbols() {
    fisb.syms.forEach(function (o) { wxGroup.removeLayer(o.marker); });
    fisb.syms.clear();
    fisb.symDirty = true;
  }
  function renderWxSymbols() {
    syncWinds();
    fisb.symDirty = false;
    if (!map) return;
    var now = dataNow(), want = [], view = map.getBounds().pad(WX_VIEW_PAD);
    fisb.text.forEach(function (r) {
      if (!fisb.show[r.kind] || !P.stationMatches(r.station, fisb.wxFilter)) return;
      var pos = wxPlotPos(r);
      if (!pos || !view.contains(pos)) return;
      var s = wxSymbolFor(r, pos, now);
      if (s) { s.rank = WX_KIND_RANK[s.kind]; want.push(s); }
    });
    // Over the cap: the report last shown from the list first, then by kind
    // and distance from the centre.
    if (want.length > MAX_WX_SYMBOLS) want = P.capSymbols(want, map.getCenter(), MAX_WX_SYMBOLS, fisb.showKey);
    var keep = {};
    want.forEach(function (s) {
      keep[s.key] = true;
      var o = fisb.syms.get(s.key), sig = s.cls + s.html;
      if (!o) {
        var m = L.marker([s.lat, s.lon], { pane: 'fisbWx', icon: wxIcon(s), title: s.station, keyboard: false });
        m.on('add', function () { m.getElement().setAttribute('data-station', s.station); });
        m.on('click', function (ev) {
          if (rulerTakes(ev, m.getLatLng())) return;
          L.DomEvent.stopPropagation(ev);
          showReport(s.key);
        });
        wxGroup.addLayer(m);
        fisb.syms.set(s.key, { marker: m, sig: sig, s: s });
        return;
      }
      var ll = o.marker.getLatLng();
      if (ll.lat !== s.lat || ll.lng !== s.lon) o.marker.setLatLng([s.lat, s.lon]);
      if (o.sig !== sig) o.marker.setIcon(wxIcon(s));   // the div is reused: data-station stays
      o.sig = sig;
      o.s = s;
    });
    fisb.syms.forEach(function (o, k) {
      if (keep[k]) return;
      wxGroup.removeLayer(o.marker);
      fisb.syms.delete(k);
    });
  }

  // The detail card for a report: its parsed summary and raw text. Every
  // server string goes in through textContent.
  function windText(w) {
    if (!w) return 'not given';
    if (w.calm) return 'calm';
    var s = (w.variable || w.dir === null ? 'variable' : ('00' + w.dir).slice(-3) + '°') + ' ' + w.kt + ' kt';
    return w.gust ? s + ', gusts ' + w.gust + ' kt' : s;
  }
  function visText(v) {
    if (v === null || v === undefined) return 'unknown';
    return Math.round(v * 100) / 100 + ' SM';
  }
  function ceilText(c) {
    if (c === null || c === undefined) return 'unknown';
    return c === Infinity ? 'none' : P.thousands(c) + ' ft';
  }
  // key: a symbol key (winds:<st>) or a record key.
  function showReport(key) {
    var o = fisb.syms.get(key), r = fisb.text.get(o ? o.s.rec.key : key);
    if (!r) return;
    var card = $('shape-details');
    card.setAttribute('data-key', key);
    fillReport(r);
    openPanel('weather');
    card.scrollIntoView({ block: 'nearest' });
  }
  // The card's rows, title and text for record r (also on an update of the shown record).
  function fillReport(r) {
    var p = parsedOf(r), rows = [];
    if (r.kind === 'metar' || r.kind === 'taf') {
      rows.push(['category', 'Category', p ? p.category : 'unknown']);
      rows.push(['wind', 'Wind', windText(p && p.wind)]);
      rows.push(['visibility', 'Visibility', visText(p && p.visSm)]);
      rows.push(['ceiling', 'Ceiling', ceilText(p && p.ceilFt)]);
    } else if (r.kind === 'pirep') {
      rows.push(['type', 'Type', p && p.urgent ? 'UUA (urgent)' : 'UA']);
      var ov = p && p.ov;
      rows.push(['ov', '/OV', ov ? ov.id + (ov.radial !== null ? ' ' + ('00' + ov.radial).slice(-3) + '° ' + ov.nm + ' nm' : '') :
        'not a location id']);
    } else if (r.kind === 'winds') {
      var ft = fisb.winds.level, lv = ft === null ? null : P.windsLevel(p, ft);
      rows.push(['level', 'Level', (ft === null ? 'none' : P.thousands(ft) + ' ft') +
        (r.period_h ? ' · ' + r.period_h + ' h forecast' : '')]);
      rows.push(['wind', 'Wind', lv ? (lv.lv ? 'light and variable' : windText({ dir: lv.dir, kt: lv.kt })) : 'not given']);
      rows.push(['temp', 'Temperature', lv && lv.temp !== null ? lv.temp + ' °C' : 'not given']);
    }
    var t = Date.parse(r.issued);
    rows.push(['issued', 'Issued', r.issued ? fmtWhen(r.issued) : 'unknown']);
    rows.push(['age', 'Age', isNaN(t) ? 'unknown' : shortAge(t, dataNow())]);
    if (!wxPos(r)) rows.push(['position', 'Position', 'no position']);
    fisb.shapeKey = null;
    fisb.reportRec = r.key;
    var card = $('shape-details');
    $('shape-title').textContent = r.kind.toUpperCase() + ' ' + (r.station || '');
    var body = $('shape-fields');
    body.textContent = '';
    rows.forEach(function (row) {
      var tr = document.createElement('tr'), th = document.createElement('th'), td = document.createElement('td');
      th.scope = 'row';
      th.textContent = row[1];
      td.setAttribute('data-field', row[0]);
      td.textContent = row[2];
      tr.appendChild(th);
      tr.appendChild(td);
      body.appendChild(tr);
    });
    var pre = card.querySelector('pre.shape-text');
    pre.textContent = r.text || '';
    pre.hidden = !r.text;
    card.hidden = false;
  }
  // The list's "show": pans to the report's position (as the Receiver tab's).
  function showWxPos(key) {
    var r = fisb.text.get(key), ll = r && wxPlotPos(r);
    if (!ll || !map) return;
    // Kept on the map through MAX_WX_SYMBOLS, by its symbol key (wxSymbolFor).
    fisb.showKey = r.kind === 'winds' ? 'winds:' + r.station : key;
    fisb.symDirty = true;
    if (map.getZoom() < SHOW_MIN_ZOOM && !map.getBounds().contains(ll)) map.setView(ll, SHOW_MIN_ZOOM);
    else map.panTo(ll);
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
    updateWeatherDot();
  }

  // ---- snapshot and items ----
  function fisbReset(st) {
    fisb.gen++;
    var gen = fisb.gen;
    fisb.text.clear();
    fisb.locations = {};
    clearWxSymbols();
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
      request('GET', 'api/fisb/text?type=twgo'),
      request('GET', 'api/fisb/locations').catch(function () { return {}; })]).then(function (r) {
      if (gen !== fisb.gen) return;
      var locs = r[3] || {};
      Object.keys(locs).forEach(function (id) { if (Array.isArray(locs[id])) setLocation(id, locs[id][0], locs[id][1]); });
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

  function setLocation(id, lat, lon) {
    if (typeof id !== 'string' || typeof lat !== 'number' || typeof lon !== 'number') return;
    fisb.locations[id] = [lat, lon];
    fisb.wxDirty = fisb.symDirty = true;
  }

  function onFisbItem(it) {
    if (fisb.loading) { fisb.queue.push(it); return; }
    switch (it.type) {
      case 'fisb_text': upsertText(it.record); break;
      case 'fisb_location': setLocation(it.id, it.lat, it.lon); break;
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
  var SHOW_MIN_ZOOM = 6;            // "Show" zooms in to this from further out
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
    editing: null,                  // the id of the input the form edits; null: it adds one
    enabledTouched: false           // the form's Enabled box was changed while editing
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
    if (alertsView) alertsView.inputs(rx.inputs);
    // An id that went may come back as a new input: forget its client state.
    var ids = {};
    rx.inputs.forEach(function (i) { ids[i.id] = true; });
    [rx.dataWall, rx.rates].forEach(function (byId) {
      Object.keys(byId).forEach(function (id) { if (!ids[id]) delete byId[id]; });
    });
    if (rx.prevLines) rx.prevLines = rx.prevLines.filter(function (l) { return ids[l.id]; });
    var edited = rx.editing !== null && inputById(rx.editing);
    if (edited && !rx.enabledTouched) formEl('enabled').checked = !!edited.enabled;
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
    if (alertsView) alertsView.inputs(rx.inputs);
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
    if (ds.gps_status !== undefined) parts.push('GPS ' + P.gpsStatusText(ds.gps_status));
    if (ds.receiver_status !== undefined) parts.push('receiver ' + P.receiverStatusText(ds.receiver_status));
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
    setText(card, 'input-rejected', P.rejectedText(st));
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
        // A marker with a click listener keeps the click from the map: deselect here.
        m.on('click', function (ev) { if (!rulerTakes(ev, m.getLatLng())) deselect(); });
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
        // A marker with a click listener keeps the click from the map: deselect here.
        m.on('click', function (ev) { if (!rulerTakes(ev, m.getLatLng())) deselect(); });
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

  // ---- Cues tab ------------------------------------------------------------------
  // docs/operating.md, Cues tab. The counters follow every stats item; every
  // update and drop item goes into a ring of the last DECISIONS_RING whether
  // the tab is shown or not; the filter changes (GET /api/filters/history)
  // and the cue clients (GET /api/cues/clients) are fetched when the tab is
  // shown and every CUES_POLL_MS while it is, the history also on every
  // `filters` item. Server strings go in through textContent.
  var CUES_POLL_MS = 5000;
  var FILTER_TEXT = { max_range_nm: ['max range', ' nm'], min_elevation_deg: ['min elevation', '°'] };
  // ring: the last updates and drops; dropRing: the last drops alone, so
  // "Drops only" still has them when max-interval resends fill `ring`.
  var CUES_DROPS_ONLY_KEY = 'skycue.cues.dropsOnly';   // a view option in localStorage
  var cues = { ring: [], dropRing: [], dropsOnly: false, recentDirty: true, historySeq: 0, clientsSeq: 0 };

  function onDecision(it) {
    var row = P.decisionRow(it);
    P.pushRing(cues.ring, row, P.DECISIONS_RING);
    if (it.type === 'drop') P.pushRing(cues.dropRing, row, P.DECISIONS_RING);
    cues.recentDirty = true;
  }
  function setCuesDropsOnly(on) {
    cues.dropsOnly = !!on;
    $('cues-drops-only').checked = cues.dropsOnly;
    storeSet(CUES_DROPS_ONLY_KEY, cues.dropsOnly ? '1' : '0');
    renderCueRecent();
  }
  function clockTime(iso) {
    var ms = Date.parse(iso);
    return isNaN(ms) ? '' : new Date(ms).toISOString().substr(11, 8) + 'Z';
  }
  function renderCueCounters() {
    var body = $('cues-counters').tBodies[0], s = rx.stats || {};
    if (!body.rows.length) {
      P.CUE_COUNTERS.forEach(function (c) {
        var tr = el('tr');
        tr.setAttribute('data-counter', c[0]);
        tr.appendChild(el('th', null, c[1]));
        tr.appendChild(el('td', 'v'));
        body.appendChild(tr);
      });
    }
    P.CUE_COUNTERS.forEach(function (c, i) {
      var td = body.rows[i].cells[1], text = String(s[c[0]] || 0);
      if (td.textContent !== text) td.textContent = text;
    });
  }
  function renderCueRecent() {
    cues.recentDirty = false;
    var body = $('cues-recent').tBodies[0], ring = cues.dropsOnly ? cues.dropRing : cues.ring;
    body.textContent = '';
    for (var i = ring.length - 1; i >= 0; i--) {
      var r = ring[i], tr = el('tr');
      tr.setAttribute('data-key', r.key);
      tr.appendChild(el('td', null, clockTime(r.time)));
      tr.appendChild(el('td', null, r.hex));
      tr.appendChild(el('td', null, r.callsign));
      var d = el('td', null, r.decision);
      if (r.decision) d.className = 'decision-' + (P.decisionColor(r.decision) || 'unknown');
      tr.appendChild(d);
      tr.appendChild(el('td', null, r.reason));
      body.appendChild(tr);
    }
  }
  function filterDiffText(d) {
    var f = FILTER_TEXT[d.field] || [d.field, ''];
    var v = function (x) { return x === null || x === undefined ? '—' : String(x); };
    return f[0] + ' ' + (d.from === null ? '' : v(d.from) + ' → ') + v(d.to) + f[1];
  }
  function renderCueFilters(changes) {
    var ul = $('cues-filters'), rows = P.filterHistoryRows(changes);
    ul.textContent = '';
    if (!rows.length) ul.appendChild(el('li', 'empty', 'None.'));
    rows.forEach(function (r) {
      var li = el('li');
      li.appendChild(el('span', 'cf-time', clockTime(r.time)));
      li.appendChild(document.createTextNode(' ' + r.origin + ': ' +
        (r.diffs.length ? r.diffs.map(filterDiffText).join(', ') : 'no change')));
      ul.appendChild(li);
    });
  }
  function clientRow(cls, cells) {
    var tr = el('tr', cls);
    cells.forEach(function (c) { tr.appendChild(el('td', null, c)); });
    return tr;
  }
  function renderCueClients(c) {
    var body = $('cues-clients').tBodies[0], tcp = c.tcp || [], udp = c.udp || [], now = Date.now();
    body.textContent = '';
    tcp.forEach(function (t) {
      var ms = Date.parse(t.connected_since);
      body.appendChild(clientRow('tcp', [t.endpoint, t.direction,
        isNaN(ms) ? '' : clockTime(t.connected_since) + ' · ' + P.formatAge(ms, now), P.thousands(t.bytes_sent || 0) + ' B']));
    });
    udp.forEach(function (u) {
      body.appendChild(clientRow('udp', [u.endpoint, 'udp', '', P.thousands(u.datagrams_sent || 0) + ' datagrams']));
    });
    if (!tcp.length && !udp.length) {
      var td = el('td', 'empty', 'None.');
      td.colSpan = 4;
      body.appendChild(el('tr', 'empty')).appendChild(td);
    }
  }
  // A response that an answer to a later request overtook is not shown.
  function fetchCueHistory() {
    var seq = ++cues.historySeq;
    request('GET', 'api/filters/history').then(function (h) {
      if (seq === cues.historySeq) renderCueFilters(h.changes || []);
    }).catch(function () { /* the WebSocket watchdog reports a dead skycued */ });
  }
  function fetchCueClients() {
    var seq = ++cues.clientsSeq;
    request('GET', 'api/cues/clients').then(function (c) {
      if (seq === cues.clientsSeq) renderCueClients(c);
    }).catch(function () { /* as above */ });
  }
  function showCues() {
    renderCueCounters();
    renderCueRecent();
    fetchCueHistory();
    fetchCueClients();
  }
  function pollCues() {
    if (!connected || !tabShown('cues')) return;
    fetchCueHistory();
    fetchCueClients();
  }

  // ---- Config tab ----------------------------------------------------------------
  // docs/operating.md, Config tab. Read only: GET /api/config each time the
  // tab is shown and on Refresh; one table per CONFIG_GROUPS group with each
  // key's value, source and restart badge. "Copy as text" puts configText()
  // on the clipboard, or, where the clipboard refuses, in a selected
  // textarea to copy by hand. Server strings go in through textContent.
  var config = { data: null, seq: 0 };

  function renderConfigDaemon(d) {
    var body = $('config-daemon').tBodies[0], started = Date.parse(d.started);
    var rows = [
      ['Version', d.version],
      ['Started', isNaN(started) ? d.started : d.started + ' (' + P.formatAge(started, Date.now()) + ')'],
      ['Config file', d.config_path],
      ['Inputs file', d.inputs_path],
      ['Overlay file', d.overlay_path + (d.overlay_active ? ' (unsaved changes)' : '')],
      ['www directory', d.www_dir],
      ['Recording to', d.record_dir || 'not recording'],
      ['Recording files', (d.recording_files || []).join(', ') || 'none'],
      ['Log level', d.log_level],
      ['Runs', P.daemonModeText(d)],
      ['Learned locations', P.daemonLocationsText(d)[0]],
      ['Operator locations', P.daemonLocationsText(d)[1]]];
    body.textContent = '';
    rows.forEach(function (r) {
      var tr = el('tr');
      tr.appendChild(el('th', null, r[0]));
      tr.appendChild(el('td', null, r[1] === undefined || r[1] === null ? '' : String(r[1])));
      body.appendChild(tr);
    });
  }
  function renderConfigGroups(keys) {
    var box = $('config-groups');
    box.textContent = '';
    P.groupConfig(keys).forEach(function (g) {
      box.appendChild(el('h2', null, g.group));
      var table = el('table', 'cfg-table'), body = el('tbody');
      table.setAttribute('data-group', g.group);
      g.rows.forEach(function (r) {
        var tr = el('tr'), restart = el('td', 'cfg-restart');
        tr.setAttribute('data-key', r.key);
        tr.appendChild(el('th', null, r.key));
        tr.appendChild(el('td', 'cfg-value', String(r.value)));
        tr.appendChild(el('td', 'cfg-source', r.source));
        if (r.restart) restart.appendChild(el('span', 'badge', 'restart'));
        tr.appendChild(restart);
        body.appendChild(tr);
      });
      table.appendChild(body);
      box.appendChild(table);
    });
  }
  // A response that an answer to a later request overtook is not shown.
  function showConfig() {
    var seq = ++config.seq;
    // A fresh load: the last copy's status and fallback text no longer apply.
    showMsg($('config-copy-status'), '');
    $('config-copy-text').hidden = true;
    $('config-copy-text').value = '';
    request('GET', 'api/config').then(function (c) {
      if (seq !== config.seq) return;
      config.data = c;
      showMsg($('config-msg'), '');
      renderConfigDaemon(c.daemon || {});
      renderConfigGroups(c.keys || []);
      $('config-copy').disabled = false;
    }).catch(function (e) {
      if (seq === config.seq) showMsg($('config-msg'), 'Could not load the configuration: ' + e.message, true);
    });
  }
  function configText() { return config.data ? P.configText(config.data) : ''; }
  function copyConfig() {
    var text = configText(), status = $('config-copy-status'), area = $('config-copy-text');
    var fallback = function () {
      showMsg(status, 'Copy failed', true);
      area.value = text;
      area.hidden = false;
      area.focus();
      area.select();
    };
    if (!navigator.clipboard || !navigator.clipboard.writeText) { fallback(); return; }
    navigator.clipboard.writeText(text).then(function () {
      area.hidden = true;
      showMsg(status, 'Copied');
    }, fallback);
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
    rx.enabledTouched = false;
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
    // Editing: Enabled as the input is now (its card's toggle may have
    // changed it since Edit), unless the box itself was changed.
    var current = id !== null && inputById(id);
    if (current && !rx.enabledTouched) def.enabled = current.enabled;
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
    if (name === 'enabled') rx.enabledTouched = true;
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
      // Pans, keeping the zoom; from a zoom below SHOW_MIN_ZOOM with the
      // device out of view it zooms in to SHOW_MIN_ZOOM.
      var ll = devicePos(inp);
      if (!ll || !map) return;
      if (map.getZoom() < SHOW_MIN_ZOOM && !map.getBounds().contains(ll)) map.setView(ll, SHOW_MIN_ZOOM);
      else map.panTo(ll);
    }
  }

  // ---- WebSocket -----------------------------------------------------------------
  function setConnected(on) {
    connected = on;
    $('conn-banner').hidden = on;
    if (alertsView) alertsView.daemon(on);
  }

  var snapshotCount = 0;            // snapshots applied (a test hook)
  function onSnapshot(s) {
    snapshotCount++;
    // A silent baseline for the "new"-type alert rules (spec 11), the
    // inputs seeded by both of the snapshot's inputs lists.
    if (alertsView) alertsView.beginBaseline();
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
    if (alertsView) alertsView.endBaseline(Array.from(tracks.keys()));
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
      case 'update': flashSent(onTrack(it, false)); onDecision(it); break;
      case 'drop': onDrop(it); onDecision(it); break;
      case 'expired': removeEntry(P.trackKey(it)); break; // never forwarded, gone from skycued's table
      case 'stats': lastStatsWall = Date.now(); applyStats(it); break;
      case 'filters': applyFilters(it); if (tabShown('cues')) fetchCueHistory(); break;
      case 'lists': applyLists(it); break;
      case 'inputs': applyInputs(it.inputs); break;
      case 'fisb_text': case 'fisb_shape': case 'fisb_radar': case 'fisb_raster': case 'fisb_expired':
      case 'fisb_location':
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
  // Each header stat carries its full text as a title, kept in step with
  // every update, so a value cut by the ellipsis (style.css) reads on hover.
  function syncStatTitles() {
    Array.prototype.forEach.call(document.querySelectorAll('#stats > .stat'), function (st) {
      var t = st.textContent.replace(/\s+/g, ' ').trim();
      if (st.title !== t) st.title = t;
    });
  }
  function wire() {
    syncStatTitles();
    new MutationObserver(syncStatTitles).observe($('stats'), { childList: true, characterData: true, subtree: true });
    var stored = null;
    try { stored = window.localStorage.getItem('skycue.hideFiltered'); } catch (err) { /* no storage */ }
    if (stored === '1') setHideFiltered(true);
    wireView();
    wireViewSets();
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
    $('config-refresh').addEventListener('click', showConfig);
    $('config-copy').addEventListener('click', copyConfig);
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
        fisb.symDirty = true;
        renderWx(true);
      }, WX_DEBOUNCE_MS);
    });
    WX_REPORT_KINDS.forEach(function (k) {
      $('wx-show-' + k).addEventListener('change', function () {
        fisb.show[k] = $('wx-show-' + k).checked;
        fisb.symDirty = true;
        renderWx(true);
      });
    });
    ['period', 'level'].forEach(function (k) {
      $('wx-winds-' + k).addEventListener('change', function () {
        fisb.windsPick[k] = Number($('wx-winds-' + k).value);
        renderWx(true);   // syncWinds: the symbols follow
      });
    });
    $('wx-list').addEventListener('click', function (ev) {
      var show = ev.target.closest('.wx-show');
      if (show) {
        ev.preventDefault();
        showWxPos(show.closest('.wx-row').getAttribute('data-key'));
        return;
      }
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
    if (window.skycueStatsView && window.skycueStatsPure) {
      statsView = window.skycueStatsView.create({
        request: request, select: select, hasTrack: function (key) { return tracks.has(key); },
        setViewFilter: function (partial) { setViewFilters(partial); }, showMain: showMain,
        thousands: P.thousands, formatNum: P.formatNum, storeGet: storeGet, storeSet: storeSet, dataNow: dataNow,
        download: downloadText
      });
    } else $('stats-toggle').hidden = true;
    $('stats-toggle').addEventListener('click', function () { showMain(mainView === 'stats' ? 'map' : 'stats'); });
    if (window.skycueAlertsView && window.skycueAlertsPure) {
      alertsView = window.skycueAlertsView.create({
        dataNow: dataNow, storeGet: storeGet, storeSet: storeSet, request: request,
        hasTrack: function (key) { return tracks.has(key); },
        // Whether Hide filtered leaves the track on the map.
        trackVisible: function (key) { return P.trackVisible(tracks.get(key).t.decision, true); },
        getHideFiltered: function () { return hideFiltered; },
        setHideFiltered: setHideFiltered,
        selectTrack: function (key) { showMain('map'); select(key); },
        openReceiver: function () { openPanel('receiver'); },
        openStatsAircraft: function (key) { showMain('stats'); if (statsView) statsView.openAircraft(key); },
        tracks: function () { return Array.from(tracks.values()).map(function (e) { return e.t; }); },
        layoutChanged: function () { if (toolbar && openBoxName) sizeBox(openBoxName); }
      });
    } else {
      $('alerts-toggle').hidden = true;
      $('alerts').hidden = true;
    }
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
    $('cues-drops-only').addEventListener('change', function () { setCuesDropsOnly($('cues-drops-only').checked); });
    setCuesDropsOnly(storeGet(CUES_DROPS_ONLY_KEY) === '1');
    initTrails();
    $('cues-recent').tBodies[0].addEventListener('click', function (ev) {
      var tr = ev.target.closest('tr[data-key]');
      if (tr) select(tr.getAttribute('data-key'));
    });
    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape') {
        setViewSetMenu(false);
        if (ruler.armed) clearRuler();   // the ruler, not the panel too
        else if (openBoxName && $('box-' + openBoxName).contains(document.activeElement)) {
          var n = openBoxName;
          openBox(null);
          $('tb-' + n).focus();
        }
        else closePanel();
      }
      else if (!slashFocusesSearch(ev)) digitAppliesSet(ev);
    });
    setInterval(prune, 2000);
    setInterval(watchdog, 1000);
    setInterval(pollFisb, FISB_POLL_MS);
    setInterval(pollInputs, INPUTS_POLL_MS);
    setInterval(pollCues, CUES_POLL_MS);
    var ticks = 0;
    setInterval(function () {
      if (selectedKey) renderDetails(); // ages
      updateHiddenCount();
      if (viewRenderPending) flushViewRender(); // no animation frames in a hidden tab
      if (tableDirty) { updateViewStatus(); renderTable(); }
      if (map) radarAgeTick();
      if (tabShown('receiver')) renderReceiver(); // ages, rates
      if (tabShown('cues') && cues.recentDirty) renderCueRecent();
      if (++ticks % 15 === 0) fisb.wxDirty = fisb.symDirty = true; // ages and age classes
      renderWx();
      if (fisb.symDirty) renderWxSymbols();
      if (alertsView) alertsView.tick(); // input hold-offs, the log
    }, 1000);
  }

  // Test hooks (tests/ui/ui.spec.js); read-only views of the page state.
  window.skycueApp = {
    mapBox: function () { return openBoxName; },
    layers: function () { return layersState(); },
    setLayers: function (partial) { setLayers(partial); },
    markers: function () {
      var out = [];
      tracks.forEach(function (e) {
        if (!e.marker) return;
        var ll = e.marker.getLatLng();
        out.push({ hex: e.t.hex, addr_type: e.t.addr_type, decision: P.decisionColor(e.t.decision) || 'unknown',
          lat: ll.lat, lon: ll.lng, dimmed: markerEl(e) ? markerEl(e).classList.contains('dimmed') : false, trail: e.trail ? e.trail.getLatLngs().length : 0 });
      });
      return out;
    },
    rangeRingRadius: function () { return rangeRing ? rangeRing.getRadius() : null; },
    // The ruler: armed, its ends ({lat, lon} or null), fixed, the label text.
    ruler: function () {
      var ll = function (p) { return p ? { lat: p.lat, lon: p.lng } : null; };
      var lab = ruler.label && ruler.label.getElement();
      return { armed: ruler.armed, start: ll(ruler.start), end: ll(ruler.end), fixed: ruler.fixed,
        label: lab ? lab.textContent : '' };
    },
    // One aircraft's trail as drawn (null without one): its points, the
    // plain polyline's style, and each run's layer id, points, stroke
    // colour, opacity and dashing ([] with Colour by "decision").
    trail: function (hex) {
      var out = null;
      tracks.forEach(function (e) {
        if (e.t.hex !== hex || !e.trail) return;
        var runs = [];
        (e.runLayers || []).forEach(function (l) {
          runs.push({ id: L.stamp(l), n: l.getLatLngs().length, color: l.options.color, opacity: l.options.opacity,
            dashed: !!l.options.dashArray });
        });
        out = { points: e.trail.getLatLngs().length, color: e.trail.options.color, opacity: e.trail.options.opacity,
          weight: e.trail.options.weight, runs: runs };
      });
      return out;
    },
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
    // One aircraft's projection ahead as drawn (null without one).
    ahead: function (hex) {
      var out = null;
      tracks.forEach(function (e) {
        if (e.t.hex !== hex || !e.ahead) return;
        out = { points: e.ahead.getLatLngs().map(function (p) { return [p.lat, p.lng]; }), color: e.ahead.options.color,
          weight: e.ahead.options.weight, dashed: !!e.ahead.options.dashArray, marker: [e.marker.getLatLng().lat, e.marker.getLatLng().lng] };
      });
      return out;
    },
    // Selects the (first) aircraft with this hex, as a click on its marker would.
    selectHex: function (hex) {
      var key = null;
      tracks.forEach(function (e) { if (key === null && e.t.hex === hex) key = e.key; });
      if (key !== null) select(key);
    },
    connected: function () { return connected; },
    // The Config tab's "copy as text" (empty before GET /api/config answers).
    configText: configText,
    setHideFiltered: setHideFiltered,
    getHideFiltered: function () { return hideFiltered; },
    hiddenCount: updateHiddenCount,
    // The View section: its filters (a copy), merge-normalise-persist, back
    // to the defaults, and N of "showing N of M".
    getViewFilters: function () { return JSON.parse(JSON.stringify(viewFilter)); },
    setViewFilters: function (partial) { setViewFilters(partial); },
    // The main view ('map' | 'table' | 'stats') and the Stats view's state
    // (stats_view.js state()) and a refresh now (a Promise).
    showMain: showMain,
    mainView: function () { return mainView; },
    stats: (function () {
      var f = function () { return statsView ? statsView.state() : null; };
      // The delay before a hidden Stats view disposes its charts (default 60 s).
      f.setDisposeMs = function (ms) { if (statsView) statsView.setDisposeMs(ms); };
      // Opens the Aircraft detail of ledger entry '<hex>|<addr_type>' (a Promise).
      f.openAircraft = function (key) { return statsView ? statsView.openAircraft(key) : Promise.resolve(); };
      return f;
    })(),
    // The alert strip's state (alerts_view.js state()), with its input
    // hold-off (default 10 s) and the rules' config.
    alerts: (function () {
      var f = function () { return alertsView ? alertsView.state() : null; };
      f.setHoldOffMs = function (ms) { if (alertsView) alertsView.setHoldOffMs(ms); };
      f.config = function () { return alertsView ? alertsView.config() : null; };
      f.setConfig = function (cfg) { if (alertsView) alertsView.setConfig(cfg); };
      return f;
    })(),
    statsRefresh: function () { return statsView ? statsView.refreshNow() : Promise.resolve(false); },
    resetViewFilters: resetViewFilters,
    // Saved filter sets: the store (a copy) and the active set's index or null.
    viewSets: function () { return JSON.parse(JSON.stringify(viewSets)); },
    activeViewSet: function () { return activeSet; },
    shownCount: viewShownCount,
    // The 2-s trail and stale-track prune, run now.
    prune: function () { prune(); },
    // Runs a pending View render now (it is otherwise one animation frame away).
    renderAll: function () { viewRenderPending = true; flushViewRender(); },
    // Adds a marker skycued does not know (the reconnect test checks it goes).
    injectTestMarker: function (t) { onTrack(t, true); },
    // Removes a track as a drop or the prune would (the alert strip's "no longer tracked").
    removeTestMarker: function (key) { removeEntry(key); },
    dataNow: dataNow,
    // FIS-B: the initial /api/fisb/* load is done; WebSocket items through
    // the page's own handler; what the radar canvases hold.
    fisbLoaded: function () { return fisb.loaded; },
    injectFisbItems: function (items) { items.forEach(onItem); },
    // Weather symbols: what is drawn; "n of m positioned".
    wxSymbols: function () {
      return Array.from(fisb.syms.values()).map(function (o) {
        var s = o.s;
        return { key: s.key, kind: s.kind, station: s.station, cls: s.cls, lat: s.lat, lon: s.lon, barb: s.barb };
      });
    },
    wxPositioned: wxPositioned,
    // The winds pickers: the period and level the barbs use, and those offered.
    windsPickers: function () {
      var w = fisb.winds;
      return { period: w.period, level: w.level, periods: w.periods.slice(), levels: w.levels.slice() };
    },
    snapshots: function () { return snapshotCount; },
    // Closes the WebSocket: the page reconnects and resets from the snapshot.
    reconnect: function () { if (ws) ws.close(); },
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
    // The regions (scripts/maps/make-tiles.sh), for the tiles on their edges.
    return request('GET', 'tiles/meta.json').then(function (m) {
      tiles.regions = m && Array.isArray(m.regions) ? m.regions : null;
      return tiles;
    }, function () { return tiles; });
  });
  // The overlays (scripts/maps/make-overlays.sh) are optional like the tiles:
  // api/tiles-available says whether overlays/meta.json exists, so it is
  // fetched only then (no request, no 404, without overlays).
  var overlaysReady = tilesReady.then(function (tiles) {
    if (!tiles || tiles.overlays !== true) { overlayMeta = null; return; }
    return fetchJson('overlays/meta.json').then(function (m) {
      overlayMeta = m && typeof m === 'object' ? m : null;
    }, function () { overlayMeta = null; });
  });
  Promise.all([tilesReady, overlaysReady]).then(function (r) {
    var tiles = r[0];
    initMap(tiles);
    connect();
  });
})();
