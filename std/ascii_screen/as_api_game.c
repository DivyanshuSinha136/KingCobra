/* std.ascii_screen — bindings: HUD widgets, sprites, tile maps / streamed worlds, entities, particles, camera, raycaster. */
#include "as_api.h"

/* ------------------------------------------------------------ HUD */
NAT(u_hud) { BEGIN(1, 41, "hud(kind, options...)  kinds: text panel bar list button input hotbar graph minimap spinner crosshair fps"); Opts o = opts_make(a, n, 1); term_ensure(); return kc_int(hud_create(A_str(a, n, 0, "hud"), &o)); }
NAT(u_hud_set) { BEGIN(3, 3, "hud_set(id, property, value)"); hud_set_prop(A_int(a, n, 0, "hud_set"), A_str(a, n, 1, "hud_set"), a[2]); return OKV; }
NAT(u_hud_get) { BEGIN(2, 2, "hud_get(id, property)"); return hud_get_prop(A_int(a, n, 0, "hud_get"), A_str(a, n, 1, "hud_get")); }
NAT(u_hud_remove) { BEGIN(1, 1, "hud_remove(id)"); hud_remove(A_int(a, n, 0, "hud_remove")); return OKV; }
NAT(u_hud_clear) { BEGIN(0, 0, "hud_clear()"); hud_reset(); return OKV; }
NAT(u_hud_tween) { BEGIN(4, 5, "hud_tween(id, property, target, seconds [, \"linear\"|\"in\"|\"out\"|\"inout\"])"); hud_tween(A_int(a, n, 0, "hud_tween"), A_str(a, n, 1, "hud_tween"), A_num(a, n, 2, "hud_tween"), A_num(a, n, 3, "hud_tween"), n > 4 ? A_str(a, n, 4, "hud_tween") : "linear"); return OKV; }
NAT(u_hud_push) { BEGIN(2, 2, "hud_push(graph_id, value)"); hud_push(A_int(a, n, 0, "hud_push"), A_num(a, n, 1, "hud_push")); return OKV; }
NAT(u_hud_focus) { BEGIN(0, 1, "hud_focus([id])"); if (n == 0) return kc_int(hud_focus_get()); hud_focus_set(A_int(a, n, 0, "hud_focus")); return OKV; }
NAT(u_hud_at) { BEGIN(2, 2, "hud_at(x, y)  -> element id under a cell (0 = none)"); return kc_int(hud_at(A_int(a, n, 0, "hud_at"), A_int(a, n, 1, "hud_at"))); }
NAT(u_hud_notify) { BEGIN(1, 5, "hud_notify(text [, \"fg\", color, \"seconds\", 3])"); Opts o = opts_make(a, n, 1); char *t = a[0].type == VAL_STRING ? NULL : kc_to_display_string(a[0]); hud_notify_text(t ? t : A_str(a, n, 0, "hud_notify"), opt_col(&o, "fg", 0), (float)opt_num(&o, "seconds", 3)); free(t); return OKV; }
NAT(u_hud_dialog) { BEGIN(2, 8, "hud_dialog(speaker, text [, choices_array] [, \"speed\", 45])"); int st = 2; KcValue ch = kc_null(); if (n > 2 && a[2].type == VAL_ARRAY) { ch = a[2]; st = 3; } Opts o = opts_make(a, n, st); hud_dialog_start(A_str(a, n, 0, "hud_dialog"), A_str(a, n, 1, "hud_dialog"), ch, opt_col(&o, "fg", 0), opt_num(&o, "speed", 45)); return OKV; }
NAT(u_dialog_active) { BEGIN(0, 0, "dialog_active()"); return kc_bool(hud_active_dialog()); }
NAT(u_dialog_result) { BEGIN(0, 0, "dialog_result()  -> chosen index, -1 until finished"); return kc_int(hud_dialog_result()); }
NAT(u_hud_debug) { BEGIN(0, 1, "hud_debug([on])"); hud_set_debug(n ? A_bool(a, n, 0, "hud_debug") : true); return OKV; }
NAT(u_hud_enable) { BEGIN(1, 1, "hud_enable(on)  -- turn widget input + drawing off/on"); hud_set_enabled(A_bool(a, n, 0, "hud_enable")); return OKV; }

/* ------------------------------------------------------------ sprites */
NAT(g_sprite) { BEGIN(1, 21, "sprite(text_or_rows, options...)  options: fg, bg, colors, palette, transparent"); Opts o = opts_make(a, n, 1); return kc_int(sprite_create(a[0], &o)); }
NAT(g_sprite_anim) { BEGIN(2, 22, "sprite_anim(frames_array, fps, options...)"); Opts o = opts_make(a, n, 2); return kc_int(sprite_create_anim(a[0], A_num(a, n, 1, "sprite_anim"), &o)); }
NAT(g_sprite_from_image) {
    BEGIN(1, 41, "sprite_from_image(image|path, options...)  -- colour sprite from a picture (transparent where the image is)"); int tmp; Image *im = img_arg(a, n, 0, &tmp, "sprite_from_image"); Opts o = opts_make(a, n, 1);
    RenderOpts r; render_opts_default(&r); r.color = 1; r.transparent = true; r.mode = "half"; render_opts_parse(&r, &o); int w, h, cid = render_image_to_canvas(im, &r, &w, &h); int id = sprite_from_canvas(cv_get(cid), 8); cv_free(cid); if (tmp) im_free(tmp); return kc_int(id);
}
NAT(g_sprite_free) { BEGIN(1, 1, "sprite_free(id)"); sprite_free(A_int(a, n, 0, "sprite_free")); return OKV; }
NAT(g_sprite_size) { BEGIN(1, 1, "sprite_size(id)  -> [w, h, frames]"); int w, h, f; sprite_size(A_int(a, n, 0, "sprite_size"), &w, &h, &f); KcValue r = kc_array_new(); kc_array_push(r, kc_int(w)); kc_array_push(r, kc_int(h)); kc_array_push(r, kc_int(f)); return r; }
NAT(g_sprite_anchor) { BEGIN(3, 3, "sprite_anchor(id, ax, ay)"); sprite_set_anchor(A_int(a, n, 0, "sprite_anchor"), A_int(a, n, 1, "sprite_anchor"), A_int(a, n, 2, "sprite_anchor")); return OKV; }
NAT(g_sprite_draw) { BEGIN(3, 13, "sprite_draw(id, x, y [, \"frame\", n, \"flip_x\", true, \"flip_y\", false, \"fg\", tint])"); Opts o = opts_make(a, n, 3); int id = A_int(a, n, 0, "sprite_draw"); if (!sprite_valid(id)) as_err("sprite_draw: invalid sprite %d", id);
    sprite_draw(id, cv_target(), A_int(a, n, 1, "sprite_draw"), A_int(a, n, 2, "sprite_draw"), opt_has(&o, "frame") ? opt_int(&o, "frame", 0) : -1, opt_bool(&o, "flip_x", false), opt_bool(&o, "flip_y", false), opt_col(&o, "fg", 0), 0); return OKV; }

/* ------------------------------------------------------------ tiles / maps */
NAT(g_tile_define) {
    BEGIN(2, 20, "tile_define(tile, glyph, options...)  options: fg, bg, solid, opaque, name"); int id = tile_id_of(a[0], "tile_define"); const char *g = A_str(a, n, 1, "tile_define"); const char *q = g; uint32_t ch = *g ? utf8_next(&q) : ' '; Opts o = opts_make(a, n, 2);
    tile_define(id, ch, opt_col(&o, "fg", 0), opt_col(&o, "bg", 0), opt_bool(&o, "solid", false), opt_bool(&o, "opaque", opt_bool(&o, "solid", false)), 0, opt_str(&o, "name", "")); return OKV;
}
NAT(g_map_rows) { BEGIN(1, 1, "map_rows(rows_array)  -- build a map from text; '@' marks the spawn point"); int id = map_from_rows(a[0]); return kc_int(id); }
NAT(g_map_new) { BEGIN(2, 3, "map_new(w, h [, fill_tile])"); return kc_int(map_new_fixed(A_int(a, n, 0, "map_new"), A_int(a, n, 1, "map_new"), n > 2 ? tile_id_of(a[2], "map_new") : 0)); }
NAT(g_map_generate) { BEGIN(2, 3, "map_generate(map, \"dungeon\"|\"cave\"|\"terrain\" [, seed])"); map_generate(A_int(a, n, 0, "map_generate"), A_str(a, n, 1, "map_generate"), n > 2 ? (uint32_t)A_int(a, n, 2, "map_generate") : as_rand_u32()); return OKV; }
NAT(g_world) { BEGIN(1, 7, "world(generator [, \"seed\", n, \"radius\", chunks])  generators: flat terrain caves noise"); Opts o = opts_make(a, n, 1); return kc_int(map_new_world(A_str(a, n, 0, "world"), (uint32_t)opt_int(&o, "seed", (int)as_rand_u32()), opt_int(&o, "radius", 3))); }
NAT(g_map_free) { BEGIN(1, 1, "map_free(id)"); map_free(A_int(a, n, 0, "map_free")); return OKV; }
NAT(g_map_use) { BEGIN(1, 1, "map_use(id)  -- the map used for collisions, streaming and draw_world()"); int id = A_int(a, n, 0, "map_use"); if (!map_valid(id)) as_err("map_use: invalid map %d", id); G_map = id; return OKV; }
NAT(g_tile_at) { BEGIN(3, 3, "tile_at(map, x, y)"); return kc_int(map_tile(A_int(a, n, 0, "tile_at"), A_int(a, n, 1, "tile_at"), A_int(a, n, 2, "tile_at"))); }
NAT(g_tile_set) { BEGIN(4, 4, "tile_set(map, x, y, tile)"); map_set(A_int(a, n, 0, "tile_set"), A_int(a, n, 1, "tile_set"), A_int(a, n, 2, "tile_set"), tile_id_of(a[3], "tile_set")); return OKV; }
NAT(g_tile_solid) { BEGIN(3, 3, "tile_solid(map, x, y)"); return kc_bool(map_solid(A_int(a, n, 0, "tile_solid"), A_int(a, n, 1, "tile_solid"), A_int(a, n, 2, "tile_solid"))); }
NAT(g_map_size) { BEGIN(1, 1, "map_size(map)  -> [w, h]  (null for infinite worlds)"); int w, h; if (!map_size(A_int(a, n, 0, "map_size"), &w, &h)) return kc_null(); KcValue r = kc_array_new(); kc_array_push(r, kc_int(w)); kc_array_push(r, kc_int(h)); return r; }
NAT(g_map_spawn) { BEGIN(1, 1, "map_spawn(map)  -> [x, y] or null"); int x, y; if (!map_spawn(A_int(a, n, 0, "map_spawn"), &x, &y)) return kc_null(); KcValue r = kc_array_new(); kc_array_push(r, kc_int(x)); kc_array_push(r, kc_int(y)); return r; }
NAT(g_map_fill) { BEGIN(6, 6, "map_fill(map, x, y, w, h, tile)"); map_fill_rect(A_int(a, n, 0, "map_fill"), A_int(a, n, 1, "map_fill"), A_int(a, n, 2, "map_fill"), A_int(a, n, 3, "map_fill"), A_int(a, n, 4, "map_fill"), tile_id_of(a[5], "map_fill")); return OKV; }
NAT(g_map_stats) { BEGIN(1, 1, "map_stats(map)  -> {loaded_chunks, edited_chunks, infinite}"); int l, m; bool inf; map_stats(A_int(a, n, 0, "map_stats"), &l, &m, &inf); KcValue o = kc_object_new(); kc_object_set(o, "loaded_chunks", 13, kc_int(l)); kc_object_set(o, "edited_chunks", 13, kc_int(m)); kc_object_set(o, "infinite", 8, kc_bool(inf)); return o; }
NAT(g_fov) { BEGIN(4, 4, "fov(map, x, y, radius)  -- compute visibility for draw_world(\"fov\", true)"); map_fov(A_int(a, n, 0, "fov"), A_num(a, n, 1, "fov"), A_num(a, n, 2, "fov"), A_int(a, n, 3, "fov")); return OKV; }
NAT(g_los) { BEGIN(5, 5, "los(map, x0, y0, x1, y1)"); return kc_bool(map_los(A_int(a, n, 0, "los"), A_int(a, n, 1, "los"), A_int(a, n, 2, "los"), A_int(a, n, 3, "los"), A_int(a, n, 4, "los"))); }
NAT(g_path) { BEGIN(5, 5, "path(map, x0, y0, x1, y1)  -> [[x,y], ...] (A*, 4-way)"); int *xy; int c = map_path(A_int(a, n, 0, "path"), A_int(a, n, 1, "path"), A_int(a, n, 2, "path"), A_int(a, n, 3, "path"), A_int(a, n, 4, "path"), &xy); KcValue r = kc_array_new();
    for (int i = 0; i < c; i++) { KcValue p = kc_array_new(); kc_array_push(p, kc_int(xy[i * 2])); kc_array_push(p, kc_int(xy[i * 2 + 1])); kc_array_push(r, p); } if (c) free(xy); return r; }
NAT(g_world_scale) { BEGIN(1, 1, "world_scale(n)  -- screen cells per tile horizontally (2 = square-looking tiles)"); int s = A_int(a, n, 0, "world_scale"); if (s < 1 || s > 4) as_err("world_scale: use 1..4"); G_xscale = s; return OKV; }
NAT(g_physics) { BEGIN(1, 2, "physics(gravity [, drag])  -- tiles/s^2; 0 = top-down"); game_physics(A_num(a, n, 0, "physics"), n > 1 ? A_num(a, n, 1, "physics") : 0); return OKV; }

/* ------------------------------------------------------------ entities / particles / camera */
NAT(g_spawn) { BEGIN(2, 40, "spawn(x, y, options...)  options: sprite glyph fg bg vx vy w h tag solid gravity bounce drag hp ttl z face billboard physics collide"); Opts o = opts_make(a, n, 2); return kc_int(ent_spawn(A_num(a, n, 0, "spawn"), A_num(a, n, 1, "spawn"), &o)); }
NAT(g_despawn) { BEGIN(1, 1, "despawn(id)"); ent_despawn(A_int(a, n, 0, "despawn")); return OKV; }
NAT(g_alive) { BEGIN(1, 1, "alive(id)"); return kc_bool(ent_alive(A_int(a, n, 0, "alive"))); }
NAT(g_set) { BEGIN(3, 3, "set(id, property, value)"); ent_set(A_int(a, n, 0, "set"), A_str(a, n, 1, "set"), a[2]); return OKV; }
NAT(g_get) { BEGIN(2, 2, "get(id, property)"); return ent_get(A_int(a, n, 0, "get"), A_str(a, n, 1, "get")); }
NAT(g_move) { BEGIN(3, 3, "move(id, dx, dy)  -> true if the whole step was free (tile + solid-entity collision)"); return kc_bool(ent_move(A_int(a, n, 0, "move"), A_num(a, n, 1, "move"), A_num(a, n, 2, "move"))); }
NAT(g_touching) { BEGIN(1, 2, "touching(id [, tag])  -> ids overlapping that entity"); return ent_touching(A_int(a, n, 0, "touching"), n > 1 ? A_str(a, n, 1, "touching") : NULL); }
NAT(g_entities_at) { BEGIN(2, 5, "entities_at(x, y [, w, h [, tag]])"); double w = 1, h = 1; const char *tag = NULL; if (n > 2) { w = A_num(a, n, 2, "entities_at"); h = n > 3 ? A_num(a, n, 3, "entities_at") : 1; } if (n > 4) tag = A_str(a, n, 4, "entities_at"); return ent_query(0, A_num(a, n, 0, "entities_at"), A_num(a, n, 1, "entities_at"), w, h, tag, 0); }
NAT(g_entities_near) { BEGIN(3, 4, "entities_near(x, y, radius [, tag])"); return ent_query(1, A_num(a, n, 0, "entities_near"), A_num(a, n, 1, "entities_near"), A_num(a, n, 2, "entities_near"), 0, n > 3 ? A_str(a, n, 3, "entities_near") : NULL, 0); }
NAT(g_entities) { BEGIN(0, 1, "entities([tag])"); return ent_query(2, 0, 0, 0, 0, n ? A_str(a, n, 0, "entities") : NULL, 0); }
NAT(g_clear_entities) { BEGIN(0, 0, "clear_entities()"); ent_clear(); return OKV; }
NAT(g_emit) { BEGIN(2, 30, "emit(x, y, options...)  options: count speed spread angle life gravity drag glyphs fg fg2 screen"); Opts o = opts_make(a, n, 2); emit_particles(A_num(a, n, 0, "emit"), A_num(a, n, 1, "emit"), &o); return OKV; }
NAT(g_camera) { BEGIN(0, 2, "camera(x, y)  |  camera() -> [x, y]"); if (n == 0) { double x, y; camera_get(&x, &y); KcValue r = kc_array_new(); kc_array_push(r, kc_float(x)); kc_array_push(r, kc_float(y)); return r; } camera_set(A_num(a, n, 0, "camera"), A_num(a, n, 1, "camera")); return OKV; }
NAT(g_camera_follow) { BEGIN(1, 7, "camera_follow(id_or_null [, \"smooth\", 8, \"deadzone_w\", 0, \"deadzone_h\", 0])"); Opts o = opts_make(a, n, 1); camera_follow(a[0].type == VAL_NULL ? 0 : A_int(a, n, 0, "camera_follow"), opt_num(&o, "smooth", 8), opt_num(&o, "deadzone_w", 0), opt_num(&o, "deadzone_h", 0)); return OKV; }
NAT(g_camera_shake) { BEGIN(2, 2, "camera_shake(amount, seconds)"); camera_shake(A_num(a, n, 0, "camera_shake"), A_num(a, n, 1, "camera_shake")); return OKV; }
NAT(g_camera_bounds) { BEGIN(1, 1, "camera_bounds(map_or_null)  -- keep the camera inside a fixed map"); camera_bounds(a[0].type == VAL_NULL ? 0 : A_int(a, n, 0, "camera_bounds")); return OKV; }
NAT(g_viewport) { BEGIN(0, 4, "viewport(x, y, w, h)  |  viewport() resets to the full screen"); if (n == 0) { viewport_set(0, 0, 0, 0); return OKV; } if (n != 4) as_err("usage: ascii_screen.viewport(x, y, w, h)"); viewport_set(A_int(a, n, 0, "viewport"), A_int(a, n, 1, "viewport"), A_int(a, n, 2, "viewport"), A_int(a, n, 3, "viewport")); return OKV; }
NAT(g_world_to_screen) { BEGIN(2, 2, "world_to_screen(x, y)"); double sx, sy; world_to_screen(A_num(a, n, 0, "world_to_screen"), A_num(a, n, 1, "world_to_screen"), &sx, &sy); KcValue r = kc_array_new(); kc_array_push(r, kc_int((int64_t)floor(sx + 0.5))); kc_array_push(r, kc_int((int64_t)floor(sy + 0.5))); return r; }
NAT(g_screen_to_world) { BEGIN(2, 2, "screen_to_world(x, y)"); double wx, wy; screen_to_world(A_num(a, n, 0, "screen_to_world"), A_num(a, n, 1, "screen_to_world"), &wx, &wy); KcValue r = kc_array_new(); kc_array_push(r, kc_float(wx)); kc_array_push(r, kc_float(wy)); return r; }
NAT(g_draw_world) { BEGIN(0, 20, "draw_world(options...)  options: map fog fov fog_color tiles entities particles"); Opts o = opts_make(a, n, 0); game_draw_world(cv_target(), &o); return OKV; }
NAT(g_raycast) {
    BEGIN(4, 5, "raycast(map, x, y, angle_radians [, max_dist])  -> {dist, tile, x, y, side} or null"); RayHit h; if (!map_raycast(A_int(a, n, 0, "raycast"), A_num(a, n, 1, "raycast"), A_num(a, n, 2, "raycast"), A_num(a, n, 3, "raycast"), n > 4 ? A_num(a, n, 4, "raycast") : 64, &h)) return kc_null();
    KcValue o = kc_object_new(); kc_object_set(o, "dist", 4, kc_float(h.dist)); kc_object_set(o, "tile", 4, kc_int(h.tile)); kc_object_set(o, "x", 1, kc_float(h.hx)); kc_object_set(o, "y", 1, kc_float(h.hy)); kc_object_set(o, "side", 4, kc_int(h.side)); return o;
}
NAT(g_raycast_view) {
    BEGIN(4, 24, "raycast_view(map, x, y, angle_radians, options...)  -- first-person ASCII 3D. options: fov x y w h max_dist fog ramp color ceiling floor sprites"); Opts o = opts_make(a, n, 4); int vx, vy, vw, vh; viewport_get(&vx, &vy, &vw, &vh);
    raycast_view(A_int(a, n, 0, "raycast_view"), cv_target(), A_num(a, n, 1, "raycast_view"), A_num(a, n, 2, "raycast_view"), A_num(a, n, 3, "raycast_view"), opt_num(&o, "fov", 66), opt_int(&o, "x", vx), opt_int(&o, "y", vy), opt_int(&o, "w", vw), opt_int(&o, "h", vh), &o); return OKV;
}

KcModuleEntry AS_T_GAME[] = {
    E("hud", u_hud), E("hud_set", u_hud_set), E("hud_get", u_hud_get), E("hud_remove", u_hud_remove), E("hud_clear", u_hud_clear), E("hud_tween", u_hud_tween), E("hud_push", u_hud_push), E("hud_focus", u_hud_focus), E("hud_at", u_hud_at),
    E("hud_notify", u_hud_notify), E("hud_dialog", u_hud_dialog), E("dialog_active", u_dialog_active), E("dialog_result", u_dialog_result), E("hud_debug", u_hud_debug), E("hud_enable", u_hud_enable),
    E("sprite", g_sprite), E("sprite_anim", g_sprite_anim), E("sprite_from_image", g_sprite_from_image), E("sprite_free", g_sprite_free), E("sprite_size", g_sprite_size), E("sprite_anchor", g_sprite_anchor), E("sprite_draw", g_sprite_draw),
    E("tile_define", g_tile_define), E("map_rows", g_map_rows), E("map_new", g_map_new), E("map_generate", g_map_generate), E("world", g_world), E("map_free", g_map_free), E("map_use", g_map_use), E("tile_at", g_tile_at), E("tile_set", g_tile_set),
    E("tile_solid", g_tile_solid), E("map_size", g_map_size), E("map_spawn", g_map_spawn), E("map_fill", g_map_fill), E("map_stats", g_map_stats), E("fov", g_fov), E("los", g_los), E("path", g_path), E("world_scale", g_world_scale), E("physics", g_physics),
    E("spawn", g_spawn), E("despawn", g_despawn), E("alive", g_alive), E("set", g_set), E("get", g_get), E("move", g_move), E("touching", g_touching), E("entities_at", g_entities_at), E("entities_near", g_entities_near), E("entities", g_entities),
    E("clear_entities", g_clear_entities), E("emit", g_emit), E("camera", g_camera), E("camera_follow", g_camera_follow), E("camera_shake", g_camera_shake), E("camera_bounds", g_camera_bounds), E("viewport", g_viewport),
    E("world_to_screen", g_world_to_screen), E("screen_to_world", g_screen_to_world), E("draw_world", g_draw_world), E("raycast", g_raycast), E("raycast_view", g_raycast_view),
};
const int AS_N_GAME = sizeof(AS_T_GAME) / sizeof(AS_T_GAME[0]);
