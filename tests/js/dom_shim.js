'use strict';
// A minimal fake DOM for the node tests of the page's modules: elements
// with children, attributes, classList, textContent, querySelector over the
// few selector shapes the page uses, closest, and html() to serialize one.
// Not a browser; just enough to run www/resource_panel.js and compare the
// markup it builds.

function El(tag) {
  this.tagName = tag.toUpperCase();
  this.children = [];
  this.parentNode = null;
  this.attrs = {};
  this.listeners = {};
  this.own = '';
  var self = this;
  this.classList = {
    contains: function (c) { return self.classes().indexOf(c) >= 0; },
    add: function (c) { if (!this.contains(c)) self.attrs['class'] = self.classes().concat([c]).join(' '); },
    remove: function (c) { self.attrs['class'] = self.classes().filter(function (x) { return x !== c; }).join(' '); },
    toggle: function (c, force) {
      var on = force === undefined ? !this.contains(c) : !!force;
      if (on) this.add(c); else this.remove(c);
      return on;
    }
  };
}
El.prototype.classes = function () { return (this.attrs['class'] || '').split(/\s+/).filter(Boolean); };
function reflect(name, attr, bool) {
  Object.defineProperty(El.prototype, name, {
    get: function () { return bool ? attr in this.attrs : (this.attrs[attr] || ''); },
    set: function (v) {
      if (bool) { if (v) this.attrs[attr] = ''; else delete this.attrs[attr]; } else this.attrs[attr] = String(v);
    }
  });
}
reflect('className', 'class');
reflect('title', 'title');
reflect('type', 'type');
reflect('hidden', 'hidden', true);
reflect('disabled', 'disabled', true);
reflect('readOnly', 'readonly', true);
Object.defineProperty(El.prototype, 'textContent', {
  get: function () {
    return this.own + this.children.map(function (c) { return c.textContent; }).join('');
  },
  set: function (v) {
    this.children.forEach(function (c) { c.parentNode = null; });
    this.children = [];
    this.own = String(v);
  }
});
El.prototype.setAttribute = function (k, v) { this.attrs[k] = String(v); };
El.prototype.getAttribute = function (k) { return k in this.attrs ? this.attrs[k] : null; };
El.prototype.removeAttribute = function (k) { delete this.attrs[k]; };
El.prototype.appendChild = function (c) { return this.insertBefore(c, null); };
El.prototype.insertBefore = function (c, ref) {
  if (c.parentNode) c.parentNode.children.splice(c.parentNode.children.indexOf(c), 1);
  var i = ref ? this.children.indexOf(ref) : -1;
  if (i < 0) this.children.push(c); else this.children.splice(i, 0, c);
  c.parentNode = this;
  return c;
};
El.prototype.remove = function () {
  if (this.parentNode) this.parentNode.children.splice(this.parentNode.children.indexOf(this), 1);
  this.parentNode = null;
};
El.prototype.addEventListener = function (type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); };
El.prototype.dispatch = function (type, ev) {
  ev = ev || {};
  if (!ev.target) ev.target = this;
  (this.listeners[type] || []).forEach(function (fn) { fn(ev); });
};
El.prototype.scrollIntoView = function () { this.scrolled = true; };
// selector: .cls, tag, tag.cls, [attr=value], tag[attr=value], tag[attr]
function matcher(sel) {
  var m = /^([a-z0-9]*)(?:\.([\w-]+))?(?:\[([\w-]+)(?:=([\w-]+))?\])?$/.exec(sel);
  if (!m) throw new Error('dom_shim: unsupported selector ' + sel);
  return function (e) {
    if (m[1] && e.tagName.toLowerCase() !== m[1]) return false;
    if (m[2] && !e.classList.contains(m[2])) return false;
    if (m[3]) {
      var v = e.getAttribute(m[3]);
      if (v === null || (m[4] !== undefined && v !== m[4])) return false;
    }
    return true;
  };
}
El.prototype.querySelectorAll = function (sel) {
  var ok = matcher(sel), out = [];
  (function walk(e) {
    e.children.forEach(function (c) { if (ok(c)) out.push(c); walk(c); });
  })(this);
  return out;
};
El.prototype.querySelector = function (sel) { return this.querySelectorAll(sel)[0] || null; };
El.prototype.closest = function (sel) {
  var ok = matcher(sel);
  for (var e = this; e; e = e.parentNode) if (ok(e)) return e;
  return null;
};
// Attributes sorted by name, so the order they were set in does not matter.
El.prototype.html = function () {
  var attrs = Object.keys(this.attrs).sort().map(function (k) {
    return this.attrs[k] === '' ? ' ' + k : ' ' + k + '="' + this.attrs[k] + '"';
  }, this).join('');
  return '<' + this.tagName.toLowerCase() + attrs + '>' + this.own +
    this.children.map(function (c) { return c.html(); }).join('') + '</' + this.tagName.toLowerCase() + '>';
};

function el(tag, cls, text) {
  var e = new El(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

module.exports = { El: El, el: el };
