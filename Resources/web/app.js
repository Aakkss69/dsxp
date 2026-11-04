/* DSXP mapper UI. Served by the app on dsxp://app/ - relative URLs only.
 * Wording follows Apple's guidance: title-style buttons and segments,
 * sentence-style labels, status and notes, US English. Names match the config
 * files: a trigger, rumble or light "source" is manual, wind or sim. */

let commands = [], slots = [], axisSources = [], axisTargets = [];
let selectedSlot = -1, resultIndex = 0, shownResults = [];
let bindings = [], axisMap = [], previousButtons = [];
let tune = { deadzone: 0.06, curve: 1, gesture: 0.5 };
let triggerSource = 'manual', rumbleSource = 'manual';
let light = { r: 0, g: 90, b: 255, on: true, pattern: 0, speed: 0.5, player: 4, bright: 1, source: 'manual' };
let triggerManual = [{ mode: 0, start: 70, end: 160, strength: 200, freq: 40 },
                     { mode: 0, start: 70, end: 160, strength: 200, freq: 40 }];
let rumbleLevels = [0, 0];            /* the slider wins while it is being dragged */
let lastPanel = null;                 /* the panel the user last worked in, for errors */
let busyAt = 0;                       /* last touch of a tuning panel */
let stopped = false;

/* A value the user just changed wins over the poll for a moment, so a slider
 * does not jump back before the core has applied it. */
const EDIT_HOLD_MS = 600;
const editedAt = { tune: 0, light: 0, trigger: 0, triggerSource: 0, rumble: 0, rumbleSource: 0 };
const markEdited = key => { editedAt[key] = Date.now(); };
const recentlyEdited = key => Date.now() - editedAt[key] < EDIT_HOLD_MS;

const TRIGGER_SWEEP = 26;          /* degrees of blade travel, rest -> fully in */
const STICK_TRAVEL  = 32;          /* px the knob moves at full deflection (well r~101, cap r~50) */
const MS_TO_KT = 1.943844;         /* X-Plane sends wind in m/s; pilots read knots */

const $ = selector => document.querySelector(selector);
const byId = id => document.getElementById(id);
const escapeHtml = s => String(s).replace(/[&<>"]/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;' }[c]));
const percent = v => Math.round(v * 100) + '%';
const bytePercent = v => Math.round(v / 255 * 100) + '%';
const setText = (element, text) => { if (element && element.textContent !== text) element.textContent = text; };
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/* a segmented control: exactly one segment on */
function selectSegment(group, isOn) {
  group.querySelectorAll('button').forEach(b => {
    const on = isOn(b);
    b.classList.toggle('on', on);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
  });
}

/* ------------------------------------------------------ command search */

function score(item, query) {
  let i = item.pathLower.indexOf(query);
  if (i >= 0) return 1000 - i * 2 - item.pathLower.length * 0.05;
  i = item.descLower.indexOf(query);
  if (i >= 0) return 600 - i * 2 - item.descLower.length * 0.03;
  let matched = 0;                  /* forgiving subsequence over the path */
  for (let k = 0; k < item.pathLower.length && matched < query.length; k++)
    if (item.pathLower[k] === query[matched]) matched++;
  return matched === query.length ? 300 - item.pathLower.length * 0.05 : -1;
}

function searchCommands(query) {
  query = query.trim().toLowerCase();
  if (!query) return commands.slice(0, 80);
  const scored = [];
  for (const c of commands) { const s = score(c, query); if (s > 0) scored.push([s, c]); }
  scored.sort((a, b) => b[0] - a[0]);
  return scored.slice(0, 100).map(x => x[1]);
}

function highlight(text, query) {
  if (!query) return escapeHtml(text);
  const i = text.toLowerCase().indexOf(query.toLowerCase());
  if (i < 0) return escapeHtml(text);
  return escapeHtml(text.slice(0, i)) + '<mark>' + escapeHtml(text.slice(i, i + query.length)) + '</mark>'
       + escapeHtml(text.slice(i + query.length));
}

function renderResults() {
  const list = byId('command-results');
  if (selectedSlot < 0) {
    list.innerHTML = '<div class="list-empty">Select a control on the map or in the list, '
                   + 'or press it on the controller.</div>';
    return;
  }
  const query = byId('command-search').value.trim();
  shownResults = searchCommands(query);
  if (!shownResults.length) {
    list.innerHTML = '<div class="list-empty">' + (commands.length
      ? 'No commands match &ldquo;' + escapeHtml(query) + '&rdquo;.'
      : 'No command list yet. Choose your X-Plane folder at the top of the window.') + '</div>';
    return;
  }
  if (resultIndex >= shownResults.length) resultIndex = 0;
  list.innerHTML = shownResults.map((c, i) =>
    '<div class="result' + (i === resultIndex ? ' current' : '') + '" role="option" data-index="' + i + '"'
      + (i === resultIndex ? ' aria-selected="true"' : '') + '>' +
      '<div class="result-path">' + highlight(c.path, query) + '</div>' +
      (c.desc ? '<div class="result-desc">' + highlight(c.desc, query) + '</div>' : '') +
    '</div>').join('');
  const current = list.querySelector('.result.current');
  if (current && resultIndex > 0) current.scrollIntoView({ block: 'nearest' });
}

/* ------------------------------------------------------------ changes */

/* A short neutral notice at the bottom of the window. */
let toastTimer = 0;
function toast(message) {
  const t = byId('toast');
  t.textContent = message;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3000);
}

/* A refused change (e.g. 409 while wind sets the triggers) is shown under the
   panel it came from, not swallowed. */
function panelError(message) {
  const panel = lastPanel || $('main section');
  const foot = panel.querySelector('.group-foot');
  let p = foot.querySelector('.inline-error');
  if (!p) {
    p = document.createElement('p');
    p.className = 'inline-error';
    p.setAttribute('role', 'alert');
    foot.appendChild(p);
  }
  p.innerHTML = '<i class="dot warn"></i>' + escapeHtml(message);
  p.hidden = false;
  clearTimeout(p.hideTimer);
  p.hideTimer = setTimeout(() => { p.hidden = true; }, 4000);
}

/* Every change is a POST. The poll loop picks up the new state within 50 ms. */
const post = url => fetch(url, { method: 'POST' })
  .then(r => { if (!r.ok) return r.text().then(t => panelError(t || ('The change was refused (' + r.status + ').'))); })
  .catch(() => {});

function assignCommand(path) {
  if (selectedSlot < 0) return;
  const repeat = bindings[selectedSlot] ? bindings[selectedSlot].r : 0;
  post('/api/bind?i=' + selectedSlot + '&repeat=' + repeat + '&cmd=' + encodeURIComponent(path));
}
function setRepeat(slot, on) {
  const b = bindings[slot];
  if (!b || !b.c) return;
  post('/api/bind?i=' + slot + '&repeat=' + (on ? 1 : 0) + '&cmd=' + encodeURIComponent(b.c));
}
const setAxis = (source, target, invert) =>
  post('/api/axis?k=' + source + '&t=' + target + '&inv=' + (invert ? 1 : 0));
function sendTune() {
  markEdited('tune');
  post('/api/tune?deadzone=' + tune.deadzone + '&curve=' + tune.curve + '&gesture=' + tune.gesture);
}
function sendLight() {
  markEdited('light');
  post('/api/light?r=' + light.r + '&g=' + light.g + '&b=' + light.b
     + '&on=' + (light.on ? 1 : 0) + '&pattern=' + light.pattern
     + '&speed=' + light.speed + '&player=' + light.player
     + '&bright=' + light.bright + '&source=' + light.source);
}
function sendTrigger(side) {
  markEdited('trigger');
  const t = triggerManual[side];
  post('/api/trigger?side=' + (side ? 'r' : 'l') + '&mode=' + t.mode +
       '&start=' + t.start + '&end=' + t.end + '&strength=' + t.strength + '&freq=' + t.freq);
}
function sendRumble() {
  markEdited('rumble');
  post('/api/rumble?left=' + rumbleLevels[0] + '&right=' + rumbleLevels[1]);
}

/* ------------------------------------------------------------- colors */

const hexToRgb = h => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
const rgbToHex = (r, g, b) => '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('').toUpperCase();

function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d) {
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h = (h * 60 + 360) % 360;
  }
  return { h, s: max ? d / max : 0, v: max };
}

function hsvToRgb({ h, s, v }) {
  const c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c;
  const [r, g, b] = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x]
                  : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
  return [r, g, b].map(t => Math.round((t + m) * 255));
}

const PRESET_COLORS = [['#FF2D1A', 'Red'], ['#FF8C00', 'Orange'], ['#FFD400', 'Yellow'], ['#22C55E', 'Green'],
                       ['#005AFF', 'Blue'], ['#8B5CF6', 'Purple'], ['#FFFFFF', 'White']];

function renderSwatches(container) {
  container.innerHTML = PRESET_COLORS.map(([hex, name]) =>
    '<button class="swatch" data-color="' + hex + '" style="--swatch:' + hex + '" title="' + name +
    '" aria-label="' + name + '"></button>').join('');
}

function markCurrentSwatch() {
  const hex = rgbToHex(light.r, light.g, light.b);
  document.querySelectorAll('.swatch').forEach(s => s.classList.toggle('current', s.dataset.color === hex));
  byId('light-color').firstElementChild.style.background = hex;
}

/* The color popover: drag in the field (saturation, brightness) and the hue
 * strip, type a hex value, or pick a preset. The light bar follows while you
 * pick; OK keeps the color, Cancel (or Esc, or a click outside) puts the old
 * one back. Both close the popover. */
const picker = { open: false, original: [0, 0, 0], hsv: { h: 0, s: 0, v: 0 }, sendTimer: 0 };

function renderPicker(updateHexField = true) {
  const { h, s, v } = picker.hsv;
  const field = byId('color-field');
  field.style.setProperty('--hue', String(Math.round(h)));
  byId('color-field-knob').style.left = (s * 100) + '%';
  byId('color-field-knob').style.top = ((1 - v) * 100) + '%';
  byId('color-hue-knob').style.left = (h / 360 * 100) + '%';
  field.setAttribute('aria-valuetext', 'saturation ' + percent(s) + ', brightness ' + percent(v));
  byId('color-hue').setAttribute('aria-valuenow', String(Math.round(h)));
  const hex = rgbToHex(light.r, light.g, light.b);
  if (updateHexField) byId('color-hex').value = hex;
  byId('color-old').style.background = rgbToHex(...picker.original);
  byId('color-new').style.background = hex;
  markCurrentSwatch();
}

/* the light bar follows the picker, at most every 50 ms */
function previewColor() {
  markEdited('light');
  if (picker.sendTimer) return;
  picker.sendTimer = setTimeout(() => { picker.sendTimer = 0; sendLight(); }, 50);
}

function pickerChanged(updateHexField = true) {
  [light.r, light.g, light.b] = hsvToRgb(picker.hsv);
  renderPicker(updateHexField);
  previewColor();
}

function setPickerColor(hex, updateHexField = true) {
  [light.r, light.g, light.b] = hexToRgb(hex);
  const hsv = rgbToHsv(light.r, light.g, light.b);
  /* keep the hue where it was for greys, so the field does not jump to red */
  if (hsv.s === 0) hsv.h = picker.hsv.h;
  picker.hsv = hsv;
  renderPicker(updateHexField);
  previewColor();
}

function placePopover(popover, anchor) {
  const a = anchor.getBoundingClientRect();
  const w = popover.offsetWidth, h = popover.offsetHeight;
  const left = clamp(a.left, 8, window.innerWidth - w - 8) + window.scrollX;
  const below = a.bottom + 8, above = a.top - 8 - h;
  const top = (below + h <= window.innerHeight - 8 || above < 8 ? below : above) + window.scrollY;
  popover.style.left = left + 'px';
  popover.style.top = top + 'px';
}

function openColorPicker() {
  if (picker.open) return;
  picker.open = true;
  picker.original = [light.r, light.g, light.b];
  picker.hsv = rgbToHsv(light.r, light.g, light.b);
  const popover = byId('color-popover'), well = byId('light-color');
  popover.hidden = false;
  renderPicker();
  placePopover(popover, well);
  well.setAttribute('aria-expanded', 'true');
  byId('color-hex').focus();
  byId('color-hex').select();
}

function closeColorPicker(keep) {
  if (!picker.open) return;
  picker.open = false;
  clearTimeout(picker.sendTimer);
  picker.sendTimer = 0;
  if (!keep) [light.r, light.g, light.b] = picker.original;
  sendLight();                       /* the final color, kept or restored */
  byId('color-popover').hidden = true;
  const well = byId('light-color');
  well.setAttribute('aria-expanded', 'false');
  well.focus();
  markCurrentSwatch();
}

/* drag inside an element: fn(x, y) gets 0..1 fractions */
function trackPointer(element, fn) {
  const move = e => {
    const r = element.getBoundingClientRect();
    fn(clamp((e.clientX - r.left) / r.width, 0, 1), clamp((e.clientY - r.top) / r.height, 0, 1));
  };
  let dragging = false;
  element.addEventListener('pointerdown', e => {
    dragging = true;
    try { element.setPointerCapture(e.pointerId); } catch (err) { /* keep tracking without capture */ }
    element.focus({ preventScroll: true });
    move(e);
  });
  element.addEventListener('pointermove', e => { if (dragging) move(e); });
  const stop = () => { dragging = false; };
  element.addEventListener('pointerup', stop);
  element.addEventListener('pointercancel', stop);
  element.addEventListener('lostpointercapture', stop);
}

/* ---------------------------------------------------------- light bar */

const PATTERN_NAME = ['Solid', 'Breathe', 'Pulse', 'Strobe', 'Rainbow', 'Alert'];

function showLightSource() {
  const isSim = light.source === 'sim';
  const tab = isSim ? 'sim' : (light.pattern !== 0 ? 'pattern' : 'solid');
  selectSegment(byId('light-source'), b => b.dataset.source === tab);
  for (const which of ['solid', 'pattern', 'sim'])
    document.querySelectorAll('.when-' + which).forEach(e => { e.hidden = tab !== which; });
  selectSegment(byId('light-patterns'), b => +b.dataset.pattern === light.pattern);
  selectSegment(byId('player-lamps'), b => +b.dataset.lamps === light.player);
  byId('light-note').textContent = isSim
    ? 'The light bar becomes an annunciator, most urgent first: a crash strobes red, a stall '
    + 'pulses red, a stall warning breathes amber, high load holds amber, turbulence breathes '
    + 'cyan, otherwise it is steady green. The thresholds are the light.sim settings in feedback.conf.'
    : tab === 'pattern'
    ? 'The pattern animates the color chosen under Solid. It refreshes 20 times a second, '
    + 'which leaves the Bluetooth link free for input.'
    : 'DSXP sends the color to the controller and repeats it every half second, because macOS '
    + 'has been observed to set its own color.';
}

function selectSlot(slot, focusSearch) {
  selectedSlot = slot; resultIndex = 0;
  renderResults();
  if (!focusSearch) return;
  /* focus({preventScroll}) alone is not enough - input.select() scrolls the box
   * into view on its own, which dragged the page around when a trigger moved.
   * Only do either when the user actually clicked something. */
  byId('command-search').focus({ preventScroll: true });
  byId('command-search').select();
}

/* ------------------------------------------------------ trigger effects */

/* Parameters mean different things per effect, so the labels follow it. The
 * wire format is a raw 0-255 byte per parameter: positions read as a share of
 * trigger travel, force as a share of the maximum, the vibrate rate as Hz. */
const AS_PERCENT = { max: 255, step: 1, format: v => bytePercent(v) };
const AS_HERTZ   = { max: 60,  step: 1, format: v => v + ' Hz' };
const EFFECT_PARAMS = {
  0: {},
  1: { start:    { label: 'Start:',      ...AS_PERCENT },
       strength: { label: 'Force:',      ...AS_PERCENT } },
  2: { start:    { label: 'Band start:', ...AS_PERCENT },
       end:      { label: 'Band end:',   ...AS_PERCENT },
       strength: { label: 'Force:',      ...AS_PERCENT } },
  3: { start:    { label: 'Threshold:',  ...AS_PERCENT },
       strength: { label: 'Amplitude:',  ...AS_PERCENT },
       freq:     { label: 'Frequency:',  ...AS_HERTZ   } },
};
const EFFECT_DEFAULTS = {
  1: { start: 70, end: 0,   strength: 200 },
  2: { start: 60, end: 160, strength: 255 },
  3: { start: 40, freq: 40, strength: 180 },
};
const EFFECT_NAME = ['Off', 'Resist', 'Weapon', 'Vibrate'];

/* Syncs the effect panel to triggerManual. It only renders: the listeners are
   attached once, at the bottom of this file. */
function renderTriggerEffects() {
  document.querySelectorAll('.effect-column').forEach(column => {
    const side = column.dataset.side === 'r' ? 1 : 0;
    const t = triggerManual[side];
    selectSegment(column.querySelector('.effect-modes'), b => +b.dataset.effect === t.mode);
    const params = EFFECT_PARAMS[t.mode] || {};
    const form = column.querySelector('.effect-params');
    const signature = Object.keys(params).join(',') + '|' + t.mode;
    if (form.dataset.signature !== signature) {
      form.dataset.signature = signature;
      form.innerHTML = Object.entries(params).map(([key, p]) => {
        const id = 'effect-' + side + '-' + key;
        return '<label for="' + id + '">' + p.label + '</label>' +
          '<input type="range" id="' + id + '" data-param="' + key + '" min="0" max="' + p.max +
          '" step="' + p.step + '" value="' + t[key] + '">' +
          '<output for="' + id + '" data-value="' + key + '">' + p.format(t[key]) + '</output>';
      }).join('') || '<span class="off">No resistance.</span>';
    }
    Object.entries(params).forEach(([key, p]) => {
      const input = form.querySelector('[data-param="' + key + '"]');
      if (input && document.activeElement !== input) input.value = t[key];
      setText(form.querySelector('[data-value="' + key + '"]'), p.format(t[key]));
    });
  });
}

function showTriggerSource() {
  selectSegment(byId('trigger-source'), b => b.dataset.source === triggerSource);
  byId('trigger-manual').hidden = triggerSource !== 'manual';
  byId('trigger-wind').hidden   = triggerSource !== 'wind';
  byId('trigger-note').innerHTML = triggerSource === 'wind'
    ? 'Force follows dynamic pressure, so it grows with the square of indicated airspeed, '
    + 'like the load on a real rudder pedal. A crosswind makes one trigger heavier, and '
    + 'turbulence turns resistance into buffet. The numbers behind this are the wind settings '
    + 'in feedback.conf; your manual effects come back when you switch to Manual.'
    : '<b>Resist</b> holds a constant force past a point. <b>Weapon</b> resists across a band, '
    + 'then breaks through like a gun trigger. <b>Vibrate</b> buzzes once you press past the '
    + 'threshold. Positions are a share of trigger travel and force is a share of the maximum. '
    + 'Squeeze a trigger to feel the change.';
}

/* ---------------------------------------------------------------- rumble */

/* Motor bytes read as % of full scale. The left motor is the heavy one, so the
 * same byte feels stronger there - that asymmetry is real, not a bug. */
const RUMBLE_TESTS = {
  soft:  [70, 70],   hard:  [200, 200],
  left:  [220, 40],  right: [40, 220],
  crash: [255, 255], stop:  [0, 0],
};

function showRumbleSource() {
  selectSegment(byId('rumble-source'), b => b.dataset.source === rumbleSource);
  byId('rumble-manual').hidden = rumbleSource !== 'manual';
  byId('rumble-sim').hidden    = rumbleSource !== 'sim';
  byId('rumble-note').textContent = rumbleSource === 'sim'
    ? 'Set by the flight model. A stall on one wing buzzes that side harder, so you feel which '
    + 'way the aircraft is about to roll; high load adds a jolt on both sides, and a crash shakes '
    + 'both. The thresholds are the rumble settings in feedback.conf.'
    : 'Run the motors by hand to feel their range. The left motor is the heavier one, so equal '
    + 'values feel stronger on the left. Closing this window stops them. If the motors do '
    + 'nothing, turn the v2 path off.';
}

/* ------------------------------------------------------------ rendering */

/* L2/R2 carry an analog axis as well as a click, so their rows say so -
 * otherwise "Not assigned" reads as if the trigger does nothing. */
const AXIS_FOR_SLOT = { 14: 'l2', 15: 'r2' };

function renderButtons() {
  byId('button-list').innerHTML = slots.map(s => {
    const b = bindings[s.i] || { c: '', r: 0 };
    return '<div class="binding" role="option" data-slot="' + s.i + '">' +
      '<div class="binding-name">' + escapeHtml(s.n) + '</div>' +
      '<div class="binding-command' + (b.c ? '' : ' none') + '">' + escapeHtml(b.c || 'Not assigned') + '</div>' +
      (AXIS_FOR_SLOT[s.i] ? '<span class="axis-tag" title="This trigger also moves an axis (see Axes)" hidden>Also an axis</span>' : '') +
      '<label class="check repeat"' + (b.c ? '' : ' hidden') + '><input type="checkbox" data-repeat="' + s.i + '"' +
        (b.r ? ' checked' : '') + '> Repeat</label>' +
    '</div>';
  }).join('');
}

function renderAxes() {
  byId('axis-list').innerHTML = axisSources.map((s, i) => {
    const a = axisMap[i] || { t: 'none', inv: 0 };
    return '<div class="axis-row">' +
      '<label for="axis-' + s.k + '">' + escapeHtml(s.n) + '</label>' +
      '<div class="axis-bar"><i data-axis-bar="' + i + '"></i></div>' +
      '<select id="axis-' + s.k + '" data-axis="' + s.k + '">' + axisTargets.map(t =>
        '<option value="' + t.k + '"' + (t.k === a.t ? ' selected' : '') + '>' +
        escapeHtml(t.n) + '</option>').join('') + '</select>' +
      '<label class="check"><input type="checkbox" data-invert="' + s.k + '"' + (a.inv ? ' checked' : '') +
        '> Invert</label>' +
    '</div>';
  }).join('');
}

/* Keep the axis pickers and Invert boxes in step with the core. */
function syncAxes() {
  document.querySelectorAll('#axis-list .axis-row').forEach((row, i) => {
    const a = axisMap[i];
    if (!a) return;
    const select = row.querySelector('select[data-axis]');
    if (select && document.activeElement !== select && select.value !== a.t) select.value = a.t;
    const invert = row.querySelector('input[data-invert]');
    if (invert && invert.checked !== !!a.inv) invert.checked = !!a.inv;
  });
}

/* The status strip: controller, X-Plane, flight controls. */
function setStatus(id, kind, text) {
  const item = byId(id);
  item.querySelector('.dot').className = 'dot' + (kind ? ' ' + kind : '');
  setText(item.querySelector('.status-text'), text);
}

/* Config problems: a button in the strip, the list in a popover. */
let configGeneration = -1, configSignature = '';
function renderConfig(config) {
  if (!config) return;
  const warnings = config.warnings || [];
  const count = warnings.length + (config.dropped || 0);
  const button = byId('config-problems');
  button.hidden = count === 0;
  setText(button.querySelector('span'), count === 1 ? '1 Config Problem…' : count + ' Config Problems…');
  const signature = JSON.stringify(warnings) + config.dropped;
  if (signature !== configSignature) {
    configSignature = signature;
    byId('config-list').innerHTML = warnings.map(w =>
      '<li><div class="where">' + escapeHtml(w.file) + (w.line ? ', line ' + w.line : '') +
        (w.key ? ' &middot; ' + escapeHtml(w.key) : '') + '</div>' +
      '<div class="msg">' + escapeHtml(w.msg.charAt(0).toUpperCase() + w.msg.slice(1)) + '.</div>' +
      '<div class="fix"><button data-open="' + escapeHtml(w.file) + '">Open ' + escapeHtml(w.file) + '</button></div></li>'
    ).join('') + (config.dropped ? '<li>' + config.dropped + ' more not shown.</li>' : '');
    if (count === 0) byId('config-popover').hidden = true;
  }
  /* an edit made outside the mapper was applied: say so once */
  if (configGeneration >= 0 && config.gen !== configGeneration && config.reloaded)
    toast('Applied your edit to ' + config.reloaded + (warnings.some(w => w.file === config.reloaded)
          ? ', except the lines listed under Config Problems.' : '.'));
  configGeneration = config.gen;
}

let lastCommandCount = -1;
function renderApp(app, state) {
  byId('allow-input').hidden = app.access === 'granted' || app.input_live || app.access === undefined;
  const folder = byId('xplane-folder');
  setText(folder, app.xpdir ? app.xpdir.replace(/^\/Users\/[^/]+/, '~') : 'X-Plane folder not found');
  folder.title = app.xpdir || 'Choose your X-Plane 12 folder to search its commands';
  folder.classList.toggle('missing', !app.xpdir);
  byId('imported-note').hidden = !app.imported;
  renderConfig(app.config);
  if (typeof app.cmds === 'number' && app.cmds !== lastCommandCount) {
    if (lastCommandCount >= 0) loadCatalog();     /* the command list changed: reload it */
    lastCommandCount = app.cmds;
  }

  const gate = state.gate;
  setText(byId('status-rate'), !gate.usb && gate.bt && app.input_live && app.hz ? Math.round(app.hz) + ' Hz' : '');
  if (gate.usb) setStatus('status-controller', 'warn', 'Controller on USB, X-Plane reads it directly');
  else if (gate.bt) setStatus('status-controller', app.input_live === false ? 'warn' : 'ok',
                              app.input_live === false ? 'Controller connected, no input' : 'Controller connected');
  else setStatus('status-controller', '', 'No controller');
  if (!gate.xp) setStatus('status-xplane', '', 'X-Plane 12 not running');
  else if (app.xp_answering === false) setStatus('status-xplane', 'warn', 'X-Plane 12 not answering');
  else setStatus('status-xplane', 'ok', 'X-Plane 12 answering');
  byId('status-xplane').title = gate.xp && app.xp_answering === false
    ? 'X-Plane is running but sends no data back. Check Settings › Network in X-Plane.' : '';
  if (app.suspended) setStatus('status-controls', '', 'Paused while the Mac sleeps');
  else setStatus('status-controls', gate.engaged ? 'ok' : '', gate.engaged ? 'Flight controls active' : 'Standing by');
  byId('status-controls').title = gate.engaged
    ? 'DSXP is sending your stick, trigger and button input to X-Plane'
    : 'DSXP takes the controls when the controller is on Bluetooth and X-Plane answers';
  byId('status-controller').title = state.reports.toLocaleString() + ' input reports in the last 10 seconds, '
                                  + state.total.toLocaleString() + ' since DSXP started';
}

/* mirrors shape() in Sources/Core/input/decode.c so the preview matches what is sent */
function shape(v) {
  const deadzone = tune.deadzone, curve = tune.curve;
  const sign = v < 0 ? -1 : 1, magnitude = Math.abs(v);
  if (deadzone > 0 && magnitude <= deadzone) return 0;
  let a = deadzone > 0 ? (magnitude - deadzone) / (1 - deadzone) : magnitude;
  if (curve !== 1) a = Math.pow(a, curve);
  return sign * a;
}

function drawCurve() {
  const points = [];
  for (let i = 0; i <= 60; i++) {
    const x = -1 + (2 * i) / 60;
    points.push((60 + x * 58).toFixed(1) + ',' + (60 - shape(x) * 58).toFixed(1));
  }
  byId('curve-line').setAttribute('points', points.join(' '));
  setText(byId('deadzone-value'), (tune.deadzone * 100).toFixed(1) + '%');
  setText(byId('curve-value'), tune.curve.toFixed(2));
  setText(byId('gesture-value'), percent(tune.gesture ?? 0.5));
}

/* Side-view gauge: the blade pivots through its real travel, so a glance tells
 * you which trigger is down and by how much. */
function drawTrigger(side, travel) {
  byId('trigger-' + side + '-lever').setAttribute('transform',
    'translate(66 66) rotate(' + (-travel * TRIGGER_SWEEP) + ')');
  setText(byId('trigger-' + side + '-value'), percent(travel));
  byId('trigger-' + side + '-bar').setAttribute('width', 120 * travel);
}

/* ---------------------------------------------------------- live poll */

async function poll() {
  if (stopped) return;
  let state;
  try { state = await (await fetch('/api/state')).json(); } catch (e) { return; }
  bindings = state.bind; axisMap = state.axismap;
  syncAxes();
  renderApp(state.app || {}, state);
  setText(byId('controller-status'), state.connected ? 'Receiving input' : 'Waiting for input');

  if (!recentlyEdited('tune')) {
    tune = state.tune;
    for (const key of ['deadzone', 'curve', 'gesture'])
      if (document.activeElement !== byId(key)) byId(key).value = tune[key];
    drawCurve();
  }

  /* the manual panel edits the hand-set values; with the wind source it is hidden */
  if (!recentlyEdited('trigger') && (state.trigger_manual || state.fx)) {
    triggerManual = state.trigger_manual || state.fx;
    renderTriggerEffects();
  }
  if (!recentlyEdited('triggerSource') && state.trigger_source && state.trigger_source !== triggerSource) {
    triggerSource = state.trigger_source; showTriggerSource();
  }
  setText(byId('trigger-status'), triggerSource === 'wind'
    ? (state.wx && state.wx.live ? 'Wind, live' : 'Wind, waiting for X-Plane')
    : 'L2 ' + EFFECT_NAME[triggerManual[0].mode] + ', R2 ' + EFFECT_NAME[triggerManual[1].mode]);

  if (!recentlyEdited('rumbleSource') && state.rumble_source && state.rumble_source !== rumbleSource) {
    rumbleSource = state.rumble_source; showRumbleSource();
  }
  setText(byId('rumble-status'), state.rumble_note || '');

  if (state.rumble && document.activeElement !== byId('rumble-v2')) byId('rumble-v2').checked = !!state.rumble.v2;
  if (rumbleSource === 'manual' && !recentlyEdited('rumble') && state.rumble) {
    rumbleLevels = [state.rumble.left, state.rumble.right];
    if (document.activeElement !== byId('rumble-left')) byId('rumble-left').value = rumbleLevels[0];
    if (document.activeElement !== byId('rumble-right')) byId('rumble-right').value = rumbleLevels[1];
    setText(byId('rumble-left-value'), bytePercent(rumbleLevels[0]));
    setText(byId('rumble-right-value'), bytePercent(rumbleLevels[1]));
  }
  if (rumbleSource === 'sim' && state.wx) {
    const w = state.wx;
    setText(byId('rumble-stall-left'), w.live ? percent(w.stall_l) : '—');
    setText(byId('rumble-stall-right'), w.live ? percent(w.stall_r) : '—');
    setText(byId('rumble-stall-warning'), w.live ? percent(w.stall) : '—');
    setText(byId('rumble-load'), w.live ? w.gn.toFixed(2) + ' g' : '—');
    const crashed = byId('rumble-crashed');
    const text = w.live ? (w.crashed ? 'Yes' : 'No') : '—';
    if (crashed.dataset.text !== text) {
      crashed.dataset.text = text;
      crashed.innerHTML = (w.live && w.crashed ? '<i class="dot bad"></i>' : '') + text;
    }
    setText(byId('rumble-state'), state.rumble_note || '—');
    byId('rumble-state').classList.toggle('stale', !w.live);
    setText(byId('rumble-out-left'), state.rumble ? bytePercent(state.rumble.left) : '—');
    setText(byId('rumble-out-right'), state.rumble ? bytePercent(state.rumble.right) : '—');
  }

  if (triggerSource === 'wind' && state.wx) {
    const w = state.wx;
    setText(byId('wind-ias'),        w.live ? Math.round(w.ias) + ' kt' : '—');
    setText(byId('wind-speed'),      w.live ? Math.round(w.wind * MS_TO_KT) + ' kt from ' + Math.round(w.dir) + '°' : '—');
    setText(byId('wind-crosswind'),  w.live
      ? (Math.abs(w.xwind) * MS_TO_KT).toFixed(1) + ' kt from the ' + (w.xwind >= 0 ? 'right' : 'left') : '—');
    setText(byId('wind-turbulence'), w.live ? w.turb.toFixed(1) + ' of 10' : '—');
    setText(byId('wind-load'),       w.live ? percent(w.load) : '—');
    setText(byId('wind-state'),      w.trigger_note || '—');
    byId('wind-state').classList.toggle('stale', !w.live);
    const describe = e => e.mode === 0 ? 'Free'
      : e.mode === 3 ? 'Buzz ' + bytePercent(e.strength) + ' at ' + e.end + ' Hz'
      : bytePercent(e.strength) + ' force';
    /* what is actually on the triggers (state.fx), not the manual set */
    setText(byId('wind-l2'), describe(state.fx[0]));
    setText(byId('wind-r2'), describe(state.fx[1]));
  }

  if (!picker.open && !recentlyEdited('light') && state.light) {
    light = state.light;
    const hex = rgbToHex(light.r, light.g, light.b);
    byId('light-on').checked = !!light.on;
    if (document.activeElement !== byId('light-speed')) byId('light-speed').value = light.speed;
    setText(byId('light-speed-value'), percent(light.speed));
    if (document.activeElement !== byId('light-brightness')) byId('light-brightness').value = light.bright;
    setText(byId('light-brightness-value'), percent(light.bright));
    if (light.source === 'sim') setText(byId('light-showing'), light.note || '—');
    setText(byId('light-status'), !light.on ? 'Off'
      : light.source === 'sim' ? 'Simulator: ' + (light.note || '')
      : (light.pattern ? PATTERN_NAME[light.pattern] + ', ' + hex : hex));
    markCurrentSwatch();
    showLightSource();
  }

  /* live touch points over the traced touchpad (bbox 394,11 522x281) */
  if (state.touch) state.touch.forEach((t, i) => {
    const dot = byId('touch-' + i);
    if (!dot) return;
    if (t.on) { dot.setAttribute('cx', 394 + t.x * 522); dot.setAttribute('cy', 11 + t.y * 281); }
    else      { dot.setAttribute('cx', -99); dot.setAttribute('cy', -99); }
  });

  /* Physically pressing a control jumps to its command - but not while you are
   * working a slider or picker, otherwise squeezing a trigger to feel an effect
   * yanks focus into the search box mid-drag. */
  const focused = document.activeElement;
  const busyElsewhere = picker.open || (focused && focused !== byId('command-search') &&
      /^(INPUT|SELECT|BUTTON|TEXTAREA)$/.test(focused.tagName))
    || Date.now() - busyAt < 1500;
  const typing = focused === byId('command-search') && byId('command-search').value.trim() !== '';
  if (!busyElsewhere && !typing)
    for (let i = 0; i < state.btn.length; i++)
      if (state.btn[i] && !previousButtons[i]) selectSlot(i, false);
  previousButtons = state.btn.slice();

  for (let i = 0; i < state.btn.length; i++) {
    const shapeOnMap = byId('b-' + i);
    if (!shapeOnMap) continue;
    shapeOnMap.classList.toggle('down', !!state.btn[i]);
    shapeOnMap.classList.toggle('sel', selectedSlot === i);
  }
  /* a diagonal lights its two adjacent arms too, the way the pad actually feels */
  const neighbors = { 1: [0, 2], 3: [2, 4], 5: [4, 6], 7: [6, 0] };
  for (const d in neighbors)
    if (state.btn[d]) neighbors[d].forEach(a => byId('b-' + a).classList.add('down'));
  /* the shoulder shown in the side view mirrors the one on the pad */
  byId('b-12s').classList.toggle('down', !!state.btn[12]);
  byId('b-13s').classList.toggle('down', !!state.btn[13]);

  /* sticks follow the shaped values; the traced caps are paths, so they move by transform */
  byId('stick-l').setAttribute('transform', 'translate(' + (state.axes[0] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (state.axes[1] * STICK_TRAVEL).toFixed(1) + ')');
  byId('stick-r').setAttribute('transform', 'translate(' + (state.axes[2] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (state.axes[3] * STICK_TRAVEL).toFixed(1) + ')');

  /* triggers: raw travel, so the gauge shows the physical squeeze even when the
   * dead zone is swallowing it downstream */
  drawTrigger('l', state.raw[4]);
  drawTrigger('r', state.raw[5]);
  setText(byId('trigger-travel'), state.raw[4] > 0.02 || state.raw[5] > 0.02
    ? 'L2 ' + percent(state.raw[4]) + ', R2 ' + percent(state.raw[5]) : '');

  const list = byId('button-list');
  if (list.children.length !== slots.length) renderButtons();
  [...list.children].forEach((row, i) => {
    row.classList.toggle('down', !!state.btn[i]);
    row.classList.toggle('selected', selectedSlot === i);
    row.setAttribute('aria-selected', selectedSlot === i ? 'true' : 'false');
    const b = bindings[i] || { c: '', r: 0 };
    const command = row.querySelector('.binding-command');
    const wanted = b.c || 'Not assigned';
    if (command.textContent !== wanted) {
      command.textContent = wanted;
      command.className = 'binding-command' + (b.c ? '' : ' none');
    }
    const repeat = row.querySelector('.repeat');
    repeat.hidden = !b.c;
    const box = repeat.querySelector('input');
    if (box.checked !== !!b.r) box.checked = !!b.r;
    const tag = row.querySelector('.axis-tag');
    if (tag) {
      const a = axisMap[axisSources.findIndex(x => x.k === AXIS_FOR_SLOT[i])];
      tag.hidden = !(a && a.t !== 'none');
    }
  });
  setText(byId('button-count'), bindings.filter(b => b.c).length + ' of ' + slots.length + ' assigned');

  if (!byId('axis-list').children.length) renderAxes();
  state.axes.forEach((v, i) => {
    const bar = document.querySelector('[data-axis-bar="' + i + '"]');
    if (!bar) return;
    if (i >= 4) { bar.style.left = '0'; bar.style.width = (v * 100) + '%'; }
    else {
      const half = Math.abs(v) * 50;
      bar.style.left = (v < 0 ? 50 - half : 50) + '%';
      bar.style.width = half + '%';
    }
  });

  let deflection = 0;                  /* the stick furthest from center drives the dot */
  for (let i = 0; i < 4; i++)
    if (Math.abs(state.raw[i] * 2 - 1) > Math.abs(deflection)) deflection = state.raw[i] * 2 - 1;
  byId('curve-dot').setAttribute('cx', 60 + deflection * 58);
  byId('curve-dot').setAttribute('cy', 60 - shape(deflection) * 58);

  setText(byId('command-title'), selectedSlot < 0 ? 'Command' : 'Command for ' + slots[selectedSlot].n);
  setText(byId('command-current'), selectedSlot < 0 ? 'Select a control'
    : (bindings[selectedSlot] && bindings[selectedSlot].c) ? 'Now: ' + bindings[selectedSlot].c : 'Not assigned');
  byId('command-clear').disabled = selectedSlot < 0 || !(bindings[selectedSlot] && bindings[selectedSlot].c);
}

/* --------------------------------------------------------------- events */

const mapClick = e => {
  const shapeOnMap = e.target.closest('[id^="b-"]');
  if (shapeOnMap) selectSlot(+shapeOnMap.id.slice(2).replace(/\D/g, ''), true);
};
byId('controller-map').addEventListener('click', mapClick);
byId('trigger-view').addEventListener('click', mapClick);

byId('button-list').addEventListener('click', e => {
  if (e.target.closest('.repeat')) return;          /* the Repeat box has its own handler */
  const row = e.target.closest('.binding');
  if (row) selectSlot(+row.dataset.slot, true);
});
byId('button-list').addEventListener('change', e => {
  const box = e.target.closest('input[data-repeat]');
  if (box) setRepeat(+box.dataset.repeat, box.checked);
});
byId('axis-list').addEventListener('change', e => {
  const select = e.target.closest('select[data-axis]');
  if (select) {
    const i = axisSources.findIndex(x => x.k === select.dataset.axis);
    setAxis(select.dataset.axis, select.value, axisMap[i].inv);
    return;
  }
  const invert = e.target.closest('input[data-invert]');
  if (invert) {
    const i = axisSources.findIndex(x => x.k === invert.dataset.invert);
    setAxis(invert.dataset.invert, axisMap[i].t, invert.checked);
  }
});
byId('command-results').addEventListener('click', e => {
  const result = e.target.closest('.result');
  if (result) assignCommand(shownResults[+result.dataset.index].path);
});
byId('command-clear').addEventListener('click', () => assignCommand(''));

for (const key of ['deadzone', 'curve', 'gesture'])
  byId(key).addEventListener('input', e => { tune[key] = +e.target.value; drawCurve(); sendTune(); });

byId('rumble-source').addEventListener('click', e => {
  const b = e.target.closest('[data-source]');
  if (!b || b.dataset.source === rumbleSource) return;
  rumbleSource = b.dataset.source;
  markEdited('rumbleSource');
  showRumbleSource();
  post('/api/rumblesource?source=' + rumbleSource);
});
for (const [side, id] of [[0, 'rumble-left'], [1, 'rumble-right']])
  byId(id).addEventListener('input', e => {
    rumbleLevels[side] = +e.target.value;
    setText(byId(id + '-value'), bytePercent(rumbleLevels[side]));
    sendRumble();
  });
byId('rumble-v2').addEventListener('change', e => post('/api/rumblepath?v2=' + (e.target.checked ? 1 : 0)));
byId('rumble-tests').addEventListener('click', e => {
  const b = e.target.closest('[data-test]');
  if (!b) return;
  rumbleLevels = RUMBLE_TESTS[b.dataset.test].slice();
  byId('rumble-left').value = rumbleLevels[0]; byId('rumble-right').value = rumbleLevels[1];
  setText(byId('rumble-left-value'), bytePercent(rumbleLevels[0]));
  setText(byId('rumble-right-value'), bytePercent(rumbleLevels[1]));
  sendRumble();
});
byId('trigger-source').addEventListener('click', e => {
  const b = e.target.closest('[data-source]');
  if (!b || b.dataset.source === triggerSource) return;
  triggerSource = b.dataset.source;
  markEdited('triggerSource');
  showTriggerSource();
  post('/api/triggersource?source=' + triggerSource);
});
document.querySelectorAll('.effect-column').forEach(column => {
  const side = column.dataset.side === 'r' ? 1 : 0;
  column.querySelector('.effect-modes').addEventListener('click', e => {
    const b = e.target.closest('[data-effect]');
    if (!b || +b.dataset.effect === triggerManual[side].mode) return;
    const mode = +b.dataset.effect;
    triggerManual[side] = Object.assign({}, triggerManual[side], EFFECT_DEFAULTS[mode] || {}, { mode });
    renderTriggerEffects(); sendTrigger(side);
  });
  column.querySelector('.effect-params').addEventListener('input', e => {
    const input = e.target.closest('[data-param]');
    if (!input) return;
    triggerManual[side][input.dataset.param] = +input.value;
    const p = (EFFECT_PARAMS[triggerManual[side].mode] || {})[input.dataset.param];
    if (p) setText(column.querySelector('[data-value="' + input.dataset.param + '"]'), p.format(+input.value));
    sendTrigger(side);
  });
});

/* light bar */
renderSwatches(byId('light-swatches'));
renderSwatches(byId('color-swatches'));
byId('light-on').addEventListener('change', e => { light.on = e.target.checked; sendLight(); });
byId('light-swatches').addEventListener('click', e => {
  const s = e.target.closest('[data-color]');
  if (!s) return;
  [light.r, light.g, light.b] = hexToRgb(s.dataset.color);
  markCurrentSwatch(); sendLight();
});
byId('light-source').addEventListener('click', e => {
  const b = e.target.closest('[data-source]');
  if (!b) return;
  light.source = b.dataset.source === 'sim' ? 'sim' : 'manual';
  /* Solid is pattern 0, so the segment and the pattern choice are the same state */
  if (b.dataset.source !== 'sim')
    light.pattern = b.dataset.source === 'pattern' ? (light.pattern || 1) : 0;
  showLightSource(); sendLight();
});
byId('light-brightness').addEventListener('input', e => {
  light.bright = +e.target.value;
  setText(byId('light-brightness-value'), percent(light.bright));
  sendLight();
});
byId('light-patterns').addEventListener('click', e => {
  const b = e.target.closest('[data-pattern]');
  if (!b) return;
  light.pattern = +b.dataset.pattern;
  showLightSource(); sendLight();
});
byId('light-speed').addEventListener('input', e => {
  light.speed = +e.target.value;
  setText(byId('light-speed-value'), percent(light.speed));
  sendLight();
});
byId('player-lamps').addEventListener('click', e => {
  const b = e.target.closest('[data-lamps]');
  if (!b) return;
  light.player = +b.dataset.lamps;
  showLightSource(); sendLight();
});

/* color popover */
byId('light-color').addEventListener('click', () => picker.open ? closeColorPicker(false) : openColorPicker());
byId('color-ok').addEventListener('click', () => closeColorPicker(true));
byId('color-cancel').addEventListener('click', () => closeColorPicker(false));
trackPointer(byId('color-field'), (x, y) => { picker.hsv.s = x; picker.hsv.v = 1 - y; pickerChanged(); });
trackPointer(byId('color-hue'), x => { picker.hsv.h = Math.min(x * 360, 359.9); pickerChanged(); });
byId('color-swatches').addEventListener('click', e => {
  const s = e.target.closest('[data-color]');
  if (s) setPickerColor(s.dataset.color);
});
byId('color-hex').addEventListener('input', e => {
  let v = e.target.value.trim();
  if (!v.startsWith('#')) v = '#' + v;
  if (/^#[0-9a-fA-F]{6}$/.test(v)) setPickerColor(v.toUpperCase(), false);
});
byId('color-field').addEventListener('keydown', e => {
  const step = e.shiftKey ? 0.1 : 0.01, hsv = picker.hsv;
  if (e.key === 'ArrowLeft') hsv.s = clamp(hsv.s - step, 0, 1);
  else if (e.key === 'ArrowRight') hsv.s = clamp(hsv.s + step, 0, 1);
  else if (e.key === 'ArrowUp') hsv.v = clamp(hsv.v + step, 0, 1);
  else if (e.key === 'ArrowDown') hsv.v = clamp(hsv.v - step, 0, 1);
  else return;
  e.preventDefault(); pickerChanged();
});
byId('color-hue').addEventListener('keydown', e => {
  const step = e.shiftKey ? 10 : 1;
  if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') picker.hsv.h = (picker.hsv.h - step + 360) % 360;
  else if (e.key === 'ArrowRight' || e.key === 'ArrowUp') picker.hsv.h = (picker.hsv.h + step) % 360;
  else return;
  e.preventDefault(); pickerChanged();
});
byId('color-popover').addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.target.closest('button')) { e.preventDefault(); closeColorPicker(true); }
  else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closeColorPicker(false); }
});
/* a click outside the popover cancels, like Esc */
document.addEventListener('pointerdown', e => {
  if (picker.open && !e.target.closest('#color-popover') && !e.target.closest('#light-color'))
    closeColorPicker(false);
}, true);
window.addEventListener('resize', () => { if (picker.open) placePopover(byId('color-popover'), byId('light-color')); });

/* command search */
byId('command-search').addEventListener('input', () => { resultIndex = 0; renderResults(); });
byId('command-search').addEventListener('keydown', e => {
  if (e.key === 'ArrowDown')    { e.preventDefault(); resultIndex = Math.min(resultIndex + 1, shownResults.length - 1); renderResults(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); resultIndex = Math.max(resultIndex - 1, 0); renderResults(); }
  else if (e.key === 'Enter')   { e.preventDefault(); if (shownResults[resultIndex]) assignCommand(shownResults[resultIndex].path); }
  else if (e.key === 'Escape')  { byId('command-search').value = ''; resultIndex = 0; renderResults(); }
});

/* any interaction with a tuning panel suppresses press-to-select for a moment,
 * and remembers the panel so a refused change is reported next to it */
document.querySelectorAll('main section').forEach(panel => {
  panel.addEventListener('pointerdown', () => { lastPanel = panel; }, true);
  panel.addEventListener('focusin', () => { lastPanel = panel; });
  if (panel.id === 'panel-command') return;        // the command panel still jumps
  panel.addEventListener('pointerdown', () => { busyAt = Date.now(); });
  panel.addEventListener('input',       () => { busyAt = Date.now(); });
});

/* The app asks for confirmation in a native sheet and answers ok:false on Cancel. */
byId('quit').addEventListener('click', async () => {
  try {
    const r = await (await fetch('/api/quit', { method: 'POST' })).json();
    if (!r.ok) return;
  } catch (e) { /* the app is closing */ }
  stopped = true;
  byId('quit-overlay').hidden = false;
});

byId('choose-xplane').addEventListener('click', async () => {
  try {
    const r = await (await fetch('/api/pickxp', { method: 'POST' })).json();
    if (r.ok) await loadCatalog();
  } catch (e) { /* canceled */ }
});
byId('allow-input').addEventListener('click', () => post('/api/openaccess'));
const openConfig = file => post('/api/openconfig' + (file ? '?file=' + encodeURIComponent(file) : ''));
byId('open-config-folder').addEventListener('click', () => openConfig(''));
byId('config-popover-folder').addEventListener('click', () => openConfig(''));
byId('config-problems').addEventListener('click', () => { byId('config-popover').hidden = !byId('config-popover').hidden; });
byId('config-popover-done').addEventListener('click', () => { byId('config-popover').hidden = true; });
byId('config-list').addEventListener('click', e => {
  const b = e.target.closest('[data-open]');
  if (b) openConfig(b.dataset.open);
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && !byId('config-popover').hidden) byId('config-popover').hidden = true;
});

/* ----------------------------------------------------------------- boot */

async function loadCatalog() {
  const init = await (await fetch('/api/init')).json();
  commands = init.commands.map(c => ({ path: c.c, desc: c.d, pathLower: c.c.toLowerCase(),
                                       descLower: (c.d || '').toLowerCase() }));
  slots = init.slots; axisSources = init.sources; axisTargets = init.targets;
  byId('command-search').placeholder = commands.length
    ? 'Search ' + commands.length.toLocaleString() + ' X-Plane commands'
    : 'No command list yet';
  renderResults();
}

/* One poll at a time, every 50 ms: the next starts only after the last one
   finished, so slow replies can never pile up. */
async function pollLoop() {
  while (!stopped) {
    const started = Date.now();
    try { await poll(); }
    catch (e) { console.error(e); await new Promise(r => setTimeout(r, 1000)); }   /* never stop polling */
    await new Promise(r => setTimeout(r, Math.max(10, 50 - (Date.now() - started))));
  }
}

(async () => {
  for (;;) {                       /* retry until the API answers */
    try { await loadCatalog(); break; }
    catch (e) { console.error(e); await new Promise(r => setTimeout(r, 1000)); }
  }
  await poll();
  renderButtons(); renderAxes(); renderResults(); drawCurve(); renderTriggerEffects();
  showTriggerSource(); showRumbleSource(); showLightSource();
  pollLoop();
})();
