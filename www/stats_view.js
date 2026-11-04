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
// of each; hours, coverage and rssi are fetched on open, after a reset
// and every 6th refresh, and kept until the next fetch of them; each is
// null until fetched and null after its own fetch failed, which leaves
// the tiles and the other data drawn). It adds its cards
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
      tiles: {}, detailKey: null, disposeMs: DISPOSE_MS
    };
    var data = { summary: null, series: null, hours: null, coverage: null, rssi: null };
    var slots = [];               // every chart: {id, kind: 'card'|'spark', el, inst, ro}
    var cards = {};               // card id -> its handle
    var sections = [];
    var aircraftHooks = {};       // the Aircraft section's state() and open (made below)
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
    // A sparkline spans its first bucket to its last (each point sits at its
    // bucket's start, so an axis to the bucket's end would leave a gap), a
    // dot per bucket while there are few.
    function renderTiles(summary, series) {
      S.tiles(summary, series).forEach(function (t) {
        var n = t.spark.length;
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
          xAxis: { type: 'time', show: false, min: n ? t.spark[0][0] : null, max: n > 1 ? t.spark[n - 1][0] : null },
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
      // Snapshot and clear: an open() or reset while this one is in flight
      // sets it again for the next refresh.
      var slow = st.slowDue;
      st.slowDue = false;
      // A failing slow request is null on its own (logged), not a failed refresh.
      var opt = function (path) {
        return get(path).catch(function (err) { console.error('stats ' + path, err); return null; });
      };
      var reqs = [get('api/stats/summary?' + qs), get('api/stats/series?res=' + q.res + '&' + qs)];
      if (slow) reqs.push(opt('api/stats/series?res=hour'), opt('api/stats/coverage'), opt('api/stats/rssi'));
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
        st.slowDue = st.slowDue || st.refreshes % SLOW_EVERY === 0;
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
        if (slow) st.slowDue = true;   // not fetched: the next refresh does
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
      st.disposeTimer = setTimeout(disposeCharts, st.disposeMs);
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
        tiles: tiles, detailKey: st.detailKey, aircraft: aircraftHooks.state ? aircraftHooks.state() : null
      };
    }

    // The sections register here, each with registerSection(fn), using
    // sectionEl(name) and addCard (see the head of this file). A null field
    // of the data (a slow fetch not made yet or failed) leaves the cards
    // that draw from it as they are.

    // ---- shared chart pieces ---------------------------------------------------
    var tk = theme.tokens || {};
    var dec = theme.decision || {};
    var pal = theme.color || [];
    var fmt = function (v) { return typeof v === 'number' && isFinite(v) ? host.thousands(v) : '—'; };
    var rangeOf = function (summary) { return summary && summary.range ? summary.range : null; };
    var perBucket = function (series) { return series && series.res === 'hour' ? '/h' : '/min'; };
    // A tooltip value formatter with the unit after the number.
    var unitFmt = function (unit) { return function (v) { return fmt(v) + ' ' + unit; }; };
    // A time axis from half a bucket before the first bucket to half one
    // after the last (each point sits at its bucket's start), so one bucket
    // is not drawn on an axis of days, and bars are not cut at the edges.
    function timeAxis(series) {
      var times = S.seriesTimes(series);
      var half = (series && series.bucket_s ? series.bucket_s : 60) * 500;
      var n = times.length;
      return { type: 'time', axisLabel: { hideOverlap: true },
        min: n ? times[0] - half : null, max: n ? times[n - 1] + half : null };
    }
    function countAxis() {
      return { type: 'value', min: 0, minInterval: 1, axisLabel: { formatter: function (v) { return fmt(v); } } };
    }
    var LEGEND = { type: 'scroll', top: 0, left: 0, right: 0, itemWidth: 12, itemHeight: 8, textStyle: { fontSize: 11 } };
    var LEGEND_WRAP = { type: 'plain', top: 0, left: 0, right: 0, itemGap: 8, itemWidth: 12, itemHeight: 8,
      textStyle: { fontSize: 11 } };

    // ---- Traffic (spec 6 item 2) -----------------------------------------------
    var SENT_DROP_LABEL = { sent: 'sent', timeout: 'timeout', source_lost: 'source lost', blocked: 'blocked',
      out_of_range: 'out of range', below_elevation: 'below elevation', no_position: 'no position' };
    // Decision colours for the decision series; the two drops of a track's
    // life take two theme hues the decisions do not use.
    var SENT_DROP_COLOUR = { sent: dec.sent, timeout: pal[5] || '#e3b341', source_lost: pal[6] || '#56d4dd',
      blocked: dec.blocked, out_of_range: dec.range, below_elevation: dec.elev, no_position: dec.unknown };
    var DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    var HOURS = [];
    for (var hh = 0; hh < 24; hh++) HOURS.push((hh < 10 ? '0' : '') + hh);

    function drawInputs(card, d) {
      if (!d.series) return;
      var list = S.inputSeries(d.series);
      var r = rangeOf(d.summary);
      card.setCaption((r ? fmt(r.observations) : fmt(S.total(d.series, 'observations'))) + ' observations · ' +
        list.length + (list.length === 1 ? ' input' : ' inputs'));
      var ch = card.chart();
      if (!ch) return;
      var unit = 'obs' + perBucket(d.series);
      ch.setOption({
        animation: false,
        useUTC: true,   // UTC like the toolbar's since and the hour-of-week heatmap
        legend: LEGEND,
        tooltip: { trigger: 'axis', valueFormatter: unitFmt(unit) },
        grid: { left: 48, right: 24, top: 26, bottom: 54 },
        xAxis: timeAxis(d.series),
        yAxis: countAxis(),
        // No start/end here: a merge keeps the window the user zoomed to.
        dataZoom: [{ id: 'in', type: 'inside', xAxisIndex: 0, filterMode: 'none' },
          { id: 'slide', type: 'slider', xAxisIndex: 0, height: 18, bottom: 6, filterMode: 'none' }],
        // replaceMerge: an input gone from the range loses its series.
        series: list.map(function (x) {
          return { id: 'in:' + x.name, name: x.name, type: 'line', stack: 'obs', data: x.data,
            symbol: 'circle', showSymbol: x.data.length < 3, symbolSize: 4, lineStyle: { width: 1 }, areaStyle: { opacity: 0.35 } };
        })
      }, { replaceMerge: ['series'] });
    }

    function drawAircraft(card, d) {
      if (!d.series) return;
      var a = S.aircraftSeries(d.series);
      var r = rangeOf(d.summary);
      // Without the range totals, only the new ones can be summed (distinct
      // aircraft per bucket do not add up to the range's).
      card.setCaption(r ? fmt(r.aircraft) + ' aircraft · ' + fmt(r.new_aircraft) + ' new'
        : fmt(S.total(d.series, 'new_aircraft')) + ' new');
      var ch = card.chart();
      if (!ch) return;
      ch.setOption({
        animation: false,
        useUTC: true,   // UTC like the toolbar's since and the hour-of-week heatmap
        legend: LEGEND,
        tooltip: { trigger: 'axis', valueFormatter: unitFmt('aircraft' + perBucket(d.series)) },
        grid: { left: 40, right: 24, top: 26, bottom: 24 },
        xAxis: timeAxis(d.series),
        yAxis: countAxis(),
        series: [
          { id: 'distinct', name: 'distinct', type: 'bar', data: a.distinct, barMaxWidth: 12,
            itemStyle: { color: tk.accent } },
          { id: 'new', name: 'new', type: 'line', data: a.fresh, symbol: 'circle', showSymbol: a.fresh.length < 3, symbolSize: 4,
            lineStyle: { width: 1.5, color: dec.sent }, itemStyle: { color: dec.sent } }
        ]
      });
    }

    function drawSent(card, d) {
      if (!d.series) return;
      var list = S.sentDropSeries(d.series);
      var r = rangeOf(d.summary);
      card.setCaption((r ? fmt(r.sent_updates) : fmt(S.total(d.series, 'sent_updates'))) + ' sent · ' +
        (r ? fmt(r.drops) : fmt(S.total(d.series, S.DROP_FIELDS))) + ' drops');
      var ch = card.chart();
      if (!ch) return;
      ch.setOption({
        animation: false,
        useUTC: true,   // UTC like the toolbar's since and the hour-of-week heatmap
        legend: LEGEND_WRAP,   // all seven reasons on view (two rows), not paged
        tooltip: { trigger: 'axis', valueFormatter: unitFmt(d.series.res === 'hour' ? 'per hour' : 'per minute') },
        grid: { left: 40, right: 24, top: 44, bottom: 24 },
        xAxis: timeAxis(d.series),
        yAxis: countAxis(),
        // Two bars per bucket: the updates sent, and the drops stacked by reason.
        series: list.map(function (x) {
          return { id: x.name, name: SENT_DROP_LABEL[x.name], type: 'bar', stack: x.name === 'sent' ? 'sent' : 'drops',
            data: x.data, barMaxWidth: 10, itemStyle: { color: SENT_DROP_COLOUR[x.name] } };
        })
      });
    }

    function drawWeek(card, d) {
      if (!d.hours) return;   // not fetched yet, or its fetch failed: keep the last one
      var cells = S.hourOfWeek(d.hours);
      var max = S.hourOfWeekMax(cells);
      var busiest = null;
      cells.forEach(function (c) { if (c[2] > 0 && (!busiest || c[2] > busiest[2])) busiest = c; });
      card.setCaption(fmt(S.total(d.hours, 'observations')) + ' observations' +
        (busiest ? ' · busiest ' + DAYS[busiest[1]] + ' ' + HOURS[busiest[0]] + ':00' : '') + ' · UTC');
      var ch = card.chart();
      if (!ch) return;
      ch.setOption({
        animation: false,
        tooltip: {
          formatter: function (p) {
            var h = p.value[0];
            return DAYS[p.value[1]] + ' ' + HOURS[h] + ':00–' + (h === 23 ? '24' : HOURS[h + 1]) + ':00 UTC<br>' +
              fmt(p.value[2]) + ' observations';
          }
        },
        grid: { left: 36, right: 30, top: 4, bottom: 46 },
        xAxis: { type: 'category', data: HOURS, axisTick: { show: false }, axisLabel: { interval: 2 },
          name: 'UTC', nameLocation: 'end', nameGap: 4 },
        yAxis: { type: 'category', data: DAYS, inverse: true, axisTick: { show: false } },
        visualMap: { type: 'continuous', min: 0, max: max || 1, calculable: false, orient: 'horizontal',
          left: 'center', bottom: 0, itemWidth: 10, itemHeight: 140, text: [fmt(max), '0'],
          inRange: { color: [tk.line || '#30363d', tk.accent || '#58a6ff'] } },
        series: [{ id: 'week', name: 'observations', type: 'heatmap', data: cells,
          itemStyle: { borderColor: tk.panel, borderWidth: 1 }, emphasis: { itemStyle: { borderColor: tk.text } } }]
      });
    }

    registerSection(function (d) {
      var grid = sectionEl('traffic');
      drawInputs(addCard(grid, 'traffic-inputs', 'Observations by input'), d);
      drawAircraft(addCard(grid, 'traffic-aircraft', 'Aircraft per bucket'), d);
      drawSent(addCard(grid, 'traffic-sent', 'Sent vs drops'), d);
      drawWeek(addCard(grid, 'traffic-week', 'Busy hours of the week (UTC)'), d);
    });

    // ---- Weather (FIS-B, spec 6 item 6) ----------------------------------------
    // Shown while summary.uat_seen (a UAT observation, frame or FIS-B product
    // counted since the last reset); its card is made the first time.
    registerSection(function (d) {
      var shown = S.uatShown(d.summary);
      if (shown === null) return;   // no summary: leave it as it is
      var grid = sectionEl('weather');
      var sec = grid.closest('.stats-section');
      if (!shown) {
        if (sec) sec.hidden = true;
        return;
      }
      var card = addCard(grid, 'weather-uat', 'UAT frames and FIS-B products');
      if (sec) sec.hidden = false;
      if (!d.series) return;
      var u = S.uatSeries(d.series);
      card.setCaption(fmt(S.total(d.series, 'uat_uplink')) + ' uplink · ' + fmt(S.total(d.series, 'uat_downlink')) +
        ' downlink frames · ' + fmt(S.total(d.series, 'fisb_products')) + ' products');
      var ch = card.chart();
      if (!ch) return;
      var per = perBucket(d.series);
      var line = function (id, data, col, unit) {
        return { id: id, name: id, type: 'line', data: data, symbol: 'circle', showSymbol: data.length < 3, symbolSize: 4,
          lineStyle: { width: 1.5, color: col }, itemStyle: { color: col },
          tooltip: { valueFormatter: unitFmt(unit + per) } };
      };
      ch.setOption({
        animation: false,
        useUTC: true,   // UTC like the toolbar's since and the hour-of-week heatmap
        legend: LEGEND,
        tooltip: { trigger: 'axis' },
        grid: { left: 40, right: 24, top: 26, bottom: 24 },
        xAxis: timeAxis(d.series),
        yAxis: countAxis(),
        series: [
          line('uplink', u.uplink, tk.accent, 'frames'),
          line('downlink', u.downlink, dec.sent, 'frames'),
          line('products', u.products, dec.elev, 'products')
        ]
      });
    });

    // ---- Aircraft (the ledger, spec 6 item 7) ----------------------------------
    // A table of GET api/stats/aircraft (search, header sort, 100 a page;
    // the whole ledger, not the range), and a detail card of one entry.
    // Every API string goes in through textContent.
    var SEARCH_DEBOUNCE_MS = 300;
    var ac = { q: '', sort: 'last_seen', dir: 'desc', page: 0, total: 0, evicted: 0, rows: [], seq: 0,
      built: false, searchTimer: null, detail: null, detailSeq: 0 };
    var acEls = {};
    function mk(tag, attrs, text) {
      var el = document.createElement(tag);
      Object.keys(attrs || {}).forEach(function (k) { el.setAttribute(k, attrs[k]); });
      if (text !== undefined) el.textContent = text;
      return el;
    }
    // The section's two cards, made on its first refresh: the table (with
    // the search, the pager and the evicted count) and the detail (hidden
    // until a row is opened; its chart is a card chart, so it is disposed
    // and recreated like the others).
    function buildAircraft() {
      if (ac.built) return;
      ac.built = true;
      var grid = sectionEl('aircraft');
      var card = mk('section', { 'class': 'card stats-wide', 'data-card': 'aircraft-table' });
      card.appendChild(mk('h3', { 'class': 'card-title' }, 'Aircraft ledger'));
      acEls.caption = card.appendChild(mk('p', { 'class': 'card-caption', id: 'stats-aircraft-caption' }));
      var bar = card.appendChild(mk('div', { 'class': 'stats-aircraft-bar' }));
      acEls.search = bar.appendChild(mk('input', { type: 'search', id: 'stats-aircraft-search', autocomplete: 'off',
        spellcheck: 'false', placeholder: 'hex, callsign or registration', 'aria-label': 'Search the aircraft ledger' }));
      var pager = bar.appendChild(mk('div', { id: 'stats-aircraft-pager', role: 'group', 'aria-label': 'Pages' }));
      acEls.prev = pager.appendChild(mk('button', { type: 'button', 'data-page': 'prev', 'aria-label': 'Previous page' }, '‹ prev'));
      acEls.pageText = pager.appendChild(mk('span', { 'class': 'stats-pager-text', 'aria-live': 'polite' }));
      acEls.next = pager.appendChild(mk('button', { type: 'button', 'data-page': 'next', 'aria-label': 'Next page' }, 'next ›'));
      var wrap = card.appendChild(mk('div', { 'class': 'stats-table-wrap' }));
      var table = wrap.appendChild(mk('table', { id: 'stats-aircraft-table' }));
      var tr = table.appendChild(mk('thead')).appendChild(mk('tr'));
      S.AIRCRAFT_COLS.forEach(function (c) {
        var a = { 'data-key': c.key };
        if (c.num) a['class'] = 'num';
        if (c.sort) { a['data-sort'] = c.sort; a.tabindex = '0'; a['aria-sort'] = 'none'; } else a['class'] = (c.num ? 'num ' : '') + 'nosort';
        tr.appendChild(mk('th', a, c.label));
      });
      acEls.headers = tr;
      acEls.body = table.appendChild(mk('tbody'));
      acEls.evicted = card.appendChild(mk('p', { id: 'stats-evicted', 'class': 'card-caption' }));
      acEls.evicted.hidden = true;
      grid.appendChild(card);

      var det = addCard(grid, 'aircraft-detail', 'Aircraft');
      det.el.id = 'stats-detail';
      det.el.classList.add('stats-wide');
      det.el.hidden = true;
      var head = mk('div', { 'class': 'stats-detail-actions' });
      acEls.showOnMap = head.appendChild(mk('button', { type: 'button', id: 'stats-show-on-map' }, 'Show on map'));
      acEls.filterMap = head.appendChild(mk('button', { type: 'button', id: 'stats-filter-map' }, 'Filter the map to it'));
      acEls.closeDetail = head.appendChild(mk('button', { type: 'button', id: 'stats-detail-close', 'aria-label': 'Close the detail' }, 'Close'));
      var chartEl = det.el.querySelector('.card-chart');
      chartEl.id = 'stats-detail-polar';
      var body = mk('div', { 'class': 'stats-detail-body' });
      acEls.fields = body.appendChild(mk('dl', { 'class': 'stats-detail-fields' }));
      det.el.insertBefore(head, chartEl);
      det.el.insertBefore(body, chartEl);
      body.appendChild(chartEl);
      acEls.detailCard = det;
      var sec = grid.closest('.stats-section');
      if (sec) sec.hidden = false;

      acEls.search.addEventListener('input', function () {
        clearTimeout(ac.searchTimer);
        ac.searchTimer = setTimeout(function () {
          ac.searchTimer = null;
          if (acEls.search.value.trim() === ac.q) return;
          ac.q = acEls.search.value.trim();
          ac.page = 0;
          loadAircraft();
        }, SEARCH_DEBOUNCE_MS);
      });
      var sortOn = function (th) {
        var name = th && th.getAttribute('data-sort');
        if (!name) return;
        // The same header again flips the order; another starts high to low.
        ac.dir = ac.sort === name ? (ac.dir === 'desc' ? 'asc' : 'desc') : 'desc';
        ac.sort = name;
        ac.page = 0;
        syncHeaders();
        loadAircraft();
      };
      tr.addEventListener('click', function (ev) { sortOn(ev.target.closest('th[data-sort]')); });
      tr.addEventListener('keydown', function (ev) {
        if (ev.key !== 'Enter' && ev.key !== ' ') return;
        var th = ev.target.closest('th[data-sort]');
        if (th) { ev.preventDefault(); sortOn(th); }
      });
      pager.addEventListener('click', function (ev) {
        var b = ev.target.closest('button[data-page]');
        if (!b || b.disabled) return;
        ac.page = Math.max(0, ac.page + (b.getAttribute('data-page') === 'next' ? 1 : -1));
        loadAircraft();
      });
      acEls.body.addEventListener('click', function (ev) {
        var row = ev.target.closest('tr[data-key]');
        if (row) openAircraft(row.getAttribute('data-key'));
      });
      acEls.body.addEventListener('keydown', function (ev) {
        if (ev.key !== 'Enter') return;
        var row = ev.target.closest('tr[data-key]');
        if (row) { ev.preventDefault(); openAircraft(row.getAttribute('data-key')); }
      });
      acEls.showOnMap.addEventListener('click', function () {
        var key = st.detailKey;
        if (!key || !host.hasTrack(key)) return;
        host.showMain('map');
        host.select(key);
      });
      acEls.filterMap.addEventListener('click', function () {
        if (!ac.detail) return;
        host.setViewFilter({ search: ac.detail.hex });
        host.showMain('map');
      });
      acEls.closeDetail.addEventListener('click', closeAircraft);
      syncHeaders();
    }
    function syncHeaders() {
      Array.prototype.forEach.call(acEls.headers.querySelectorAll('th[data-sort]'), function (th) {
        var on = th.getAttribute('data-sort') === ac.sort;
        th.classList.toggle('sorted', on);
        th.classList.toggle('desc', on && ac.dir === 'desc');
        th.setAttribute('aria-sort', on ? (ac.dir === 'desc' ? 'descending' : 'ascending') : 'none');
      });
    }
    function renderAircraftTable() {
      var body = acEls.body;
      while (body.firstChild) body.removeChild(body.firstChild);
      ac.rows.forEach(function (r) {
        var tr = mk('tr', { 'data-key': String(r.key), tabindex: '0' });
        if (r.key === st.detailKey) tr.className = 'selected';
        S.AIRCRAFT_COLS.forEach(function (c) {
          var a = { 'data-key': c.key };
          if (c.num) a['class'] = 'num';
          tr.appendChild(mk('td', a, S.aircraftCell(r, c.key)));
        });
        body.appendChild(tr);
      });
      var pg = S.aircraftPager(ac.total, ac.page, ac.rows.length);
      acEls.pageText.textContent = pg.text;
      acEls.prev.disabled = !pg.prev;
      acEls.next.disabled = !pg.next;
      acEls.evicted.hidden = !(ac.evicted > 0);
      acEls.evicted.textContent = ac.evicted > 0 ? 'and ' + fmt(ac.evicted) + ' older aircraft' +
        ' (dropped from the ledger when it was full)' : '';
      acEls.caption.classList.remove('error');
      acEls.caption.textContent = fmt(ac.total) + (ac.q ? ' matching' : ' in the ledger') + ' · since the last reset';
    }
    // The current page (a newer request wins over an older answer). A
    // page past the end (the ledger shrank, e.g. after a reset) moves to
    // the last page.
    function loadAircraft() {
      var seq = ++ac.seq;
      return get(S.aircraftQuery({ q: ac.q, sort: ac.sort, dir: ac.dir, page: ac.page })).then(function (r) {
        if (seq !== ac.seq) return;
        ac.total = r && typeof r.total === 'number' ? r.total : 0;
        ac.evicted = r && typeof r.evicted === 'number' ? r.evicted : 0;
        ac.rows = r && Array.isArray(r.rows) ? r.rows : [];
        if (!ac.rows.length && ac.page > S.aircraftLastPage(ac.total)) {
          ac.page = S.aircraftLastPage(ac.total);
          return loadAircraft();
        }
        renderAircraftTable();
      }, function (err) {
        if (seq !== ac.seq) return;
        acEls.caption.textContent = 'Aircraft list unavailable: ' + (err && err.message);
        acEls.caption.classList.add('error');
      });
    }

    // ---- the detail card ---------------------------------------------------------
    function detailPath(key) {
      var k = String(key).split('|');
      return 'api/stats/aircraft/' + encodeURIComponent(k[0]) + (k[1] ? '?addr_type=' + encodeURIComponent(k[1]) : '');
    }
    function syncShowOnMap() {
      if (acEls.showOnMap) acEls.showOnMap.hidden = !(st.detailKey && host.hasTrack(st.detailKey));
    }
    function renderDetail() {
      var row = ac.detail, card = acEls.detailCard;
      if (!row || !card) return;
      var call = Array.isArray(row.callsigns) && row.callsigns.length ? ' · ' + row.callsigns[row.callsigns.length - 1] : '';
      card.el.querySelector('.card-title').textContent = 'Aircraft ' + row.hex + call;
      var p = S.detailPolar(row);
      card.setCaption(p ? 'farthest ' + host.formatNum(p.range_nm, 1, ' nm') + ' at ' + host.formatNum(p.bearing_deg, 0, '°') +
        ' true' : 'no range yet (needs receiver coordinates and a position)');
      var dl = acEls.fields;
      while (dl.firstChild) dl.removeChild(dl.firstChild);
      S.detailRows(row).forEach(function (f) {
        var div = dl.appendChild(mk('div', { 'data-field': f[2] }));
        div.appendChild(mk('dt', {}, f[0]));
        div.appendChild(mk('dd', {}, f[1]));
      });
      syncShowOnMap();
      Array.prototype.forEach.call(acEls.body.querySelectorAll('tr[data-key]'), function (tr) {
        tr.classList.toggle('selected', tr.getAttribute('data-key') === st.detailKey);
      });
      var ch = card.chart();
      if (!ch) return;
      var max = p ? p.max_nm : 50;
      ch.setOption({
        animation: false,
        tooltip: { trigger: 'item', formatter: function () {
          return p ? 'max range ' + host.formatNum(p.range_nm, 1, ' nm') + '<br>bearing ' + host.formatNum(p.bearing_deg, 1, '°') : '';
        } },
        polar: { radius: '78%' },
        angleAxis: { type: 'value', min: 0, max: 360, startAngle: 90, clockwise: true, interval: 45,
          axisLabel: { formatter: function (v) { return { 0: 'N', 90: 'E', 180: 'S', 270: 'W' }[v] || ''; } },
          splitLine: { show: true } },
        radiusAxis: { type: 'value', min: 0, max: max, interval: max / 2,
          axisLabel: { formatter: function (v) { return v ? v + ' nm' : ''; }, fontSize: 10 } },
        series: [
          { id: 'ray', type: 'line', coordinateSystem: 'polar', silent: true, symbol: 'none',
            lineStyle: { color: tk.accent, width: 1.5, opacity: 0.6 }, data: p ? [[0, p.bearing_deg], [p.range_nm, p.bearing_deg]] : [] },
          { id: 'max', name: 'max range', type: 'scatter', coordinateSystem: 'polar', symbolSize: 10,
            itemStyle: { color: tk.accent }, data: p ? [[p.range_nm, p.bearing_deg]] : [] }
        ]
      });
    }
    // Opens the detail of ledger entry `key` ('<hex>|<addr_type>'): at once
    // from the table's row when it is on the page, then from the entry route.
    function openAircraft(key) {
      buildAircraft();
      st.detailKey = String(key);
      var cached = null;
      ac.rows.forEach(function (r) { if (r.key === st.detailKey) cached = r; });
      acEls.detailCard.el.hidden = false;
      if (cached) { ac.detail = cached; renderDetail(); }
      acEls.detailCard.el.scrollIntoView({ block: 'nearest' });
      return loadDetail();
    }
    function loadDetail() {
      var key = st.detailKey;
      if (!key) return Promise.resolve();
      var seq = ++ac.detailSeq;
      return get(detailPath(key)).then(function (row) {
        if (seq !== ac.detailSeq || key !== st.detailKey) return;
        ac.detail = row;
        renderDetail();
      }, function (err) {
        if (seq !== ac.detailSeq || key !== st.detailKey) return;
        // Gone (a reset) or never there: close it; another failure keeps the last.
        if (err && err.status === 404) closeAircraft();
        else console.error('stats aircraft ' + key, err);
      });
    }
    function closeAircraft() {
      st.detailKey = null;
      ac.detail = null;
      ac.detailSeq++;
      if (acEls.detailCard) acEls.detailCard.el.hidden = true;
      if (acEls.body) Array.prototype.forEach.call(acEls.body.querySelectorAll('tr.selected'), function (tr) {
        tr.classList.remove('selected');
      });
    }

    registerSection(function () {
      buildAircraft();
      return Promise.all([loadAircraft(), loadDetail()]);
    });
    aircraftHooks.state = function () {
      return { q: ac.q, sort: ac.sort, dir: ac.dir, page: ac.page, total: ac.total, evicted: ac.evicted,
        rows: ac.rows.map(function (r) { return r.key; }) };
    };
    aircraftHooks.open = openAircraft;

    // setDisposeMs: the dispose delay for the next close() (tests).
    return { open: open, close: close, isOpen: function () { return st.open; }, state: state, refreshNow: refreshNow,
      setDisposeMs: function (ms) { st.disposeMs = ms >= 0 ? ms : DISPOSE_MS; },
      openAircraft: function (key) { return aircraftHooks.open(key); } };
  }

  window.skycueStatsView = { create: create };
})();
