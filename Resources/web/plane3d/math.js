/* Plane3D - math.js: the small amount of linear algebra and interpolation the 3D plane
 * needs. Pure functions, no state. Vectors are plain [x, y, z] arrays.
 *
 * Frames used by the plane (see view.js):
 *   body   x right wing, y up, z nose            (the airframe is modelled in this frame)
 *   world  the body frame after roll, pitch and yaw; the camera looks at its origin
 *
 * Loaded first; the other plane3d/*.js files read Plane3D.math. */
(function (P3) {
  'use strict';

  /* a - b */
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];

  /* a . b */
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

  /* a x b (right-handed component formula; the body frame above is left-handed, so
   * callers pick operand order by checking the result's direction, as noted where used) */
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1],
                           a[2] * b[0] - a[0] * b[2],
                           a[0] * b[1] - a[1] * b[0]];

  /* a scaled to unit length; a zero vector stays zero */
  const norm = a => {
    const l = Math.hypot(a[0], a[1], a[2]) || 1;     /* 1 avoids dividing by zero */
    return [a[0] / l, a[1] / l, a[2] / l];
  };

  /* midpoint of a and b */
  const mid = (a, b) => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2];

  /*
   * Monotone cubic interpolation (PCHIP, Fritsch-Carlson slopes with the three-point end
   * rule, as in SciPy's PchipInterpolator). Unlike an ordinary cubic spline it never
   * overshoots the data, so a radius that falls to 0 at the nose never goes negative.
   * xs ascending. Returns z -> [value, slope]; z outside the data is clamped to the ends.
   */
  function pchip(xs, ys) {
    const n = xs.length, h = [], d = [], m = [];
    for (let k = 0; k < n - 1; k++) {
      h[k] = xs[k + 1] - xs[k];                       /* interval width            */
      d[k] = (ys[k + 1] - ys[k]) / h[k];              /* secant slope              */
    }
    /* end slope from the two nearest intervals, kept shape-preserving */
    const edge = (h0, h1, d0, d1) => {
      let s = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
      if (Math.sign(s) !== Math.sign(d0)) s = 0;      /* would turn the wrong way  */
      else if (Math.sign(d0) !== Math.sign(d1) && Math.abs(s) > Math.abs(3 * d0)) s = 3 * d0;
      return s;
    };
    m[0] = edge(h[0], h[1], d[0], d[1]);
    m[n - 1] = edge(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
    for (let k = 1; k < n - 1; k++) {
      if (d[k - 1] * d[k] <= 0) m[k] = 0;             /* local extremum: flat      */
      else {                                          /* weighted harmonic mean    */
        const w1 = 2 * h[k] + h[k - 1], w2 = h[k] + 2 * h[k - 1];
        m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
      }
    }
    return z => {
      let k = 0;
      while (k < n - 2 && z > xs[k + 1]) k++;         /* the interval holding z    */
      const hk = h[k], t = Math.min(1, Math.max(0, (z - xs[k]) / hk));
      const t2 = t * t, t3 = t2 * t;
      /* cubic Hermite basis and its derivative */
      const v = (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * hk * m[k] +
                (3 * t2 - 2 * t3) * ys[k + 1] + (t3 - t2) * hk * m[k + 1];
      const dv = ((6 * t2 - 6 * t) * ys[k] + (3 * t2 - 4 * t + 1) * hk * m[k] +
                  (6 * t - 6 * t2) * ys[k + 1] + (3 * t2 - 2 * t) * hk * m[k + 1]) / hk;
      return [v, dv];
    };
  }

  /*
   * Rotate a body vector into the world: roll about the nose axis, then pitch about the
   * wing axis, then yaw about the vertical (angles in radians; positive = right wing
   * down, nose up, nose right, as X-Plane's yoke ratios).
   */
  function rotate(v, a) {
    let [x, y, z] = v;
    let c = Math.cos(-a.roll), s = Math.sin(-a.roll);
    [x, y] = [x * c - y * s, x * s + y * c];          /* roll: right wing goes down  */
    c = Math.cos(-a.pitch); s = Math.sin(-a.pitch);
    [y, z] = [y * c - z * s, y * s + z * c];          /* pitch: nose goes up         */
    c = Math.cos(a.yaw); s = Math.sin(a.yaw);
    [x, z] = [x * c + z * s, -x * s + z * c];         /* yaw: nose goes right        */
    return [x, y, z];
  }

  /* The rotation as a matrix: its columns are the body axes in the world, so
   * world = m[0]*x + m[1]*y + m[2]*z and body = (world . m[0], world . m[1], world . m[2]). */
  function basis(a) {
    return [rotate([1, 0, 0], a), rotate([0, 1, 0], a), rotate([0, 0, 1], a)];
  }

  /* body -> world with a basis from basis() */
  const toWorld = (m, b) => [m[0][0] * b[0] + m[1][0] * b[1] + m[2][0] * b[2],
                             m[0][1] * b[0] + m[1][1] * b[1] + m[2][1] * b[2],
                             m[0][2] * b[0] + m[1][2] * b[1] + m[2][2] * b[2]];

  /* world -> body (the inverse of a rotation is its transpose) */
  const toBody = (m, w) => [dot(w, m[0]), dot(w, m[1]), dot(w, m[2])];

  P3.math = { sub, dot, cross, norm, mid, pchip, rotate, basis, toWorld, toBody };
})(window.Plane3D = window.Plane3D || {});
