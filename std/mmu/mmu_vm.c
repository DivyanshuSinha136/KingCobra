/* KingCobra std.mmu — an OS-style virtual-memory manager on top of the page-table engine.
 *
 *   VMAs            sorted region list: prot, growsdown stacks, locked, huge, guard, shared memory
 *   demand paging   not-present faults inside a VMA allocate a zeroed frame (or a 2 MiB huge page)
 *   copy-on-write   fork() shares frames read-only; the first write copies (or just re-enables
 *                   write when the faulting space is the last owner)
 *   swap + reclaim  a clock (second-chance) scan evicts cold anonymous pages to swap slots; the
 *                   page tables hold swap entries and a later touch swaps the page back in
 *   shared memory   shm objects mapped into several spaces
 *   PROT_NONE       kept as special non-present entries so contents survive mprotect round trips
 *
 * The built-in fault handler only ever touches pages the VM layer owns (SW_OWN) or addresses
 * covered by a VMA, so raw map()/unmap() users keep full manual control.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

#define PG ((uint64_t)MMU_FRAME_SIZE)
static inline uint64_t pg_down(uint64_t v) { return v & ~(PG - 1); }
static inline bool add_ovf(uint64_t a, uint64_t b) { return a + b < a; }

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ space lifetime */

static int g_next_space_id = 1;
static uint32_t g_next_asid[MMU_ARCH_COUNT];

MmuSpace *mmu_space_new(const MmuArch *A, MmuPhys *phys, const char *name, const char *opts, int asid_req, char *err, size_t errn) {
    MmuSpace *S = kc_malloc(sizeof *S);
    memset(S, 0, sizeof *S);
    S->id = g_next_space_id++; S->A = A; S->phys = phys; phys->refs++;
    snprintf(S->name, sizeof S->name, "%s", name && *name ? name : "space");
    MmuCpu *h = mmu_cpu_create(A, S->name, err, errn);
    if (!h) { phys->refs--; free(S); return NULL; }
    h->private_cpu = true;
    int asid = -1; bool given = false;
    if (!mmu_cpu_parse_opts(h, opts, &asid, &given, err, errn)) { mmu_cpu_destroy(h); phys->refs--; free(S); return NULL; }
    if (asid_req >= 0) { asid = asid_req; given = true; }
    if (A->asid_bits == 0) S->asid = 0;
    else if (given) S->asid = (uint32_t)asid;
    else {
        uint32_t lim = ((uint32_t)1 << A->asid_bits) - 1;
        if (g_next_asid[A->id] == 0) g_next_asid[A->id] = 1;
        if (g_next_asid[A->id] > lim) { seterr(err, errn, "ASID space exhausted for %s; pass asid=N to reuse one deliberately", A->name); mmu_cpu_destroy(h); phys->refs--; free(S); return NULL; }
        S->asid = g_next_asid[A->id]++;
    }
    if (!mmu_space_create(S, err, errn)) { mmu_cpu_destroy(h); phys->refs--; free(S); return NULL; }
    S->home = h; h->cur = S; S->alive = true;
    S->mmap_cursor = A->pte_size == 4 ? 0x40000000ull : mmu_user_top(A) / 4 * 3 / 2;
    S->mmap_cursor &= ~((uint64_t)0x1FFFFF);
    S->stack_max = (uint64_t)64 << 20;
    return S;
}

void mmu_vm_destroy(MmuSpace *S) {
    if (!S->alive) return;
    S->alive = false;
    mmu_tlb_shootdown_all(S);
    for (int i = 0; i < MMU_MAX_CPUS; i++) if (MMU_CPUS[i] && MMU_CPUS[i]->cur == S) MMU_CPUS[i]->cur = NULL;
    mmu_pt_destroy(S);
    for (int i = 0; i < S->nvma; i++) if (S->vmas[i].shm) mmu_shm_release(S->vmas[i].shm);
    free(S->vmas); S->vmas = NULL; S->nvma = S->cap_vma = 0;
    if (S->home) { mmu_cpu_destroy(S->home); S->home = NULL; }
    mmu_phys_release(S->phys);
}

/* ------------------------------------------------------------------ shared memory objects */

MmuShm *mmu_shm_new(MmuPhys *p, uint64_t len, const char *name) {
    MmuShm *s = kc_malloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->phys = p; p->refs++;
    s->len = (len + PG - 1) & ~(PG - 1);
    s->npages = s->len / PG;
    s->frames = kc_malloc(sizeof(uint64_t) * (size_t)s->npages);
    memset(s->frames, 0, sizeof(uint64_t) * (size_t)s->npages);
    s->refs = 1; s->alive = true;
    snprintf(s->name, sizeof s->name, "%s", name ? name : "shm");
    return s;
}

void mmu_shm_release(MmuShm *s) {
    if (!s || --s->refs > 0) return;
    for (uint64_t i = 0; i < s->npages; i++) if (s->frames[i]) mmu_phys_ref_dec(s->phys, s->frames[i] - 1);
    MmuPhys *p = s->phys;
    free(s->frames); free(s);
    mmu_phys_release(p);
}

/* frames[] stores pa+1 so that physical frame 0 is representable */
static bool shm_page(MmuShm *s, uint64_t idx, uint64_t *pa) {
    if (idx >= s->npages) return false;
    if (!s->frames[idx]) {
        uint64_t f; if (!mmu_phys_alloc(s->phys, 1, 1, &f)) return false;
        s->frames[idx] = f + 1;
    }
    *pa = s->frames[idx] - 1;
    return true;
}

/* ------------------------------------------------------------------ VMA list */

MmuVma *mmu_vma_find(MmuSpace *S, uint64_t va) {
    int lo = 0, hi = S->nvma - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2; MmuVma *v = &S->vmas[mid];
        if (va < v->start) hi = mid - 1; else if (va >= v->end) lo = mid + 1; else return v;
    }
    return NULL;
}

static int vma_lower_bound(MmuSpace *S, uint64_t addr) {      /* first VMA whose end > addr */
    int lo = 0, hi = S->nvma;
    while (lo < hi) { int mid = (lo + hi) / 2; if (S->vmas[mid].end <= addr) lo = mid + 1; else hi = mid; }
    return lo;
}

static void vma_insert_at(MmuSpace *S, int idx, const MmuVma *v) {
    if (S->nvma == S->cap_vma) { S->cap_vma = S->cap_vma ? S->cap_vma * 2 : 16; S->vmas = kc_realloc(S->vmas, sizeof(MmuVma) * (size_t)S->cap_vma); }
    memmove(&S->vmas[idx + 1], &S->vmas[idx], sizeof(MmuVma) * (size_t)(S->nvma - idx));
    S->vmas[idx] = *v; S->nvma++;
    if (v->shm) v->shm->refs++;
}

static void vma_remove_at(MmuSpace *S, int idx) {
    if (S->vmas[idx].shm) mmu_shm_release(S->vmas[idx].shm);
    memmove(&S->vmas[idx], &S->vmas[idx + 1], sizeof(MmuVma) * (size_t)(S->nvma - idx - 1));
    S->nvma--;
}

static void vma_split_at(MmuSpace *S, uint64_t addr) {
    int i = vma_lower_bound(S, addr);
    if (i >= S->nvma) return;
    MmuVma *v = &S->vmas[i];
    if (v->start >= addr || v->end <= addr) return;
    MmuVma up = *v;
    up.start = addr;
    if (v->shm) up.shm_off = v->shm_off + (addr - v->start);
    v->end = addr;
    vma_insert_at(S, i + 1, &up);
}

static void vma_merge_all(MmuSpace *S) {
    for (int i = 0; i + 1 < S->nvma;) {
        MmuVma *a = &S->vmas[i], *b = &S->vmas[i + 1];
        if (a->end == b->start && a->prot == b->prot && a->flags == b->flags && a->cache == b->cache && !strcmp(a->name, b->name) &&
            a->shm == b->shm && (!a->shm || a->shm_off + (a->end - a->start) == b->shm_off)) {
            a->end = b->end; vma_remove_at(S, i + 1);
        } else i++;
    }
}

static bool find_area(MmuSpace *S, uint64_t len, uint64_t align, uint64_t hint, uint64_t *out) {
    uint64_t top = mmu_user_top(S->A), lo = 0x10000;
    uint64_t cand = hint < lo ? lo : hint;
    cand = (cand + align - 1) & ~(align - 1);
    for (int i = vma_lower_bound(S, cand); i < S->nvma; i++) {
        MmuVma *v = &S->vmas[i];
        if (v->start >= top) break;
        if (!add_ovf(cand, len) && cand + len <= v->start) break;
        cand = (v->end + align - 1) & ~(align - 1);
    }
    if (add_ovf(cand, len) || cand + len > top) return false;
    *out = cand;
    return true;
}

void mmu_vm_default_pte(const MmuVma *v, Pte *p) {
    memset(p, 0, sizeof *p);
    p->kind = PK_LEAF;
    p->r = (v->prot & (PROT_R | PROT_W)) != 0;
    p->w = (v->prot & PROT_W) != 0; p->x = (v->prot & PROT_X) != 0;
    p->user = (v->prot & PROT_U) != 0; p->global = (v->prot & PROT_G) != 0;
    p->cache = v->cache; p->sw = SW_OWN;
}

/* ------------------------------------------------------------------ frames with reclaim */

static bool alloc_frame(MmuSpace *S, uint64_t *pa) {
    if (mmu_phys_alloc(S->phys, 1, 1, pa)) return true;
    if (S->phys->swap_slots) { int64_t sc; if (mmu_vm_reclaim(S, 16, &sc) > 0 && mmu_phys_alloc(S->phys, 1, 1, pa)) return true; }
    return false;
}

static bool install(MmuSpace *S, uint64_t va, uint64_t pa, int level, const MmuVma *v, uint32_t extra_sw, bool w_ok, bool dirty, char *err, size_t errn) {
    Pte p; mmu_vm_default_pte(v, &p);
    p.w = p.w && w_ok; p.pa = pa; p.sw |= (uint8_t)extra_sw; p.accessed = true; p.dirty = dirty;
    return mmu_pt_map_pte(S, va, level, &p, false, err, errn);
}

/* ------------------------------------------------------------------ fault handling */

static bool demand_fill(MmuSpace *S, MmuVma *v, uint64_t va, Access acc, char *err, size_t errn) {
    const MmuArch *A = S->A;
    uint64_t page = pg_down(va);
    bool dirty = acc == ACC_WRITE;
    if (v->shm) {
        uint64_t pa; if (!shm_page(v->shm, (v->shm_off + (page - v->start)) / PG, &pa)) { seterr(err, errn, "shared memory page out of range or out of physical memory"); return false; }
        mmu_phys_ref_inc(S->phys, pa);
        if (!install(S, page, pa, 0, v, 0, true, dirty, err, errn)) { mmu_phys_ref_dec(S->phys, pa); return false; }
        S->n_demand++;
        return true;
    }
    if ((v->flags & VF_HUGE) && (A->leaf_mask & 2u)) {
        uint64_t hs = mmu_page_size(A, 1), hb = va & ~(hs - 1);
        if (hb >= v->start && !add_ovf(hb, hs) && hb + hs <= v->end) {
            uint64_t base;
            if (mmu_phys_alloc(S->phys, hs / PG, hs / PG, &base)) {
                char e2[96];
                if (install(S, hb, base, 1, v, 0, true, dirty, e2, sizeof e2)) { S->n_demand++; S->n_huge++; return true; }
                for (uint64_t i = 0; i < hs / PG; i++) mmu_phys_ref_dec(S->phys, base + i * PG);
            }
        }
    }
    uint64_t pa;
    if (!alloc_frame(S, &pa)) { seterr(err, errn, "out of physical memory"); return false; }
    if (!install(S, page, pa, 0, v, 0, true, dirty, err, errn)) { mmu_phys_ref_dec(S->phys, pa); return false; }
    S->n_demand++;
    return true;
}

static bool swap_in(MmuSpace *S, MmuVma *v, uint64_t va, const Pte *sp, uint64_t pte_pa, char *err, size_t errn) {
    (void)pte_pa;
    uint32_t slot = (uint32_t)(sp->pa >> MMU_FRAME_SHIFT);
    uint64_t pa; if (!alloc_frame(S, &pa)) { seterr(err, errn, "out of physical memory during swap-in"); return false; }
    if (!mmu_swap_load(S->phys, slot, pa)) { mmu_phys_ref_dec(S->phys, pa); seterr(err, errn, "swap slot %u is invalid", slot); return false; }
    mmu_swap_free(S->phys, slot);
    /* the entry currently holds the swap marker: replace it */
    Pte p; mmu_vm_default_pte(v, &p);
    p.pa = pg_down(pa); p.accessed = true; p.dirty = false;
    if (sp->sp_cow) { p.sw |= SW_COW; p.w = false; }
    p.global = p.global || sp->sp_global;
    if (!mmu_pt_map_pte(S, pg_down(va), 0, &p, true, err, errn)) { mmu_phys_ref_dec(S->phys, pa); return false; }
    S->n_swapin++; S->phys->swap_ins += 0;
    return true;
}

static bool break_cow(MmuSpace *S, MmuVma *v, uint64_t va, char *err, size_t errn) {
    WalkResult w; mmu_walk(S, va, &w);
    if (!w.ok) return false;
    if (w.level > 0) { if (!mmu_pt_split(S, va, err, errn)) return false; mmu_walk(S, va, &w); if (!w.ok) return false; }
    if (!(w.leaf.sw & SW_COW)) return false;
    Pte p = w.leaf;
    uint64_t old = p.pa;
    bool want_w = v ? (v->prot & PROT_W) != 0 : true;
    if (!want_w) return false;
    if (mmu_phys_refcount(S->phys, old) <= 1) {
        p.sw &= (uint8_t)~SW_COW; p.w = true; p.dirty = true; p.accessed = true;
    } else {
        uint64_t np; if (!alloc_frame(S, &np)) { seterr(err, errn, "out of physical memory during copy-on-write"); return false; }
        mmu_phys_copy_frame(S->phys, np, old);
        p.pa = np; p.sw = (uint8_t)((p.sw & ~SW_COW) | SW_OWN); p.w = true; p.dirty = true; p.accessed = true;
        mmu_phys_ref_dec(S->phys, old);
    }
    uint64_t raw = mmu_pte_encode_leaf(S->A, 0, &p, err, errn);
    if (!raw) return false;
    mmu_wr_pte(S, w.steps[w.nsteps - 1].table_pa, w.steps[w.nsteps - 1].index, raw);
    mmu_tlb_shootdown(S, pg_down(va), PG);
    S->n_cow++;
    return true;
}

bool mmu_vm_fault(MmuCpu *c, MmuSpace *S, Fault *f) {
    (void)c;
    if (f->kind == FK_NONCANONICAL || f->kind == FK_RESERVED || f->kind == FK_BUS || f->kind == FK_NONE) return false;
    char err[160];
    uint64_t va = f->va;
    MmuVma *v = mmu_vma_find(S, va);
    bool have_vmas = S->nvma > 0;

    /* access/dirty fix-ups for software-managed A/D (arm64 without HAFDBS, RISC-V Svade) */
    if (f->kind == FK_ACCESS_FLAG || f->kind == FK_DIRTY) {
        WalkResult w; mmu_walk(S, va, &w);
        if (!w.ok || !(v || (w.leaf.sw & SW_OWN))) return false;
        uint64_t nr = mmu_pte_set_ad(S->A, w.level, w.raw, false, true, f->access == ACC_WRITE);
        mmu_phys_write_u(S->phys, w.pte_pa, S->A->pte_size, nr);
        mmu_tlb_shootdown(S, pg_down(va), PG);
        S->n_ad_fix++;
        return true;
    }

    if (f->kind == FK_PROTECTION) {
        if (f->cow) {
            if (v && !(v->prot & PROT_W)) { f->segv = 2; return false; }
            return break_cow(S, v, va, err, sizeof err);
        }
        if (have_vmas) f->segv = v ? 2 : 1;
        return false;
    }

    /* FK_NOT_PRESENT */
    if (!have_vmas) return false;
    if (!v) {
        /* stack growth: a GROWSDOWN VMA directly above the faulting address */
        int i = vma_lower_bound(S, va);
        if (i < S->nvma && (S->vmas[i].flags & VF_GROWSDOWN) && S->vmas[i].start > va) {
            MmuVma *g = &S->vmas[i];
            uint64_t ns = pg_down(va);
            bool room = (i == 0 || S->vmas[i - 1].end <= ns) && (g->end - ns) <= S->stack_max && ns >= 0x10000;
            if (room) { g->start = ns; v = g; }
        }
        if (!v) { f->segv = 1; return false; }
    }
    uint32_t need = f->access == ACC_WRITE ? PROT_W : f->access == ACC_EXEC ? PROT_X : PROT_R;
    if (!(v->prot & need) || (f->user && !(v->prot & PROT_U))) { f->segv = 2; return false; }

    WalkResult w; mmu_walk(S, va, &w);
    if (w.special) {
        if (w.sp.sp_type == SP_SWAP) return swap_in(S, v, va, &w.sp, w.pte_pa, err, sizeof err);
        f->segv = 2; return false;                       /* PROT_NONE remnant but VMA now allows: should have been restored */
    }
    return demand_fill(S, v, va, f->access, err, sizeof err);
}

/* ------------------------------------------------------------------ mmap & friends */

bool mmu_vm_mmap(MmuSpace *S, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, uint8_t cache, bool fixed, bool noreplace,
                 bool populate, MmuShm *shm, uint64_t shm_off, const char *name, uint64_t *out, char *err, size_t errn) {
    const MmuArch *A = S->A;
    if (len == 0) { seterr(err, errn, "length must be > 0"); return false; }
    if (add_ovf(len, PG - 1)) { seterr(err, errn, "length overflows"); return false; }
    len = (len + PG - 1) & ~(PG - 1);
    if (!mmu_cache_supported(A, cache)) { seterr(err, errn, "cache type '%s' is not available on %s", MMU_CACHE_NAMES[cache], A->name); return false; }
    if (shm) {
        if (shm->phys != S->phys) { seterr(err, errn, "shared memory object belongs to a different physical memory"); return false; }
        if ((shm_off & (PG - 1)) || add_ovf(shm_off, len) || shm_off + len > shm->len) { seterr(err, errn, "shm offset/length exceeds the object (size %llu)", (unsigned long long)shm->len); return false; }
        flags |= VF_SHARED; flags &= ~(uint32_t)VF_HUGE;
    }
    if (!(A->leaf_mask & 2u)) flags &= ~(uint32_t)VF_HUGE;
    uint64_t base;
    if (fixed) {
        if (addr & (PG - 1)) { seterr(err, errn, "fixed address must be page aligned"); return false; }
        if (!mmu_range_ok(S, addr, len, NULL, err, errn)) return false;
        int i = vma_lower_bound(S, addr);
        bool overlap = i < S->nvma && S->vmas[i].start < addr + len;
        if (overlap) {
            if (noreplace) { seterr(err, errn, "range overlaps an existing mapping"); return false; }
            if (!mmu_vm_munmap(S, addr, len, err, errn)) return false;
        }
        base = addr;
    } else {
        uint64_t align = ((flags & VF_HUGE) && len >= mmu_page_size(A, 1)) ? mmu_page_size(A, 1) : PG;
        uint64_t hint = addr ? addr : S->mmap_cursor;
        if (!find_area(S, len, align, hint, &base) && !find_area(S, len, align, 0, &base)) { seterr(err, errn, "no free address range of %llu bytes", (unsigned long long)len); return false; }
        S->mmap_cursor = base + len;
    }
    MmuVma v; memset(&v, 0, sizeof v);
    v.start = base; v.end = base + len; v.prot = prot; v.flags = flags; v.cache = cache; v.shm = shm; v.shm_off = shm_off;
    snprintf(v.name, sizeof v.name, "%s", name ? name : "");
    vma_insert_at(S, vma_lower_bound(S, base), &v);
    vma_merge_all(S);
    *out = base;
    if (populate && (prot & (PROT_R | PROT_W | PROT_X))) {
        if (!mmu_vm_populate(S, base, len, (prot & PROT_W) != 0, err, errn)) return false;
    }
    return true;
}

bool mmu_vm_munmap(MmuSpace *S, uint64_t addr, uint64_t len, char *err, size_t errn) {
    uint64_t last;
    if (addr & (PG - 1)) { seterr(err, errn, "address must be page aligned"); return false; }
    if (len == 0) { seterr(err, errn, "length must be > 0"); return false; }
    len = (len + PG - 1) & ~(PG - 1);
    if (!mmu_range_ok(S, addr, len, &last, err, errn)) return false;
    uint64_t end = addr + len;
    vma_split_at(S, addr); vma_split_at(S, end);
    for (int i = vma_lower_bound(S, addr); i < S->nvma && S->vmas[i].start < end;) {
        MmuVma *v = &S->vmas[i];
        if (mmu_pt_unmap(S, v->start, v->end - v->start, err, errn) < 0) return false;
        vma_remove_at(S, i);
    }
    return true;
}

static bool vma_covered(MmuSpace *S, uint64_t addr, uint64_t end) {
    uint64_t cur = addr;
    for (int i = vma_lower_bound(S, addr); i < S->nvma && cur < end; i++) {
        if (S->vmas[i].start > cur) return false;
        cur = S->vmas[i].end;
    }
    return cur >= end;
}

typedef struct { MmuSpace *S; uint32_t prot; char *err; size_t errn; bool fail; MmuVma *v; } RestoreCtx;

static bool restore_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    RestoreCtx *c = ud;
    if (lf->pte.kind != PK_SPECIAL || lf->pte.sp_type != SP_PROTNONE) return true;
    Pte p; mmu_vm_default_pte(c->v, &p);
    p.pa = lf->pte.pa; p.sw = (uint8_t)((lf->pte.sp_own ? SW_OWN : 0) | (lf->pte.sp_cow ? SW_COW : 0));
    if (lf->pte.sp_cow) p.w = false;
    p.accessed = true;
    uint64_t raw = mmu_pte_encode_leaf(S->A, 0, &p, c->err, c->errn);
    if (!raw) { c->fail = true; return false; }
    mmu_pt_rewrite_leaf(S, lf, raw);
    return true;
}

typedef struct { bool again; } SplitCtx;
static bool split_scan_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    SplitCtx *c = ud;
    if (lf->pte.kind == PK_LEAF && lf->level > 0) { char e[96]; if (mmu_pt_split(S, lf->va, e, sizeof e)) c->again = true; return false; }
    return true;
}
static bool split_range_to_base(MmuSpace *S, uint64_t lo, uint64_t hi) {
    for (int guard = 0; guard < 100000; guard++) {
        SplitCtx c = { false };
        mmu_pt_iter(S, lo, hi, split_scan_cb, &c, false);
        if (!c.again) return true;
    }
    return false;
}

static bool protnone_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    (void)ud;
    if (lf->pte.kind != PK_LEAF || lf->level != 0) return true;
    uint64_t raw = mmu_pte_special(SP_PROTNONE, lf->pte.pa, (lf->pte.sw & SW_COW) != 0, (lf->pte.sw & SW_OWN) != 0, lf->pte.global);
    mmu_pt_rewrite_leaf(S, lf, raw);
    return true;
}

bool mmu_vm_mprotect(MmuSpace *S, uint64_t addr, uint64_t len, uint32_t prot, char *err, size_t errn) {
    uint64_t last;
    if (addr & (PG - 1)) { seterr(err, errn, "address must be page aligned"); return false; }
    if (len == 0) { seterr(err, errn, "length must be > 0"); return false; }
    len = (len + PG - 1) & ~(PG - 1);
    if (!mmu_range_ok(S, addr, len, &last, err, errn)) return false;
    uint64_t end = addr + len;
    if (!vma_covered(S, addr, end)) { seterr(err, errn, "range is not fully covered by mappings (ENOMEM)"); return false; }
    vma_split_at(S, addr); vma_split_at(S, end);
    for (int i = vma_lower_bound(S, addr); i < S->nvma && S->vmas[i].start < end; i++) {
        MmuVma *v = &S->vmas[i];
        uint32_t old = v->prot;
        v->prot = prot;
        uint64_t lo = v->start, hi = v->end - 1;
        bool now_none = !(prot & (PROT_R | PROT_W | PROT_X)), was_none = !(old & (PROT_R | PROT_W | PROT_X));
        if (now_none) {
            if (!was_none) {
                if (!split_range_to_base(S, lo, hi)) { seterr(err, errn, "could not split large pages"); return false; }
                mmu_pt_iter(S, lo, hi, protnone_cb, NULL, false);
                mmu_tlb_shootdown(S, lo, hi - lo + 1);
            }
        } else {
            if (was_none) { RestoreCtx rc = { S, prot, err, errn, false, v }; mmu_pt_iter(S, lo, hi, restore_cb, &rc, true); if (rc.fail) return false; }
            int mask = PROT_R | PROT_W | PROT_X | PROT_U | PROT_G;
            int64_t r = mmu_pt_protect(S, lo, hi - lo + 1, mask, (int)prot, false, 0, false, err, errn);
            if (r < 0) return false;
        }
    }
    vma_merge_all(S);
    return true;
}

bool mmu_vm_populate(MmuSpace *S, uint64_t addr, uint64_t len, bool write, char *err, size_t errn) {
    uint64_t end = addr + len; if (end < addr) end = ~(uint64_t)0;
    uint64_t va = pg_down(addr);
    while (va < end) {
        MmuVma *v = mmu_vma_find(S, va);
        if (!v) { seterr(err, errn, "address 0x%llx is not mapped", (unsigned long long)va); return false; }
        if (!(v->prot & (PROT_R | PROT_W | PROT_X))) { va = v->end; continue; }
        Access acc = (write && (v->prot & PROT_W)) ? ACC_WRITE : (v->prot & PROT_R) ? ACC_READ : (v->prot & PROT_W) ? ACC_WRITE : ACC_EXEC;
        MmuXlate x; mmu_xlate(S->home, va, acc, false, &x, false);
        if (!x.ok) { char d[200]; mmu_fault_describe(S->A, &x.fault, d, sizeof d); seterr(err, errn, "populate failed: %s", d); return false; }
        uint64_t next = x.page_va + x.page_size;
        if (next <= va) next = va + PG;
        va = next;
    }
    return true;
}

bool mmu_vm_madvise(MmuSpace *S, uint64_t addr, uint64_t len, const char *adv, char *err, size_t errn) {
    if (addr & (PG - 1)) { seterr(err, errn, "address must be page aligned"); return false; }
    len = (len + PG - 1) & ~(PG - 1);
    uint64_t last; if (!mmu_range_ok(S, addr, len, &last, err, errn)) return false;
    uint64_t end = addr + len;
    if (!strcmp(adv, "normal") || !strcmp(adv, "sequential") || !strcmp(adv, "random")) return true;
    if (!strcmp(adv, "willneed")) return mmu_vm_populate(S, addr, len, false, err, errn);
    if (!strcmp(adv, "dontneed") || !strcmp(adv, "free")) {
        vma_split_at(S, addr); vma_split_at(S, end);
        for (int i = vma_lower_bound(S, addr); i < S->nvma && S->vmas[i].start < end; i++) {
            MmuVma *v = &S->vmas[i];
            if (v->flags & VF_LOCKED) { seterr(err, errn, "cannot discard locked pages"); return false; }
            if (mmu_pt_unmap(S, v->start, v->end - v->start, err, errn) < 0) return false;
            S->n_zapped++;
        }
        vma_merge_all(S);
        return true;
    }
    if (!strcmp(adv, "hugepage") || !strcmp(adv, "nohugepage")) {
        bool on = adv[0] == 'h';
        vma_split_at(S, addr); vma_split_at(S, end);
        for (int i = vma_lower_bound(S, addr); i < S->nvma && S->vmas[i].start < end; i++) { if (on) S->vmas[i].flags |= VF_HUGE; else S->vmas[i].flags &= ~(uint32_t)VF_HUGE; }
        vma_merge_all(S);
        return true;
    }
    seterr(err, errn, "unknown advice '%s' (normal, sequential, random, willneed, dontneed, free, hugepage, nohugepage)", adv);
    return false;
}

bool mmu_vm_mlock(MmuSpace *S, uint64_t addr, uint64_t len, bool lock, char *err, size_t errn) {
    if (addr & (PG - 1)) { seterr(err, errn, "address must be page aligned"); return false; }
    len = (len + PG - 1) & ~(PG - 1);
    uint64_t last; if (!mmu_range_ok(S, addr, len, &last, err, errn)) return false;
    uint64_t end = addr + len;
    if (!vma_covered(S, addr, end)) { seterr(err, errn, "range is not fully covered by mappings (ENOMEM)"); return false; }
    vma_split_at(S, addr); vma_split_at(S, end);
    for (int i = vma_lower_bound(S, addr); i < S->nvma && S->vmas[i].start < end; i++) { if (lock) S->vmas[i].flags |= VF_LOCKED; else S->vmas[i].flags &= ~(uint32_t)VF_LOCKED; }
    vma_merge_all(S);
    return lock ? mmu_vm_populate(S, addr, len, false, err, errn) : true;
}

/* ------------------------------------------------------------------ swap: eviction, reclaim, swap-in-all */

static bool evict_leaf(MmuSpace *S, const PtLeaf *lf, char *err, size_t errn) {
    const Pte *p = &lf->pte;
    uint64_t pa = p->pa;
    uint64_t raw;
    if (mmu_phys_frame_is_zero(S->phys, pa)) {
        raw = 0;                                           /* a zero page needs no slot: drop it, refault gives zeros */
    } else {
        uint32_t slot;
        if (!mmu_swap_alloc(S->phys, &slot)) { seterr(err, errn, S->phys->swap_slots ? "swap area is full" : "no swap area (call mmu.swap_init first)"); return false; }
        if (!mmu_swap_store(S->phys, slot, pa)) { mmu_swap_free(S->phys, slot); seterr(err, errn, "could not write the page to swap"); return false; }
        raw = mmu_pte_special(SP_SWAP, (uint64_t)slot << MMU_FRAME_SHIFT, (p->sw & SW_COW) != 0, true, p->global);
        S->n_swapout++;
    }
    mmu_pt_rewrite_leaf(S, lf, raw);
    mmu_phys_ref_dec(S->phys, pa);
    mmu_tlb_shootdown(S, lf->va, PG);
    return true;
}

typedef struct { int64_t want, evicted, scanned; uint64_t hand; bool stop; } ClockCtx;

static bool clock_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    ClockCtx *c = ud;
    const Pte *p = &lf->pte;
    if (p->kind != PK_LEAF || lf->level != 0 || !(p->sw & SW_OWN)) return true;
    MmuVma *v = mmu_vma_find(S, lf->va);
    if (!v || (v->flags & (VF_SHARED | VF_LOCKED)) || v->shm) return true;
    if (mmu_phys_refcount(S->phys, p->pa) != 1) return true;
    c->scanned++;
    if (p->accessed) {                                      /* second chance */
        mmu_pt_rewrite_leaf(S, lf, mmu_pte_clear_a(S->A, lf->raw));
        mmu_tlb_shootdown(S, lf->va, PG);
        return true;
    }
    char e[96];
    if (!evict_leaf(S, lf, e, sizeof e)) { c->stop = true; return false; }
    c->evicted++; c->hand = lf->va + PG;
    if (c->evicted >= c->want) { c->stop = true; return false; }
    return true;
}

int64_t mmu_vm_reclaim(MmuSpace *S, int64_t want, int64_t *scanned) {
    ClockCtx c = { want, 0, 0, S->clock_hand, false };
    for (int sweep = 0; sweep < 2 && !c.stop; sweep++) {
        uint64_t hand = c.hand;
        mmu_pt_iter(S, hand, ~(uint64_t)0, clock_cb, &c, false);
        if (c.stop) break;
        if (hand) mmu_pt_iter(S, 0, hand - 1, clock_cb, &c, false);
    }
    S->clock_hand = c.hand;
    if (scanned) *scanned = c.scanned;
    return c.evicted;
}

typedef struct { MmuSpace *S; uint64_t va; PtLeaf lf; bool found; } FindCtx;
static bool find_cb(MmuSpace *S, void *ud, const PtLeaf *lf) { (void)S; FindCtx *c = ud; c->lf = *lf; c->found = true; return false; }

bool mmu_vm_evict(MmuSpace *S, uint64_t va, char *err, size_t errn) {
    FindCtx c = { S, pg_down(va), {0}, false };
    mmu_pt_iter(S, c.va, c.va + PG - 1, find_cb, &c, false);
    if (!c.found || c.lf.pte.kind != PK_LEAF) { seterr(err, errn, "page is not resident"); return false; }
    if (c.lf.level != 0) { if (!mmu_pt_split(S, va, err, errn)) return false; mmu_pt_iter(S, c.va, c.va + PG - 1, find_cb, &c, false); }
    if (!(c.lf.pte.sw & SW_OWN)) { seterr(err, errn, "page is not managed by the VM layer (mapped with raw map)"); return false; }
    MmuVma *v = mmu_vma_find(S, va);
    if (v && (v->flags & (VF_LOCKED | VF_SHARED))) { seterr(err, errn, "page is locked or shared"); return false; }
    if (mmu_phys_refcount(S->phys, c.lf.pte.pa) != 1) { seterr(err, errn, "page is shared with another address space (copy-on-write)"); return false; }
    return evict_leaf(S, &c.lf, err, errn);
}

typedef struct { MmuSpace *S; char *err; size_t errn; bool fail; } SwapAllCtx;
static bool swapall_cb(MmuSpace *S, void *ud, const PtLeaf *lf) {
    SwapAllCtx *c = ud;
    if (lf->pte.kind != PK_SPECIAL || lf->pte.sp_type != SP_SWAP) return true;
    MmuVma *v = mmu_vma_find(S, lf->va);
    MmuVma tmp; memset(&tmp, 0, sizeof tmp); tmp.prot = PROT_R | PROT_W | PROT_U;
    if (!swap_in(S, v ? v : &tmp, lf->va, &lf->pte, lf->pte_pa, c->err, c->errn)) { c->fail = true; return false; }
    return true;
}

bool mmu_vm_swap_in_all(MmuSpace *S, char *err, size_t errn) {
    if (S->phys->swap_in_use == 0) return true;
    SwapAllCtx c = { S, err, errn, false };
    mmu_pt_iter(S, 0, ~(uint64_t)0, swapall_cb, &c, true);
    return !c.fail;
}

/* ------------------------------------------------------------------ fork */

typedef struct { MmuSpace *p, *ch; char *err; size_t errn; bool fail; } ForkCtx;

static bool fork_cb(MmuSpace *P, void *ud, const PtLeaf *lf) {
    ForkCtx *c = ud;
    const MmuArch *A = P->A;
    MmuVma *v = mmu_vma_find(P, lf->va);
    bool priv = !(v && (v->flags & VF_SHARED));
    uint64_t raw = lf->raw;
    if (lf->pte.kind == PK_LEAF) {
        if (lf->pte.sw & SW_OWN) {
            uint64_t n = (uint64_t)1 << (A->idx_bits * lf->level);
            for (uint64_t i = 0; i < n; i++) mmu_phys_ref_inc(P->phys, lf->pte.pa + (i << MMU_FRAME_SHIFT));
            if (priv && lf->pte.w) {
                Pte q = lf->pte; q.sw |= SW_COW; q.w = false;
                uint64_t nr = mmu_pte_encode_leaf(A, lf->level, &q, c->err, c->errn);
                if (!nr) { c->fail = true; return false; }
                mmu_pt_rewrite_leaf(P, lf, nr);
                raw = nr; P->n_fork_cow++;
            }
        }
    } else {                                                 /* PROT_NONE remnant */
        if (lf->pte.sp_own) {
            mmu_phys_ref_inc(P->phys, lf->pte.pa);
            if (priv && !lf->pte.sp_cow) { raw = mmu_pte_special(SP_PROTNONE, lf->pte.pa, true, true, lf->pte.sp_global); mmu_pt_rewrite_leaf(P, lf, raw); }
        }
    }
    if (!mmu_pt_map(c->ch, lf->va, lf->level, raw, false, c->err, c->errn)) {
        if (lf->pte.kind == PK_LEAF && (lf->pte.sw & SW_OWN)) { uint64_t n = (uint64_t)1 << (A->idx_bits * lf->level); for (uint64_t i = 0; i < n; i++) mmu_phys_ref_dec(P->phys, lf->pte.pa + (i << MMU_FRAME_SHIFT)); }
        c->fail = true; return false;
    }
    return true;
}

MmuSpace *mmu_vm_fork(MmuSpace *parent, const char *name, char *err, size_t errn) {
    if (!mmu_vm_swap_in_all(parent, err, errn)) return NULL;
    MmuSpace *ch = mmu_space_new(parent->A, parent->phys, name, NULL, -1, err, errn);
    if (!ch) return NULL;
    MmuCpu *pc = parent->home, *cc = ch->home;
    cc->wp = pc->wp; cc->smep = pc->smep; cc->smap = pc->smap; cc->sw_ad = pc->sw_ad; cc->manual_tlb = pc->manual_tlb; cc->use_asid = pc->use_asid && ch->A->asid_bits > 0; cc->strict = pc->strict;
    if (pc->tlb_entries != cc->tlb_entries || pc->tlb_ways != cc->tlb_ways) mmu_tlb_configure(cc, pc->tlb_entries, pc->tlb_ways);
    ch->mmap_cursor = parent->mmap_cursor; ch->stack_max = parent->stack_max;
    for (int i = 0; i < parent->nvma; i++) vma_insert_at(ch, ch->nvma, &parent->vmas[i]);
    ForkCtx c = { parent, ch, err, errn, false };
    mmu_pt_iter(parent, 0, ~(uint64_t)0, fork_cb, &c, true);
    mmu_tlb_shootdown_all(parent);
    if (c.fail) { mmu_vm_destroy(ch); free(ch); return NULL; }
    return ch;
}
