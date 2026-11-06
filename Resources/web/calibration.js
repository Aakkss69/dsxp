/* DSXP mapper - the motion control calibration sheet (index.html #calibration).
 *
 * The core runs the routine (Sources/Core/input/motion_calib.c) and reports its progress
 * in /api/state as state.motion.calibration; this file only shows it and sends
 * start / close. The sheet is shown:
 *   - while motion control has never been calibrated: it covers the mapper and cannot be
 *     closed (the status strip, with Quit, stays above it); sticks keep working
 *   - while a calibration started from the Motion control panel runs, and on its result
 *
 * Loaded after app.js and motion-panel.js; uses byId, setText, clamp, post, onState,
 * showVariant (app.js) and Plane3D.View (plane3d/view.js).
 */
(function () {
  'use strict';

  const sheet = byId('calibration');
  const view = new Plane3D.View(byId('cal-plane'));
  const ORDER = ['flat', 'level', 'roll', 'pitch', 'yaw'];   /* = calib_step_t, idle and done aside */

  /* What to do in each step (sentence style, as the rest of the mapper). */
  const INSTRUCTION = {
    idle:  'DSXP needs to learn your controller and how you hold it before motion control can fly.',
    flat:  'Lay the controller face up on a table and take your hands off it.',
    level: 'Pick it up and hold it the way you would fly level. Keep it steady.',
    roll:  'Bank it left and right as far as is comfortable, then back to level.',
    pitch: 'Raise and lower the top edge as far as is comfortable, then back to level.',
    yaw:   'Twist it left and right as far as is comfortable, keeping it flat, then back to center.',
    done:  'Done. Motion control is calibrated to your controller and your hands.',
  };
  /* The ends of the reach gauge: which way is which, and how far the scale goes (deg). */
  const SCALE = {
    roll:  { lo: 'Left', hi: 'Right', max: 90 },
    pitch: { lo: 'Down', hi: 'Up',    max: 60 },
    yaw:   { lo: 'Left', hi: 'Right', max: 90 },
  };
  /* How far the drawn aircraft moves at 100 % (plane3d/view.js RANGE_DEG). */
  const DRAWN = { roll: 45, pitch: 25, yaw: 30 };

  /* ------------------------------------------------------------- sending */

  const start = () => post('/api/calibrate?action=start');
  const close = () => post('/api/calibrate?action=close');

  /* One button, three jobs, decided by what the sheet shows (dataset.action, set below). */
  byId('cal-primary').addEventListener('click', () => {
    const action = byId('cal-primary').dataset.action;
    if (action === 'switch-on') post('/api/motion?enabled=1').then(close);   /* then close */
    else if (action === 'close') close();
    else start();
  });
  byId('cal-restart').addEventListener('click', start);
  byId('cal-done').addEventListener('click', close);
  byId('cal-cancel').addEventListener('click', close);
  byId('motion-calibrate').addEventListener('click', start);

  /* ------------------------------------------------------------- showing */

  /* The sheet sits under the status strip, whose height changes when the window narrows. */
  const strip = byId('strip');
  const fitUnderStrip = () => document.documentElement.style.setProperty('--strip-h', strip.offsetHeight + 'px');
  new ResizeObserver(fitUnderStrip).observe(strip);

  /* Show or hide the sheet; while shown, the mapper behind it is out of reach. */
  function setOpen(open) {
    if (sheet.hidden === !open) return;
    sheet.hidden = !open;
    document.querySelector('main').inert = open;
    if (open) { fitUnderStrip(); byId('cal-primary').focus(); }
  }

  /* x deg on a gauge running -max..+max, as a share of its width (0..1). */
  const at = (x, max) => clamp((x + max) / (2 * max), 0, 1);
  const pct = v => (v * 100).toFixed(2) + '%';
  const signed = (v, digits) => (v < 0 ? '−' : v > 0 ? '+' : '') + Math.abs(v).toFixed(digits);

  onState(state => {
    const m = state.motion;
    if (!m || !m.calibration) return;                       /* an older core              */
    const k = m.calibration, step = k.step;
    const running = ORDER.includes(step);
    setOpen(!m.calibrated || step !== 'idle');              /* required, running, or result */
    if (sheet.hidden) return;

    /* the step list: done ones, the current one */
    const now = ORDER.indexOf(step);
    document.querySelectorAll('#cal-steps li').forEach((li, i) => {
      li.classList.toggle('done', step === 'done' || (now >= 0 && i < now));
      li.classList.toggle('now', i === now);
    });

    /* what to do, or why it can't start */
    const waiting = !m.live && step !== 'done';             /* no motion data at all      */
    setText(byId('cal-instruction'), waiting && !running
      ? 'Connect the DualSense over Bluetooth to begin.' : INSTRUCTION[step] || '');
    setText(byId('cal-note'), waiting && running ? 'Waiting for the controller.' : k.note || '');

    /* the meter: a bar for the timed steps, a gauge for the reaches, the result at the end */
    const meter = byId('cal-meter');
    if (step === 'flat' || step === 'level') {
      showVariant(meter, 'bar');
      const share = k.need > 0 ? clamp(k.hold / k.need, 0, 1) : 0;
      byId('cal-bar-fill').style.width = pct(share);
      meter.querySelector('.cal-bar').setAttribute('aria-valuenow', Math.round(share * 100));
    } else if (SCALE[step]) {
      showVariant(meter, 'reach');
      const sc = SCALE[step];
      setText(byId('cal-scale-lo'), sc.lo);
      setText(byId('cal-scale-hi'), sc.hi);
      setText(byId('cal-angle'), signed(k.angle, 0) + '°');
      byId('cal-span').style.left = pct(at(k.lo, sc.max));                 /* reached so far */
      byId('cal-span').style.width = pct(at(k.hi, sc.max) - at(k.lo, sc.max));
      byId('cal-need-lo').style.left = pct(at(-k.min, sc.max));           /* how far to go  */
      byId('cal-need-hi').style.left = pct(at(k.min, sc.max));
      byId('cal-needle').style.left = pct(at(k.angle, sc.max));           /* where it is now */
    } else if (step === 'done') {
      showVariant(meter, 'result');
      const r = k.result;
      setText(byId('cal-r-drift'), r.bias.map(v => signed(v, 2)).join('  ') + ' °/s');
      setText(byId('cal-r-gravity'), r.gravity.toFixed(3) + ' g');
      setText(byId('cal-r-level'), 'bank ' + signed(r.level[0], 1) + '°, pitch ' + signed(r.level[1], 1) + '°');
      setText(byId('cal-r-full'), 'roll ' + r.full[0] + '°, pitch ' + r.full[1] + '°, twist ' + r.full[2] + '°');
    } else showVariant(meter, 'none');

    /* buttons: Start, or Start Over while running; at the end Switch On (unless already
     * on) and Done; Cancel only when an earlier calibration exists to fall back to */
    const primary = byId('cal-primary');
    primary.dataset.action = step !== 'done' ? 'start' : m.enabled ? 'close' : 'switch-on';
    setText(primary, step !== 'done' ? 'Start' : m.enabled ? 'Done' : 'Switch On Motion Control');
    primary.hidden = running;
    primary.disabled = waiting && !running;
    byId('cal-restart').hidden = !running;
    byId('cal-done').hidden = !(step === 'done' && !m.enabled);
    byId('cal-cancel').hidden = !(running && m.calibrated);
    setText(byId('cal-foot-note'), step === 'done'
      ? 'Saved in controls.conf. Run it again any time from the Motion control panel.'
      : m.calibrated ? 'About a minute. Motion control holds the controls still meanwhile; Cancel keeps your previous calibration.'
      : 'About a minute. Your sticks keep working meanwhile; motion control can be switched on once this is done.');

    /* the aircraft follows the controller: relative to the new level once it is set */
    const a = m.axes;
    const twist = step === 'yaw' ? k.angle : 0;
    view.show({ valid: m.live ? 1 : 0,
                roll: a[0].angle / DRAWN.roll, pitch: a[1].angle / DRAWN.pitch, yaw: twist / DRAWN.yaw });
  });
})();
