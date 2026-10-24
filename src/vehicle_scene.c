/* Bicubic Catmull-Rom electric hover-car renderer.
 *
 * This is deliberately a compositor-side OpenGL renderer, not a second scene
 * or a browser-side WebGL model. The same current EGL/GLX context draws the
 * hovercar, cockpit, and Wayland client textures on the main thread, avoiding
 * cross-thread context sharing or duplicate scene graphs.
 *
 * Critical geometric design decisions:
 *  1. Unified parametric surface evaluation (vehicle_hull_point and
 *     vehicle_canopy_point in vehicle.c): both the lower grand-tourer hull and
 *     the fastback greenhouse are evaluated from continuous Catmull-Rom
 *     splines with analytical finite-difference normals. The canopy base is
 *     anchored directly to the hull's upper beltline deck (v = 0.84) with zero
 *     endpoint height at the cowl and rear decklid, making Overhangs or air
 *     gaps mathematically impossible.
 *  2. Surface-locked exterior trims: headlamps, DRL light blades, front hex
 *     grille, 4-ring emblem, side wing camera-mirrors, flush door handles,
 *     rocker underglow tubes, rear OLED lightbar, and roof LiDAR sensor pod
 *     are all evaluated directly on vehicle_hull_point / vehicle_canopy_point
 *     and offset along the outward surface normal. Nothing uses detached
 *     hardcoded coordinates that could float outside the body.
 *  3. Flush hover-thruster nacelles (no wheels, no stovepipe legs): four
 *     electromagnetic vectoring levitation nacelles are recessed upward into
 *     the flared front/rear undertray bays flush with s.lower, emitting a
 *     ground-effect plasma cushion without protruding legs or axles.
 *  4. Strictly bounded packaging: internal powertrain, battery, and thermal
 *     components are rendered only in VEHICLE_CAMERA_SYSTEMS; detailed seats
 *     share one renderer in driver/cutaway, and all are envelope-tested.
 */
#include "chrome.h"
#include "vehicle_seat.h"

#include <math.h>
#include <string.h>

#define EV_PI 3.14159265358979323846f

static void ev_color(float r, float g, float b) { glColor3f(r, g, b); }

typedef enum {
 EV_MAT_BODY_PAINT = 0,
 EV_MAT_DARK_GLASS = 1,
 EV_MAT_SATIN_METAL = 2,
 EV_MAT_MATTE_CARBON = 3
} EvMaterial;

static void ev_set_material(EvMaterial mat) {
 switch (mat) {
 case EV_MAT_BODY_PAINT: {
  GLfloat spec[] = {0.82f, 0.86f, 0.98f, 1.0f};
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 78.0f);
  break;
 }
 case EV_MAT_DARK_GLASS: {
  GLfloat spec[] = {0.92f, 0.96f, 1.00f, 1.0f};
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 108.0f);
  break;
 }
 case EV_MAT_SATIN_METAL: {
  GLfloat spec[] = {0.58f, 0.62f, 0.72f, 1.0f};
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 46.0f);
  break;
 }
 case EV_MAT_MATTE_CARBON: {
  GLfloat spec[] = {0.14f, 0.16f, 0.20f, 1.0f};
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 16.0f);
  break;
 }
 }
}

static void ev_restore_scene_material(void) {
 GLfloat shine[] = {0.50f, 0.52f, 0.62f, 1.0f};
 glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, shine);
 glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 65.0f);
}

static void ev_box(float x, float y, float z, float w, float h, float d) {
 float a = x - w * 0.5f, b = x + w * 0.5f;
 float c = y - h * 0.5f, e = y + h * 0.5f;
 float f = z - d * 0.5f, g = z + d * 0.5f;
 glBegin(GL_QUADS);
 glNormal3f( 0,  0,  1); glVertex3f(a, c, g); glVertex3f(b, c, g); glVertex3f(b, e, g); glVertex3f(a, e, g);
 glNormal3f( 0,  0, -1); glVertex3f(b, c, f); glVertex3f(a, c, f); glVertex3f(a, e, f); glVertex3f(b, e, f);
 glNormal3f( 0,  1,  0); glVertex3f(a, e, g); glVertex3f(b, e, g); glVertex3f(b, e, f); glVertex3f(a, e, f);
 glNormal3f( 0, -1,  0); glVertex3f(a, c, f); glVertex3f(b, c, f); glVertex3f(b, c, g); glVertex3f(a, c, g);
 glNormal3f( 1,  0,  0); glVertex3f(b, c, g); glVertex3f(b, c, f); glVertex3f(b, e, f); glVertex3f(b, e, g);
 glNormal3f(-1,  0,  0); glVertex3f(a, c, f); glVertex3f(a, c, g); glVertex3f(a, e, g); glVertex3f(a, e, f);
 glEnd();
}

static void ev_cylinder(Vec3 a, Vec3 b, float radius, int slices) {
 Vec3 diff = vsub(b, a);
 if (vdot(diff, diff) < 1e-8f) return;
 Vec3 dir = vnorm(diff);
 Vec3 ref = fabsf(dir.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
 Vec3 u = vnorm(vcross(dir, ref));
 Vec3 v = vcross(dir, u);
 glBegin(GL_QUAD_STRIP);
 for (int i = 0; i <= slices; i++) {
  float t = (float)i * 2.0f * EV_PI / (float)slices;
  Vec3 n = vadd(vmul(u, cosf(t)), vmul(v, sinf(t)));
  Vec3 pa = vadd(a, vmul(n, radius));
  Vec3 pb = vadd(b, vmul(n, radius));
  glNormal3f(n.x, n.y, n.z);
  glVertex3f(pa.x, pa.y, pa.z);
  glVertex3f(pb.x, pb.y, pb.z);
 }
 glEnd();
 for (int end = 0; end < 2; end++) {
  Vec3 cap = end ? b : a;
  Vec3 n = end ? dir : vmul(dir, -1.0f);
  glBegin(GL_TRIANGLE_FAN);
  glNormal3f(n.x, n.y, n.z);
  glVertex3f(cap.x, cap.y, cap.z);
  for (int i = 0; i <= slices; i++) {
   float t = (float)(end ? i : (slices - i)) * 2.0f * EV_PI / (float)slices;
   Vec3 rim = vadd(cap, vmul(vadd(vmul(u, cosf(t)), vmul(v, sinf(t))), radius));
   glVertex3f(rim.x, rim.y, rim.z);
  }
  glEnd();
 }
}

static void ev_torus(float major, float minor, int rings, int sides) {
 for (int i = 0; i < rings; i++) {
  glBegin(GL_QUAD_STRIP);
  for (int j = 0; j <= sides; j++) {
   for (int k = 0; k < 2; k++) {
    float a = (float)(i + k) * 2.0f * EV_PI / (float)rings;
    float b = (float)j * 2.0f * EV_PI / (float)sides;
    glNormal3f(cosf(a) * cosf(b), sinf(a) * cosf(b), sinf(b));
    glVertex3f((major + minor * cosf(b)) * cosf(a),
               (major + minor * cosf(b)) * sinf(a),
               minor * sinf(b));
   }
  }
  glEnd();
 }
}

static void ev_glow_line(Vec3 a, Vec3 b, float radius, float r, float g, float bl) {
 glDisable(GL_LIGHTING);
 ev_color(r, g, bl);
 ev_cylinder(a, b, radius, 10);
 glEnable(GL_BLEND);
 glBlendFunc(GL_SRC_ALPHA, GL_ONE);
 glDepthMask(GL_FALSE);
 for (int k = 3; k >= 1; k--) {
  glColor4f(r, g, bl, 0.038f);
  ev_cylinder(a, b, radius * (1.0f + (float)k * 1.55f), 10);
 }
 glDepthMask(GL_TRUE);
 glDisable(GL_BLEND);
 glEnable(GL_LIGHTING);
}

static int clamp_index(int i, int count) {
 return i < 0 ? 0 : (i >= count ? count - 1 : i);
}

static void spline_cable(const Vec3 *points, int count, float radius) {
 /* Piecewise Catmull-Rom cable routing avoids impossible right angles while
  * duplicating endpoints so each harness stays anchored at its connector. */
 for (int segment = 0; segment < count - 1; segment++) {
  Vec3 p[4] = {
   points[clamp_index(segment - 1, count)],
   points[segment],
   points[segment + 1],
   points[clamp_index(segment + 2, count)]
  };
  Vec3 previous = p[1];
  for (int i = 1; i <= 12; i++) {
   Vec3 next = vehicle_catmull_rom_vec3(p, (float)i / 12.0f);
   ev_cylinder(previous, next, radius, 8);
   previous = next;
  }
 }
}

/* Evaluate a point on the front (u=0) or rear (u=1) fascia cap with a smooth
 * compound aerodynamic bow that shares the exact boundary ring of the hull. */
static Vec3 fascia_point(bool front, float v, float x_frac, int side, Vec3 *normal) {
 float u = front ? 0.0f : 1.0f;
 float sign = front ? -1.0f : 1.0f;
 Vec3 outer = vehicle_hull_point(u, v, side, NULL);
 Vec3 centre_top = vehicle_hull_point(u, 1.0f, 1, NULL);
 Vec3 centre_bot = vehicle_hull_point(u, 0.0f, 1, NULL);
 float cy = outer.y;
 if (v > 0.82f) {
  float t = (v - 0.82f) / 0.18f;
  cy = outer.y * (1.0f - t) + centre_top.y * t;
 } else if (v < 0.16f) {
  float t = v / 0.16f;
  cy = centre_bot.y * (1.0f - t) + outer.y * t;
 }
 float xf = clampf(x_frac, 0.0f, 1.0f);
 float x = outer.x * xf;
 float y = cy * (1.0f - xf) + outer.y * xf;
 /* Nose cone bows forward more strongly near the shoulder (v ~ 0.55); tail
  * Kammback fascia bows slightly outward at the ducktail lip and diffuser. */
 float v_profile = sinf(clampf(v, 0.05f, 0.95f) * EV_PI);
 float max_bow = front ? (0.24f * v_profile + 0.06f) : (0.14f * v_profile + 0.04f);
 float bow = sign * max_bow * (1.0f - xf * xf);
 if (normal) {
  float nx = (float)side * xf * 0.36f;
  float ny = (v - 0.52f) * 0.45f;
  *normal = vnorm(v3(nx, ny, sign));
 }
 return v3(x, y, outer.z + bow);
}

static void render_fascia_cap(Server *s, bool front) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);
 const int v_steps = 24, x_steps = 16;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < v_steps; iv++) {
   float v0 = (float)iv / (float)v_steps;
   float v1 = (float)(iv + 1) / (float)v_steps;
   float vm = 0.5f * (v0 + v1);
   /* Zone-based material: lower carbon splitter/diffuser, dark recessed
    * intake/lightbar aperture in the middle, metallic body paint on top. */
   if (vm < 0.25f) {
    ev_set_material(EV_MAT_MATTE_CARBON);
    ev_color(0.055f, 0.062f, 0.082f);
   } else if (front && vm >= 0.25f && vm <= 0.58f) {
    ev_set_material(EV_MAT_MATTE_CARBON);
    ev_color(0.025f, 0.032f, 0.046f);
   } else if (!front && vm >= 0.44f && vm <= 0.68f) {
    ev_set_material(EV_MAT_MATTE_CARBON);
    ev_color(0.022f, 0.026f, 0.038f);
   } else {
    ev_set_material(EV_MAT_BODY_PAINT);
    ev_color(0.22f, 0.25f, 0.34f);
   }
   glBegin(GL_QUAD_STRIP);
   for (int ix = 0; ix <= x_steps; ix++) {
    float xf = (float)ix / (float)x_steps;
    Vec3 n0, n1;
    Vec3 p0 = fascia_point(front, v0, xf, side, &n0);
    Vec3 p1 = fascia_point(front, v1, xf, side, &n1);
    glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
    glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }

 if (front) {
  /* Satin titanium single-frame intake surround & horizontal aero louvers,
   * evaluated directly on the front fascia surface so nothing detaches. */
  ev_set_material(EV_MAT_SATIN_METAL);
  glDisable(GL_LIGHTING);
  glColor3f(0.46f, 0.52f, 0.62f);
  glLineWidth(1.8f);
  for (int side = -1; side <= 1; side += 2) {
   glBegin(GL_LINE_STRIP);
   for (int ix = 0; ix <= 12; ix++) {
    float xf = (float)ix / 12.0f * 0.78f;
    Vec3 p = fascia_point(true, 0.26f, xf, side, NULL);
    glVertex3f(p.x, p.y, p.z - 0.008f);
   }
   for (int iv = 0; iv <= 8; iv++) {
    float v = 0.26f + 0.30f * (float)iv / 8.0f;
    Vec3 p = fascia_point(true, v, 0.78f, side, NULL);
    glVertex3f(p.x, p.y, p.z - 0.008f);
   }
   for (int ix = 12; ix >= 0; ix--) {
    float xf = (float)ix / 12.0f * 0.78f;
    Vec3 p = fascia_point(true, 0.56f, xf, side, NULL);
    glVertex3f(p.x, p.y, p.z - 0.008f);
   }
   glEnd();
  }
  /* Horizontal intake vanes inside the front aero mouth. */
  glColor3f(0.14f, 0.17f, 0.23f);
  glLineWidth(1.3f);
  for (int lv = 1; lv <= 3; lv++) {
   float v = 0.26f + 0.075f * (float)lv;
   for (int side = -1; side <= 1; side += 2) {
    glBegin(GL_LINE_STRIP);
    for (int ix = 0; ix <= 10; ix++) {
     Vec3 p = fascia_point(true, v, (float)ix / 10.0f * 0.74f, side, NULL);
     glVertex3f(p.x, p.y, p.z - 0.006f);
    }
    glEnd();
   }
  }
  /* Four interlocked metallic concept rings mounted flush on the nose cone. */
  glColor3f(0.74f, 0.80f, 0.90f);
  glLineWidth(1.8f);
  Vec3 badge_c = fascia_point(true, 0.45f, 0.0f, 1, NULL);
  for (int ring = 0; ring < 4; ring++) {
   float cx = ((float)ring - 1.5f) * 0.145f;
   glBegin(GL_LINE_STRIP);
   for (int i = 0; i <= 32; i++) {
    float a = (float)i * 2.0f * EV_PI / 32.0f;
    float rx = cx + 0.102f * cosf(a);
    float ry = badge_c.y + 0.098f * sinf(a);
    float bz = badge_c.z - 0.014f + 0.018f * rx * rx;
    glVertex3f(rx, ry, bz);
   }
   glEnd();
  }
  /* Subtle neon front splitter leading-edge accent. */
  glColor3f(nr * 0.85f, ng * 0.85f, nb * 0.85f);
  glLineWidth(1.6f);
  for (int side = -1; side <= 1; side += 2) {
   glBegin(GL_LINE_STRIP);
   for (int ix = 0; ix <= 14; ix++) {
    Vec3 p = fascia_point(true, 0.18f, (float)ix / 14.0f * 0.90f, side, NULL);
    glVertex3f(p.x, p.y, p.z - 0.008f);
   }
   glEnd();
  }
  glLineWidth(1.0f);
  glEnable(GL_LIGHTING);
 } else {
  /* Full-width crimson OLED rear lightblade & vertical laser brake strakes,
   * evaluated directly on the rear fascia surface. */
  glDisable(GL_LIGHTING);
  for (int side = -1; side <= 1; side += 2) {
   glColor3f(1.0f, 0.06f, 0.14f);
   glBegin(GL_QUAD_STRIP);
   for (int ix = 0; ix <= 16; ix++) {
    float xf = (float)ix / 16.0f * 0.92f;
    Vec3 p0 = fascia_point(false, 0.55f, xf, side, NULL);
    Vec3 p1 = fascia_point(false, 0.60f, xf, side, NULL);
    glVertex3f(p0.x, p0.y, p0.z + 0.008f);
    glVertex3f(p1.x, p1.y, p1.z + 0.008f);
   }
   glEnd();
   /* Four vertical OLED brake-light fins per side inside the rear channel. */
   glColor3f(1.0f, 0.18f, 0.22f);
   for (int fin = 1; fin <= 4; fin++) {
    float xf = 0.28f + (float)fin * 0.14f;
    Vec3 b0 = fascia_point(false, 0.48f, xf - 0.015f, side, NULL);
    Vec3 b1 = fascia_point(false, 0.48f, xf + 0.015f, side, NULL);
    Vec3 t1 = fascia_point(false, 0.55f, xf + 0.015f, side, NULL);
    Vec3 t0 = fascia_point(false, 0.55f, xf - 0.015f, side, NULL);
    glBegin(GL_QUADS);
    glVertex3f(b0.x, b0.y, b0.z + 0.009f);
    glVertex3f(b1.x, b1.y, b1.z + 0.009f);
    glVertex3f(t1.x, t1.y, t1.z + 0.009f);
    glVertex3f(t0.x, t0.y, t0.z + 0.009f);
    glEnd();
   }
  }
  /* Rear Venturi diffuser strakes & central energy-recovery beacon. */
  glEnable(GL_LIGHTING);
  ev_set_material(EV_MAT_MATTE_CARBON);
  ev_color(0.08f, 0.095f, 0.125f);
  for (int side = -1; side <= 1; side += 2) {
   for (int st = 1; st <= 2; st++) {
    Vec3 p = fascia_point(false, 0.14f, 0.24f * (float)st, side, NULL);
    ev_box(p.x, p.y + 0.02f, p.z - 0.08f, 0.028f, 0.14f, 0.22f);
   }
  }
  Vec3 beacon = fascia_point(false, 0.16f, 0.0f, 1, NULL);
  ev_glow_line(v3(-0.08f, beacon.y + 0.02f, beacon.z + 0.01f),
               v3( 0.08f, beacon.y + 0.02f, beacon.z + 0.01f), 0.014f, 1.0f, 0.08f, 0.16f);
 }
}

static void render_exterior_hull(void) {
 const int u_steps = 80, v_steps = 36;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < v_steps; iv++) {
   float v0 = (float)iv / (float)v_steps;
   float v1 = (float)(iv + 1) / (float)v_steps;
   float vm = 0.5f * (v0 + v1);

   if (vm < 0.24f) {
    /* Flat Venturi undertray and lower carbon aero rocker skirt. */
    ev_set_material(EV_MAT_MATTE_CARBON);
    ev_color(0.050f, 0.058f, 0.078f);
   } else if (vm < 0.30f) {
    /* Satin titanium lower rocker molding. */
    ev_set_material(EV_MAT_SATIN_METAL);
    ev_color(0.14f, 0.16f, 0.22f);
   } else {
    /* Metallic clearcoat body panels, subtly richer on the upper deck. */
    ev_set_material(EV_MAT_BODY_PAINT);
    float shade = 0.88f + 0.18f * ((vm - 0.30f) / 0.70f);
    ev_color(0.21f * shade, 0.24f * shade, 0.34f * shade);
   }

   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= u_steps; iu++) {
    float u = (float)iu / (float)u_steps;
    /* Darken the recessed side-scoop cove along the doors for visual depth. */
    if (vm >= 0.32f && vm <= 0.50f && u >= 0.33f && u <= 0.66f) {
     ev_color(0.13f, 0.15f, 0.22f);
    } else if (vm >= 0.30f) {
     float shade = 0.88f + 0.18f * ((vm - 0.30f) / 0.70f);
     ev_color(0.21f * shade, 0.24f * shade, 0.34f * shade);
    }
    Vec3 n0, n1;
    Vec3 p0 = vehicle_hull_point(u, v0, side, &n0);
    Vec3 p1 = vehicle_hull_point(u, v1, side, &n1);
    glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
    glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }
}

static Vec3 hull_offset_point(float u, float v, int side, float offset, Vec3 *n_out) {
 Vec3 n;
 Vec3 p = vehicle_hull_point(u, v, side, &n);
 if (n_out) *n_out = n;
 return vadd(p, vmul(n, offset));
}

static void render_driver_hull(void) {
 /* From the first-person driver's seat, only the outer side flanks (v <= 0.78,
  * outside the cabin beltline), the front hood (u <= 0.41, ahead of the z=-1.0
  * dashboard), and the rear deck (u >= 0.82) are drawn. Keeping the front hood
  * and rear deck as separate quad strips prevents bridging across the cockpit. */
 ev_set_material(EV_MAT_BODY_PAINT);
 const int u_steps = 72;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 4; iv < 19; iv++) {
   float v0 = (float)iv / 24.0f;
   float v1 = (float)(iv + 1) / 24.0f;
   ev_color(iv < 15 ? 0.19f : 0.22f, iv < 15 ? 0.22f : 0.25f, iv < 15 ? 0.30f : 0.34f);
   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= u_steps; iu++) {
    float u = (float)iu / (float)u_steps;
    Vec3 n0, n1;
    Vec3 p0 = vehicle_hull_point(u, v0, side, &n0);
    Vec3 p1 = vehicle_hull_point(u, v1, side, &n1);
    glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
    glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }
 /* Front hood (ahead of the windshield/dashboard) and rear deck center panels. */
 for (int part = 0; part < 2; part++) {
  float u_from = part ? 0.82f : 0.0f;
  float u_to   = part ? 1.00f : 0.41f;
  for (int side = -1; side <= 1; side += 2) {
   for (int iv = 19; iv < 24; iv++) {
    float v0 = (float)iv / 24.0f;
    float v1 = (float)(iv + 1) / 24.0f;
    ev_color(0.23f, 0.26f, 0.35f);
    glBegin(GL_QUAD_STRIP);
    for (int iu = 0; iu <= 32; iu++) {
     float u = u_from + (u_to - u_from) * (float)iu / 32.0f;
     Vec3 n0, n1;
     Vec3 p0 = vehicle_hull_point(u, v0, side, &n0);
     Vec3 p1 = vehicle_hull_point(u, v1, side, &n1);
     glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
     glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
    }
    glEnd();
   }
  }
 }
}

static void render_exterior_canopy(Server *s) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);
 const int u_steps = 64, v_steps = 28;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < v_steps; iv++) {
   float v0 = (float)iv / (float)v_steps;
   float v1 = (float)(iv + 1) / (float)v_steps;
   float vm = 0.5f * (v0 + v1);

   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= u_steps; iu++) {
    float u = (float)iu / (float)u_steps;
    /* Structural body-colored A-pillars, cantrails (v in [0.64, 0.74]), and
     * flying-buttress C-pillars frame the tinted sapphire-obsidian glass. */
    bool cantrail = (vm >= 0.64f && vm <= 0.74f && u >= 0.08f && u <= 0.92f);
    bool base_trim = (vm < 0.06f);
    bool b_pillar = (vm < 0.66f && u >= 0.44f && u <= 0.47f);
    bool rear_buttress = (vm < 0.22f && u > 0.74f);
    bool front_pillar = (vm < 0.16f && u < 0.18f);

    if (base_trim) {
     ev_set_material(EV_MAT_SATIN_METAL);
     ev_color(0.38f, 0.43f, 0.52f);
    } else if (cantrail || rear_buttress || front_pillar) {
     ev_set_material(EV_MAT_BODY_PAINT);
     ev_color(0.22f, 0.25f, 0.35f);
    } else if (b_pillar) {
     ev_set_material(EV_MAT_DARK_GLASS);
     ev_color(0.018f, 0.022f, 0.032f);
    } else {
     ev_set_material(EV_MAT_DARK_GLASS);
     ev_color(0.032f, 0.052f, 0.082f);
    }
    Vec3 n0, n1;
    Vec3 p0 = vehicle_canopy_point(u, v0, side, &n0);
    Vec3 p1 = vehicle_canopy_point(u, v1, side, &n1);
    glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
    glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }

 /* Flush roof LiDAR & autonomous sensor module contoured directly onto the
  * windshield header crown (u_c in [0.17, 0.26], v_c in [0.78, 1.00]). */
 ev_set_material(EV_MAT_MATTE_CARBON);
 ev_color(0.07f, 0.085f, 0.12f);
 for (int side = -1; side <= 1; side += 2) {
  glBegin(GL_QUAD_STRIP);
  for (int iu = 0; iu <= 10; iu++) {
   float u = 0.17f + 0.09f * (float)iu / 10.0f;
   Vec3 n0, n1;
   Vec3 p0 = vehicle_canopy_point(u, 0.80f, side, &n0);
   Vec3 p1 = vehicle_canopy_point(u, 1.00f, side, &n1);
   p0 = vadd(p0, vmul(n0, 0.018f));
   p1 = vadd(p1, vmul(n1, 0.026f));
   glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
   glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
  }
  glEnd();
 }
 Vec3 lidar_l = vehicle_canopy_point(0.175f, 0.86f, -1, NULL);
 Vec3 lidar_r = vehicle_canopy_point(0.175f, 0.86f,  1, NULL);
 ev_glow_line(v3(lidar_l.x * 0.7f, lidar_l.y + 0.022f, lidar_l.z - 0.01f),
              v3(lidar_r.x * 0.7f, lidar_r.y + 0.022f, lidar_r.z - 0.01f),
              0.008f, nr, ng, nb);
}

/* Flush electromagnetic vectoring hover-thruster nacelle.
 * Instead of dangling below the car on stovepipe legs, the thruster housing is
 * recessed upward into the undertray bay so its chamfered stator ring sits
 * flush with the lower hull contour (y = base_y). */
static void render_hover_nacelle(float x, float base_y, float z,
                                 float nr, float ng, float nb, float phase, bool cutaway) {
 /* Upper magnetic flux housing tucked inside the fender bay. */
 ev_set_material(EV_MAT_SATIN_METAL);
 ev_color(0.16f, 0.19f, 0.26f);
 ev_cylinder(v3(x, base_y + 0.01f, z), v3(x, base_y + 0.26f, z), 0.44f, 28);
 if (cutaway) {
  /* In cutaway mode, expose the upper copper/orange electromagnetic coil packs. */
  ev_color(0.88f, 0.36f, 0.10f);
  glPushMatrix();
  glTranslatef(x, base_y + 0.16f, z);
  glRotatef(90.0f, 1, 0, 0);
  ev_torus(0.38f, 0.045f, 32, 10);
  glPopMatrix();
 }

 /* Flush chamfered titanium outer shroud ring at the underbody plane. */
 glPushMatrix();
 glTranslatef(x, base_y, z);
 glRotatef(90.0f, 1, 0, 0);
 ev_set_material(EV_MAT_SATIN_METAL);
 ev_color(0.32f, 0.36f, 0.45f);
 ev_torus(0.46f, 0.048f, 36, 12);
 ev_set_material(EV_MAT_MATTE_CARBON);
 ev_color(0.05f, 0.06f, 0.09f);
 ev_torus(0.32f, 0.032f, 32, 10);

 /* Twelve radial thrust-vectoring stator vanes inside the recessed aperture. */
 ev_color(0.24f, 0.28f, 0.36f);
 for (int vane = 0; vane < 12; vane++) {
  float a = (float)vane * 2.0f * EV_PI / 12.0f;
  Vec3 inner = v3(0.12f * cosf(a), 0.12f * sinf(a), 0.0f);
  Vec3 outer = v3(0.41f * cosf(a), 0.41f * sinf(a), 0.0f);
  ev_cylinder(inner, outer, 0.012f, 6);
 }

 /* Dual concentric emissive plasma levitation rings. */
 float pulse = 0.82f + 0.18f * sinf(phase);
 glDisable(GL_LIGHTING);
 glColor3f(nr * pulse, ng * pulse, nb * pulse);
 ev_torus(0.37f, 0.022f, 36, 8);
 glColor3f(0.55f + 0.40f * nr, 0.78f + 0.20f * ng, 1.0f);
 ev_torus(0.20f, 0.018f, 28, 8);
 glEnable(GL_LIGHTING);
 glPopMatrix();

 /* Subtle downward ground-effect magnetic cushion glow directly beneath the
  * flush aperture (visual emissive aura only, never a physical wheel/leg). */
 glDisable(GL_LIGHTING);
 glEnable(GL_BLEND);
 glBlendFunc(GL_SRC_ALPHA, GL_ONE);
 glDepthMask(GL_FALSE);
 glColor4f(nr, ng, nb, 0.11f);
 ev_cylinder(v3(x, base_y - 0.10f, z), v3(x, base_y - 0.01f, z), 0.38f, 24);
 glColor4f(0.55f, 0.85f, 1.0f, 0.08f);
 ev_cylinder(v3(x, base_y - 0.16f, z), v3(x, base_y - 0.02f, z), 0.24f, 20);
 glDepthMask(GL_TRUE);
 glDisable(GL_BLEND);
 glEnable(GL_LIGHTING);
}

static void render_all_hover_nacelles(Server *s, bool cutaway) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);
 static const float u_pos[2] = {0.25f, 0.75f}; /* z = -3.55 front, +3.55 rear */
 static const float x_pos[2] = {1.68f, 1.76f}; /* inside the flat 0.74*width undertray */
 for (int end = 0; end < 2; end++) {
  VehicleHullSection sec = vehicle_hull_section(u_pos[end]);
  for (int side = -1; side <= 1; side += 2) {
   float phase = (float)s->time * 3.2f + (float)side + (float)(end * 2);
   render_hover_nacelle((float)side * x_pos[end], sec.lower - 0.01f, sec.z,
                        nr, ng, nb, phase, cutaway);
  }
 }
}

static void render_exterior_details(Server *s) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);

 for (int side = -1; side <= 1; side += 2) {
  /* 1. Sculpted Matrix LED Headlamps & Ice-Cyan DRLs:
   * Every vertex is evaluated on vehicle_hull_point(u, v, side, &n) and
   * displaced slightly along the outward normal so the lamp housing and light
   * blades hug the compound front fender curvature with zero air gap. */
  ev_set_material(EV_MAT_MATTE_CARBON);
  ev_color(0.025f, 0.032f, 0.048f);
  glBegin(GL_QUAD_STRIP);
  for (int i = 0; i <= 14; i++) {
   float t = (float)i / 14.0f;
   float u = 0.012f + 0.115f * t;
   float v_lo = 0.56f + 0.06f * t;
   float v_hi = 0.67f + 0.07f * t;
   Vec3 n0, n1;
   Vec3 p0 = vehicle_hull_point(u, v_lo, side, &n0);
   Vec3 p1 = vehicle_hull_point(u, v_hi, side, &n1);
   p0 = vadd(p0, vmul(n0, 0.006f));
   p1 = vadd(p1, vmul(n1, 0.006f));
   glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
   glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
  }
  glEnd();

  /* Upper ice-cyan DRL light blade & lower matrix LED segments. */
  glDisable(GL_LIGHTING);
  glColor3f(0.68f, 0.92f, 1.0f);
  glBegin(GL_QUAD_STRIP);
  for (int i = 0; i <= 14; i++) {
   float t = (float)i / 14.0f;
   float u = 0.012f + 0.115f * t;
   float v_a = 0.65f + 0.07f * t;
   float v_b = 0.675f + 0.07f * t;
   Vec3 pa = hull_offset_point(u, v_a, side, 0.011f, NULL);
   Vec3 pb = hull_offset_point(u, v_b, side, 0.011f, NULL);
   glVertex3f(pa.x, pa.y, pa.z);
   glVertex3f(pb.x, pb.y, pb.z);
  }
  glEnd();
  /* Three crystalline matrix projector modules inside each headlamp recess. */
  glColor3f(0.90f, 0.96f, 1.0f);
  for (int m = 0; m < 3; m++) {
   float t0 = 0.15f + (float)m * 0.24f, t1 = t0 + 0.15f;
   float u0 = 0.012f + 0.115f * t0, u1 = 0.012f + 0.115f * t1;
   Vec3 p0 = hull_offset_point(u0, 0.58f + 0.06f * t0, side, 0.012f, NULL);
   Vec3 p1 = hull_offset_point(u1, 0.58f + 0.06f * t1, side, 0.012f, NULL);
   Vec3 p2 = hull_offset_point(u1, 0.63f + 0.06f * t1, side, 0.012f, NULL);
   Vec3 p3 = hull_offset_point(u0, 0.63f + 0.06f * t0, side, 0.012f, NULL);
   glBegin(GL_QUADS);
   glVertex3f(p0.x, p0.y, p0.z); glVertex3f(p1.x, p1.y, p1.z);
   glVertex3f(p2.x, p2.y, p2.z); glVertex3f(p3.x, p3.y, p3.z);
   glEnd();
  }
  glEnable(GL_LIGHTING);

  /* 2. Aerodynamic Side Camera-Mirrors:
   * Rooted inside the upper door shoulder at u = 0.34, v = 0.76 (right at the
   * base of the A-pillar) so the strut emerges directly from the bodywork. */
  Vec3 m_norm;
  Vec3 m_base = vehicle_hull_point(0.34f, 0.76f, side, &m_norm);
  Vec3 m_root = vsub(m_base, vmul(m_norm, 0.03f)); /* anchored inside hull */
  Vec3 m_tip  = vadd(m_base, v3((float)side * 0.28f, 0.05f, 0.06f));
  ev_set_material(EV_MAT_SATIN_METAL);
  ev_color(0.18f, 0.21f, 0.29f);
  ev_cylinder(m_root, m_tip, 0.026f, 12);
  ev_set_material(EV_MAT_BODY_PAINT);
  ev_color(0.22f, 0.25f, 0.35f);
  ev_box(m_tip.x + (float)side * 0.04f, m_tip.y, m_tip.z + 0.02f, 0.20f, 0.085f, 0.26f);
  /* Rear-facing camera lens & integrated neon turn repeater. */
  ev_set_material(EV_MAT_MATTE_CARBON);
  ev_color(0.02f, 0.03f, 0.045f);
  ev_box(m_tip.x + (float)side * 0.04f, m_tip.y, m_tip.z + 0.152f, 0.14f, 0.05f, 0.012f);
  glDisable(GL_LIGHTING);
  glColor3f(nr, ng, nb);
  ev_box(m_tip.x + (float)side * 0.05f, m_tip.y, m_tip.z - 0.112f, 0.15f, 0.014f, 0.012f);
  glEnable(GL_LIGHTING);

  /* 3. Flush Capacitive Door Handles:
   * Evaluated directly on the door shoulder surface (u in [0.46, 0.51]). */
  ev_set_material(EV_MAT_SATIN_METAL);
  ev_color(0.10f, 0.12f, 0.17f);
  glBegin(GL_QUAD_STRIP);
  for (int i = 0; i <= 6; i++) {
   float u = 0.455f + 0.055f * (float)i / 6.0f;
   Vec3 n0, n1;
   Vec3 p0 = hull_offset_point(u, 0.63f, side, 0.007f, &n0);
   Vec3 p1 = hull_offset_point(u, 0.655f, side, 0.007f, &n1);
   glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
   glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
  }
  glEnd();

  /* 4. Side Rocker Neon Underglow Blade:
   * Follows the exact plan-view coke-bottle curve along v = 0.25. */
  Vec3 rocker_pts[7];
  for (int i = 0; i < 7; i++) {
   float u = 0.24f + 0.50f * (float)i / 6.0f;
   rocker_pts[i] = hull_offset_point(u, 0.25f, side, 0.010f, NULL);
  }
  glDisable(GL_LIGHTING);
  ev_color(nr, ng, nb);
  spline_cable(rocker_pts, 7, 0.012f);
  glEnable(GL_LIGHTING);

  /* 5. Flush Hover-Thruster Side Gill Louvers (front u~0.24, rear u~0.75):
   * Replaces traditional wheel cutouts with flush carbon vector-thrust intake
   * gills and an embedded plasma status bar along the lower fender flares. */
  static const float bay_u[2] = {0.20f, 0.71f};
  for (int bay = 0; bay < 2; bay++) {
   float u_start = bay_u[bay], u_end = u_start + 0.085f;
   ev_set_material(EV_MAT_MATTE_CARBON);
   ev_color(0.032f, 0.038f, 0.054f);
   glBegin(GL_QUAD_STRIP);
   for (int i = 0; i <= 8; i++) {
    float u = u_start + (u_end - u_start) * (float)i / 8.0f;
    Vec3 n0, n1;
    Vec3 p0 = hull_offset_point(u, 0.27f, side, 0.005f, &n0);
    Vec3 p1 = hull_offset_point(u, 0.39f, side, 0.005f, &n1);
    glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
    glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
   glDisable(GL_LIGHTING);
   glColor3f(0.42f * nr + 0.35f, 0.42f * ng + 0.52f, 0.42f * nb + 0.58f);
   glLineWidth(1.5f);
   for (int gill = 1; gill <= 3; gill++) {
    float v_g = 0.27f + 0.03f * (float)gill;
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= 6; i++) {
     float u = u_start + 0.008f + (u_end - u_start - 0.016f) * (float)i / 6.0f;
     Vec3 pg = hull_offset_point(u, v_g, side, 0.009f, NULL);
     glVertex3f(pg.x, pg.y, pg.z);
    }
    glEnd();
   }
   glLineWidth(1.0f);
   glEnable(GL_LIGHTING);
  }

  /* 6. Sculpted Front Hood Aero Extractor Vents (u in [0.16, 0.24], v in [0.85, 0.92]). */
  ev_set_material(EV_MAT_MATTE_CARBON);
  ev_color(0.035f, 0.042f, 0.060f);
  glBegin(GL_QUAD_STRIP);
  for (int i = 0; i <= 6; i++) {
   float u = 0.16f + 0.08f * (float)i / 6.0f;
   Vec3 n0, n1;
   Vec3 p0 = hull_offset_point(u, 0.85f, side, 0.005f, &n0);
   Vec3 p1 = hull_offset_point(u, 0.92f, side, 0.005f, &n1);
   glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
   glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
  }
  glEnd();
 }

 /* 7. Flush Left-Rear Charge Port Door (side = -1, u in [0.76, 0.80]) with a
  * thin surface-hugging LED strip (no protruding halo cylinder). */
 ev_set_material(EV_MAT_MATTE_CARBON);
 ev_color(0.08f, 0.10f, 0.15f);
 glBegin(GL_QUAD_STRIP);
 for (int i = 0; i <= 6; i++) {
  float u = 0.76f + 0.045f * (float)i / 6.0f;
  Vec3 n0, n1;
  Vec3 p0 = hull_offset_point(u, 0.52f, -1, 0.006f, &n0);
  Vec3 p1 = hull_offset_point(u, 0.62f, -1, 0.006f, &n1);
  glNormal3f(n0.x, n0.y, n0.z); glVertex3f(p0.x, p0.y, p0.z);
  glNormal3f(n1.x, n1.y, n1.z); glVertex3f(p1.x, p1.y, p1.z);
 }
 glEnd();
 glDisable(GL_LIGHTING);
 glColor3f(nr, ng, nb);
 glLineWidth(1.8f);
 glBegin(GL_LINE_STRIP);
 for (int i = 0; i <= 6; i++) {
  float u = 0.765f + 0.035f * (float)i / 6.0f;
  Vec3 p = hull_offset_point(u, 0.54f, -1, 0.009f, NULL);
  glVertex3f(p.x, p.y, p.z);
 }
 glEnd();
 glLineWidth(1.0f);
 glEnable(GL_LIGHTING);
}

static void finned_box(float x, float y, float z, float w, float h, float d) {
 ev_set_material(EV_MAT_SATIN_METAL);
 ev_color(0.26f, 0.30f, 0.38f);
 ev_box(x, y, z, w, h, d);
 ev_color(0.48f, 0.53f, 0.62f);
 for (float fx = x - w * 0.40f; fx <= x + w * 0.401f; fx += w * 0.16f) {
  ev_box(fx, y + h * 0.54f, z, w * 0.035f, h * 0.18f, d * 0.86f);
 }
}

static void render_ev_systems(Server *s) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);

 /* 1. Structural 800 V Skateboard Platform & 82 kWh 12-Module Battery Pack:
  * Packaged strictly inside z in [-2.20, +2.20], |x| <= 1.65, y in [-0.72, -0.36]
  * so it sits cleanly above the -0.82 hull floor and inside the side sills. */
 ev_set_material(EV_MAT_MATTE_CARBON);
 ev_color(0.10f, 0.13f, 0.18f);
 ev_box(0.0f, -0.66f, 0.0f, 3.30f, 0.12f, 4.40f);
 ev_set_material(EV_MAT_SATIN_METAL);
 for (int row = 0; row < 4; row++) {
  for (int col = 0; col < 3; col++) {
   float x = (float)(col - 1) * 0.98f;
   float z = -1.47f + (float)row * 0.98f;
   ev_color(0.13f, 0.23f + 0.025f * (float)row, 0.32f + 0.025f * (float)col);
   ev_box(x, -0.50f, z, 0.86f, 0.20f, 0.86f);
   ev_color(0.32f, 0.56f, 0.64f);
   ev_box(x, -0.39f, z, 0.70f, 0.022f, 0.68f);
   ev_color(0.94f, 0.40f, 0.09f);
   ev_box(x + 0.35f, -0.37f, z, 0.048f, 0.040f, 0.56f);
  }
 }

 /* 2. Front & Rear Coaxial PMSM Drive Units, Reduction Housings, SiC Inverters,
  * and Internal Hover-Thruster Power Conduits (z = -3.45 front, +3.45 rear). */
 for (int end = -1; end <= 1; end += 2) {
  float z = end < 0 ? -3.45f : 3.45f;
  ev_set_material(EV_MAT_SATIN_METAL);
  ev_color(0.25f, 0.29f, 0.37f);
  ev_cylinder(v3(-0.62f, -0.42f, z), v3(0.62f, -0.42f, z), 0.30f, 28);
  for (int fin = 0; fin < 8; fin++) {
   float a = (float)fin * EV_PI / 4.0f;
   ev_cylinder(v3(-0.54f, -0.42f + 0.31f * cosf(a), z + 0.31f * sinf(a)),
               v3( 0.54f, -0.42f + 0.31f * cosf(a), z + 0.31f * sinf(a)), 0.015f, 6);
  }
  /* Coaxial reduction gearbox on the left (-X) and SiC inverter on the right (+X). */
  ev_color(0.36f, 0.40f, 0.48f);
  ev_cylinder(v3(-0.98f, -0.42f, z), v3(-0.62f, -0.42f, z), 0.24f, 22);
  finned_box(0.96f, -0.34f, z, 0.62f, 0.26f, 0.74f);
  /* Internal transverse structural mount linking the flush hover nacelles. */
  ev_color(0.16f, 0.19f, 0.26f);
  float pod_x = end < 0 ? 1.68f : 1.76f;
  ev_cylinder(v3(-pod_x, -0.60f, z), v3(pod_x, -0.60f, z), 0.050f, 12);
 }

 /* 3. 800 V Junction/Contactor Box, Onboard Charger (OBC), and HV->12 V DC/DC
  * Converter packaged neatly on the rear electronics shelf (z = 2.55 .. 4.15,
  * |x| <= 1.05, y in [-0.42, -0.14]) strictly inside the rear decklid. */
 finned_box( 0.00f, -0.30f, 2.55f, 0.96f, 0.24f, 0.56f); /* HV junction box */
 finned_box(-0.78f, -0.30f, 4.10f, 0.76f, 0.24f, 0.68f); /* Onboard charger */
 finned_box( 0.78f, -0.30f, 4.10f, 0.72f, 0.24f, 0.68f); /* HV->12V DC/DC */

 /* 4. Front Slanted Heat Exchanger / Condenser, Coolant Reservoir, and 12 V
  * Auxiliary Lithium Battery packaged inside the front nose (z = -4.55 .. -2.65,
  * |x| <= 1.25, y in [-0.60, -0.18]). */
 glPushMatrix();
 glTranslatef(0.0f, -0.42f, -4.55f);
 glRotatef(26.0f, 1, 0, 0);
 ev_set_material(EV_MAT_SATIN_METAL);
 ev_color(0.22f, 0.26f, 0.33f);
 ev_box(0.0f, 0.0f, 0.0f, 2.10f, 0.36f, 0.14f);
 ev_color(0.45f, 0.50f, 0.58f);
 for (float x = -0.92f; x <= 0.93f; x += 0.13f) {
  ev_box(x, 0.0f, 0.08f, 0.025f, 0.30f, 0.025f);
 }
 glPopMatrix();

 /* Coolant expansion tank (+X front bay) & 12 V auxiliary battery (-X front bay). */
 ev_color(0.18f, 0.50f, 0.60f);
 ev_cylinder(v3(1.18f, -0.58f, -2.75f), v3(1.18f, -0.22f, -2.75f), 0.16f, 20);
 ev_set_material(EV_MAT_MATTE_CARBON);
 ev_color(0.10f, 0.12f, 0.17f);
 ev_box(-1.18f, -0.38f, -2.75f, 0.54f, 0.30f, 0.68f);
 ev_color(0.88f, 0.28f, 0.08f);
 ev_box(-1.32f, -0.21f, -2.88f, 0.09f, 0.05f, 0.12f);
 ev_color(0.18f, 0.22f, 0.28f);
 ev_box(-1.04f, -0.21f, -2.88f, 0.09f, 0.05f, 0.12f);

 /* 5. Internal Charge-Port Receiver anchored inside the left rear quarter. */
 Vec3 cp_in = vehicle_hull_point(0.78f, 0.56f, -1, NULL);
 ev_color(0.12f, 0.15f, 0.21f);
 ev_box(cp_in.x + 0.16f, cp_in.y - 0.04f, cp_in.z, 0.24f, 0.24f, 0.34f);

 /* 6. Piecewise Catmull-Rom 800 V Orange Harnesses & Blue Thermal Coolant Loop. */
 ev_set_material(EV_MAT_SATIN_METAL);
 ev_color(1.0f, 0.32f, 0.04f);
 const Vec3 front_hv[] = {
  {0.00f, -0.30f,  2.55f},
  {0.55f, -0.32f,  1.20f},
  {0.88f, -0.32f, -1.40f},
  {0.96f, -0.34f, -3.45f}
 };
 const Vec3 rear_hv[] = {
  {0.00f, -0.30f, 2.55f},
  {0.52f, -0.28f, 3.00f},
  {0.96f, -0.34f, 3.45f}
 };
 const Vec3 charge_hv[] = {
  { 0.00f, -0.30f, 2.55f},
  {-0.48f, -0.28f, 3.30f},
  {-0.78f, -0.30f, 4.10f},
  {cp_in.x + 0.22f, cp_in.y - 0.04f, cp_in.z}
 };
 spline_cable(front_hv, 4, 0.034f);
 spline_cable(rear_hv,  3, 0.034f);
 spline_cable(charge_hv, 4, 0.030f);

 ev_color(0.10f, 0.52f, 0.72f);
 const Vec3 coolant[] = {
  { 0.00f, -0.44f, -4.50f},
  { 1.18f, -0.42f, -2.75f},
  {-0.72f, -0.40f, -3.45f},
  {-0.95f, -0.44f,  0.00f},
  {-0.72f, -0.40f,  3.45f}
 };
 spline_cable(coolant, 5, 0.026f);

}

static void render_body_outline(float nr, float ng, float nb) {
 /* In EV systems cutaway view, draw a faint translucent ghost pass of the hull
  * and canopy plus crisp spline wireframe curves (longitudinal feature lines
  * and transverse cross-section ribs) so the viewer sees the complete hovercar
  * silhouette enclosing every internal component. */
 glDisable(GL_LIGHTING);
 glEnable(GL_BLEND);
 glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
 glDepthMask(GL_FALSE);

 glColor4f(0.18f, 0.24f, 0.36f, 0.055f);
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < 12; iv++) {
   float v0 = (float)iv / 12.0f, v1 = (float)(iv + 1) / 12.0f;
   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= 48; iu++) {
    float u = (float)iu / 48.0f;
    Vec3 p0 = vehicle_hull_point(u, v0, side, NULL);
    Vec3 p1 = vehicle_hull_point(u, v1, side, NULL);
    glVertex3f(p0.x, p0.y, p0.z);
    glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
  for (int iv = 0; iv < 8; iv++) {
   float v0 = (float)iv / 8.0f, v1 = (float)(iv + 1) / 8.0f;
   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= 36; iu++) {
    float u = (float)iu / 36.0f;
    Vec3 p0 = vehicle_canopy_point(u, v0, side, NULL);
    Vec3 p1 = vehicle_canopy_point(u, v1, side, NULL);
    glVertex3f(p0.x, p0.y, p0.z);
    glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }

 glLineWidth(1.5f);
 static const float hull_v_lines[] = {0.18f, 0.32f, 0.56f, 0.67f, 0.84f};
 for (int side = -1; side <= 1; side += 2) {
  for (int li = 0; li < 5; li++) {
   float v = hull_v_lines[li];
   if (li == 3 || li == 4) glColor4f(nr, ng, nb, 0.82f);
   else glColor4f(0.32f, 0.42f, 0.56f, 0.65f);
   glBegin(GL_LINE_STRIP);
   for (int iu = 0; iu <= 64; iu++) {
    Vec3 p = vehicle_hull_point((float)iu / 64.0f, v, side, NULL);
    glVertex3f(p.x, p.y, p.z);
   }
   glEnd();
  }
  /* Fastback canopy cantrail & roof crown lines. */
  static const float canopy_v_lines[] = {0.0f, 0.68f, 1.0f};
  for (int ci = 0; ci < 3; ci++) {
   glColor4f(ci == 1 ? nr : 0.38f, ci == 1 ? ng : 0.52f, ci == 1 ? nb : 0.68f, 0.75f);
   glBegin(GL_LINE_STRIP);
   for (int iu = 0; iu <= 48; iu++) {
    Vec3 p = vehicle_canopy_point((float)iu / 48.0f, canopy_v_lines[ci], side, NULL);
    glVertex3f(p.x, p.y, p.z);
   }
   glEnd();
  }
 }
 /* Transverse station ribs so the 3D cross-section volume reads clearly. */
 glColor4f(0.28f, 0.38f, 0.52f, 0.45f);
 for (int r = 1; r <= 7; r++) {
  float u = (float)r / 8.0f;
  for (int side = -1; side <= 1; side += 2) {
   glBegin(GL_LINE_STRIP);
   for (int iv = 0; iv <= 18; iv++) {
    Vec3 p = vehicle_hull_point(u, (float)iv / 18.0f, side, NULL);
    glVertex3f(p.x, p.y, p.z);
   }
   glEnd();
  }
 }

 glDepthMask(GL_TRUE);
 glDisable(GL_BLEND);
 glEnable(GL_LIGHTING);
 glLineWidth(1.0f);
}

/* The driver uses the *same* canopy parameterization as the exterior. Only
 * its material visibility differs: structural pillars/cantrails are opaque in
 * this pass, while glass is blended later without writing depth. */
static int canopy_zone(float u, float v) {
 if (v < 0.06f) return 1; /* satin belt trim */
 if ((v >= 0.675f && v <= 0.715f && u >= 0.08f && u <= 0.92f)
     || (v < 0.22f && u > 0.74f) || (v < 0.16f && u < 0.18f)) return 2;
 if (v < 0.66f && u >= 0.44f && u <= 0.47f) return 3; /* B pillar */
 return 0;
}

static void render_driver_canopy(void) {
 const int u_steps = 64, v_steps = 28;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < v_steps; iv++) {
   float v0 = (float)iv / v_steps, v1 = (float)(iv + 1) / v_steps;
   for (int iu = 0; iu < u_steps; iu++) {
    float u0 = (float)iu / u_steps, u1 = (float)(iu + 1) / u_steps;
    int zone = canopy_zone(0.5f * (u0 + u1), 0.5f * (v0 + v1));
    if (!zone) continue;
    if (zone == 1) { ev_set_material(EV_MAT_SATIN_METAL); ev_color(0.38f, 0.43f, 0.52f); }
    else if (zone == 2) { ev_set_material(EV_MAT_BODY_PAINT); ev_color(0.22f, 0.25f, 0.35f); }
    else { ev_set_material(EV_MAT_DARK_GLASS); ev_color(0.018f, 0.022f, 0.032f); }
    Vec3 n00, n01, n11, n10;
    Vec3 p00 = vehicle_canopy_point(u0, v0, side, &n00);
    Vec3 p01 = vehicle_canopy_point(u0, v1, side, &n01);
    Vec3 p11 = vehicle_canopy_point(u1, v1, side, &n11);
    Vec3 p10 = vehicle_canopy_point(u1, v0, side, &n10);
    glBegin(GL_QUADS);
    glNormal3f(n00.x, n00.y, n00.z); glVertex3f(p00.x, p00.y, p00.z);
    glNormal3f(n01.x, n01.y, n01.z); glVertex3f(p01.x, p01.y, p01.z);
    glNormal3f(n11.x, n11.y, n11.z); glVertex3f(p11.x, p11.y, p11.z);
    glNormal3f(n10.x, n10.y, n10.z); glVertex3f(p10.x, p10.y, p10.z);
    glEnd();
   }
  }
 }
}

void vehicle_render(Server *s, bool cutaway) {
 float nr, ng, nb;
 scene_neon_color(s, &nr, &ng, &nb);

 /* Studio front-quarter key/rim light (LIGHT3) illuminates the nose, hood
  * creases, and fastback glass facing the exterior showcase camera. */
 glEnable(GL_LIGHT3);
 GLfloat l3_pos[]  = {6.5f, 8.0f, -9.5f, 0.0f};
 GLfloat l3_diff[] = {0.54f, 0.60f, 0.74f, 1.0f};
 GLfloat l3_spec[] = {0.85f, 0.90f, 1.00f, 1.0f};
 glLightfv(GL_LIGHT3, GL_POSITION, l3_pos);
 glLightfv(GL_LIGHT3, GL_DIFFUSE,  l3_diff);
 glLightfv(GL_LIGHT3, GL_SPECULAR, l3_spec);

 if (cutaway) {
  /* The same detailed seat geometry is shared by driver and cutaway. It is
   * not wasted in the opaque exterior pass, where it cannot be seen. */
  vehicle_seats_render();
  render_ev_systems(s);
  render_all_hover_nacelles(s, true);
  render_body_outline(nr, ng, nb);
  glDisable(GL_LIGHT3);
  ev_restore_scene_material();
  return;
 }

 if (s->camera_mode == VEHICLE_CAMERA_DRIVER) {
  /* Driver mode renders only the exterior panels visible through the cockpit
   * windows; internal EV cutaway geometry is omitted so no cables or boxes
   * ever intersect the interior dashboard or waste software-rasterizer time. */
  vehicle_seats_render();
  render_driver_hull();
  render_fascia_cap(s, true);
  render_fascia_cap(s, false);
  render_driver_canopy();
 } else {
  render_exterior_hull();
  render_fascia_cap(s, true);
  render_fascia_cap(s, false);
  render_exterior_canopy(s);
  render_all_hover_nacelles(s, false);
  render_exterior_details(s);
 }

 glDisable(GL_LIGHT3);
 ev_restore_scene_material();
}

void vehicle_render_glass(Server *s) {
 if (s->camera_mode != VEHICLE_CAMERA_DRIVER) return;
 /* Same exterior canopy vertices, rendered late and transparently. Depth
  * writes stay off so Wayland surfaces behind the glass remain visible. */
 glDisable(GL_LIGHTING);
 glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
 glColor4f(0.035f, 0.075f, 0.11f, 0.13f);
 const int u_steps = 64, v_steps = 28;
 for (int side = -1; side <= 1; side += 2) {
  for (int iv = 0; iv < v_steps; iv++) {
   float v0 = (float)iv / v_steps, v1 = (float)(iv + 1) / v_steps;
   glBegin(GL_QUAD_STRIP);
   for (int iu = 0; iu <= u_steps; iu++) {
    float u = (float)iu / u_steps;
    Vec3 p0 = vehicle_canopy_point(u, v0, side, NULL);
    Vec3 p1 = vehicle_canopy_point(u, v1, side, NULL);
    glVertex3f(p0.x, p0.y, p0.z); glVertex3f(p1.x, p1.y, p1.z);
   }
   glEnd();
  }
 }
 glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_LIGHTING);
}
