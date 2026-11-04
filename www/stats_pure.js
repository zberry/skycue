// skycue web UI: the Stats view's pure helpers (no DOM, no ECharts): the
// time ranges and the shaping of /api/stats/* JSON (docs/web-api.md,
// Statistics) into what the charts draw. Shared by the page
// (window.skycueStatsPure, loaded after app_pure.js) and the node test
// (module.exports; it loads ./app_pure.js itself). Plain ES2017.
(function (root) {
  'use strict';

  var P = root && root.skycuePure ? root.skycuePure
    : (typeof require === 'function' ? require('./app_pure.js') : null);

  var HOUR_MS = 3600000;
  // The toolbar's ranges: minute buckets up to 24 h, hour buckets beyond
  // (the daemon keeps 24 h of minutes and 30 days of hours).
  var RANGES = [
    { key: '1h', label: '1 h', ms: HOUR_MS, res: 'minute' },
    { key: '6h', label: '6 h', ms: 6 * HOUR_MS, res: 'minute' },
    { key: '24h', label: '24 h', ms: 24 * HOUR_MS, res: 'minute' },
    { key: '7d', label: '7 d', ms: 7 * 24 * HOUR_MS, res: 'hour' },
    { key: '30d', label: '30 d', ms: 30 * 24 * HOUR_MS, res: 'hour' }
  ];
  var DEFAULT_RANGE = '24h';

  function findRange(key) {
    for (var i = 0; i < RANGES.length; i++) if (RANGES[i].key === key) return RANGES[i];
    return null;
  }
  // A known range key, else the default (24 h).
  function rangeKey(key) { return findRange(key) ? key : DEFAULT_RANGE; }

  // The from/to/res query of a range ending at nowMs (the data clock).
  function rangeQuery(key, nowMs) {
    var r = findRange(rangeKey(key));
    return { from: new Date(nowMs - r.ms).toISOString(), to: new Date(nowMs).toISOString(), res: r.res };
  }

  // The series' bucket start times in ms (NaN for an unparsable one).
  function seriesTimes(series) {
    if (!series || !Array.isArray(series.t)) return [];
    return series.t.map(function (t) { return Date.parse(t); });
  }

  // The series cut to the buckets with fromMs <= t <= toMs: every array
  // column as long as t, and every by_input column; other fields copied.
  function sliceSeries(series, fromMs, toMs) {
    var times = seriesTimes(series);
    var keep = [];
    times.forEach(function (ms, i) { if (ms >= fromMs && ms <= toMs) keep.push(i); });
    var pick = function (arr) { return keep.map(function (i) { return arr[i]; }); };
    var out = {};
    var n = times.length;
    Object.keys(series || {}).forEach(function (k) {
      var v = series[k];
      if (Array.isArray(v) && v.length === n) out[k] = pick(v);
      else if (k === 'by_input' && v && typeof v === 'object') {
        out.by_input = {};
        Object.keys(v).forEach(function (id) { out.by_input[id] = Array.isArray(v[id]) ? pick(v[id]) : v[id]; });
      } else out[k] = v;
    });
    if (!out.t) out.t = [];
    if (!out.by_input) out.by_input = {};
    return out;
  }

  function num(v) { return typeof v === 'number' && isFinite(v) ? v : 0; }

  // [[ms, value]] per bucket of one column, or the per-bucket sum of
  // several (field an array of names); a missing column counts as zeros.
  // A bucket whose time does not parse is left out.
  function spark(series, field) {
    var fields = Array.isArray(field) ? field : [field];
    var out = [];
    seriesTimes(series).forEach(function (ms, i) {
      if (isNaN(ms)) return;
      var v = 0;
      fields.forEach(function (f) { if (Array.isArray(series[f])) v += num(series[f][i]); });
      out.push([ms, v]);
    });
    return out;
  }

  // The drops tile: the decisions that mean "not sent" for reasons of the
  // track's life, the same three the daemon's range.drops sums.
  var DROP_FIELDS = ['drops_timeout', 'drops_filtered', 'drops_source_lost'];

  function countText(v) { return typeof v === 'number' && isFinite(v) ? P.thousands(v) : '—'; }

  // The headline tiles of a summary (its range object) with a sparkline
  // per tile from the range's series: [{id, label, value, text, sub, spark}].
  // value is the number (null when unknown), text its display form.
  function tiles(summary, series) {
    var r = summary && summary.range ? summary.range : {};
    var has = summary && summary.range;
    var v = function (k) { return has && typeof r[k] === 'number' ? r[k] : null; };
    var maxKey = typeof r.max_range_key === 'string' ? r.max_range_key : '';
    var maxNm = v('max_range_nm');
    var out = [
      // "N new" only when it can be read as part of the count (after a
      // restart of the replay's clock the ledger can hold fewer in range).
      { id: 'aircraft', label: 'Aircraft', value: v('aircraft'),
        sub: v('new_aircraft') !== null && v('aircraft') !== null && v('new_aircraft') <= v('aircraft')
          ? countText(v('new_aircraft')) + ' new' : (has ? 'in range' : ''), field: 'aircraft' },
      { id: 'observations', label: 'Observations', value: v('observations'),
        sub: v('positions') === null ? '' : countText(v('positions')) + ' positions', field: 'observations' },
      { id: 'sent', label: 'Sent cues', value: v('sent_updates'), sub: 'updates sent', field: 'sent_updates' },
      { id: 'drops', label: 'Drops', value: v('drops'), sub: 'timeout/filter/lost', field: DROP_FIELDS },
      { id: 'max_range', label: 'Max range', value: maxNm,
        sub: maxNm === null ? '' : maxKey.split('|')[0], field: 'max_range_nm' },
      { id: 'emergencies', label: 'Emergencies', value: v('emergencies'), sub: 'emergency squawks', field: 'emergencies' }
    ];
    return out.map(function (t) {
      return { id: t.id, label: t.label, value: t.value,
        text: t.id === 'max_range' ? P.formatNum(t.value, 1, ' nm') : countText(t.value),
        sub: t.sub, spark: series ? spark(series, t.field) : [] };
    });
  }

  // The toolbar's "since" text from summary.since (UTC, to the minute).
  function sinceText(iso) {
    var ms = Date.parse(iso);
    if (typeof iso !== 'string' || isNaN(ms)) return '';
    var s = new Date(ms).toISOString();
    return 'since ' + s.substr(0, 10) + ' ' + s.substr(11, 5) + 'Z';
  }

  // ---- Traffic and Weather ---------------------------------------------------

  // The sum of a column (or of several) over the series' buckets.
  function total(series, field) {
    return spark(series, field).reduce(function (a, p) { return a + p[1]; }, 0);
  }

  // Observations per bucket by input: [{name, data: [[ms, n]]}], one per
  // by_input key, the largest total first (ties by name), 'other' last.
  function inputSeries(series) {
    var by = series && series.by_input && typeof series.by_input === 'object' ? series.by_input : {};
    var times = seriesTimes(series);
    var out = Object.keys(by).map(function (name) {
      var col = Array.isArray(by[name]) ? by[name] : [];
      var sum = 0;
      var data = [];
      times.forEach(function (ms, i) {
        if (isNaN(ms)) return;   // an unparsable bucket time: left out
        var v = num(col[i]);
        sum += v;
        data.push([ms, v]);
      });
      return { name: name, data: data, sum: sum };
    });
    out.sort(function (a, b) {
      if ((a.name === 'other') !== (b.name === 'other')) return a.name === 'other' ? 1 : -1;
      if (a.sum !== b.sum) return b.sum - a.sum;
      return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
    });
    return out.map(function (x) { return { name: x.name, data: x.data }; });
  }

  // Distinct and new aircraft per bucket.
  function aircraftSeries(series) {
    return { distinct: spark(series, 'aircraft'), fresh: spark(series, 'new_aircraft') };
  }

  // Sent vs drops by reason, in the order of the stacked bar: the updates
  // sent, the drops of a track's life (timeout, source lost) and the
  // filtered drops by the decision that caused them (their sum is
  // drops_filtered, which is therefore not a series of its own).
  var SENT_DROPS = [
    ['sent', 'sent_updates'], ['timeout', 'drops_timeout'], ['source_lost', 'drops_source_lost'],
    ['blocked', 'drops_blocked'], ['out_of_range', 'drops_out_of_range'],
    ['below_elevation', 'drops_below_elevation'], ['no_position', 'drops_no_position']
  ];
  function sentDropSeries(series) {
    return SENT_DROPS.map(function (p) { return { name: p[0], data: spark(series, p[1]) }; });
  }

  // The 7 x 24 hour-of-week cells of an hour series' observations (UTC),
  // summed over the weeks it holds: [[hour 0..23, day 0..6 (Mon = 0), n]],
  // all 168, day by day then hour by hour.
  function hourOfWeek(hourSeries, field) {
    var f = field || 'observations';
    var sums = [];
    for (var k = 0; k < 168; k++) sums.push(0);
    var col = hourSeries && Array.isArray(hourSeries[f]) ? hourSeries[f] : [];
    seriesTimes(hourSeries).forEach(function (ms, i) {
      if (isNaN(ms)) return;
      var d = new Date(ms);
      sums[((d.getUTCDay() + 6) % 7) * 24 + d.getUTCHours()] += num(col[i]);
    });
    return sums.map(function (n, k) { return [k % 24, Math.floor(k / 24), n]; });
  }
  function hourOfWeekMax(cells) {
    return (cells || []).reduce(function (a, c) { return Math.max(a, c[2]); }, 0);
  }

  // UAT frames (downlink, uplink) and FIS-B products per bucket.
  function uatSeries(series) {
    return { downlink: spark(series, 'uat_downlink'), uplink: spark(series, 'uat_uplink'),
      products: spark(series, 'fisb_products') };
  }

  // Whether the Weather (FIS-B) section shows: summary.uat_seen (a UAT
  // observation, frame or FIS-B product has ever been counted since the
  // last reset). null without a summary: keep what is shown.
  function uatShown(summary) {
    if (!summary || typeof summary !== 'object') return null;
    return summary.uat_seen === true;
  }

  var api = {
    RANGES: RANGES, DEFAULT_RANGE: DEFAULT_RANGE, rangeKey: rangeKey, rangeQuery: rangeQuery,
    seriesTimes: seriesTimes, sliceSeries: sliceSeries, spark: spark, DROP_FIELDS: DROP_FIELDS,
    tiles: tiles, sinceText: sinceText,
    total: total, inputSeries: inputSeries, aircraftSeries: aircraftSeries, SENT_DROPS: SENT_DROPS,
    sentDropSeries: sentDropSeries, hourOfWeek: hourOfWeek, hourOfWeekMax: hourOfWeekMax,
    uatSeries: uatSeries, uatShown: uatShown
  };
  if (root) root.skycueStatsPure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
