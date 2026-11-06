/* Plane3D - view.js: draws the aircraft in a canvas, attitude = what DSXP sent to X-Plane.
 *
 *   const view = new Plane3D.View(canvas);
 *   view.show({ valid, roll, pitch, yaw });   // -1..1 each, as sent; valid = engaged
 *
 * Chase view from behind and above. +-100 % sent draws as 45 deg bank, 25 deg pitch and
 * 30 deg yaw (rotation roll, then pitch, then yaw). While nothing is being sent (valid
 * false) the aircraft eases to level and its lines fade to 40 %. The drawing eases toward
 * each new value in about 70 ms, and the canvas is redrawn only while something moves
 * (a /api/state poll that changes nothing draws nothing). Per-frame work reuses buffers
 * made once here, so drawing allocates little beyond the Path2D and the contour.
 * The line color is the canvas's CSS `color` (the panel sets it to the label color), read
 * again when the appearance changes.
 *
 * Each frame: rotate, fill the depth buffer (zbuffer.js), trace the fuselage contour
 * (contour.js) and the part outlines (airframe.js) through it (lines.js), stroke once.
 * Requires math.js, airframe.js, contour.js, zbuffer.js, lines.js. */
(function (P3) {
  'use strict';
  const { sub, cross, norm, basis, toWorldTo, toBody } = P3.math;
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
      this.w = new Float64Array(3);                      /* one point, world and projected     */
      this.s = new Float64Array(3);
      /* the part outlines' vertices: their world position and facing change per frame */
      this.loops = AF.loops.map(loop => loop.map(o => ({ w: new Float64Array(3), own: o.own,
                                                          junction: o.junction, facing: 0 })));
      this.chain = [];                                   /* contour vertices, grown as needed  */
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

    /* New values from /api/state. Anything outside -1..1 is clamped. A frame is asked for
     * only when the target changed: the poll repeats the same values ~20 times a second. */
    show(sent) {
      const valid = !!(sent && sent.valid);
      const c = v => Math.max(-1, Math.min(1, Number(v) || 0));
      const target = valid ? { roll: c(sent.roll), pitch: c(sent.pitch), yaw: c(sent.yaw) }
                           : { roll: 0, pitch: 0, yaw: 0 };   /* standing by: level            */
      const alpha = valid ? 1 : STANDBY_ALPHA;
      const t = this.target;
      if (target.roll === t.roll && target.pitch === t.pitch && target.yaw === t.yaw &&
          alpha === this.targetAlpha) return;            /* nothing new to draw             */
      this.target = target;
      this.targetAlpha = alpha;
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
      /* world point -> out = [x, y] in CSS pixels and the distance along the view (the
       * dot products written out, in the same order as dot(): no arrays per call) */
      const E0 = EYE[0], E1 = EYE[1], E2 = EYE[2];
      const project = (w, out) => {
        const d0 = w[0] - E0, d1 = w[1] - E1, d2 = w[2] - E2;
        const z = d0 * FORWARD[0] + d1 * FORWARD[1] + d2 * FORWARD[2], k = focal / z;
        out[0] = W / 2 + (d0 * RIGHT[0] + d1 * RIGHT[1] + d2 * RIGHT[2]) * k;
        out[1] = H * CENTER_Y - (d0 * UP[0] + d1 * UP[1] + d2 * UP[2]) * k;
        out[2] = z;
        return out;
      };

      /* every mesh vertex, once */
      const V = AF.mesh.vertices, sx = this.sx, sy = this.sy, iz = this.iz, wp = this.wp;
      const w = this.w, s = this.s;
      for (let v = 0; v < V.length; v++) {
        toWorldTo(m, V[v], w);
        project(w, s);
        wp[3 * v] = w[0]; wp[3 * v + 1] = w[1]; wp[3 * v + 2] = w[2];
        sx[v] = s[0] * dpr; sy[v] = s[1] * dpr; iz[v] = 1 / s[2];
      }

      /* depth buffer: plates from both sides, closed surfaces front faces only */
      const T = AF.mesh.triangles, plate = AF.mesh.triPlate, part = AF.mesh.triPart;
      for (let t = 0; t < T.length; t += 3) {
        const a = T[t], b = T[t + 1], c = T[t + 2];
        if (!plate[t / 3]) {
          /* outward normal n = (B - A) x (C - A), then n . (eye - A), as cross() and dot() */
          const ax = wp[3 * a], ay = wp[3 * a + 1], az = wp[3 * a + 2];
          const ux = wp[3 * b] - ax, uy = wp[3 * b + 1] - ay, uz = wp[3 * b + 2] - az;
          const vx = wp[3 * c] - ax, vy = wp[3 * c + 1] - ay, vz = wp[3 * c + 2] - az;
          const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
          if (nx * (E0 - ax) + ny * (E1 - ay) + nz * (E2 - az) <= 0) continue;   /* faces away */
        }
        zb.triangle(sx[a], sy[a], iz[a], sx[b], sy[b], iz[b], sx[c], sy[c], iz[c], part[t / 3]);
      }

      /* the lines: fuselage contour, then each part's outline */
      const path = new Path2D(), fuselage = 1 << AF.PART.FUSELAGE;
      const pool = this.chain;
      for (const chain of P3.contour(eyeBody)) {
        while (pool.length < chain.length)               /* grow the pool once, reuse after */
          pool.push({ w: new Float64Array(3), own: fuselage, junction: false, facing: 0 });
        for (let i = 0; i < chain.length; i++) toWorldTo(m, chain[i], pool[i].w);
        P3.traceLine(path, pool, project, zb, dpr, chain.length);
      }
      AF.loops.forEach((loop, l) => {
        const vs = this.loops[l];
        for (let i = 0; i < loop.length; i++) {
          const o = loop[i], v = vs[i], b = o.b, n = o.normal;
          toWorldTo(m, b, v.w);
          v.facing = n ? n[0] * (eyeBody[0] - b[0]) + n[1] * (eyeBody[1] - b[1]) + n[2] * (eyeBody[2] - b[2]) : 0;
        }
        P3.traceLine(path, vs, project, zb, dpr);
      });

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
