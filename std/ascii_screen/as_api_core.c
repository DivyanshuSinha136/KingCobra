/* std.ascii_screen — bindings: screen lifecycle, main loop, live input, controller, math helpers. */
#include "as_api.h"

bool AS_game_auto = true; static bool hud_auto = true;
KcValue col_ret(Col c) { return (c & COL_SET) ? kc_int((int64_t)(c & 0xFFFFFF)) : kc_int(-1); }
static void obj_set(KcValue o, const char *k, KcValue v) { kc_object_set(o, k, strlen(k), v); }
static void hook_restore(void) { term_restore_for_error(); }

bool as_do_poll(void) {
    if (!term_poll()) return false;
    anims_update_frame(T.dt, T.frame); hud_update((float)T.dt);
    if (AS_game_auto) game_update((float)T.dt);
    return true;
}
static void do_begin(void) { Canvas *s = S_screen; cv_reset_clip(s); cv_clear(s, ' ', 0, 0, 0); T.cur_show = false; }
static void do_present(void) {
    if (S_target != 0) S_target = 0;
    if (hud_auto) hud_draw(S_screen);
    term_present(); term_frame_wait();
}

/* ------------------------------------------------------------ lifecycle */
NAT(s_open) { AC.ip = ip; AC.pos = pos; Opts o = opts_make(a, n, 0); kc_error_hook = hook_restore; term_open(ip, &o); return OKV; }
NAT(s_close) { BEGIN(0, 0, "close()"); term_close(); return OKV; }
NAT(s_is_open) { BEGIN(0, 0, "is_open()"); return kc_bool(T.open); }
NAT(s_headless) {
    BEGIN(0, 1, "headless([on])"); bool on = n ? A_bool(a, n, 0, "headless") : true;
    if (on) { if (T.open && !T.headless) term_close(); KcValue kv[2] = { kc_string("headless"), kc_bool(true) }; Opts o = opts_make(kv, 2, 0); if (!T.open) term_open(ip, &o); }
    return OKV;
}
NAT(s_is_headless) { BEGIN(0, 0, "is_headless()"); term_ensure(); return kc_bool(T.headless); }
NAT(s_size) { BEGIN(0, 2, "size() | size(w, h)"); term_ensure(); if (n == 2) { if (!T.headless) as_err("size(w, h) only applies to headless screens (a real terminal sets its own size)"); term_resize_virtual(A_int(a, n, 0, "size"), A_int(a, n, 1, "size")); return OKV; } KcValue r = kc_array_new(); kc_array_push(r, kc_int(T.w)); kc_array_push(r, kc_int(T.h)); return r; }
NAT(s_width) { BEGIN(0, 0, "width()"); term_ensure(); return kc_int(T.w); }
NAT(s_height) { BEGIN(0, 0, "height()"); term_ensure(); return kc_int(T.h); }
NAT(s_resized) { BEGIN(0, 0, "resized()"); return kc_bool(T.resized); }
NAT(s_title) { BEGIN(1, 1, "title(text)"); term_ensure(); term_set_title(A_str(a, n, 0, "title")); return OKV; }
NAT(s_caps) {
    BEGIN(0, 0, "caps()"); term_ensure(); KcValue o = kc_object_new();
    obj_set(o, "backend", kc_string(T.backend)); obj_set(o, "keys", kc_string(T.keymode)); obj_set(o, "headless", kc_bool(T.headless)); obj_set(o, "kitty_keys", kc_bool(T.kitty));
    obj_set(o, "color", kc_string(T.colors == 0 ? "none" : T.colors == 16 ? "ansi16" : T.colors == 256 ? "ansi256" : "truecolor")); obj_set(o, "mouse", kc_bool(T.mouse_on)); obj_set(o, "unicode", kc_bool(true));
    obj_set(o, "ffmpeg", kc_bool(ffmpeg_available())); obj_set(o, "width", kc_int(T.w)); obj_set(o, "height", kc_int(T.h)); return o;
}
NAT(s_color_mode) {
    BEGIN(0, 1, "color_mode([mode])"); term_ensure();
    if (n == 1) { const char *s = A_str(a, n, 0, "color_mode"); int c = !strcmp(s, "none") ? 0 : !strcmp(s, "ansi16") ? 16 : !strcmp(s, "ansi256") ? 256 : !strcmp(s, "truecolor") ? 24 : -1; if (c < 0) as_err("color_mode: use none, ansi16, ansi256 or truecolor"); T.colors = c; term_refresh(); return OKV; }
    return kc_string(T.colors == 0 ? "none" : T.colors == 16 ? "ansi16" : T.colors == 256 ? "ansi256" : "truecolor");
}
NAT(s_cursor) { BEGIN(1, 3, "cursor(visible) | cursor(x, y)"); if (n == 1) { T.cur_show = A_bool(a, n, 0, "cursor"); return OKV; } T.cur_x = A_int(a, n, 0, "cursor"); T.cur_y = A_int(a, n, 1, "cursor"); T.cur_show = true; return OKV; }
NAT(s_bell) { BEGIN(0, 0, "bell()"); term_bell(); return OKV; }
NAT(s_refresh) { BEGIN(0, 0, "refresh()"); term_refresh(); return OKV; }

/* ------------------------------------------------------------ main loop */
NAT(s_poll) { BEGIN(0, 0, "poll()"); term_ensure(); return kc_bool(as_do_poll()); }
NAT(s_begin) { BEGIN(0, 0, "begin()"); term_ensure(); do_begin(); return OKV; }
NAT(s_present) { BEGIN(0, 0, "present()"); term_ensure(); do_present(); return OKV; }
NAT(s_quit) { BEGIN(0, 0, "quit()"); T.quit = true; return OKV; }
NAT(s_should_quit) { BEGIN(0, 0, "should_quit()"); return kc_bool(T.quit); }
NAT(s_run) {
    BEGIN(1, 3, "run(update [, draw [, max_frames]])"); term_ensure(); if (!as_is_fn(a[0])) as_err("run: first argument must be a function");
    KcValue f1 = a[0], f2 = kc_null(); bool has2 = false; int64_t maxf = -1;
    for (int i = 1; i < n; i++) { if (as_is_fn(a[i])) { f2 = a[i]; has2 = true; } else maxf = (int64_t)A_int(a, n, i, "run"); }
    int64_t count = 0;
    while (as_do_poll()) {
        KcValue dt = kc_float(T.dt);
        if (!has2) { do_begin(); kc_call_value(ip, f1, &dt, 1, pos); }
        else { kc_call_value(ip, f1, &dt, 1, pos); do_begin(); kc_call_value(ip, f2, NULL, 0, pos); }
        do_present(); if (maxf >= 0 && ++count >= maxf) break;
    }
    return OKV;
}
NAT(s_dt) { BEGIN(0, 0, "dt()"); return kc_float(T.dt); }
NAT(s_time) { BEGIN(0, 0, "time()"); return kc_float(T.time); }
NAT(s_fps) { BEGIN(0, 0, "fps()"); return kc_float(T.fps); }
NAT(s_frame) { BEGIN(0, 0, "frame()"); return kc_int(T.frame); }
NAT(s_target_fps) { BEGIN(1, 1, "target_fps(n)"); double f = A_num(a, n, 0, "target_fps"); if (f < 1 || f > 1000) as_err("target_fps: use 1..1000"); T.target_fps = f; return OKV; }
NAT(s_advance) { BEGIN(1, 1, "advance(seconds)"); term_ensure(); double d = A_num(a, n, 0, "advance"); if (d <= 0) as_err("advance: seconds must be > 0"); term_advance(d); return OKV; }
NAT(s_stats) {
    BEGIN(0, 0, "stats()"); term_ensure(); KcValue o = kc_object_new(); obj_set(o, "fps", kc_float(T.fps)); obj_set(o, "frame", kc_int(T.frame)); obj_set(o, "dt", kc_float(T.dt)); obj_set(o, "time", kc_float(T.time));
    obj_set(o, "width", kc_int(T.w)); obj_set(o, "height", kc_int(T.h)); obj_set(o, "bytes_out", kc_int((int64_t)T.bytes_out)); obj_set(o, "cells_changed", kc_int(T.cells_changed)); obj_set(o, "entities", kc_int(game_entity_count()));
    obj_set(o, "backend", kc_string(T.backend)); obj_set(o, "keys", kc_string(T.keymode)); return o;
}
NAT(s_reset) { BEGIN(0, 0, "reset()"); game_reset(); hud_reset(); anims_free_all(); effects_free_all(); maps_free_all(); S_target = 0; S_style.fg = S_style.bg = 0; S_style.attr = 0; return OKV; }
NAT(s_hud_auto) { BEGIN(1, 1, "hud_auto(on)"); hud_auto = A_bool(a, n, 0, "hud_auto"); return OKV; }
NAT(s_game_auto) { BEGIN(1, 1, "game_auto(on)"); AS_game_auto = A_bool(a, n, 0, "game_auto"); return OKV; }

/* ------------------------------------------------------------ keyboard / mouse */
static int kc_(KcValue *a, int n, int i, const char *who) { const char *s = A_str(a, n, i, who); int c = key_code(s); if (c < 0) as_err("%s: unknown key \"%s\"", who, s); return c; }
NAT(i_key) { BEGIN(1, 1, "key(name)"); term_ensure(); return kc_bool(term_key(kc_(a, n, 0, "key"))); }
NAT(i_key_pressed) { BEGIN(1, 1, "key_pressed(name)"); term_ensure(); return kc_bool(term_key_pressed(kc_(a, n, 0, "key_pressed"))); }
NAT(i_key_released) { BEGIN(1, 1, "key_released(name)"); term_ensure(); return kc_bool(term_key_released(kc_(a, n, 0, "key_released"))); }
NAT(i_key_repeat) { BEGIN(1, 1, "key_repeat(name)"); term_ensure(); return kc_bool(term_key_repeat(kc_(a, n, 0, "key_repeat"))); }
NAT(i_keys_down) { BEGIN(0, 0, "keys_down()"); term_ensure(); KcValue r = kc_array_new(); for (int i = 1; i < KEYN; i++) if (term_key(i)) { const char *nm = key_name(i); if (nm) kc_array_push(r, kc_string(nm)); } return r; }
NAT(i_last_key) { BEGIN(0, 0, "last_key()"); const char *nm = T.last_key >= 0 ? key_name(T.last_key) : NULL; return nm ? kc_string(nm) : kc_null(); }
NAT(i_typed) { BEGIN(0, 0, "typed()"); return kc_string(T.typed); }
NAT(i_mods) { BEGIN(0, 0, "mods()"); KcValue o = kc_object_new(); obj_set(o, "shift", kc_bool(T.mods & 1)); obj_set(o, "alt", kc_bool(T.mods & 2)); obj_set(o, "ctrl", kc_bool(T.mods & 4)); obj_set(o, "super", kc_bool(T.mods & 8)); return o; }
NAT(i_key_hold) { BEGIN(1, 2, "key_hold(first_seconds [, repeat_seconds])"); T.hold_first = A_num(a, n, 0, "key_hold"); if (n > 1) T.hold_repeat = A_num(a, n, 1, "key_hold"); return OKV; }
NAT(i_text_input_active) { BEGIN(0, 0, "text_input_active()"); return kc_bool(hud_input_focused()); }
NAT(i_mouse) { BEGIN(0, 0, "mouse()"); KcValue r = kc_array_new(); kc_array_push(r, kc_int(T.mx)); kc_array_push(r, kc_int(T.my)); return r; }
NAT(i_mouse_delta) { BEGIN(0, 0, "mouse_delta()"); KcValue r = kc_array_new(); kc_array_push(r, kc_int(T.dmx)); kc_array_push(r, kc_int(T.dmy)); return r; }
static int mb(KcValue *a, int n, int i, const char *who) { if (a[i].type == VAL_STRING) { const char *s = a[i].as.s->data; if (!strcmp(s, "left")) return 1; if (!strcmp(s, "middle")) return 2; if (!strcmp(s, "right")) return 3; as_err("%s: button must be \"left\", \"middle\" or \"right\"", who); } int b = A_int(a, n, i, who); return b; }
NAT(i_mouse_down) { BEGIN(0, 1, "mouse_down([button])"); return kc_bool(term_mouse_down(n ? mb(a, n, 0, "mouse_down") : 1)); }
NAT(i_mouse_pressed) { BEGIN(0, 1, "mouse_pressed([button])"); return kc_bool(term_mouse_pressed(n ? mb(a, n, 0, "mouse_pressed") : 1)); }
NAT(i_mouse_released) { BEGIN(0, 1, "mouse_released([button])"); return kc_bool(term_mouse_released(n ? mb(a, n, 0, "mouse_released") : 1)); }
NAT(i_wheel) { BEGIN(0, 0, "wheel()"); return kc_int(T.wheel_y); }
NAT(i_send_key) {
    BEGIN(1, 3, "send_key(name [, down [, mods]])"); term_ensure(); int c = kc_(a, n, 0, "send_key"); int mods = n > 2 ? A_int(a, n, 2, "send_key") : 0;
    if (n >= 2 && a[1].type != VAL_STRING) term_send_key(c, A_bool(a, n, 1, "send_key"), mods); else term_send_key_tap(c, mods); return OKV;
}
NAT(i_send_mouse) {
    BEGIN(2, 4, "send_mouse(x, y [, button [, \"press\"|\"release\"|\"move\"|\"wheel\"]])"); term_ensure(); int x = A_int(a, n, 0, "send_mouse"), y = A_int(a, n, 1, "send_mouse"), b = n > 2 ? mb(a, n, 2, "send_mouse") : 1; const char *k = n > 3 ? A_str(a, n, 3, "send_mouse") : "press";
    if (!strcmp(k, "press")) term_send_mouse(x, y, b, 1); else if (!strcmp(k, "release")) term_send_mouse(x, y, b, 2); else if (!strcmp(k, "move")) term_send_mouse(x, y, 0, 0); else if (!strcmp(k, "wheel")) term_send_mouse(x, y, A_int(a, n, 2, "send_mouse"), 3);
    else as_err("send_mouse: kind must be press, release, move or wheel");
    return OKV;
}
NAT(i_send_text) { BEGIN(1, 1, "send_text(text)"); term_ensure(); term_send_text(A_str(a, n, 0, "send_text")); return OKV; }

/* ------------------------------------------------------------ controller */
NAT(c_controller) { BEGIN(0, 1, "controller([preset])"); controller_preset(n ? A_str(a, n, 0, "controller") : "both"); return OKV; }
NAT(c_bind) { BEGIN(2, 12, "bind(action, key, ...)"); bind_action(A_str(a, n, 0, "bind"), a + 1, n - 1); return OKV; }
NAT(c_unbind) { BEGIN(1, 1, "unbind(action)"); unbind_action(A_str(a, n, 0, "unbind")); return OKV; }
NAT(c_action) { BEGIN(1, 1, "action(name)"); return kc_bool(action_state(A_str(a, n, 0, "action"), 0)); }
NAT(c_action_pressed) { BEGIN(1, 1, "action_pressed(name)"); return kc_bool(action_state(A_str(a, n, 0, "action_pressed"), 1)); }
NAT(c_action_released) { BEGIN(1, 1, "action_released(name)"); return kc_bool(action_state(A_str(a, n, 0, "action_released"), 2)); }
NAT(c_action_repeat) { BEGIN(1, 1, "action_repeat(name)"); return kc_bool(action_state(A_str(a, n, 0, "action_repeat"), 3)); }
NAT(c_axis) { BEGIN(1, 1, "axis(name)"); return kc_float(axis_value(A_str(a, n, 0, "axis"))); }
NAT(c_bind_axis) { BEGIN(3, 3, "bind_axis(name, negative_action, positive_action)"); bind_axis(A_str(a, n, 0, "bind_axis"), A_str(a, n, 1, "bind_axis"), A_str(a, n, 2, "bind_axis")); return OKV; }
NAT(c_actions) { BEGIN(0, 0, "actions()"); return actions_list(); }
NAT(c_mouse_world) { BEGIN(0, 0, "mouse_world()"); double wx, wy; screen_to_world(T.mx, T.my, &wx, &wy); KcValue r = kc_array_new(); kc_array_push(r, kc_float(wx)); kc_array_push(r, kc_float(wy)); return r; }

/* ------------------------------------------------------------ math / util */
#define M1(nm, expr) NAT(nm) { BEGIN(1, 1, #nm "(x)"); double x = A_num(a, n, 0, #nm); (void)x; return kc_float(expr); }
M1(m_sin, sin(x)) M1(m_cos, cos(x)) M1(m_tan, tan(x)) M1(m_asin, asin(x)) M1(m_acos, acos(x)) M1(m_atan, atan(x)) M1(m_sqrt, sqrt(x)) M1(m_rad, x * M_PI / 180.0) M1(m_deg, x * 180.0 / M_PI)
NAT(m_dsin) { BEGIN(1, 1, "dsin(deg)"); return kc_float(sin(A_num(a, n, 0, "dsin") * M_PI / 180)); }
NAT(m_dcos) { BEGIN(1, 1, "dcos(deg)"); return kc_float(cos(A_num(a, n, 0, "dcos") * M_PI / 180)); }
NAT(m_atan2) { BEGIN(2, 2, "atan2(y, x)"); return kc_float(atan2(A_num(a, n, 0, "atan2"), A_num(a, n, 1, "atan2"))); }
NAT(m_pow) { BEGIN(2, 2, "pow(x, y)"); return kc_float(pow(A_num(a, n, 0, "pow"), A_num(a, n, 1, "pow"))); }
NAT(m_abs) { BEGIN(1, 1, "abs(x)"); if (a[0].type == VAL_INT) return kc_int(a[0].as.i < 0 ? -a[0].as.i : a[0].as.i); return kc_float(fabs(A_num(a, n, 0, "abs"))); }
NAT(m_floor) { BEGIN(1, 1, "floor(x)"); return kc_int((int64_t)floor(A_num(a, n, 0, "floor"))); }
NAT(m_ceil) { BEGIN(1, 1, "ceil(x)"); return kc_int((int64_t)ceil(A_num(a, n, 0, "ceil"))); }
NAT(m_round) { BEGIN(1, 1, "round(x)"); return kc_int((int64_t)floor(A_num(a, n, 0, "round") + 0.5)); }
NAT(m_sign) { BEGIN(1, 1, "sign(x)"); double x = A_num(a, n, 0, "sign"); return kc_int(x > 0 ? 1 : x < 0 ? -1 : 0); }
NAT(m_min) { BEGIN(2, 2, "min(a, b)"); if (a[0].type == VAL_INT && a[1].type == VAL_INT) return kc_int(a[0].as.i < a[1].as.i ? a[0].as.i : a[1].as.i); double x = A_num(a, n, 0, "min"), y = A_num(a, n, 1, "min"); return kc_float(x < y ? x : y); }
NAT(m_max) { BEGIN(2, 2, "max(a, b)"); if (a[0].type == VAL_INT && a[1].type == VAL_INT) return kc_int(a[0].as.i > a[1].as.i ? a[0].as.i : a[1].as.i); double x = A_num(a, n, 0, "max"), y = A_num(a, n, 1, "max"); return kc_float(x > y ? x : y); }
NAT(m_clamp) { BEGIN(3, 3, "clamp(x, lo, hi)"); double x = A_num(a, n, 0, "clamp"), lo = A_num(a, n, 1, "clamp"), hi = A_num(a, n, 2, "clamp"); if (a[0].type == VAL_INT && a[1].type == VAL_INT && a[2].type == VAL_INT) return kc_int((int64_t)(x < lo ? lo : x > hi ? hi : x)); return kc_float(x < lo ? lo : x > hi ? hi : x); }
NAT(m_lerp) { BEGIN(3, 3, "lerp(a, b, t)"); double x = A_num(a, n, 0, "lerp"), y = A_num(a, n, 1, "lerp"), t = A_num(a, n, 2, "lerp"); return kc_float(x + (y - x) * t); }
NAT(m_smoothstep) { BEGIN(3, 3, "smoothstep(edge0, edge1, x)"); double e0 = A_num(a, n, 0, "smoothstep"), e1 = A_num(a, n, 1, "smoothstep"), x = A_num(a, n, 2, "smoothstep"); double t = e1 == e0 ? 0 : (x - e0) / (e1 - e0); t = t < 0 ? 0 : t > 1 ? 1 : t; return kc_float(t * t * (3 - 2 * t)); }
NAT(m_rand) { BEGIN(0, 2, "rand() | rand(max) | rand(lo, hi)"); double r = as_rand(); if (n == 1) return kc_float(r * A_num(a, n, 0, "rand")); if (n == 2) { double lo = A_num(a, n, 0, "rand"), hi = A_num(a, n, 1, "rand"); return kc_float(lo + r * (hi - lo)); } return kc_float(r); }
NAT(m_rand_int) { BEGIN(2, 2, "rand_int(lo, hi)  (inclusive)"); int lo = A_int(a, n, 0, "rand_int"), hi = A_int(a, n, 1, "rand_int"); if (hi < lo) { int t = lo; lo = hi; hi = t; } return kc_int(lo + (int)(as_rand_u32() % (uint32_t)(hi - lo + 1))); }
NAT(m_seed) { BEGIN(1, 1, "seed(n)"); as_seed((uint32_t)A_int(a, n, 0, "seed")); return OKV; }
NAT(m_noise) { BEGIN(2, 3, "noise(x, y [, seed])"); return kc_float(as_noise2((float)A_num(a, n, 0, "noise"), (float)A_num(a, n, 1, "noise"), n > 2 ? (uint32_t)A_int(a, n, 2, "noise") : 0)); }
NAT(m_fbm) { BEGIN(2, 4, "fbm(x, y [, octaves [, seed]])"); return kc_float(as_fbm((float)A_num(a, n, 0, "fbm"), (float)A_num(a, n, 1, "fbm"), n > 3 ? (uint32_t)A_int(a, n, 3, "fbm") : 0, n > 2 ? A_int(a, n, 2, "fbm") : 4)); }
NAT(m_rgb) { BEGIN(3, 3, "rgb(r, g, b)"); return col_ret(col_rgb(A_int(a, n, 0, "rgb"), A_int(a, n, 1, "rgb"), A_int(a, n, 2, "rgb"))); }
NAT(m_hsv) { BEGIN(3, 3, "hsv(h 0..1, s 0..1, v 0..1)"); return col_ret(col_hsv((float)A_num(a, n, 0, "hsv"), (float)A_num(a, n, 1, "hsv"), (float)A_num(a, n, 2, "hsv"))); }
NAT(m_mix) { BEGIN(3, 3, "mix(color_a, color_b, t)"); return col_ret(col_mix(A_col(a, n, 0, "mix"), A_col(a, n, 1, "mix"), (float)A_num(a, n, 2, "mix"))); }
NAT(m_shade) { BEGIN(2, 2, "shade(color, factor)"); return col_ret(col_scale(A_col(a, n, 0, "shade"), (float)A_num(a, n, 1, "shade"))); }

KcModuleEntry AS_T_CORE[] = {
    E("open", s_open), E("screen", s_open), E("close", s_close), E("is_open", s_is_open), E("headless", s_headless), E("is_headless", s_is_headless), E("size", s_size), E("width", s_width), E("height", s_height), E("resized", s_resized),
    E("title", s_title), E("caps", s_caps), E("color_mode", s_color_mode), E("cursor", s_cursor), E("bell", s_bell), E("refresh", s_refresh),
    E("poll", s_poll), E("begin", s_begin), E("present", s_present), E("quit", s_quit), E("should_quit", s_should_quit), E("run", s_run), E("dt", s_dt), E("time", s_time), E("fps", s_fps), E("frame", s_frame),
    E("target_fps", s_target_fps), E("advance", s_advance), E("stats", s_stats), E("reset", s_reset), E("hud_auto", s_hud_auto), E("game_auto", s_game_auto),
    E("key", i_key), E("key_pressed", i_key_pressed), E("key_released", i_key_released), E("key_repeat", i_key_repeat), E("keys_down", i_keys_down), E("last_key", i_last_key), E("typed", i_typed), E("mods", i_mods), E("key_hold", i_key_hold),
    E("text_input_active", i_text_input_active), E("mouse", i_mouse), E("mouse_delta", i_mouse_delta), E("mouse_down", i_mouse_down), E("mouse_pressed", i_mouse_pressed), E("mouse_released", i_mouse_released), E("wheel", i_wheel),
    E("send_key", i_send_key), E("send_mouse", i_send_mouse), E("send_text", i_send_text),
    E("controller", c_controller), E("bind", c_bind), E("unbind", c_unbind), E("action", c_action), E("action_pressed", c_action_pressed), E("action_released", c_action_released), E("action_repeat", c_action_repeat),
    E("axis", c_axis), E("bind_axis", c_bind_axis), E("actions", c_actions), E("mouse_world", c_mouse_world),
    E("sin", m_sin), E("cos", m_cos), E("tan", m_tan), E("asin", m_asin), E("acos", m_acos), E("atan", m_atan), E("atan2", m_atan2), E("dsin", m_dsin), E("dcos", m_dcos), E("sqrt", m_sqrt), E("abs", m_abs), E("floor", m_floor), E("ceil", m_ceil), E("round", m_round),
    E("rad", m_rad), E("deg", m_deg), E("sign", m_sign), E("pow", m_pow), E("min", m_min), E("max", m_max), E("clamp", m_clamp), E("lerp", m_lerp), E("smoothstep", m_smoothstep),
    E("rand", m_rand), E("rand_int", m_rand_int), E("seed", m_seed), E("noise", m_noise), E("fbm", m_fbm), E("rgb", m_rgb), E("hsv", m_hsv), E("mix", m_mix), E("shade", m_shade),
};
const int AS_N_CORE = sizeof(AS_T_CORE) / sizeof(AS_T_CORE[0]);
