/* KingCobra std.vector — vector, quaternion and matrix math.
 *
 * Representation (no new value kind needed): a vector is a plain array
 * of numbers, so vec3 is [x, y, z] and works with len(), indexing,
 * for-loops and every array method. Results are always float arrays.
 *
 *   - Vector functions are N-dimensional: add/dot/length/lerp/... work
 *     on any equal-length arrays (2D, 3D, 4D, 128D...).
 *   - Quaternions are [x, y, z, w].
 *   - Matrices are flat arrays in COLUMN-MAJOR order (OpenGL/Vulkan
 *     convention): mat4 has 16 elements, mat3 has 9. m[col*N + row].
 *   - Angles are radians.
 *
 * Pure C, no platform branching, no allocation beyond the result arrays.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"
#include <math.h>

#define KC_MAXDIM 4096

static double num_of(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_FLOAT) return v.as.f;
    if (v.type == VAL_INT) return (double)v.as.i;
    kc_error_at(pos, "%s: expected a number, got %s", who, kc_type_name(v));
    return 0;
}

/* Reads an array of numbers into out[]; returns the length. */
static int read_vec(KcValue v, double *out, int cap, KcPos pos, const char *who) {
    if (v.type != VAL_ARRAY) kc_error_at(pos, "%s: expected an array of numbers, got %s", who, kc_type_name(v));
    int n = v.as.arr->count;
    if (n > cap) kc_error_at(pos, "%s: vector too long (%d > %d)", who, n, cap);
    for (int i = 0; i < n; i++) out[i] = num_of(v.as.arr->items[i], pos, who);
    return n;
}

static int read_exact(KcValue v, double *out, int want, KcPos pos, const char *who) {
    int n = read_vec(v, out, want > 0 ? want : KC_MAXDIM, pos, who);
    if (want > 0 && n != want) kc_error_at(pos, "%s: expected %d components, got %d", who, want, n);
    return n;
}

static KcValue make_vec(const double *d, int n) {
    KcValue a = kc_array_new();
    for (int i = 0; i < n; i++) kc_array_push(a, kc_float(d[i]));
    return a;
}

static void need_args(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: vector.%s", sig);
}

/* ---------------- construction ---------------- */

static KcValue v_vec2(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "vec2(x, y)");
    double d[2] = { num_of(a[0], pos, "vec2"), num_of(a[1], pos, "vec2") };
    return make_vec(d, 2);
}
static KcValue v_vec3(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "vec3(x, y, z)");
    double d[3];
    for (int i = 0; i < 3; i++) d[i] = num_of(a[i], pos, "vec3");
    return make_vec(d, 3);
}
static KcValue v_vec4(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 4, 4, pos, "vec4(x, y, z, w)");
    double d[4];
    for (int i = 0; i < 4; i++) d[i] = num_of(a[i], pos, "vec4");
    return make_vec(d, 4);
}
static KcValue v_zero(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "zero(dim)");
    int64_t k = (int64_t)num_of(a[0], pos, "zero");
    if (k < 0 || k > KC_MAXDIM) kc_error_at(pos, "vector.zero: dimension out of range");
    double *d = kc_malloc(sizeof(double) * (size_t)(k ? k : 1));
    for (int64_t i = 0; i < k; i++) d[i] = 0;
    KcValue r = make_vec(d, (int)k); free(d); return r;
}
static KcValue v_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "fill(dim, value)");
    int64_t k = (int64_t)num_of(a[0], pos, "fill");
    double x = num_of(a[1], pos, "fill");
    if (k < 0 || k > KC_MAXDIM) kc_error_at(pos, "vector.fill: dimension out of range");
    double *d = kc_malloc(sizeof(double) * (size_t)(k ? k : 1));
    for (int64_t i = 0; i < k; i++) d[i] = x;
    KcValue r = make_vec(d, (int)k); free(d); return r;
}
static KcValue v_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "copy(v)");
    double d[KC_MAXDIM]; int k = read_vec(a[0], d, KC_MAXDIM, pos, "copy");
    return make_vec(d, k);
}

/* ---------------- element-wise binary ops ---------------- */

typedef double (*BinOp)(double, double);
static double op_add(double x, double y) { return x + y; }
static double op_sub(double x, double y) { return x - y; }
static double op_mul(double x, double y) { return x * y; }
static double op_div(double x, double y) { return x / y; }
static double op_min(double x, double y) { return x < y ? x : y; }
static double op_max(double x, double y) { return x > y ? x : y; }

static KcValue binop(KcValue *a, int n, KcPos pos, const char *who, BinOp f) {
    need_args(n, 2, 2, pos, who);
    double x[KC_MAXDIM], y[KC_MAXDIM];
    int nx = read_vec(a[0], x, KC_MAXDIM, pos, who);
    int ny = read_vec(a[1], y, KC_MAXDIM, pos, who);
    if (nx != ny) kc_error_at(pos, "vector.%s: dimension mismatch (%d vs %d)", who, nx, ny);
    for (int i = 0; i < nx; i++) x[i] = f(x[i], y[i]);
    return make_vec(x, nx);
}
static KcValue v_add(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "add", op_add); }
static KcValue v_sub(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "sub", op_sub); }
static KcValue v_mul(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "mul", op_mul); }
static KcValue v_div(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "div", op_div); }
static KcValue v_min(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "min", op_min); }
static KcValue v_max(KcInterp *ip, KcValue *a, int n, KcPos pos) { return binop(a, n, pos, "max", op_max); }

/* ---------------- unary element-wise ---------------- */

typedef double (*UnOp)(double);
static double u_neg(double x) { return -x; }
static double u_abs(double x) { return fabs(x); }
static double u_floor(double x) { return floor(x); }
static double u_ceil(double x) { return ceil(x); }
static double u_round(double x) { return round(x); }
static double u_frac(double x) { return x - floor(x); }
static double u_sign(double x) { return (x > 0) - (x < 0); }

static KcValue unop(KcValue *a, int n, KcPos pos, const char *who, UnOp f) {
    need_args(n, 1, 1, pos, who);
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, who);
    for (int i = 0; i < k; i++) x[i] = f(x[i]);
    return make_vec(x, k);
}
static KcValue v_neg(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "neg", u_neg); }
static KcValue v_abs(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "abs", u_abs); }
static KcValue v_floor(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "floor", u_floor); }
static KcValue v_ceil(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "ceil", u_ceil); }
static KcValue v_round(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "round", u_round); }
static KcValue v_frac(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "frac", u_frac); }
static KcValue v_sign(KcInterp *ip, KcValue *a, int n, KcPos pos) { return unop(a, n, pos, "sign", u_sign); }

static KcValue v_scale(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "scale(v, s)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "scale");
    double s = num_of(a[1], pos, "scale");
    for (int i = 0; i < k; i++) x[i] *= s;
    return make_vec(x, k);
}
static KcValue v_clamp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "clamp(v, lo, hi)  (lo/hi are scalars)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "clamp");
    double lo = num_of(a[1], pos, "clamp"), hi = num_of(a[2], pos, "clamp");
    if (lo > hi) kc_error_at(pos, "vector.clamp: lo > hi");
    for (int i = 0; i < k; i++) x[i] = x[i] < lo ? lo : (x[i] > hi ? hi : x[i]);
    return make_vec(x, k);
}

/* ---------------- reductions / geometry ---------------- */

static double dot_n(const double *x, const double *y, int k) {
    double s = 0; for (int i = 0; i < k; i++) s += x[i] * y[i]; return s;
}
static int two_vecs(KcValue *a, int n, double *x, double *y, KcPos pos, const char *who) {
    need_args(n, 2, 2, pos, who);
    int nx = read_vec(a[0], x, KC_MAXDIM, pos, who);
    int ny = read_vec(a[1], y, KC_MAXDIM, pos, who);
    if (nx != ny) kc_error_at(pos, "vector.%s: dimension mismatch (%d vs %d)", who, nx, ny);
    return nx;
}

static KcValue v_dot(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "dot(a, b)");
    return kc_float(dot_n(x, y, k));
}
static KcValue v_length_sq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "length_sq(v)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "length_sq");
    return kc_float(dot_n(x, x, k));
}
static KcValue v_length(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "length(v)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "length");
    return kc_float(sqrt(dot_n(x, x, k)));
}
static KcValue v_sum(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "sum(v)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "sum");
    double s = 0; for (int i = 0; i < k; i++) s += x[i];
    return kc_float(s);
}
static KcValue v_normalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "normalize(v)  (zero vector stays zero)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "normalize");
    double l = sqrt(dot_n(x, x, k));
    if (l > 0) for (int i = 0; i < k; i++) x[i] /= l;
    return make_vec(x, k);
}
static KcValue v_distance(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "distance(a, b)");
    double s = 0; for (int i = 0; i < k; i++) { double d = x[i] - y[i]; s += d * d; }
    return kc_float(sqrt(s));
}
static KcValue v_distance_sq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "distance_sq(a, b)");
    double s = 0; for (int i = 0; i < k; i++) { double d = x[i] - y[i]; s += d * d; }
    return kc_float(s);
}
/* angle between two vectors, radians in [0, pi]; 0 if either is zero */
static KcValue v_angle(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "angle(a, b)");
    double lx = sqrt(dot_n(x, x, k)), ly = sqrt(dot_n(y, y, k));
    if (lx == 0 || ly == 0) return kc_float(0);
    double c = dot_n(x, y, k) / (lx * ly);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return kc_float(acos(c));
}
static KcValue v_lerp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "lerp(a, b, t)");
    double x[KC_MAXDIM], y[KC_MAXDIM];
    int nx = read_vec(a[0], x, KC_MAXDIM, pos, "lerp"), ny = read_vec(a[1], y, KC_MAXDIM, pos, "lerp");
    if (nx != ny) kc_error_at(pos, "vector.lerp: dimension mismatch (%d vs %d)", nx, ny);
    double t = num_of(a[2], pos, "lerp");
    for (int i = 0; i < nx; i++) x[i] += (y[i] - x[i]) * t;
    return make_vec(x, nx);
}
/* component of a along b */
static KcValue v_project(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "project(a, onto)");
    double d = dot_n(y, y, k);
    double s = d > 0 ? dot_n(x, y, k) / d : 0;
    for (int i = 0; i < k; i++) y[i] *= s;
    return make_vec(y, k);
}
static KcValue v_reject(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double x[KC_MAXDIM], y[KC_MAXDIM]; int k = two_vecs(a, n, x, y, pos, "reject(a, from)");
    double d = dot_n(y, y, k);
    double s = d > 0 ? dot_n(x, y, k) / d : 0;
    for (int i = 0; i < k; i++) x[i] -= y[i] * s;
    return make_vec(x, k);
}
/* reflect(v, normal): normal need not be unit length */
static KcValue v_reflect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double v[KC_MAXDIM], nn[KC_MAXDIM]; int k = two_vecs(a, n, v, nn, pos, "reflect(v, normal)");
    double l = sqrt(dot_n(nn, nn, k));
    if (l > 0) for (int i = 0; i < k; i++) nn[i] /= l;
    double d = 2 * dot_n(v, nn, k);
    for (int i = 0; i < k; i++) v[i] -= d * nn[i];
    return make_vec(v, k);
}
/* refract(v, normal, eta): v and normal normalised internally. Returns the
 * zero vector on total internal reflection (GLSL semantics). */
static KcValue v_refract(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "refract(v, normal, eta)");
    double v[KC_MAXDIM], nn[KC_MAXDIM];
    int k = read_vec(a[0], v, KC_MAXDIM, pos, "refract");
    int k2 = read_vec(a[1], nn, KC_MAXDIM, pos, "refract");
    if (k != k2) kc_error_at(pos, "vector.refract: dimension mismatch (%d vs %d)", k, k2);
    double eta = num_of(a[2], pos, "refract");
    double lv = sqrt(dot_n(v, v, k)), ln = sqrt(dot_n(nn, nn, k));
    if (lv > 0) for (int i = 0; i < k; i++) v[i] /= lv;
    if (ln > 0) for (int i = 0; i < k; i++) nn[i] /= ln;
    double d = dot_n(nn, v, k);
    double kk = 1.0 - eta * eta * (1.0 - d * d);
    if (kk < 0) { for (int i = 0; i < k; i++) v[i] = 0; return make_vec(v, k); }
    double s = eta * d + sqrt(kk);
    for (int i = 0; i < k; i++) v[i] = eta * v[i] - s * nn[i];
    return make_vec(v, k);
}
static KcValue v_clamp_length(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "clamp_length(v, max_len)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "clamp_length");
    double m = num_of(a[1], pos, "clamp_length");
    double l = sqrt(dot_n(x, x, k));
    if (l > m && l > 0) for (int i = 0; i < k; i++) x[i] *= m / l;
    return make_vec(x, k);
}
static KcValue v_move_towards(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "move_towards(current, target, max_delta)");
    double c[KC_MAXDIM], t[KC_MAXDIM];
    int k = read_vec(a[0], c, KC_MAXDIM, pos, "move_towards"), k2 = read_vec(a[1], t, KC_MAXDIM, pos, "move_towards");
    if (k != k2) kc_error_at(pos, "vector.move_towards: dimension mismatch");
    double md = num_of(a[2], pos, "move_towards");
    double d[KC_MAXDIM]; double l = 0;
    for (int i = 0; i < k; i++) { d[i] = t[i] - c[i]; l += d[i] * d[i]; }
    l = sqrt(l);
    if (l <= md || l == 0) return make_vec(t, k);
    for (int i = 0; i < k; i++) c[i] += d[i] / l * md;
    return make_vec(c, k);
}
static KcValue v_equals(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 3, pos, "equals(a, b[, eps])");
    double x[KC_MAXDIM], y[KC_MAXDIM];
    int nx = read_vec(a[0], x, KC_MAXDIM, pos, "equals"), ny = read_vec(a[1], y, KC_MAXDIM, pos, "equals");
    double eps = n == 3 ? num_of(a[2], pos, "equals") : 1e-9;
    if (nx != ny) return kc_bool(false);
    for (int i = 0; i < nx; i++) if (fabs(x[i] - y[i]) > eps) return kc_bool(false);
    return kc_bool(true);
}
static KcValue v_is_finite(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "is_finite(v)");
    double x[KC_MAXDIM]; int k = read_vec(a[0], x, KC_MAXDIM, pos, "is_finite");
    for (int i = 0; i < k; i++) if (!isfinite(x[i])) return kc_bool(false);
    return kc_bool(true);
}

/* cross: 3D -> vec3; 2D -> scalar z-component (a.x*b.y - a.y*b.x) */
static KcValue v_cross(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "cross(a, b)  (2D -> scalar, 3D -> vec3)");
    double x[KC_MAXDIM], y[KC_MAXDIM];
    int nx = read_vec(a[0], x, KC_MAXDIM, pos, "cross"), ny = read_vec(a[1], y, KC_MAXDIM, pos, "cross");
    if (nx != ny || (nx != 2 && nx != 3)) kc_error_at(pos, "vector.cross: needs two 2D or two 3D vectors");
    if (nx == 2) return kc_float(x[0] * y[1] - x[1] * y[0]);
    double r[3] = { x[1]*y[2] - x[2]*y[1], x[2]*y[0] - x[0]*y[2], x[0]*y[1] - x[1]*y[0] };
    return make_vec(r, 3);
}
static KcValue v_triple(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "triple(a, b, c)  (scalar triple product a . (b x c))");
    double x[3], y[3], z[3];
    read_exact(a[0], x, 3, pos, "triple"); read_exact(a[1], y, 3, pos, "triple"); read_exact(a[2], z, 3, pos, "triple");
    double c[3] = { y[1]*z[2] - y[2]*z[1], y[2]*z[0] - y[0]*z[2], y[0]*z[1] - y[1]*z[0] };
    return kc_float(x[0]*c[0] + x[1]*c[1] + x[2]*c[2]);
}
static KcValue v_perp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "perp(v2)  (rotate 90 deg counter-clockwise)");
    double x[2]; read_exact(a[0], x, 2, pos, "perp");
    double r[2] = { -x[1], x[0] };
    return make_vec(r, 2);
}
static KcValue v_rotate2(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "rotate2(v2, angle)");
    double x[2]; read_exact(a[0], x, 2, pos, "rotate2");
    double t = num_of(a[1], pos, "rotate2"), c = cos(t), s = sin(t);
    double r[2] = { x[0]*c - x[1]*s, x[0]*s + x[1]*c };
    return make_vec(r, 2);
}
/* Rodrigues rotation of v about an arbitrary axis (axis need not be unit). */
static void rot_axis(const double *v, const double *ax, double t, double *out) {
    double l = sqrt(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    if (l == 0) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return; }
    double k[3] = { ax[0]/l, ax[1]/l, ax[2]/l };
    double c = cos(t), s = sin(t);
    double kv = k[0]*v[0] + k[1]*v[1] + k[2]*v[2];
    double cr[3] = { k[1]*v[2] - k[2]*v[1], k[2]*v[0] - k[0]*v[2], k[0]*v[1] - k[1]*v[0] };
    for (int i = 0; i < 3; i++) out[i] = v[i]*c + cr[i]*s + k[i]*kv*(1 - c);
}
static KcValue v_rotate3(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "rotate3(v3, axis3, angle)");
    double v[3], ax[3], o[3];
    read_exact(a[0], v, 3, pos, "rotate3"); read_exact(a[1], ax, 3, pos, "rotate3");
    rot_axis(v, ax, num_of(a[2], pos, "rotate3"), o);
    return make_vec(o, 3);
}
static KcValue v_deg2rad(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "deg2rad(deg)"); return kc_float(num_of(a[0], pos, "deg2rad") * M_PI / 180.0);
}
static KcValue v_rad2deg(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "rad2deg(rad)"); return kc_float(num_of(a[0], pos, "rad2deg") * 180.0 / M_PI);
}
static KcValue v_smoothstep(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "smoothstep(edge0, edge1, x)");
    double e0 = num_of(a[0], pos, "smoothstep"), e1 = num_of(a[1], pos, "smoothstep"), x = num_of(a[2], pos, "smoothstep");
    if (e0 == e1) return kc_float(x < e0 ? 0 : 1);
    double t = (x - e0) / (e1 - e0); t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return kc_float(t * t * (3 - 2 * t));
}
/* barycentric(p, a, b, c) -> [u, v, w] for 2D/3D points */
static KcValue v_barycentric(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 4, 4, pos, "barycentric(p, a, b, c)");
    double p[KC_MAXDIM], A[KC_MAXDIM], B[KC_MAXDIM], C[KC_MAXDIM];
    int k = read_vec(a[0], p, KC_MAXDIM, pos, "barycentric");
    if (read_vec(a[1], A, KC_MAXDIM, pos, "barycentric") != k || read_vec(a[2], B, KC_MAXDIM, pos, "barycentric") != k ||
        read_vec(a[3], C, KC_MAXDIM, pos, "barycentric") != k) kc_error_at(pos, "vector.barycentric: dimension mismatch");
    double v0[KC_MAXDIM], v1[KC_MAXDIM], v2[KC_MAXDIM];
    for (int i = 0; i < k; i++) { v0[i] = B[i]-A[i]; v1[i] = C[i]-A[i]; v2[i] = p[i]-A[i]; }
    double d00 = dot_n(v0,v0,k), d01 = dot_n(v0,v1,k), d11 = dot_n(v1,v1,k), d20 = dot_n(v2,v0,k), d21 = dot_n(v2,v1,k);
    double den = d00*d11 - d01*d01;
    if (den == 0) kc_error_at(pos, "vector.barycentric: degenerate triangle");
    double v = (d11*d20 - d01*d21) / den, w = (d00*d21 - d01*d20) / den;
    double r[3] = { 1 - v - w, v, w };
    return make_vec(r, 3);
}
/* ray_sphere(origin, dir, center, radius) -> distance t >= 0, or null */
static KcValue v_ray_sphere(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 4, 4, pos, "ray_sphere(origin, dir, center, radius) -> t or null");
    double o[3], d[3], c[3];
    read_exact(a[0], o, 3, pos, "ray_sphere"); read_exact(a[1], d, 3, pos, "ray_sphere"); read_exact(a[2], c, 3, pos, "ray_sphere");
    double r = num_of(a[3], pos, "ray_sphere");
    double oc[3] = { o[0]-c[0], o[1]-c[1], o[2]-c[2] };
    double A = dot_n(d, d, 3), B = 2 * dot_n(oc, d, 3), C = dot_n(oc, oc, 3) - r * r;
    if (A == 0) return kc_null();
    double disc = B*B - 4*A*C;
    if (disc < 0) return kc_null();
    double sq = sqrt(disc), t0 = (-B - sq) / (2*A), t1 = (-B + sq) / (2*A);
    if (t0 >= 0) return kc_float(t0);
    if (t1 >= 0) return kc_float(t1);
    return kc_null();
}
/* ray_plane(origin, dir, plane_point, plane_normal) -> t or null */
static KcValue v_ray_plane(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 4, 4, pos, "ray_plane(origin, dir, plane_point, plane_normal) -> t or null");
    double o[3], d[3], p[3], nn[3];
    read_exact(a[0], o, 3, pos, "ray_plane"); read_exact(a[1], d, 3, pos, "ray_plane");
    read_exact(a[2], p, 3, pos, "ray_plane"); read_exact(a[3], nn, 3, pos, "ray_plane");
    double den = dot_n(nn, d, 3);
    if (fabs(den) < 1e-12) return kc_null();
    double pm[3] = { p[0]-o[0], p[1]-o[1], p[2]-o[2] };
    double t = dot_n(pm, nn, 3) / den;
    return t >= 0 ? kc_float(t) : kc_null();
}

/* ---------------- quaternions [x, y, z, w] ---------------- */

static void q_norm(double *q) {
    double l = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (l > 0) for (int i = 0; i < 4; i++) q[i] /= l; else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
}
static void q_mul(const double *a, const double *b, double *o) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void q_rot(const double *q, const double *v, double *o) {
    /* v' = v + 2*w*(u x v) + 2*(u x (u x v)),  u = q.xyz */
    double u[3] = { q[0], q[1], q[2] }, w = q[3];
    double c1[3] = { u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0] };
    double c2[3] = { u[1]*c1[2]-u[2]*c1[1], u[2]*c1[0]-u[0]*c1[2], u[0]*c1[1]-u[1]*c1[0] };
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2*w*c1[i] + 2*c2[i];
}
static KcValue qv_identity(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double q[4] = { 0, 0, 0, 1 }; return make_vec(q, 4);
}
static KcValue qv_from_axis_angle(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "quat_from_axis_angle(axis3, angle)");
    double ax[3]; read_exact(a[0], ax, 3, pos, "quat_from_axis_angle");
    double t = num_of(a[1], pos, "quat_from_axis_angle");
    double l = sqrt(dot_n(ax, ax, 3));
    double q[4] = { 0, 0, 0, 1 };
    if (l > 0) { double s = sin(t / 2) / l; q[0] = ax[0]*s; q[1] = ax[1]*s; q[2] = ax[2]*s; q[3] = cos(t / 2); }
    return make_vec(q, 4);
}
/* Euler angles applied in Z (yaw) * Y (pitch) * X (roll) order — x=roll, y=pitch, z=yaw. */
static KcValue qv_from_euler(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "quat_from_euler(x, y, z)  (radians; x=roll, y=pitch, z=yaw)");
    double x = num_of(a[0], pos, "quat_from_euler") / 2, y = num_of(a[1], pos, "quat_from_euler") / 2, z = num_of(a[2], pos, "quat_from_euler") / 2;
    double cx = cos(x), sx = sin(x), cy = cos(y), sy = sin(y), cz = cos(z), sz = sin(z);
    double q[4] = { sx*cy*cz - cx*sy*sz, cx*sy*cz + sx*cy*sz, cx*cy*sz - sx*sy*cz, cx*cy*cz + sx*sy*sz };
    return make_vec(q, 4);
}
static KcValue qv_to_euler(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "quat_to_euler(q) -> [x, y, z]");
    double q[4]; read_exact(a[0], q, 4, pos, "quat_to_euler"); q_norm(q);
    double sinr = 2*(q[3]*q[0] + q[1]*q[2]), cosr = 1 - 2*(q[0]*q[0] + q[1]*q[1]);
    double sinp = 2*(q[3]*q[1] - q[2]*q[0]); if (sinp > 1) sinp = 1; if (sinp < -1) sinp = -1;
    double siny = 2*(q[3]*q[2] + q[0]*q[1]), cosy = 1 - 2*(q[1]*q[1] + q[2]*q[2]);
    double e[3] = { atan2(sinr, cosr), asin(sinp), atan2(siny, cosy) };
    return make_vec(e, 3);
}
static KcValue qv_mul(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "quat_mul(a, b)  (apply b first, then a)");
    double x[4], y[4], o[4];
    read_exact(a[0], x, 4, pos, "quat_mul"); read_exact(a[1], y, 4, pos, "quat_mul");
    q_mul(x, y, o); return make_vec(o, 4);
}
static KcValue qv_conj(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "quat_conjugate(q)");
    double q[4]; read_exact(a[0], q, 4, pos, "quat_conjugate");
    q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; return make_vec(q, 4);
}
static KcValue qv_inverse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "quat_inverse(q)");
    double q[4]; read_exact(a[0], q, 4, pos, "quat_inverse");
    double l2 = dot_n(q, q, 4);
    if (l2 == 0) kc_error_at(pos, "vector.quat_inverse: zero quaternion");
    q[0] = -q[0]/l2; q[1] = -q[1]/l2; q[2] = -q[2]/l2; q[3] /= l2;
    return make_vec(q, 4);
}
static KcValue qv_normalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "quat_normalize(q)");
    double q[4]; read_exact(a[0], q, 4, pos, "quat_normalize"); q_norm(q); return make_vec(q, 4);
}
static KcValue qv_rotate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "quat_rotate(q, v3)");
    double q[4], v[3], o[3];
    read_exact(a[0], q, 4, pos, "quat_rotate"); read_exact(a[1], v, 3, pos, "quat_rotate");
    q_norm(q); q_rot(q, v, o); return make_vec(o, 3);
}
static KcValue qv_slerp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "quat_slerp(a, b, t)");
    double x[4], y[4], o[4];
    read_exact(a[0], x, 4, pos, "quat_slerp"); read_exact(a[1], y, 4, pos, "quat_slerp");
    double t = num_of(a[2], pos, "quat_slerp");
    q_norm(x); q_norm(y);
    double d = dot_n(x, y, 4);
    if (d < 0) { for (int i = 0; i < 4; i++) y[i] = -y[i]; d = -d; }   /* shortest path */
    if (d > 0.9995) { for (int i = 0; i < 4; i++) o[i] = x[i] + (y[i]-x[i]) * t; q_norm(o); }
    else {
        double th = acos(d), s0 = sin((1-t)*th) / sin(th), s1 = sin(t*th) / sin(th);
        for (int i = 0; i < 4; i++) o[i] = x[i]*s0 + y[i]*s1;
    }
    return make_vec(o, 4);
}
static void q_to_m4(const double *q, double *m) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double xx = x*x, yy = y*y, zz = z*z, xy = x*y, xz = x*z, yz = y*z, wx = w*x, wy = w*y, wz = w*z;
    double t[16] = {
        1-2*(yy+zz), 2*(xy+wz),   2*(xz-wy),   0,
        2*(xy-wz),   1-2*(xx+zz), 2*(yz+wx),   0,
        2*(xz+wy),   2*(yz-wx),   1-2*(xx+yy), 0,
        0, 0, 0, 1 };
    memcpy(m, t, sizeof t);
}
static KcValue qv_to_mat4(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "quat_to_mat4(q)");
    double q[4], m[16]; read_exact(a[0], q, 4, pos, "quat_to_mat4"); q_norm(q); q_to_m4(q, m);
    return make_vec(m, 16);
}

/* ---------------- matrices (column-major, flat) ---------------- */

static void m4_identity(double *m) { memset(m, 0, 16 * sizeof(double)); m[0] = m[5] = m[10] = m[15] = 1; }
static void m4_mul(const double *a, const double *b, double *o) {
    double r[16];
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        double s = 0; for (int k = 0; k < 4; k++) s += a[k*4 + rr] * b[c*4 + k];
        r[c*4 + rr] = s;
    }
    memcpy(o, r, sizeof r);
}
static KcValue m4_ret(const double *m) { return make_vec(m, 16); }

static KcValue mm4_identity(KcInterp *ip, KcValue *a, int n, KcPos pos) { double m[16]; m4_identity(m); return m4_ret(m); }
static KcValue mm4_translate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "mat4_translate(x, y, z)");
    double m[16]; m4_identity(m);
    m[12] = num_of(a[0], pos, "mat4_translate"); m[13] = num_of(a[1], pos, "mat4_translate"); m[14] = num_of(a[2], pos, "mat4_translate");
    return m4_ret(m);
}
static KcValue mm4_scale(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "mat4_scale(x, y, z)");
    double m[16]; m4_identity(m);
    m[0] = num_of(a[0], pos, "mat4_scale"); m[5] = num_of(a[1], pos, "mat4_scale"); m[10] = num_of(a[2], pos, "mat4_scale");
    return m4_ret(m);
}
static void m4_rot_axis(const double *ax, double t, double *m) {
    double l = sqrt(dot_n(ax, ax, 3));
    m4_identity(m);
    if (l == 0) return;
    double x = ax[0]/l, y = ax[1]/l, z = ax[2]/l, c = cos(t), s = sin(t), k = 1 - c;
    m[0] = c + x*x*k;   m[1] = y*x*k + z*s; m[2] = z*x*k - y*s;
    m[4] = x*y*k - z*s; m[5] = c + y*y*k;   m[6] = z*y*k + x*s;
    m[8] = x*z*k + y*s; m[9] = y*z*k - x*s; m[10] = c + z*z*k;
}
static KcValue mm4_rot_axis(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat4_rotate(axis3, angle)");
    double ax[3], m[16]; read_exact(a[0], ax, 3, pos, "mat4_rotate");
    m4_rot_axis(ax, num_of(a[1], pos, "mat4_rotate"), m); return m4_ret(m);
}
static KcValue mm4_rot_x(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_rotate_x(angle)"); double ax[3] = {1,0,0}, m[16];
    m4_rot_axis(ax, num_of(a[0], pos, "mat4_rotate_x"), m); return m4_ret(m);
}
static KcValue mm4_rot_y(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_rotate_y(angle)"); double ax[3] = {0,1,0}, m[16];
    m4_rot_axis(ax, num_of(a[0], pos, "mat4_rotate_y"), m); return m4_ret(m);
}
static KcValue mm4_rot_z(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_rotate_z(angle)"); double ax[3] = {0,0,1}, m[16];
    m4_rot_axis(ax, num_of(a[0], pos, "mat4_rotate_z"), m); return m4_ret(m);
}
static KcValue mm4_mul(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat4_mul(a, b)  (result applies b first, then a)");
    double x[16], y[16];
    read_exact(a[0], x, 16, pos, "mat4_mul"); read_exact(a[1], y, 16, pos, "mat4_mul");
    m4_mul(x, y, x); return m4_ret(x);
}
static KcValue mm4_transpose(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_transpose(m)");
    double x[16], o[16]; read_exact(a[0], x, 16, pos, "mat4_transpose");
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) o[c*4 + r] = x[r*4 + c];
    return m4_ret(o);
}
/* general 4x4 inverse via cofactors; returns false if singular */
static bool m4_inv(const double *m, double *out, double *det_out) {
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det_out) *det_out = det;
    if (fabs(det) < 1e-14) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}
static KcValue mm4_inverse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_inverse(m)  (error if singular)");
    double x[16], o[16]; read_exact(a[0], x, 16, pos, "mat4_inverse");
    if (!m4_inv(x, o, NULL)) kc_error_at(pos, "vector.mat4_inverse: matrix is singular");
    return m4_ret(o);
}
static KcValue mm4_det(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat4_det(m)");
    double x[16], o[16], det; read_exact(a[0], x, 16, pos, "mat4_det");
    m4_inv(x, o, &det); return kc_float(det);
}
static KcValue mm4_perspective(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 4, 4, pos, "mat4_perspective(fovy, aspect, near, far)  (OpenGL clip space, right-handed)");
    double fovy = num_of(a[0], pos, "mat4_perspective"), asp = num_of(a[1], pos, "mat4_perspective");
    double zn = num_of(a[2], pos, "mat4_perspective"), zf = num_of(a[3], pos, "mat4_perspective");
    if (asp == 0 || zn == zf || fovy <= 0 || fovy >= M_PI) kc_error_at(pos, "vector.mat4_perspective: invalid parameters");
    double f = 1.0 / tan(fovy / 2), m[16] = {0};
    m[0] = f / asp; m[5] = f; m[10] = (zf + zn) / (zn - zf); m[11] = -1; m[14] = 2 * zf * zn / (zn - zf);
    return m4_ret(m);
}
static KcValue mm4_ortho(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 6, 6, pos, "mat4_ortho(left, right, bottom, top, near, far)");
    double l = num_of(a[0], pos, "mat4_ortho"), r = num_of(a[1], pos, "mat4_ortho"), b = num_of(a[2], pos, "mat4_ortho");
    double t = num_of(a[3], pos, "mat4_ortho"), zn = num_of(a[4], pos, "mat4_ortho"), zf = num_of(a[5], pos, "mat4_ortho");
    if (l == r || b == t || zn == zf) kc_error_at(pos, "vector.mat4_ortho: degenerate volume");
    double m[16]; m4_identity(m);
    m[0] = 2/(r-l); m[5] = 2/(t-b); m[10] = -2/(zf-zn);
    m[12] = -(r+l)/(r-l); m[13] = -(t+b)/(t-b); m[14] = -(zf+zn)/(zf-zn);
    return m4_ret(m);
}
static KcValue mm4_look_at(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "mat4_look_at(eye, center, up)");
    double e[3], c[3], u[3];
    read_exact(a[0], e, 3, pos, "mat4_look_at"); read_exact(a[1], c, 3, pos, "mat4_look_at"); read_exact(a[2], u, 3, pos, "mat4_look_at");
    double f[3] = { c[0]-e[0], c[1]-e[1], c[2]-e[2] };
    double fl = sqrt(dot_n(f, f, 3));
    if (fl == 0) kc_error_at(pos, "vector.mat4_look_at: eye and center coincide");
    for (int i = 0; i < 3; i++) f[i] /= fl;
    double s[3] = { f[1]*u[2]-f[2]*u[1], f[2]*u[0]-f[0]*u[2], f[0]*u[1]-f[1]*u[0] };
    double sl = sqrt(dot_n(s, s, 3));
    if (sl == 0) kc_error_at(pos, "vector.mat4_look_at: up is parallel to the view direction");
    for (int i = 0; i < 3; i++) s[i] /= sl;
    double uu[3] = { s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };
    double m[16]; m4_identity(m);
    m[0] = s[0]; m[4] = s[1]; m[8]  = s[2];
    m[1] = uu[0]; m[5] = uu[1]; m[9]  = uu[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -dot_n(s, e, 3); m[13] = -dot_n(uu, e, 3); m[14] = dot_n(f, e, 3);
    return m4_ret(m);
}
static KcValue mm4_mul_vec4(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat4_mul_vec4(m, v4)");
    double m[16], v[4], o[4];
    read_exact(a[0], m, 16, pos, "mat4_mul_vec4"); read_exact(a[1], v, 4, pos, "mat4_mul_vec4");
    for (int r = 0; r < 4; r++) o[r] = m[r]*v[0] + m[4+r]*v[1] + m[8+r]*v[2] + m[12+r]*v[3];
    return make_vec(o, 4);
}
/* transform a 3D point (w=1) with perspective divide */
static KcValue mm4_xform_point(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat4_transform_point(m, p3)");
    double m[16], v[3], o[4];
    read_exact(a[0], m, 16, pos, "mat4_transform_point"); read_exact(a[1], v, 3, pos, "mat4_transform_point");
    for (int r = 0; r < 4; r++) o[r] = m[r]*v[0] + m[4+r]*v[1] + m[8+r]*v[2] + m[12+r];
    if (o[3] != 0 && o[3] != 1) { o[0] /= o[3]; o[1] /= o[3]; o[2] /= o[3]; }
    return make_vec(o, 3);
}
/* transform a 3D direction (w=0): translation ignored */
static KcValue mm4_xform_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat4_transform_dir(m, d3)");
    double m[16], v[3], o[3];
    read_exact(a[0], m, 16, pos, "mat4_transform_dir"); read_exact(a[1], v, 3, pos, "mat4_transform_dir");
    for (int r = 0; r < 3; r++) o[r] = m[r]*v[0] + m[4+r]*v[1] + m[8+r]*v[2];
    return make_vec(o, 3);
}
/* compose T * R * S in one call */
static KcValue mm4_trs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 3, 3, pos, "mat4_trs(translation3, rotation_quat4, scale3)");
    double t[3], q[4], s[3], m[16];
    read_exact(a[0], t, 3, pos, "mat4_trs"); read_exact(a[1], q, 4, pos, "mat4_trs"); read_exact(a[2], s, 3, pos, "mat4_trs");
    q_norm(q); q_to_m4(q, m);
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m[c*4 + r] *= s[c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
    return m4_ret(m);
}

/* ---- mat3 ---- */
static void m3_mul(const double *a, const double *b, double *o) {
    double r[9];
    for (int c = 0; c < 3; c++) for (int rr = 0; rr < 3; rr++) {
        double s = 0; for (int k = 0; k < 3; k++) s += a[k*3 + rr] * b[c*3 + k];
        r[c*3 + rr] = s;
    }
    memcpy(o, r, sizeof r);
}
static KcValue mm3_identity(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    double m[9] = {1,0,0, 0,1,0, 0,0,1}; return make_vec(m, 9);
}
static KcValue mm3_mul(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat3_mul(a, b)");
    double x[9], y[9]; read_exact(a[0], x, 9, pos, "mat3_mul"); read_exact(a[1], y, 9, pos, "mat3_mul");
    m3_mul(x, y, x); return make_vec(x, 9);
}
static KcValue mm3_det(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat3_det(m)");
    double m[9]; read_exact(a[0], m, 9, pos, "mat3_det");
    return kc_float(m[0]*(m[4]*m[8]-m[7]*m[5]) - m[3]*(m[1]*m[8]-m[7]*m[2]) + m[6]*(m[1]*m[5]-m[4]*m[2]));
}
static KcValue mm3_inverse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat3_inverse(m)");
    double m[9]; read_exact(a[0], m, 9, pos, "mat3_inverse");
    double c00 = m[4]*m[8]-m[7]*m[5], c01 = m[7]*m[2]-m[1]*m[8], c02 = m[1]*m[5]-m[4]*m[2];
    double det = m[0]*c00 + m[3]*c01 + m[6]*c02;
    if (fabs(det) < 1e-14) kc_error_at(pos, "vector.mat3_inverse: matrix is singular");
    double o[9] = {
        c00, c01, c02,
        m[6]*m[5]-m[3]*m[8], m[0]*m[8]-m[6]*m[2], m[3]*m[2]-m[0]*m[5],
        m[3]*m[7]-m[6]*m[4], m[6]*m[1]-m[0]*m[7], m[0]*m[4]-m[3]*m[1] };
    for (int i = 0; i < 9; i++) o[i] /= det;
    return make_vec(o, 9);
}
static KcValue mm3_transpose(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat3_transpose(m)");
    double x[9], o[9]; read_exact(a[0], x, 9, pos, "mat3_transpose");
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) o[c*3 + r] = x[r*3 + c];
    return make_vec(o, 9);
}
static KcValue mm3_mul_vec3(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 2, 2, pos, "mat3_mul_vec3(m, v3)");
    double m[9], v[3], o[3];
    read_exact(a[0], m, 9, pos, "mat3_mul_vec3"); read_exact(a[1], v, 3, pos, "mat3_mul_vec3");
    for (int r = 0; r < 3; r++) o[r] = m[r]*v[0] + m[3+r]*v[1] + m[6+r]*v[2];
    return make_vec(o, 3);
}
static KcValue mm3_from_mat4(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat3_from_mat4(m4)  (upper-left 3x3)");
    double m[16], o[9]; read_exact(a[0], m, 16, pos, "mat3_from_mat4");
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) o[c*3 + r] = m[c*4 + r];
    return make_vec(o, 9);
}
/* normal matrix = transpose(inverse(upper-left 3x3 of a model matrix)) */
static KcValue mm3_normal(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_args(n, 1, 1, pos, "mat3_normal(model_mat4)");
    double m[16], u[9]; read_exact(a[0], m, 16, pos, "mat3_normal");
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) u[c*3 + r] = m[c*4 + r];
    double c00 = u[4]*u[8]-u[7]*u[5], c01 = u[7]*u[2]-u[1]*u[8], c02 = u[1]*u[5]-u[4]*u[2];
    double det = u[0]*c00 + u[3]*c01 + u[6]*c02;
    if (fabs(det) < 1e-14) kc_error_at(pos, "vector.mat3_normal: singular model matrix");
    double inv[9] = {
        c00, c01, c02,
        u[6]*u[5]-u[3]*u[8], u[0]*u[8]-u[6]*u[2], u[3]*u[2]-u[0]*u[5],
        u[3]*u[7]-u[6]*u[4], u[6]*u[1]-u[0]*u[7], u[0]*u[4]-u[3]*u[1] };
    double o[9];
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) o[c*3 + r] = inv[r*3 + c] / det;
    return make_vec(o, 9);
}

static KcModuleEntry ENTRIES[] = {
    {"vec2", v_vec2}, {"vec3", v_vec3}, {"vec4", v_vec4}, {"zero", v_zero}, {"fill", v_fill}, {"copy", v_copy},
    {"add", v_add}, {"sub", v_sub}, {"mul", v_mul}, {"div", v_div}, {"min", v_min}, {"max", v_max},
    {"neg", v_neg}, {"abs", v_abs}, {"floor", v_floor}, {"ceil", v_ceil}, {"round", v_round}, {"frac", v_frac}, {"sign", v_sign},
    {"scale", v_scale}, {"clamp", v_clamp},
    {"dot", v_dot}, {"length", v_length}, {"length_sq", v_length_sq}, {"sum", v_sum}, {"normalize", v_normalize},
    {"distance", v_distance}, {"distance_sq", v_distance_sq}, {"angle", v_angle}, {"lerp", v_lerp},
    {"project", v_project}, {"reject", v_reject}, {"reflect", v_reflect}, {"refract", v_refract},
    {"clamp_length", v_clamp_length}, {"move_towards", v_move_towards}, {"equals", v_equals}, {"is_finite", v_is_finite},
    {"cross", v_cross}, {"triple", v_triple}, {"perp", v_perp}, {"rotate2", v_rotate2}, {"rotate3", v_rotate3},
    {"deg2rad", v_deg2rad}, {"rad2deg", v_rad2deg}, {"smoothstep", v_smoothstep},
    {"barycentric", v_barycentric}, {"ray_sphere", v_ray_sphere}, {"ray_plane", v_ray_plane},
    {"quat_identity", qv_identity}, {"quat_from_axis_angle", qv_from_axis_angle}, {"quat_from_euler", qv_from_euler},
    {"quat_to_euler", qv_to_euler}, {"quat_mul", qv_mul}, {"quat_conjugate", qv_conj}, {"quat_inverse", qv_inverse},
    {"quat_normalize", qv_normalize}, {"quat_rotate", qv_rotate}, {"quat_slerp", qv_slerp}, {"quat_to_mat4", qv_to_mat4},
    {"mat4_identity", mm4_identity}, {"mat4_translate", mm4_translate}, {"mat4_scale", mm4_scale},
    {"mat4_rotate", mm4_rot_axis}, {"mat4_rotate_x", mm4_rot_x}, {"mat4_rotate_y", mm4_rot_y}, {"mat4_rotate_z", mm4_rot_z},
    {"mat4_mul", mm4_mul}, {"mat4_transpose", mm4_transpose}, {"mat4_inverse", mm4_inverse}, {"mat4_det", mm4_det},
    {"mat4_perspective", mm4_perspective}, {"mat4_ortho", mm4_ortho}, {"mat4_look_at", mm4_look_at},
    {"mat4_mul_vec4", mm4_mul_vec4}, {"mat4_transform_point", mm4_xform_point}, {"mat4_transform_dir", mm4_xform_dir},
    {"mat4_trs", mm4_trs},
    {"mat3_identity", mm3_identity}, {"mat3_mul", mm3_mul}, {"mat3_det", mm3_det}, {"mat3_inverse", mm3_inverse},
    {"mat3_transpose", mm3_transpose}, {"mat3_mul_vec3", mm3_mul_vec3}, {"mat3_from_mat4", mm3_from_mat4},
    {"mat3_normal", mm3_normal},
};

KcModule *kc_mod_vector(void) {
    static KcModule m = { "vector", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
