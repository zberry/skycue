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

  // ---- Aircraft (the ledger table and detail) --------------------------------

  var AIRCRAFT_PAGE = 100;      // rows per page of the table
  // The sort names GET /api/stats/aircraft accepts.
  var AIRCRAFT_SORTS = ['first_seen', 'last_seen', 'observations', 'positions', 'sessions', 'sent_updates', 'drops',
    'emergencies', 'max_range_nm', 'max_range_time', 'max_range_lat', 'max_range_lon', 'max_range_bearing_deg',
    'min_alt_ft', 'max_alt_ft', 'max_gs_kt', 'min_rssi', 'max_rssi'];
  // The table's columns: row field, header, right-aligned, the API sort
  // name (null: the API cannot sort by it).
  var AIRCRAFT_COLS = [
    { key: 'hex', label: 'hex', num: false, sort: null },
    { key: 'callsigns', label: 'callsigns', num: false, sort: null },
    { key: 'registration', label: 'reg', num: false, sort: null },
    { key: 'category', label: 'cat', num: false, sort: null },
    { key: 'class', label: 'class', num: false, sort: null },
    { key: 'country', label: 'country', num: false, sort: null },
    { key: 'first_seen', label: 'first seen', num: false, sort: 'first_seen' },
    { key: 'last_seen', label: 'last seen', num: false, sort: 'last_seen' },
    { key: 'sessions', label: 'sessions', num: true, sort: 'sessions' },
    { key: 'observations', label: 'obs', num: true, sort: 'observations' },
    { key: 'sent_updates', label: 'sent', num: true, sort: 'sent_updates' },
    { key: 'max_range_nm', label: 'max range (nm)', num: true, sort: 'max_range_nm' },
    { key: 'max_alt_ft', label: 'max alt (ft)', num: true, sort: 'max_alt_ft' },
    { key: 'max_gs_kt', label: 'max gs (kt)', num: true, sort: 'max_gs_kt' }
  ];
  var DASH = '—';
  function isNum(v) { return typeof v === 'number' && isFinite(v); }
  function str(v) { return typeof v === 'string' && v !== '' ? v : DASH; }
  function list(v, sep) { return Array.isArray(v) && v.length ? v.join(sep) : DASH; }
  function count(v) { return isNum(v) ? P.thousands(v) : DASH; }
  // 'MM-DD HH:MMZ' (the table, no year so the measures fit; the cell's
  // title and the detail have the full time), or the full ISO time.
  function shortTime(iso) {
    var ms = Date.parse(iso);
    if (typeof iso !== 'string' || isNaN(ms)) return DASH;
    var s = new Date(ms).toISOString();
    return s.substr(5, 5) + ' ' + s.substr(11, 5) + 'Z';
  }
  function isoTime(iso) {
    var ms = Date.parse(iso);
    return typeof iso !== 'string' || isNaN(ms) ? DASH : new Date(ms).toISOString();
  }

  // The page request: sort (an API name, else last_seen), dir (asc|desc,
  // else desc), page (0-based) and the search text (trimmed; left out when
  // blank), URI-encoded. limit: the rows per page (AIRCRAFT_PAGE when absent).
  function aircraftQuery(o, limit) {
    o = o || {};
    var per = isNum(limit) && limit > 0 ? Math.floor(limit) : AIRCRAFT_PAGE;
    var sort = AIRCRAFT_SORTS.indexOf(o.sort) >= 0 ? o.sort : 'last_seen';
    var dir = o.dir === 'asc' ? 'asc' : 'desc';
    var page = isNum(o.page) && o.page > 0 ? Math.floor(o.page) : 0;
    var q = typeof o.q === 'string' ? o.q.trim() : '';
    return 'api/stats/aircraft?sort=' + sort + '&dir=' + dir + '&limit=' + per +
      '&offset=' + page * per + (q ? '&q=' + encodeURIComponent(q) : '');
  }

  // One table cell's text (a dash when the row has no value).
  function aircraftCell(row, key) {
    if (!row) return DASH;
    var v = row[key];
    switch (key) {
      case 'callsigns': return list(v, ' ');
      case 'first_seen': case 'last_seen': return shortTime(v);
      case 'sessions': case 'observations': case 'sent_updates': case 'max_alt_ft': return count(v);
      case 'max_range_nm': return P.formatNum(v, 1);
      case 'max_gs_kt': return P.formatNum(v, 0);
      default: return str(v);
    }
  }

  // The pager of page `page` (0-based) holding nRows of `total`:
  // {text: 'N–M of T', prev, next}.
  function aircraftPager(total, page, nRows) {
    total = isNum(total) ? total : 0;
    var off = page * AIRCRAFT_PAGE;
    var text = total === 0 ? 'no aircraft'
      : nRows > 0 ? P.thousands(off + 1) + '–' + P.thousands(off + nRows) + ' of ' + P.thousands(total)
        : '0 of ' + P.thousands(total);
    return { text: text, prev: page > 0, next: off + AIRCRAFT_PAGE < total };
  }
  // The last page index holding rows of `total` (0 when none).
  function aircraftLastPage(total) {
    return isNum(total) && total > 0 ? Math.floor((total - 1) / AIRCRAFT_PAGE) : 0;
  }

  // The detail card: [label, text, field] for every field of a ledger
  // row (docs/web-api.md), in reading order, with units; times in ISO.
  function alt(v) { return isNum(v) ? P.thousands(v) + ' ft' + (v === 0 ? ' (ground)' : '') : DASH; }
  var DETAIL_FIELDS = [
    ['hex', 'address', str], ['addr_type', 'address type', str], ['key', 'track key', str],
    ['callsigns', 'callsigns', function (v) { return list(v, ', '); }], ['registration', 'registration', str],
    ['category', 'category', str], ['class', 'class', str], ['country', 'country', str],
    ['squawks', 'squawks', function (v) { return list(v, ', '); }], ['source', 'source', str],
    ['inputs', 'inputs', function (v) { return list(v, ', '); }],
    ['first_seen', 'first seen', isoTime], ['last_seen', 'last seen', isoTime],
    ['sessions', 'sessions', count], ['observations', 'observations', count], ['positions', 'positions', count],
    ['sent_updates', 'updates sent', count], ['drops', 'drops', count], ['emergencies', 'emergencies', count],
    ['max_range_nm', 'max range', function (v) { return P.formatNum(v, 1, ' nm'); }],
    ['max_range_bearing_deg', 'max range bearing', function (v) { return P.formatNum(v, 1, '°'); }],
    ['max_range_time', 'max range time', isoTime],
    ['max_range_lat', 'max range lat', function (v) { return P.formatNum(v, 4, '°'); }],
    ['max_range_lon', 'max range lon', function (v) { return P.formatNum(v, 4, '°'); }],
    ['min_alt_ft', 'min altitude', alt], ['max_alt_ft', 'max altitude', alt],
    ['max_gs_kt', 'max ground speed', function (v) { return P.formatNum(v, 1, ' kt'); }],
    ['min_rssi', 'min signal', function (v) { return P.formatNum(v, 1, ' dBFS'); }],
    ['max_rssi', 'max signal', function (v) { return P.formatNum(v, 1, ' dBFS'); }]
  ];
  function detailRows(row) {
    if (!row || typeof row !== 'object') return [];
    return DETAIL_FIELDS.map(function (f) { return [f[1], f[2](row[f[0]]), f[0]]; });
  }

  // The detail's mini polar: the max-range point and a round outer ring
  // (nm), null without a bearing and range.
  var POLAR_RINGS = [5, 10, 20, 50, 100, 150, 200, 300, 400, 500];   // halves are round too
  function detailPolar(row) {
    if (!row || !isNum(row.max_range_nm) || !isNum(row.max_range_bearing_deg)) return null;
    var r = row.max_range_nm, max = Math.ceil(r / 100) * 100 || 5;
    for (var i = 0; i < POLAR_RINGS.length; i++) if (POLAR_RINGS[i] >= r) { max = POLAR_RINGS[i]; break; }
    return { range_nm: r, bearing_deg: row.max_range_bearing_deg, max_nm: max };
  }

  // ---- Coverage, Fleet and Signal (spec 6 items 3-5) ----------------------------
  var COVERAGE_BANDS = ['0-2.5k', '2.5-5k', '5-10k', '10-20k', '20-30k', '30-40k', '40k+', 'unknown'];
  var SECTORS = 72;

  // GET api/stats/coverage's cells as the polar chart's series: one per
  // band (in the coverage's band order), 72 sector values each (the band's
  // largest range there, 0 where empty), holder(sector, band) -> that
  // cell or null, and max = the farthest cell of all (null when none).
  function polarSeries(coverage) {
    var names = coverage && Array.isArray(coverage.bands) && coverage.bands.length ? coverage.bands : COVERAGE_BANDS;
    var deg = coverage && isNum(coverage.sector_deg) && coverage.sector_deg > 0 ? coverage.sector_deg : 360 / SECTORS;
    var n = Math.round(360 / deg);
    var angles = [], i;
    for (i = 0; i < n; i++) angles.push(i * deg);
    var bands = names.map(function (name) {
      var data = [];
      for (var k = 0; k < n; k++) data.push(0);
      return { name: name, data: data };
    });
    var cells = {}, max = null;
    var list = coverage && Array.isArray(coverage.cells) ? coverage.cells : [];
    list.forEach(function (c) {
      if (!c || !isNum(c.sector) || !isNum(c.band) || !isNum(c.max_range_nm)) return;
      if (c.sector < 0 || c.sector >= n || c.band < 0 || c.band >= bands.length) return;
      bands[c.band].data[c.sector] = c.max_range_nm;
      cells[c.sector + ':' + c.band] = c;
      if (!max || c.max_range_nm > max.max_range_nm) max = c;
    });
    return {
      angles: angles, bands: bands, max: max,
      holder: function (sector, band) { return cells[sector + ':' + band] || null; }
    };
  }

  // A histogram {start, bin, counts} as bars: labels "<start>–<end>" of each
  // bin and the counts. The daemon counts values below the first edge in
  // the first bin and those at or above the last edge in the last, so the
  // first is "<end>" and the last "<start>+". With trim, the empty bins before the first count
  // and after the last are left out (none at all when every bin is empty).
  function histBars(h, trim) {
    var out = { labels: [], values: [] };
    if (!h || !Array.isArray(h.counts) || !isNum(h.start) || !isNum(h.bin)) return out;
    var lo = 0, hi = h.counts.length - 1;
    if (trim) {
      while (lo <= hi && !(h.counts[lo] > 0)) lo++;
      while (hi >= lo && !(h.counts[hi] > 0)) hi--;
    }
    for (var i = lo; i <= hi; i++) {
      var a = h.start + i * h.bin, b = a + h.bin;
      out.labels.push(i === h.counts.length - 1 ? a + '+' : i === 0 ? '<' + b : a + '–' + b);
      out.values.push(isNum(h.counts[i]) ? h.counts[i] : 0);
    }
    return out;
  }

  // GET api/stats/rssi as heatmap cells [rangeBin, rssiBin, n] (non-zero
  // only, range-major) and the median signal per range column
  // [rangeBin, dB at the median bin's centre] (columns without counts left
  // out; an even count takes the lower middle). ranges/levels: the matrix size.
  function rssiHeatmap(rssi) {
    var out = { data: [], median: [], ranges: 0, levels: 0 };
    if (!rssi || !Array.isArray(rssi.counts)) return out;
    var start = isNum(rssi.rssi_start) ? rssi.rssi_start : -50;
    var bin = isNum(rssi.rssi_bin_db) ? rssi.rssi_bin_db : 2;
    out.ranges = rssi.counts.length;
    rssi.counts.forEach(function (row, r) {
      if (!Array.isArray(row)) return;
      if (row.length > out.levels) out.levels = row.length;
      var sum = 0;
      row.forEach(function (n, b) {
        if (isNum(n) && n > 0) { out.data.push([r, b, n]); sum += n; }
      });
      if (!sum) return;
      var cum = 0;
      for (var b = 0; b < row.length; b++) {
        cum += isNum(row[b]) && row[b] > 0 ? row[b] : 0;
        if (cum >= sum / 2) { out.median.push([r, start + b * bin + bin / 2]); break; }
      }
    });
    return out;
  }

  // A count object {name: n} as donut slices, most first (ties by name),
  // zeros dropped.
  function donut(obj) {
    if (!obj || typeof obj !== 'object') return [];
    return Object.keys(obj).filter(function (k) { return isNum(obj[k]) && obj[k] > 0; })
      .map(function (k) { return { name: k, value: obj[k] }; })
      .sort(function (a, b) { return b.value - a.value || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0); });
  }

  // summary.range.by_country ([{name, n}], most first) as the first n bars
  // and one final 'other' holding the rest plus the daemon's own 'other'.
  function topCountries(byCountry, n) {
    if (!Array.isArray(byCountry)) return [];
    var other = 0, list = [];
    byCountry.forEach(function (c) {
      if (!c || !isNum(c.n)) return;
      if (c.name === 'other') other += c.n;
      else list.push({ name: String(c.name), value: c.n });
    });
    list.sort(function (a, b) { return b.value - a.value || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0); });
    list.slice(n).forEach(function (c) { other += c.value; });
    list = list.slice(0, n);
    if (other > 0) list.push({ name: 'other', value: other });
    return list;
  }

  // The position grid (GET api/stats/coverage's grid, [lat, lon, n]) as
  // heatmap cells [lon, lat, n] and a view square on the ground around the
  // receiver (else the cells' centre) that holds every cell: the latitude
  // span is the longitude span times cos(latitude), so 50 nm rings are
  // round on a square plot. Half a side is 50 nm at the least. null
  // without cells. centre = [lon, lat]; halfNm = half the side in nm.
  var NM_PER_DEG = 60;
  function gridView(coverage) {
    var g = coverage && coverage.grid;
    var cells = g && Array.isArray(g.cells) ? g.cells.filter(function (c) {
      return Array.isArray(c) && isNum(c[0]) && isNum(c[1]) && isNum(c[2]);
    }) : [];
    if (!cells.length) return null;
    var cell = isNum(g.cell_deg) ? g.cell_deg : 0.05, h = cell / 2;
    var rx = coverage.receiver && isNum(coverage.receiver.lat) && isNum(coverage.receiver.lon) ? coverage.receiver : null;
    var lat0, lon0;
    if (rx) { lat0 = rx.lat; lon0 = rx.lon; } else {
      var b = [Infinity, -Infinity, Infinity, -Infinity];
      cells.forEach(function (c) {
        b[0] = Math.min(b[0], c[0] - h); b[1] = Math.max(b[1], c[0] + h);
        b[2] = Math.min(b[2], c[1] - h); b[3] = Math.max(b[3], c[1] + h);
      });
      lat0 = (b[0] + b[1]) / 2;
      lon0 = (b[2] + b[3]) / 2;
    }
    var cos = Math.max(0.05, Math.cos(lat0 * Math.PI / 180));
    var half = 50 / NM_PER_DEG;   // degrees of latitude
    cells.forEach(function (c) {
      half = Math.max(half, Math.abs(c[0] - lat0) + h, (Math.abs(c[1] - lon0) + h) * cos);
    });
    return {
      data: cells.map(function (c) { return [c[1], c[0], c[2]]; }),
      xMin: lon0 - half / cos, xMax: lon0 + half / cos, yMin: lat0 - half, yMax: lat0 + half,
      cell: cell, centre: [lon0, lat0], receiver: !!rx, halfNm: half * NM_PER_DEG
    };
  }

  // A range ring nm around the receiver: 72 points [lon, lat] every 5°
  // (great circle, P.destinationPoint).
  function rangeRing(receiver, nm) {
    if (!receiver || !isNum(receiver.lat) || !isNum(receiver.lon)) return [];
    var out = [];
    for (var b = 0; b < 360; b += 5) {
      var p = P.destinationPoint(receiver.lat, receiver.lon, b, nm);
      out.push([p[1], p[0]]);
    }
    return out;
  }
  // The rings of a square view of half side halfNm: every 50 nm out to
  // its corners, one at the least.
  function ringRadii(halfNm) {
    var out = [50];
    for (var r = 100; r <= halfNm * Math.SQRT2; r += 50) out.push(r);
    return out;
  }

  // The view filter a Fleet click sets (ruling 5): a category slice A1-A7
  // is itself, 'none' is 'none', any other code 'other'; a class slice is
  // that class alone; a country bar is that country, the daemon's 'unknown'
  // (no country) the filter's 'other', and the remainder bar ('other',
  // many countries) none (null).
  var FILTER_CATEGORIES = ['A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7'];
  function categoryFilter(code) {
    if (FILTER_CATEGORIES.indexOf(code) >= 0 || code === 'none') return { category: code };
    return { category: 'other' };
  }
  function classFilter(name) {
    return P.CLASSES.indexOf(name) >= 0 ? { classes: [name] } : null;
  }
  function countryFilter(name) {
    if (!name || name === 'other') return null;
    return { country: name === 'unknown' ? 'other' : String(name) };
  }
  // A ledger key '<hex>|<addr_type>' -> its hex.
  function keyHex(key) { return key ? String(key).split('|')[0] : ''; }

  // ---- CSV export (operator tools spec 5) ---------------------------------------
  // RFC 4180 text of rows (arrays of cells): a cell holding '"', ',', CR or
  // LF is quoted with its '"' doubled; null/undefined are empty; every
  // record ends with CRLF. The page makes the Blob from this string (UTF-8,
  // no BOM).
  function csvCell(v) {
    if (v === null || v === undefined) return '';
    var s = String(v);
    return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }
  function csvText(rows) {
    return (rows || []).map(function (r) { return r.map(csvCell).join(',') + '\r\n'; }).join('');
  }

  // Every field of a ledger row (docs/web-api.md), the header with its unit
  // where the name has none.
  var LEDGER_CSV_COLS = ['key', 'hex', 'addr_type', 'first_seen', 'last_seen', 'observations', 'positions', 'sessions',
    'sent_updates', 'drops', 'emergencies', 'max_range_nm', 'max_range_time', ['max_range_lat', 'max_range_lat_deg'],
    ['max_range_lon', 'max_range_lon_deg'], 'max_range_bearing_deg', 'min_alt_ft', 'max_alt_ft', 'max_gs_kt',
    ['min_rssi', 'min_rssi_dbfs'], ['max_rssi', 'max_rssi_dbfs'], 'callsigns', 'squawks', 'inputs', 'registration',
    'category', 'source', 'class', 'country'].map(function (c) {
    return Array.isArray(c) ? { key: c[0], head: c[1] } : { key: c, head: c };
  });
  var LEDGER_EXPORT_PAGE = 1000;
  // The header, then a record per ledger row: lists joined with a space,
  // an absent field empty. A key already seen is left out (the ledger can
  // grow while the export pages through it, shifting a row into the next page).
  function ledgerCsvRows(rows) {
    var out = [LEDGER_CSV_COLS.map(function (c) { return c.head; })];
    var seen = {};
    (rows || []).forEach(function (r) {
      if (!r || Object.prototype.hasOwnProperty.call(seen, r.key)) return;
      seen[r.key] = true;
      out.push(LEDGER_CSV_COLS.map(function (c) {
        var v = r[c.key];
        if (Array.isArray(v)) return v.join(' ');
        return v === null || v === undefined ? '' : v;
      }));
    });
    return out;
  }
  // The export's page at offset (a multiple of LEDGER_EXPORT_PAGE) in the
  // table's sort, direction and search.
  function ledgerExportQuery(o, offset) {
    o = o || {};
    return aircraftQuery({ sort: o.sort, dir: o.dir, q: o.q, page: Math.floor((offset || 0) / LEDGER_EXPORT_PAGE) },
      LEDGER_EXPORT_PAGE);
  }

  // The series columns in reading order (GET api/stats/series).
  var SERIES_CSV_COLS = ['observations', 'positions', 'aircraft', 'new_aircraft', 'sent_updates', 'drops_timeout',
    'drops_filtered', 'drops_source_lost', 'drops_blocked', 'drops_out_of_range', 'drops_below_elevation',
    'drops_no_position', 'uat_downlink', 'uat_uplink', 'fisb_products', 'emergencies', 'tcp_clients', 'web_clients',
    'max_range_nm', 'max_range_key'];
  // A record per bucket: time, the listed columns the series has, any
  // other bucket column (an array as long as t) by name, then
  // obs_<input> per by_input key by name.
  function seriesCsvRows(series) {
    var s = series && typeof series === 'object' ? series : {};
    var t = Array.isArray(s.t) ? s.t : [];
    var isCol = function (k) { return k !== 't' && Array.isArray(s[k]) && s[k].length === t.length; };
    var cols = SERIES_CSV_COLS.filter(isCol);
    cols = cols.concat(Object.keys(s).filter(function (k) { return isCol(k) && cols.indexOf(k) < 0; }).sort());
    var by = s.by_input && typeof s.by_input === 'object' ? s.by_input : {};
    var inputs = Object.keys(by).sort();
    var out = [['time'].concat(cols, inputs.map(function (id) { return 'obs_' + id; }))];
    t.forEach(function (time, i) {
      out.push([time].concat(cols.map(function (k) { return s[k][i]; }),
        inputs.map(function (id) { return Array.isArray(by[id]) ? by[id][i] : null; })));
    });
    return out;
  }

  // The coverage table: a record per sector and band (sector-major), its
  // start bearing, the band's name, and the cell's range, holder, time and
  // observations (empty, 0 observations, where the cell has none).
  function coverageCsvRows(coverage) {
    var p = polarSeries(coverage);
    var out = [['sector_start_deg', 'band', 'max_range_nm', 'holder', 'time', 'n']];
    p.angles.forEach(function (a, sector) {
      p.bands.forEach(function (b, band) {
        var h = p.holder(sector, band);
        out.push(h ? [a, b.name, h.max_range_nm, h.key, h.time, h.n] : [a, b.name, '', '', '', 0]);
      });
    });
    return out;
  }

  // 'skycue-<what>-YYYY-MM-DD.csv' (the UTC date of nowMs).
  function csvFileName(what, nowMs) { return 'skycue-' + what + '-' + P.utcDate(nowMs) + '.csv'; }

  // ---- The Flow card's model (pure; the view in stats_view.js draws it) ----
  // A ring of counter snapshots {t (ms), stats, inputs, clients, files[, outputs]}
  // -> per-second rates -> Sankey nodes, links and tags.
  var FLOW_KEEP = 12;
  var FLOW_COUNTERS = ['observations', 'sent_updates', 'sent_drops', 'drops_blocked', 'drops_out_of_range',
    'drops_below_elevation', 'drops_no_position'];
  var FLOW_GAP = 30;          // nodeGap, px
  var FLOW_MIN_H = 360;       // minimum chart height, px
  var FLOW_LINE_PX = 15;      // one label line
  var FLOW_THIN = 0.01;       // the value of a band that carries nothing (about 1 px)

  function num(v) { return typeof v === 'number' && isFinite(v) ? v : 0; }

  function flowWindow() { return { samples: [] }; }

  // Append a snapshot, keep the last 12. A counter below the previous sample's
  // (skycued restarted) replaces the window with the snapshot alone.
  function flowPush(win, snap) {
    var prev = win.samples.length ? win.samples[win.samples.length - 1] : null;
    if (prev) {
      for (var i = 0; i < FLOW_COUNTERS.length; i++) {
        var k = FLOW_COUNTERS[i];
        if (snap.stats && prev.stats && num(snap.stats[k]) < num(prev.stats[k])) { win.samples = []; break; }
      }
    }
    win.samples.push(snap);
    if (win.samples.length > FLOW_KEEP) win.samples = win.samples.slice(win.samples.length - FLOW_KEEP);
    return win;
  }

  function inputCounters(row) {
    var frames = num(row.frames), msgs = num(row.messages);
    return { msg: Math.max(frames, msgs), rejected: num(row.rejected) + num(row.crc_fail), uplinks: num(row.uplinks) };
  }
  function byId(list) {
    var m = {};
    (list || []).forEach(function (r) { if (r && r.id != null) m[r.id] = r; });
    return m;
  }
  function clientList(c) {
    var out = [];
    ((c && c.tcp) || []).forEach(function (t) { out.push({ key: 'tcp:' + t.endpoint, kind: 'tcp', row: t }); });
    ((c && c.udp) || []).forEach(function (u) { out.push({ key: 'udp:' + u.endpoint, kind: 'udp', row: u }); });
    return out;
  }

  // Rates over the whole window (oldest to newest sample); null under 2 samples.
  function flowRates(win) {
    var s = win && win.samples;
    if (!s || s.length < 2) return null;
    var a = s[0], b = s[s.length - 1];
    var span = (b.t - a.t) / 1000;
    if (!(span > 0)) return null;
    var d = function (x, y) { return Math.max(0, num(x) - num(y)) / span; };
    var sa = a.stats || {}, sb = b.stats || {};
    var oldIn = byId(sa.inputs);
    var r = { span_s: span, inputs: {}, decoder: { in_s: 0, rejected_s: 0 }, tracks_s: 0, decisions: {}, cues_s: 0, clients: [], files: [] };
    (sb.inputs || []).forEach(function (row) {
      var nc = inputCounters(row), oc = oldIn[row.id] ? inputCounters(oldIn[row.id]) : nc;
      var o = { msg_s: d(nc.msg, oc.msg), rejected_s: d(nc.rejected, oc.rejected), uplinks_s: d(nc.uplinks, oc.uplinks) };
      r.inputs[row.id] = o;
      r.decoder.in_s += o.msg_s;
      r.decoder.rejected_s += o.rejected_s;
    });
    r.tracks_s = Math.max(0, r.decoder.in_s - r.decoder.rejected_s);
    var dec = {
      range: d(sb.drops_out_of_range, sa.drops_out_of_range), elev: d(sb.drops_below_elevation, sa.drops_below_elevation),
      blocked: d(sb.drops_blocked, sa.drops_blocked), nopos: d(sb.drops_no_position, sa.drops_no_position)
    };
    dec.sent = Math.max(0, r.tracks_s - (dec.range + dec.elev + dec.blocked + dec.nopos));
    r.decisions = { sent: dec.sent, range: dec.range, elev: dec.elev, blocked: dec.blocked, nopos: dec.nopos };
    r.cues_s = d(num(sb.sent_updates) + num(sb.sent_drops), num(sa.sent_updates) + num(sa.sent_drops));
    var old = {};
    clientList(a.clients).forEach(function (c) { old[c.key] = c.row; });
    clientList(b.clients).forEach(function (c) {
      var o = old[c.key] || {}, e = { key: c.key, kind: c.kind, endpoint: c.row.endpoint };
      if (c.kind === 'tcp') {
        e.direction = c.row.direction;
        e.per_min_bytes = d(c.row.bytes_sent, o.bytes_sent) * 60;
      } else {
        e.per_s_datagrams = d(c.row.datagrams_sent, o.datagrams_sent);
      }
      r.clients.push(e);
    });
    var oldF = {};
    (a.files || []).forEach(function (f) { oldF[f.name] = f; });
    (b.files || []).forEach(function (f) {
      r.files.push({ name: f.name, stream: f.stream, bytes: num(f.bytes), bytes_per_s: oldF[f.name] ? d(f.bytes, oldF[f.name].bytes) : 0 });
    });
    return r;
  }

  function formatBytes(n) {
    n = num(n);
    var u = ['B', 'kB', 'MB', 'GB', 'TB'], i = 0;
    while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; }
    if (i === 0) return Math.round(n) + ' B';
    var v = Math.round(n * 10) / 10;
    return (v >= 10 ? String(Math.round(v)) : String(v)) + ' ' + u[i];
  }

  // Cut text to at most maxPx at charPx per character, ending in an ellipsis.
  function flowTruncate(text, maxPx, charPx) {
    text = String(text);
    var max = Math.max(1, Math.floor(maxPx / charPx));
    return text.length <= max ? text : text.slice(0, max - 1) + '…';
  }

  function ageText(s) {
    if (typeof s !== 'number' || !isFinite(s) || s < 0) return '';
    s = Math.round(s);
    if (s < 60) return s + ' s';
    if (s < 3600) return Math.floor(s / 60) + ' min';
    if (s < 86400) return Math.floor(s / 3600) + ' h';
    return Math.floor(s / 86400) + ' d';
  }
  function rateText(v, unit) {
    var t = v >= 10 ? String(Math.round(v)).replace(/\B(?=(\d{3})+$)/g, ',') : String(Math.round(v * 10) / 10);
    return t + ' ' + unit;
  }
  function isUat(inp) { return /uat|978/i.test(String(inp.type || '') + ' ' + String(inp.id || '')); }

  function flowCsvRows(rates) {
    var out = [['source', 'target', 'rate_per_s']];
    if (!rates) return out;
    var r2 = function (v) { return Math.round(v * 100) / 100; };
    // source and target are the graph's node ids
    Object.keys(rates.inputs).forEach(function (id) { out.push([id, 'decode', r2(rates.inputs[id].msg_s)]); });
    out.push(['decode', 'rejected', r2(rates.decoder.rejected_s)], ['decode', 'tracks', r2(rates.tracks_s)]);
    ['sent', 'range', 'elev', 'blocked', 'nopos'].forEach(function (k) { out.push(['tracks', k, r2(rates.decisions[k])]); });
    out.push(['sent', 'sched', r2(rates.decisions.sent)]);
    rates.clients.forEach(function (c) { out.push(['sched', c.key, r2(rates.cues_s)]); });
    out.push(['file', 'stream', 'bytes', 'bytes_per_s']);
    rates.files.forEach(function (f) { out.push([f.name, f.stream, f.bytes, r2(f.bytes_per_s)]); });
    return out;
  }

  // The Sankey: nodes (fixed order), links, tags, height and caption.
  // Height depends on the node set only, never on the rates (a chart that
  // resized with traffic would jump): max(360, tallest column's nodes * 42),
  // 42 = nodeGap 30 + a 12 px minimum visual band. bandPx (the label's room)
  // is the band at the 360 px scale and may vary with rates.
  function flowGraph(rates, latest, opts) {
    opts = opts || {};
    var width = opts.width || 900, th = opts.theme || {};
    var tk = th.tokens || {}, dc = th.decision || {};
    var colPx = (width - 230) / 6, charPx = 6.6;
    var stats = (latest && latest.stats) || {};
    var inputs = (latest && latest.inputs) || [];
    var rt = rates || { inputs: {}, decoder: { in_s: 0, rejected_s: 0 }, tracks_s: 0, decisions: {}, cues_s: 0, clients: [], files: [], span_s: 0 };
    var newest = null;
    inputs.forEach(function (i) {
      var a = i.state && i.state.last_data_age_s;
      if (typeof a === 'number' && (newest === null || a < newest)) newest = a;
    });
    var nodes = [], links = [], byName = {};
    function addNode(name, depth, color, title, value, detail, age, unit) {
      var n = { name: name, depth: depth, color: color, value: value, _title: title, _detail: detail, _age: age, _unit: unit };
      nodes.push(n); byName[name] = n; return n;
    }
    function link(s, t, v, color) { links.push({ source: s, target: t, value: v > 0 ? v : FLOW_THIN, color: color }); }

    inputs.forEach(function (inp) {
      var o = rt.inputs[inp.id] || { msg_s: 0 };
      var age = inp.state && inp.state.last_data_age_s;
      addNode(inp.id, 0, isUat(inp) ? tk.uat : tk.accent, inp.name || inp.id, o.msg_s,
        isUat(inp) ? '978 MHz' : '1090 MHz', age, '/s').own = true;
    });
    addNode('decode', 1, tk.accent, 'decoder', rt.decoder.in_s, 'messages in', newest, '/s');
    addNode('rejected', 2, tk.muted, 'rejected', rt.decoder.rejected_s, 'CRC or parse failed', newest, '/s');
    addNode('tracks', 2, tk.accent, 'track updates', rt.tracks_s, 'after decoding', newest, '/s');
    var D = [['sent', 'sent to cues', dc.sent], ['range', 'out of range', dc.range], ['elev', 'below elevation', dc.elev],
      ['blocked', 'blocked', dc.blocked], ['nopos', 'no position', dc.unknown]];
    D.forEach(function (x) { addNode(x[0], 3, x[2], x[1], rt.decisions[x[0]] || 0, '', newest, '/s'); });
    addNode('sched', 4, tk.accent, 'cue scheduler', rt.cues_s, 'cues out', newest, '/s');
    rt.clients.forEach(function (c) {
      var detail = c.kind === 'tcp' ? (c.direction || 'tcp') + ' · ' + formatBytes(c.per_min_bytes || 0) + '/min'
        : 'udp · ' + rateText(c.per_s_datagrams || 0, 'dgrams/s');
      addNode(c.key, 5, tk.accent, c.endpoint, rt.cues_s, detail, newest, '/s');
    });

    nodes.forEach(function (n) { if (n.own) link(n.name, 'decode', n.value, n.color); });
    link('decode', 'rejected', rt.decoder.rejected_s, tk.accent);
    link('decode', 'tracks', rt.tracks_s, tk.accent);
    D.forEach(function (x) { link('tracks', x[0], rt.decisions[x[0]] || 0, x[2]); });
    link('sent', 'sched', rt.decisions.sent || 0, dc.sent);
    rt.clients.forEach(function (c) { link('sched', c.key, rt.cues_s, tk.accent); });

    // Band sizes: the node's value is what passes through it (the larger side).
    var inV = {}, outV = {};
    links.forEach(function (l) { inV[l.target] = (inV[l.target] || 0) + l.value; outV[l.source] = (outV[l.source] || 0) + l.value; });
    var cols = {};
    nodes.forEach(function (n) {
      n.band = Math.max(inV[n.name] || 0, outV[n.name] || 0, FLOW_THIN);
      (cols[n.depth] = cols[n.depth] || []).push(n);
    });
    var nMax = 0;
    Object.keys(cols).forEach(function (k) { nMax = Math.max(nMax, cols[k].length); });
    var height = Math.max(FLOW_MIN_H, nMax * 42);
    // The drawing height is capped so a few nodes keep readable bands.
    var drawH = Math.min(height - 16, 110 * nMax);
    var top = Math.round((height - drawH) / 2);
    var scale = Infinity;
    Object.keys(cols).forEach(function (k) {
      var sum = 0; cols[k].forEach(function (n) { sum += n.band; });
      scale = Math.min(scale, Math.max(0, drawH - cols[k].length * FLOW_GAP) / sum);
    });
    Object.keys(cols).forEach(function (k) {
      cols[k].forEach(function (n) { n.bandPx = Math.max(1, n.band * scale); });
    });

    // Labels: name, then the rate (or "no data <age>"), a third line on a tall node.
    nodes.forEach(function (n) {
      var lines = [n._title];
      n.idle = !(n.value > 0);
      if (n.idle && n.own) lines.push('no data' + (ageText(n._age) ? ' ' + ageText(n._age) : ''));
      else if (n.idle) lines.push('0/s');
      else lines.push(rateText(n.value, n._unit));
      if (n.bandPx >= 3 * FLOW_LINE_PX && n._detail) lines.splice(1, 0, n._detail);
      n.label = lines.slice(0, n.bandPx >= 3 * FLOW_LINE_PX ? 3 : 2)
        .map(function (l) { return flowTruncate(l, colPx, charPx); }).join('\n');
      n.tall = n.bandPx >= 3 * FLOW_LINE_PX;
    });

    // Tags: recorders and raw outputs.
    var tags = [];
    var up = null, upN = 0;
    inputs.forEach(function (i) {
      var row = byId(stats.inputs)[i.id];
      var u = (rt.inputs[i.id] && rt.inputs[i.id].uplinks_s > 0) || (row && num(row.uplinks) > 0) || (i.state && num(i.state.uplinks) > 0);
      if (u) { upN++; up = i.id; }
    });
    var perNode = {}, order = [];
    (rt.files || []).forEach(function (f) {
      var node = f.stream === 'received' ? 'tracks' : f.stream === 'sent' ? 'sched' : f.stream === 'fisb' ? (upN === 1 ? up : 'decode') : null;
      if (!node || !byName[node]) return;
      if (!perNode[node]) { perNode[node] = []; order.push(node); }
      perNode[node].push(f);
    });
    order.forEach(function (node) {
      var fl = perNode[node], bytes = 0;
      fl.forEach(function (f) { bytes += f.bytes; });
      tags.push({
        node: node, text: byName[node].tall ? '⏺ recording · ' + formatBytes(bytes) : '⏺',
        hover: fl.map(function (f) { return f.name + ': ' + formatBytes(f.bytes) + ', ' + formatBytes(f.bytes_per_s) + '/s'; }).join('\n')
      });
    });
    var outs = (latest && latest.outputs) || [];
    if (outs.outputs) outs = outs.outputs;
    var peersBy = {}, outOrder = [];
    var addPeers = function (target, p) {
      if (peersBy[target] === undefined) { peersBy[target] = 0; outOrder.push(target); }
      peersBy[target] += p;
    };
    outs.forEach(function (o) {
      if (!o || o.kind === 'cue' || o.builtin) return;
      var p = num(o.state && o.state.peers);
      if (o.input && byName[o.input]) { addPeers(o.input, p); return; }
      // GET /api/outputs names no input: a Hub output serves a kind, so it
      // taps every input of that kind's type; none matches: the decoder.
      var want = { beast: /beast_raw/, uat: /uat_raw|dump978/, json: /readsb/, pingstation_json: /pingstation_json/, cat021: /pingstation_cat021/ }[o.kind];
      var hit = 0;
      inputs.forEach(function (i) { if (want && want.test(String(i.type))) { addPeers(i.id, p); hit++; } });
      if (!hit) addPeers('decode', p);
    });
    outOrder.forEach(function (node) {
      var p = peersBy[node];
      tags.push({ node: node, text: '↗ raw output · ' + p + (p === 1 ? ' peer' : ' peers'), hover: 'raw output, ' + p + (p === 1 ? ' peer' : ' peers') });
    });

    var span = Math.round(rt.span_s || 0);
    var caption = 'messages per second, last ' + (!rt.span_s ? 0 : rt.span_s < 55 ? span : 60) + ' s · a thin band carries nothing';
    return {
      nodes: nodes.map(function (n) { return { name: n.name, label: n.label, color: n.color, depth: n.depth, bandPx: n.bandPx, tall: n.tall, idle: n.idle }; }),
      links: links, tags: tags, height: height, top: top, bottom: top, caption: caption
    };
  }

  var api = {
    RANGES: RANGES, DEFAULT_RANGE: DEFAULT_RANGE, rangeKey: rangeKey, rangeQuery: rangeQuery,
    seriesTimes: seriesTimes, sliceSeries: sliceSeries, spark: spark, DROP_FIELDS: DROP_FIELDS,
    tiles: tiles, sinceText: sinceText,
    total: total, inputSeries: inputSeries, aircraftSeries: aircraftSeries, SENT_DROPS: SENT_DROPS,
    sentDropSeries: sentDropSeries, hourOfWeek: hourOfWeek, hourOfWeekMax: hourOfWeekMax,
    uatSeries: uatSeries, uatShown: uatShown,
    AIRCRAFT_PAGE: AIRCRAFT_PAGE, AIRCRAFT_SORTS: AIRCRAFT_SORTS, AIRCRAFT_COLS: AIRCRAFT_COLS,
    aircraftQuery: aircraftQuery, aircraftCell: aircraftCell, aircraftPager: aircraftPager,
    aircraftLastPage: aircraftLastPage, detailRows: detailRows, detailPolar: detailPolar, isoTime: isoTime,
    COVERAGE_BANDS: COVERAGE_BANDS, polarSeries: polarSeries, histBars: histBars, rssiHeatmap: rssiHeatmap,
    donut: donut, topCountries: topCountries, gridView: gridView, rangeRing: rangeRing, ringRadii: ringRadii,
    categoryFilter: categoryFilter, classFilter: classFilter, countryFilter: countryFilter, keyHex: keyHex,
    csvText: csvText, LEDGER_CSV_COLS: LEDGER_CSV_COLS, LEDGER_EXPORT_PAGE: LEDGER_EXPORT_PAGE,
    ledgerCsvRows: ledgerCsvRows, ledgerExportQuery: ledgerExportQuery, SERIES_CSV_COLS: SERIES_CSV_COLS,
    seriesCsvRows: seriesCsvRows, coverageCsvRows: coverageCsvRows, csvFileName: csvFileName,
    flowWindow: flowWindow, flowPush: flowPush, flowRates: flowRates, flowGraph: flowGraph,
    flowCsvRows: flowCsvRows, flowTruncate: flowTruncate, formatBytes: formatBytes
  };
  if (root) root.skycueStatsPure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
