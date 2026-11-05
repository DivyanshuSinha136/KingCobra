/* KingCobra std.game — internal shared header.
 *
 * std.game is a software-rendered 2D + 3D game engine. Everything is plain C
 * (no GPU, no third-party libs) so a game runs identically on every machine
 * KingCobra runs on. Files:
 *
 *   game_util.c   argument parsing, colours, noise, RNG, option lists
 *   game_gfx.c    framebuffer, 2D drawing, textures, image decode/encode
 *   game_model.c  meshes, built-in "custom" models, OBJ/STL/PLY/glTF/FBX/PSK loaders
 *   game_r3d.c    3D pipeline: camera, lighting, fog/cloud, z-buffer rasteriser
 *   game_world.c  streamed chunk world (generator, disk store, meshing, physics, rays)
 *   game_scene.c  entities, particles, sprite sheets/animations, streamed 2D tilemaps
 *   game_hud.c    retained-mode HUD (bars, hotbar, minimap, compass, dialogs, ...)
 *   game.c        window/input backend, main loop and the KingCobra bindings
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef KC_GAME_INTERNAL_H
#define KC_GAME_INTERNAL_H

#include "interp.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((d) * 0.017453292519943295f)
#define RAD2DEG(r) ((r) * 57.29577951308232f)

/* ------------------------------------------------------------------ math */
typedef struct { float x, y, z; } V3;
typedef struct { float m[16]; } M4;           /* column-major: m[col*4+row] */

static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 v3add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3len(V3 a) { return sqrtf(v3dot(a, a)); }
static inline V3 v3norm(V3 a) { float l = v3len(a); return l > 1e-12f ? v3mul(a, 1.0f / l) : v3(0, 0, 0); }
static inline float fclampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int iclampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int imini(int a, int b) { return a < b ? a : b; }
static inline int imaxi(int a, int b) { return a > b ? a : b; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline int floordiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static inline int floormod(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }

M4 m4_identity(void);
M4 m4_mul(const M4 *a, const M4 *b);
M4 m4_perspective(float fovy_rad, float aspect, float n, float f);
M4 m4_trs(V3 pos, V3 rot_deg, V3 scale);      /* T * Ry * Rx * Rz * S */
V3 m4_point(const M4 *m, V3 p);
V3 m4_dir(const M4 *m, V3 d);

/* --------------------------------------------------------------- colours */
/* Framebuffer / texture pixel = 0xAARRGGBB. */
static inline uint32_t col_pack(int r, int g, int b, int a) {
    return ((uint32_t)iclampi(a, 0, 255) << 24) | ((uint32_t)iclampi(r, 0, 255) << 16) | ((uint32_t)iclampi(g, 0, 255) << 8) | (uint32_t)iclampi(b, 0, 255);
}
#define CA(c) ((int)((c) >> 24))
#define CR(c) ((int)(((c) >> 16) & 255))
#define CG(c) ((int)(((c) >> 8) & 255))
#define CB(c) ((int)((c) & 255))
uint32_t col_mix(uint32_t a, uint32_t b, float t);   /* lerp incl. alpha */
uint32_t col_mul(uint32_t a, uint32_t b);            /* component multiply */
uint32_t col_scale(uint32_t a, float k);             /* scale rgb, keep alpha */
uint32_t col_hsv(float h, float s, float v, int a);

/* ----------------------------------------------------------- game state */
typedef struct {
    KcInterp *ip; KcPos pos;
    char title[160];
    int win_w, win_h, scale;               /* window size in pixels, integer upscale */
    int w, h;                              /* framebuffer size (win / scale) */
    uint32_t *fb; float *zb;               /* colour 0x00RRGGBB; depth = 1/w (bigger = nearer) */
    bool created, headless, quit, begun, mode3d, backend_open, hud_auto, vsync_sleep;
    double time, dt, fps, vclock, last_real;
    uint64_t frame; int target_fps;
    uint8_t key[256], kp[256], kr[256];
    uint8_t mb[8], mbp[8], mbr[8];
    int mx, my, dmx, dmy, wheel; bool captured;
    char typed[64];
    /* 3D camera: right-handed, +Y up, yaw 0 looks toward -Z (yaw grows turning left) */
    V3 cam; float yaw, pitch, roll, fov, znear, zfar;
    /* 2D camera */
    float c2x, c2y, c2zoom;
    /* per-frame statistics */
    int st_tris, st_tris_drawn, st_quads, st_chunks_drawn, st_chunks_culled, st_draws, st_ents_drawn, st_particles;
    double st_frame_ms, st_render_ms, st_stream_ms;
    size_t mem_limit_models, mem_limit_tex;
} Game;
extern Game G;

#define ENTER() do { G.ip = ip; G.pos = pos; } while (0)

/* ------------------------------------------------------------------ util */
void  gm_err(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void  gm_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  A_argc(int n, int lo, int hi, const char *sig);
double A_num(KcValue *a, int n, int i, const char *who);
int   A_int(KcValue *a, int n, int i, const char *who);
const char *A_str(KcValue *a, int n, int i, const char *who);
bool  A_bool(KcValue *a, int n, int i, const char *who);
uint32_t A_col(KcValue *a, int n, int i, const char *who);
V3    A_v3(KcValue *a, int n, int i, const char *who);        /* array [x,y,z] */
bool  V_isnum(KcValue v);
double V_num(KcValue v, const char *who);
bool  V_truthy_arg(KcValue v);
uint32_t col_from_value(KcValue v, const char *who);          /* int / "#hex" / name / [r,g,b,a] */
KcValue arr2(double a, double b);
KcValue arr3(double a, double b, double c);
KcValue arr4(double a, double b, double c, double d);
KcValue kc_str(const char *s);
KcValue obj_new(void);
void  obj_num(KcValue o, const char *k, double v);
void  obj_int(KcValue o, const char *k, int64_t v);
void  obj_bool(KcValue o, const char *k, bool v);
void  obj_str(KcValue o, const char *k, const char *v);
void  obj_val(KcValue o, const char *k, KcValue v);

typedef struct { KcValue *a; int n; int start; KcValue obj; bool has_obj; } Opts;
Opts  opts_make(KcValue *a, int n, int start);                /* trailing "key", value ... or one object */
const KcValue *opt_find(const Opts *o, const char *key);
bool  opt_has(const Opts *o, const char *key);
double opt_num(const Opts *o, const char *key, double def);
int   opt_int(const Opts *o, const char *key, int def);
bool  opt_bool(const Opts *o, const char *key, bool def);
uint32_t opt_col(const Opts *o, const char *key, uint32_t def);
const char *opt_str(const Opts *o, const char *key, const char *def);
V3    opt_v3(const Opts *o, const char *key, V3 def);

uint32_t hash32(uint32_t x);
uint32_t hash3(int x, int y, int z, uint32_t seed);
float vnoise2(float x, float y, uint32_t seed);               /* 0..1 */
float vnoise3(float x, float y, float z, uint32_t seed);      /* 0..1 */
float fbm2(float x, float y, int oct, float lac, float gain, uint32_t seed);   /* 0..1 */
float fbm3(float x, float y, float z, int oct, float lac, float gain, uint32_t seed);
void  rnd_seed(uint32_t s);
uint32_t rnd_u32(void);
float rnd_f(void);                                            /* 0..1 */
float rnd_range(float lo, float hi);
double clock_s(void);
void  sleep_ms_(int ms);
char *read_file_all(const char *path, size_t *len);            /* malloc; NULL on failure */
bool  write_file_all(const char *path, const void *d, size_t n);
char *path_dir(const char *path);                              /* malloc'd directory part (no trailing slash; "" if none) */
char *path_join(const char *dir, const char *rel);             /* malloc */
bool  dir_make(const char *path);
bool  file_exists(const char *path);

/* ------------------------------------------------------------------- gfx */
typedef struct { int w, h; uint32_t *px; } Img;
typedef struct {
    int w, h; uint32_t *px;
    char *path, *name;
    bool alive, resident, proc, protect;      /* protect: never evicted */
    uint64_t last;
} Tex;

void gfx_resize(int w, int h);
void gfx_clear(uint32_t c);
void gfx_clip(int x0, int y0, int x1, int y1);
void gfx_clip_reset(void);
void gfx_px(int x, int y, uint32_t c);                        /* alpha-blended */
uint32_t gfx_get(int x, int y);
void gfx_rect(int x, int y, int w, int h, uint32_t c, bool fill);
void gfx_rect_grad(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t c, int thick);
void gfx_circle(int cx, int cy, int r, uint32_t c, bool fill);
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, bool fill);
void gfx_text(int x, int y, const char *s, uint32_t c, int scale);
void gfx_text_shadow(int x, int y, const char *s, uint32_t c, int scale, uint32_t shadow);
int  gfx_text_w(const char *s, int scale);
int  gfx_text_h(const char *s, int scale);
void gfx_blit(Tex *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, float rot_rad, int flip, uint32_t tint);

int  tex_new(int w, int h, uint32_t fill);
int  tex_file(const char *path);                              /* lazy: read on first use */
int  tex_from_img(Img *img, const char *name, bool protect);  /* takes ownership of img->px */
int  tex_mem(const uint8_t *d, size_t n, const char *name, char *err);
int  tex_proc(const char *kind, int size, uint32_t seed, uint32_t c1, uint32_t c2);
Tex *tex_get(int id);                                         /* loads/touches; NULL if invalid */
Tex *tex_peek(int id);                                        /* no load */
void tex_free(int id);
void tex_evict(size_t budget_bytes);
size_t tex_resident_bytes(void);
int  tex_count(void);
bool img_decode(const uint8_t *d, size_t n, Img *out, char *err);
uint8_t *gm_inflate(const uint8_t *in, size_t n, int mode, size_t *outlen, char *err);   /* mode 0 raw, 1 zlib, 2 gzip, 3 auto; err >= 256 bytes */
uint32_t gm_crc32(uint32_t crc, const uint8_t *d, size_t n);
bool img_save(const char *path, const uint32_t *px, int w, int h, char *err);
uint32_t tex_sample(const Tex *t, float u, float v);          /* nearest, repeat */
uint32_t tex_average(const Tex *t);

/* ----------------------------------------------------------------- model */
typedef struct { V3 p, n; float u, v; uint32_t c; } Vtx;
typedef struct { uint32_t first, count; int tex; uint32_t col; bool two_sided, translucent, unlit; } Part;
typedef struct {
    Vtx *v; int nv; uint32_t *idx; int ni;
    Part *parts; int np;
    V3 bmin, bmax;
    char fmt[24];
    size_t bytes;
    int *own_tex; int n_own;
} Mesh;
typedef struct {
    char *name, *path; Mesh *mesh;
    bool alive, builtin, failed;
    uint64_t last;
    float scale; bool unreal, flip_uv, flip_winding, center, ground, meters, lazy;
    int tex_override; uint32_t tint;
} Model;

void  mesh_free(Mesh *m);
Mesh *model_mesh(int id);                       /* loads lazily, touches LRU; NULL if invalid/failed */
Model *model_info(int id);
int   model_load(const char *path, const Opts *o);   /* registers (lazy unless "lazy", false) */
int   model_builtin(const char *name);          /* default custom models; -1 if unknown */
int   model_from_mesh(Mesh *m, const char *name);
int   model_count(void);
void  model_free(int id);
void  model_evict(size_t budget_bytes);
size_t model_resident_bytes(void);
KcValue model_builtin_names(void);
KcValue model_formats(void);
/* programmatic builder (game.model_begin ... game.model_end) */
void  mb_begin(void);
int   mb_vertex(V3 p, V3 n, float u, float v, uint32_t c, bool has_n);
void  mb_tri(int a, int b, int c);
void  mb_part(int tex, uint32_t col, bool two_sided, bool translucent, bool unlit);
Mesh *mb_end(void);                             /* finishes, computes normals/bounds */
int   mb_vcount(void);
void  mb_own_tex(int tex_id);                   /* texture freed together with the mesh */
void  mb_flat_normals(bool on);                 /* next mb_end recomputes ALL normals flat */
typedef struct { bool ue; float scale; bool meters, flip_winding, flip_uv; } LoadOpts;
bool  modelio_load(const char *path, const LoadOpts *lo, char *err);   /* parser; emits through mb_*; err >= 256 */
bool  model_save_kcm(Mesh *m, const char *path);
int   model_from_kcm(const char *text, const char *name, char *err);

/* ------------------------------------------------------------------- 3D */
typedef struct { float x, y, z, w, u, v, r, g, b, d; } RV;    /* clip-space vertex + attributes */
enum { RF_TWOSIDED = 1, RF_NOFOG = 2, RF_BLEND = 4, RF_NOZWRITE = 8, RF_UNLIT = 16, RF_WIRE = 32 };

typedef struct {
    int mode;                   /* 0 off, 1 linear, 2 exp, 3 cloud */
    float start, end, density;
    uint32_t color; bool auto_color;
    bool autofog;               /* clamp to streaming horizon (world) */
    float cur_end;              /* smoothed effective end */
    float cloud_t;
} FogCfg;
extern FogCfg R_fog;
extern uint32_t R_sky_top, R_sky_bot;
extern V3 R_light_dir; extern uint32_t R_light_col; extern float R_ambient;
extern bool R_sun, R_sky_clouds;

void  r3d_set_alpha(float a);                   /* alpha for RF_BLEND draws (0..1) */
void  r3d_begin(void);                          /* sky + clears; computes matrices from G.cam */
void  r3d_matrices(void);
const M4 *r3d_viewproj(void);
const M4 *r3d_view(void);
V3    r3d_forward(void);
V3    r3d_right(void);
V3    r3d_up(void);
void  r3d_draw_mesh(Mesh *m, const M4 *model, uint32_t tint, int tex_override, int flags);
void  r3d_tri(const RV *a, const RV *b, const RV *c, Tex *tex, int flags);   /* clip + raster (clip-space inputs) */
void  r3d_billboard(int tex, V3 p, float w, float h, uint32_t tint, int flags);
void  r3d_point(V3 p, float size_world, uint32_t c);
void  r3d_line(V3 a, V3 b, uint32_t c);
void  r3d_box(V3 mn, V3 mx, uint32_t c);
bool  r3d_project(V3 p, float *sx, float *sy, float *depth);
bool  r3d_aabb_visible(V3 mn, V3 mx);
void  r3d_ray(float sx, float sy, V3 *org, V3 *dir);
float r3d_fog_factor(float dist, int px, int py);
void  r3d_fog_update(float safe_dist, float dt);   /* auto-fog from streaming horizon */
void  r3d_sun_screen(float *sx, float *sy, bool *visible);

/* ----------------------------------------------------------------- world */
void  world_reset(void);
bool  world_open(const char *dir, const Opts *o);   /* dir may be "" (memory-only) */
bool  world_active(void);
void  world_update(float dt);                       /* streaming: load/unload/mesh around G.cam */
void  world_draw(void);
void  world_flush(void);                            /* save edited chunks */
int   world_get(int x, int y, int z, bool *loaded); /* block id (0 = air) */
bool  world_set(int x, int y, int z, int block);
bool  world_solid_at(int x, int y, int z, bool unloaded_solid);
float world_horizon(void);                          /* metres to nearest missing needed chunk in view */
int   world_surface(int x, int z, int *block);      /* top solid y of loaded column, -1 if unloaded */
bool  world_raycast(V3 o, V3 d, float maxd, int *hx, int *hy, int *hz, int *nx, int *ny, int *nz, int *blk, float *dist, bool *hit_unloaded);
void  world_move(V3 *p, float hw, float hh, V3 d, float stepup, bool flags[4]);   /* AABB sweep: [x,y(down),z,ground] */
int   block_define(const char *name, int tex_top, int tex_side, int tex_bot, bool solid, bool translucent, uint32_t color);
int   block_by_name(const char *name);
const char *block_name(int id);
uint32_t block_color(int id);
void  world_stats(KcValue o);
void  world_counts(int *loaded, int *needed, int *ready, size_t *bytes);
void  world_config_num(const char *k, double v);
void  world_config_str(const char *k, const char *v);
int   world_bake(int radius_chunks);
int   world_chunk_size(void);
int   world_height(void);
bool  world_chunk_loaded(int cx, int cz);
uint32_t world_gen_color(int x, int z);

/* ----------------------------------------------------------------- scene */
void  scene_reset(void);
void  scene_update(float dt);
void  scene_draw3d(void);
/* entities */
int   ent_spawn(int model, V3 pos);
void  ent_despawn(int id);
bool  ent_alive(int id);
bool  ent_set(int id, const char *key, KcValue *v, int nv);
KcValue ent_get(int id, const char *key);
bool  ent_move(int id, V3 d);
int   ent_count(void);
void  ent_apply_opts(int id, const Opts *o);
int   ent_pick(V3 org, V3 dir, float maxd, float *dist);
bool  sprite_draw(int tex, float x, float y, const Opts *o);   /* 2D textured draw (screen or world via camera2d) */
void  world_to_screen2d(float x, float y, float *sx, float *sy);
extern float G_gravity;
/* particles */
void  part_emit(bool d3, V3 p, int count, const Opts *o);
void  part_draw2d(void);
/* sprites / animations */
int   sheet_new(int tex, int cw, int ch, int margin, int spacing);
bool  sheet_draw(int sheet, int index, float x, float y, const Opts *o);
int   anim_new(int sheet, const int *frames, int nf, float fps, bool loop);
bool  anim_draw(int anim, float x, float y, const Opts *o);
void  anim_ctl(int anim, const char *what);
/* tilemaps */
int   tmap_new(int tw, int th, int sheet, const char *dir);
bool  tmap_set(int id, int x, int y, int tile);
int   tmap_get(int id, int x, int y);
void  tmap_draw(int id, const Opts *o);
void  tmap_solid(int id, int tile, bool solid);
bool  tmap_hit(int id, float x, float y, float w, float h);
void  tmap_move(int id, float *x, float *y, float w, float h, float dx, float dy, bool *hx, bool *hy);
int   tmap_import_csv(int id, const char *path, int ox, int oy);
int   tmap_flush(int id);
void  tmap_stats(int id, int *chunks, int *loaded_bytes);
bool  tmap_valid(int id);

/* ------------------------------------------------------------------- HUD */
void  hud_reset(void);
void  hud_update(float dt);
void  hud_draw(void);
int   hud_add(const char *kind, KcValue *a, int n);        /* a[] = args after kind */
bool  hud_set(int id, const char *key, KcValue v);
KcValue hud_get(int id, const char *key);
void  hud_remove(int id);
void  hud_clear(void);
int   hud_hit(int x, int y);
void  hud_notify(const char *text, uint32_t col, float secs);
void  hud_float_text(V3 p, const char *text, uint32_t col, float secs);
void  hud_dialog_open(const char *speaker, const char *text, KcValue *choices, int nch);
int   hud_dialog_result(void);                      /* -2 none yet, -1 closed w/o choice, else idx */
bool  hud_dialog_active(void);
void  hud_dialog_advance(void);
void  hud_tween(int id, const char *key, float target, float secs, const char *ease);
void  hud_debug(bool on);
void  hud_scale(int s);
bool  hud_click(int x, int y);                      /* mouse press routing; true if consumed */
void  hud_key(int key);                              /* dialog choice keys */

/* ------------------------------------------------------------------ game */
int   key_code(const char *name);                   /* -1 if unknown */
const char *key_name(int code);
void  game_begin_frame(void);                       /* clears (sky) once per frame */
void  game_end_frame(void);                         /* HUD + present */
KcModule *kc_mod_game(void);

#endif
