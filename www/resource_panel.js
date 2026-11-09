// skycue web UI: the Inputs and Outputs panels of the Feeds tab
// (docs/web-api.md, Inputs and Outputs). Both are a list of cards, one per
// resource, over an add / edit form; this module is the part they share and
// app.js hands it what differs, as a descriptor.
//
// window.skycueResourcePanel.create(host) -> makeResourcePanel(desc) ->
//   {apply(list), items(), refresh(), poll(), render(), edit(item), reset(),
//    submit(ev), syncFields(), wire()}
// host = {$, request, showMsg, eachEl, el, connected(), tabShown(name),
//         confirm(text), now()} (app.js builds it).
// desc = {noun: 'input' | 'output',           // the list is #<noun>s-list, the polled key <noun>s
//         listPath: 'api/inputs', formId, titleId, msgId,
//         cardClass: 'input-card',            // the card's own class; it also gets res-card
//         makeCard(item) -> card              // its parts, without the actions row
//         fillCard(card, item, now),          // the parts' text; the toggle label is set here
//         actions(item) -> ['toggle', 'edit', 'remove', ...],
//         extraAct(act, item),                // a button the panel does not know (Show)
//         fieldsFn(formEl) -> the form's field names for what it holds now,
//         defFn(formEl) -> definition, validateFn(def) -> {ok, errors},
//         fillForm(item, formEl),             // edit: the values into the form
//         onFormInput(name, formEl),          // optional, before the fields are synced
//         syncOn: names whose change shows or hides fields,
//         alwaysShown: [field names that stay visible beside fieldsFn's],
//         onReset(formEl),                    // optional, part of reset()
//         pollNeedsTab: poll only while the Receiver tab is shown}
// formEl(name) is the form's element of that name. Everything a resource or
// the daemon says goes in with textContent, never as markup. Plain ES2017.
(function () {
  'use strict';

  var P = window.skycuePure;
  var ACT_LABELS = { toggle: 'Disable', edit: 'Edit', remove: 'Remove', show: 'Show' };

  function create(host) {
    var $ = host.$;

    function makeResourcePanel(desc) {
      var noun = desc.noun, items = [], cards = new Map();
      var editing = null;            // the id of the resource the form edits; null: it adds one
      var enabledTouched = false;    // the form's Enabled box was changed while editing

      function formEl(name) { return $(desc.formId).elements[name]; }
      function byId(id) {
        for (var i = 0; i < items.length; i++) if (items[i].id === id) return items[i];
        return null;
      }

      function makeCard(item) {
        var card = desc.makeCard(item);
        card.classList.add('res-card');
        card.setAttribute('data-id', item.id);
        var acts = host.el('div', noun + '-actions');
        desc.actions(item).forEach(function (a) {
          var b = host.el('button', null, ACT_LABELS[a]);
          b.type = 'button';
          b.setAttribute('data-act', a);
          acts.appendChild(b);
        });
        card.appendChild(acts);
        return card;
      }
      function render() {
        var list = $(noun + 's-list'), now = host.now(), seen = {};
        items.forEach(function (item, i) {
          seen[item.id] = true;
          var card = cards.get(item.id);
          if (!card) {
            card = makeCard(item);
            cards.set(item.id, card);
          }
          if (list.children[i] !== card) list.insertBefore(card, list.children[i] || null);
          desc.fillCard(card, item, now);
          card.querySelector('[data-act=toggle]').textContent = item.enabled ? 'Disable' : 'Enable';
        });
        cards.forEach(function (card, id) {
          if (seen[id]) return;
          card.remove();
          cards.delete(id);
        });
        list.classList.toggle('empty', !items.length);
      }
      function apply(list) {
        items = Array.isArray(list) ? list : [];
        var edited = editing !== null && byId(editing);
        if (edited && !enabledTouched) formEl('enabled').checked = !!edited.enabled;
        if (host.tabShown('receiver')) render();
      }
      function refresh() {
        return host.request('GET', desc.listPath).then(function (j) { apply(j[noun + 's']); });
      }
      function poll() {
        if (host.connected() && (!desc.pollNeedsTab || host.tabShown('receiver'))) {
          P.quietly(refresh()); // the WebSocket watchdog reports a dead skycued
        }
      }

      // ---- the add / edit form ----
      function syncFields() {
        var shown = desc.fieldsFn(formEl);
        host.eachEl('#' + desc.formId + ' [data-field]', function (l) {
          var f = l.getAttribute('data-field');
          l.hidden = (desc.alwaysShown || []).indexOf(f) < 0 && shown.indexOf(f) < 0;
        });
      }
      function showFieldErrors(errors) {
        host.eachEl('#' + desc.formId + ' .field-error', function (e) {
          var f = e.getAttribute('data-for'), msg = errors[f] || '';
          e.textContent = msg;
          var input = formEl(f);
          input.classList.toggle('invalid', !!msg);
          if (msg) input.setAttribute('aria-invalid', 'true'); else input.removeAttribute('aria-invalid');
        });
      }
      function showFormError(text) { $(desc.formId).querySelector('.form-error').textContent = text || ''; }
      function reset() {
        var form = $(desc.formId);
        form.reset();
        editing = null;
        enabledTouched = false;
        formEl('id').readOnly = false;
        if (desc.onReset) desc.onReset(formEl);
        $(desc.titleId).textContent = 'Add ' + noun;
        form.querySelector('button[type=submit]').textContent = 'Add';
        form.querySelector('[data-act=cancel]').hidden = true;
        showFieldErrors({});
        showFormError('');
        syncFields();
      }
      function edit(item) {
        reset();
        editing = item.id;
        var form = $(desc.formId);
        desc.fillForm(item, formEl);
        formEl('id').readOnly = true;
        $(desc.titleId).textContent = 'Edit ' + item.id;
        form.querySelector('button[type=submit]').textContent = 'Save';
        form.querySelector('[data-act=cancel]').hidden = false;
        syncFields();
        form.scrollIntoView({ block: 'nearest' });
      }
      function submit(ev) {
        ev.preventDefault();
        var def = desc.defFn(formEl), id = editing;
        // Editing: Enabled as the resource is now (its card's toggle may have
        // changed it since Edit), unless the box itself was changed.
        var current = id !== null && byId(id);
        if (current && !enabledTouched) def.enabled = current.enabled;
        var check = desc.validateFn(def);
        showFieldErrors(check.errors);
        showFormError('');
        if (!check.ok) return;
        var button = $(desc.formId).querySelector('button[type=submit]');
        button.disabled = true;
        var req = id === null ? host.request('POST', desc.listPath, def) :
          host.request('PUT', desc.listPath + '/' + encodeURIComponent(id), def);
        req.then(function () {
          reset();
          return refresh();
        }).catch(function (err) {
          showFormError(err.message); // skycued's message, word for word
        }).then(function () { button.disabled = false; });
      }
      function onFormInput(ev) {
        var name = ev.target.name;
        if (name === 'enabled') enabledTouched = true;
        if (desc.onFormInput) desc.onFormInput(name, formEl);
        if (desc.syncOn.indexOf(name) >= 0) syncFields();
        // Once marked, the marks follow the typing.
        if ($(desc.formId).querySelector('.invalid')) showFieldErrors(desc.validateFn(desc.defFn(formEl)).errors);
      }

      function onCardClick(ev) {
        var b = ev.target.closest('button[data-act]');
        var card = b && b.closest('.' + desc.cardClass);
        var item = card && byId(card.getAttribute('data-id'));
        if (!item) return;
        var act = b.getAttribute('data-act'), path = desc.listPath + '/' + encodeURIComponent(item.id);
        var outcome = P.act(function (m, e) { host.showMsg($(desc.msgId), m, e); }, refresh);
        if (act === 'toggle') {
          host.request('POST', path + (item.enabled ? '/disable' : '/enable'), {}).then(outcome.done).catch(outcome.failed);
        } else if (act === 'edit') {
          edit(item);
        } else if (act === 'remove') {
          if (!host.confirm('Remove ' + noun + ' ' + item.id + '?')) return;
          if (editing === item.id) reset();
          host.request('DELETE', path).then(outcome.done).catch(outcome.failed);
        } else if (desc.extraAct) {
          desc.extraAct(act, item);
        }
      }

      // The listeners, and the form's fields for what it holds now.
      function wire() {
        var form = $(desc.formId);
        form.addEventListener('submit', submit);
        form.addEventListener('input', onFormInput);
        form.addEventListener('change', onFormInput);
        form.querySelector('[data-act=cancel]').addEventListener('click', reset);
        $(noun + 's-list').addEventListener('click', onCardClick);
        syncFields();
      }

      return { apply: apply, items: function () { return items; }, refresh: refresh, poll: poll, render: render,
        edit: edit, reset: reset, submit: submit, syncFields: syncFields, wire: wire };
    }
    return makeResourcePanel;
  }

  window.skycueResourcePanel = { create: create };
})();
