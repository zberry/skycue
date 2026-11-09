'use strict';
// Tests for the Feeds tab's resource panel (www/resource_panel.js), plain
// node over the fake DOM of dom_shim.js and a fake request.
// usage: node resource_panel.test.js [path/to/resource_panel.js]
const path = require('path');
const assert = require('assert');
const shim = require('./dom_shim.js');

const file = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'www', 'resource_panel.js'));
global.window = {};
global.window.skycuePure = require(path.join(path.dirname(file), 'app_pure.js'));
require(file);
const P = global.window.skycuePure;
const RP = global.window.skycueResourcePanel;

let failed = 0;
let passed = 0;
const pending = [];
function test(name, fn) {
  pending.push(Promise.resolve().then(fn).then(function () {
    passed++;
    console.log('ok   ' + name);
  }, function (e) {
    failed++;
    console.log('FAIL ' + name + ': ' + (e && e.message));
  }));
}
const tick = () => new Promise(function (r) { setImmediate(r); });

// A page in miniature: a list, a form with id, name and enabled, a title, a message.
function setup(opts) {
  opts = opts || {};
  const calls = [];
  const state = { connected: true, tab: true, confirmAnswer: true, confirms: [], shown: [], reject: null, listBody: { inputs: [] } };
  const list = shim.el('div');
  const form = shim.el('form');
  form.reset = function () { form.resets = (form.resets || 0) + 1; form.elements.id.value = ''; form.elements.name.value = ''; form.elements.enabled.checked = true; };
  form.elements = { id: shim.el('input'), name: shim.el('input'), enabled: shim.el('input') };
  form.elements.id.value = ''; form.elements.name.value = ''; form.elements.enabled.type = 'checkbox'; form.elements.enabled.checked = true;
  const labels = ['id', 'name', 'enabled'].map(function (f) { const l = shim.el('label'); l.setAttribute('data-field', f); return l; });
  const errs = ['id', 'name'].map(function (f) { const e = shim.el('span', 'field-error'); e.setAttribute('data-for', f); return e; });
  const submitBtn = shim.el('button', null, 'Add'); submitBtn.type = 'submit';
  const cancel = shim.el('button'); cancel.setAttribute('data-act', 'cancel'); cancel.hidden = true;
  const formError = shim.el('div', 'form-error');
  [submitBtn, cancel, formError].forEach(function (c) { form.appendChild(c); });
  Object.keys(form.elements).forEach(function (k) { form.appendChild(form.elements[k]); });
  const title = shim.el('h3', null, 'Add input');
  const msg = shim.el('div');
  const byId = { 'inputs-list': list, f: form, t: title, m: msg };
  const host = {
    $: function (id) { return byId[id]; },
    request: function (method, p, body) {
      calls.push({ method: method, path: p, body: body });
      if (state.reject) return Promise.reject(new Error(state.reject));
      return Promise.resolve(method === 'GET' ? state.listBody : null);
    },
    showMsg: function (el, text, isError) { el.textContent = text || ''; el.classList.toggle('error', !!isError); },
    eachEl: function (sel, fn) {
      if (sel === '#f [data-field]') labels.forEach(fn);
      else if (sel === '#f .field-error') errs.forEach(fn);
      else throw new Error('eachEl ' + sel);
    },
    el: shim.el,
    connected: function () { return state.connected; },
    tabShown: function () { return state.tab; },
    confirm: function (text) { state.confirms.push(text); return state.confirmAnswer; },
    now: function () { return 1000; }
  };
  const desc = {
    noun: 'input', listPath: 'api/inputs', formId: 'f', titleId: 't', msgId: 'm', cardClass: 'input-card',
    makeCard: function (item) {
      const c = shim.el('div', 'input-card');
      c.appendChild(shim.el('span', 'input-name'));
      return c;
    },
    fillCard: function (card, item, now) { card.querySelector('.input-name').textContent = item.name || item.id; card.now = now; },
    actions: function (item) { return item.builtin ? ['toggle'] : ['toggle', 'edit', 'remove', 'show']; },
    extraAct: function (act, item) { state.shown.push([act, item.id]); },
    fieldsFn: function () { return ['id', 'name', 'enabled']; },
    defFn: function (formEl) { return P.formDefinition(form.elements, desc.fieldsFn(formEl), {}); },
    validateFn: function (def) { return def.id ? { ok: true, errors: {} } : { ok: false, errors: { id: 'id is required' } }; },
    fillForm: function (item) { form.elements.id.value = item.id; form.elements.name.value = item.name || ''; form.elements.enabled.checked = !!item.enabled; },
    syncOn: ['name'], alwaysShown: ['id']
  };
  if (opts.desc) Object.assign(desc, opts.desc);
  const panel = RP.create(host)(desc);
  panel.wire();
  return { panel: panel, calls: calls, state: state, list: list, form: form, title: title, msg: msg, formError: formError,
    errs: errs, labels: labels, submitBtn: submitBtn, cancel: cancel, host: host };
}
const A = { id: 'a', name: 'Alpha', enabled: true };
const B = { id: 'b', enabled: false };
const C = { id: 'c', builtin: true, enabled: true };
function click(ctx, id, act) {
  const card = ctx.list.children.filter(function (c) { return c.getAttribute('data-id') === id; })[0];
  ctx.list.dispatch('click', { target: card.querySelector('[data-act=' + act + ']') });
}

test('apply creates one card per item and removes the cards of items that vanished', function () {
  const x = setup();
  x.panel.apply([A, B, C]);
  assert.deepStrictEqual(x.list.children.map(function (c) { return c.getAttribute('data-id'); }), ['a', 'b', 'c']);
  assert.ok(x.list.children.every(function (c) { return c.classList.contains('res-card') && c.classList.contains('input-card'); }));
  assert.strictEqual(x.list.children[0].now, 1000);
  assert.strictEqual(x.list.classList.contains('empty'), false);
  const keep = x.list.children[0];
  x.panel.apply([C, A]);
  assert.deepStrictEqual(x.list.children.map(function (c) { return c.getAttribute('data-id'); }), ['c', 'a']);
  assert.strictEqual(x.list.children[1], keep);
  x.panel.apply(null);
  assert.strictEqual(x.list.children.length, 0);
  assert.strictEqual(x.list.classList.contains('empty'), true);
  assert.deepStrictEqual(x.panel.items(), []);
});

test('apply renders only while the Receiver tab is shown; render draws on demand', function () {
  const x = setup();
  x.state.tab = false;
  x.panel.apply([A]);
  assert.strictEqual(x.list.children.length, 0);
  x.panel.render();
  assert.strictEqual(x.list.children.length, 1);
});

test('a card has its actions in order, a built-in only the toggle, and the toggle says Enable or Disable', function () {
  const x = setup();
  x.panel.apply([A, B, C]);
  const labels = x.list.children.map(function (c) {
    return c.querySelector('.input-actions').children.map(function (b) { return b.getAttribute('data-act') + ':' + b.textContent; }).join(',');
  });
  assert.deepStrictEqual(labels, ['toggle:Disable,edit:Edit,remove:Remove,show:Show', 'toggle:Enable,edit:Edit,remove:Remove,show:Show', 'toggle:Disable']);
});

test('refresh asks GET listPath once and applies the list', async function () {
  const x = setup();
  x.state.listBody = { inputs: [A] };
  await x.panel.refresh();
  assert.deepStrictEqual(x.calls, [{ method: 'GET', path: 'api/inputs', body: undefined }]);
  assert.strictEqual(x.list.children.length, 1);
});

test('poll refreshes only while connected, and with pollNeedsTab only while the tab is shown', async function () {
  const x = setup();
  x.state.connected = false;
  x.panel.poll();
  assert.strictEqual(x.calls.length, 0);
  x.state.connected = true; x.state.tab = false;
  x.panel.poll();
  assert.strictEqual(x.calls.length, 1);
  const y = setup({ desc: { pollNeedsTab: true } });
  y.state.tab = false;
  y.panel.poll();
  assert.strictEqual(y.calls.length, 0);
  y.state.tab = true;
  y.panel.poll();
  assert.strictEqual(y.calls.length, 1);
  await tick();
});

test('toggle posts disable for an enabled item and enable for a disabled one, then refreshes', async function () {
  const x = setup();
  x.panel.apply([A, B]);
  click(x, 'a', 'toggle');
  click(x, 'b', 'toggle');
  await tick();
  assert.deepStrictEqual(x.calls.map(function (c) { return c.method + ' ' + c.path; }),
    ['POST api/inputs/a/disable', 'POST api/inputs/b/enable', 'GET api/inputs', 'GET api/inputs']);
});

test('remove asks the question, does nothing on no, and deletes on yes', async function () {
  const x = setup();
  x.panel.apply([A]);
  x.state.confirmAnswer = false;
  click(x, 'a', 'remove');
  await tick();
  assert.deepStrictEqual(x.state.confirms, ['Remove input a?']);
  assert.strictEqual(x.calls.length, 0);
  x.state.confirmAnswer = true;
  click(x, 'a', 'remove');
  await tick();
  assert.deepStrictEqual(x.calls.map(function (c) { return c.method + ' ' + c.path; }), ['DELETE api/inputs/a', 'GET api/inputs']);
});

test('a button the panel does not know goes to extraAct', function () {
  const x = setup();
  x.panel.apply([A]);
  click(x, 'a', 'show');
  assert.deepStrictEqual(x.state.shown, [['show', 'a']]);
});

test('edit fills the form and titles it Edit <id>; reset puts Add <noun> back', function () {
  const x = setup();
  x.panel.apply([A]);
  click(x, 'a', 'edit');
  assert.strictEqual(x.title.textContent, 'Edit a');
  assert.strictEqual(x.form.elements.name.value, 'Alpha');
  assert.strictEqual(x.form.elements.id.readOnly, true);
  assert.strictEqual(x.submitBtn.textContent, 'Save');
  assert.strictEqual(x.cancel.hidden, false);
  assert.strictEqual(x.form.scrolled, true);
  x.panel.reset();
  assert.strictEqual(x.title.textContent, 'Add input');
  assert.strictEqual(x.form.elements.id.readOnly, false);
  assert.strictEqual(x.submitBtn.textContent, 'Add');
  assert.strictEqual(x.cancel.hidden, true);
});

test('removing the item being edited resets the form', async function () {
  const x = setup();
  x.panel.apply([A]);
  click(x, 'a', 'edit');
  click(x, 'a', 'remove');
  assert.strictEqual(x.title.textContent, 'Add input');
  await tick();
});

test('the fields shown follow fieldsFn, with alwaysShown kept', function () {
  const x = setup({ desc: { fieldsFn: function () { return ['name']; } } });
  assert.deepStrictEqual(x.labels.map(function (l) { return l.hidden; }), [false, false, true]);
});

test('a failing request shows its text in the message element with the error class', async function () {
  const x = setup();
  x.panel.apply([A]);
  x.state.reject = 'nope';
  click(x, 'a', 'toggle');
  await tick();
  assert.strictEqual(x.msg.textContent, 'nope');
  assert.strictEqual(x.msg.classList.contains('error'), true);
});

test('submit with an invalid definition marks the field and sends nothing', function () {
  const x = setup();
  x.form.dispatch('submit', { preventDefault: function () {} });
  assert.strictEqual(x.calls.length, 0);
  assert.strictEqual(x.errs[0].textContent, 'id is required');
  assert.strictEqual(x.form.elements.id.classList.contains('invalid'), true);
  assert.strictEqual(x.form.elements.id.getAttribute('aria-invalid'), 'true');
  x.form.elements.id.value = 'z';
  x.form.dispatch('input', { target: { name: 'id' } });
  assert.strictEqual(x.errs[0].textContent, '');
  assert.strictEqual(x.form.elements.id.getAttribute('aria-invalid'), null);
});

test('submit posts a new definition, resets the form and refreshes; editing puts to the id with Enabled as it is now', async function () {
  const x = setup();
  x.form.elements.id.value = 'z';
  x.form.dispatch('submit', { preventDefault: function () {} });
  assert.strictEqual(x.submitBtn.disabled, true);
  await tick();
  assert.deepStrictEqual(x.calls.map(function (c) { return c.method + ' ' + c.path; }), ['POST api/inputs', 'GET api/inputs']);
  assert.deepStrictEqual(x.calls[0].body, { id: 'z', enabled: true });
  assert.strictEqual(x.submitBtn.disabled, false);
  x.panel.apply([A]);
  click(x, 'a', 'edit');
  x.panel.apply([{ id: 'a', name: 'Alpha', enabled: false }]); // its card's toggle ran since Edit
  x.form.dispatch('submit', { preventDefault: function () {} });
  await tick();
  const put = x.calls.filter(function (c) { return c.method === 'PUT'; })[0];
  assert.strictEqual(put.path, 'api/inputs/a');
  assert.strictEqual(put.body.enabled, false);
});

test('a refused submit shows the server text in the form error and re-enables the button', async function () {
  const x = setup();
  x.form.elements.id.value = 'z';
  x.state.reject = 'id taken';
  x.form.dispatch('submit', { preventDefault: function () {} });
  await tick();
  assert.strictEqual(x.formError.textContent, 'id taken');
  assert.strictEqual(x.submitBtn.disabled, false);
});

Promise.all(pending).then(function () {
  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
});
