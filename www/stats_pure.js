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
  // blank), URI-encoded.
  function aircraftQuery(o) {
    o = o || {};
    var sort = AIRCRAFT_SORTS.indexOf(o.sort) >= 0 ? o.sort : 'last_seen';
    var dir = o.dir === 'asc' ? 'asc' : 'desc';
    var page = isNum(o.page) && o.page > 0 ? Math.floor(o.page) : 0;
    var q = typeof o.q === 'string' ? o.q.trim() : '';
    return 'api/stats/aircraft?sort=' + sort + '&dir=' + dir + '&limit=' + AIRCRAFT_PAGE +
      '&offset=' + page * AIRCRAFT_PAGE + (q ? '&q=' + encodeURIComponent(q) : '');
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
    categoryFilter: categoryFilter, classFilter: classFilter, countryFilter: countryFilter, keyHex: keyHex
  };
  if (root) root.skycueStatsPure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
