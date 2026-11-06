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

  /* Per-frame working storage, allocated once. An edge borders at most two cells and
   * each cell links it to one other edge, so an edge has at most two neighbours. */
  const NE = NZ * NT * 2;                          /* edge keys                        */
  const LINK = new Int32Array(NE * 2).fill(-1);    /* neighbours of edge k at 2k, 2k+1 */
  const SEEN = new Uint8Array(NE);                 /* already in a chain, this frame   */
  const TOUCHED = new Int32Array(NE);              /* edges linked this frame, in the
                                                      order first linked               */
  let touched = 0;
  const KEYS = new Int32Array(NE + 1);             /* the chain being walked           */
  const CROSSED = new Int32Array(4);               /* one cell's crossed edges         */

  /* q becomes a neighbour of p (the first free slot). */
  function addLink(p, q) {
    if (LINK[2 * p] < 0) { LINK[2 * p] = q; TOUCHED[touched++] = p; }
    else LINK[2 * p + 1] = q;
  }
  const link = (a, b) => { addLink(a, b); addLink(b, a); };

  /* Follow the links from `start` until they run out; the chain's surface points. */
  function walk(start) {
    let n = 0;
    KEYS[n++] = start;
    SEEN[start] = 1;
    let prev = -1, cur = start;
    for (;;) {
      const l0 = LINK[2 * cur], l1 = LINK[2 * cur + 1];   /* the next unseen neighbour */
      const next = l0 >= 0 && l0 !== prev && !SEEN[l0] ? l0
                 : l1 >= 0 && l1 !== prev && !SEEN[l1] ? l1 : -1;
      if (next < 0) {                              /* end of the chain              */
        if (n > 2 && (l0 === start || l1 === start)) KEYS[n++] = start;   /* closed     */
        break;
      }
      KEYS[n++] = next;
      SEEN[next] = 1;
      prev = cur;
      cur = next;
    }
    const points = new Array(n);
    for (let i = 0; i < n; i++) points[i] = crossing(KEYS[i]);
    const a = tipOf(KEYS[0]), b = tipOf(KEYS[n - 1]);
    if (a) points.unshift(a);                      /* close through the tip         */
    if (b && n > 1) points.push(b);
    return points;
  }

  function contour(eye) {
    /* last frame's links and marks off (only the edges it touched) */
    for (let i = 0; i < touched; i++) {
      const k = TOUCHED[i];
      LINK[2 * k] = LINK[2 * k + 1] = -1;
      SEEN[k] = 0;
    }
    touched = 0;

    /* g at every node for this eye position */
    for (let n = 0, o = 0; n < NZ * NT; n++, o += 3)
      G[n] = GN[o] * (GP[o] - eye[0]) + GN[o + 1] * (GP[o + 1] - eye[1]) + GN[o + 2] * (GP[o + 2] - eye[2]);

    /* marching squares: each cell links the edges where g changes sign */
    for (let i = 0; i < NZ - 1; i++) {
      for (let j = 0; j < NT; j++) {
        const j1 = (j + 1) % NT;
        /* corners counter-clockwise in (z, t): (i,j) (i+1,j) (i+1,j+1) (i,j+1) */
        const g0 = G[i * NT + j], g1 = G[(i + 1) * NT + j], g2 = G[(i + 1) * NT + j1], g3 = G[i * NT + j1];
        const s0 = g0 > 0, s1 = g1 > 0, s2 = g2 > 0, s3 = g3 > 0;
        if (s0 === s1 && s1 === s2 && s2 === s3) continue;          /* no crossing      */
        const e0 = zEdge(i, j), e1 = tEdge(i + 1, j), e2 = zEdge(i, j1), e3 = tEdge(i, j);
        let crossed = 0;
        if (s0 !== s1) CROSSED[crossed++] = e0;
        if (s1 !== s2) CROSSED[crossed++] = e1;
        if (s2 !== s3) CROSSED[crossed++] = e2;
        if (s3 !== s0) CROSSED[crossed++] = e3;
        if (crossed === 2) link(CROSSED[0], CROSSED[1]);
        /* a saddle (4 crossings): the cell's mean decides which corners connect */
        else if ((g0 + g1 + g2 + g3 > 0) === s0) { link(e0, e1); link(e2, e3); }
        else { link(e3, e0); link(e1, e2); }
      }
    }

    /* chain the links into polylines: open chains (ending on a tip row) first, then the
     * closed ones, each started from the edge linked first (the order TOUCHED keeps) */
    const chains = [];
    for (let i = 0; i < touched; i++) {
      const k = TOUCHED[i];
      if (LINK[2 * k + 1] < 0 && !SEEN[k]) chains.push(walk(k));   /* one neighbour: an end */
    }
    for (let i = 0; i < touched; i++) if (!SEEN[TOUCHED[i]]) chains.push(walk(TOUCHED[i]));
    return chains;
  }

  P3.contour = contour;
})(window.Plane3D = window.Plane3D || {});
