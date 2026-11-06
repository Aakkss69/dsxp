/* Plane3D - zbuffer.js: a depth + part-id buffer at device-pixel resolution, used only
 * to decide which parts of the outline lines are hidden. Nothing in it is ever shown.
 *
 * Depth is stored as 1/z (z = distance along the view direction), which is linear in
 * screen space, so plain barycentric interpolation is perspective-correct; larger means
 * nearer, and 0 means empty.
 *
 * new Plane3D.ZBuffer() ; zb.resize(w, h) ; zb.clear() ; zb.triangle(...) ; zb.hides(...)
 * No dependencies. */
(function (P3) {
  'use strict';

  class ZBuffer {
    constructor() {
      this.width = 0;
      this.height = 0;
      this.depth = new Float32Array(0);           /* 1/z of the nearest surface per pixel */
      this.part = new Uint8Array(0);              /* part id of that surface, 0 = none    */
    }

    /* Match the canvas's backing store; reallocates only when the size changes. */
    resize(width, height) {
      if (width === this.width && height === this.height) return;
      this.width = width;
      this.height = height;
      this.depth = new Float32Array(width * height);
      this.part = new Uint8Array(width * height);
    }

    clear() {
      this.depth.fill(0);
      this.part.fill(0);
    }

    /*
     * Rasterize one triangle given in device pixels (x, y) with 1/z per corner. A pixel
     * belongs to it when its center lies inside (a tiny tolerance closes hairline gaps
     * between neighbours). The barycentric weights are linear in x and y, so each one is
     * stepped by a constant per pixel instead of being recomputed.
     */
    triangle(x0, y0, iz0, x1, y1, iz1, x2, y2, iz2, part) {
      const area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);   /* twice the signed area */
      if (Math.abs(area) < 1e-9) return;                             /* edge-on: covers nothing */
      const W = this.width, H = this.height;
      const minX = Math.max(0, Math.floor(Math.min(x0, x1, x2)));
      const maxX = Math.min(W - 1, Math.ceil(Math.max(x0, x1, x2)));
      const minY = Math.max(0, Math.floor(Math.min(y0, y1, y2)));
      const maxY = Math.min(H - 1, Math.ceil(Math.max(y0, y1, y2)));
      if (minX > maxX || minY > maxY) return;                        /* off screen        */
      /* weight of corner 0 and of corner 1 as A*px + B*py + C (corner 2 = 1 - both) */
      const A0 = (y1 - y2) / area, B0 = (x2 - x1) / area, C0 = (x1 * y2 - y1 * x2) / area;
      const A1 = (y2 - y0) / area, B1 = (x0 - x2) / area, C1 = (x2 * y0 - y2 * x0) / area;
      const depth = this.depth, ids = this.part;
      for (let y = minY; y <= maxY; y++) {
        const py = y + .5;                                           /* pixel center      */
        let w0 = A0 * (minX + .5) + B0 * py + C0;
        let w1 = A1 * (minX + .5) + B1 * py + C1;
        let o = y * W + minX;
        for (let x = minX; x <= maxX; x++, o++, w0 += A0, w1 += A1) {
          const w2 = 1 - w0 - w1;
          if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) continue;      /* outside           */
          const iz = w0 * iz0 + w1 * iz1 + w2 * iz2;
          if (iz > depth[o]) { depth[o] = iz; ids[o] = part; }       /* nearer: keep      */
        }
      }
    }

    /*
     * True when the point at device pixel (x, y), at distance z, is hidden: some part not
     * in `ownMask` (bit = part id) is nearer there by more than `eps`. A line is never
     * hidden by its own part: a convex body or a thin plate cannot cover its own outline,
     * and at a silhouette its own surface is (by definition) at the same depth.
     */
    hides(x, y, z, ownMask, eps) {
      const ix = Math.floor(x), iy = Math.floor(y);
      if (ix < 0 || iy < 0 || ix >= this.width || iy >= this.height) return false;   /* off screen */
      const o = iy * this.width + ix, id = this.part[o];
      if (!id || ((ownMask >> id) & 1)) return false;                 /* empty, or our own */
      return this.depth[o] * (z - eps) > 1;          /* 1/z_buffer > 1/(z - eps): nearer   */
    }
  }

  P3.ZBuffer = ZBuffer;
})(window.Plane3D = window.Plane3D || {});
