/* std.ascii_screen — bindings: images, SVG, image->ASCII, fonts/big text, animations & video, effects, transitions. */
#include "as_api.h"

/* image argument: numeric id or a file path (loaded temporarily) */
Image *img_arg(KcValue *a, int n, int i, int *temp_id, const char *who) {
    *temp_id = 0;
    if (i >= n) as_err("%s: missing image argument", who);
    if (a[i].type == VAL_STRING) { *temp_id = im_load(a[i].as.s->data, 0, 0); return im_get(*temp_id); }
    return im_must(A_int(a, n, i, who), who);
}
static void ropts(RenderOpts *r, const Opts *o, bool default_color) {
    render_opts_default(r); r->color = default_color ? 1 : 0; render_opts_parse(r, o);
}
static int ansi_mode(const Opts *o) {                                  /* "ansi": none|16|256|truecolor|true */
    const KcValue *v = opt_find(o, "ansi"); if (!v) return 0;
    if (v->type == VAL_STRING) { const char *s = v->as.s->data; if (!strcmp(s, "none") || !strcmp(s, "off")) return 0; if (!strcmp(s, "16") || !strcmp(s, "ansi16")) return 1; if (!strcmp(s, "256") || !strcmp(s, "ansi256")) return 2; return 3; }
    return kc_truthy(*v) ? 3 : 0;
}

NAT(m_image) {
    BEGIN(1, 7, "image(path [, \"width\", px, \"height\", px])  -- PNG, JPEG, GIF, BMP, TGA, PPM/PGM, SVG (+ anything ffmpeg reads)"); Opts o = opts_make(a, n, 1);
    return kc_int(im_load(A_str(a, n, 0, "image"), opt_int(&o, "width", 0), opt_int(&o, "height", 0)));
}
NAT(m_image_new) { BEGIN(2, 3, "image_new(w, h [, color])"); Col c = n > 2 ? col_from_value(a[2], "image_new") : col_rgb(0, 0, 0); return kc_int(im_new(A_int(a, n, 0, "image_new"), A_int(a, n, 1, "image_new"), 0xFF000000u | (c & 0xFFFFFF))); }
NAT(m_image_free) { BEGIN(1, 1, "image_free(id)"); im_free(A_int(a, n, 0, "image_free")); return OKV; }
NAT(m_image_size) { BEGIN(1, 1, "image_size(id)"); Image *im = im_must(A_int(a, n, 0, "image_size"), "image_size"); KcValue r = kc_array_new(); kc_array_push(r, kc_int(im->w)); kc_array_push(r, kc_int(im->h)); return r; }
NAT(m_image_frames) { BEGIN(1, 1, "image_frames(id)  -> frame count (GIF animation)"); Image *im = im_must(A_int(a, n, 0, "image_frames"), "image_frames"); return kc_int(im->nframes ? im->nframes : 1); }
NAT(m_image_get) { BEGIN(3, 3, "image_get(id, x, y)"); Image *im = im_must(A_int(a, n, 0, "image_get"), "image_get"); int x = A_int(a, n, 1, "image_get"), y = A_int(a, n, 2, "image_get"); if (x < 0 || y < 0 || x >= im->w || y >= im->h) return kc_int(-1); return kc_int(im->px[(size_t)y * im->w + x] & 0xFFFFFF); }
NAT(m_image_set) { BEGIN(4, 4, "image_set(id, x, y, color)"); Image *im = im_must(A_int(a, n, 0, "image_set"), "image_set"); int x = A_int(a, n, 1, "image_set"), y = A_int(a, n, 2, "image_set"); if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[(size_t)y * im->w + x] = 0xFF000000u | (A_col(a, n, 3, "image_set") & 0xFFFFFF); return OKV; }
NAT(m_image_save) { BEGIN(2, 2, "image_save(id, path.png|.bmp|.ppm|.tga)"); Image *im = im_must(A_int(a, n, 0, "image_save"), "image_save"); char err[256] = ""; if (!im_save(im, A_str(a, n, 1, "image_save"), err)) as_err("image_save: %s", err[0] ? err : "write failed"); return OKV; }
NAT(m_svg) {
    BEGIN(1, 7, "svg(path_or_markup [, \"width\", px, \"height\", px])  -> image id"); const char *s = A_str(a, n, 0, "svg"); Opts o = opts_make(a, n, 1); int w = opt_int(&o, "width", 0), h = opt_int(&o, "height", 0);
    const char *t = s; while (isspace((unsigned char)*t)) t++;
    if (*t == '<') { Image im; memset(&im, 0, sizeof im); char err[256] = ""; if (!svg_render(s, strlen(s), w, h, &im, err)) as_err("svg: %s", err); return kc_int(im_from_rgba(im.w, im.h, im.px)); }
    return kc_int(im_load(s, w, h));
}
NAT(m_image_to_canvas) {
    BEGIN(1, 41, "image_to_canvas(image|path, options...)  -> canvas id"); int tmp; Image *im = img_arg(a, n, 0, &tmp, "image_to_canvas"); Opts o = opts_make(a, n, 1); RenderOpts r; ropts(&r, &o, true);
    int w, h, id = render_image_to_canvas(im, &r, &w, &h); if (tmp) im_free(tmp); return kc_int(id);
}
NAT(m_image_to_ascii) {
    BEGIN(1, 41, "image_to_ascii(image|path, options...)  -> string.  options: width, height, mode, ramp, color, ansi, dither, contrast, brightness, gamma, invert, fit..."); int tmp; Image *im = img_arg(a, n, 0, &tmp, "image_to_ascii"); Opts o = opts_make(a, n, 1);
    int am = ansi_mode(&o); RenderOpts r; ropts(&r, &o, am > 0); if (am > 0 && r.color == 0) r.color = 1; int w, h, id = render_image_to_canvas(im, &r, &w, &h);
    size_t l; char *t = cv_to_text(cv_get(id), 0, 0, 0, 0, am, true, &l); KcValue rv = ret_str(t, l); free(t); cv_free(id); if (tmp) im_free(tmp); return rv;
}
NAT(m_draw_image) {
    BEGIN(3, 43, "draw_image(image|path, x, y, options...)  -- renders into the current target"); int tmp; Image *im = img_arg(a, n, 0, &tmp, "draw_image"); int x = A_int(a, n, 1, "draw_image"), y = A_int(a, n, 2, "draw_image"); Opts o = opts_make(a, n, 3);
    RenderOpts r; ropts(&r, &o, T.colors != 0 || !T.open); if (!opt_has(&o, "transparent")) r.transparent = true; Canvas *c = cv_target(); render_image_into(im, &r, c, x, y);
    int w, h; render_dims(im, &r, &w, &h); if (tmp) im_free(tmp); KcValue rv = kc_array_new(); kc_array_push(rv, kc_int(w)); kc_array_push(rv, kc_int(h)); return rv;
}
NAT(m_ramps) { BEGIN(0, 0, "ramps()"); KcValue r = kc_array_new(); for (int i = 0; RAMP_NAMES[i]; i++) kc_array_push(r, kc_string(RAMP_NAMES[i])); return r; }

/* ------------------------------------------------------------ fonts */
NAT(f_font_load) { BEGIN(1, 3, "font_load(path [, name])  -- FIGlet .flf/.tlf or BDF"); return kc_int(font_load_file(A_str(a, n, 0, "font_load"), n > 1 ? A_str(a, n, 1, "font_load") : NULL)); }
NAT(f_font_define) { BEGIN(4, 4, "font_define(name, height, chars, glyphs)  glyphs[i] = array of rows (\"#\"=on)"); return kc_int(font_define(A_str(a, n, 0, "font_define"), A_int(a, n, 1, "font_define"), A_str(a, n, 2, "font_define"), a[3])); }
NAT(f_font_free) { BEGIN(1, 1, "font_free(id)"); font_free_id(font_find(a[0], "font_free")); return OKV; }
NAT(f_fonts) { BEGIN(0, 0, "fonts()"); KcValue r = kc_array_new(); for (int i = 0; FONT_BUILTIN_NAMES[i]; i++) kc_array_push(r, kc_string(FONT_BUILTIN_NAMES[i])); return r; }
static int font_opt(const Opts *o) { const KcValue *v = opt_find(o, "font"); return font_find(v ? *v : kc_string("half"), "font"); }
NAT(f_big_text) {
    BEGIN(3, 30, "big_text(x, y, text, options...)  options: font, fg, gradient, rainbow, align, scale, spacing, shadow, style, layout"); Opts o = opts_make(a, n, 3); int w, h; const char *s = A_str(a, n, 2, "big_text");
    big_text_render(font_opt(&o), s, &o, cv_target(), A_int(a, n, 0, "big_text"), A_int(a, n, 1, "big_text"), true, &w, &h); KcValue r = kc_array_new(); kc_array_push(r, kc_int(w)); kc_array_push(r, kc_int(h)); return r;
}
NAT(f_big_text_size) { BEGIN(1, 20, "big_text_size(text, options...)"); Opts o = opts_make(a, n, 1); int w, h; big_text_render(font_opt(&o), A_str(a, n, 0, "big_text_size"), &o, NULL, 0, 0, false, &w, &h); KcValue r = kc_array_new(); kc_array_push(r, kc_int(w)); kc_array_push(r, kc_int(h)); return r; }
NAT(f_big_text_lines) { BEGIN(1, 20, "big_text_lines(text, options...)  -> array of strings"); Opts o = opts_make(a, n, 1); return big_text_lines(font_opt(&o), A_str(a, n, 0, "big_text_lines"), &o); }

/* ------------------------------------------------------------ animations */
NAT(n_anim_new) { BEGIN(0, 1, "anim_new([fps])"); return kc_int(anim_new(n ? A_num(a, n, 0, "anim_new") : 12)); }
NAT(n_anim_add) { BEGIN(2, 3, "anim_add(anim, canvas [, delay_ms])"); int id = A_int(a, n, 0, "anim_add"); if (!anim_valid(id)) as_err("anim_add: invalid animation %d", id); anim_add_canvas(id, cv_must(A_int(a, n, 1, "anim_add"), "anim_add"), n > 2 ? A_int(a, n, 2, "anim_add") : 0); return OKV; }
NAT(n_anim_from_text) {
    BEGIN(1, 3, "anim_from_text(frames_array [, fps])  -- each frame is a string"); if (a[0].type != VAL_ARRAY || !a[0].as.arr->count) as_err("anim_from_text: expected a non-empty array of strings"); int id = anim_new(n > 1 ? A_num(a, n, 1, "anim_from_text") : 12);
    for (int i = 0; i < a[0].as.arr->count; i++) { KcValue f = a[0].as.arr->items[i]; if (f.type != VAL_STRING) as_err("anim_from_text: frame %d is not a string", i); int cid = cv_from_text(f.as.s->data, strlen(f.as.s->data), false, 0); Canvas *c = cv_get(cid);
        if (i == 0) anim_add_canvas(id, c, 0); else { int w, h, nf, cur; double fps; bool pl, fin, st; anim_info(id, &w, &h, &nf, &fps, &cur, &pl, &fin, &st); int pid = cv_new(w, h); cv_blit(cv_get(pid), c, 0, 0, 0, 0, 0, 0, false, false); anim_add_canvas(id, cv_get(pid), 0); cv_free(pid); } cv_free(cid); }
    return kc_int(id);
}
NAT(n_anim_from_images) {
    BEGIN(1, 41, "anim_from_images(paths_array, options...)"); if (a[0].type != VAL_ARRAY || !a[0].as.arr->count) as_err("anim_from_images: expected a non-empty array of paths"); Opts o = opts_make(a, n, 1); RenderOpts r; ropts(&r, &o, T.colors != 0 || !T.open); int id = anim_new(opt_num(&o, "fps", 12));
    for (int i = 0; i < a[0].as.arr->count; i++) { KcValue p = a[0].as.arr->items[i]; if (p.type != VAL_STRING) as_err("anim_from_images: path %d is not a string", i); int iid = im_load(p.as.s->data, 0, 0); int w, h, cid = render_image_to_canvas(im_get(iid), &r, &w, &h); anim_add_canvas(id, cv_get(cid), 0); cv_free(cid); im_free(iid); }
    return kc_int(id);
}
NAT(n_anim_load) { BEGIN(1, 41, "anim_load(path, options...)  -- .gif, .kca, .txt/.ans frames, \"frame%04d.png\" sequences, video via ffmpeg (\"stream\", true)"); Opts o = opts_make(a, n, 1); return kc_int(anim_load_file(A_str(a, n, 0, "anim_load"), &o)); }
NAT(n_anim_free) { BEGIN(1, 1, "anim_free(id)"); anim_free(A_int(a, n, 0, "anim_free")); return OKV; }
NAT(n_anim_save) { BEGIN(2, 2, "anim_save(id, path.kca|path.txt)"); if (!anim_save_file(A_int(a, n, 0, "anim_save"), A_str(a, n, 1, "anim_save"))) as_err("anim_save: cannot write '%s'", A_str(a, n, 1, "anim_save")); return OKV; }
#define ACTL(nm, op, sig) NAT(nm) { BEGIN(1, 1, sig); anim_ctl(A_int(a, n, 0, op), op, 0); return OKV; }
ACTL(n_anim_play, "play", "anim_play(id)") ACTL(n_anim_pause, "pause", "anim_pause(id)") ACTL(n_anim_stop, "stop", "anim_stop(id)")
NAT(n_anim_seek) { BEGIN(2, 2, "anim_seek(id, frame)  (streams: seconds)"); anim_ctl(A_int(a, n, 0, "anim_seek"), "seek", A_num(a, n, 1, "anim_seek")); return OKV; }
NAT(n_anim_speed) { BEGIN(2, 2, "anim_speed(id, factor)"); anim_ctl(A_int(a, n, 0, "anim_speed"), "speed", A_num(a, n, 1, "anim_speed")); return OKV; }
NAT(n_anim_loop) { BEGIN(2, 2, "anim_loop(id, \"once\"|\"loop\"|\"pingpong\")"); const char *m = A_str(a, n, 1, "anim_loop"); int v = !strcmp(m, "once") ? 0 : !strcmp(m, "loop") ? 1 : !strcmp(m, "pingpong") ? 2 : -1; if (v < 0) as_err("anim_loop: use once, loop or pingpong"); anim_ctl(A_int(a, n, 0, "anim_loop"), "loop", v); return OKV; }
NAT(n_anim_info) {
    BEGIN(1, 1, "anim_info(id)"); int w, h, nf, cur; double fps; bool pl, fin, st; anim_info(A_int(a, n, 0, "anim_info"), &w, &h, &nf, &fps, &cur, &pl, &fin, &st); KcValue o = kc_object_new();
    #define S(k, v) kc_object_set(o, k, strlen(k), v)
    S("width", kc_int(w)); S("height", kc_int(h)); S("frames", kc_int(nf)); S("fps", kc_float(fps)); S("frame", kc_int(cur)); S("playing", kc_bool(pl)); S("finished", kc_bool(fin)); S("streaming", kc_bool(st));
    #undef S
    return o;
}
NAT(n_anim_draw) {
    BEGIN(3, 7, "anim_draw(id, x, y [, \"transparent\", true])"); Opts o = opts_make(a, n, 3); anim_draw_into(A_int(a, n, 0, "anim_draw"), cv_target(), A_int(a, n, 1, "anim_draw"), A_int(a, n, 2, "anim_draw"), opt_bool(&o, "transparent", false)); return OKV;
}
NAT(n_anim_frame) { BEGIN(2, 2, "anim_frame(id, index)  -> new canvas id holding that frame"); Canvas tmp; Canvas *f = anim_frame_canvas(A_int(a, n, 0, "anim_frame"), A_int(a, n, 1, "anim_frame"), &tmp); if (!f) as_err("anim_frame: no such frame"); int id = cv_new(f->w, f->h); memcpy(cv_get(id)->c, f->c, (size_t)f->w * f->h * sizeof(Cell)); return kc_int(id); }

/* ------------------------------------------------------------ effects / transitions */
static int64_t eff_last[512];
NAT(e_effect) { BEGIN(3, 20, "effect(name, w, h, options...)  names: plasma fire matrix starfield rain snow waves tunnel life noise"); Opts o = opts_make(a, n, 3); return kc_int(effect_new(A_str(a, n, 0, "effect"), A_int(a, n, 1, "effect"), A_int(a, n, 2, "effect"), &o)); }
NAT(e_effect_step) { BEGIN(2, 2, "effect_step(id, dt)"); int id = A_int(a, n, 0, "effect_step"); effect_step(id, A_num(a, n, 1, "effect_step")); if (id > 0 && id < 512) eff_last[id] = T.frame; return OKV; }
NAT(e_effect_draw) {
    BEGIN(3, 7, "effect_draw(id, x, y [, \"fg\", color])  -- advances automatically once per frame"); int id = A_int(a, n, 0, "effect_draw"); if (!effect_valid(id)) as_err("effect_draw: invalid effect %d", id); Opts o = opts_make(a, n, 3);
    if (id < 512 && eff_last[id] != T.frame) { eff_last[id] = T.frame; effect_step(id, T.dt); }
    effect_draw(id, cv_target(), A_int(a, n, 1, "effect_draw"), A_int(a, n, 2, "effect_draw"), opt_col(&o, "fg", 0)); return OKV;
}
NAT(e_effect_free) { BEGIN(1, 1, "effect_free(id)"); effect_free(A_int(a, n, 0, "effect_free")); return OKV; }
NAT(e_transition) {
    BEGIN(4, 6, "transition(kind, from_canvas, to_canvas, t [, x, y])  -- composites into the current target"); Canvas *ca = cv_must(A_int(a, n, 1, "transition"), "transition"), *cb = cv_must(A_int(a, n, 2, "transition"), "transition");
    transition_draw(A_str(a, n, 0, "transition"), ca, cb, cv_target(), (float)A_num(a, n, 3, "transition"), n > 4 ? A_int(a, n, 4, "transition") : 0, n > 5 ? A_int(a, n, 5, "transition") : 0); return OKV;
}

KcModuleEntry AS_T_MEDIA[] = {
    E("image", m_image), E("image_new", m_image_new), E("image_free", m_image_free), E("image_size", m_image_size), E("image_frames", m_image_frames), E("image_get", m_image_get), E("image_set", m_image_set), E("image_save", m_image_save),
    E("svg", m_svg), E("image_to_canvas", m_image_to_canvas), E("image_to_ascii", m_image_to_ascii), E("draw_image", m_draw_image), E("ramps", m_ramps),
    E("font_load", f_font_load), E("font_define", f_font_define), E("font_free", f_font_free), E("fonts", f_fonts), E("big_text", f_big_text), E("big_text_size", f_big_text_size), E("big_text_lines", f_big_text_lines),
    E("anim_new", n_anim_new), E("anim_add", n_anim_add), E("anim_from_text", n_anim_from_text), E("anim_from_images", n_anim_from_images), E("anim_load", n_anim_load), E("anim_free", n_anim_free), E("anim_save", n_anim_save),
    E("anim_play", n_anim_play), E("anim_pause", n_anim_pause), E("anim_stop", n_anim_stop), E("anim_seek", n_anim_seek), E("anim_speed", n_anim_speed), E("anim_loop", n_anim_loop), E("anim_info", n_anim_info), E("anim_draw", n_anim_draw), E("anim_frame", n_anim_frame),
    E("effect", e_effect), E("effect_step", e_effect_step), E("effect_draw", e_effect_draw), E("effect_free", e_effect_free), E("transition", e_transition),
};
const int AS_N_MEDIA = sizeof(AS_T_MEDIA) / sizeof(AS_T_MEDIA[0]);
