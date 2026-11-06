/* Plane3D - lines.js: turns 3D polylines into the visible pieces of 2D lines.
 *
 * Every segment is sampled about every 0.7 device pixels; each sample is projected and
 * tested against the depth buffer. Runs of visible samples become subpaths of one
 * Path2D, so the whole drawing is stroked once, with one color and width: no line is
 * painted twice, and nothing flickers where lines meet. A run is extended half a sample
 * into the hidden part, which tucks its end under the occluding part's own outline.
 *
 * Plane3D.traceLine(path, vertices, project, zbuffer, dpr)
 *   vertices: [{ w: world point, own: part mask, junction: bool, facing: number }]
 *             facing > 0 means the fuselage faces the eye there (junction points only)
 *   project:  world point -> [x, y, z] in CSS pixels, z = distance along the view
 * No dependencies. */
(function (P3) {
  'use strict';

  const STEP_PX = 0.7;   /* sample spacing, device pixels                       */
  const EPS = 0.04;      /* a nearer part must be this much nearer to hide a line */

  function traceLine(path, vs, project, zbuffer, dpr) {
    let pen = false;                   /* inside a visible run                           */
    let px = 0, py = 0;                /* the previous sample, CSS pixels                 */
    for (let q = 0; q < vs.length - 1; q++) {
      const a = vs[q], b = vs[q + 1];
      const sa = project(a.w), sb = project(b.w);
      const n = Math.max(1, Math.ceil(Math.hypot(sb[0] - sa[0], sb[1] - sa[1]) * dpr / STEP_PX));
      const own = a.own & b.own;       /* parts that may not hide this segment           */
      const junction = a.junction && b.junction;   /* both ends on the fuselage           */
      for (let k = q ? 1 : 0; k <= n; k++) {       /* the first sample once, then onward  */
        const t = k / n;
        const [x, y, z] = project([a.w[0] + (b.w[0] - a.w[0]) * t,        /* straight in 3D */
                                   a.w[1] + (b.w[1] - a.w[1]) * t,
                                   a.w[2] + (b.w[2] - a.w[2]) * t]);
        let visible = !zbuffer.hides(x * dpr, y * dpr, z, own, EPS);
        /* a junction on the far side of the fuselage is behind it, though no other part is */
        if (visible && junction && a.facing + (b.facing - a.facing) * t <= 0) visible = false;
        if (visible) {
          if (!pen) {                                      /* a run starts               */
            if (q === 0 && k === 0) path.moveTo(x, y);     /* at the very first sample   */
            else path.moveTo((px + x) / 2, (py + y) / 2);  /* half a step back            */
            pen = true;
          }
          path.lineTo(x, y);
        } else if (pen) {                                  /* a run ends half a step on   */
          path.lineTo((px + x) / 2, (py + y) / 2);
          pen = false;
        }
        px = x;
        py = y;
      }
    }
  }

  P3.traceLine = traceLine;
})(window.Plane3D = window.Plane3D || {});
