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
  function spark(series, field) {
    var fields = Array.isArray(field) ? field : [field];
    return seriesTimes(series).map(function (ms, i) {
      var v = 0;
      fields.forEach(function (f) { if (Array.isArray(series[f])) v += num(series[f][i]); });
      return [ms, v];
    });
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
      { id: 'aircraft', label: 'Aircraft', value: v('aircraft'),
        sub: v('new_aircraft') === null ? '' : countText(v('new_aircraft')) + ' new', field: 'aircraft' },
      { id: 'observations', label: 'Observations', value: v('observations'),
        sub: v('positions') === null ? '' : countText(v('positions')) + ' positions', field: 'observations' },
      { id: 'sent', label: 'Sent cues', value: v('sent_updates'), sub: 'updates sent', field: 'sent_updates' },
      { id: 'drops', label: 'Drops', value: v('drops'), sub: 'timeout · filtered · source lost', field: DROP_FIELDS },
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

  var api = {
    RANGES: RANGES, DEFAULT_RANGE: DEFAULT_RANGE, rangeKey: rangeKey, rangeQuery: rangeQuery,
    seriesTimes: seriesTimes, sliceSeries: sliceSeries, spark: spark, DROP_FIELDS: DROP_FIELDS,
    tiles: tiles, sinceText: sinceText
  };
  if (root) root.skycueStatsPure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
