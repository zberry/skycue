'use strict';
// The Feeds tab's Inputs and Outputs cards: the markup the panels render
// from a fixed snapshot (tests/js/panel_sample.js), compared with
// resource_panel_golden.json, which was captured from the page's code from
// before the two panels shared a module. The panels' descriptors live in
// www/app.js between the markers `panels:begin` and `panels:end`; this
// test runs that region over the fake DOM of dom_shim.js and the real
// resource_panel.js. The cards gained one class, res-card, which is left
// out of the comparison and checked on its own.
// usage: node panel_markup.test.js [path/to/www]
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const shim = require('./dom_shim.js');
const fx = require('./panel_sample.js');
const golden = require('./resource_panel_golden.json');

const www = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'www'));
global.window = {};
global.window.skycuePure = require(path.join(www, 'app_pure.js'));
require(path.join(www, 'resource_panel.js'));
const P = global.window.skycuePure;

const src = fs.readFileSync(path.join(www, 'app.js'), 'utf8');
const begin = src.indexOf('panels:begin'), end = src.indexOf('panels:end');
assert.ok(begin > 0 && end > begin, 'app.js has no panels:begin / panels:end region');
const region = src.slice(src.indexOf('\n', begin), src.lastIndexOf('\n', end));

let failed = 0;
let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log('ok   ' + name);
  } catch (e) {
    failed++;
    console.log('FAIL ' + name + ': ' + (e && e.message));
  }
}
function plain(html) { return html.replace(/ res-card/g, ''); }

function build() {
  const lists = { 'inputs-list': shim.el('div'), 'outputs-list': shim.el('div') };
  const rx = { inputs: [], outputs: [], wall: fx.page.wall, rates: fx.page.rates, decoderLines: fx.page.decoderLines,
    dataWall: fx.page.dataWall };
  const host = {
    $: function (id) { return lists[id]; }, request: function () { return new Promise(function () {}); },
    showMsg: function () {}, eachEl: function () {}, el: shim.el, connected: function () { return true; },
    tabShown: function () { return true; }, confirm: function () { return true; }, now: function () { return fx.NOW; }
  };
  const makePanel = global.window.skycueResourcePanel.create(host);
  const names = ['P', 'rx', 'el', 'makePanel', '$', 'dataNow', 'map', 'SHOW_MIN_ZOOM', 'NUMERIC_FIELDS', 'applyInputs'];
  const hasOutputs = region.indexOf('outputsPanel') >= 0;
  const f = new Function(names.join(','),
    region + '; return {inputs: inputsPanel, outputs: ' + (hasOutputs ? 'outputsPanel' : 'null') + '};');
  const panels = f(P, rx, shim.el, makePanel, host.$, host.now, null, 6, {}, null);
  return { lists: lists, panels: panels };
}

const realNow = Date.now;
Date.now = function () { return fx.NOW; };

test('input cards render the markup the page always built', () => {
  const b = build();
  b.panels.inputs.apply(fx.inputs);
  const list = b.lists['inputs-list'];
  assert.deepStrictEqual(list.children.map(function (c) { return plain(c.html()); }), golden.inputs);
  assert.ok(list.children.every(function (c) { return c.classList.contains('res-card'); }));
  assert.strictEqual(list.className, golden.inputsListClass);
});

test('input cards keep their order, and drop the ones that went', () => {
  const b = build();
  b.panels.inputs.apply(fx.inputs);
  b.panels.inputs.apply([fx.inputs[3], fx.inputs[0]]);
  assert.deepStrictEqual(b.lists['inputs-list'].children.map(function (c) { return c.getAttribute('data-id'); }), golden.inputs2);
});

if (region.indexOf('outputsPanel') >= 0) {
  test('output cards render the markup the page always built', () => {
    const b = build();
    b.panels.outputs.apply(fx.outputs);
    const list = b.lists['outputs-list'];
    assert.deepStrictEqual(list.children.map(function (c) { return plain(c.html()); }), golden.outputs);
    assert.ok(list.children.every(function (c) { return c.classList.contains('res-card'); }));
    assert.strictEqual(list.className, golden.outputsListClass);
  });
  test('an empty outputs list is marked empty', () => {
    const b = build();
    b.panels.outputs.apply(fx.outputs);
    b.panels.outputs.apply([]);
    assert.strictEqual(b.lists['outputs-list'].className, golden.outputsListClass2);
  });
}

Date.now = realNow;
console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
