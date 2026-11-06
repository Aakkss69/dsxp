/* Plane3D - view.js: draws the aircraft in a canvas, attitude = what DSXP sent to X-Plane.
 *
 *   const view = new Plane3D.View(canvas);
 *   view.show({ valid, roll, pitch, yaw });   // -1..1 each, as sent; valid = engaged
 *
 * Chase view from behind and above. +-100 % sent draws as 45 deg bank, 25 deg pitch and
 * 30 deg yaw (rotation roll, then pitch, then yaw). While nothing is being sent (valid
 * false) the aircraft eases to level and its lines fade to 40 %. The drawing eases toward
 * each new value in about 70 ms, and the canvas is redrawn only while something moves.
 * The line color is the canvas's CSS `color` (the panel sets it to the label color), read
 * again when the appearance changes.
 *
 * Each frame: rotate, fill the depth buffer (zbuffer.js), trace the fuselage contour
 * (contour.js) and the part outlines (airframe.js) through it (lines.js), stroke once.
 * Requires math.js, airframe.js, contour.js, zbuffer.js, lines.js. */
(function (P3) {
  'use strict';
  const { sub, dot, cross, norm, basis, toWorld, toBody } = P3.math;
  const AF = P3.airframe;

  const RANGE_DEG = { roll: 45, pitch: 25, yaw: 30 };   /* drawn angle at 100 % sent        */
  const EYE = [0, 2.1, -8.2];                            /* camera, world frame: behind, above */
  const FORWARD = norm(sub([0, 0, 0], EYE));             /* it looks at the origin            */
  /* up x forward points to the plane's right in this (x right, y up, z nose) frame, so
   * screen right is the right wing: a right bank lowers the right wing on screen */
  const RIGHT = norm(cross([0, 1, 0], FORWARD));
  const UP = cross(FORWARD, RIGHT);                      /* screen up                         */
  /* Focal length: as large as the canvas allows with room for the extremes. At 45 deg
   * bank the wing reaches about 2 units above and below the center (needs focal <= 1.8 x
   * the height at the camera's 8.5 units); level, the wing is 2.75 units each side
   * (needs focal <= 1.3 x the width). */
  const FOCAL_PER_HEIGHT = 1.8, FOCAL_PER_WIDTH = 1.3;
  const CENTER_Y = 0.52;                                 /* the origin's height on screen     */
  const EASE = 0.18;                                     /* share of the gap closed per frame */
  const FADE_EASE = 0.15;
  const STANDBY_ALPHA = 0.4;                             /* line opacity while standing by    */
  const LINE_WIDTH = 1.3;                                /* CSS pixels                        */
  const DEG = Math.PI / 180;

  class View {
    constructor(canvas) {
      this.canvas = canvas;
      this.ctx = canvas.getContext('2d');
      this.zbuffer = new P3.ZBuffer();
      this.current = { roll: 0, pitch: 0, yaw: 0 };     /* what is drawn now, -1..1          */
      this.target = { roll: 0, pitch: 0, yaw: 0 };      /* what was last sent                */
      this.alpha = STANDBY_ALPHA;                        /* starts standing by                */
      this.targetAlpha = STANDBY_ALPHA;
      this.frame = 0;                                    /* pending requestAnimationFrame id  */
      const n = AF.mesh.vertices.length;                 /* per-vertex scratch, reused         */
      this.sx = new Float64Array(n);                     /* device-pixel x                     */
      this.sy = new Float64Array(n);                     /* device-pixel y                     */
      this.iz = new Float64Array(n);                     /* 1 / distance                       */
      this.wp = new Float64Array(n * 3);                 /* world position                     */
      this.reduceMotion = matchMedia('(prefers-reduced-motion: reduce)');
      this.readColor();
      matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        this.readColor();
        this.request();
      });
      new ResizeObserver(() => this.request()).observe(canvas);   /* redraw at the new size */
      this.request();
    }

    /* The panel's label color, resolved by the browser (works for system colors too). */
    readColor() { this.color = getComputedStyle(this.canvas).color; }

    /* New values from /api/state. Anything outside -1..1 is clamped. */
    show(sent) {
      const valid = !!(sent && sent.valid);
      const c = v => Math.max(-1, Math.min(1, Number(v) || 0));
      this.target = valid ? { roll: c(sent.roll), pitch: c(sent.pitch), yaw: c(sent.yaw) }
                          : { roll: 0, pitch: 0, yaw: 0 };   /* standing by: level             */
      this.targetAlpha = valid ? 1 : STANDBY_ALPHA;
      this.request();
    }

    /* Ask for one frame (at most one pending). */
    request() {
      if (!this.frame) this.frame = requestAnimationFrame(() => this.step());
    }

    /* Ease toward the target, draw, and keep going only while something still moves. */
    step() {
      this.frame = 0;
      const snap = this.reduceMotion.matches;            /* Reduce Motion: jump, do not ease */
      let moving = false;
      for (const k of ['roll', 'pitch', 'yaw']) {
        const gap = this.target[k] - this.current[k];
        if (snap || Math.abs(gap) < 1e-4) this.current[k] = this.target[k];
        else { this.current[k] += gap * EASE; moving = true; }
      }
      const fadeGap = this.targetAlpha - this.alpha;
      if (snap || Math.abs(fadeGap) < 1e-3) this.alpha = this.targetAlpha;
      else { this.alpha += fadeGap * FADE_EASE; moving = true; }
      this.draw();
      if (moving) this.request();
    }

    draw() {
      const canvas = this.canvas, ctx = this.ctx;
      const dpr = window.devicePixelRatio || 1, W = canvas.clientWidth, H = canvas.clientHeight;
      if (!W || !H) return;                              /* hidden or not laid out yet       */
      const bw = Math.round(W * dpr), bh = Math.round(H * dpr);
      if (canvas.width !== bw || canvas.height !== bh) { canvas.width = bw; canvas.height = bh; }
      const zb = this.zbuffer;
      zb.resize(bw, bh);
      zb.clear();

      /* attitude -> rotation; the eye in body coordinates for the contour */
      const m = basis({ roll: this.current.roll * RANGE_DEG.roll * DEG,
                        pitch: this.current.pitch * RANGE_DEG.pitch * DEG,
                        yaw: this.current.yaw * RANGE_DEG.yaw * DEG });
      const eyeBody = toBody(m, EYE);
      const focal = Math.min(H * FOCAL_PER_HEIGHT, W * FOCAL_PER_WIDTH);
      /* world point -> [x, y] in CSS pixels and the distance along the view */
      const project = w => {
        const d = sub(w, EYE), z = dot(d, FORWARD), k = focal / z;
        return [W / 2 + dot(d, RIGHT) * k, H * CENTER_Y - dot(d, UP) * k, z];
      };

      /* every mesh vertex, once */
      const V = AF.mesh.vertices, sx = this.sx, sy = this.sy, iz = this.iz, wp = this.wp;
      for (let v = 0; v < V.length; v++) {
        const w = toWorld(m, V[v]), s = project(w);
        wp[3 * v] = w[0]; wp[3 * v + 1] = w[1]; wp[3 * v + 2] = w[2];
        sx[v] = s[0] * dpr; sy[v] = s[1] * dpr; iz[v] = 1 / s[2];
      }

      /* depth buffer: plates from both sides, closed surfaces front faces only */
      const T = AF.mesh.triangles, plate = AF.mesh.triPlate, part = AF.mesh.triPart;
      for (let t = 0; t < T.length; t += 3) {
        const a = T[t], b = T[t + 1], c = T[t + 2];
        if (!plate[t / 3]) {
          const A = [wp[3 * a], wp[3 * a + 1], wp[3 * a + 2]];
          const n = cross(sub([wp[3 * b], wp[3 * b + 1], wp[3 * b + 2]], A),
                          sub([wp[3 * c], wp[3 * c + 1], wp[3 * c + 2]], A));   /* outward */
          if (dot(n, sub(EYE, A)) <= 0) continue;        /* faces away: behind the front     */
        }
        zb.triangle(sx[a], sy[a], iz[a], sx[b], sy[b], iz[b], sx[c], sy[c], iz[c], part[t / 3]);
      }

      /* the lines: fuselage contour, then each part's outline */
      const path = new Path2D(), fuselage = 1 << AF.PART.FUSELAGE;
      for (const chain of P3.contour(eyeBody))
        P3.traceLine(path, chain.map(b => ({ w: toWorld(m, b), own: fuselage, junction: false, facing: 0 })),
                     project, zb, dpr);
      for (const loop of AF.loops)
        P3.traceLine(path, loop.map(o => ({ w: toWorld(m, o.b), own: o.own, junction: o.junction,
                                            facing: o.normal ? dot(o.normal, sub(eyeBody, o.b)) : 0 })),
                     project, zb, dpr);

      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);            /* draw in CSS pixels               */
      ctx.clearRect(0, 0, W, H);
      ctx.globalAlpha = this.alpha;
      ctx.strokeStyle = this.color;
      ctx.lineWidth = LINE_WIDTH;
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      ctx.stroke(path);                                  /* one stroke: even, no overlaps    */
    }
  }

  P3.View = View;
})(window.Plane3D = window.Plane3D || {});
