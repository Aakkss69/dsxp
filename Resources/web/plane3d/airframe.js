/* Plane3D - airframe.js: the aircraft's shape, built once when the script loads.
 *
 * What it provides (Plane3D.airframe):
 *   fuselage   a smooth surface: at(z, t) -> [point, outward normal]; tips; helpers
 *   mesh       triangles for the hidden-line depth buffer: the fuselage as a closed
 *              48 x 48 tube, the wings, tailplane and fin as zero-thickness plates on
 *              their mid-planes (so an occluder's edge is exactly the line drawn for it)
 *   loops      one closed outline per wing half, tailplane half and fin: leading edge,
 *              rounded tip, trailing edge, then the junction curve where it meets the
 *              fuselage, which closes the loop
 *   PART       part ids used in the depth buffer (0 = background)
 *
 * Units are arbitrary (the fuselage is 5.4 long); body frame x right wing, y up, z nose.
 * Requires math.js. */
(function (P3) {
  'use strict';
  const { sub, dot, cross, mid, pchip } = P3.math;

  /* Depth-buffer part ids. A line is never hidden by its own part (lines.js). */
  const PART = { FUSELAGE: 1, WING_R: 2, WING_L: 3, TAIL_R: 4, TAIL_L: 5, FIN: 6 };

  /* ------------------------------------------------------------- fuselage */

  /* Stations from nose to tail: [z, half-width rx, half-height ry, centerline height yo].
   * Elliptical sections; rx = ry = 0 closes the nose and the tail cone. The tail sweeps
   * up slightly (yo grows aft), as on a real fuselage. */
  const STATIONS = [
    [2.48, 0, 0, 0], [2.43, .07, .065, 0], [2.34, .14, .13, 0], [2.2, .2, .19, 0],
    [2.0, .26, .25, 0], [1.7, .31, .31, 0], [1.3, .335, .35, 0], [0.7, .345, .37, 0],
    [0.0, .34, .37, 0], [-0.6, .31, .34, .01], [-1.2, .25, .28, .03], [-1.75, .18, .2, .05],
    [-2.25, .11, .13, .07], [-2.62, .06, .075, .085], [-2.82, .03, .04, .09], [-2.9, 0, 0, .09],
  ].reverse();                                    /* PCHIP wants z ascending: tail first */

  const Z = STATIONS.map(r => r[0]);
  const RX = pchip(Z, STATIONS.map(r => r[1]));   /* z -> [half-width, slope]        */
  const RY = pchip(Z, STATIONS.map(r => r[2]));   /* z -> [half-height, slope]       */
  const YO = pchip(Z, STATIONS.map(r => r[3]));   /* z -> [centerline height, slope] */
  const Z_TAIL = Z[0], Z_NOSE = Z[Z.length - 1];

  /*
   * Surface point and outward normal at station z, angle t around it (t = 0 at the right
   * side, pi/2 on top). P(z, t) = (rx cos t, yo + ry sin t, z). The normal is
   * dP/dt x dP/dz; with dP/dt = (-rx sin t, ry cos t, 0) and
   * dP/dz = (rx' cos t, yo' + ry' sin t, 1) it expands to the expression below
   * (checked: at t = 0 it points along +x, outward).
   */
  function fuselageAt(z, t) {
    const [rx, drx] = RX(z), [ry, dry] = RY(z), [yo, dyo] = YO(z);
    const c = Math.cos(t), s = Math.sin(t);
    return [[rx * c, yo + ry * s, z],
            [ry * c, rx * s, -rx * s * (dyo + dry * s) - ry * drx * c * c]];
  }

  /* Half-width of the fuselage at height y and station z (0 where y misses it). */
  const halfWidthAt = y => z => {
    const rx = RX(z)[0], ry = RY(z)[0];
    const q = ry > 0 ? (y - YO(z)[0]) / ry : 2;    /* 2: outside the section       */
    return q * q < 1 ? rx * Math.sqrt(1 - q * q) : 0;
  };

  /* Height of the fuselage's top at station z (where the fin stands). */
  const topAt = z => YO(z)[0] + RY(z)[0];

  /* Outward normal of the fuselage at a point lying on it. */
  function normalAtPoint(p) {
    const z = p[2];
    const t = Math.atan2((p[1] - YO(z)[0]) / RY(z)[0], p[0] / RX(z)[0]);   /* back to the angle */
    return fuselageAt(z, t)[1];
  }

  /* --------------------------------------------------------- mesh building */

  const vertices = [];      /* body-frame points                                      */
  const triangles = [];     /* vertex index triples, flattened                          */
  const triPart = [];       /* part id per triangle                                     */
  const triPlate = [];      /* 1 = a plate (drawn from both sides), 0 = a closed surface */
  const addVertex = p => vertices.push(p) - 1;

  /*
   * Join rings of vertex indices into a tube: quads between consecutive rings, split
   * into triangles, each turned to face away from `centers` (the ring centers), so back
   * faces can be culled by winding. A collapsed ring (one vertex repeated) makes a cone.
   */
  function tube(rings, centers, part) {
    const K = rings[0].length;
    for (let s = 0; s < rings.length - 1; s++) {
      const axis = mid(centers[s], centers[s + 1]);          /* a point inside the band */
      for (let k = 0; k < K; k++) {
        const quad = [rings[s][k], rings[s][(k + 1) % K], rings[s + 1][(k + 1) % K], rings[s + 1][k]]
          .filter((v, i, a) => v !== a[(i + a.length - 1) % a.length]);   /* drop repeats */
        for (let t = 1; t < quad.length - 1; t++) {
          let a = quad[0], b = quad[t], c = quad[t + 1];
          const A = vertices[a], B = vertices[b], C = vertices[c];
          const n = cross(sub(B, A), sub(C, A));
          if (Math.hypot(n[0], n[1], n[2]) < 1e-12) continue;  /* degenerate: skip     */
          const centroid = [(A[0] + B[0] + C[0]) / 3, (A[1] + B[1] + C[1]) / 3, (A[2] + B[2] + C[2]) / 3];
          if (dot(n, sub(centroid, axis)) < 0) { const x = b; b = c; c = x; }   /* face out */
          triangles.push(a, b, c);
          triPart.push(part);
          triPlate.push(0);
        }
      }
    }
  }

  /* The fuselage as a closed 48 x 48 tube for the depth buffer. Stations are spaced
   * with a cosine, tighter at the nose and tail where the shape changes fastest. */
  {
    const ROWS = 48, AROUND = 48, rings = [], centers = [];
    for (let i = 0; i < ROWS; i++) {
      const z = Z_TAIL + (Z_NOSE - Z_TAIL) * (1 - Math.cos(Math.PI * i / (ROWS - 1))) / 2;
      const center = [0, YO(z)[0], z];
      centers.push(center);
      if (i === 0 || i === ROWS - 1) rings.push(Array(AROUND).fill(addVertex(center)));   /* tips */
      else rings.push(Array.from({ length: AROUND }, (_, k) => addVertex(fuselageAt(z, 2 * Math.PI * k / AROUND)[0])));
    }
    tube(rings, centers, PART.FUSELAGE);
  }

  /* ---------------------------------------------- wings, tailplane and fin */

  /* The rounded tips: angles from the tip chord's middle, front to tip point. */
  const ARC = [0, 15, 30, 45, 58, 70, 80, 87, 90].map(d => d * Math.PI / 180);

  /* Planforms: leading edge (le) and trailing edge (te) z as functions of the span
   * coordinate u (x for the wing and tailplane, height y for the fin). Each starts inside
   * the fuselage, so the visible root is exactly where it meets the fuselage. */
  const wingLE = x => .55 - (x - .3) / 2.45 * .36,  wingTE = x => -.45 + (x - .3) / 2.45 * .22;
  const tailLE = x => -2.1 - (x - .1) / .9 * .12,   tailTE = x => -2.62 + (x - .1) / .9 * .06;
  const finLE = y => -1.95 - (y - .18) / .6 * .47,  finTE = y => -2.8 + (y - .18) / .6 * .07;

  /* Map a planform point (u, z) into the body frame, and the fuselage edge the part is
   * clipped against: for a horizontal surface at height yc, u = |x| and the edge is the
   * fuselage's half-width there; for the fin, u = y and the edge is the fuselage top. */
  const horizontal = (side, yc) => ({ plan: (u, z) => [side * u, yc, z], edge: halfWidthAt(yc) });
  const vertical = { plan: (u, z) => [0, u, z], edge: topAt };

  const WING = { spans: [.2, .8, 1.4, 2, 2.45, 2.75], le: wingLE, te: wingTE };
  const TAIL = { spans: [0, .45, .8, 1], le: tailLE, te: tailTE };
  const PARTS = [
    { part: PART.WING_R, ...WING, ...horizontal(1, .15) },
    { part: PART.WING_L, ...WING, ...horizontal(-1, .15) },
    { part: PART.TAIL_R, ...TAIL, ...horizontal(1, .06) },
    { part: PART.TAIL_L, ...TAIL, ...horizontal(-1, .06) },
    { part: PART.FIN, spans: [.08, .4, .6, .78], le: finLE, te: finTE, ...vertical },
  ];

  const loops = [];

  for (const P of PARTS) {
    const tipU = P.spans[P.spans.length - 1];              /* where the rounded tip starts */
    const h0 = (P.le(tipU) - P.te(tipU)) / 2;              /* half chord there = tip radius */
    const m0 = (P.le(tipU) + P.te(tipU)) / 2;              /* mid chord there               */
    const inner = P.spans.slice(0, -1);

    /* stations [u, mid chord, half chord] root to tip, then around the rounded tip */
    const stations = inner.map(u => [u, (P.le(u) + P.te(u)) / 2, (P.le(u) - P.te(u)) / 2]);
    for (const a of ARC) stations.push([tipU + h0 * Math.sin(a), m0, h0 * Math.cos(a)]);

    /* the plate: leading and trailing edge vertices per station, quads between them */
    const edges = stations.map(([u, m, h]) => h < 1e-4 ? [addVertex(P.plan(u, m))]
                                                       : [addVertex(P.plan(u, m + h)), addVertex(P.plan(u, m - h))]);
    for (let i = 0; i < edges.length - 1; i++) {
      const a = edges[i], b = edges[i + 1];
      const quad = [a[0], b[0], b[b.length - 1], a[a.length - 1]]
        .filter((v, j, q) => v !== q[(j + q.length - 1) % q.length]);
      for (let t = 1; t < quad.length - 1; t++) {
        triangles.push(quad[0], quad[t], quad[t + 1]);
        triPart.push(P.part);
        triPlate.push(1);                                  /* seen from either side       */
      }
    }

    /* the outline: leading edge out, around the tip, trailing edge back (a closed loop
     * in planform coordinates [u, z]) ... */
    const raw = inner.map(u => [u, P.le(u)]);
    for (const a of ARC) raw.push([tipU + h0 * Math.sin(a), m0 + h0 * Math.cos(a)]);
    for (let i = ARC.length - 2; i >= 0; i--) raw.push([tipU + h0 * Math.sin(ARC[i]), m0 - h0 * Math.cos(ARC[i])]);
    for (let i = inner.length - 1; i >= 0; i--) raw.push([inner[i], P.te(inner[i])]);
    /* ... resampled every 0.02 so the clip below finds the fuselage edge precisely ... */
    const Q = [];
    raw.forEach((a, i) => {
      const b = raw[(i + 1) % raw.length];
      const n = Math.max(1, Math.ceil(Math.hypot(b[0] - a[0], b[1] - a[1]) / .02));
      for (let k = 0; k < n; k++) Q.push([a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n]);
    });
    /* ... then cut where it enters the fuselage: F > 0 outside, F <= 0 inside */
    const n = Q.length, F = Q.map(p => p[0] - P.edge(p[1]));
    let s = 0;
    while (s < n && !(F[(s + n - 1) % n] <= 0 && F[s] > 0)) s++;   /* where it comes out */
    const cut = (i0, i1) => {                              /* the crossing, on the fuselage */
      const z = Q[i0][1] + (Q[i1][1] - Q[i0][1]) * F[i0] / (F[i0] - F[i1]);
      return [P.edge(z), z];
    };
    const A = cut((s + n - 1) % n, s);                     /* leading-edge root            */
    const points = [[A, true]];                            /* [planform point, on fuselage] */
    let i = s;
    while (F[i] > 0) { points.push([Q[i], false]); i = (i + 1) % n; }
    const B = cut((i + n - 1) % n, i);                     /* trailing-edge root           */
    points.push([B, true]);
    for (let k = 1; k < 24; k++) {                         /* the junction, back to A      */
      const z = B[1] + (A[1] - B[1]) * k / 24;
      points.push([[P.edge(z), z], true]);
    }
    points.push([A, true]);                                /* closed                        */

    /* In body coordinates. `own` = part ids that never hide this point; a junction point
     * lies on the fuselage too, so it also has the fuselage's normal, and is drawn only
     * where that surface faces the eye (lines.js). */
    const self = 1 << P.part;
    loops.push(points.map(([p, onFuselage]) => {
      const b = P.plan(p[0], p[1]);
      return { b, own: onFuselage ? self | (1 << PART.FUSELAGE) : self,
               junction: onFuselage, normal: onFuselage ? normalAtPoint(b) : null };
    }));
  }

  P3.airframe = {
    PART,
    fuselage: { at: fuselageAt, zTail: Z_TAIL, zNose: Z_NOSE, centerAt: z => YO(z)[0] },
    mesh: { vertices, triangles, triPart, triPlate },
    loops,
  };
})(window.Plane3D = window.Plane3D || {});
