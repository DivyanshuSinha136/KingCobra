/* KingCobra std.mmu — script-facing natives, part 1: glue, address helpers, physical memory,
 * address spaces / page tables, CPUs / TLB, and virtual-address data access.
 *
 * Error model (same split as std.mmio):
 *   - programming errors (wrong types, bad arguments, use after destroy) are fatal;
 *   - expected simulation outcomes are values: a page fault makes an access return null/false and
 *     sets mmu.last_fault(); a failed operation (no free frame, address already mapped, ...) returns
 *     null/false and sets mmu.last_error(). `strict` mode turns unhandled faults into fatal errors.
 *
 * Addresses are 64-bit and the language's ints are signed: x86-64 / arm64 high-half addresses show
 * up negative. Every address argument also accepts a string ("0xFFFF800000000000") and mmu.hex()
 * prints any value as unsigned.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"
#include <ctype.h>
#include <errno.h>

/* ------------------------------------------------------------------ errors & args */

static char g_err[320];

void mm_set_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap);
}
const char *mm_last_error_str(void) { return g_err; }

void mm_need(int n, int lo, int hi, KcPos pos, const char *usage) {
    if (n < lo || n > hi) kc_error_at(pos, "%s", usage);
}

uint64_t mm_arg_addr(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i >= n) kc_error_at(pos, "%s: missing address argument", who);
    if (a[i].type == VAL_INT) return (uint64_t)a[i].as.i;
    if (a[i].type == VAL_STRING) {
        const char *s = a[i].as.s->data; char *end; errno = 0;
        bool neg = *s == '-';
        unsigned long long v = strtoull(neg ? s + 1 : s, &end, 0);
        if (end == (neg ? s + 1 : s) || *end || errno) kc_error_at(pos, "%s: '%s' is not a valid address", who, s);
        return neg ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
    }
    kc_error_at(pos, "%s: address must be an int or a string like \"0x...\" (got %s)", who, kc_type_name(a[i]));
    return 0;
}

int64_t mm_arg_int(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i >= n || a[i].type != VAL_INT) kc_error_at(pos, "%s: argument %d must be an int", who, i + 1);
    return a[i].as.i;
}

uint64_t mm_arg_size(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i >= n) kc_error_at(pos, "%s: missing size argument", who);
    if (a[i].type == VAL_INT) { if (a[i].as.i < 0) kc_error_at(pos, "%s: size must not be negative", who); return (uint64_t)a[i].as.i; }
    if (a[i].type == VAL_STRING) {
        const char *s = a[i].as.s->data; char *end; errno = 0;
        unsigned long long v = strtoull(s, &end, 0);
        if (end == s || errno) kc_error_at(pos, "%s: '%s' is not a valid size", who, s);
        while (*end == ' ') end++;
        unsigned sh = 0;
        if (*end) {
            char c = (char)toupper((unsigned char)*end);
            sh = c == 'K' ? 10 : c == 'M' ? 20 : c == 'G' ? 30 : c == 'T' ? 40 : 99;
            const char *r = end + 1; if (*r == 'i' || *r == 'I') r++; if (*r == 'b' || *r == 'B') r++;
            if (sh == 99 || *r) { if (!(c == 'B' && !end[1])) kc_error_at(pos, "%s: '%s' is not a valid size (use e.g. 4096, \"64M\", \"2GiB\")", who, s); sh = 0; }
        }
        if (sh && v > (UINT64_MAX >> sh)) kc_error_at(pos, "%s: size overflows", who);
        return (uint64_t)v << sh;
    }
    kc_error_at(pos, "%s: size must be an int or a string like \"64M\"", who);
    return 0;
}

const char *mm_arg_str(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i >= n || a[i].type != VAL_STRING) kc_error_at(pos, "%s: argument %d must be a string", who, i + 1);
    return a[i].as.s->data;
}

KcValue mm_str_bytes(const void *data, size_t len) {
    char *b = kc_malloc(len + 1);
    if (len) memcpy(b, data, len);
    b[len] = 0;
    return kc_string_take(b, (int)len);
}

KcValue mm_hex_value(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%016llx", (unsigned long long)v); return kc_string(b); }

static void oset(KcValue o, const char *k, KcValue v) { kc_object_set(o, k, strlen(k), v); }
#define OI(o, k, v)  oset(o, k, kc_int((int64_t)(v)))
#define OB(o, k, v)  oset(o, k, kc_bool(v))
#define OS(o, k, v)  oset(o, k, kc_string(v))

/* ------------------------------------------------------------------ handles */

KcValue mmuh_wrap(int sub, void *p) {
    KcHandle *h = kc_malloc(sizeof *h);
    memset(h, 0, sizeof *h);
    h->kind = H_MMU; h->ptr = p; h->extra = sub;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

void *mmuh_get(KcValue v, int sub) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_MMU || v.as.handle->extra != sub) return NULL;
    return v.as.handle->ptr;
}

static const char *sub_name(int s) { return s == MH_PHYS ? "physical memory" : s == MH_SPACE ? "address space" : s == MH_CPU ? "cpu" : s == MH_SHM ? "shared memory object" : "host region"; }

static void *need_h(KcValue *a, int n, int i, int sub, KcPos pos, const char *who) {
    if (i >= n || a[i].type != VAL_HANDLE || a[i].as.handle->kind != H_MMU || a[i].as.handle->extra != sub)
        kc_error_at(pos, "%s: argument %d must be an mmu %s handle", who, i + 1, sub_name(sub));
    if (!a[i].as.handle->ptr) kc_error_at(pos, "%s: the %s was already destroyed", who, sub_name(sub));
    return a[i].as.handle->ptr;
}
#define NEED_PHYS(i)  ((MmuPhys *)need_h(a, n, i, MH_PHYS, pos, who))
#define NEED_SPACE(i) ((MmuSpace *)need_h(a, n, i, MH_SPACE, pos, who))

static MmuCpu *need_cpu(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i < n && a[i].type == VAL_HANDLE && a[i].as.handle->kind == H_MMU && a[i].as.handle->extra == MH_SPACE) {
        MmuSpace *S = need_h(a, n, i, MH_SPACE, pos, who);
        return S->home;
    }
    return need_h(a, n, i, MH_CPU, pos, who);
}

static MmuCpu *need_loaded_cpu(KcValue *a, int n, int i, KcPos pos, const char *who) {
    MmuCpu *c = need_cpu(a, n, i, pos, who);
    if (!c->cur) kc_error_at(pos, "%s: the cpu has no address space loaded (use mmu.switch(cpu, space))", who);
    return c;
}

char *kc_mmu_display(KcHandle *h) {
    char b[160];
    if (!h->ptr) { snprintf(b, sizeof b, "<mmu %s destroyed>", sub_name((int)h->extra)); return kc_strdup(b); }
    switch (h->extra) {
    case MH_PHYS: { MmuPhys *p = h->ptr; snprintf(b, sizeof b, "<mmu phys '%s' %llu MiB, %llu frames in use>", p->name, (unsigned long long)(p->size >> 20), (unsigned long long)p->used_frames); break; }
    case MH_SPACE: { MmuSpace *s = h->ptr; snprintf(b, sizeof b, "<mmu space '%s' %s asid=%u>", s->name, s->A->name, s->asid); break; }
    case MH_CPU: { MmuCpu *c = h->ptr; snprintf(b, sizeof b, "<mmu cpu '%s' %s%s%s>", c->name, c->A->name, c->cur ? " space=" : " (no space)", c->cur ? c->cur->name : ""); break; }
    case MH_SHM: { MmuShm *s = h->ptr; snprintf(b, sizeof b, "<mmu shm '%s' %llu KiB>", s->name, (unsigned long long)(s->len >> 10)); break; }
    default: snprintf(b, sizeof b, "<mmu host region %llu KiB>", (unsigned long long)(h->size >> 10)); break;
    }
    return kc_strdup(b);
}

/* ------------------------------------------------------------------ argument parsing */

static Access parse_access(KcValue *a, int n, int i, KcPos pos, const char *who) {
    const char *s = mm_arg_str(a, n, i, pos, who);
    if (!strcmp(s, "r") || !strcmp(s, "read") || !strcmp(s, "load")) return ACC_READ;
    if (!strcmp(s, "w") || !strcmp(s, "write") || !strcmp(s, "store")) return ACC_WRITE;
    if (!strcmp(s, "x") || !strcmp(s, "exec") || !strcmp(s, "execute") || !strcmp(s, "fetch")) return ACC_EXEC;
    kc_error_at(pos, "%s: access must be \"r\", \"w\" or \"x\" (got '%s')", who, s);
    return ACC_READ;
}

static bool parse_mode(KcValue *a, int n, int i, KcPos pos, const char *who) {
    if (i >= n) return false;
    const char *s = mm_arg_str(a, n, i, pos, who);
    if (!strcmp(s, "u") || !strcmp(s, "user")) return true;
    if (!strcmp(s, "s") || !strcmp(s, "supervisor") || !strcmp(s, "k") || !strcmp(s, "kernel")) return false;
    kc_error_at(pos, "%s: mode must be \"u\" (user) or \"s\" (supervisor) (got '%s')", who, s);
    return false;
}


static int prot_bit(const char *t) { return !strcmp(t, "r") ? PROT_R : !strcmp(t, "w") ? PROT_W : !strcmp(t, "x") ? PROT_X : !strcmp(t, "u") ? PROT_U : !strcmp(t, "g") ? PROT_G : 0; }
static int cache_by_name(const char *t) { for (int i = 0; i < MC_COUNT; i++) if (!strcmp(MMU_CACHE_NAMES[i], t)) return i; return -1; }

static void parse_flags(const char *s, MFlags *F, KcPos pos, const char *who) {
    memset(F, 0, sizeof *F);
    char buf[200]; snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ", |"); t; t = strtok(NULL, ", |")) {
        char sign = (*t == '+' || *t == '-') ? *t : 0;
        const char *k = sign ? t + 1 : t;
        int pb = prot_bit(k), cn = cache_by_name(k);
        if (sign) {
            F->rel = true;
            if (pb) { if (sign == '+') F->add |= (uint32_t)pb; else F->del |= (uint32_t)pb; }
            else kc_error_at(pos, "%s: only r, w, x, u, g may carry a +/- prefix (got '%s')", who, t);
            continue;
        }
        if (pb) F->prot |= (uint32_t)pb;
        else if (cn >= 0) { F->cache = cn; F->cache_set = true; }
        else if (!strcmp(k, "a") || !strcmp(k, "accessed")) F->a = true;
        else if (!strcmp(k, "d") || !strcmp(k, "dirty")) F->d = true;
        else if (!strcmp(k, "cow")) F->cow = true;
        else if (!strcmp(k, "own")) F->own = true;
        else if (!strcmp(k, "force") || !strcmp(k, "replace")) F->force = true;
        else if (!strcmp(k, "huge")) F->huge = true;
        else if (!strcmp(k, "fixed")) F->fixed = true;
        else if (!strcmp(k, "noreplace") || !strcmp(k, "fixed_noreplace")) { F->fixed = true; F->noreplace = true; }
        else if (!strcmp(k, "populate")) F->populate = true;
        else if (!strcmp(k, "growsdown") || !strcmp(k, "stack")) F->growsdown = true;
        else if (!strcmp(k, "locked") || !strcmp(k, "lock")) F->locked = true;
        else if (!strcmp(k, "guard")) F->guard = true;
        else if (!strcmp(k, "none")) F->none = true;
        else if (!strcmp(k, "shared")) F->shared = true;
        else kc_error_at(pos, "%s: unknown flag '%s' (valid: r w x u g, wb wc nc uc wt wp, a d cow own force huge fixed noreplace populate growsdown locked guard none shared; +/-r etc. for protect)", who, t);
    }
    if (F->none) F->prot = 0;
}

static int level_for_size(const MmuArch *A, uint64_t size, KcPos pos, const char *who) {
    for (int L = 0; L < A->levels; L++) if (mmu_page_size(A, L) == size && (L == 0 || (A->leaf_mask & (1u << L)))) return L;
    char list[160] = ""; size_t o = 0;
    for (int L = 0; L < A->levels; L++) if (L == 0 || (A->leaf_mask & (1u << L))) o += (size_t)snprintf(list + o, sizeof list - o, "%s%llu", o ? ", " : "", (unsigned long long)mmu_page_size(A, L));
    kc_error_at(pos, "%s: %s has no %llu-byte pages (supported: %s)", who, A->name, (unsigned long long)size, list);
    return 0;
}

static void flags_to_pte(const MmuArch *A, const MFlags *F, Pte *p, KcPos pos, const char *who) {
    memset(p, 0, sizeof *p);
    p->kind = PK_LEAF;
    uint32_t pr = F->prot ? F->prot : (F->none ? 0 : PROT_R);
    p->r = (pr & (PROT_R | PROT_W)) != 0; p->w = (pr & PROT_W) != 0; p->x = (pr & PROT_X) != 0;
    p->user = (pr & PROT_U) != 0; p->global = (pr & PROT_G) != 0;
    p->cache = (uint8_t)(F->cache_set ? F->cache : MC_WB);
    p->accessed = F->a; p->dirty = F->d;
    p->sw = (uint8_t)((F->cow ? SW_COW : 0) | (F->own ? SW_OWN : 0));
    if (!mmu_cache_supported(A, p->cache)) kc_error_at(pos, "%s: cache type '%s' is not available on %s", who, MMU_CACHE_NAMES[p->cache], A->name);
}

static MmuArch const *need_arch(KcValue *a, int n, int i, KcPos pos, const char *who) {
    const char *s = mm_arg_str(a, n, i, pos, who);
    const MmuArch *A = mmu_arch_by_name(s);
    if (!A) {
        char list[300] = ""; size_t o = 0;
        for (int k = 0; k < MMU_ARCH_COUNT; k++) o += (size_t)snprintf(list + o, sizeof list - o, "%s%s", k ? ", " : "", MMU_ARCHS[k].name);
        kc_error_at(pos, "%s: unknown architecture '%s' (valid: %s)", who, s, list);
    }
    return A;
}

/* ------------------------------------------------------------------ fault & info objects */

KcValue mm_fault_object(const MmuArch *A, const Fault *f) {
    KcValue o = kc_object_new();
    char d[256]; mmu_fault_describe(A, f, d, sizeof d);
    OB(o, "ok", false);
    OS(o, "kind", MMU_FAULT_NAMES[f->kind]);
    OI(o, "va", f->va);
    oset(o, "va_hex", mm_hex_value(f->va));
    OS(o, "access", f->access == ACC_READ ? "r" : f->access == ACC_WRITE ? "w" : "x");
    OS(o, "mode", f->user ? "user" : "supervisor");
    OI(o, "level", f->level);
    OI(o, "code", f->code);
    OS(o, "vector", f->vector ? f->vector : "");
    OI(o, "raw", f->raw);
    OB(o, "swapped", f->swapped);
    OB(o, "cow", f->cow);
    OS(o, "segv", f->segv == 1 ? "maperr" : f->segv == 2 ? "accerr" : "none");
    OS(o, "message", d);
    return o;
}

static KcValue page_object(const MmuArch *A, const Pte *p, int level, uint64_t va_base, uint64_t raw) {
    KcValue o = kc_object_new();
    char fl[64]; mmu_flags_str(fl, sizeof fl, p);
    OI(o, "va", va_base); OI(o, "pa", p->pa); OI(o, "size", mmu_page_size(A, level)); OI(o, "level", level);
    OS(o, "flags", fl);
    OB(o, "r", p->r); OB(o, "w", p->w); OB(o, "x", p->x); OB(o, "user", p->user); OB(o, "global", p->global);
    OB(o, "accessed", p->accessed); OB(o, "dirty", p->dirty);
    OS(o, "cache", MMU_CACHE_NAMES[p->cache]);
    OB(o, "cow", (p->sw & SW_COW) != 0); OB(o, "own", (p->sw & SW_OWN) != 0);
    OI(o, "raw", raw);
    return o;
}

static const char *kind_name(PteKind k) { return k == PK_INVALID ? "invalid" : k == PK_TABLE ? "table" : k == PK_LEAF ? "leaf" : k == PK_BAD ? "bad" : "special"; }

/* ------------------------------------------------------------------ misc helpers */

static KcValue f_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; (void)a; mm_need(n, 0, 0, pos, "mmu.last_error() -> string"); return kc_string(g_err); }

static KcValue f_archs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; mm_need(n, 0, 0, pos, "mmu.archs() -> [names]");
    KcValue r = kc_array_new();
    for (int i = 0; i < MMU_ARCH_COUNT; i++) kc_array_push(r, kc_string(MMU_ARCHS[i].name));
    return r;
}

static KcValue f_arch_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.arch_info"; mm_need(n, 1, 1, pos, "mmu.arch_info(arch) -> object");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    KcValue o = kc_object_new();
    OS(o, "name", A->name); OS(o, "family", A->family_name); OI(o, "levels", A->levels); OI(o, "va_bits", A->va_bits);
    OI(o, "pa_bits", A->pa_bits); OI(o, "pte_size", A->pte_size); OI(o, "index_bits", A->idx_bits); OI(o, "table_size", 4096);
    OI(o, "asid_bits", A->asid_bits); OS(o, "root_register", A->root_reg); OB(o, "has_nx", A->has_nx); OB(o, "split_va", A->split_va);
    OB(o, "canonical_hole", A->pte_size == 8);
    OI(o, "user_top", mmu_user_top(A));
    KcValue ps = kc_array_new();
    for (int L = 0; L < A->levels; L++) if (L == 0 || (A->leaf_mask & (1u << L))) kc_array_push(ps, kc_int((int64_t)mmu_page_size(A, L)));
    oset(o, "page_sizes", ps);
    KcValue ct = kc_array_new();
    for (int c = 0; c < MC_COUNT; c++) if (mmu_cache_supported(A, c)) kc_array_push(ct, kc_string(MMU_CACHE_NAMES[c]));
    oset(o, "cache_types", ct);
    return o;
}

static KcValue f_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 1, 2, pos, "mmu.hex(value [, digits]) -> string");
    uint64_t v = mm_arg_addr(a, n, 0, pos, "mmu.hex");
    int w = n == 2 ? (int)mm_arg_int(a, n, 1, pos, "mmu.hex") : 16;
    if (w < 1 || w > 16) MM_ERR("mmu.hex: digits must be 1..16");
    char b[32]; snprintf(b, sizeof b, "0x%0*llx", w, (unsigned long long)v);
    return kc_string(b);
}
static KcValue f_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 1, 1, pos, "mmu.parse(\"0x...\") -> int");
    return kc_int((int64_t)mm_arg_addr(a, n, 0, pos, "mmu.parse"));
}
static KcValue f_ucmp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.ucmp(a, b) -> -1|0|1 (unsigned compare)");
    uint64_t x = mm_arg_addr(a, n, 0, pos, "mmu.ucmp"), y = mm_arg_addr(a, n, 1, pos, "mmu.ucmp");
    return kc_int(x < y ? -1 : x > y ? 1 : 0);
}
static uint64_t pow2_arg(KcValue *a, int n, int i, KcPos pos, const char *who) {
    uint64_t al = mm_arg_addr(a, n, i, pos, who);
    if (al == 0 || (al & (al - 1))) kc_error_at(pos, "%s: alignment must be a power of two", who);
    return al;
}
static KcValue f_align_down(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.align_down(value, alignment) -> int");
    return kc_int((int64_t)(mm_arg_addr(a, n, 0, pos, "mmu.align_down") & ~(pow2_arg(a, n, 1, pos, "mmu.align_down") - 1)));
}
static KcValue f_align_up(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.align_up(value, alignment) -> int");
    uint64_t al = pow2_arg(a, n, 1, pos, "mmu.align_up"), v = mm_arg_addr(a, n, 0, pos, "mmu.align_up");
    return kc_int((int64_t)((v + al - 1) & ~(al - 1)));
}
static KcValue f_is_aligned(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.is_aligned(value, alignment) -> bool");
    return kc_bool((mm_arg_addr(a, n, 0, pos, "mmu.is_aligned") & (pow2_arg(a, n, 1, pos, "mmu.is_aligned") - 1)) == 0);
}
static KcValue f_is_canonical(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.is_canonical(arch, va) -> bool");
    return kc_bool(mmu_is_canonical(need_arch(a, n, 0, pos, "mmu.is_canonical"), mm_arg_addr(a, n, 1, pos, "mmu.is_canonical")));
}
static KcValue f_canonical(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.canonical(arch, va) -> int (sign-extended form)");
    return kc_int((int64_t)mmu_canonical(need_arch(a, n, 0, pos, "mmu.canonical"), mm_arg_addr(a, n, 1, pos, "mmu.canonical")));
}

static KcValue f_split(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.split"; mm_need(n, 2, 2, pos, "mmu.split(arch, va) -> { indices, offset, half, canonical }");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who);
    KcValue o = kc_object_new();
    KcValue idx = kc_array_new();
    for (int L = A->levels - 1; L >= 0; L--) kc_array_push(idx, kc_int((int64_t)((va >> mmu_shift(A, L)) & (uint64_t)(mmu_entries(A) - 1))));
    oset(o, "indices", idx);
    OI(o, "offset", va & 0xFFF);
    OI(o, "half", mmu_va_half(A, va));
    OB(o, "canonical", mmu_is_canonical(A, va));
    return o;
}

/* ---- raw PTE codec ---- */

static KcValue f_pte_encode(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.pte_encode"; mm_need(n, 3, 4, pos, "mmu.pte_encode(arch, pa, flags [, page_size]) -> int (raw leaf entry)");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    uint64_t pa = mm_arg_addr(a, n, 1, pos, who);
    MFlags F; parse_flags(mm_arg_str(a, n, 2, pos, who), &F, pos, who);
    int L = n == 4 ? level_for_size(A, mm_arg_size(a, n, 3, pos, who), pos, who) : 0;
    Pte p; flags_to_pte(A, &F, &p, pos, who); p.pa = pa;
    char err[160]; uint64_t raw = mmu_pte_encode_leaf(A, L, &p, err, sizeof err);
    if (!raw) { mm_set_error("%s", err); return kc_null(); }
    return kc_int((int64_t)raw);
}

static KcValue f_pte_decode(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.pte_decode"; mm_need(n, 2, 3, pos, "mmu.pte_decode(arch, raw [, page_size]) -> object");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    uint64_t raw = mm_arg_addr(a, n, 1, pos, who);
    int L = n == 3 ? level_for_size(A, mm_arg_size(a, n, 2, pos, who), pos, who) : 0;
    Pte p; mmu_pte_decode(A, L, raw, &p);
    KcValue o = p.kind == PK_LEAF ? page_object(A, &p, L, 0, raw) : kc_object_new();
    OS(o, "kind", kind_name(p.kind)); OI(o, "raw", raw); OI(o, "level", L);
    if (p.kind == PK_TABLE) { OI(o, "next_table", p.pa); OB(o, "restrict_write", p.t_ro); OB(o, "restrict_user", p.t_nouser); }
    if (p.kind == PK_BAD) OS(o, "reason", p.bad_reason);
    if (p.kind == PK_SPECIAL) { OS(o, "special", p.sp_type == SP_SWAP ? "swap" : "protnone"); OI(o, "payload", p.pa); }
    return o;
}

static KcValue f_table_entry(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.table_entry"; mm_need(n, 2, 2, pos, "mmu.table_entry(arch, table_pa) -> int (raw next-level pointer)");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    uint64_t pa = mm_arg_addr(a, n, 1, pos, who);
    if (pa & 0xFFF) MM_ERR("%s: table address must be 4 KiB aligned", who);
    return kc_int((int64_t)mmu_pte_encode_table(A, pa));
}

/* ------------------------------------------------------------------ physical memory */

static KcValue f_phys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.phys"; mm_need(n, 1, 2, pos, "mmu.phys(size [, name]) -> physical memory");
    uint64_t sz = mm_arg_size(a, n, 0, pos, who);
    if (sz < 4096 || (sz & 4095)) MM_ERR("%s: size must be a multiple of 4096 (got %llu)", who, (unsigned long long)sz);
    if (sz > MMU_MAX_PHYS) MM_ERR("%s: at most 256 GiB of simulated physical memory", who);
    MmuPhys *p = mmu_phys_new(sz, n == 2 ? mm_arg_str(a, n, 1, pos, who) : "ram");
    return mmuh_wrap(MH_PHYS, p);
}

static KcValue f_phys_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.phys_info"; mm_need(n, 1, 1, pos, "mmu.phys_info(phys) -> object");
    MmuPhys *p = NEED_PHYS(0);
    KcValue o = kc_object_new();
    OS(o, "name", p->name); OI(o, "size", p->size); OI(o, "frames", p->nframes);
    OI(o, "frames_used", p->used_frames - p->reserved_frames); OI(o, "frames_free", p->nframes - p->used_frames);
    OI(o, "frames_reserved", p->reserved_frames);
    OI(o, "bytes_used", (p->used_frames - p->reserved_frames) * 4096);
    OI(o, "devices", p->ndev); OI(o, "swap_slots", p->swap_slots); OI(o, "swap_used", p->swap_in_use);
    OI(o, "swap_outs", p->swap_outs); OI(o, "swap_ins", p->swap_ins);
    OI(o, "reads", p->phys_reads); OI(o, "writes", p->phys_writes);
    KcValue devs = kc_array_new();
    for (int i = 0; i < p->ndev; i++) { KcValue d = kc_object_new(); OS(d, "name", p->dev[i].name); OI(d, "base", p->dev[i].base); OI(d, "size", p->dev[i].size); OI(d, "reads", p->dev[i].reads); OI(d, "writes", p->dev[i].writes); kc_array_push(devs, d); }
    oset(o, "device_list", devs);
    return o;
}

static int want_width(int64_t w, KcPos pos, const char *who) {
    if (w != 1 && w != 2 && w != 4 && w != 8) MM_ERR("%s: width must be 1, 2, 4 or 8 bytes (got %lld)", who, (long long)w);
    return (int)w;
}
static uint64_t fit_value(int64_t v, int w, KcPos pos, const char *who) {
    if (w == 8) return (uint64_t)v;
    int bits = w * 8; int64_t umax = ((int64_t)1 << bits) - 1, smin = -((int64_t)1 << (bits - 1));
    if (v < smin || v > umax) MM_ERR("%s: value %lld does not fit in %d bits", who, (long long)v, bits);
    return (uint64_t)v & (uint64_t)umax;
}

static KcValue f_phys_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_read"; mm_need(n, 3, 3, pos, "mmu.phys_read(phys, pa, width) -> int | null");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who); int w = want_width(mm_arg_int(a, n, 2, pos, who), pos, who);
    uint64_t v; if (!mmu_phys_read_u(p, pa, w, &v)) { mm_set_error("physical address 0x%llx is beyond the end of memory", (unsigned long long)pa); return kc_null(); }
    return kc_int((int64_t)v);
}
static KcValue f_phys_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_write"; mm_need(n, 4, 4, pos, "mmu.phys_write(phys, pa, width, value) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who); int w = want_width(mm_arg_int(a, n, 2, pos, who), pos, who);
    uint64_t v = fit_value(mm_arg_int(a, n, 3, pos, who), w, pos, who);
    if (!mmu_phys_write_u(p, pa, w, v)) { mm_set_error("physical address 0x%llx is beyond the end of memory", (unsigned long long)pa); return kc_bool(false); }
    return kc_bool(true);
}
static KcValue f_phys_read_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_read_bytes"; mm_need(n, 3, 3, pos, "mmu.phys_read_bytes(phys, pa, len) -> string | null");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who), len = (uint64_t)mm_arg_int(a, n, 2, pos, who);
    if ((int64_t)len < 0 || len > ((uint64_t)1 << 28)) MM_ERR("%s: length must be 0..256 MiB", who);
    uint8_t *buf = kc_malloc((size_t)len + 1);
    if (!mmu_phys_read(p, pa, buf, (size_t)len)) { free(buf); mm_set_error("range 0x%llx+%llu is beyond the end of memory", (unsigned long long)pa, (unsigned long long)len); return kc_null(); }
    buf[len] = 0;
    return kc_string_take((char *)buf, (int)len);
}
static KcValue f_phys_write_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_write_bytes"; mm_need(n, 3, 3, pos, "mmu.phys_write_bytes(phys, pa, string) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who);
    if (a[2].type != VAL_STRING) MM_ERR("%s: data must be a string", who);
    if (!mmu_phys_write(p, pa, a[2].as.s->data, (size_t)a[2].as.s->len)) { mm_set_error("range is beyond the end of memory"); return kc_bool(false); }
    return kc_bool(true);
}
static KcValue f_phys_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_fill"; mm_need(n, 4, 4, pos, "mmu.phys_fill(phys, pa, len, byte) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who), len = (uint64_t)mm_arg_int(a, n, 2, pos, who);
    int64_t b = mm_arg_int(a, n, 3, pos, who);
    if (b < -128 || b > 255) MM_ERR("%s: byte value out of range", who);
    if ((int64_t)len < 0 || len > ((uint64_t)1 << 28)) MM_ERR("%s: length must be 0..256 MiB", who);
    if (!mmu_phys_in_range(p, pa, len)) { mm_set_error("range is beyond the end of memory"); return kc_bool(false); }
    uint8_t blk[4096]; memset(blk, (int)(b & 0xFF), sizeof blk);
    for (uint64_t off = 0; off < len; off += sizeof blk) { size_t c = len - off < sizeof blk ? (size_t)(len - off) : sizeof blk; if (!mmu_phys_write(p, pa + off, blk, c)) return kc_bool(false); }
    return kc_bool(true);
}
static KcValue f_phys_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.phys_copy"; mm_need(n, 4, 4, pos, "mmu.phys_copy(phys, dst_pa, src_pa, len) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t d = mm_arg_addr(a, n, 1, pos, who), s = mm_arg_addr(a, n, 2, pos, who), len = (uint64_t)mm_arg_int(a, n, 3, pos, who);
    if ((int64_t)len < 0 || len > ((uint64_t)1 << 28)) MM_ERR("%s: length must be 0..256 MiB", who);
    if (!mmu_phys_in_range(p, d, len) || !mmu_phys_in_range(p, s, len)) { mm_set_error("range is beyond the end of memory"); return kc_bool(false); }
    uint8_t *tmp = kc_malloc((size_t)len + 1);
    bool ok = mmu_phys_read(p, s, tmp, (size_t)len) && mmu_phys_write(p, d, tmp, (size_t)len);
    free(tmp);
    return kc_bool(ok);
}

static KcValue f_frame_alloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *who = "mmu.frame_alloc"; mm_need(n, 1, 3, pos, "mmu.frame_alloc(phys [, count [, align_frames]]) -> pa | null");
    (void)ip;
    MmuPhys *p = NEED_PHYS(0);
    int64_t cnt = n >= 2 ? mm_arg_int(a, n, 1, pos, who) : 1, al = n >= 3 ? mm_arg_int(a, n, 2, pos, who) : 1;
    if (cnt < 1 || al < 1 || (al & (al - 1))) MM_ERR("%s: count must be >= 1 and align_frames a power of two", who);
    uint64_t pa; if (!mmu_phys_alloc(p, (uint64_t)cnt, (uint64_t)al, &pa)) { mm_set_error("no %lld contiguous free frame(s) (%llu of %llu in use)", (long long)cnt, (unsigned long long)p->used_frames, (unsigned long long)p->nframes); return kc_null(); }
    return kc_int((int64_t)pa);
}
static KcValue f_frame_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.frame_free"; mm_need(n, 2, 3, pos, "mmu.frame_free(phys, pa [, count]) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t pa = mm_arg_addr(a, n, 1, pos, who); int64_t cnt = n == 3 ? mm_arg_int(a, n, 2, pos, who) : 1;
    if (pa & 0xFFF || cnt < 1) MM_ERR("%s: pa must be frame aligned and count >= 1", who);
    for (int64_t i = 0; i < cnt; i++) if (!mmu_phys_frame_live(p, pa + (uint64_t)i * 4096) || (mmu_phys_refcount(p, pa + (uint64_t)i * 4096) == 0)) { mm_set_error("frame 0x%llx is not allocated", (unsigned long long)(pa + (uint64_t)i * 4096)); return kc_bool(false); }
    for (int64_t i = 0; i < cnt; i++) mmu_phys_ref_dec(p, pa + (uint64_t)i * 4096);
    return kc_bool(true);
}
static KcValue f_frame_refs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.frame_refs"; mm_need(n, 2, 2, pos, "mmu.frame_refs(phys, pa) -> int (0 = free)");
    MmuPhys *p = NEED_PHYS(0);
    return kc_int(mmu_phys_refcount(p, mm_arg_addr(a, n, 1, pos, who) & ~(uint64_t)0xFFF));
}

static KcValue f_device(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.device"; mm_need(n, 5, 6, pos, "mmu.device(phys, base, size, read_fn, write_fn [, name]) -> bool");
    MmuPhys *p = NEED_PHYS(0); uint64_t base = mm_arg_addr(a, n, 1, pos, who), size = mm_arg_size(a, n, 2, pos, who);
    for (int i = 3; i <= 4; i++) if (a[i].type != VAL_CLOSURE && a[i].type != VAL_NATIVE && a[i].type != VAL_NULL) MM_ERR("%s: read/write handlers must be functions (or null)", who);
    if (!mmu_phys_add_device(p, base, size, a[3], a[4], n == 6 ? mm_arg_str(a, n, 5, pos, who) : "device")) {
        mm_set_error("cannot place a device at 0x%llx (+0x%llx): must be page aligned, inside memory, free of allocated frames, and at most %d devices", (unsigned long long)base, (unsigned long long)size, MMU_MAX_DEVICES);
        return kc_bool(false);
    }
    return kc_bool(true);
}

static KcValue f_swap_init(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.swap_init"; mm_need(n, 2, 2, pos, "mmu.swap_init(phys, slots) -> bool");
    MmuPhys *p = NEED_PHYS(0); int64_t sl = mm_arg_int(a, n, 1, pos, who);
    if (sl < 1 || sl > (1 << 22)) MM_ERR("%s: slots must be 1..4194304", who);
    if (!mmu_swap_init(p, (uint32_t)sl)) { mm_set_error("swap area already configured"); return kc_bool(false); }
    return kc_bool(true);
}

/* ------------------------------------------------------------------ address spaces */

static KcValue f_space(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.space"; mm_need(n, 2, 4, pos, "mmu.space(arch, phys [, options [, name]]) -> address space");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    MmuPhys *p = need_h(a, n, 1, MH_PHYS, pos, who);
    char err[200] = "";
    MmuSpace *S = mmu_space_new(A, p, n == 4 ? mm_arg_str(a, n, 3, pos, who) : "space", n >= 3 && a[2].type == VAL_STRING ? a[2].as.s->data : NULL, -1, err, sizeof err);
    if (!S) { mm_set_error("%s", err); if (strstr(err, "unknown option") || strstr(err, "out of range") || strstr(err, "must be")) kc_error_at(pos, "%s: %s", who, err); return kc_null(); }
    return mmuh_wrap(MH_SPACE, S);
}

static KcValue f_cpu(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.cpu"; mm_need(n, 1, 3, pos, "mmu.cpu(arch [, options [, name]]) -> cpu");
    const MmuArch *A = need_arch(a, n, 0, pos, who);
    char err[200] = "";
    MmuCpu *c = mmu_cpu_create(A, n == 3 ? mm_arg_str(a, n, 2, pos, who) : "cpu", err, sizeof err);
    if (!c) { mm_set_error("%s", err); return kc_null(); }
    if (n >= 2 && a[1].type == VAL_STRING && !mmu_cpu_parse_opts(c, a[1].as.s->data, NULL, NULL, err, sizeof err)) { mmu_cpu_destroy(c); kc_error_at(pos, "%s: %s", who, err); }
    return mmuh_wrap(MH_CPU, c);
}

static KcValue f_destroy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.destroy"; mm_need(n, 1, 1, pos, "mmu.destroy(handle) -> bool");
    if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != H_MMU) MM_ERR("%s: expected an mmu handle", who);
    KcHandle *h = a[0].as.handle;
    if (!h->ptr) return kc_bool(false);
    void *ptr = h->ptr; h->ptr = NULL;
    switch (h->extra) {
    case MH_PHYS: { MmuPhys *p = ptr; p->handle_alive = false; mmu_phys_release(p); break; }
    case MH_SPACE: { MmuSpace *S = ptr; mmu_vm_destroy(S); free(S); break; }
    case MH_CPU: mmu_cpu_destroy(ptr); break;
    case MH_SHM: { MmuShm *s = ptr; s->alive = false; mmu_shm_release(s); break; }
    default: h->ptr = ptr; mmu_host_destroy(h); break;
    }
    return kc_bool(true);
}

static KcValue f_root(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.root"; mm_need(n, 1, 2, pos, "mmu.root(space [, half]) -> physical address of the root table");
    MmuSpace *S = NEED_SPACE(0); int h = n == 2 ? (int)mm_arg_int(a, n, 1, pos, who) : 0;
    if (h < 0 || h > 1 || (h == 1 && !S->A->split_va)) MM_ERR("%s: half must be 0 (or 1 on arm64 for TTBR1)", who);
    if (!S->has_root[h]) return kc_null();
    return kc_int((int64_t)S->root[h]);
}
static KcValue f_root_reg(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.root_reg"; mm_need(n, 1, 2, pos, "mmu.root_reg(space [, half]) -> raw CR3/TTBR/satp value");
    MmuSpace *S = NEED_SPACE(0); int h = n == 2 ? (int)mm_arg_int(a, n, 1, pos, who) : 0;
    if (h < 0 || h > 1 || (h == 1 && !S->A->split_va)) MM_ERR("%s: half must be 0 (or 1 on arm64)", who);
    if (!S->has_root[h]) return kc_null();
    return kc_int((int64_t)mmu_root_reg(S->A, S->root[h], S->asid, h));
}

/* ---- map / unmap / protect ---- */

static KcValue f_map(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.map"; mm_need(n, 4, 5, pos, "mmu.map(space, va, pa, flags [, page_size]) -> bool");
    MmuSpace *S = NEED_SPACE(0); uint64_t va = mm_arg_addr(a, n, 1, pos, who), pa = mm_arg_addr(a, n, 2, pos, who);
    MFlags F; parse_flags(mm_arg_str(a, n, 3, pos, who), &F, pos, who);
    int L = n == 5 ? level_for_size(S->A, mm_arg_size(a, n, 4, pos, who), pos, who) : 0;
    Pte p; flags_to_pte(S->A, &F, &p, pos, who); p.pa = pa;
    char err[200];
    if (!mmu_pt_map_pte(S, va, L, &p, F.force, err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue f_map_range(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.map_range"; mm_need(n, 5, 5, pos, "mmu.map_range(space, va, pa, len, flags) -> number of entries | null");
    MmuSpace *S = NEED_SPACE(0); const MmuArch *A = S->A;
    uint64_t va = mm_arg_addr(a, n, 1, pos, who), pa = mm_arg_addr(a, n, 2, pos, who), len = mm_arg_size(a, n, 3, pos, who);
    MFlags F; parse_flags(mm_arg_str(a, n, 4, pos, who), &F, pos, who);
    if ((va | pa | len) & 0xFFF || len == 0) MM_ERR("%s: va, pa and len must be 4 KiB aligned and len > 0", who);
    Pte p; flags_to_pte(A, &F, &p, pos, who);
    char err[200]; int64_t entries = 0; uint64_t done = 0;
    uint64_t last; if (!mmu_range_ok(S, va, len, &last, err, sizeof err)) { mm_set_error("%s", err); return kc_null(); }
    while (done < len) {
        int L = 0;
        if (F.huge) for (int l = A->levels - 1; l > 0; l--) {
            if (!(A->leaf_mask & (1u << l))) continue;
            uint64_t sz = mmu_page_size(A, l);
            if (!((va + done) & (sz - 1)) && !((pa + done) & (sz - 1)) && len - done >= sz) { L = l; break; }
        }
        p.pa = pa + done;
        if (!mmu_pt_map_pte(S, va + done, L, &p, F.force, err, sizeof err)) {
            if (done) mmu_pt_unmap(S, va, done, NULL, 0);
            mm_set_error("%s (while mapping 0x%llx)", err, (unsigned long long)(va + done));
            return kc_null();
        }
        done += mmu_page_size(A, L); entries++;
    }
    return kc_int(entries);
}

static KcValue f_unmap(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.unmap"; mm_need(n, 2, 3, pos, "mmu.unmap(space, va [, len]) -> number of 4 KiB pages removed | null");
    MmuSpace *S = NEED_SPACE(0); uint64_t va = mm_arg_addr(a, n, 1, pos, who), len = n == 3 ? mm_arg_size(a, n, 2, pos, who) : 4096;
    if (len & 0xFFF) len = (len + 0xFFF) & ~(uint64_t)0xFFF;
    char err[200]; int64_t r = mmu_pt_unmap(S, va & ~(uint64_t)0xFFF, len, err, sizeof err);
    if (r < 0) { mm_set_error("%s", err); return kc_null(); }
    return kc_int(r);
}

static KcValue f_protect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.protect"; mm_need(n, 4, 4, pos, "mmu.protect(space, va, len, flags) -> number of 4 KiB pages changed | null   (flags: \"r,w\" replaces, \"+w,-x\" modifies)");
    MmuSpace *S = NEED_SPACE(0); uint64_t va = mm_arg_addr(a, n, 1, pos, who), len = mm_arg_size(a, n, 2, pos, who);
    MFlags F; parse_flags(mm_arg_str(a, n, 3, pos, who), &F, pos, who);
    int mask, val;
    if (F.rel) { mask = (int)(F.add | F.del); val = (int)F.add; if (F.prot) { mask |= (int)F.prot; val |= (int)F.prot; } }
    else { mask = PROT_R | PROT_W | PROT_X | PROT_U | PROT_G; val = (int)F.prot; }
    char err[200];
    int64_t r = mmu_pt_protect(S, va & ~(uint64_t)0xFFF, (len + 0xFFF) & ~(uint64_t)0xFFF, mask, val, F.cache_set, F.cache, false, err, sizeof err);
    if (r < 0) { mm_set_error("%s", err); return kc_null(); }
    return kc_int(r);
}

static KcValue f_split_page(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.split_page"; mm_need(n, 2, 2, pos, "mmu.split_page(space, va) -> bool (breaks a large page into the next smaller size)");
    MmuSpace *S = NEED_SPACE(0); char err[160];
    if (!mmu_pt_split(S, mm_arg_addr(a, n, 1, pos, who), err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

/* ---- queries & diagnostics ---- */

static KcValue f_query(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.query"; mm_need(n, 2, 2, pos, "mmu.query(space, va) -> page object | null");
    MmuSpace *S = NEED_SPACE(0); uint64_t va = mm_arg_addr(a, n, 1, pos, who);
    WalkResult w; mmu_walk(S, va, &w);
    if (w.ok) {
        KcValue o = page_object(S->A, &w.leaf, w.level, w.page_va, w.raw);
        OI(o, "phys", w.leaf.pa + (va & (mmu_page_size(S->A, w.level) - 1)));
        OI(o, "pte_addr", w.pte_pa);
        return o;
    }
    if (w.special) {
        KcValue o = kc_object_new();
        OI(o, "va", va & ~(uint64_t)0xFFF);
        if (w.sp.sp_type == SP_SWAP) { OB(o, "swapped", true); OI(o, "slot", w.sp.pa >> 12); }
        else { OB(o, "protnone", true); OI(o, "pa", w.sp.pa); }
        OB(o, "own", w.sp.sp_own); OB(o, "cow", w.sp.sp_cow);
        return o;
    }
    mm_set_error("0x%llx is not mapped", (unsigned long long)va);
    return kc_null();
}

static KcValue f_walk(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.walk"; mm_need(n, 2, 2, pos, "mmu.walk(space, va) -> { ok, steps: [...], ... }");
    MmuSpace *S = NEED_SPACE(0); uint64_t va = mm_arg_addr(a, n, 1, pos, who);
    WalkResult w; mmu_walk(S, va, &w);
    KcValue o;
    if (w.ok) { o = page_object(S->A, &w.leaf, w.level, w.page_va, w.raw); OB(o, "ok", true); OI(o, "phys", w.leaf.pa + (va & (mmu_page_size(S->A, w.level) - 1))); }
    else { w.fault.access = ACC_READ; mmu_fault_fill_code(S->A, &w.fault); o = mm_fault_object(S->A, &w.fault); }
    KcValue steps = kc_array_new();
    for (int i = 0; i < w.nsteps; i++) {
        KcValue s = kc_object_new();
        OI(s, "level", w.steps[i].level); OI(s, "table", w.steps[i].table_pa); OI(s, "index", w.steps[i].index);
        OI(s, "entry_addr", w.steps[i].table_pa + (uint64_t)w.steps[i].index * (uint64_t)S->A->pte_size);
        OI(s, "raw", w.steps[i].raw); OS(s, "kind", kind_name(w.steps[i].kind));
        kc_array_push(steps, s);
    }
    oset(o, "steps", steps);
    return o;
}

static void opt_range(KcValue *a, int n, int i, KcPos pos, const char *who, uint64_t *lo, uint64_t *hi) {
    *lo = 0; *hi = ~(uint64_t)0;
    if (n > i) *lo = mm_arg_addr(a, n, i, pos, who);
    if (n > i + 1) *hi = mm_arg_addr(a, n, i + 1, pos, who);
}

static KcValue f_mappings(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.mappings"; mm_need(n, 1, 3, pos, "mmu.mappings(space [, lo, hi_inclusive]) -> [ { va, size, pa, flags, ... } ]");
    MmuSpace *S = NEED_SPACE(0); uint64_t lo, hi; opt_range(a, n, 1, pos, who, &lo, &hi);
    return mmu_pt_mappings(S, lo, hi);
}
static KcValue f_dump(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.dump"; mm_need(n, 1, 3, pos, "mmu.dump(space [, lo, hi_inclusive]) -> string (ptdump-style)");
    MmuSpace *S = NEED_SPACE(0); uint64_t lo, hi; opt_range(a, n, 1, pos, who, &lo, &hi);
    char *s = mmu_pt_dump(S, lo, hi);
    return kc_string_take(s, (int)strlen(s));
}
static KcValue f_verify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.verify"; mm_need(n, 1, 1, pos, "mmu.verify(space) -> [issues] (empty = consistent)");
    return mmu_pt_verify(NEED_SPACE(0));
}

static const char *size_key(uint64_t v, char *b, size_t n) {
    if (v >= (1ull << 30)) snprintf(b, n, "%lluG", (unsigned long long)(v >> 30));
    else if (v >= (1ull << 20)) snprintf(b, n, "%lluM", (unsigned long long)(v >> 20));
    else snprintf(b, n, "%lluK", (unsigned long long)(v >> 10));
    return b;
}

static KcValue tlb_stats_obj(MmuCpu *c) {
    KcValue t = kc_object_new();
    OI(t, "hits", c->tlb_hits); OI(t, "misses", c->tlb_misses); OI(t, "evictions", c->tlb_evictions);
    OI(t, "flushes", c->tlb_flushes); OI(t, "invalidations", c->tlb_invlpg); OI(t, "walks", c->walks);
    uint64_t tot = c->tlb_hits + c->tlb_misses;
    oset(t, "hit_rate", kc_float(tot ? (double)c->tlb_hits / (double)tot : 0.0));
    return t;
}

static KcValue f_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.stats"; mm_need(n, 1, 1, pos, "mmu.stats(space) -> object");
    MmuSpace *S = NEED_SPACE(0);
    uint64_t pages[MMU_MAX_LEVELS], tables[MMU_MAX_LEVELS], sw, pn, own;
    mmu_pt_stats(S, pages, tables, &sw, &pn, &own);
    KcValue o = kc_object_new(), pg = kc_object_new(), tb = kc_object_new();
    uint64_t bytes = 0, tcount = 0;
    for (int L = 0; L < S->A->levels; L++) {
        if (pages[L]) { char b[16]; OI(pg, size_key(mmu_page_size(S->A, L), b, sizeof b), pages[L]); }
        bytes += pages[L] * mmu_page_size(S->A, L);
        char lb[16]; snprintf(lb, sizeof lb, "L%d", L); OI(tb, lb, tables[L]); tcount += tables[L];
    }
    oset(o, "pages", pg); oset(o, "tables", tb);
    OI(o, "mapped_bytes", bytes); OI(o, "table_frames", tcount); OI(o, "swapped_pages", sw); OI(o, "protnone_pages", pn); OI(o, "owned_leaves", own);
    OI(o, "vmas", S->nvma); OI(o, "asid", S->asid);
    KcValue f = kc_object_new();
    OI(f, "total", S->home->faults); OI(f, "handled", S->home->handled_faults); OI(f, "demand_zero", S->n_demand); OI(f, "cow", S->n_cow);
    OI(f, "swap_in", S->n_swapin); OI(f, "swap_out", S->n_swapout); OI(f, "ad_fixups", S->n_ad_fix); OI(f, "huge_pages", S->n_huge);
    OI(f, "splits", S->n_splits); OI(f, "forked_cow_pages", S->n_fork_cow);
    oset(o, "faults", f);
    oset(o, "tlb", tlb_stats_obj(S->home));
    return o;
}

/* ------------------------------------------------------------------ cpu: switch, policy, handlers, TLB */

static KcValue f_switch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.switch"; mm_need(n, 2, 3, pos, "mmu.switch(cpu, space [, flush]) -> bool   (load a new root: CR3/TTBR0/satp write)");
    MmuCpu *c = need_h(a, n, 0, MH_CPU, pos, who);
    MmuSpace *S = NEED_SPACE(1);
    if (S->A != c->A) MM_ERR("%s: cpu is %s but the address space is %s", who, c->A->name, S->A->name);
    bool fl = n == 3 && kc_truthy(a[2]);
    mmu_cpu_switch(c, S, fl);
    return kc_bool(true);
}

static KcValue f_current(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.current"; mm_need(n, 1, 1, pos, "mmu.current(cpu) -> asid of the loaded space | null");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    return c->cur ? kc_int(c->cur->asid) : kc_null();
}

static KcValue f_policy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.policy"; mm_need(n, 1, 2, pos, "mmu.policy(cpu_or_space [, options]) -> object   (options: wp nowp smep nosmep smap nosmap sw_ad hw_ad manual_tlb auto_tlb strict)");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    if (n == 2) {
        char err[200];
        if (!mmu_cpu_parse_opts(c, mm_arg_str(a, n, 1, pos, who), NULL, NULL, err, sizeof err)) MM_ERR("%s: %s", who, err);
    }
    KcValue o = kc_object_new();
    OB(o, "wp", c->wp); OB(o, "smep", c->smep); OB(o, "smap", c->smap); OB(o, "sw_ad", c->sw_ad); OB(o, "manual_tlb", c->manual_tlb);
    OB(o, "strict", c->strict); OB(o, "asid", c->use_asid); OI(o, "tlb_entries", c->tlb_entries); OI(o, "tlb_ways", c->tlb_ways);
    return o;
}

static KcValue f_on_fault(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.on_fault"; mm_need(n, 2, 2, pos, "mmu.on_fault(cpu_or_space, handler | null) -> bool   (handler(fault) -> true to retry the access)");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    if (a[1].type == VAL_NULL) { c->has_handler = false; return kc_bool(true); }
    if (a[1].type != VAL_CLOSURE && a[1].type != VAL_NATIVE) MM_ERR("%s: handler must be a function or null", who);
    c->on_fault = a[1]; c->has_handler = true;
    return kc_bool(true);
}

static KcValue f_strict(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.strict"; mm_need(n, 2, 2, pos, "mmu.strict(cpu_or_space, on) -> bool   (unhandled faults become fatal errors)");
    need_cpu(a, n, 0, pos, who)->strict = kc_truthy(a[1]);
    return kc_bool(true);
}

static KcValue f_last_fault(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.last_fault"; mm_need(n, 0, 1, pos, "mmu.last_fault([cpu_or_space]) -> fault object | null");
    if (n == 1) { MmuCpu *c = need_cpu(a, n, 0, pos, who); return c->has_last_fault ? mm_fault_object(c->A, &c->last_fault) : kc_null(); }
    return MMU_LAST_FAULT_VALID ? mm_fault_object(MMU_LAST_FAULT_ARCH, &MMU_LAST_FAULT) : kc_null();
}

static KcValue f_tlb_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.tlb_info"; mm_need(n, 1, 1, pos, "mmu.tlb_info(cpu_or_space) -> object");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    KcValue o = tlb_stats_obj(c);
    OI(o, "entries", c->tlb_entries); OI(o, "ways", c->tlb_ways); OB(o, "asid_tagged", c->use_asid); OB(o, "manual", c->manual_tlb);
    KcValue banks = kc_array_new(); int live = 0;
    for (int L = 0; L < c->A->levels; L++) {
        if (!c->bank[L].e) continue;
        KcValue b = kc_object_new(); int v = 0, tot = c->bank[L].sets * c->bank[L].ways;
        for (int i = 0; i < tot; i++) if (c->bank[L].e[i].valid) v++;
        live += v;
        OI(b, "level", L); OI(b, "page_size", mmu_page_size(c->A, L)); OI(b, "sets", c->bank[L].sets); OI(b, "ways", c->bank[L].ways); OI(b, "valid", v);
        kc_array_push(banks, b);
    }
    oset(o, "banks", banks); OI(o, "valid", live);
    return o;
}

static KcValue f_tlb_dump(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.tlb_dump"; mm_need(n, 1, 1, pos, "mmu.tlb_dump(cpu_or_space) -> [ { va, pa, size, asid, flags, ... } ]");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    KcValue arr = kc_array_new();
    for (int L = 0; L < c->A->levels; L++) {
        if (!c->bank[L].e) continue;
        int tot = c->bank[L].sets * c->bank[L].ways;
        for (int i = 0; i < tot; i++) {
            MmuTlbEnt *e = &c->bank[L].e[i]; if (!e->valid) continue;
            KcValue o = kc_object_new(); Pte p; memset(&p, 0, sizeof p); p.r = e->r; p.w = e->w; p.x = e->x; p.user = e->user; p.global = e->global; p.cache = e->cache;
            char fl[64]; mmu_flags_str(fl, sizeof fl, &p);
            OI(o, "va", e->vpn << mmu_shift(c->A, L)); OI(o, "pa", e->pa); OI(o, "size", mmu_page_size(c->A, L)); OI(o, "level", L);
            OI(o, "asid", e->asid); OB(o, "global", e->global); OB(o, "dirty", e->dirty); OS(o, "flags", fl);
            kc_array_push(arr, o);
        }
    }
    return arr;
}

static KcValue f_tlb_flush(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.tlb_flush"; mm_need(n, 1, 2, pos, "mmu.tlb_flush(cpu_or_space [, include_global]) -> bool");
    mmu_tlb_flush(need_cpu(a, n, 0, pos, who), n == 2 && kc_truthy(a[1]));
    return kc_bool(true);
}
static KcValue f_tlb_invalidate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.tlb_invalidate"; mm_need(n, 2, 3, pos, "mmu.tlb_invalidate(cpu_or_space, va [, asid]) -> bool   (INVLPG / TLBI / SFENCE.VMA)");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    int asid = n == 3 ? (int)mm_arg_int(a, n, 2, pos, who) : (c->cur ? (int)c->cur->asid : -1);
    mmu_tlb_invalidate(c, mm_arg_addr(a, n, 1, pos, who), c->use_asid ? asid : -1);
    return kc_bool(true);
}
static KcValue f_tlb_config(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.tlb_config"; mm_need(n, 2, 3, pos, "mmu.tlb_config(cpu_or_space, entries [, ways]) -> bool   (0 entries disables the TLB)");
    MmuCpu *c = need_cpu(a, n, 0, pos, who);
    int64_t e = mm_arg_int(a, n, 1, pos, who), w = n == 3 ? mm_arg_int(a, n, 2, pos, who) : c->tlb_ways;
    if (e < 0 || e > 65536 || w < 1 || w > 16) MM_ERR("%s: entries 0..65536, ways 1..16", who);
    mmu_tlb_configure(c, (int)e, (int)w);
    return kc_bool(true);
}

/* ------------------------------------------------------------------ translation & data access */

static KcValue xlate_object(MmuCpu *c, const MmuXlate *x, uint64_t va) {
    if (!x->ok) return mm_fault_object(c->A, &x->fault);
    Pte p; memset(&p, 0, sizeof p); p.r = x->r; p.w = x->w; p.x = x->x; p.user = x->user; p.global = x->global; p.cache = x->cache; p.sw = x->sw;
    char fl[64]; mmu_flags_str(fl, sizeof fl, &p);
    KcValue o = kc_object_new();
    OB(o, "ok", true); OI(o, "va", va); OI(o, "pa", x->pa); OI(o, "page_va", x->page_va); OI(o, "page_pa", x->page_pa);
    OI(o, "page_size", x->page_size); OI(o, "level", x->level); OS(o, "flags", fl);
    OB(o, "r", x->r); OB(o, "w", x->w); OB(o, "x", x->x); OB(o, "user", x->user); OB(o, "global", x->global); OB(o, "dirty", x->dirty);
    OS(o, "cache", MMU_CACHE_NAMES[x->cache]); OB(o, "tlb_hit", x->from_tlb);
    return o;
}

static KcValue f_translate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.translate"; mm_need(n, 3, 4, pos, "mmu.translate(cpu_or_space, va, access [, mode]) -> { ok, pa, ... } | fault object");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); Access acc = parse_access(a, n, 2, pos, who); bool user = parse_mode(a, n, 3, pos, who);
    MmuXlate x; mmu_xlate(c, va, acc, user, &x, false);
    return xlate_object(c, &x, va);
}
static KcValue f_probe(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.probe"; mm_need(n, 3, 4, pos, "mmu.probe(cpu_or_space, va, access [, mode]) -> like translate, but with no side effects (no TLB fill, A/D update or fault handlers)");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); Access acc = parse_access(a, n, 2, pos, who); bool user = parse_mode(a, n, 3, pos, who);
    MmuXlate x; mmu_xlate(c, va, acc, user, &x, true);
    return xlate_object(c, &x, va);
}
static KcValue f_virt_to_phys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.virt_to_phys"; mm_need(n, 2, 4, pos, "mmu.virt_to_phys(cpu_or_space, va [, access [, mode]]) -> pa | null");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); Access acc = n >= 3 ? parse_access(a, n, 2, pos, who) : ACC_READ; bool user = parse_mode(a, n, 3, pos, who);
    MmuXlate x; mmu_xlate(c, va, acc, user, &x, false);
    if (!x.ok) { mm_set_error("page fault"); return kc_null(); }
    return kc_int((int64_t)x.pa);
}
static KcValue f_can(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.can"; mm_need(n, 3, 4, pos, "mmu.can(cpu_or_space, va, access [, mode]) -> bool   (side-effect-free permission probe)");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); Access acc = parse_access(a, n, 2, pos, who); bool user = parse_mode(a, n, 3, pos, who);
    MmuXlate x; mmu_xlate(c, va, acc, user, &x, true);
    return kc_bool(x.ok);
}

static KcValue access_failed(MmuCpu *c, const Fault *f, KcValue fail_value) {
    if (c->strict) mmu_deliver_fault_strict(c, f);
    mm_set_error("page fault");
    return fail_value;
}

static KcValue do_read(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, int w, int mode_idx) {
    MM_ENTER();
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); bool user = parse_mode(a, n, mode_idx, pos, who);
    uint8_t b[8] = {0}; Fault f;
    if (!mmu_vread(c, va, b, (size_t)w, user, &f)) return access_failed(c, &f, kc_null());
    uint64_t v = 0; for (int i = 0; i < w; i++) v |= (uint64_t)b[i] << (8 * i);
    return kc_int((int64_t)v);
}
static KcValue do_write(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, int w, int val_idx, int mode_idx) {
    MM_ENTER();
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); uint64_t v = fit_value(mm_arg_int(a, n, val_idx, pos, who), w, pos, who);
    bool user = parse_mode(a, n, mode_idx, pos, who);
    uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    Fault f;
    if (!mmu_vwrite(c, va, b, (size_t)w, user, &f)) return access_failed(c, &f, kc_bool(false));
    return kc_bool(true);
}
#define RD(NAME, W) static KcValue f_read##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { mm_need(n, 2, 3, pos, "mmu.read" #NAME "(cpu_or_space, va [, mode]) -> int | null"); return do_read(ip, a, n, pos, "mmu.read" #NAME, W, 2); }
#define WR(NAME, W) static KcValue f_write##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { mm_need(n, 3, 4, pos, "mmu.write" #NAME "(cpu_or_space, va, value [, mode]) -> bool"); return do_write(ip, a, n, pos, "mmu.write" #NAME, W, 2, 3); }
RD(8, 1) RD(16, 2) RD(32, 4) RD(64, 8)
WR(8, 1) WR(16, 2) WR(32, 4) WR(64, 8)

static KcValue f_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mm_need(n, 3, 4, pos, "mmu.read(cpu_or_space, va, width [, mode]) -> int | null");
    int w = want_width(mm_arg_int(a, n, 2, pos, "mmu.read"), pos, "mmu.read");
    KcValue sh[4]; sh[0] = a[0]; sh[1] = a[1]; if (n == 4) sh[2] = a[3];
    return do_read(ip, sh, n - 1, pos, "mmu.read", w, 2);
}
static KcValue f_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mm_need(n, 4, 5, pos, "mmu.write(cpu_or_space, va, width, value [, mode]) -> bool");
    int w = want_width(mm_arg_int(a, n, 2, pos, "mmu.write"), pos, "mmu.write");
    KcValue sh[5]; sh[0] = a[0]; sh[1] = a[1]; sh[2] = a[3]; if (n == 5) sh[3] = a[4];
    return do_write(ip, sh, n - 1, pos, "mmu.write", w, 2, 3);
}

#define MAX_VBYTES ((uint64_t)1 << 26)

static KcValue f_read_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.read_bytes"; mm_need(n, 3, 4, pos, "mmu.read_bytes(cpu_or_space, va, len [, mode]) -> string | null");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); int64_t len = mm_arg_int(a, n, 2, pos, who); bool user = parse_mode(a, n, 3, pos, who);
    if (len < 0 || (uint64_t)len > MAX_VBYTES) MM_ERR("%s: length must be 0..64 MiB", who);
    uint8_t *buf = kc_malloc((size_t)len + 1); Fault f;
    if (!mmu_vread(c, va, buf, (size_t)len, user, &f)) { free(buf); return access_failed(c, &f, kc_null()); }
    buf[len] = 0;
    return kc_string_take((char *)buf, (int)len);
}
static KcValue f_write_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.write_bytes"; mm_need(n, 3, 4, pos, "mmu.write_bytes(cpu_or_space, va, string [, mode]) -> bool");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who);
    if (a[2].type != VAL_STRING) MM_ERR("%s: data must be a string", who);
    bool user = parse_mode(a, n, 3, pos, who); Fault f;
    if (!mmu_vwrite(c, va, a[2].as.s->data, (size_t)a[2].as.s->len, user, &f)) return access_failed(c, &f, kc_bool(false));
    return kc_bool(true);
}
static KcValue f_read_cstring(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.read_cstring"; mm_need(n, 2, 4, pos, "mmu.read_cstring(cpu_or_space, va [, max_len [, mode]]) -> string | null");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); int64_t mx = n >= 3 ? mm_arg_int(a, n, 2, pos, who) : 4096; bool user = parse_mode(a, n, 3, pos, who);
    if (mx < 0 || (uint64_t)mx > MAX_VBYTES) MM_ERR("%s: max_len must be 0..64 MiB", who);
    char *buf = kc_malloc((size_t)mx + 1); size_t len = 0; Fault f;
    while ((int64_t)len < mx) {
        size_t chunk = 4096 - (size_t)((va + len) & 4095); if ((int64_t)(len + chunk) > mx) chunk = (size_t)mx - len;
        if (!mmu_vread(c, va + len, buf + len, chunk, user, &f)) { free(buf); return access_failed(c, &f, kc_null()); }
        char *z = memchr(buf + len, 0, chunk);
        if (z) { len = (size_t)(z - buf); break; }
        len += chunk;
    }
    buf[len] = 0;
    return kc_string_take(buf, (int)len);
}
static KcValue f_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.fill"; mm_need(n, 4, 5, pos, "mmu.fill(cpu_or_space, va, len, byte [, mode]) -> bool");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t va = mm_arg_addr(a, n, 1, pos, who); int64_t len = mm_arg_int(a, n, 2, pos, who), b = mm_arg_int(a, n, 3, pos, who); bool user = parse_mode(a, n, 4, pos, who);
    if (len < 0 || (uint64_t)len > MAX_VBYTES || b < -128 || b > 255) MM_ERR("%s: len 0..64 MiB, byte -128..255", who);
    uint8_t *buf = kc_malloc((size_t)len + 1); memset(buf, (int)(b & 0xFF), (size_t)len); Fault f;
    bool ok = mmu_vwrite(c, va, buf, (size_t)len, user, &f); free(buf);
    return ok ? kc_bool(true) : access_failed(c, &f, kc_bool(false));
}
static KcValue f_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.copy"; mm_need(n, 4, 5, pos, "mmu.copy(cpu_or_space, dst, src, len [, mode]) -> bool");
    MmuCpu *c = need_loaded_cpu(a, n, 0, pos, who);
    uint64_t d = mm_arg_addr(a, n, 1, pos, who), s = mm_arg_addr(a, n, 2, pos, who); int64_t len = mm_arg_int(a, n, 3, pos, who); bool user = parse_mode(a, n, 4, pos, who);
    if (len < 0 || (uint64_t)len > MAX_VBYTES) MM_ERR("%s: len must be 0..64 MiB", who);
    uint8_t *buf = kc_malloc((size_t)len + 1); Fault f; bool ok = mmu_vread(c, s, buf, (size_t)len, user, &f) && mmu_vwrite(c, d, buf, (size_t)len, user, &f);
    free(buf);
    return ok ? kc_bool(true) : access_failed(c, &f, kc_bool(false));
}
static KcValue f_sext(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; mm_need(n, 2, 2, pos, "mmu.sext(value, bits) -> int (sign-extend the low `bits` bits)");
    uint64_t v = mm_arg_addr(a, n, 0, pos, "mmu.sext"); int64_t b = mm_arg_int(a, n, 1, pos, "mmu.sext");
    if (b < 1 || b > 64) MM_ERR("mmu.sext: bits must be 1..64");
    if (b == 64) return kc_int((int64_t)v);
    return kc_int((int64_t)(v << (64 - b)) >> (64 - b));
}

#define E(n, f) { n, f }
const MmuEntry mmu_api_entries[] = {
    E("last_error", f_last_error), E("archs", f_archs), E("arch_info", f_arch_info),
    E("hex", f_hex), E("parse", f_parse), E("ucmp", f_ucmp), E("sext", f_sext),
    E("align_down", f_align_down), E("align_up", f_align_up), E("is_aligned", f_is_aligned),
    E("is_canonical", f_is_canonical), E("canonical", f_canonical), E("split", f_split),
    E("pte_encode", f_pte_encode), E("pte_decode", f_pte_decode), E("table_entry", f_table_entry),
    E("phys", f_phys), E("phys_info", f_phys_info), E("phys_read", f_phys_read), E("phys_write", f_phys_write),
    E("phys_read_bytes", f_phys_read_bytes), E("phys_write_bytes", f_phys_write_bytes), E("phys_fill", f_phys_fill), E("phys_copy", f_phys_copy),
    E("frame_alloc", f_frame_alloc), E("frame_free", f_frame_free), E("frame_refs", f_frame_refs),
    E("device", f_device), E("swap_init", f_swap_init),
    E("space", f_space), E("cpu", f_cpu), E("destroy", f_destroy), E("root", f_root), E("root_reg", f_root_reg),
    E("map", f_map), E("map_range", f_map_range), E("unmap", f_unmap), E("protect", f_protect), E("split_page", f_split_page),
    E("query", f_query), E("walk", f_walk), E("mappings", f_mappings), E("dump", f_dump), E("verify", f_verify), E("stats", f_stats),
    E("switch", f_switch), E("current", f_current), E("policy", f_policy), E("on_fault", f_on_fault), E("strict", f_strict), E("last_fault", f_last_fault),
    E("tlb_info", f_tlb_info), E("tlb_dump", f_tlb_dump), E("tlb_flush", f_tlb_flush), E("tlb_invalidate", f_tlb_invalidate), E("tlb_config", f_tlb_config),
    E("translate", f_translate), E("probe", f_probe), E("virt_to_phys", f_virt_to_phys), E("can", f_can),
    E("read8", f_read8), E("read16", f_read16), E("read32", f_read32), E("read64", f_read64), E("read", f_read),
    E("write8", f_write8), E("write16", f_write16), E("write32", f_write32), E("write64", f_write64), E("write", f_write),
    E("read_bytes", f_read_bytes), E("write_bytes", f_write_bytes), E("read_cstring", f_read_cstring), E("fill", f_fill), E("copy", f_copy),
};
const int mmu_api_count = (int)(sizeof mmu_api_entries / sizeof mmu_api_entries[0]);

void  mm_parse_flags(const char *s, MFlags *F, KcPos pos, const char *who) { parse_flags(s, F, pos, who); }
void *mm_need_handle(KcValue *a, int n, int i, int sub, KcPos pos, const char *who) { return need_h(a, n, i, sub, pos, who); }
void  mm_oset(KcValue o, const char *k, KcValue v) { oset(o, k, v); }
