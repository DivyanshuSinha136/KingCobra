/* Lexical environments (scopes).
 *
 * Performance model (v0.5.1):
 *   - Every identifier is INTERNED (kc_intern): one canonical char* per
 *     distinct name, so a lookup is a pointer compare, never a strcmp.
 *   - Scopes come from a free list and carry their first KC_ENV_INLINE
 *     entries inline: creating a scope is a pop, not two mallocs.
 *   - A scope is recycled when its owner finishes with it, unless something
 *     captured it (a closure, or a `&variable` pointer). Capture is tracked
 *     dynamically with the `captured` flag, which makes recycling exact.
 */
#ifndef KC_ENV_H
#define KC_ENV_H
#include "value.h"

#define KC_ENV_INLINE 6

typedef struct { const char *name; KcValue value; bool is_mutable; } KcEnvEntry;

struct KcEnv {
    KcEnv *parent;
    KcEnvEntry *entries;       /* == inl until the scope outgrows it */
    int count, cap;
    int32_t *hidx;             /* big scopes only (builtins, large globals): open-addressed index, slot+1 per name */
    uint32_t hmask;
    bool captured;             /* a closure or &pointer references this scope */
    bool is_statics;           /* holds a function's `static` locals (v0.8) */
    KcEnv *next_free;          /* free-list link while pooled */
    KcEnvEntry inl[KC_ENV_INLINE];
};

const char *kc_intern(const char *s);  /* canonical, never freed */

KcEnv *kc_env_new(KcEnv *parent);
void kc_env_release(KcEnv *env);       /* recycle; caller guarantees !captured */
void kc_env_capture(KcEnv *env);       /* mark env + ancestors as referenced */
void kc_env_grow(KcEnv *env);
void kc_env_index_add(KcEnv *env);               /* maintain the hash index of a scope with many names */
KcEnvEntry *kc_env_hash_find(KcEnv *env, const char *iname);
#define KC_ENV_HASH_MIN 16                       /* scopes up to this size are scanned linearly: faster */

/* Name-taking API (interns the name; fine off the hot path). */
void kc_env_define(KcEnv *env, const char *name, KcValue v, bool is_mutable);
bool kc_env_get(KcEnv *env, const char *name, KcValue *out);
bool kc_env_set(KcEnv *env, const char *name, KcValue v);
/* Locates the scope + slot index a name resolves to (for `&name`). Slot
 * indices are stable: entries are only ever appended, never removed. */
bool kc_env_find(KcEnv *env, const char *name, KcEnv **env_out, int *idx_out);

/* Hot-path API: `iname` MUST already be interned. */
static inline void kc_env_define_i(KcEnv *env, const char *iname, KcValue v, bool is_mutable) {
    if (__builtin_expect(env->count == env->cap, 0)) kc_env_grow(env);
    KcEnvEntry *e = &env->entries[env->count++];
    e->name = iname; e->value = v; e->is_mutable = is_mutable;
    if (__builtin_expect(env->count > KC_ENV_HASH_MIN, 0)) kc_env_index_add(env);
}

static inline KcEnvEntry *kc_env_lookup_i(KcEnv *env, const char *iname) {
    for (KcEnv *e = env; e; e = e->parent) {
        int cnt = e->count;                                            /* needed by the scan anyway */
        if (__builtin_expect(cnt > KC_ENV_HASH_MIN, 0)) {              /* large scope (has an index): one hashed probe */
            KcEnvEntry *r = kc_env_hash_find(e, iname);
            if (r) return r;
            continue;
        }
        KcEnvEntry *ents = e->entries;
        for (int i = cnt - 1; i >= 0; i--)
            if (ents[i].name == iname) return &ents[i];
    }
    return NULL;
}

#endif
