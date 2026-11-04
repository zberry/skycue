// skycue web UI: the Stats view (#stats-view), the third main view beside
// the map and the table. It polls /api/stats/* (docs/web-api.md,
// Statistics) while it is shown and draws the headline tiles and the cards
// of its sections with ECharts (theme 'skycue', chart_theme.js). The data
// shaping lives in stats_pure.js (window.skycueStatsPure, S here).
//
// window.skycueStatsView.create(host) -> {open, close, isOpen, state, refreshNow}
// host = {request(method, path, body), select(key), hasTrack(key),
//         setViewFilter(partial), showMain(which), thousands, formatNum,
//         storeGet(k), storeSet(k, v), dataNow()}  (app.js builds it).
//
// A section is a function registerSection(fn): fn(data) runs after every
// refresh with data = {summary, series, hours, coverage, rssi} (the newest
// of each; hours, coverage and rssi are fetched on open and every 6th
// refresh, null until then or after a failed fetch). It adds its cards
// once with addCard(sectionEl('traffic'), id, title) and on every call sets
// each card's whole option with card.chart().setOption(option) (a merge,
// so the same code serves a chart that was just created or recreated after
// a dispose). It may return a Promise (its own requests); refreshNow waits
// for it. card.chart() is null while ECharts is missing or the view is
// closed. Plain ES2017 (functions and var, like app.js).
(function () {
  'use strict';

  var S = window.skycueStatsPure;
  var LIVE_MS = 10000;          // live refresh period while the view is shown
  var DISPOSE_MS = 60000;       // charts are disposed once hidden this long
  var SLOW_EVERY = 6;           // hours, coverage and rssi: on open and every 6th refresh
  var RANGE_KEY = 'skycue.stats.range';
  var RESET_QUESTION = 'Reset all statistics? The counters, charts and aircraft ledger start again from now; ' +
    'this cannot be undone.';
  // The tiles' sparkline colours (theme.decision names or 'accent').
  var TILE_COLOURS = { aircraft: 'accent', observations: 'accent', sent: 'sent', drops: 'elev',
    max_range: 'unknown', emergencies: 'blocked' };

  function create(host) {
    var $ = function (id) { return document.getElementById(id); };
    var st = {
      open: false, range: S.rangeKey(host.storeGet(RANGE_KEY)), res: null, refreshes: 0,
      slowDue: true, liveTimer: null, disposeTimer: null, chain: Promise.resolve(true),
      tiles: {}, detailKey: null
    };
    var data = { summary: null, series: null, hours: null, coverage: null, rssi: null };
    var slots = [];               // every chart: {id, kind: 'card'|'spark', el, inst, ro}
    var cards = {};               // card id -> its handle
    var sections = [];
    var theme = window.skycueChartTheme ? window.skycueChartTheme() : { decision: {}, tokens: {} };

    function showMsg(text, isError) {
      var el = $('stats-msg');
      el.textContent = text || '';
      el.classList.toggle('error', !!isError);
    }

    // ---- charts --------------------------------------------------------------
    function addSlot(id, kind, el) {
      var s = { id: id, kind: kind, el: el, inst: null, ro: null };
      slots.push(s);
      return s;
    }
    // The slot's chart, created on first use while the view is shown (an
    // element that is not laid out would give a 0 x 0 chart).
    function slotChart(s) {
      if (s.inst) return s.inst;
      if (!window.echarts || !st.open) return null;
      s.inst = window.echarts.init(s.el, 'skycue');
      if (typeof ResizeObserver === 'function') {
        s.ro = new ResizeObserver(function () { if (s.inst) s.inst.resize(); });
        s.ro.observe(s.el);
      }
      return s.inst;
    }
    function disposeCharts() {
      st.disposeTimer = null;
      slots.forEach(function (s) {
        if (s.ro) s.ro.disconnect();
        s.ro = null;
        if (s.inst) s.inst.dispose();
        s.inst = null;
      });
    }
    function sectionEl(name) { return $('stats-' + name); }
    // A card in a section's grid: <section class="card" data-card="<id>">
    // with its title, one-line caption and chart. Made once per id; the
    // section it is in shows from its first card on.
    function addCard(sectionGrid, id, title) {
      if (cards[id]) return cards[id];
      var el = document.createElement('section');
      el.className = 'card';
      el.setAttribute('data-card', id);
      el.innerHTML = '<h3 class="card-title"></h3><p class="card-caption"></p><div class="card-chart"></div>';
      el.querySelector('.card-title').textContent = title;
      sectionGrid.appendChild(el);
      var sec = sectionGrid.closest('.stats-section');
      if (sec) sec.hidden = false;
      var s = addSlot(id, 'card', el.querySelector('.card-chart'));
      var caption = el.querySelector('.card-caption');
      cards[id] = {
        el: el,
        chart: function () { return slotChart(s); },
        setCaption: function (text) { caption.textContent = text || ''; }
      };
      return cards[id];
    }
    function registerSection(fn) { sections.push(fn); }

    // ---- headline tiles --------------------------------------------------------
    var tileEls = {};
    S.tiles(null, null).forEach(function (t) {
      var el = document.createElement('div');
      el.className = 'tile';
      el.setAttribute('data-tile', t.id);
      el.innerHTML = '<div class="tile-label"></div><div class="tile-value"></div><div class="tile-sub"></div>' +
        '<div class="tile-spark" aria-hidden="true"></div>';
      el.querySelector('.tile-label').textContent = t.label;
      el.querySelector('.tile-value').textContent = t.text;
      $('stats-tiles').appendChild(el);
      tileEls[t.id] = { el: el, slot: addSlot(t.id, 'spark', el.querySelector('.tile-spark')) };
      st.tiles[t.id] = t.text;
    });
    // A sparkline spans its first bucket to the range's end (the daemon's
    // data clock), a dot per bucket while there are few.
    function renderTiles(summary, series) {
      var r = summary && summary.range ? summary.range : {};
      var to = Date.parse(r.to);
      S.tiles(summary, series).forEach(function (t) {
        var te = tileEls[t.id];
        te.el.querySelector('.tile-value').textContent = t.text;
        te.el.querySelector('.tile-sub').textContent = t.sub;
        st.tiles[t.id] = t.text;
        var chart = slotChart(te.slot);
        if (!chart) return;
        var name = TILE_COLOURS[t.id];
        var col = (name === 'accent' ? theme.tokens.accent : theme.decision[name]) || '#58a6ff';
        chart.setOption({
          animation: false,
          grid: { left: 0, right: 0, top: 2, bottom: 1 },
          tooltip: { show: false },
          xAxis: { type: 'time', show: false, min: t.spark.length ? t.spark[0][0] : null, max: isNaN(to) ? null : to },
          yAxis: { type: 'value', show: false, min: 0 },
          series: [{ type: 'line', data: t.spark, showSymbol: t.spark.length < 3, symbol: 'circle', symbolSize: 3,
            itemStyle: { color: col }, silent: true,
            lineStyle: { width: 1.5, color: col }, areaStyle: { color: col, opacity: 0.15 } }]
        });
      });
    }

    // ---- refresh ---------------------------------------------------------------
    function get(path) { return host.request('GET', path); }
    // One refresh: the range's summary and series (plus, when due, the
    // whole hour series, coverage and rssi), then the tiles and every
    // section. Never rejects: true when it drew, false when a request failed.
    function doRefresh() {
      var q = S.rangeQuery(st.range, host.dataNow());
      // No `to`: the daemon's data clock ends the range. The page's
      // estimate of it (dataNow) can trail by a moment, which would leave
      // out the aircraft seen in that moment (most of them, live).
      var qs = 'from=' + encodeURIComponent(q.from);
      var slow = st.slowDue;
      var reqs = [get('api/stats/summary?' + qs), get('api/stats/series?res=' + q.res + '&' + qs)];
      if (slow) reqs.push(get('api/stats/series?res=hour'), get('api/stats/coverage'), get('api/stats/rssi'));
      return Promise.all(reqs).then(function (r) {
        data.summary = r[0];
        data.series = r[1];
        if (slow) {
          data.hours = r[2];
          data.coverage = r[3];
          data.rssi = r[4];
        }
        st.res = q.res;
        st.refreshes++;
        st.slowDue = st.refreshes % SLOW_EVERY === 0;
        showMsg('');
        $('stats-since').textContent = S.sinceText(data.summary && data.summary.since);
        if (!st.open) return true;
        renderTiles(data.summary, data.series);
        var d = { summary: data.summary, series: data.series, hours: data.hours, coverage: data.coverage, rssi: data.rssi };
        return Promise.all(sections.map(function (fn) {
          try {
            return Promise.resolve(fn(d)).catch(function (err) { console.error('stats section', err); });
          } catch (err) {
            console.error('stats section', err);
            return null;
          }
        })).then(function () { return true; });
      }).catch(function (err) {
        showMsg('Statistics unavailable: ' + (err && err.message), true);
        return false;
      });
    }
    // Refreshes run one at a time, in order.
    function refreshNow() {
      st.chain = st.chain.then(doRefresh);
      return st.chain;
    }
    function liveOn() { return $('stats-live').checked; }
    function schedule() {
      clearTimeout(st.liveTimer);
      st.liveTimer = null;
      if (!st.open || !liveOn()) return;
      st.liveTimer = setTimeout(function () {
        st.liveTimer = null;
        if (!st.open) return;
        if (document.hidden) { schedule(); return; } // a background tab: no requests
        refreshNow().then(schedule);
      }, LIVE_MS);
    }

    // ---- toolbar ---------------------------------------------------------------
    function syncRange() {
      Array.prototype.forEach.call($('stats-range').querySelectorAll('button[data-range]'), function (b) {
        b.setAttribute('aria-pressed', String(b.getAttribute('data-range') === st.range));
      });
    }
    function setRange(key) {
      key = S.rangeKey(key);
      if (key === st.range) return;
      st.range = key;
      host.storeSet(RANGE_KEY, key);
      syncRange();
      if (st.open) refreshNow();
    }
    $('stats-range').addEventListener('click', function (ev) {
      var b = ev.target.closest('button[data-range]');
      if (b) setRange(b.getAttribute('data-range'));
    });
    $('stats-live').addEventListener('change', function () {
      if (!st.open) return;
      if (liveOn()) refreshNow().then(schedule);
      else schedule();
    });
    $('stats-reset').addEventListener('click', function () {
      if (!window.confirm(RESET_QUESTION)) return;
      var btn = $('stats-reset');
      btn.disabled = true;
      host.request('POST', 'api/stats/reset', {}).then(function () {
        st.slowDue = true;
        return refreshNow().then(function (ok) { if (ok) showMsg('Statistics reset.'); });
      }, function (err) {
        showMsg('Reset failed: ' + (err && err.message), true);
      }).then(function () { btn.disabled = false; });
    });
    syncRange();

    // ---- open / close ------------------------------------------------------------
    function open() {
      if (st.open) return;
      st.open = true;
      clearTimeout(st.disposeTimer);
      st.disposeTimer = null;
      st.slowDue = true;
      refreshNow().then(schedule);
    }
    function close() {
      if (!st.open) return;
      st.open = false;
      clearTimeout(st.liveTimer);
      st.liveTimer = null;
      clearTimeout(st.disposeTimer);
      st.disposeTimer = setTimeout(disposeCharts, DISPOSE_MS);
    }
    function hasCanvas(s) { return !!s.el.querySelector('canvas'); }
    function state() {
      var tiles = {};
      Object.keys(st.tiles).forEach(function (k) { tiles[k] = st.tiles[k]; });
      return {
        open: st.open, range: st.range, res: st.res, refreshes: st.refreshes, liveTimer: st.liveTimer !== null,
        cards: slots.filter(function (s) { return s.kind === 'card'; })
          .map(function (s) { return { id: s.id, canvas: hasCanvas(s) }; }),
        sparks: slots.filter(function (s) { return s.kind === 'spark'; })
          .map(function (s) { return { id: s.id, canvas: hasCanvas(s) }; }),
        tiles: tiles, detailKey: st.detailKey
      };
    }

    // The sections register here, each with registerSection(fn), using
    // sectionEl(name) and addCard (see the head of this file).

    return { open: open, close: close, isOpen: function () { return st.open; }, state: state, refreshNow: refreshNow };
  }

  window.skycueStatsView = { create: create };
})();
