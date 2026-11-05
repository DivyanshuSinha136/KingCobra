/* KingCobra std.mmu — the page-table engine.
 *
 * Generic over architecture: every function here works on neutral Pte values and the arch layer
 * (mmu_arch.c) does the bit-twiddling. Tables live inside the simulated physical memory, exactly
 * where real hardware would find them, so a script (or the TLB model) sees real page-table bytes.
 *
 *   walk      hardware-style multi-level walk with a per-level trace
 *   map       install a leaf at any legal level, building intermediate tables on demand
 *   unmap     remove a range; splits large pages that straddle the edge; frees emptied tables
 *   protect   change permissions/cache of a range (splitting as needed); COW pages stay read-only
 *   iterate   ordered visit of every leaf (and special non-present entry) in a range
 *   dump / mappings / verify / stats   diagnostics
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

/* ------------------------------------------------------------------ entry I/O */

bool mmu_rd_pte(MmuSpace *S, uint64_t table_pa, int idx, uint64_t *raw) {
    return mmu_phys_read_u(S->phys, table_pa + (uint64_t)idx * (uint64_t)S->A->pte_size, S->A->pte_size, raw);
}
bool mmu_wr_pte(MmuSpace *S, uint64_t table_pa, int idx, uint64_t raw) {
    return mmu_phys_write_u(S->phys, table_pa + (uint64_t)idx * (uint64_t)S->A->pte_size, S->A->pte_size, raw);
}

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}

static bool alloc_table(MmuSpace *S, uint64_t *pa, char *err, size_t errn) {
    if (!mmu_phys_alloc(S->phys, 1, 1, pa)) { seterr(err, errn, "out of physical memory while allocating a page table"); return false; }
    S->n_tables++;
    return true;
}

bool mmu_space_create(MmuSpace *S, char *err, size_t errn) {
    if (S->A->pte_size == 4 && S->phys->size > ((uint64_t)1 << 32)) {
        seterr(err, errn, "%s uses 32-bit table entries: at most 4 GiB of physical memory is supported (PAE/PSE-36 are not modeled)", S->A->name);
        return false;
    }
    uint64_t pa;
    if (!alloc_table(S, &pa, err, errn)) return false;
    S->root[0] = pa; S->has_root[0] = true;
    return true;
}

bool mmu_root_for(MmuSpace *S, uint64_t va, bool create, uint64_t *root_pa) {
    int h = mmu_va_half(S->A, va);
    if (!S->has_root[h]) {
        if (!create) return false;
        uint64_t pa; if (!alloc_table(S, &pa, NULL, 0)) return false;
        S->root[h] = pa; S->has_root[h] = true;
    }
    *root_pa = S->root[h];
    return true;
}

bool mmu_range_ok(const MmuSpace *S, uint64_t va, uint64_t len, uint64_t *last_out, char *err, size_t errn) {
    const MmuArch *A = S->A;
    if (len == 0) { seterr(err, errn, "length must be > 0"); return false; }
    uint64_t last = va + (len - 1);
    if (last < va) { seterr(err, errn, "range wraps around the 64-bit address space"); return false; }
    if (!mmu_is_canonical(A, va) || !mmu_is_canonical(A, last)) { seterr(err, errn, "range touches non-canonical addresses on %s", A->name); return false; }
    if (A->pte_size == 8 && (va >> 63) != (last >> 63)) { seterr(err, errn, "range crosses the non-canonical hole"); return false; }
    if (last_out) *last_out = last;
    return true;
}

/* ------------------------------------------------------------------ walk */

static void fail(WalkResult *w, FaultKind k, int level, uint64_t raw) {
    w->ok = false; w->fault.kind = k; w->fault.level = level; w->fault.raw = raw;
}

void mmu_walk(MmuSpace *S, uint64_t va, WalkResult *w) {
    const MmuArch *A = S->A;
    memset(w, 0, sizeof *w);
    w->fault.va = va;
    if (!mmu_is_canonical(A, va)) { fail(w, FK_NONCANONICAL, A->levels - 1, 0); return; }
    int half = mmu_va_half(A, va);
    if (!S->has_root[half]) { fail(w, FK_NOT_PRESENT, A->levels - 1, 0); return; }
    uint64_t table = S->root[half];
    int mask = mmu_entries(A) - 1;
    for (int L = A->levels - 1; L >= 0; L--) {
        int idx = (int)((va >> mmu_shift(A, L)) & (uint64_t)mask);
        uint64_t raw;
        if (!mmu_rd_pte(S, table, idx, &raw)) { fail(w, FK_BUS, L, 0); return; }
        Pte p; mmu_pte_decode(A, L, raw, &p);
        WalkStep *st = &w->steps[w->nsteps++];
        st->level = L; st->table_pa = table; st->index = idx; st->raw = raw; st->kind = p.kind;
        switch (p.kind) {
        case PK_INVALID: fail(w, FK_NOT_PRESENT, L, raw); return;
        case PK_SPECIAL:
            fail(w, FK_NOT_PRESENT, L, raw);
            w->special = true; w->sp = p; w->fault.swapped = (p.sp_type == SP_SWAP);
            w->pte_pa = table + (uint64_t)idx * (uint64_t)A->pte_size; w->level = L;
            return;
        case PK_BAD: fail(w, FK_RESERVED, L, raw); return;
        case PK_TABLE:
            w->t_ro |= p.t_ro; w->t_nouser |= p.t_nouser; w->t_xn_u |= p.t_xn_u; w->t_xn_p |= p.t_xn_p;
            table = p.pa;
            break;
        case PK_LEAF:
            w->ok = true; w->level = L; w->leaf = p; w->raw = raw;
            w->pte_pa = table + (uint64_t)idx * (uint64_t)A->pte_size;
            w->page_va = va & ~(mmu_page_size(A, L) - 1);
            return;
        }
    }
    fail(w, FK_RESERVED, 0, 0);   /* unreachable: level 0 is always a leaf or an error */
}

bool mmu_pt_locate(MmuSpace *S, uint64_t va, uint64_t *table_pa, int *idx_out, int *level) {
    WalkResult w; mmu_walk(S, va, &w);
    if (!w.ok && !w.special) return false;
    const WalkStep *st = &w.steps[w.nsteps - 1];
    *table_pa = st->table_pa; *idx_out = st->index; *level = st->level;
    return true;
}

/* ------------------------------------------------------------------ release / free */

void mmu_pt_release_leaf(MmuSpace *S, const Pte *pte, int level) {
    const MmuArch *A = S->A;
    if (pte->kind == PK_LEAF) {
        if (pte->sw & SW_OWN) {
            uint64_t n = (uint64_t)1 << (A->idx_bits * level);
            for (uint64_t i = 0; i < n; i++) mmu_phys_ref_dec(S->phys, pte->pa + (i << MMU_FRAME_SHIFT));
        }
    } else if (pte->kind == PK_SPECIAL) {
        if (pte->sp_type == SP_SWAP) mmu_swap_free(S->phys, (uint32_t)(pte->pa >> MMU_FRAME_SHIFT));
        else if (pte->sp_own) mmu_phys_ref_dec(S->phys, pte->pa);
    }
}

static void free_subtree(MmuSpace *S, uint64_t table_pa, int L) {
    const MmuArch *A = S->A;
    int n = mmu_entries(A);
    for (int i = 0; i < n; i++) {
        uint64_t raw; if (!mmu_rd_pte(S, table_pa, i, &raw) || raw == 0) continue;
        Pte p; mmu_pte_decode(A, L, raw, &p);
        if (p.kind == PK_TABLE && L > 0) free_subtree(S, p.pa, L - 1);
        else if (p.kind == PK_LEAF || p.kind == PK_SPECIAL) mmu_pt_release_leaf(S, &p, L);
    }
    mmu_phys_ref_dec(S->phys, table_pa);
    if (S->n_tables) S->n_tables--;
}

void mmu_pt_destroy(MmuSpace *S) {
    for (int h = 0; h < 2; h++) if (S->has_root[h]) { free_subtree(S, S->root[h], S->A->levels - 1); S->has_root[h] = false; }
}

/* ------------------------------------------------------------------ iteration */

typedef struct { PtLeafFn fn; void *ud; uint64_t lo, hi; bool special, stop; } IterCtx;

static void iter_table(MmuSpace *S, IterCtx *c, uint64_t table_pa, int L, int half, uint64_t base_low) {
    const MmuArch *A = S->A;
    int n = mmu_entries(A), shift = mmu_shift(A, L);
    uint64_t span = (uint64_t)1 << shift;
    for (int i = 0; i < n && !c->stop; i++) {
        uint64_t vs = mmu_make_va(A, half, base_low | ((uint64_t)i << shift));
        uint64_t ve = vs + (span - 1);
        if (ve < c->lo || vs > c->hi) continue;
        uint64_t raw; if (!mmu_rd_pte(S, table_pa, i, &raw) || raw == 0) continue;
        Pte p; mmu_pte_decode(A, L, raw, &p);
        if (p.kind == PK_TABLE && L > 0) { iter_table(S, c, p.pa, L - 1, half, base_low | ((uint64_t)i << shift)); continue; }
        if (p.kind == PK_LEAF || (p.kind == PK_SPECIAL && c->special)) {
            PtLeaf lf = { vs, L, table_pa + (uint64_t)i * (uint64_t)A->pte_size, raw, p };
            if (!c->fn(S, c->ud, &lf)) c->stop = true;
        }
    }
}

bool mmu_pt_iter(MmuSpace *S, uint64_t lo, uint64_t hi, PtLeafFn fn, void *ud, bool special) {
    IterCtx c = { fn, ud, lo, hi, special, false };
    for (int h = 0; h < 2 && !c.stop; h++) {
        if (!S->has_root[h]) continue;
        iter_table(S, &c, S->root[h], S->A->levels - 1, h, 0);
    }
    return !c.stop;
}

/* ------------------------------------------------------------------ split */

static bool split_entry(MmuSpace *S, uint64_t table_pa, int idx, int L, const Pte *pte, char *err, size_t errn) {
    const MmuArch *A = S->A;
    uint64_t nt; if (!alloc_table(S, &nt, err, errn)) return false;
    uint64_t span = mmu_page_size(A, L - 1);
    Pte c = *pte;
    int n = mmu_entries(A);
    for (int j = 0; j < n; j++) {
        c.pa = pte->pa + (uint64_t)j * span;
        uint64_t raw = mmu_pte_encode_leaf(A, L - 1, &c, err, errn);
        if (!raw) { mmu_phys_ref_dec(S->phys, nt); S->n_tables--; return false; }
        mmu_wr_pte(S, nt, j, raw);
    }
    mmu_wr_pte(S, table_pa, idx, mmu_pte_encode_table(A, nt));
    S->n_splits++;
    return true;
}

bool mmu_pt_split(MmuSpace *S, uint64_t va, char *err, size_t errn) {
    uint64_t tp; int idx, L;
    if (!mmu_pt_locate(S, va, &tp, &idx, &L)) { seterr(err, errn, "address is not mapped"); return false; }
    uint64_t raw; mmu_rd_pte(S, tp, idx, &raw);
    Pte p; mmu_pte_decode(S->A, L, raw, &p);
    if (p.kind != PK_LEAF) { seterr(err, errn, "address is not mapped"); return false; }
    if (L == 0) { seterr(err, errn, "already a base-size page"); return false; }
    bool ok = split_entry(S, tp, idx, L, &p, err, errn);
    if (ok) mmu_tlb_shootdown(S, va & ~(mmu_page_size(S->A, L) - 1), mmu_page_size(S->A, L));
    return ok;
}

/* ------------------------------------------------------------------ map */

bool mmu_pt_map(MmuSpace *S, uint64_t va, int level, uint64_t raw_leaf, bool force, char *err, size_t errn) {
    const MmuArch *A = S->A;
    if (level < 0 || level >= A->levels || (level > 0 && !(A->leaf_mask & (1u << level)))) { seterr(err, errn, "%s cannot map %llu-byte pages at that level", A->name, (unsigned long long)mmu_page_size(A, level < 0 ? 0 : level)); return false; }
    uint64_t ps = mmu_page_size(A, level);
    if (!mmu_is_canonical(A, va)) { seterr(err, errn, "non-canonical address 0x%llx for %s", (unsigned long long)va, A->name); return false; }
    if (va & (ps - 1)) { seterr(err, errn, "virtual address 0x%llx is not aligned to the %llu-byte page size", (unsigned long long)va, (unsigned long long)ps); return false; }
    uint64_t table;
    if (!mmu_root_for(S, va, true, &table)) { seterr(err, errn, "out of physical memory while allocating a page table"); return false; }
    int mask = mmu_entries(A) - 1;
    bool replaced = false;
    for (int L = A->levels - 1; L > level; L--) {
        int idx = (int)((va >> mmu_shift(A, L)) & (uint64_t)mask);
        uint64_t raw; mmu_rd_pte(S, table, idx, &raw);
        Pte p; mmu_pte_decode(A, L, raw, &p);
        if (p.kind == PK_TABLE) { table = p.pa; continue; }
        if (p.kind == PK_LEAF || p.kind == PK_SPECIAL || p.kind == PK_BAD) {
            if (!force) { seterr(err, errn, "overlaps an existing %s mapping at 0x%llx", p.kind == PK_LEAF ? "larger" : "special", (unsigned long long)(va & ~(mmu_page_size(A, L) - 1))); return false; }
            if (p.kind != PK_BAD) mmu_pt_release_leaf(S, &p, L);
            replaced = true;
        }
        uint64_t nt; if (!alloc_table(S, &nt, err, errn)) return false;
        mmu_wr_pte(S, table, idx, mmu_pte_encode_table(A, nt));
        table = nt;
    }
    int idx = (int)((va >> mmu_shift(A, level)) & (uint64_t)mask);
    uint64_t old; mmu_rd_pte(S, table, idx, &old);
    Pte p; mmu_pte_decode(A, level, old, &p);
    if (p.kind != PK_INVALID) {
        if (!force) {
            seterr(err, errn, p.kind == PK_TABLE ? "range is already covered by smaller pages (unmap them first)" : "address 0x%llx is already mapped", (unsigned long long)va);
            return false;
        }
        if (p.kind == PK_TABLE && level > 0) free_subtree(S, p.pa, level - 1);
        else if (p.kind == PK_LEAF || p.kind == PK_SPECIAL) mmu_pt_release_leaf(S, &p, level);
        replaced = true;
    }
    mmu_wr_pte(S, table, idx, raw_leaf);
    if (replaced) mmu_tlb_shootdown(S, va, ps);
    return true;
}

bool mmu_pt_map_pte(MmuSpace *S, uint64_t va, int level, const Pte *p, bool force, char *err, size_t errn) {
    uint64_t raw = mmu_pte_encode_leaf(S->A, level, p, err, errn);
    if (!raw) return false;
    return mmu_pt_map(S, va, level, raw, force, err, errn);
}

/* ------------------------------------------------------------------ unmap / protect (shared recursion) */

typedef struct {
    MmuSpace *S; uint64_t lo, hi; int half;
    bool protect; int set_mask, set_val; bool set_cache; int cache; bool clear_ad;
    int64_t count; bool failed; char *err; size_t errn;
} RangeCtx;

static bool apply_protect(RangeCtx *c, uint64_t table_pa, int idx, int L, const Pte *p) {
    const MmuArch *A = c->S->A;
    Pte q = *p;
    if (c->set_mask & PROT_R) q.r = (c->set_val & PROT_R) != 0;
    if (c->set_mask & PROT_W) q.w = (c->set_val & PROT_W) != 0;
    if (c->set_mask & PROT_X) q.x = (c->set_val & PROT_X) != 0;
    if (c->set_mask & PROT_U) q.user = (c->set_val & PROT_U) != 0;
    if (c->set_mask & PROT_G) q.global = (c->set_val & PROT_G) != 0;
    if (c->set_cache) q.cache = (uint8_t)c->cache;
    if (q.sw & SW_COW) q.w = false;                      /* a shared COW page must keep faulting on write */
    if (c->clear_ad) { q.accessed = false; q.dirty = false; }
    if (A->fam == FAM_RV32 || A->fam == FAM_RV64) { if (q.w) q.r = true; }
    uint64_t raw = mmu_pte_encode_leaf(A, L, &q, c->err, c->errn);
    if (!raw) { c->failed = true; return false; }
    mmu_wr_pte(c->S, table_pa, idx, raw);
    return true;
}

static bool range_rec(RangeCtx *c, uint64_t table_pa, int L, uint64_t base_low) {
    /* returns true when the table holds no entries any more */
    MmuSpace *S = c->S; const MmuArch *A = S->A;
    int n = mmu_entries(A), shift = mmu_shift(A, L);
    uint64_t span = (uint64_t)1 << shift;
    bool remaining = false;
    for (int i = 0; i < n && !c->failed; i++) {
        uint64_t raw; if (!mmu_rd_pte(S, table_pa, i, &raw) || raw == 0) continue;
        uint64_t vs = mmu_make_va(A, c->half, base_low | ((uint64_t)i << shift));
        uint64_t ve = vs + (span - 1);
        if (ve < c->lo || vs > c->hi) { remaining = true; continue; }
        Pte p; mmu_pte_decode(A, L, raw, &p);
        bool inside = vs >= c->lo && ve <= c->hi;
        if (p.kind == PK_BAD) { if (!c->protect && inside) { mmu_wr_pte(S, table_pa, i, 0); } else remaining = true; continue; }
        if (p.kind == PK_SPECIAL) {
            if (!c->protect) { mmu_pt_release_leaf(S, &p, L); mmu_wr_pte(S, table_pa, i, 0); c->count++; }
            else remaining = true;
            continue;
        }
        if (p.kind == PK_LEAF) {
            if (inside) {
                if (c->protect) { if (!apply_protect(c, table_pa, i, L, &p)) break; remaining = true; c->count += (int64_t)1 << (A->idx_bits * L); }
                else { mmu_pt_release_leaf(S, &p, L); mmu_wr_pte(S, table_pa, i, 0); c->count += (int64_t)1 << (A->idx_bits * L); }
                continue;
            }
            if (!split_entry(S, table_pa, i, L, &p, c->err, c->errn)) { c->failed = true; break; }
            mmu_rd_pte(S, table_pa, i, &raw); mmu_pte_decode(A, L, raw, &p);     /* now a table */
        }
        if (p.kind == PK_TABLE && L > 0) {
            bool empty = range_rec(c, p.pa, L - 1, base_low | ((uint64_t)i << shift));
            if (empty) { mmu_phys_ref_dec(S->phys, p.pa); if (S->n_tables) S->n_tables--; mmu_wr_pte(S, table_pa, i, 0); }
            else remaining = true;
        }
    }
    return !remaining && !c->failed;
}

static int64_t range_op(MmuSpace *S, uint64_t va, uint64_t len, RangeCtx *c, char *err, size_t errn) {
    uint64_t last;
    if (!mmu_range_ok(S, va, len, &last, err, errn)) return -1;
    int half = mmu_va_half(S->A, va);
    c->S = S; c->lo = va; c->hi = last; c->half = half; c->err = err; c->errn = errn;
    if (!S->has_root[half]) return 0;
    range_rec(c, S->root[half], S->A->levels - 1, 0);
    if (c->failed) { if (c->count > 0) mmu_tlb_shootdown(S, va, len); return -1; }
    if (c->count > 0) mmu_tlb_shootdown(S, va, len);
    return c->count;
}

int64_t mmu_pt_unmap(MmuSpace *S, uint64_t va, uint64_t len, char *err, size_t errn) {
    RangeCtx c; memset(&c, 0, sizeof c);
    return range_op(S, va, len, &c, err, errn);
}

int64_t mmu_pt_protect(MmuSpace *S, uint64_t va, uint64_t len, int set_mask, int set_val, bool set_cache, int cache,
                       bool clear_ad, char *err, size_t errn) {
    RangeCtx c; memset(&c, 0, sizeof c);
    c.protect = true; c.set_mask = set_mask; c.set_val = set_val; c.set_cache = set_cache; c.cache = cache; c.clear_ad = clear_ad;
    return range_op(S, va, len, &c, err, errn);
}

bool mmu_pt_rewrite_leaf(MmuSpace *S, const PtLeaf *lf, uint64_t new_raw) {
    return mmu_phys_write_u(S->phys, lf->pte_pa, S->A->pte_size, new_raw);
}

/* ------------------------------------------------------------------ formatting helpers */

void mmu_flags_str(char *out, size_t n, const Pte *p) {
    size_t o = 0; out[0] = 0;
#define ADD(s) do { o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "," : "", s); } while (0)
    if (p->r) ADD("r");
    if (p->w) ADD("w");
    if (p->x) ADD("x");
    if (p->user) ADD("u");
    if (p->global) ADD("g");
    if (p->cache != MC_WB) ADD(MMU_CACHE_NAMES[p->cache]);
    if (p->sw & SW_COW) ADD("cow");
    if (p->sw & SW_OWN) ADD("own");
#undef ADD
}

static void fmt_size(char *out, size_t n, uint64_t v) {
    if (v >= ((uint64_t)1 << 30) && !(v & (((uint64_t)1 << 30) - 1))) snprintf(out, n, "%lluG", (unsigned long long)(v >> 30));
    else if (v >= ((uint64_t)1 << 20) && !(v & (((uint64_t)1 << 20) - 1))) snprintf(out, n, "%lluM", (unsigned long long)(v >> 20));
    else if (v >= 1024 && !(v & 1023)) snprintf(out, n, "%lluK", (unsigned long long)(v >> 10));
    else snprintf(out, n, "%lluB", (unsigned long long)v);
}

/* ------------------------------------------------------------------ coalesced runs (dump / mappings) */

typedef struct { uint64_t va, pa, size; int level; Pte first; bool a, d; uint64_t leaves; } Run;
typedef struct { Run *r; int n, cap; } RunList;

static bool same_attrs(const Pte *a, const Pte *b) {
    return a->r == b->r && a->w == b->w && a->x == b->x && a->user == b->user && a->global == b->global &&
           a->cache == b->cache && a->sw == b->sw;
}

static bool run_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    (void)S;
    RunList *rl = ud;
    uint64_t sz = mmu_page_size(S->A, lf->level);
    if (rl->n) {
        Run *l = &rl->r[rl->n - 1];
        if (l->level == lf->level && l->va + l->size == lf->va && l->pa + l->size == lf->pte.pa && same_attrs(&l->first, &lf->pte)) {
            l->size += sz; l->a |= lf->pte.accessed; l->d |= lf->pte.dirty; l->leaves++;
            return true;
        }
    }
    if (rl->n == rl->cap) { rl->cap = rl->cap ? rl->cap * 2 : 64; rl->r = kc_realloc(rl->r, sizeof(Run) * (size_t)rl->cap); }
    Run *r = &rl->r[rl->n++];
    r->va = lf->va; r->pa = lf->pte.pa; r->size = sz; r->level = lf->level; r->first = lf->pte;
    r->a = lf->pte.accessed; r->d = lf->pte.dirty; r->leaves = 1;
    return true;
}

char *mmu_pt_dump(MmuSpace *S, uint64_t lo, uint64_t hi) {
    RunList rl = {0};
    mmu_pt_iter(S, lo, hi, run_cb, &rl, false);
    size_t cap = 256 + (size_t)rl.n * 140, len = 0;
    char *out = kc_malloc(cap); out[0] = 0;
    len += (size_t)snprintf(out + len, cap - len, "%-18s %-18s %6s  %-3s %-5s %s\n", "virtual start", "virtual end (excl)", "size", "prm", "attrs", "physical");
    for (int i = 0; i < rl.n; i++) {
        Run *r = &rl.r[i];
        char sz[24], ps[16]; fmt_size(sz, sizeof sz, r->size); fmt_size(ps, sizeof ps, mmu_page_size(S->A, r->level));
        const Pte *p = &r->first;
        char attrs[16]; snprintf(attrs, sizeof attrs, "%c%c%c%c%c", p->user ? 'u' : 's', p->global ? 'g' : '-', r->a ? 'A' : '-', r->d ? 'D' : '-',
                                 (p->sw & SW_COW) ? 'C' : (p->sw & SW_OWN) ? 'o' : '-');
        char line[200];
        snprintf(line, sizeof line, "0x%016llx 0x%016llx %6s  %c%c%c %-5s pa=0x%llx pg=%s %s%s\n",
                 (unsigned long long)r->va, (unsigned long long)(r->va + r->size), sz,
                 p->r ? 'r' : '-', p->w ? 'w' : '-', p->x ? 'x' : '-', attrs, (unsigned long long)r->pa, ps,
                 MMU_CACHE_NAMES[p->cache], "");
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) { cap = (len + ll + 1) * 2; out = kc_realloc(out, cap); }
        memcpy(out + len, line, ll + 1); len += ll;
    }
    if (!rl.n) len += (size_t)snprintf(out + len, cap - len, "(no mappings)\n");
    free(rl.r);
    return out;
}

KcValue mmu_pt_mappings(MmuSpace *S, uint64_t lo, uint64_t hi) {
    RunList rl = {0};
    mmu_pt_iter(S, lo, hi, run_cb, &rl, false);
    KcValue arr = kc_array_new();
    for (int i = 0; i < rl.n; i++) {
        Run *r = &rl.r[i]; const Pte *p = &r->first;
        KcValue o = kc_object_new();
        char fl[64]; mmu_flags_str(fl, sizeof fl, p);
        kc_object_set(o, "va", 2, kc_int((int64_t)r->va));
        kc_object_set(o, "size", 4, kc_int((int64_t)r->size));
        kc_object_set(o, "pa", 2, kc_int((int64_t)r->pa));
        kc_object_set(o, "page_size", 9, kc_int((int64_t)mmu_page_size(S->A, r->level)));
        kc_object_set(o, "level", 5, kc_int(r->level));
        kc_object_set(o, "flags", 5, kc_string(fl));
        kc_object_set(o, "r", 1, kc_bool(p->r)); kc_object_set(o, "w", 1, kc_bool(p->w));
        kc_object_set(o, "x", 1, kc_bool(p->x)); kc_object_set(o, "user", 4, kc_bool(p->user));
        kc_object_set(o, "global", 6, kc_bool(p->global));
        kc_object_set(o, "accessed", 8, kc_bool(r->a)); kc_object_set(o, "dirty", 5, kc_bool(r->d));
        kc_object_set(o, "cache", 5, kc_string(MMU_CACHE_NAMES[p->cache]));
        kc_object_set(o, "cow", 3, kc_bool((p->sw & SW_COW) != 0)); kc_object_set(o, "own", 3, kc_bool((p->sw & SW_OWN) != 0));
        kc_array_push(arr, o);
    }
    free(rl.r);
    return arr;
}

/* ------------------------------------------------------------------ stats */

typedef struct { uint64_t *pages; uint64_t swapped, protnone, owned; } StatCtx;
static bool stat_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    (void)S;
    StatCtx *c = ud;
    if (lf->pte.kind == PK_LEAF) { c->pages[lf->level]++; if (lf->pte.sw & SW_OWN) c->owned++; }
    else if (lf->pte.sp_type == SP_SWAP) c->swapped++;
    else c->protnone++;
    return true;
}
static void count_tables(MmuSpace *S, uint64_t table, int L, uint64_t *tables) {
    tables[L]++;
    if (L == 0) return;
    for (int i = 0; i < mmu_entries(S->A); i++) {
        uint64_t raw; if (!mmu_rd_pte(S, table, i, &raw) || raw == 0) continue;
        Pte p; mmu_pte_decode(S->A, L, raw, &p);
        if (p.kind == PK_TABLE) count_tables(S, p.pa, L - 1, tables);
    }
}
void mmu_pt_stats(MmuSpace *S, uint64_t *pages_by_level, uint64_t *tables_by_level, uint64_t *swapped, uint64_t *protnone, uint64_t *owned) {
    memset(pages_by_level, 0, sizeof(uint64_t) * MMU_MAX_LEVELS);
    memset(tables_by_level, 0, sizeof(uint64_t) * MMU_MAX_LEVELS);
    StatCtx c = { pages_by_level, 0, 0, 0 };
    mmu_pt_iter(S, 0, ~(uint64_t)0, stat_cb, &c, true);
    for (int h = 0; h < 2; h++) if (S->has_root[h]) count_tables(S, S->root[h], S->A->levels - 1, tables_by_level);
    *swapped = c.swapped; *protnone = c.protnone; *owned = c.owned;
}

/* ------------------------------------------------------------------ verify */

typedef struct { KcValue out; uint64_t *seen; int nseen, capseen; int issues; } VerCtx;

static void issue(VerCtx *v, const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (v->issues < 200) kc_array_push(v->out, kc_string(b));
    v->issues++;
}

static void verify_table(MmuSpace *S, VerCtx *v, uint64_t table, int L, int half, uint64_t base_low, int depth) {
    const MmuArch *A = S->A;
    int n = mmu_entries(A), shift = mmu_shift(A, L);
    for (int i = 0; i < n; i++) {
        uint64_t raw; if (!mmu_rd_pte(S, table, i, &raw)) { issue(v, "L%d table 0x%llx: unreadable entry %d", L, (unsigned long long)table, i); continue; }
        if (raw == 0) continue;
        uint64_t va = mmu_make_va(A, half, base_low | ((uint64_t)i << shift));
        Pte p; mmu_pte_decode(A, L, raw, &p);
        switch (p.kind) {
        case PK_INVALID: if (raw & 1) issue(v, "L%d entry %d at va 0x%llx: unclassifiable", L, i, (unsigned long long)va); break;
        case PK_BAD: issue(v, "L%d entry %d at va 0x%llx: malformed (%s) raw=0x%llx", L, i, (unsigned long long)va, p.bad_reason, (unsigned long long)raw); break;
        case PK_SPECIAL:
            if (p.sp_type == SP_SWAP) {
                uint32_t slot = (uint32_t)(p.pa >> MMU_FRAME_SHIFT);
                if (slot >= S->phys->swap_slots || !S->phys->swap_used[slot]) issue(v, "va 0x%llx: swapped to slot %u which is not allocated", (unsigned long long)va, slot);
            } else if (!mmu_phys_frame_live(S->phys, p.pa)) issue(v, "va 0x%llx: PROT_NONE page refers to unallocated frame 0x%llx", (unsigned long long)va, (unsigned long long)p.pa);
            break;
        case PK_TABLE:
            if (L == 0) { issue(v, "level-0 entry at va 0x%llx is a table pointer", (unsigned long long)va); break; }
            if (!mmu_phys_in_range(S->phys, p.pa, MMU_FRAME_SIZE)) { issue(v, "L%d entry at va 0x%llx points to table 0x%llx outside physical memory", L, (unsigned long long)va, (unsigned long long)p.pa); break; }
            if (!mmu_phys_frame_live(S->phys, p.pa)) issue(v, "L%d entry at va 0x%llx points to table frame 0x%llx that is not allocated", L, (unsigned long long)va, (unsigned long long)p.pa);
            for (int k = 0; k < v->nseen; k++) if (v->seen[k] == p.pa) { issue(v, "warn: table frame 0x%llx is referenced more than once (va 0x%llx)", (unsigned long long)p.pa, (unsigned long long)va); goto next; }
            if (v->nseen == v->capseen) { v->capseen = v->capseen ? v->capseen * 2 : 64; v->seen = kc_realloc(v->seen, sizeof(uint64_t) * (size_t)v->capseen); }
            if (v->nseen < 8192) v->seen[v->nseen++] = p.pa;
            if (depth < 8) verify_table(S, v, p.pa, L - 1, half, base_low | ((uint64_t)i << shift), depth + 1);
            next: break;
        case PK_LEAF: {
            uint64_t ps = mmu_page_size(A, L);
            if (!mmu_phys_in_range(S->phys, p.pa, ps) && !mmu_phys_find_device(S->phys, p.pa)) issue(v, "va 0x%llx (%llu-byte page) maps physical 0x%llx beyond the end of RAM", (unsigned long long)va, (unsigned long long)ps, (unsigned long long)p.pa);
            if ((p.sw & SW_OWN) && !mmu_phys_frame_live(S->phys, p.pa)) issue(v, "va 0x%llx: owned page refers to free frame 0x%llx", (unsigned long long)va, (unsigned long long)p.pa);
            if (p.w && p.x && A->has_nx) issue(v, "warn: va 0x%llx is writable and executable (W^X violation)", (unsigned long long)va);
            if (p.dirty && !p.accessed && A->fam != FAM_ARM64) issue(v, "warn: va 0x%llx has the dirty bit set but not accessed", (unsigned long long)va);
            break; }
        }
    }
}

KcValue mmu_pt_verify(MmuSpace *S) {
    VerCtx v = { kc_array_new(), NULL, 0, 0, 0 };
    for (int h = 0; h < 2; h++) {
        if (!S->has_root[h]) continue;
        if (!mmu_phys_frame_live(S->phys, S->root[h])) issue(&v, "root table frame 0x%llx is not allocated", (unsigned long long)S->root[h]);
        verify_table(S, &v, S->root[h], S->A->levels - 1, h, 0, 0);
    }
    if (v.issues > 200) { char b[64]; snprintf(b, sizeof b, "... and %d more", v.issues - 200); kc_array_push(v.out, kc_string(b)); }
    free(v.seen);
    return v.out;
}
