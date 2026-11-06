/* Plane3D - contour.js: the fuselage's outline as the eye sees it.
 *
 * The outline of a smooth surface is where its normal is at right angles to the line of
 * sight: g(z, t) = n(z, t) . (P(z, t) - eye) = 0 (g < 0 faces the eye). The surface is
 * sampled once on a 100 x 96 grid of (station z, angle t); each frame only g is
 * recomputed, with the eye moved into the body frame, and the zero line is traced with
 * marching squares (W. E. Lorensen, H. E. Cline, "Marching Cubes", SIGGRAPH 1987, in its
 * 2D form), each crossing placed by linear interpolation and then evaluated exactly on
 * the surface. The result is one smooth closed loop for this convex body, the front of
 * the fuselage included, that never jumps from facet to facet.
 *
 * Plane3D.contour(eyeBody) -> array of polylines (body-frame points).
 * Requires math.js, airframe.js. */
(function (P3) {
  'use strict';
  const F = P3.airframe.fuselage;

  const NZ = 100, NT = 96;                         /* stations x angles               */
  const GZ = new Float64Array(NZ);                 /* station z per row               */
  const GP = new Float64Array(NZ * NT * 3);        /* surface point per node          */
  const GN = new Float64Array(NZ * NT * 3);        /* surface normal per node         */
  const G = new Float64Array(NZ * NT);             /* g per node, this frame          */

  /* Grid rows cosine-spaced (tight near the nose and tail) and kept 0.002 short of the
   * tips, where the normal vanishes; a chain that reaches a tip is closed through it. */
  for (let i = 0; i < NZ; i++) {
    GZ[i] = F.zTail + .002 + (F.zNose - F.zTail - .004) * (1 - Math.cos(Math.PI * i / (NZ - 1))) / 2;
    for (let j = 0; j < NT; j++) {
      const [p, n] = F.at(GZ[i], 2 * Math.PI * j / NT), o = (i * NT + j) * 3;
      GP.set(p, o);
      GN.set(n, o);
    }
  }

  /* Edge keys: (node index * 2) for the edge to the next station (z-edge),
   * (node index * 2 + 1) for the edge to the next angle (t-edge, wrapping around). */
  const zEdge = (i, j) => (i * NT + j) * 2;
  const tEdge = (i, j) => (i * NT + j) * 2 + 1;

  /* The surface point where g crosses zero on edge `key`. */
  function crossing(key) {
    const n = key >> 1, i = (n / NT) | 0, j = n % NT;
    if (key & 1) {                                  /* t-edge: (i, j) -> (i, j+1)     */
      const g1 = G[i * NT + (j + 1) % NT], f = G[n] / (G[n] - g1);
      return F.at(GZ[i], 2 * Math.PI * (j + f) / NT)[0];
    }
    const f = G[n] / (G[n] - G[n + NT]);            /* z-edge: (i, j) -> (i+1, j)     */
    return F.at(GZ[i] + f * (GZ[i + 1] - GZ[i]), 2 * Math.PI * j / NT)[0];
  }

  /* The tip a chain ending on the first or last row reaches (or null). */
  function tipOf(key) {
    if (!(key & 1)) return null;                    /* only t-edges lie on a boundary row */
    const i = ((key >> 1) / NT) | 0;
    if (i === 0) return [0, F.centerAt(F.zTail), F.zTail];
    if (i === NZ - 1) return [0, F.centerAt(F.zNose), F.zNose];
    return null;
  }

  function contour(eye) {
    /* g at every node for this eye position */
    for (let n = 0, o = 0; n < NZ * NT; n++, o += 3)
      G[n] = GN[o] * (GP[o] - eye[0]) + GN[o + 1] * (GP[o + 1] - eye[1]) + GN[o + 2] * (GP[o + 2] - eye[2]);

    /* marching squares: each cell links the edges where g changes sign */
    const adjacent = new Map();
    const link = (a, b) => {
      for (const [p, q] of [[a, b], [b, a]]) {
        let list = adjacent.get(p);
        if (!list) adjacent.set(p, list = []);
        list.push(q);
      }
    };
    for (let i = 0; i < NZ - 1; i++) {
      for (let j = 0; j < NT; j++) {
        const j1 = (j + 1) % NT;
        /* corners counter-clockwise in (z, t): (i,j) (i+1,j) (i+1,j+1) (i,j+1) */
        const g0 = G[i * NT + j], g1 = G[(i + 1) * NT + j], g2 = G[(i + 1) * NT + j1], g3 = G[i * NT + j1];
        const s0 = g0 > 0, s1 = g1 > 0, s2 = g2 > 0, s3 = g3 > 0;
        if (s0 === s1 && s1 === s2 && s2 === s3) continue;          /* no crossing      */
        const e0 = zEdge(i, j), e1 = tEdge(i + 1, j), e2 = zEdge(i, j1), e3 = tEdge(i, j);
        const crossed = [];
        if (s0 !== s1) crossed.push(e0);
        if (s1 !== s2) crossed.push(e1);
        if (s2 !== s3) crossed.push(e2);
        if (s3 !== s0) crossed.push(e3);
        if (crossed.length === 2) link(crossed[0], crossed[1]);
        /* a saddle (4 crossings): the cell's mean decides which corners connect */
        else if ((g0 + g1 + g2 + g3 > 0) === s0) { link(e0, e1); link(e2, e3); }
        else { link(e3, e0); link(e1, e2); }
      }
    }

    /* chain the links into polylines: open chains (ending on a tip row) first */
    const seen = new Set(), chains = [];
    const walk = start => {
      const keys = [start];
      seen.add(start);
      let prev = -1, cur = start;
      for (;;) {
        const next = adjacent.get(cur).find(k => k !== prev && !seen.has(k));
        if (next === undefined) {                    /* end of the chain              */
          if (keys.length > 2 && adjacent.get(cur).includes(start)) keys.push(start);   /* closed */
          break;
        }
        keys.push(next);
        seen.add(next);
        prev = cur;
        cur = next;
      }
      const points = keys.map(crossing);
      const a = tipOf(keys[0]), b = tipOf(keys[keys.length - 1]);
      if (a) points.unshift(a);                      /* close through the tip         */
      if (b && keys.length > 1) points.push(b);
      return points;
    };
    for (const [k, v] of adjacent) if (v.length === 1 && !seen.has(k)) chains.push(walk(k));
    for (const k of adjacent.keys()) if (!seen.has(k)) chains.push(walk(k));
    return chains;
  }

  P3.contour = contour;
})(window.Plane3D = window.Plane3D || {});
