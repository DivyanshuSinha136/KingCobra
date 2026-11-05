#include "env.h"

/* ---- name interning: open-addressing hash set, FNV-1a ---- */

static const char **g_tab = NULL;
static size_t g_cap = 0, g_n = 0;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void tab_insert(const char *s) {
    size_t i = (size_t)fnv1a(s) & (g_cap - 1);
    while (g_tab[i]) i = (i + 1) & (g_cap - 1);
    g_tab[i] = s;
}

const char *kc_intern(const char *s) {
    if (g_n * 2 >= g_cap) {
        size_t ocap = g_cap; const char **old = g_tab;
        g_cap = g_cap ? g_cap * 2 : 1024;
        g_tab = kc_malloc(sizeof(char *) * g_cap);
        memset(g_tab, 0, sizeof(char *) * g_cap);
        for (size_t i = 0; i < ocap; i++) if (old[i]) tab_insert(old[i]);
        free(old);
    }
    size_t i = (size_t)fnv1a(s) & (g_cap - 1);
    while (g_tab[i]) {
        if (strcmp(g_tab[i], s) == 0) return g_tab[i];
        i = (i + 1) & (g_cap - 1);
    }
    const char *c = kc_strdup(s);
    g_tab[i] = c; g_n++;
    return c;
}

/* ---- scope pool ---- */

static KcEnv *g_pool = NULL;

/* Debug switch: -DKC_ENV_NOPOOL makes every scope a real malloc/free so
 * AddressSanitizer can catch any use of a scope after it was recycled. */
KcEnv *kc_env_new(KcEnv *parent) {
    KcEnv *e;
#ifdef KC_ENV_NOPOOL
    e = kc_malloc(sizeof(KcEnv));
#else
    e = g_pool;
    if (e) g_pool = e->next_free;
    else e = kc_malloc(sizeof(KcEnv));
#endif
    e->parent = parent; e->entries = e->inl;
    e->count = 0; e->cap = KC_ENV_INLINE;
    e->hidx = NULL; e->hmask = 0;
    e->captured = false; e->is_statics = false; e->next_free = NULL;
    return e;
}

void kc_env_release(KcEnv *e) {
    if (e->entries != e->inl) free(e->entries);
    if (__builtin_expect(e->hidx != NULL, 0)) { free(e->hidx); e->hidx = NULL; e->hmask = 0; }   /* only big scopes have one */
#ifdef KC_ENV_NOPOOL
    free(e);
    return;
#endif
    e->entries = e->inl; e->count = 0; e->cap = KC_ENV_INLINE;
    e->parent = NULL;
    e->next_free = g_pool; g_pool = e;
}

void kc_env_capture(KcEnv *e) {
    for (; e && !e->captured; e = e->parent) e->captured = true;
}

void kc_env_grow(KcEnv *env) {
    int ncap = env->cap * 2;
    KcEnvEntry *n = kc_malloc(sizeof(KcEnvEntry) * (size_t)ncap);
    memcpy(n, env->entries, sizeof(KcEnvEntry) * (size_t)env->count);
    if (env->entries != env->inl) free(env->entries);
    env->entries = n; env->cap = ncap;
}

/* ---- hash index for large scopes ----
 * Names are interned, so the key is the pointer itself. The table maps a name to the slot of its
 * NEWEST entry (a later `let` of the same name shadows an earlier one, exactly as the backward
 * linear scan does). Open addressing with linear probing, kept under 50% full. */

static inline uint32_t hash_ptr(const char *p, uint32_t mask) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h = (h >> 4) * 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(h >> 32) & mask;
}

static void index_put(KcEnv *e, int slot) {
    const char *name = e->entries[slot].name;
    uint32_t i = hash_ptr(name, e->hmask);
    while (e->hidx[i] && e->entries[e->hidx[i] - 1].name != name) i = (i + 1) & e->hmask;
    e->hidx[i] = slot + 1;                              /* a later entry of the same name replaces the earlier */
}

static void index_rebuild(KcEnv *e) {
    uint32_t size = 64;
    while (size < (uint32_t)e->count * 2 + 16) size <<= 1;
    free(e->hidx);
    e->hidx = kc_malloc(sizeof(int32_t) * size);
    memset(e->hidx, 0, sizeof(int32_t) * size);
    e->hmask = size - 1;
    for (int i = 0; i < e->count; i++) index_put(e, i);
}

void kc_env_index_add(KcEnv *e) {
    if (!e->hidx || (uint32_t)e->count * 2 > e->hmask) index_rebuild(e);   /* first time over the threshold, or table too full */
    else index_put(e, e->count - 1);
}

KcEnvEntry *kc_env_hash_find(KcEnv *e, const char *iname) {
    uint32_t i = hash_ptr(iname, e->hmask);
    while (e->hidx[i]) {
        KcEnvEntry *en = &e->entries[e->hidx[i] - 1];
        if (en->name == iname) return en;
        i = (i + 1) & e->hmask;
    }
    return NULL;
}

/* ---- name-taking API ---- */

void kc_env_define(KcEnv *env, const char *name, KcValue v, bool is_mutable) {
    kc_env_define_i(env, kc_intern(name), v, is_mutable);
}

bool kc_env_get(KcEnv *env, const char *name, KcValue *out) {
    KcEnvEntry *e = kc_env_lookup_i(env, kc_intern(name));
    if (!e) return false;
    *out = e->value; return true;
}

bool kc_env_set(KcEnv *env, const char *name, KcValue v) {
    KcEnvEntry *e = kc_env_lookup_i(env, kc_intern(name));
    if (!e) return false;
    e->value = v; return true;
}

bool kc_env_find(KcEnv *env, const char *name, KcEnv **env_out, int *idx_out) {
    const char *iname = kc_intern(name);
    for (KcEnv *e = env; e; e = e->parent) {
        if (e->hidx) {
            KcEnvEntry *r = kc_env_hash_find(e, iname);
            if (r) { *env_out = e; *idx_out = (int)(r - e->entries); return true; }
            continue;
        }
        for (int i = e->count - 1; i >= 0; i--)
            if (e->entries[i].name == iname) { *env_out = e; *idx_out = i; return true; }
    }
    return false;
}
