/* DSXP mapper - the Motion control panel (index.html #panel-motion).
 *
 * Shows, per axis, whether motion or the sticks fly it, the controller's angle now, what
 * X-Plane was sent, and the "full deflection" angle (the sensitivity). Draws the aircraft
 * with Plane3D.View from what was sent. Settings go to POST /api/motion with the words of
 * controls.conf (roll=motion|sticks, roll.full=<deg>, ...); Recenter to /api/motionrecenter.
 *
 * Loaded after app.js and uses its helpers: byId, setText, clamp, post, onState,
 * editedAt / markEdited / recentlyEdited (a value just changed here wins over the poll
 * for a moment, so a slider never jumps back). */
(function () {
  'use strict';

  const AXES = ['roll', 'pitch', 'yaw'];              /* = MOTION_ROLL, PITCH, YAW        */
  const view = new Plane3D.View(byId('motion-plane'));
  editedAt.motion = 0;                                 /* this panel's edit-hold slot      */

  const one = (attr, axis) => document.querySelector('[' + attr + '="' + axis + '"]');
  const sw = byId('motion-on');

  /* ------------------------------------------------------------- sending */

  function send(query) {
    markEdited('motion');
    post('/api/motion?' + query);
  }

  sw.addEventListener('change', () => send('enabled=' + (sw.checked ? 1 : 0)));
  byId('motion-recenter').addEventListener('click', () => post('/api/motionrecenter'));
  for (const axis of AXES) {
    const use = one('data-motion-use', axis), full = one('data-motion-full', axis);
    use.addEventListener('change', () => send(axis + '=' + (use.checked ? 'motion' : 'sticks')));
    full.addEventListener('input', () => {
      setText(one('data-motion-full-value', axis), full.value + '°');   /* show it at once */
      send(axis + '.full=' + full.value);
    });
  }

  /* ------------------------------------------------------------- showing */

  /* signed degrees with one decimal and a real minus sign */
  const degrees = v => (v < 0 ? '−' : '') + Math.abs(v).toFixed(1) + '°';
  /* a -1..1 ratio as a signed percent */
  const ratio = v => { const p = Math.round(v * 100); return (p < 0 ? '−' : p > 0 ? '+' : '') + Math.abs(p) + '%'; };

  /* One line in the panel's title row, most important first. */
  function statusText(m) {
    if (!m.enabled) return 'Off';
    if (!m.live) return 'On, waiting for the controller';
    if (m.out_of_range) return 'Controller set down: holding the controls neutral';
    if (m.hold) return 'Held: controls frozen until you let go';
    if (m.cal === 'nominal') return 'On, using standard sensor scales';
    return 'On';
  }

  onState(state => {
    const m = state.motion;
    if (!m) return;                                   /* an older core: nothing to show   */
    const editing = recentlyEdited('motion');
    if (!editing && document.activeElement !== sw) sw.checked = !!m.enabled;
    const sent = m.sent || {};
    m.axes.forEach((a, i) => {
      const axis = AXES[i];
      const use = one('data-motion-use', axis), full = one('data-motion-full', axis);
      if (!editing && document.activeElement !== use) use.checked = a.use === 'motion';
      if (!editing && document.activeElement !== full) {
        full.value = a.full;
        setText(one('data-motion-full-value', axis), Math.round(a.full) + '°');
      }
      setText(one('data-motion-angle', axis), m.live ? degrees(a.angle) : '—');
      setText(one('data-motion-sent', axis), sent.valid ? ratio(sent[axis]) : '—');
      /* the tick under the slider: where |angle| sits on the slider's scale; darker once
       * past the full-deflection angle (the axis is at its stop) */
      const marker = one('data-motion-marker', axis);
      const lo = +full.min, hi = +full.max, at = clamp((Math.abs(a.angle) - lo) / (hi - lo), 0, 1);
      marker.style.left = 'calc(8px + (100% - 16px) * ' + at.toFixed(4) + ')';   /* the thumb's travel */
      marker.hidden = !m.live;
      marker.classList.toggle('full', m.live && Math.abs(a.angle) >= a.full);
    });
    setText(byId('motion-status'), statusText(m));
    byId('motion-standby').hidden = !!sent.valid;     /* the plane says it too: faded     */
    view.show(sent);
  });
})();
