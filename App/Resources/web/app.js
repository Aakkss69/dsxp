/* DSXP mapper UI. Served by the app on dsxp://app/ - relative URLs only. */

let CMDS = [], SLOTS = [], SOURCES = [], TARGETS = [];
let sel = -1, kidx = 0, shown = [];
let bind = [], axismap = [], prevBtn = [], tune = { deadzone: 0.06, curve: 1, gesture: 0.5 };
let tuneDirty = 0, lightDirty = 0, fxDirty = 0, uiBusy = 0, fxmodeDirty = 0;
let fxmode = 'manual', rmode = 'manual';
let rumbleDirty = 0, rmodeDirty = 0;
let light = { r:0, g:90, b:255, on:true, pattern:0, speed:0.5, player:4,
              bright:1, lmode:'manual' };
let fx = [{mode:0,start:0,end:0,strength:0},{mode:0,start:0,end:0,strength:0}];
let rumble = [0, 0];                 /* the slider wins while it is being dragged */

const TRIGGER_SWEEP = 26;          /* degrees of blade travel, rest -> fully in */
const STICK_TRAVEL  = 32;          /* px the knob moves at full deflection (well r~101, cap r~50) */

const $ = s => document.querySelector(s);
const el = id => document.getElementById(id);
const esc = s => s.replace(/[&<>"]/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;' }[c]));

/* ------------------------------------------------------ command search */

function score(item, q) {
  let i = item.lp.indexOf(q);
  if (i >= 0) return 1000 - i * 2 - item.lp.length * 0.05;
  i = item.ld.indexOf(q);
  if (i >= 0) return 600 - i * 2 - item.ld.length * 0.03;
  let qi = 0;                       /* forgiving subsequence over the path */
  for (let k = 0; k < item.lp.length && qi < q.length; k++)
    if (item.lp[k] === q[qi]) qi++;
  return qi === q.length ? 300 - item.lp.length * 0.05 : -1;
}

function search(q) {
  q = q.trim().toLowerCase();
  if (!q) return CMDS.slice(0, 80);
  const out = [];
  for (const c of CMDS) { const s = score(c, q); if (s > 0) out.push([s, c]); }
  out.sort((a, b) => b[0] - a[0]);
  return out.slice(0, 100).map(x => x[1]);
}

function hl(t, q) {
  if (!q) return esc(t);
  const i = t.toLowerCase().indexOf(q.toLowerCase());
  if (i < 0) return esc(t);
  return esc(t.slice(0, i)) + '<mark>' + esc(t.slice(i, i + q.length)) + '</mark>'
       + esc(t.slice(i + q.length));
}

function renderResults() {
  const box = $('#results');
  if (sel < 0) {
    box.innerHTML = '<div class="empty">Pick a control on the map, or just press it on the pad.</div>';
    return;
  }
  const q = $('#q').value.trim();
  shown = search(q);
  if (!shown.length) {
    box.innerHTML = '<div class="empty">Nothing matches &ldquo;' + esc(q) + '&rdquo;</div>';
    return;
  }
  if (kidx >= shown.length) kidx = 0;
  box.innerHTML = shown.map((c, i) =>
    '<div class="res' + (i === kidx ? ' k' : '') + '" data-i="' + i + '">' +
      '<div class="p">' + hl(c.c, q) + '</div>' +
      (c.d ? '<div class="d">' + hl(c.d, q) + '</div>' : '') +
    '</div>').join('');
  const k = box.querySelector('.res.k');
  if (k && kidx > 0) k.scrollIntoView({ block: 'nearest' });
}

/* ------------------------------------------------------------ mutations */

/* Every change is a POST. The poll loop picks up the new state within 50 ms;
   a refused change (e.g. 409 while the sim drives the triggers) is shown
   briefly instead of being silently swallowed. */
let toastTimer = 0;
function flash(msg) {
  const t = el('toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 2500);
}
const post = url => fetch(url, { method: 'POST' })
  .then(r => { if (!r.ok) return r.text().then(t => flash(t || ('error ' + r.status))); })
  .catch(() => {});

function assign(cmd) {
  if (sel < 0) return;
  const rep = bind[sel] ? bind[sel].r : 0;
  post('/api/bind?i=' + sel + '&repeat=' + rep + '&cmd=' + encodeURIComponent(cmd));
}
function toggleRepeat(i, ev) {
  ev.stopPropagation();
  const b = bind[i];
  if (!b || !b.c) return;
  post('/api/bind?i=' + i + '&repeat=' + (b.r ? 0 : 1) + '&cmd=' + encodeURIComponent(b.c));
}
const setAxis = (k, t, inv) => post('/api/axis?k=' + k + '&t=' + t + '&inv=' + (inv ? 1 : 0));
function pushTune() {
  tuneDirty = Date.now();
  post('/api/tune?deadzone=' + tune.deadzone + '&curve=' + tune.curve
       + '&gesture=' + tune.gesture);
}
const hex2rgb = h => [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)];
const rgb2hex = (r,g,b) => '#' + [r,g,b].map(v => v.toString(16).padStart(2,'0')).join('');
function pushLight() {
  lightDirty = Date.now();
  post('/api/light?r=' + light.r + '&g=' + light.g + '&b=' + light.b
     + '&on=' + (light.on ? 1 : 0) + '&pattern=' + light.pattern
     + '&speed=' + light.speed + '&player=' + light.player
     + '&bright=' + light.bright + '&lmode=' + light.lmode);
}

const PAT_NAME = ['solid','breathe','pulse','strobe','rainbow','alert'];

function applyLightMode() {
  const isSim = light.lmode === 'sim';
  const tab = isSim ? 'sim' : (light.pattern !== 0 ? 'pattern' : 'solid');
  document.querySelectorAll('#ltabs button').forEach(b =>
    b.classList.toggle('on', b.dataset.lt === tab));
  el('lsolid').hidden   = tab !== 'solid';
  el('lpattern').hidden = tab !== 'pattern';
  el('lsim').hidden     = tab !== 'sim';
  document.querySelectorAll('.patrow button').forEach(b =>
    b.classList.toggle('on', +b.dataset.pat === light.pattern));
  document.querySelectorAll('.pleds button').forEach(b =>
    b.classList.toggle('on', +b.dataset.pl === light.player));
  el('lnote').innerHTML = isSim
    ? 'The bar becomes an annunciator, ordered by urgency &mdash; crash strobes red, '
    + 'stall pulses red, stall warning breathes amber, over 2.5 g holds amber, '
    + 'turbulence breathes cyan, otherwise steady green. Read-only while Sim is on.'
    : tab === 'pattern'
    ? 'The pattern animates your chosen colour &mdash; switch to Solid to change it. '
    + 'Refreshed at 20 Hz, which is as fast as the bar resolves, so the Bluetooth '
    + 'link stays free for input.'
    : 'Written straight to the controller as an output report. macOS re-asserts '
    + 'its own colour, so this is held with a keepalive.';
}

function select(i, focusSearch) {
  sel = i; kidx = 0;
  renderResults();
  if (!focusSearch) return;
  /* focus({preventScroll}) alone is not enough - input.select() scrolls the box
   * into view on its own, which is what dragged the page around when a trigger
   * moved. Only do either when the user actually clicked something. */
  $('#q').focus({ preventScroll: true });
  $('#q').select();
}

/* Parameters mean different things per mode, so the labels follow the mode
 * instead of showing three generic sliders. null = unused by that mode. */
/* The wire format is a raw 0-255 byte per parameter, which tells you nothing on
 * screen. Position bytes are a fraction of trigger travel and force bytes a
 * fraction of maximum, so both read as %. The vibrate rate byte is a frequency,
 * so it reads as Hz over a range you would actually use. */
const PCT = { max: 255, step: 1,  fmt: v => Math.round(v / 255 * 100) + '%' };
const HZ  = { max: 60,  step: 1,  fmt: v => v + ' Hz' };
const FX_PARAMS = {
  0: {},
  1: { start:    { label: 'Begins at',   ...PCT },
       strength: { label: 'Force',       ...PCT } },
  2: { start:    { label: 'Band start',  ...PCT },
       end:      { label: 'Band end',    ...PCT },
       strength: { label: 'Break force', ...PCT } },
  3: { start:    { label: 'Threshold',   ...PCT },
       strength: { label: 'Amplitude',   ...PCT },
       end:      { label: 'Rate',        ...HZ  } },
};
const FX_DEFAULTS = {
  1: { start: 70,  end: 0,   strength: 200 },
  2: { start: 60,  end: 160, strength: 255 },
  3: { start: 40,  end: 40,  strength: 180 },
};

/* Syncs the trigger panel to `fx`. It only renders: the listeners are attached
   once, at the bottom of this file. (The prototype re-attached five listeners
   here on every 50 ms poll, so one click fired dozens of requests.) */
function renderFx() {
  document.querySelectorAll('.fxcol').forEach(col => {
    const i = col.dataset.side === 'r' ? 1 : 0;
    const f = fx[i];
    col.querySelectorAll('.modes button').forEach(b =>
      b.classList.toggle('on', +b.dataset.m === f.mode));
    const spec = FX_PARAMS[f.mode] || {};
    const box = col.querySelector('.fxparams');
    const want = Object.keys(spec).join(',') + '|' + f.mode;
    if (box.dataset.sig !== want) {
      box.dataset.sig = want;
      box.innerHTML = Object.entries(spec).map(([k, p]) =>
        '<label>' + p.label + '</label>' +
        '<input type="range" data-p="' + k + '" min="0" max="' + p.max +
        '" step="' + p.step + '" value="' + f[k] + '">' +
        '<output data-o="' + k + '" title="raw byte ' + f[k] + '">' +
        p.fmt(f[k]) + '</output>').join('') ||
        '<label class="off">free travel, no resistance</label><span></span><span></span>';
    }
    Object.entries(spec).forEach(([k, p]) => {
      const inp = box.querySelector('[data-p="' + k + '"]');
      const out = box.querySelector('[data-o="' + k + '"]');
      if (inp && document.activeElement !== inp) inp.value = f[k];
      if (out) { out.textContent = p.fmt(f[k]); out.title = 'raw byte ' + f[k]; }
    });
  });
}

function applyFxMode() {
  document.querySelectorAll('.fxtabs button').forEach(b =>
    b.classList.toggle('on', b.dataset.fxm === fxmode));
  el('fxmanual').hidden = fxmode !== 'manual';
  el('fxwind').hidden   = fxmode !== 'wind';
  el('fxnote').innerHTML = fxmode === 'wind'
    ? 'Resistance follows dynamic pressure, so it rises with the <b>square</b> of '
    + 'indicated airspeed &mdash; the same law that loads a real rudder pedal. '
    + 'Crosswind biases the two triggers apart; turbulence above 1.5/10 takes over '
    + 'as buffet. Read-only while wind mode is on.'
    : '<b>Resist</b> holds a constant force past a point. <b>Weapon</b> resists '
    + 'across a band then breaks through, like a gun trigger. <b>Vibrate</b> buzzes '
    + 'once you press past the threshold. Positions are % of trigger travel, '
    + 'forces are % of maximum.';
}

/* Motor bytes read as % of full scale. The left motor is the heavy one, so the
 * same byte feels stronger there - that asymmetry is real, not a bug. */
const RUMBLE_TESTS = {
  soft:  [70, 70],   hard:  [200, 200],
  left:  [220, 40],  right: [40, 220],
  crash: [255, 255], stop:  [0, 0],
};

function applyRumbleMode() {
  document.querySelectorAll('#rtabs button').forEach(b =>
    b.classList.toggle('on', b.dataset.rm === rmode));
  el('rmanual').hidden = rmode !== 'manual';
  el('rsim').hidden    = rmode !== 'sim';
  el('rnote').innerHTML = rmode === 'sim'
    ? 'Driven by the flight model. Per-wing stall comes from '
    + '<b>element_is_stalled</b>, so one wing dropping buzzes that side harder &mdash; '
    + 'the cue that tells you which way it is about to go. Load factor past 2.5 g adds '
    + 'a symmetric jolt, and a crash saturates both. Read-only while sim mode is on.'
    : 'Drive the motors by hand to feel the range. The left motor is the heavy one, '
    + 'so equal values feel stronger on the left.';
}

function pushRumble() {
  rumbleDirty = Date.now();
  post('/api/rumble?left=' + rumble[0] + '&right=' + rumble[1]);
}

function pushFx(i) {
  fxDirty = Date.now();
  const f = fx[i];
  post('/api/trigger?side=' + (i ? 'r' : 'l') + '&mode=' + f.mode +
       '&start=' + f.start + '&end=' + f.end + '&strength=' + f.strength);
}

/* ------------------------------------------------------------ rendering */

/* L2/R2 carry an analog axis as well as a digital click, so their rows are
 * tagged - otherwise "unassigned" reads as if the trigger does nothing. */
const AXIS_SLOT = { 14: 'l2', 15: 'r2' };

function renderButtons() {
  $('#btns').innerHTML = SLOTS.map(s => {
    const b = bind[s.i] || { c: '', r: 0 };
    const axKey = AXIS_SLOT[s.i];
    const ax = axKey ? axismap[SOURCES.findIndex(x => x.k === axKey)] : null;
    const axTag = ax && ax.t !== 'none'
      ? '<div class="axtag" title="analog axis assigned under Axes">AXIS</div>' : '';
    return '<div class="row' + (b.c ? '' : ' empty-slot') + '" data-i="' + s.i + '">' +
      '<div class="nm">' + esc(s.n) + '</div>' +
      '<div class="bd' + (b.c ? '' : ' none') + '">' + esc(b.c || 'unassigned') + '</div>' +
      axTag +
      (b.c ? '<div class="rep' + (b.r ? ' on' : '') + '" data-rep="' + s.i +
             '" title="fire once per press, or repeat while held">' +
             (b.r ? 'HOLD' : 'ONCE') + '</div>' : '') +
    '</div>';
  }).join('');
}

function renderAxes() {
  $('#axes').innerHTML = SOURCES.map((s, i) => {
    const a = axismap[i] || { t: 'none', inv: 0 };
    return '<div class="axrow">' +
      '<div class="nm">' + esc(s.n) + '</div>' +
      '<div class="bar"><i data-bar="' + i + '"></i></div>' +
      '<select data-ax="' + s.k + '">' + TARGETS.map(t =>
        '<option value="' + t.k + '"' + (t.k === a.t ? ' selected' : '') + '>' +
        esc(t.n) + '</option>').join('') + '</select>' +
      '<div class="inv' + (a.inv ? ' on' : '') + '" data-inv="' + s.k +
        '" title="invert this axis">INV</div>' +
    '</div>';
  }).join('');
}

/* Keep the axis pickers and INV pills in step with the bridge. renderAxes()
   only builds the rows once, so without this an INV click never showed. */
function syncAxes() {
  document.querySelectorAll('#axes .axrow').forEach((row, i) => {
    const a = axismap[i];
    if (!a) return;
    const s = row.querySelector('select[data-ax]');
    if (s && document.activeElement !== s && s.value !== a.t) s.value = a.t;
    const inv = row.querySelector('.inv');
    if (inv) inv.classList.toggle('on', !!a.inv);
  });
}

/* The app strip: Input Monitoring state, X-Plane folder, command count. */
let lastCmds = -1;
function renderApp(app) {
  const acc = el('app-access');
  acc.hidden = app.access === 'granted' || app.input_live || app.access === undefined;
  const dir = el('app-xpdir');
  dir.textContent = app.xpdir ? app.xpdir.replace(/^\/Users\/[^/]+/, '~') : 'X-Plane folder not found';
  dir.title = app.xpdir || 'Choose your X-Plane 12 folder to search its commands';
  dir.classList.toggle('missing', !app.xpdir);
  el('app-imported').hidden = !app.imported;
  if (typeof app.cmds === 'number' && app.cmds !== lastCmds) {
    if (lastCmds >= 0) loadInit();            /* catalogue changed: reload it */
    lastCmds = app.cmds;
  }
}

/* mirrors shape() in Core/src/decode.c so the preview matches what is sent */
function shaped(v) {
  const dz = tune.deadzone, c = tune.curve;
  const s = v < 0 ? -1 : 1, a0 = Math.abs(v);
  if (dz > 0 && a0 <= dz) return 0;
  let a = dz > 0 ? (a0 - dz) / (1 - dz) : a0;
  if (c !== 1) a = Math.pow(a, c);
  return s * a;
}

function drawCurve() {
  const pts = [];
  for (let i = 0; i <= 60; i++) {
    const x = -1 + (2 * i) / 60;
    pts.push((60 + x * 58).toFixed(1) + ',' + (60 - shaped(x) * 58).toFixed(1));
  }
  $('#curveline').setAttribute('points', pts.join(' '));
  $('#dz-v').textContent = tune.deadzone.toFixed(3);
  $('#cv-v').textContent = tune.curve.toFixed(2);
  $('#gs-v').textContent = (tune.gesture ?? 0.5).toFixed(2);
}

/* Side-view gauge: the blade pivots through its real travel, so a glance tells
 * you which trigger is down and by how much. */
function drawTrigger(side, v) {
  const lever = el('trig-' + side + '-lever');
  lever.setAttribute('transform',
    'translate(66 66) rotate(' + (-v * TRIGGER_SWEEP) + ')');
  el('trig-' + side + '-pct').textContent = Math.round(v * 100) + '%';
  el('trig-' + side + '-bar').setAttribute('width', 120 * v);
}

const pill = (e, ok, txt) => { e.className = 'pill ' + (ok ? 'on' : 'off'); e.textContent = txt; };

/* ---------------------------------------------------------- live poll */

async function poll() {
  if (stopped) return;
  let st;
  try { st = await (await fetch('/api/state')).json(); } catch (e) { return; }
  bind = st.bind; axismap = st.axismap;
  syncAxes();
  renderApp(st.app || {});

  pill($('#g-bt'),  st.gate.bt,      'bluetooth '  + (st.gate.bt  ? 'yes' : 'no'));
  pill($('#g-usb'), !st.gate.usb,    'usb '        + (st.gate.usb ? 'connected' : 'clear'));
  pill($('#g-xp'),  st.gate.xp,      'x-plane 12 ' + (st.gate.xp  ? 'running' : 'closed'));
  pill($('#g-eng'), st.gate.engaged, st.gate.engaged ? 'engaged' : 'idle');
  const rep = $('#g-rep');
  rep.textContent = st.reports.toLocaleString() + ' / 10s';
  rep.title = 'reports this 10-second window · ' + st.total.toLocaleString()
            + ' total since start · flushed to logs/';
  $('#mapstatus').textContent = st.connected ? 'live · report 0x31' : 'waiting for report 0x31';

  if (Date.now() - tuneDirty > 600) {
    tune = st.tune;
    if (document.activeElement !== $('#dz')) $('#dz').value = tune.deadzone;
    if (document.activeElement !== $('#cv')) $('#cv').value = tune.curve;
    if (document.activeElement !== $('#gs')) $('#gs').value = tune.gesture;
    drawCurve();
  }

  if (Date.now() - fxDirty > 600 && st.fx) { fx = st.fx; renderFx(); }
  if (Date.now() - fxmodeDirty > 600 && st.fxmode && st.fxmode !== fxmode) {
    fxmode = st.fxmode; applyFxMode();
  }
  const MODE_NAME = ['off','resist','weapon','vibrate'];
  $('#fxstatus').textContent = fxmode === 'wind'
    ? (st.wx && st.wx.live ? 'wind · live' : 'wind · no sim data')
    : MODE_NAME[fx[0].mode] + ' · ' + MODE_NAME[fx[1].mode];

  if (Date.now() - rmodeDirty > 600 && st.rmode && st.rmode !== rmode) {
    rmode = st.rmode; applyRumbleMode();
  }
  $('#rstatus').textContent = st.rnote || rmode;

  if (st.rumble && document.activeElement !== el('rv2')) el('rv2').checked = !!st.rumble.v2;
  if (rmode === 'manual' && Date.now() - rumbleDirty > 600 && st.rumble) {
    rumble = [st.rumble.left, st.rumble.right];
    if (document.activeElement !== el('r-l')) el('r-l').value = rumble[0];
    if (document.activeElement !== el('r-r')) el('r-r').value = rumble[1];
    el('r-lv').textContent = Math.round(rumble[0] / 255 * 100) + '%';
    el('r-rv').textContent = Math.round(rumble[1] / 255 * 100) + '%';
  }
  if (rmode === 'sim' && st.wx) {
    const w = st.wx, pct = v => Math.round(v * 100) + '%';
    el('rx-sl').textContent = w.live ? pct(w.stall_l) : '—';
    el('rx-sr').textContent = w.live ? pct(w.stall_r) : '—';
    el('rx-sw').textContent = w.live ? pct(w.stall) : '—';
    el('rx-gn').textContent = w.live ? w.gn.toFixed(2) + ' g' : '—';
    el('rx-crash').textContent = w.live ? (w.crashed ? 'YES' : 'no') : '—';
    el('rx-crash').classList.toggle('hit', !!w.crashed);
    el('rx-note').textContent = st.rnote || '—';
    el('rx-note').classList.toggle('stale', !w.live);
    el('rx-l').textContent = st.rumble ? pct(st.rumble.left / 255) : '—';
    el('rx-r').textContent = st.rumble ? pct(st.rumble.right / 255) : '—';
  }

  if (fxmode === 'wind' && st.wx) {
    const w = st.wx, n = (v, u, d = 0) => v.toFixed(d) + u;
    el('wx-ias').textContent  = w.live ? n(w.ias, ' kt') : '—';
    el('wx-wind').textContent = w.live ? n(w.wind, ' m/s') + ' from ' + n(w.dir, '°') : '—';
    el('wx-xw').textContent   = w.live
      ? Math.abs(w.xwind).toFixed(1) + ' m/s ' + (w.xwind >= 0 ? 'from right' : 'from left') : '—';
    el('wx-turb').textContent = w.live ? w.turb.toFixed(1) + ' / 10' : '—';
    el('wx-load').textContent = w.live ? Math.round(w.load * 100) + '%' : '—';
    el('wx-note').textContent = w.note || '—';
    el('wx-note').classList.toggle('stale', !w.live);
    const desc = f => f.mode === 0 ? 'free'
      : f.mode === 3 ? 'buzz ' + Math.round(f.strength / 255 * 100) + '% @ ' + f.end + ' Hz'
      : Math.round(f.strength / 255 * 100) + '% force';
    el('wx-l').textContent = desc(fx[0]);
    el('wx-r').textContent = desc(fx[1]);
  }

  if (Date.now() - lightDirty > 600 && st.light) {
    light = st.light;
    const hex = rgb2hex(light.r, light.g, light.b);
    if (document.activeElement !== $('#lc')) $('#lc').value = hex;
    $('#lon').checked = !!light.on;
    if (document.activeElement !== el('lspeed')) el('lspeed').value = light.speed;
    el('lspeed-v').textContent = light.speed.toFixed(2);
    if (document.activeElement !== el('lbright')) el('lbright').value = light.bright;
    el('lbright-v').textContent = Math.round(light.bright * 100) + '%';
    if (light.lmode === 'sim') {
      el('lx-note').textContent = light.lnote || '—';
      el('lx-col').textContent = rgb2hex(light.r, light.g, light.b);
    }
    $('#lightstatus').textContent = !light.on ? 'off'
      : light.lmode === 'sim' ? 'sim · ' + (light.lnote || '')
      : (light.pattern ? PAT_NAME[light.pattern] + ' · ' + hex : hex);
    applyLightMode();
  }

  /* live touch points over the traced touchpad (bbox 394,11 522x281) */
  if (st.touch) st.touch.forEach((t, i) => {
    const d = el('touch-' + i);
    if (!d) return;
    if (t.on) { d.setAttribute('cx', 394 + t.x * 522); d.setAttribute('cy', 11 + t.y * 281); }
    else      { d.setAttribute('cx', -99); d.setAttribute('cy', -99); }
  });

  /* Physically pressing a control jumps to its binding - but not while you are
   * working a slider or picker, otherwise squeezing a trigger to feel an effect
   * yanks focus into the search box mid-drag. */
  const af = document.activeElement;
  const busyElsewhere = (af && af !== $('#q') &&
      /^(INPUT|SELECT|BUTTON|TEXTAREA)$/.test(af.tagName))
    || Date.now() - uiBusy < 1500;
  /* Everything selects on press, triggers included - the earlier jumping was the
   * viewport chasing focus, which preventScroll in select() fixes. The only
   * thing worth protecting is a search already in progress. */
  const typing = af === $('#q') && $('#q').value.trim() !== '';
  if (!busyElsewhere && !typing)
    for (let i = 0; i < st.btn.length; i++)
      if (st.btn[i] && !prevBtn[i]) select(i, false);
  prevBtn = st.btn.slice();

  for (let i = 0; i < st.btn.length; i++) {
    const e = el('b-' + i);
    if (!e) continue;
    e.classList.toggle('down', !!st.btn[i]);
    e.classList.toggle('sel', sel === i);
  }
  /* a diagonal lights its two adjacent arms too, the way the pad actually feels */
  const adj = { 1: [0, 2], 3: [2, 4], 5: [4, 6], 7: [6, 0] };
  for (const d in adj)
    if (st.btn[d]) adj[d].forEach(a => el('b-' + a).classList.add('down'));
  /* the shoulder shown in the side view mirrors the one on the pad */
  el('b-12s').classList.toggle('down', !!st.btn[12]);
  el('b-13s').classList.toggle('down', !!st.btn[13]);

  /* sticks follow the shaped values */
  const L = el('stick-l'), R = el('stick-r');
  /* the traced caps are paths, so they move by transform rather than cx/cy */
  L.setAttribute('transform', 'translate(' + (st.axes[0] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (st.axes[1] * STICK_TRAVEL).toFixed(1) + ')');
  R.setAttribute('transform', 'translate(' + (st.axes[2] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (st.axes[3] * STICK_TRAVEL).toFixed(1) + ')');

  /* triggers: raw travel, so the gauge shows the physical squeeze even when the
   * deadzone is swallowing it downstream */
  drawTrigger('l', st.raw[4]);
  drawTrigger('r', st.raw[5]);
  $('#trigstatus').textContent =
    st.raw[4] > 0.02 || st.raw[5] > 0.02
      ? 'L2 ' + Math.round(st.raw[4] * 100) + '%  ·  R2 ' + Math.round(st.raw[5] * 100) + '%'
      : 'analog — side view';

  if ($('#btns').children.length !== SLOTS.length) renderButtons();
  [...$('#btns').children].forEach((e, i) => {
    e.classList.toggle('down', !!st.btn[i]);
    e.classList.toggle('sel', sel === i);
    const b = bind[i] || { c: '', r: 0 };
    const bd = e.querySelector('.bd');
    const want = b.c || 'unassigned';
    if (bd.textContent !== want) { bd.textContent = want; bd.className = 'bd' + (b.c ? '' : ' none'); }
    const rp = e.querySelector('.rep');
    if (rp) { rp.className = 'rep' + (b.r ? ' on' : ''); rp.textContent = b.r ? 'HOLD' : 'ONCE'; }
  });
  $('#bindcount').textContent = bind.filter(b => b.c).length + ' of ' + SLOTS.length + ' assigned';

  if (!$('#axes').children.length) renderAxes();
  st.axes.forEach((v, i) => {
    const bar = document.querySelector('[data-bar="' + i + '"]');
    if (!bar) return;
    if (i >= 4) { bar.style.left = '0'; bar.style.width = (v * 100) + '%'; }
    else {
      const pct = Math.abs(v) * 50;
      bar.style.left = (v < 0 ? 50 - pct : 50) + '%';
      bar.style.width = pct + '%';
    }
  });

  let drive = 0;
  for (let i = 0; i < 4; i++)
    if (Math.abs(st.raw[i] * 2 - 1) > Math.abs(drive)) drive = st.raw[i] * 2 - 1;
  $('#curvedot').setAttribute('cx', 60 + drive * 58);
  $('#curvedot').setAttribute('cy', 60 - shaped(drive) * 58);

  $('#target').textContent = sel < 0 ? 'Select a control'
    : 'Assigning: ' + SLOTS[sel].n + '  →  ' + ((bind[sel] && bind[sel].c) || 'unassigned');
}

/* --------------------------------------------------------------- events */

const padClick = e => {
  const t = e.target.closest('[id^="b-"]');
  if (t) select(+t.id.slice(2).replace(/\D/g, ''), true);
};
$('#pad').addEventListener('click', padClick);
$('#trigview').addEventListener('click', padClick);

$('#btns').addEventListener('click', e => {
  const rep = e.target.closest('[data-rep]');
  if (rep) { toggleRepeat(+rep.dataset.rep, e); return; }
  const row = e.target.closest('.row');
  if (row) select(+row.dataset.i, true);
});
$('#axes').addEventListener('change', e => {
  const s = e.target.closest('[data-ax]');
  if (!s) return;
  const i = SOURCES.findIndex(x => x.k === s.dataset.ax);
  setAxis(s.dataset.ax, s.value, axismap[i].inv);
});
$('#axes').addEventListener('click', e => {
  const t = e.target.closest('[data-inv]');
  if (!t) return;
  const i = SOURCES.findIndex(x => x.k === t.dataset.inv);
  setAxis(t.dataset.inv, axismap[i].t, !axismap[i].inv);
});
$('#results').addEventListener('click', e => {
  const r = e.target.closest('.res');
  if (r) assign(shown[+r.dataset.i].c);
});
$('#unbind').addEventListener('click', e => { e.preventDefault(); assign(''); });

$('#dz').addEventListener('input', e => { tune.deadzone = +e.target.value; drawCurve(); pushTune(); });
$('#cv').addEventListener('input', e => { tune.curve    = +e.target.value; drawCurve(); pushTune(); });
$('#gs').addEventListener('input', e => { tune.gesture  = +e.target.value; drawCurve(); pushTune(); });
$('#rtabs').addEventListener('click', e => {
  const b = e.target.closest('[data-rm]');
  if (!b) return;
  rmode = b.dataset.rm;
  rmodeDirty = Date.now();
  applyRumbleMode();
  post('/api/rumblemode?mode=' + rmode);
});
$('.rparams').addEventListener('input', e => {
  const i = e.target.id === 'r-r' ? 1 : 0;
  rumble[i] = +e.target.value;
  el(i ? 'r-rv' : 'r-lv').textContent = Math.round(rumble[i] / 255 * 100) + '%';
  pushRumble();
});
$('#rv2').addEventListener('change', e => post('/api/rumblepath?v2=' + (e.target.checked ? 1 : 0)));
$('.rtests').addEventListener('click', e => {
  const b = e.target.closest('[data-test]');
  if (!b) return;
  rumble = RUMBLE_TESTS[b.dataset.test].slice();
  el('r-l').value = rumble[0]; el('r-r').value = rumble[1];
  el('r-lv').textContent = Math.round(rumble[0] / 255 * 100) + '%';
  el('r-rv').textContent = Math.round(rumble[1] / 255 * 100) + '%';
  pushRumble();
});
document.querySelector('.fxtabs').addEventListener('click', e => {
  const b = e.target.closest('[data-fxm]');
  if (!b) return;
  fxmode = b.dataset.fxm;
  fxmodeDirty = Date.now();
  applyFxMode();
  post('/api/fxmode?mode=' + fxmode);
});
document.querySelectorAll('.fxcol').forEach(col => {
  const i = col.dataset.side === 'r' ? 1 : 0;
  col.querySelector('.modes').addEventListener('click', e => {
    const b = e.target.closest('[data-m]');
    if (!b) return;
    const m = +b.dataset.m;
    fx[i] = Object.assign({ start: 0, end: 0, strength: 0 }, FX_DEFAULTS[m] || {}, { mode: m });
    renderFx(); pushFx(i);
  });
  col.querySelector('.fxparams').addEventListener('input', e => {
    const inp = e.target.closest('[data-p]');
    if (!inp) return;
    fx[i][inp.dataset.p] = +inp.value;
    const p = (FX_PARAMS[fx[i].mode] || {})[inp.dataset.p];
    const out = col.querySelector('[data-o="' + inp.dataset.p + '"]');
    if (out && p) { out.textContent = p.fmt(+inp.value); out.title = 'raw byte ' + inp.value; }
    pushFx(i);
  });
});
const setHex = h => { [light.r, light.g, light.b] = hex2rgb(h); };
$('#lc').addEventListener('input', e => { setHex(e.target.value); pushLight(); });
$('#lon').addEventListener('change', e => { light.on = e.target.checked; pushLight(); });
document.querySelector('.swatches').addEventListener('click', e => {
  const b = e.target.closest('[data-c]');
  if (!b) return;
  $('#lc').value = b.dataset.c; setHex(b.dataset.c); pushLight();
});
$('#ltabs').addEventListener('click', e => {
  const b = e.target.closest('[data-lt]');
  if (!b) return;
  light.lmode = b.dataset.lt === 'sim' ? 'sim' : 'manual';
  /* Solid is pattern 0, so the tab and the pattern choice are the same state */
  if (b.dataset.lt !== 'sim')
    light.pattern = b.dataset.lt === 'pattern' ? (light.pattern || 1) : 0;
  applyLightMode(); pushLight();
});
el('lbright').addEventListener('input', e => {
  light.bright = +e.target.value;
  el('lbright-v').textContent = Math.round(light.bright * 100) + '%';
  pushLight();
});
$('.patrow').addEventListener('click', e => {
  const b = e.target.closest('[data-pat]');
  if (!b) return;
  light.pattern = +b.dataset.pat;
  applyLightMode(); pushLight();
});
el('lspeed').addEventListener('input', e => {
  light.speed = +e.target.value;
  el('lspeed-v').textContent = light.speed.toFixed(2);
  pushLight();
});
$('.pleds').addEventListener('click', e => {
  const b = e.target.closest('[data-pl]');
  if (!b) return;
  light.player = +b.dataset.pl;
  applyLightMode(); pushLight();
});

$('#q').addEventListener('input', () => { kidx = 0; renderResults(); });
$('#q').addEventListener('keydown', e => {
  if (e.key === 'ArrowDown')    { e.preventDefault(); kidx = Math.min(kidx + 1, shown.length - 1); renderResults(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); kidx = Math.max(kidx - 1, 0); renderResults(); }
  else if (e.key === 'Enter')   { e.preventDefault(); if (shown[kidx]) assign(shown[kidx].c); }
  else if (e.key === 'Escape')  { $('#q').value = ''; kidx = 0; renderResults(); }
});

/* any interaction with a tuning panel suppresses press-to-select for a moment */
document.querySelectorAll('section').forEach(sec => {
  if (sec.querySelector('#target')) return;      // the assign panel still jumps
  sec.addEventListener('pointerdown', () => { uiBusy = Date.now(); });
  sec.addEventListener('input',       () => { uiBusy = Date.now(); });
});

let stopped = false;
$('#quit').addEventListener('click', async () => {
  if (!confirm('Quit DSXP? X-Plane gets its controls back and the controller goes quiet.'))
    return;
  stopped = true;
  el('gone').hidden = false;
  try { await fetch('/api/quit', { method: 'POST' }); } catch (e) { /* the app is closing */ }
});

el('app-pickxp').addEventListener('click', async () => {
  try {
    const r = await (await fetch('/api/pickxp', { method: 'POST' })).json();
    if (r.ok) await loadInit();
  } catch (e) { /* cancelled */ }
});
el('app-access').addEventListener('click', () => post('/api/openaccess'));

/* ----------------------------------------------------------------- boot */

async function loadInit() {
  const init = await (await fetch('/api/init')).json();
  CMDS = init.commands.map(c => ({ c: c.c, d: c.d, lp: c.c.toLowerCase(), ld: (c.d || '').toLowerCase() }));
  SLOTS = init.slots; SOURCES = init.sources; TARGETS = init.targets;
  $('#q').placeholder = CMDS.length
    ? 'Search ' + CMDS.length.toLocaleString() + ' X-Plane commands…'
    : 'No X-Plane command list yet - choose your X-Plane folder above';
  renderResults();
}

/* One poll at a time, every 50 ms: the next starts only after the last one
   finished, so slow replies can never pile up. */
async function pollLoop() {
  while (!stopped) {
    const t0 = Date.now();
    await poll();
    await new Promise(r => setTimeout(r, Math.max(10, 50 - (Date.now() - t0))));
  }
}

(async () => {
  await loadInit();
  await poll();
  renderButtons(); renderAxes(); renderResults(); drawCurve(); renderFx(); applyFxMode(); applyRumbleMode(); applyLightMode();
  pollLoop();
})();
