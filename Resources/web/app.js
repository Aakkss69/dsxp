/* DSXP mapper UI. Served by the app on dsxp://app/ - relative URLs only.
 * Wording follows Apple's guidance: title-style buttons and segments,
 * sentence-style labels, status and notes, US English. */

let CMDS = [], SLOTS = [], SOURCES = [], TARGETS = [];
let sel = -1, kidx = 0, shown = [];
let bind = [], axismap = [], prevBtn = [], tune = { deadzone: 0.06, curve: 1, gesture: 0.5 };
let tuneDirty = 0, lightDirty = 0, fxDirty = 0, uiBusy = 0, fxmodeDirty = 0;
let fxmode = 'manual', rmode = 'manual';
let rumbleDirty = 0, rmodeDirty = 0;
let light = { r:0, g:90, b:255, on:true, pattern:0, speed:0.5, player:4,
              bright:1, lmode:'manual' };
let fx = [{mode:0,start:70,end:160,strength:200,freq:40},{mode:0,start:70,end:160,strength:200,freq:40}];
let rumble = [0, 0];                 /* the slider wins while it is being dragged */
let lastSection = null;              /* the panel the user last worked in, for errors */

const TRIGGER_SWEEP = 26;          /* degrees of blade travel, rest -> fully in */
const STICK_TRAVEL  = 32;          /* px the knob moves at full deflection (well r~101, cap r~50) */
const MS_TO_KT = 1.943844;         /* X-Plane sends wind in m/s; pilots read knots */

const $ = s => document.querySelector(s);
const el = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;' }[c]));
const pct = v => Math.round(v * 100) + '%';
const bytePct = v => Math.round(v / 255 * 100) + '%';
const setText = (e, t) => { if (e && e.textContent !== t) e.textContent = t; };

/* a segmented control: exactly one segment on */
function segSelect(root, pick) {
  root.querySelectorAll('button').forEach(b => {
    const on = pick(b);
    b.classList.toggle('on', on);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
  });
}

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
    box.innerHTML = '<div class="empty">Select a control on the map or in the list, '
                  + 'or press it on the controller.</div>';
    return;
  }
  const q = $('#q').value.trim();
  shown = search(q);
  if (!shown.length) {
    box.innerHTML = '<div class="empty">' + (CMDS.length
      ? 'No commands match &ldquo;' + esc(q) + '&rdquo;.'
      : 'No command list yet. Choose your X-Plane folder at the top of the window.') + '</div>';
    return;
  }
  if (kidx >= shown.length) kidx = 0;
  box.innerHTML = shown.map((c, i) =>
    '<div class="res' + (i === kidx ? ' k' : '') + '" role="option" data-i="' + i + '"'
      + (i === kidx ? ' aria-selected="true"' : '') + '>' +
      '<div class="p">' + hl(c.c, q) + '</div>' +
      (c.d ? '<div class="d">' + hl(c.d, q) + '</div>' : '') +
    '</div>').join('');
  const k = box.querySelector('.res.k');
  if (k && kidx > 0) k.scrollIntoView({ block: 'nearest' });
}

/* ------------------------------------------------------------ mutations */

/* A short neutral notice at the bottom of the window. */
let toastTimer = 0;
function toast(msg) {
  const t = el('toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3000);
}

/* A refused change (e.g. 409 while the simulator drives the triggers) is shown
   under the panel it came from, not swallowed. */
function panelError(msg) {
  const sec = lastSection || $('main section');
  let p = sec.querySelector('.inlineerr');
  if (!p) {
    p = document.createElement('p');
    p.className = 'inlineerr';
    p.setAttribute('role', 'alert');
    sec.appendChild(p);
  }
  p.innerHTML = '<i class="dot warn"></i>' + esc(msg);
  p.hidden = false;
  clearTimeout(p._t);
  p._t = setTimeout(() => { p.hidden = true; }, 4000);
}

/* Every change is a POST. The poll loop picks up the new state within 50 ms. */
const post = url => fetch(url, { method: 'POST' })
  .then(r => { if (!r.ok) return r.text().then(t => panelError(t || ('The change was refused (' + r.status + ').'))); })
  .catch(() => {});

function assign(cmd) {
  if (sel < 0) return;
  const rep = bind[sel] ? bind[sel].r : 0;
  post('/api/bind?i=' + sel + '&repeat=' + rep + '&cmd=' + encodeURIComponent(cmd));
}
function setRepeat(i, on) {
  const b = bind[i];
  if (!b || !b.c) return;
  post('/api/bind?i=' + i + '&repeat=' + (on ? 1 : 0) + '&cmd=' + encodeURIComponent(b.c));
}
const setAxis = (k, t, inv) => post('/api/axis?k=' + k + '&t=' + t + '&inv=' + (inv ? 1 : 0));
function pushTune() {
  tuneDirty = Date.now();
  post('/api/tune?deadzone=' + tune.deadzone + '&curve=' + tune.curve
       + '&gesture=' + tune.gesture);
}
const hex2rgb = h => [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)];
const rgb2hex = (r,g,b) => '#' + [r,g,b].map(v => v.toString(16).padStart(2,'0')).join('').toUpperCase();
function pushLight() {
  lightDirty = Date.now();
  post('/api/light?r=' + light.r + '&g=' + light.g + '&b=' + light.b
     + '&on=' + (light.on ? 1 : 0) + '&pattern=' + light.pattern
     + '&speed=' + light.speed + '&player=' + light.player
     + '&bright=' + light.bright + '&lmode=' + light.lmode);
}

const PAT_NAME = ['Solid','Breathe','Pulse','Strobe','Rainbow','Alert'];

function applyLightMode() {
  const isSim = light.lmode === 'sim';
  const tab = isSim ? 'sim' : (light.pattern !== 0 ? 'pattern' : 'solid');
  segSelect(el('ltabs'), b => b.dataset.lt === tab);
  el('lsolid').hidden   = tab !== 'solid';
  el('lpattern').hidden = tab !== 'pattern';
  el('lsim').hidden     = tab !== 'sim';
  segSelect($('.patrow'), b => +b.dataset.pat === light.pattern);
  segSelect($('.pleds'), b => +b.dataset.pl === light.player);
  el('lnote').textContent = isSim
    ? 'The light bar becomes an annunciator, most urgent first: a crash strobes red, a stall '
    + 'pulses red, a stall warning breathes amber, high load holds amber, turbulence breathes '
    + 'cyan, otherwise it is steady green. The thresholds are the light.sim settings in feedback.conf.'
    : tab === 'pattern'
    ? 'The pattern animates the color chosen under Solid. It refreshes 20 times a second, '
    + 'which leaves the Bluetooth link free for input.'
    : 'DSXP sends the color to the controller and repeats it every half second, because macOS '
    + 'has been observed to set its own color.';
}

function select(i, focusSearch) {
  sel = i; kidx = 0;
  renderResults();
  if (!focusSearch) return;
  /* focus({preventScroll}) alone is not enough - input.select() scrolls the box
   * into view on its own, which dragged the page around when a trigger moved.
   * Only do either when the user actually clicked something. */
  $('#q').focus({ preventScroll: true });
  $('#q').select();
}

/* Parameters mean different things per mode, so the labels follow the mode.
 * The wire format is a raw 0-255 byte per parameter: positions read as a share
 * of trigger travel, force as a share of the maximum, the vibrate rate as Hz. */
const PCT = { max: 255, step: 1,  fmt: v => bytePct(v) };
const HZ  = { max: 60,  step: 1,  fmt: v => v + ' Hz' };
const FX_PARAMS = {
  0: {},
  1: { start:    { label: 'Start:',      ...PCT },
       strength: { label: 'Force:',      ...PCT } },
  2: { start:    { label: 'Band start:', ...PCT },
       end:      { label: 'Band end:',   ...PCT },
       strength: { label: 'Force:',      ...PCT } },
  3: { start:    { label: 'Threshold:',  ...PCT },
       strength: { label: 'Amplitude:',  ...PCT },
       freq:     { label: 'Frequency:',  ...HZ  } },
};
const FX_DEFAULTS = {
  1: { start: 70,  end: 0,   strength: 200 },
  2: { start: 60,  end: 160, strength: 255 },
  3: { start: 40,  freq: 40, strength: 180 },
};
const MODE_NAME = ['Off','Resist','Weapon','Vibrate'];

/* Syncs the trigger panel to `fx`. It only renders: the listeners are attached
   once, at the bottom of this file. */
function renderFx() {
  document.querySelectorAll('.fxcol').forEach(col => {
    const i = col.dataset.side === 'r' ? 1 : 0;
    const f = fx[i];
    segSelect(col.querySelector('.modes'), b => +b.dataset.m === f.mode);
    const spec = FX_PARAMS[f.mode] || {};
    const box = col.querySelector('.fxparams');
    const want = Object.keys(spec).join(',') + '|' + f.mode;
    if (box.dataset.sig !== want) {
      box.dataset.sig = want;
      box.innerHTML = Object.entries(spec).map(([k, p]) => {
        const id = 'fx-' + i + '-' + k;
        return '<label for="' + id + '">' + p.label + '</label>' +
          '<input type="range" id="' + id + '" data-p="' + k + '" min="0" max="' + p.max +
          '" step="' + p.step + '" value="' + f[k] + '">' +
          '<output for="' + id + '" data-o="' + k + '">' + p.fmt(f[k]) + '</output>';
      }).join('') || '<span class="off">No resistance.</span>';
    }
    Object.entries(spec).forEach(([k, p]) => {
      const inp = box.querySelector('[data-p="' + k + '"]');
      const out = box.querySelector('[data-o="' + k + '"]');
      if (inp && document.activeElement !== inp) inp.value = f[k];
      setText(out, p.fmt(f[k]));
    });
  });
}

function applyFxMode() {
  segSelect(el('fxtabs'), b => b.dataset.fxm === fxmode);
  el('fxmanual').hidden = fxmode !== 'manual';
  el('fxwind').hidden   = fxmode !== 'wind';
  el('fxnote').innerHTML = fxmode === 'wind'
    ? 'Force follows dynamic pressure, so it grows with the square of indicated airspeed, '
    + 'like the load on a real rudder pedal. A crosswind makes one trigger heavier, and '
    + 'turbulence turns resistance into buffet. The numbers behind this are the wind settings '
    + 'in feedback.conf; your manual effects come back when you switch to Manual.'
    : '<b>Resist</b> holds a constant force past a point. <b>Weapon</b> resists across a band, '
    + 'then breaks through like a gun trigger. <b>Vibrate</b> buzzes once you press past the '
    + 'threshold. Positions are a share of trigger travel and force is a share of the maximum. '
    + 'Squeeze a trigger to feel the change.';
}

/* Motor bytes read as % of full scale. The left motor is the heavy one, so the
 * same byte feels stronger there - that asymmetry is real, not a bug. */
const RUMBLE_TESTS = {
  soft:  [70, 70],   hard:  [200, 200],
  left:  [220, 40],  right: [40, 220],
  crash: [255, 255], stop:  [0, 0],
};

function applyRumbleMode() {
  segSelect(el('rtabs'), b => b.dataset.rm === rmode);
  el('rmanual').hidden = rmode !== 'manual';
  el('rsim').hidden    = rmode !== 'sim';
  el('rnote').innerHTML = rmode === 'sim'
    ? 'Driven by the flight model. A stall on one wing buzzes that side harder, so you feel '
    + 'which way the aircraft is about to roll; high load adds a jolt on both sides, and a crash '
    + 'shakes both. The thresholds are the rumble settings in feedback.conf.'
    : 'Run the motors by hand to feel their range. The left motor is the heavier one, so equal '
    + 'values feel stronger on the left. Closing this window stops them. If the motors do '
    + 'nothing, turn the v2 path off.';
}

function pushRumble() {
  rumbleDirty = Date.now();
  post('/api/rumble?left=' + rumble[0] + '&right=' + rumble[1]);
}

function pushFx(i) {
  fxDirty = Date.now();
  const f = fx[i];
  post('/api/trigger?side=' + (i ? 'r' : 'l') + '&mode=' + f.mode +
       '&start=' + f.start + '&end=' + f.end + '&strength=' + f.strength + '&freq=' + f.freq);
}

/* ------------------------------------------------------------ rendering */

/* L2/R2 carry an analog axis as well as a digital click, so their rows say so -
 * otherwise "Not assigned" reads as if the trigger does nothing. */
const AXIS_SLOT = { 14: 'l2', 15: 'r2' };

function renderButtons() {
  $('#btns').innerHTML = SLOTS.map(s => {
    const b = bind[s.i] || { c: '', r: 0 };
    return '<div class="row" role="option" data-i="' + s.i + '">' +
      '<div class="nm">' + esc(s.n) + '</div>' +
      '<div class="bd' + (b.c ? '' : ' none') + '">' + esc(b.c || 'Not assigned') + '</div>' +
      (AXIS_SLOT[s.i] ? '<span class="axtag" title="This trigger also drives an axis (see Axes)" hidden>Also an axis</span>' : '') +
      '<label class="check rep"' + (b.c ? '' : ' hidden') + '><input type="checkbox" data-rep="' + s.i + '"' +
        (b.r ? ' checked' : '') + '> Repeat</label>' +
    '</div>';
  }).join('');
}

function renderAxes() {
  $('#axes').innerHTML = SOURCES.map((s, i) => {
    const a = axismap[i] || { t: 'none', inv: 0 };
    return '<div class="axrow">' +
      '<label for="ax-' + s.k + '">' + esc(s.n) + '</label>' +
      '<div class="bar"><i data-bar="' + i + '"></i></div>' +
      '<select id="ax-' + s.k + '" data-ax="' + s.k + '">' + TARGETS.map(t =>
        '<option value="' + t.k + '"' + (t.k === a.t ? ' selected' : '') + '>' +
        esc(t.n) + '</option>').join('') + '</select>' +
      '<label class="check"><input type="checkbox" data-inv="' + s.k + '"' + (a.inv ? ' checked' : '') +
        '> Invert</label>' +
    '</div>';
  }).join('');
}

/* Keep the axis pickers and Invert boxes in step with the bridge. */
function syncAxes() {
  document.querySelectorAll('#axes .axrow').forEach((row, i) => {
    const a = axismap[i];
    if (!a) return;
    const s = row.querySelector('select[data-ax]');
    if (s && document.activeElement !== s && s.value !== a.t) s.value = a.t;
    const inv = row.querySelector('input[data-inv]');
    if (inv && inv.checked !== !!a.inv) inv.checked = !!a.inv;
  });
}

/* The status strip: controller, X-Plane, bridge. */
function status(id, kind, text, sub) {
  const e = el(id);
  e.querySelector('.dot').className = 'dot' + (kind ? ' ' + kind : '');
  setText(e.querySelector('.t'), text);
  if (sub !== undefined) setText(el('st-hz'), sub);
}

/* Config problems: a button in the strip, the list in a popover. */
let cfgGen = -1, cfgSig = '';
function renderConfig(cfg) {
  if (!cfg) return;
  const w = cfg.warnings || [];
  const n = w.length + (cfg.dropped || 0);
  const btn = el('app-config');
  btn.hidden = n === 0;
  setText(btn.querySelector('span'), n === 1 ? '1 Config Problem…' : n + ' Config Problems…');
  const sig = JSON.stringify(w) + cfg.dropped;
  if (sig !== cfgSig) {
    cfgSig = sig;
    el('cfglist').innerHTML = w.map(x =>
      '<li><div class="where">' + esc(x.file) + (x.line ? ', line ' + x.line : '') +
        (x.key ? ' &middot; ' + esc(x.key) : '') + '</div>' +
      '<div class="msg">' + esc(x.msg.charAt(0).toUpperCase() + x.msg.slice(1)) + '.</div>' +
      '<div class="fixrow"><button data-open="' + esc(x.file) + '">Open ' + esc(x.file) + '</button></div></li>'
    ).join('') + (cfg.dropped ? '<li>' + cfg.dropped + ' more not shown.</li>' : '');
    if (n === 0) el('cfgpop').hidden = true;
  }
  /* an edit made outside the mapper was applied: say so once */
  if (cfgGen >= 0 && cfg.gen !== cfgGen && cfg.reloaded)
    toast('Applied your edit to ' + cfg.reloaded + (w.some(x => x.file === cfg.reloaded)
          ? ', except the lines listed under Config Problems.' : '.'));
  cfgGen = cfg.gen;
}

let lastCmds = -1;
function renderApp(app, st) {
  el('app-access').hidden = app.access === 'granted' || app.input_live || app.access === undefined;
  const dir = el('app-xpdir');
  setText(dir, app.xpdir ? app.xpdir.replace(/^\/Users\/[^/]+/, '~') : 'X-Plane folder not found');
  dir.title = app.xpdir || 'Choose your X-Plane 12 folder to search its commands';
  dir.classList.toggle('missing', !app.xpdir);
  el('app-imported').hidden = !app.imported;
  renderConfig(app.config);
  if (typeof app.cmds === 'number' && app.cmds !== lastCmds) {
    if (lastCmds >= 0) loadInit();            /* catalog changed: reload it */
    lastCmds = app.cmds;
  }

  const g = st.gate;
  const hz = app.input_live && app.hz ? Math.round(app.hz) + ' Hz' : '';
  if (g.usb) status('st-ctl', 'warn', 'Controller on USB, X-Plane reads it directly', '');
  else if (g.bt) status('st-ctl', app.input_live === false ? 'warn' : 'ok',
                        app.input_live === false ? 'Controller connected, no input' : 'Controller connected', hz);
  else status('st-ctl', '', 'No controller', '');
  if (!g.xp) status('st-xp', '', 'X-Plane 12 not running');
  else if (app.xp_answering === false) status('st-xp', 'warn', 'X-Plane 12 not answering');
  else status('st-xp', 'ok', 'X-Plane 12 answering');
  el('st-xp').title = g.xp && app.xp_answering === false
    ? 'X-Plane is running but sends no data back. Check Settings › Network in X-Plane.' : '';
  if (app.suspended) status('st-eng', '', 'Paused while the Mac sleeps');
  else status('st-eng', g.engaged ? 'ok' : '', g.engaged ? 'Driving X-Plane' : 'Idle');
  el('st-ctl').title = st.reports.toLocaleString() + ' input reports in the last 10 seconds, '
                     + st.total.toLocaleString() + ' since DSXP started';
}

/* mirrors shape() in Sources/Core/input/decode.c so the preview matches what is sent */
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
  setText(el('dz-v'), (tune.deadzone * 100).toFixed(1) + '%');
  setText(el('cv-v'), tune.curve.toFixed(2));
  setText(el('gs-v'), pct(tune.gesture ?? 0.5));
}

/* Side-view gauge: the blade pivots through its real travel, so a glance tells
 * you which trigger is down and by how much. */
function drawTrigger(side, v) {
  el('trig-' + side + '-lever').setAttribute('transform',
    'translate(66 66) rotate(' + (-v * TRIGGER_SWEEP) + ')');
  setText(el('trig-' + side + '-pct'), pct(v));
  el('trig-' + side + '-bar').setAttribute('width', 120 * v);
}

/* ---------------------------------------------------------- live poll */

async function poll() {
  if (stopped) return;
  let st;
  try { st = await (await fetch('/api/state')).json(); } catch (e) { return; }
  bind = st.bind; axismap = st.axismap;
  syncAxes();
  renderApp(st.app || {}, st);
  setText(el('mapstatus'), st.connected ? 'Receiving input' : 'Waiting for input');

  if (Date.now() - tuneDirty > 600) {
    tune = st.tune;
    if (document.activeElement !== el('dz')) el('dz').value = tune.deadzone;
    if (document.activeElement !== el('cv')) el('cv').value = tune.curve;
    if (document.activeElement !== el('gs')) el('gs').value = tune.gesture;
    drawCurve();
  }

  /* the manual panel edits the hand-set values; in wind mode it is hidden */
  if (Date.now() - fxDirty > 600 && (st.fxm || st.fx)) { fx = st.fxm || st.fx; renderFx(); }
  if (Date.now() - fxmodeDirty > 600 && st.fxmode && st.fxmode !== fxmode) {
    fxmode = st.fxmode; applyFxMode();
  }
  setText(el('fxstatus'), fxmode === 'wind'
    ? (st.wx && st.wx.live ? 'Wind, live' : 'Wind, waiting for X-Plane')
    : 'L2 ' + MODE_NAME[fx[0].mode] + ', R2 ' + MODE_NAME[fx[1].mode]);

  if (Date.now() - rmodeDirty > 600 && st.rmode && st.rmode !== rmode) {
    rmode = st.rmode; applyRumbleMode();
  }
  setText(el('rstatus'), st.rnote || '');

  if (st.rumble && document.activeElement !== el('rv2')) el('rv2').checked = !!st.rumble.v2;
  if (rmode === 'manual' && Date.now() - rumbleDirty > 600 && st.rumble) {
    rumble = [st.rumble.left, st.rumble.right];
    if (document.activeElement !== el('r-l')) el('r-l').value = rumble[0];
    if (document.activeElement !== el('r-r')) el('r-r').value = rumble[1];
    setText(el('r-lv'), bytePct(rumble[0]));
    setText(el('r-rv'), bytePct(rumble[1]));
  }
  if (rmode === 'sim' && st.wx) {
    const w = st.wx;
    setText(el('rx-sl'), w.live ? pct(w.stall_l) : '—');
    setText(el('rx-sr'), w.live ? pct(w.stall_r) : '—');
    setText(el('rx-sw'), w.live ? pct(w.stall) : '—');
    setText(el('rx-gn'), w.live ? w.gn.toFixed(2) + ' g' : '—');
    const crash = el('rx-crash');
    const ct = w.live ? (w.crashed ? 'Yes' : 'No') : '—';
    if (crash.dataset.v !== ct) {
      crash.dataset.v = ct;
      crash.innerHTML = (w.live && w.crashed ? '<i class="dot bad"></i>' : '') + ct;
    }
    setText(el('rx-note'), st.rnote || '—');
    el('rx-note').classList.toggle('stale', !w.live);
    setText(el('rx-l'), st.rumble ? bytePct(st.rumble.left) : '—');
    setText(el('rx-r'), st.rumble ? bytePct(st.rumble.right) : '—');
  }

  if (fxmode === 'wind' && st.wx) {
    const w = st.wx;
    setText(el('wx-ias'),  w.live ? Math.round(w.ias) + ' kt' : '—');
    setText(el('wx-wind'), w.live ? Math.round(w.wind * MS_TO_KT) + ' kt from ' + Math.round(w.dir) + '°' : '—');
    setText(el('wx-xw'),   w.live
      ? (Math.abs(w.xwind) * MS_TO_KT).toFixed(1) + ' kt from the ' + (w.xwind >= 0 ? 'right' : 'left') : '—');
    setText(el('wx-turb'), w.live ? w.turb.toFixed(1) + ' of 10' : '—');
    setText(el('wx-load'), w.live ? pct(w.load) : '—');
    setText(el('wx-note'), w.note || '—');
    el('wx-note').classList.toggle('stale', !w.live);
    const desc = f => f.mode === 0 ? 'Free'
      : f.mode === 3 ? 'Buzz ' + bytePct(f.strength) + ' at ' + f.end + ' Hz'
      : bytePct(f.strength) + ' force';
    /* wind readouts show what is actually on the triggers (st.fx), not the manual set */
    setText(el('wx-l'), desc(st.fx[0]));
    setText(el('wx-r'), desc(st.fx[1]));
  }

  if (Date.now() - lightDirty > 600 && st.light) {
    light = st.light;
    const hex = rgb2hex(light.r, light.g, light.b);
    if (document.activeElement !== el('lc')) el('lc').value = hex.toLowerCase();
    el('lon').checked = !!light.on;
    if (document.activeElement !== el('lspeed')) el('lspeed').value = light.speed;
    setText(el('lspeed-v'), pct(light.speed));
    if (document.activeElement !== el('lbright')) el('lbright').value = light.bright;
    setText(el('lbright-v'), pct(light.bright));
    if (light.lmode === 'sim') setText(el('lx-note'), light.lnote || '—');
    setText(el('lightstatus'), !light.on ? 'Off'
      : light.lmode === 'sim' ? 'Simulator: ' + (light.lnote || '')
      : (light.pattern ? PAT_NAME[light.pattern] + ', ' + hex : hex));
    applyLightMode();
  }

  /* live touch points over the traced touchpad (bbox 394,11 522x281) */
  if (st.touch) st.touch.forEach((t, i) => {
    const d = el('touch-' + i);
    if (!d) return;
    if (t.on) { d.setAttribute('cx', 394 + t.x * 522); d.setAttribute('cy', 11 + t.y * 281); }
    else      { d.setAttribute('cx', -99); d.setAttribute('cy', -99); }
  });

  /* Physically pressing a control jumps to its command - but not while you are
   * working a slider or picker, otherwise squeezing a trigger to feel an effect
   * yanks focus into the search box mid-drag. */
  const af = document.activeElement;
  const busyElsewhere = (af && af !== el('q') &&
      /^(INPUT|SELECT|BUTTON|TEXTAREA)$/.test(af.tagName))
    || Date.now() - uiBusy < 1500;
  const typing = af === el('q') && el('q').value.trim() !== '';
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

  /* sticks follow the shaped values; the traced caps are paths, so they move by transform */
  el('stick-l').setAttribute('transform', 'translate(' + (st.axes[0] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (st.axes[1] * STICK_TRAVEL).toFixed(1) + ')');
  el('stick-r').setAttribute('transform', 'translate(' + (st.axes[2] * STICK_TRAVEL).toFixed(1)
                 + ' ' + (st.axes[3] * STICK_TRAVEL).toFixed(1) + ')');

  /* triggers: raw travel, so the gauge shows the physical squeeze even when the
   * dead zone is swallowing it downstream */
  drawTrigger('l', st.raw[4]);
  drawTrigger('r', st.raw[5]);
  setText(el('trigstatus'), st.raw[4] > 0.02 || st.raw[5] > 0.02
    ? 'L2 ' + pct(st.raw[4]) + ', R2 ' + pct(st.raw[5]) : '');

  if ($('#btns').children.length !== SLOTS.length) renderButtons();
  [...$('#btns').children].forEach((e, i) => {
    e.classList.toggle('down', !!st.btn[i]);
    e.classList.toggle('sel', sel === i);
    e.setAttribute('aria-selected', sel === i ? 'true' : 'false');
    const b = bind[i] || { c: '', r: 0 };
    const bd = e.querySelector('.bd');
    const want = b.c || 'Not assigned';
    if (bd.textContent !== want) { bd.textContent = want; bd.className = 'bd' + (b.c ? '' : ' none'); }
    const rp = e.querySelector('.rep');
    rp.hidden = !b.c;
    const cb = rp.querySelector('input');
    if (cb.checked !== !!b.r) cb.checked = !!b.r;
    const ax = e.querySelector('.axtag');
    if (ax) {
      const a = axismap[SOURCES.findIndex(x => x.k === AXIS_SLOT[i])];
      ax.hidden = !(a && a.t !== 'none');
    }
  });
  setText(el('bindcount'), bind.filter(b => b.c).length + ' of ' + SLOTS.length + ' assigned');

  if (!$('#axes').children.length) renderAxes();
  st.axes.forEach((v, i) => {
    const bar = document.querySelector('[data-bar="' + i + '"]');
    if (!bar) return;
    if (i >= 4) { bar.style.left = '0'; bar.style.width = (v * 100) + '%'; }
    else {
      const p = Math.abs(v) * 50;
      bar.style.left = (v < 0 ? 50 - p : 50) + '%';
      bar.style.width = p + '%';
    }
  });

  let drive = 0;
  for (let i = 0; i < 4; i++)
    if (Math.abs(st.raw[i] * 2 - 1) > Math.abs(drive)) drive = st.raw[i] * 2 - 1;
  el('curvedot').setAttribute('cx', 60 + drive * 58);
  el('curvedot').setAttribute('cy', 60 - shaped(drive) * 58);

  setText(el('target'), sel < 0 ? 'Command' : 'Command for ' + SLOTS[sel].n);
  setText(el('target-cur'), sel < 0 ? 'Select a control'
    : (bind[sel] && bind[sel].c) ? 'Now: ' + bind[sel].c : 'Not assigned');
  el('unbind').disabled = sel < 0 || !(bind[sel] && bind[sel].c);
}

/* --------------------------------------------------------------- events */

const padClick = e => {
  const t = e.target.closest('[id^="b-"]');
  if (t) select(+t.id.slice(2).replace(/\D/g, ''), true);
};
el('pad').addEventListener('click', padClick);
el('trigview').addEventListener('click', padClick);

el('btns').addEventListener('click', e => {
  if (e.target.closest('.rep')) return;          /* the Repeat box has its own handler */
  const row = e.target.closest('.row');
  if (row) select(+row.dataset.i, true);
});
el('btns').addEventListener('change', e => {
  const cb = e.target.closest('input[data-rep]');
  if (cb) setRepeat(+cb.dataset.rep, cb.checked);
});
el('axes').addEventListener('change', e => {
  const s = e.target.closest('select[data-ax]');
  if (s) {
    const i = SOURCES.findIndex(x => x.k === s.dataset.ax);
    setAxis(s.dataset.ax, s.value, axismap[i].inv);
    return;
  }
  const inv = e.target.closest('input[data-inv]');
  if (inv) {
    const i = SOURCES.findIndex(x => x.k === inv.dataset.inv);
    setAxis(inv.dataset.inv, axismap[i].t, inv.checked);
  }
});
el('results').addEventListener('click', e => {
  const r = e.target.closest('.res');
  if (r) assign(shown[+r.dataset.i].c);
});
el('unbind').addEventListener('click', () => assign(''));

el('dz').addEventListener('input', e => { tune.deadzone = +e.target.value; drawCurve(); pushTune(); });
el('cv').addEventListener('input', e => { tune.curve    = +e.target.value; drawCurve(); pushTune(); });
el('gs').addEventListener('input', e => { tune.gesture  = +e.target.value; drawCurve(); pushTune(); });
el('rtabs').addEventListener('click', e => {
  const b = e.target.closest('[data-rm]');
  if (!b || b.dataset.rm === rmode) return;
  rmode = b.dataset.rm;
  rmodeDirty = Date.now();
  applyRumbleMode();
  post('/api/rumblemode?mode=' + rmode);
});
$('.rparams').addEventListener('input', e => {
  const i = e.target.id === 'r-r' ? 1 : 0;
  rumble[i] = +e.target.value;
  setText(el(i ? 'r-rv' : 'r-lv'), bytePct(rumble[i]));
  pushRumble();
});
el('rv2').addEventListener('change', e => post('/api/rumblepath?v2=' + (e.target.checked ? 1 : 0)));
$('.rtests').addEventListener('click', e => {
  const b = e.target.closest('[data-test]');
  if (!b) return;
  rumble = RUMBLE_TESTS[b.dataset.test].slice();
  el('r-l').value = rumble[0]; el('r-r').value = rumble[1];
  setText(el('r-lv'), bytePct(rumble[0]));
  setText(el('r-rv'), bytePct(rumble[1]));
  pushRumble();
});
el('fxtabs').addEventListener('click', e => {
  const b = e.target.closest('[data-fxm]');
  if (!b || b.dataset.fxm === fxmode) return;
  fxmode = b.dataset.fxm;
  fxmodeDirty = Date.now();
  applyFxMode();
  post('/api/fxmode?mode=' + fxmode);
});
document.querySelectorAll('.fxcol').forEach(col => {
  const i = col.dataset.side === 'r' ? 1 : 0;
  col.querySelector('.modes').addEventListener('click', e => {
    const b = e.target.closest('[data-m]');
    if (!b || +b.dataset.m === fx[i].mode) return;
    const m = +b.dataset.m;
    fx[i] = Object.assign({}, fx[i], FX_DEFAULTS[m] || {}, { mode: m });
    renderFx(); pushFx(i);
  });
  col.querySelector('.fxparams').addEventListener('input', e => {
    const inp = e.target.closest('[data-p]');
    if (!inp) return;
    fx[i][inp.dataset.p] = +inp.value;
    const p = (FX_PARAMS[fx[i].mode] || {})[inp.dataset.p];
    const out = col.querySelector('[data-o="' + inp.dataset.p + '"]');
    if (out && p) setText(out, p.fmt(+inp.value));
    pushFx(i);
  });
});
const setHex = h => { [light.r, light.g, light.b] = hex2rgb(h); };
el('lc').addEventListener('input', e => { setHex(e.target.value); pushLight(); });
el('lon').addEventListener('change', e => { light.on = e.target.checked; pushLight(); });
$('.swatches').addEventListener('click', e => {
  const b = e.target.closest('[data-c]');
  if (!b) return;
  el('lc').value = b.dataset.c; setHex(b.dataset.c); pushLight();
});
el('ltabs').addEventListener('click', e => {
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
  setText(el('lbright-v'), pct(light.bright));
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
  setText(el('lspeed-v'), pct(light.speed));
  pushLight();
});
$('.pleds').addEventListener('click', e => {
  const b = e.target.closest('[data-pl]');
  if (!b) return;
  light.player = +b.dataset.pl;
  applyLightMode(); pushLight();
});

el('q').addEventListener('input', () => { kidx = 0; renderResults(); });
el('q').addEventListener('keydown', e => {
  if (e.key === 'ArrowDown')    { e.preventDefault(); kidx = Math.min(kidx + 1, shown.length - 1); renderResults(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); kidx = Math.max(kidx - 1, 0); renderResults(); }
  else if (e.key === 'Enter')   { e.preventDefault(); if (shown[kidx]) assign(shown[kidx].c); }
  else if (e.key === 'Escape')  { el('q').value = ''; kidx = 0; renderResults(); }
});

/* any interaction with a tuning panel suppresses press-to-select for a moment,
 * and remembers the panel so a refused change is reported next to it */
document.querySelectorAll('main section').forEach(sec => {
  sec.addEventListener('pointerdown', () => { lastSection = sec; }, true);
  sec.addEventListener('focusin', () => { lastSection = sec; });
  if (sec.id === 'sec-assign') return;           // the command panel still jumps
  sec.addEventListener('pointerdown', () => { uiBusy = Date.now(); });
  sec.addEventListener('input',       () => { uiBusy = Date.now(); });
});

/* The app asks for confirmation in a native sheet and answers ok:false on Cancel. */
let stopped = false;
el('quit').addEventListener('click', async () => {
  try {
    const r = await (await fetch('/api/quit', { method: 'POST' })).json();
    if (!r.ok) return;
  } catch (e) { /* the app is closing */ }
  stopped = true;
  el('gone').hidden = false;
});

el('app-pickxp').addEventListener('click', async () => {
  try {
    const r = await (await fetch('/api/pickxp', { method: 'POST' })).json();
    if (r.ok) await loadInit();
  } catch (e) { /* canceled */ }
});
el('app-access').addEventListener('click', () => post('/api/openaccess'));
const openConfig = file => post('/api/openconfig' + (file ? '?file=' + encodeURIComponent(file) : ''));
el('app-openconfig').addEventListener('click', () => openConfig(''));
el('cfg-folder').addEventListener('click', () => openConfig(''));
el('app-config').addEventListener('click', () => { el('cfgpop').hidden = !el('cfgpop').hidden; });
el('cfg-close').addEventListener('click', () => { el('cfgpop').hidden = true; });
el('cfglist').addEventListener('click', e => {
  const b = e.target.closest('[data-open]');
  if (b) openConfig(b.dataset.open);
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && !el('cfgpop').hidden) el('cfgpop').hidden = true;
});

/* ----------------------------------------------------------------- boot */

async function loadInit() {
  const init = await (await fetch('/api/init')).json();
  CMDS = init.commands.map(c => ({ c: c.c, d: c.d, lp: c.c.toLowerCase(), ld: (c.d || '').toLowerCase() }));
  SLOTS = init.slots; SOURCES = init.sources; TARGETS = init.targets;
  el('q').placeholder = CMDS.length
    ? 'Search ' + CMDS.length.toLocaleString() + ' X-Plane commands'
    : 'No command list yet';
  renderResults();
}

/* One poll at a time, every 50 ms: the next starts only after the last one
   finished, so slow replies can never pile up. */
async function pollLoop() {
  while (!stopped) {
    const t0 = Date.now();
    try { await poll(); }
    catch (e) { console.error(e); await new Promise(r => setTimeout(r, 1000)); }   /* never stop polling */
    await new Promise(r => setTimeout(r, Math.max(10, 50 - (Date.now() - t0))));
  }
}

(async () => {
  for (;;) {                       /* retry until the API answers */
    try { await loadInit(); break; }
    catch (e) { console.error(e); await new Promise(r => setTimeout(r, 1000)); }
  }
  await poll();
  renderButtons(); renderAxes(); renderResults(); drawCurve(); renderFx(); applyFxMode(); applyRumbleMode(); applyLightMode();
  pollLoop();
})();
