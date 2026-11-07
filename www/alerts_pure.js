// skycue web UI: the alert strip's pure logic (no DOM, no audio): the rules
// (spec 3.2), their edge-triggered evaluation with the one-alert-per-pair
// dedupe and 5-minute quiet period, the input hold-off, the snapshot
// baseline (spec 11) and the normalisers of the stored config
// (skycue.alerts) and log (skycue.alertLog). Shared by the page
// (window.skycueAlertsPure, loaded after app_pure.js) and the node test
// (module.exports; it loads ./app_pure.js itself). Plain ES2017.
(function (root) {
  'use strict';

  var P = root && root.skycuePure ? root.skycuePure
    : (typeof require === 'function' ? require('./app_pure.js') : null);

  // The rules in the config panel's order, with their defaults.
  var RULES = [
    { id: 'emergency', title: 'Emergency', enabled: true, sound: true, pattern: 'double', params: {} },
    { id: 'military_new', title: 'Military', enabled: true, sound: false, pattern: 'single', params: {} },
    { id: 'sent_new', title: 'Sent', enabled: true, sound: false, pattern: 'single', params: {} },
    { id: 'input_lost', title: 'Input lost', enabled: true, sound: true, pattern: 'double', params: {} },
    { id: 'range_enter', title: 'Range', enabled: false, sound: false, pattern: 'single',
      params: { threshold_nm: 25 } },
    { id: 'altitude_below', title: 'Low altitude', enabled: false, sound: false, pattern: 'single',
      params: { altitude_ft: 1000, range_nm: 10 } },
    { id: 'new_country', title: 'New country', enabled: false, sound: false, pattern: 'single', params: {} },
    { id: 'watch', title: 'Watch', enabled: false, sound: false, pattern: 'single', params: {} },
    { id: 'drop_filtered', title: 'Filtered drop', enabled: false, sound: false, pattern: 'single', params: {} }
  ];
  var RULE_BY_ID = {};
  RULES.forEach(function (r) { RULE_BY_ID[r.id] = r; });
  // The rules a track or a drop feeds (every rule but input_lost).
  var TRACK_RULES = RULES.map(function (r) { return r.id; })
    .filter(function (id) { return id !== 'input_lost'; });

  var PARAM_LIMITS = { threshold_nm: [0.1, 500], altitude_ft: [0, 60000], range_nm: [0.1, 500] };
  var QUIET_MS = 300000;
  var HOLD_OFF_MS = 10000;
  var LOG_MAX = 200;
  var PAIRS_MAX = 1000;
  var WATCH_MAX = 200;
  var WATCH_LEN = 16;
  var DAEMON_ID = 'skycued';

  function own(obj, k) { return Object.prototype.hasOwnProperty.call(obj, k); }
  function isObject(v) { return !!v && typeof v === 'object' && !Array.isArray(v); }
  // A JSON string parsed (null when it does not parse), anything else as is.
  function parsed(v) {
    if (typeof v !== 'string') return v;
    try { return JSON.parse(v); } catch (e) { return null; }
  }
  // A boolean or a number as a boolean (like normalizeView), else the default.
  function boolOr(v, dflt) { return typeof v === 'boolean' || typeof v === 'number' ? !!v : dflt; }
  function finiteOrNull(v) { return typeof v === 'number' && isFinite(v) ? v : null; }
  function isWhole(v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v; }
  function iso(ms) { return new Date(ms).toISOString(); }
  // An ISO time that parses, re-printed; else null.
  function isoOrNull(v) {
    if (typeof v !== 'string') return null;
    var ms = Date.parse(v);
    return isNaN(ms) ? null : iso(ms);
  }
  function copy(v) { return JSON.parse(JSON.stringify(v)); }

  // ---- config (skycue.alerts) ----

  function ruleDefaults(r) {
    var out = { enabled: r.enabled, sound: r.sound };
    Object.keys(r.params).forEach(function (p) { out[p] = r.params[p]; });
    return out;
  }
  function defaultConfig() {
    var rules = {};
    RULES.forEach(function (r) { rules[r.id] = ruleDefaults(r); });
    return { version: 1, mute: false, hidden: false, drawer: false, rules: rules, watch: [] };
  }
  function paramOr(name, v, dflt) {
    var lim = PARAM_LIMITS[name];
    return typeof v === 'number' && isFinite(v) && v >= lim[0] && v <= lim[1] ? v : dflt;
  }
  // Any input (an object, a JSON string, garbage) to a full config; each
  // field falls back to its default on its own, unknown rule ids are dropped.
  function normalizeConfig(obj) {
    obj = parsed(obj);
    if (!isObject(obj)) obj = {};
    var inRules = isObject(obj.rules) ? obj.rules : {};
    var rules = {};
    RULES.forEach(function (r) {
      var src = isObject(inRules[r.id]) ? inRules[r.id] : {};
      var out = { enabled: boolOr(src.enabled, r.enabled), sound: boolOr(src.sound, r.sound) };
      Object.keys(r.params).forEach(function (p) { out[p] = paramOr(p, src[p], r.params[p]); });
      rules[r.id] = out;
    });
    return {
      version: 1, mute: boolOr(obj.mute, false), hidden: boolOr(obj.hidden, false),
      drawer: boolOr(obj.drawer, false), rules: rules, watch: normalizeWatch(obj.watch)
    };
  }

  // The watch list from the textarea (one entry per line) or a stored array:
  // trimmed, empties and non-strings dropped, each cut to WATCH_LEN,
  // case-insensitive duplicates dropped (first kept), at most WATCH_MAX.
  function normalizeWatch(v) {
    var items = typeof v === 'string' ? v.split(/\r?\n/) : (Array.isArray(v) ? v : []);
    var out = [], seen = {};
    for (var i = 0; i < items.length && out.length < WATCH_MAX; i++) {
      if (typeof items[i] !== 'string') continue;
      var s = items[i].trim().slice(0, WATCH_LEN).trim();
      var low = s.toLowerCase();
      if (!s || own(seen, low)) continue;
      seen[low] = true;
      out.push(s);
    }
    return out;
  }

  // ---- settings file (spec 4.4, 11 "Import merge") ----
  // {skycue_settings: 2, sets: [...], alerts: {mute, rules, watch}}: the
  // filter sets and the alert config, never the strip's hidden and drawer
  // state. Import validates it with the stores' own normalisers.

  var SETTINGS_VERSION = 2;
  var SETTINGS_MAX_BYTES = 1048576;

  // The alert config as the file carries it.
  function exportConfig(config) {
    var c = normalizeConfig(config);
    return { mute: c.mute, rules: c.rules, watch: c.watch };
  }
  function settingsFile(viewSets, config) {
    return { skycue_settings: SETTINGS_VERSION, sets: P.normalizeViewSets(viewSets).sets, alerts: exportConfig(config) };
  }
  function settingsFileName(nowMs) { return 'skycue-settings-' + P.utcDate(nowMs) + '.json'; }

  // A file's text to {ok: true, sets, alerts, alertRules} (alerts null when
  // the file has none; alertRules the known rule ids it names) or
  // {ok: false, error}.
  function parseSettings(text) {
    var obj;
    try { obj = JSON.parse(text); } catch (e) { return { ok: false, error: 'not JSON' }; }
    if (!isObject(obj) || typeof obj.skycue_settings !== 'number') return { ok: false, error: 'not a skycue settings file' };
    if (obj.skycue_settings !== 1 && obj.skycue_settings !== SETTINGS_VERSION) return { ok: false, error: 'unsupported version ' + obj.skycue_settings };
    var inAlerts = isObject(obj.alerts) ? obj.alerts : null;
    var inRules = inAlerts && isObject(inAlerts.rules) ? inAlerts.rules : {};
    return {
      ok: true,
      sets: P.normalizeViewSets({ version: P.VIEW_SETS_VERSION, sets: obj.sets }).sets,
      alerts: inAlerts ? normalizeConfig(inAlerts) : null,
      alertRules: RULES.map(function (r) { return r.id; }).filter(function (id) { return isObject(inRules[id]); })
    };
  }

  // The stores after an import. current = {sets, alerts} (alerts the full
  // config with hidden and drawer), incoming = a parseSettings result.
  // 'replace' takes the file's sets and alert config; 'merge' keeps the
  // current sets in order, replaces same-named ones in place, appends new
  // names up to the cap, takes the file's rules rule by rule and the union of
  // the watch lists. Either way the strip's hidden and drawer state stays
  // (and, merging, the mute). Neither argument is changed.
  function mergeSettings(current, incoming, mode) {
    var cur = normalizeConfig(current.alerts);
    var inc = incoming.alerts ? copy(incoming.alerts) : null;
    var replacedNames = [], droppedNames = [];
    var sets, alerts;
    if (mode === 'replace') {
      sets = copy(incoming.sets);
      alerts = inc || cur;
    } else {
      sets = copy(current.sets);
      var names = sets.map(function (s) { return s.name; });
      incoming.sets.forEach(function (s) {
        var i = names.indexOf(s.name);
        if (i >= 0) {
          sets[i] = copy(s);
          replacedNames.push(s.name);
        } else if (sets.length < P.VIEW_SETS_MAX) {
          sets.push(copy(s));
          names.push(s.name);
        } else droppedNames.push(s.name);
      });
      alerts = cur;
      if (inc) {
        incoming.alertRules.forEach(function (id) { if (own(inc.rules, id)) alerts.rules[id] = inc.rules[id]; });
        alerts.watch = normalizeWatch(cur.watch.concat(inc.watch));
      }
    }
    alerts.hidden = cur.hidden;
    alerts.drawer = cur.drawer;
    return {
      sets: P.normalizeViewSets({ version: P.VIEW_SETS_VERSION, sets: sets }).sets, alerts: normalizeConfig(alerts),
      replacedNames: replacedNames, droppedNames: droppedNames
    };
  }

  // The first watch entry that is a case-insensitive prefix of the track's
  // trimmed hex, callsign or registration; null when none is.
  function watchMatch(t, watch) {
    var fields = [t.hex, t.callsign, t.registration].filter(function (f) { return typeof f === 'string'; })
      .map(function (f) { return f.trim().toLowerCase(); });
    for (var i = 0; i < (watch || []).length; i++) {
      var w = String(watch[i]).toLowerCase();
      if (!w) continue;
      for (var j = 0; j < fields.length; j++) if (fields[j].indexOf(w) === 0) return watch[i];
    }
    return null;
  }

  // An outage length: '12 s', '1 min 12 s', '1 h 2 min' (whole units).
  function durationText(ms) {
    var s = Math.floor(Math.max(0, ms) / 1000);
    if (s < 60) return s + ' s';
    var m = Math.floor(s / 60);
    if (m < 60) return m + ' min ' + (s % 60) + ' s';
    return Math.floor(m / 60) + ' h ' + (m % 60) + ' min';
  }

  // The beeps of a pattern for the Web Audio player (seconds, Hz).
  function tonePlan(pattern) {
    var plan = [{ at: 0, dur: 0.15, freq: 880 }];
    if (pattern === 'double') plan.push({ at: 0.25, dur: 0.15, freq: 880 });
    return plan;
  }

  // ---- log (skycue.alertLog) ----

  function emptyLog() { return { version: 1, nextId: 1, rows: [], pairs: {} }; }
  function pairRule(pk) { return pk.split('|')[0]; }
  function pairKey(pk) { return pk.slice(pk.indexOf('|') + 1); }

  function normalizeRow(r) {
    if (!isObject(r) || !isWhole(r.id) || typeof r.rule !== 'string' || !own(RULE_BY_ID, r.rule)) return null;
    var time = isoOrNull(r.time);
    if (time === null) return null;
    var def = RULE_BY_ID[r.rule];
    var str = function (v) { return typeof v === 'string' ? v : ''; };
    return {
      id: r.id, time: time, rule: r.rule, title: typeof r.title === 'string' ? r.title : def.title,
      label: str(r.label), detail: str(r.detail), key: str(r.key),
      input: typeof r.input === 'string' ? r.input : null, acked: r.acked === true, ackTime: isoOrNull(r.ackTime),
      quiet: r.quiet === true, recovered: typeof r.recovered === 'string' ? r.recovered : null,
      pattern: r.pattern === 'single' || r.pattern === 'double' ? r.pattern : def.pattern,
      sound: typeof r.sound === 'boolean' ? r.sound : def.sound
    };
  }

  // A pairs table cut to PAIRS_MAX in place: open pairs first, then the most
  // recently fired or acknowledged, then by key.
  function trimPairs(pairs) {
    var pks = Object.keys(pairs);
    if (pks.length <= PAIRS_MAX) return;
    var recent = function (p) { return Math.max(p.fired === null ? -Infinity : p.fired, p.ack === null ? -Infinity : p.ack); };
    pks.sort(function (a, b) {
      var pa = pairs[a], pb = pairs[b];
      if ((pa.open === null) !== (pb.open === null)) return pa.open === null ? 1 : -1;
      return recent(pb) - recent(pa) || (a < b ? -1 : 1);
    });
    pks.slice(PAIRS_MAX).forEach(function (pk) { delete pairs[pk]; });
  }

  // Any input (an object, a JSON string, garbage) to a log: rows without a
  // whole-number id, a known rule or a parsable time dropped, ids unique,
  // oldest first, the newest LOG_MAX kept; pairs of known rules, at most
  // PAIRS_MAX (open ones first, then the most recent), each `open` pointing
  // at an unacknowledged row; nextId past every row id.
  function normalizeLog(obj) {
    obj = parsed(obj);
    if (!isObject(obj)) return emptyLog();
    var rows = [], ids = {};
    (Array.isArray(obj.rows) ? obj.rows : []).forEach(function (r) {
      var row = normalizeRow(r);
      if (row && !own(ids, row.id)) { ids[row.id] = true; rows.push(row); }
    });
    rows.sort(function (a, b) { return a.id - b.id; });
    if (rows.length > LOG_MAX) rows = rows.slice(rows.length - LOG_MAX);
    var openIds = {};
    rows.forEach(function (r) { if (!r.acked) openIds[r.id] = r.rule + '|' + r.key; });

    var pairs = {};
    var inPairs = isObject(obj.pairs) ? obj.pairs : {};
    Object.keys(inPairs).forEach(function (pk) {
      var p = inPairs[pk];
      if (!isObject(p) || pk.indexOf('|') < 0 || !own(RULE_BY_ID, pairRule(pk))) return;
      pairs[pk] = { active: p.active === true, fired: finiteOrNull(p.fired), ack: finiteOrNull(p.ack),
        open: isWhole(p.open) && openIds[p.open] === pk ? p.open : null };
    });
    // An unacknowledged row keeps its pair open (newest row wins).
    rows.forEach(function (r) {
      if (r.acked) return;
      var pk = r.rule + '|' + r.key;
      if (!own(pairs, pk)) pairs[pk] = { active: false, fired: Date.parse(r.time), ack: null, open: null };
      pairs[pk].open = r.id;
    });
    trimPairs(pairs);
    var nextId = isWhole(obj.nextId) && obj.nextId > 0 ? obj.nextId : 1;
    rows.forEach(function (r) { if (r.id >= nextId) nextId = r.id + 1; });
    return { version: 1, nextId: nextId, rows: rows, pairs: pairs };
  }

  // ---- labels and details ----

  function trimmed(v) { return typeof v === 'string' ? v.trim() : ''; }
  function trackLabel(t) { return trimmed(t.callsign) || trimmed(t.registration) || String(t.hex); }
  function spaced(v) { return String(v === undefined || v === null ? '' : v).replace(/_/g, ' '); }
  function emergencyDetail(t) {
    var sq = t.squawk;
    if (sq === '7500' || sq === '7600' || sq === '7700') return 'squawk ' + sq;
    return 'emergency ' + spaced(t.emergency);
  }
  function numOrNull(v) { return typeof v === 'number' && isFinite(v) ? v : null; }
  function inRange(t, rules) {
    var r = numOrNull(t.range_nm);
    return r !== null && r < rules.range_enter.threshold_nm;
  }
  function isLow(t, rules) {
    var p = rules.altitude_below, alt = P.trackAlt(t), r = numOrNull(t.range_nm);
    return !t.on_ground && alt !== null && alt < p.altitude_ft && r !== null && r <= p.range_nm;
  }

  // ---- the engine ----

  // The rules engine over one page session (spec 3.5's evaluate, split by
  // source). Every method takes the data-clock nowMs (row times, quiet
  // period); the input methods also the wall clock wallMs (hold-off,
  // outage length). A method returns the rows it raised (copies), never
  // the quiet ones it logged.
  function createEngine(config, log) {
    var cfg = normalizeConfig(config);
    var lg = normalizeLog(log);
    var seen = { military: {}, sent: {}, country: {} };
    var inputState = {};
    var daemonState = { downSince: null, fired: false };
    var seenConnected = {};
    var baseline = false, seedInputs = false;
    var holdOffMs = HOLD_OFF_MS;
    var lastNow = 0;

    function clock(nowMs) { if (typeof nowMs === 'number' && isFinite(nowMs)) lastNow = nowMs; }
    function enabled(ruleId) { return cfg.rules[ruleId].enabled; }
    function pairOf(pk) {
      if (!own(lg.pairs, pk)) lg.pairs[pk] = { active: false, fired: null, ack: null, open: null };
      return lg.pairs[pk];
    }
    function findRow(id) {
      for (var i = 0; i < lg.rows.length; i++) if (lg.rows[i].id === id) return lg.rows[i];
      return null;
    }
    function ackRow(row, nowMs, recovered) {
      row.acked = true;
      row.ackTime = iso(nowMs);
      if (recovered !== undefined) row.recovered = recovered;
      var p = lg.pairs[row.rule + '|' + row.key];
      if (p && p.open === row.id) { p.open = null; p.ack = nowMs; }
    }
    // A pair that is inactive, not open and past its quiet period carries
    // nothing an absent pair does not: dropped to keep the table small.
    function prune(pk) {
      var p = lg.pairs[pk];
      if (p && !p.active && p.open === null && (p.ack === null || lastNow - p.ack >= QUIET_MS)) delete lg.pairs[pk];
    }
    function setActive(pk, active) {
      if (active) { pairOf(pk).active = true; return; }
      if (own(lg.pairs, pk)) { lg.pairs[pk].active = false; prune(pk); }
    }
    // Past LOG_MAX the oldest acknowledged row goes first, then the oldest.
    function capLog() {
      while (lg.rows.length > LOG_MAX) {
        var at = 0;
        for (var i = 0; i < lg.rows.length; i++) if (lg.rows[i].acked) { at = i; break; }
        var gone = lg.rows.splice(at, 1)[0];
        var p = lg.pairs[gone.rule + '|' + gone.key];
        if (p && p.open === gone.id) p.open = null;
      }
    }
    // Fire a pair: nothing while it is open; a quiet (already acknowledged)
    // row inside the quiet period after its last acknowledgement; else a new
    // open row, pushed to `out`.
    function fire(ruleId, key, nowMs, label, detail, input, out) {
      var pk = ruleId + '|' + key, p = pairOf(pk);
      if (p.open !== null) return;
      var def = RULE_BY_ID[ruleId];
      var row = {
        id: lg.nextId++, time: iso(nowMs), rule: ruleId, title: def.title, label: label, detail: detail,
        key: key, input: input, acked: false, ackTime: null, quiet: false, recovered: null,
        pattern: def.pattern, sound: !!cfg.rules[ruleId].sound
      };
      p.fired = nowMs;
      if (p.ack !== null && nowMs - p.ack < QUIET_MS) {
        row.quiet = true;
        row.acked = true;
        row.ackTime = row.time;
        lg.rows.push(row);
      } else {
        lg.rows.push(row);
        p.open = row.id;
        out.push(copy(row));
      }
      capLog();
    }
    // A state rule's condition for a pair: fires on false -> true when the
    // rule is enabled and not seeding.
    function state(ruleId, key, cond, silent, nowMs, label, detailFn, out) {
      var pk = ruleId + '|' + key;
      var was = own(lg.pairs, pk) && lg.pairs[pk].active;
      if (cond && !was) {
        setActive(pk, true);
        if (enabled(ruleId) && !silent) fire(ruleId, key, nowMs, label, detailFn(), null, out);
      } else if (!cond && was) {
        setActive(pk, false);
      }
    }
    // A seen-set rule: fires the first time `id` meets its condition.
    function firstSeen(ruleId, set, id, key, nowMs, label, detail, out) {
      if (own(set, id)) return;
      set[id] = true;
      if (enabled(ruleId) && !baseline) fire(ruleId, key, nowMs, label, detail, null, out);
    }

    function track(t, nowMs) {
      clock(nowMs);
      var out = [], key = P.trackKey(t), label = trackLabel(t), rules = cfg.rules;
      state('emergency', key, P.isEmergency(t), false, nowMs, label, function () { return emergencyDetail(t); }, out);
      if (P.trackClass(t) === 'military') firstSeen('military_new', seen.military, key, key, nowMs, label, 'first seen military', out);
      if (t.decision === 'send' || t.decision === 'allow_listed') {
        firstSeen('sent_new', seen.sent, key, key, nowMs, label,
          t.decision === 'send' ? 'sent to cue clients' : 'allow listed, sent', out);
      }
      state('range_enter', key, inRange(t, rules), baseline, nowMs, label, function () {
        return 'inside ' + rules.range_enter.threshold_nm + ' nm (' + t.range_nm.toFixed(1) + ' nm)';
      }, out);
      state('altitude_below', key, isLow(t, rules), baseline, nowMs, label, function () {
        var p = rules.altitude_below;
        return 'below ' + P.thousands(p.altitude_ft) + ' ft within ' + p.range_nm + ' nm (' +
          P.thousands(P.trackAlt(t)) + ' ft, ' + t.range_nm.toFixed(1) + ' nm)';
      }, out);
      var country = P.trackCountry(t);
      if (country) firstSeen('new_country', seen.country, country, key, nowMs, label, 'first aircraft from ' + country, out);
      var match = watchMatch(t, cfg.watch);
      state('watch', key, match !== null, false, nowMs, label, function () {
        return 'matches watch entry "' + match + '"';
      }, out);
      return out;
    }

    // A drop event: a filter decision's drop is a transition of its pair.
    function drop(d, nowMs) {
      clock(nowMs);
      var out = [];
      if (d.reason === 'filtered' && enabled('drop_filtered') && !baseline) {
        fire('drop_filtered', P.trackKey(d), nowMs, trimmed(d.callsign) || String(d.hex),
          'dropped: ' + spaced(d.decision), null, out);
      }
      return out;
    }

    function trackGone(key) {
      TRACK_RULES.forEach(function (id) { setActive(id + '|' + key, false); });
    }

    function beginBaseline() { baseline = true; seedInputs = true; }
    function endBaseline(presentKeys) {
      var present = {};
      (presentKeys || []).forEach(function (k) { present[k] = true; });
      Object.keys(lg.pairs).forEach(function (pk) {
        if (pairRule(pk) !== 'input_lost' && !own(present, pairKey(pk))) setActive(pk, false);
      });
      baseline = false;
      seedInputs = false;
    }

    // ---- inputs and the daemon ----

    function inputPair(id) { return 'input_lost|input:' + id; }
    // The outage ends: an open alert is acknowledged with `reason` (by
    // default the outage length), a pending hold-off is cancelled silently.
    function endOutage(id, s, nowMs, wallMs, reason) {
      if (s.fired) {
        var p = lg.pairs[inputPair(id)];
        var row = p && p.open !== null ? findRow(p.open) : null;
        if (row) ackRow(row, nowMs, reason || 'recovered after ' + durationText(wallMs - s.downSince));
      }
      s.downSince = null;
      s.fired = false;
    }
    // The first time an input (or the daemon) is seen connected, an alert
    // still open from the stored log (an outage from before a reload, its
    // length unknown) is acknowledged as recovered.
    function firstConnected(id, nowMs) {
      if (own(seenConnected, id)) return;
      seenConnected[id] = true;
      var p = lg.pairs[inputPair(id)];
      var row = p && p.open !== null ? findRow(p.open) : null;
      if (row) ackRow(row, nowMs, 'recovered');
    }
    function holdOffOver(s, wallMs) {
      return s.downSince !== null && !s.fired && wallMs - s.downSince >= holdOffMs;
    }
    function checkInput(id, s, nowMs, wallMs, out) {
      if (s.connected || !holdOffOver(s, wallMs)) return;
      s.fired = true;
      if (enabled('input_lost')) fire('input_lost', 'input:' + id, nowMs, s.name, 'disconnected', id, out);
    }
    function checkDaemon(nowMs, wallMs, out) {
      if (!holdOffOver(daemonState, wallMs)) return;
      daemonState.fired = true;
      if (enabled('input_lost')) {
        fire('input_lost', 'input:' + DAEMON_ID, nowMs, DAEMON_ID, 'connection lost', DAEMON_ID, out);
      }
    }

    // The inputs status list ({id, name, enabled, state: {connected}}).
    // Only enabled inputs count; a first sighting records the state
    // silently (an input already down starts no hold-off), in a seed call
    // or not. A known input last seen connected and now down starts its
    // hold-off, in a seed call too, and a seed never cancels a pending one;
    // a seed call only defers the firing to the next call or tick.
    function inputs(list, nowMs, wallMs) {
      clock(nowMs);
      var out = [], present = {}, disabled = {}, order = [];
      (Array.isArray(list) ? list : []).forEach(function (it) {
        if (!isObject(it) || typeof it.id !== 'string') return;
        if (it.enabled === false) { disabled[it.id] = true; return; }
        if (!own(present, it.id)) order.push(it.id);
        present[it.id] = it;
      });
      Object.keys(inputState).forEach(function (id) {
        if (own(present, id)) return;
        endOutage(id, inputState[id], nowMs, wallMs, own(disabled, id) ? 'input disabled' : 'input removed');
        delete inputState[id];
      });
      order.forEach(function (id) {
        var it = present[id];
        var connected = !!(isObject(it.state) && it.state.connected);
        var name = typeof it.name === 'string' && it.name ? it.name : id;
        var s = inputState[id];
        if (connected) firstConnected(id, nowMs);
        if (!s) {
          inputState[id] = { connected: connected, downSince: null, fired: false, name: name };
          return;
        }
        s.name = name;
        if (connected) {
          if (!s.connected) endOutage(id, s, nowMs, wallMs, null);
        } else if (s.connected) {
          s.downSince = wallMs;
        }
        s.connected = connected;
        if (!seedInputs) checkInput(id, s, nowMs, wallMs, out);
      });
      return out;
    }

    // The page's own connection to skycued (the banner).
    function daemon(connected, nowMs, wallMs) {
      clock(nowMs);
      var out = [];
      if (connected) {
        if (daemonState.downSince !== null) endOutage(DAEMON_ID, daemonState, nowMs, wallMs, null);
        firstConnected(DAEMON_ID, nowMs);
      } else {
        if (daemonState.downSince === null) daemonState.downSince = wallMs;
        checkDaemon(nowMs, wallMs, out);
      }
      return out;
    }

    // The hold-off timers, run once a second by the page. Also sweeps the
    // pairs table: a pair acknowledged shortly before its track left, or
    // whose open row the log cap evicted, is never revisited by setActive,
    // so it is pruned here once quiet, and the table is held to PAIRS_MAX.
    function tick(nowMs, wallMs) {
      clock(nowMs);
      var out = [];
      Object.keys(inputState).forEach(function (id) { checkInput(id, inputState[id], nowMs, wallMs, out); });
      checkDaemon(nowMs, wallMs, out);
      Object.keys(lg.pairs).forEach(prune);
      trimPairs(lg.pairs);
      return out;
    }

    // ---- acknowledgement and views ----

    function ack(id, nowMs) {
      clock(nowMs);
      var row = findRow(id);
      if (!row || row.acked) return false;
      ackRow(row, nowMs);
      return true;
    }
    function ackAll(nowMs) {
      clock(nowMs);
      var n = 0;
      lg.rows.forEach(function (row) { if (!row.acked) { ackRow(row, nowMs); n++; } });
      return n;
    }
    function rows() { return copy(lg.rows).reverse(); }
    function unacked() { return rows().filter(function (r) { return !r.acked; }); }
    function hasUnackedEmergency() {
      return lg.rows.some(function (r) { return !r.acked && r.rule === 'emergency'; });
    }

    // Re-seed one state rule's `active` over the present tracks, silently.
    function reseed(ruleId, cond, presentTracks) {
      var on = {};
      presentTracks.forEach(function (t) { if (cond(t)) on[P.trackKey(t)] = true; });
      Object.keys(lg.pairs).forEach(function (pk) {
        if (pairRule(pk) === ruleId && !own(on, pairKey(pk))) setActive(pk, false);
      });
      Object.keys(on).forEach(function (key) { setActive(ruleId + '|' + key, true); });
    }
    // A new config. Enabling a rule never fires for the current state (its
    // conditions were tracked all along); a changed parameter or watch list
    // re-seeds the rule's pairs over presentTracks without firing.
    function setConfig(config, presentTracks) {
      var next = normalizeConfig(config), prev = cfg;
      var tracks = Array.isArray(presentTracks) ? presentTracks : [];
      cfg = next;
      if (prev.rules.range_enter.threshold_nm !== next.rules.range_enter.threshold_nm) {
        reseed('range_enter', function (t) { return inRange(t, next.rules); }, tracks);
      }
      if (prev.rules.altitude_below.altitude_ft !== next.rules.altitude_below.altitude_ft ||
          prev.rules.altitude_below.range_nm !== next.rules.altitude_below.range_nm) {
        reseed('altitude_below', function (t) { return isLow(t, next.rules); }, tracks);
      }
      if (JSON.stringify(prev.watch) !== JSON.stringify(next.watch)) {
        reseed('watch', function (t) { return watchMatch(t, next.watch) !== null; }, tracks);
      }
    }

    // "Clear history": the rows and the dedupe bookkeeping go; the pairs'
    // conditions stay, so a condition that is still true does not re-fire.
    // Row ids keep counting up.
    function clear() {
      lg.rows = [];
      Object.keys(lg.pairs).forEach(function (pk) {
        var p = lg.pairs[pk];
        p.fired = null;
        p.ack = null;
        p.open = null;
        if (!p.active) delete lg.pairs[pk];
      });
    }

    return {
      track: track, drop: drop, trackGone: trackGone, beginBaseline: beginBaseline, endBaseline: endBaseline,
      inputs: inputs, daemon: daemon, tick: tick, ack: ack, ackAll: ackAll,
      rows: rows, unacked: unacked, hasUnackedEmergency: hasUnackedEmergency,
      config: function () { return copy(cfg); }, setConfig: setConfig, clear: clear,
      toStore: function () { return copy(lg); },
      setHoldOffMs: function (ms) { if (typeof ms === 'number' && isFinite(ms) && ms >= 0) holdOffMs = ms; }
    };
  }

  var api = {
    RULES: RULES, PARAM_LIMITS: PARAM_LIMITS, QUIET_MS: QUIET_MS, HOLD_OFF_MS: HOLD_OFF_MS, LOG_MAX: LOG_MAX,
    PAIRS_MAX: PAIRS_MAX, WATCH_MAX: WATCH_MAX, WATCH_LEN: WATCH_LEN, DAEMON_ID: DAEMON_ID,
    defaultConfig: defaultConfig, normalizeConfig: normalizeConfig, normalizeWatch: normalizeWatch,
    SETTINGS_VERSION: SETTINGS_VERSION, SETTINGS_MAX_BYTES: SETTINGS_MAX_BYTES, exportConfig: exportConfig,
    settingsFile: settingsFile, settingsFileName: settingsFileName, parseSettings: parseSettings,
    mergeSettings: mergeSettings,
    watchMatch: watchMatch, durationText: durationText, tonePlan: tonePlan, normalizeLog: normalizeLog,
    createEngine: createEngine
  };
  if (root) root.skycueAlertsPure = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : this);
