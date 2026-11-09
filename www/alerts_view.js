// skycue web UI: the alert strip (#alerts) at the bottom of the main area,
// over the map, the table and the Stats view (spec 3.1, 7, 11). It shows the
// newest unacknowledged alert of the rules engine (alerts_pure.js,
// window.skycueAlertsPure, A here) with the count, Ack, Ack all, the drawer
// of the last 200 alerts and the hide button; the header's #alerts-toggle
// shows it again and carries the unacknowledged count. Its hidden and
// drawer state live in skycue.alerts beside the rules' config, the log in
// skycue.alertLog.
//
// window.skycueAlertsView.create(host) -> {track(t), drop(d), trackGone(key),
//   beginBaseline(), endBaseline(keys), inputs(list), daemon(connected),
//   tick(), config(), setConfig(cfg), state(), setHoldOffMs(ms)}
// host = {dataNow(), storeGet(k), storeSet(k, v), request(method, path),
//         hasTrack(key), trackVisible(key), getHideFiltered(),
//         setHideFiltered(on), selectTrack(key), openReceiver(),
//         openStatsAircraft(key), tracks()}  (app.js builds it).
//
// The gear opens #alerts-config (spec 3.4): per rule enabled, sound and its
// parameters, the watch list, Mute and Clear history; every change is
// stored at once and goes to the engine with the present tracks. Sound
// (spec 3.3): the first pointerdown anywhere arms it and makes the one
// AudioContext; nothing touches Web Audio before. A raised row with sound
// on plays its tone pattern unless muted, once per engine call.
//
// Every stored or daemon string reaches the DOM through textContent.
// Plain ES2017 (functions and var, like app.js).
(function () {
  'use strict';

  var A = window.skycueAlertsPure;
  var CONFIG_KEY = 'skycue.alerts';
  var LOG_KEY = 'skycue.alertLog';
  var NOTE_MS = 20000;          // a click's note stays this long
  var NONE_TEXT = 'No unacknowledged alerts';
  var CLEAR_QUESTION = 'Clear the alert history? Acknowledged and unacknowledged alerts are removed.';
  var TONE_GAIN = 0.2;
  var TONES_KEPT = 50;          // state().tones keeps the last this many
  // The config panel's label before each rule parameter input.
  var PARAM_TEXT = { threshold_nm: 'within nm', altitude_ft: 'below ft', range_nm: 'within nm' };

  // '2026-11-04T12:34:56.789Z' -> '12:34:56Z'.
  function clockText(isoTime) { return String(isoTime).substr(11, 8) + 'Z'; }
  function detailText(row) {
    return row.detail + (row.quiet ? ' · quiet period' : '') + (row.recovered ? ' · ' + row.recovered : '');
  }

  function create(host) {
    var $ = function (id) { return document.getElementById(id); };
    var stored = A.normalizeConfig(host.storeGet(CONFIG_KEY));
    var engine = A.createEngine(stored, A.normalizeLog(host.storeGet(LOG_KEY)));
    var view = { shown: !stored.hidden, drawer: stored.drawer };
    var lineRowId = null;       // the row the strip's line shows
    var savedLog = null;        // the last skycue.alertLog written
    var logPending = false;     // a track call may have changed the log
    var noteSeq = 0, noteTimer = null;
    var audio = null;           // the AudioContext, made on the first pointerdown
    var armed = false;
    var tones = [];             // the patterns played, oldest first
    var configOpen = false;

    function now() { return host.dataNow(); }

    // ---- storage ----

    // The rules' config with the strip's own hidden and drawer state.
    function fullConfig() {
      var cfg = engine.config();
      cfg.hidden = !view.shown;
      cfg.drawer = view.drawer;
      return cfg;
    }
    function saveConfig() { host.storeSet(CONFIG_KEY, JSON.stringify(fullConfig())); }
    // Writes the log when it differs from the stored one; true when it did.
    function saveLog() {
      logPending = false;
      var text = JSON.stringify(engine.toStore());
      if (text === savedLog) return false;
      savedLog = text;
      host.storeSet(LOG_KEY, text);
      return true;
    }
    // After an engine call: a raised row or an acknowledgement always
    // redraws; any other call redraws when the log changed (a quiet row, an
    // input that recovered).
    function changed(force) {
      if (saveLog() || force) render();
    }
    // A new rules config: to the engine over the present tracks, stored.
    function applyConfig(cfg) {
      engine.setConfig(cfg, host.tracks());
      saveConfig();
      changed(false);
    }

    // ---- sound ----

    // The first pointerdown anywhere (capture, so an alert button's own
    // handler cannot stop it) arms sound; the AudioContext is made then.
    function arm() {
      document.removeEventListener('pointerdown', arm, true);
      armed = true;
      var Ctx = window.AudioContext || window.webkitAudioContext;
      try { if (Ctx) audio = new Ctx(); } catch (e) { audio = null; }
      $('alerts-sound-hint').hidden = true;
    }
    function playTone(pattern) {
      tones.push(pattern);
      if (tones.length > TONES_KEPT) tones.shift();
      if (!audio) return;
      try {
        if (audio.state === 'suspended') {
          var resumed = audio.resume();
          if (resumed && typeof resumed.catch === 'function') resumed.catch(function () {});
        }
        var t0 = audio.currentTime;
        A.tonePlan(pattern).forEach(function (beep) {
          var osc = audio.createOscillator(), gain = audio.createGain();
          osc.type = 'sine';
          osc.frequency.value = beep.freq;
          gain.gain.value = TONE_GAIN;
          osc.connect(gain);
          gain.connect(audio.destination);
          osc.start(t0 + beep.at);
          osc.stop(t0 + beep.at + beep.dur);
        });
      } catch (e) { /* no sound; the strip still shows the alert */ }
    }
    // The rows one engine call raised: one tone at most, the loudest pattern
    // of those with sound on. True when any row was raised.
    function sounded(raised) {
      var pattern = null;
      raised.forEach(function (r) {
        if (r.sound && pattern !== 'double') pattern = r.pattern;
      });
      if (pattern && armed && !engine.config().mute) playTone(pattern);
      return raised.length > 0;
    }

    // ---- the config panel ----

    function ruleRow(rule) {
      var tr = document.createElement('tr');
      tr.setAttribute('data-rule', rule.id);
      var title = document.createElement('td');
      title.className = 'rule-title';
      title.textContent = rule.title;
      tr.appendChild(title);
      ['rule-enabled', 'rule-sound'].forEach(function (cls) {
        var td = document.createElement('td');
        var box = document.createElement('input');
        box.type = 'checkbox';
        box.className = cls;
        box.setAttribute('aria-label', rule.title + (cls === 'rule-enabled' ? ' on' : ' sound'));
        td.appendChild(box);
        tr.appendChild(td);
      });
      var params = document.createElement('td');
      Object.keys(rule.params).forEach(function (p) {
        var label = document.createElement('label');
        label.className = 'rule-param';
        label.textContent = PARAM_TEXT[p] + ' ';
        var input = document.createElement('input');
        input.type = 'number';
        input.setAttribute('data-param', p);
        input.setAttribute('min', String(A.PARAM_LIMITS[p][0]));
        input.setAttribute('max', String(A.PARAM_LIMITS[p][1]));
        input.setAttribute('step', 'any');
        label.appendChild(input);
        params.appendChild(label);
      });
      tr.appendChild(params);
      return tr;
    }
    // The panel's inputs from the engine's config.
    function fillConfig() {
      var cfg = engine.config();
      Array.prototype.forEach.call($('alerts-rules').tBodies[0].rows, function (tr) {
        var r = cfg.rules[tr.getAttribute('data-rule')];
        tr.querySelector('.rule-enabled').checked = r.enabled;
        tr.querySelector('.rule-sound').checked = r.sound;
        Array.prototype.forEach.call(tr.querySelectorAll('[data-param]'), function (input) {
          input.value = String(r[input.getAttribute('data-param')]);
          input.classList.remove('invalid');
        });
      });
      $('alerts-watch').value = cfg.watch.join('\n');
      $('alerts-mute').checked = cfg.mute;
    }
    function setConfigOpen(on) {
      configOpen = on;
      if (on) fillConfig();
      $('alerts-config').hidden = !on;
      $('alerts-gear').setAttribute('aria-expanded', String(on));
      if (on) $('alerts-config').querySelector('input').focus();
      else if ($('alerts-config').contains(document.activeElement)) $('alerts-gear').focus();
    }
    // One change in the panel: fn edits a copy of the config, then it applies.
    function editConfig(fn) {
      var cfg = engine.config();
      fn(cfg);
      applyConfig(cfg);
    }
    // A parameter input: a number within PARAM_LIMITS applies, anything else
    // marks the input invalid and stores nothing.
    function paramEdited(input) {
      var p = input.getAttribute('data-param'), lim = A.PARAM_LIMITS[p];
      var text = input.value.trim();
      var v = text === '' ? NaN : Number(text);
      var ok = isFinite(v) && v >= lim[0] && v <= lim[1];
      input.classList.toggle('invalid', !ok);
      if (!ok) return;
      var id = input.closest('tr[data-rule]').getAttribute('data-rule');
      editConfig(function (cfg) { cfg.rules[id][p] = v; });
    }

    A.RULES.forEach(function (rule) { $('alerts-rules').tBodies[0].appendChild(ruleRow(rule)); });
    $('alerts-rules').addEventListener('change', function (ev) {
      var t = ev.target, tr = t.closest('tr[data-rule]');
      if (!tr || t.type !== 'checkbox') return;
      var id = tr.getAttribute('data-rule');
      var field = t.classList.contains('rule-enabled') ? 'enabled' : 'sound';
      editConfig(function (cfg) { cfg.rules[id][field] = t.checked; });
    });
    $('alerts-rules').addEventListener('input', function (ev) {
      if (ev.target.hasAttribute('data-param')) paramEdited(ev.target);
    });
    $('alerts-watch').addEventListener('change', function () {
      editConfig(function (cfg) { cfg.watch = A.normalizeWatch($('alerts-watch').value); });
      $('alerts-watch').value = engine.config().watch.join('\n');
    });
    $('alerts-mute').addEventListener('change', function () {
      editConfig(function (cfg) { cfg.mute = $('alerts-mute').checked; });
    });
    $('alerts-clear').addEventListener('click', function () {
      if (!window.confirm(CLEAR_QUESTION)) return;
      engine.clear();
      changed(true);
    });
    $('alerts-config-close').addEventListener('click', function () { setConfigOpen(false); });
    // Escape closes the panel only; the side panel's Escape does not see it.
    $('alerts-config').addEventListener('keydown', function (ev) {
      if (ev.key !== 'Escape') return;
      ev.stopPropagation();
      setConfigOpen(false);
    });
    document.addEventListener('pointerdown', arm, true);

    // ---- drawing ----

    function cell(tr, cls, text) {
      var td = document.createElement('td');
      td.className = cls;
      td.textContent = text;
      tr.appendChild(td);
      return td;
    }
    function fillLine(row) {
      var line = $('alerts-line');
      line.textContent = '';
      if (!row) { line.textContent = NONE_TEXT; return; }
      [['al-time', clockText(row.time)], ['al-kind', row.title], ['al-id', row.label], ['al-detail', row.detail]]
        .forEach(function (p) {
          var span = document.createElement('span');
          span.className = p[0];
          span.textContent = p[1];
          line.appendChild(span);
        });
    }
    function renderDrawer(rows) {
      var tbody = $('alerts-log').tBodies[0];
      tbody.textContent = '';
      rows.forEach(function (row) {
        var tr = document.createElement('tr');
        tr.setAttribute('data-alert-id', String(row.id));
        if (row.acked) tr.className = 'acked';
        cell(tr, 'al-time', clockText(row.time));
        cell(tr, 'al-kind', row.title);
        cell(tr, 'al-id', row.label);
        cell(tr, 'al-detail', detailText(row));
        var td = cell(tr, 'al-act', '');
        if (!row.acked) {
          var b = document.createElement('button');
          b.type = 'button';
          b.className = 'al-ack';
          b.textContent = 'Ack';
          td.appendChild(b);
        }
        tbody.appendChild(tr);
      });
      if (!rows.length) {
        var tr = document.createElement('tr');
        cell(tr, 'empty', 'No alerts yet').colSpan = 5;
        tbody.appendChild(tr);
      }
    }
    // The table and Stats views keep their last lines clear of the strip.
    function syncClearance() {
      var wrap = $('map-wrap');
      wrap.classList.toggle('alerts-on', view.shown);
      if (view.shown) wrap.style.setProperty('--alerts-h', $('alerts').offsetHeight + 'px');
      if (host.layoutChanged) host.layoutChanged();   // map boxes size themselves against the strip
    }
    function render() {
      var rows = engine.rows();
      var open = rows.filter(function (r) { return !r.acked; });
      var top = open.length ? open[0] : null;
      lineRowId = top ? top.id : null;
      var strip = $('alerts');
      strip.hidden = !view.shown;
      strip.classList.toggle('has-emergency', engine.hasUnackedEmergency());
      $('alerts-count').textContent = String(open.length);
      fillLine(top);
      $('alerts-line').classList.toggle('none', !top);
      $('alerts-ack').disabled = !top;
      $('alerts-ack-all').disabled = !top;
      $('alerts-expand').textContent = view.drawer ? '▼' : '▲';
      $('alerts-expand').setAttribute('aria-expanded', String(view.drawer));
      $('alerts-drawer').hidden = !view.drawer;
      if (view.drawer) renderDrawer(rows);
      var badge = $('alerts-badge');
      badge.textContent = String(open.length);
      badge.hidden = !open.length;
      $('alerts-toggle').setAttribute('aria-pressed', String(view.shown));
      syncClearance();
    }

    // ---- the note under a click ----

    // One line of text, with an "Open in Stats" button for statsKey.
    function setNote(text, statsKey) {
      clearTimeout(noteTimer);
      var note = $('alerts-note');
      note.textContent = text;
      note.hidden = !text;
      if (statsKey) {
        var b = document.createElement('button');
        b.type = 'button';
        b.id = 'alerts-open-stats';
        b.textContent = 'Open in Stats';
        b.addEventListener('click', function () { host.openStatsAircraft(statsKey); });
        note.appendChild(b);
      }
      if (text) noteTimer = setTimeout(function () { setNote('', null); }, NOTE_MS);
      syncClearance();
    }

    // ---- actions ----

    function rowById(id) {
      var found = null;
      engine.rows().forEach(function (r) { if (r.id === id) found = r; });
      return found;
    }
    // A click on an alert: an input's opens the Feeds tab; a track's
    // selects it on the map, turning Hide filtered off when that would hide
    // it, or says it is gone and offers Stats when the ledger has it.
    function activate(row) {
      var seq = ++noteSeq;
      setNote('', null);
      if (row.input !== null) { host.openReceiver(); return; }
      var key = row.key;
      if (!host.hasTrack(key)) {
        setNote(row.label + ' is no longer tracked.', null);
        var k = key.split('|');
        host.request('GET', 'api/stats/aircraft/' + encodeURIComponent(k[0]) + '?addr_type=' + encodeURIComponent(k[1] || ''))
          .then(function () {
            if (seq === noteSeq) setNote(row.label + ' is no longer tracked.', key);
          }, function () { /* not in the ledger: no button */ });
        return;
      }
      if (host.getHideFiltered() && !host.trackVisible(key)) {
        host.setHideFiltered(false);
        setNote('Hide filtered turned off to show ' + row.label + '.', null);
      }
      host.selectTrack(key);
    }
    function ack(id) { if (engine.ack(id, now())) changed(true); }
    function setShown(on) {
      view.shown = on;
      saveConfig();
      render();
    }
    function setDrawer(on) {
      view.drawer = on;
      saveConfig();
      render();
    }

    function activateLine() {
      var row = lineRowId === null ? null : rowById(lineRowId);
      if (row) activate(row);
    }
    $('alerts-line').addEventListener('click', activateLine);
    $('alerts-line').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); activateLine(); }
    });
    $('alerts-ack').addEventListener('click', function () { if (lineRowId !== null) ack(lineRowId); });
    $('alerts-ack-all').addEventListener('click', function () { if (engine.ackAll(now())) changed(true); });
    $('alerts-expand').addEventListener('click', function () { setDrawer(!view.drawer); });
    $('alerts-hide').addEventListener('click', function () { setShown(false); });
    $('alerts-toggle').addEventListener('click', function () { setShown(!view.shown); });
    $('alerts-gear').addEventListener('click', function () { setConfigOpen(!configOpen); });
    $('alerts-log').tBodies[0].addEventListener('click', function (ev) {
      var tr = ev.target.closest('tr[data-alert-id]');
      if (!tr) return;
      var id = Number(tr.getAttribute('data-alert-id'));
      if (ev.target.closest('.al-ack')) { ack(id); return; }
      var row = rowById(id);
      if (row) activate(row);
    });
    // Track updates leave a log change without a raised row (the pairs'
    // conditions, a quiet row) to the next tick; leaving the page writes it.
    window.addEventListener('pagehide', function () { if (logPending) saveLog(); });

    savedLog = host.storeGet(LOG_KEY);
    render();

    // A track call: a raised row redraws and saves now, the rest on the tick.
    function deferred(raised) {
      if (sounded(raised)) changed(true);
      else logPending = true;
    }

    return {
      track: function (t) { deferred(engine.track(t, now())); },
      drop: function (d) { deferred(engine.drop(d, now())); },
      trackGone: function (key) { engine.trackGone(key); logPending = true; },
      beginBaseline: function () { engine.beginBaseline(); },
      endBaseline: function (keys) { engine.endBaseline(keys); changed(false); },
      inputs: function (list) { changed(sounded(engine.inputs(list, now(), Date.now()))); },
      daemon: function (connected) { changed(sounded(engine.daemon(connected, now(), Date.now()))); },
      tick: function () { changed(sounded(engine.tick(now(), Date.now()))); },
      // setConfig changes the rules and the watch list, never the strip's
      // hidden and drawer state.
      config: fullConfig,
      setConfig: function (cfg) {
        applyConfig(cfg);
        if (configOpen) fillConfig();
      },
      state: function () {
        var rows = engine.rows();
        return {
          shown: view.shown, drawer: view.drawer,
          unacked: rows.filter(function (r) { return !r.acked; }).length,
          rows: rows.map(function (r) {
            return { id: r.id, rule: r.rule, key: r.key, input: r.input, label: r.label, detail: r.detail,
              acked: r.acked, quiet: r.quiet, recovered: r.recovered };
          }),
          armed: armed, tones: tones.slice(), configOpen: configOpen
        };
      },
      setHoldOffMs: function (ms) { engine.setHoldOffMs(ms); }
    };
  }

  window.skycueAlertsView = { create: create };
})();
