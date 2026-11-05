/* KingCobra std.mmu — the CPU model: translation lookaside buffer, access policy, hardware
 * accessed/dirty updates, fault delivery, and virtual-address data access.
 *
 * The TLB is real in the sense that matters: set-associative per page size, ASID/PCID tagged,
 * global-aware, LRU-replaced, and *stale until invalidated* when `manual_tlb` is on, so a script
 * can reproduce (and then fix) the classic "forgot the TLB shootdown" bug. Page-table walks only
 * happen on a miss, exactly as on hardware; permission faults seen through a cached entry trigger a
 * re-walk before they are reported.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

MmuCpu *MMU_CPUS[MMU_MAX_CPUS];
Fault MMU_LAST_FAULT;
int MMU_LAST_FAULT_VALID = 0;
const MmuArch *MMU_LAST_FAULT_ARCH = NULL;

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ TLB banks */

static void banks_free(MmuCpu *c) {
    for (int i = 0; i < MMU_MAX_LEVELS; i++) { free(c->bank[i].e); c->bank[i].e = NULL; c->bank[i].sets = c->bank[i].ways = 0; }
}

void mmu_tlb_configure(MmuCpu *c, int entries, int ways) {
    banks_free(c);
    c->tlb_entries = entries; c->tlb_ways = ways;
    if (entries <= 0) { c->tlb_entries = 0; return; }
    if (ways < 1) ways = 1;
    if (ways > 16) ways = 16;
    if (ways > entries) ways = entries;
    c->tlb_ways = ways;
    for (int L = 0; L < c->A->levels; L++) {
        if (L > 0 && !(c->A->leaf_mask & (1u << L))) continue;
        int n = L == 0 ? entries : (entries / 8 < 4 ? 4 : entries / 8);
        int w = ways > n ? n : ways;
        int sets = n / w; if (sets < 1) sets = 1;
        c->bank[L].sets = sets; c->bank[L].ways = w;
        c->bank[L].e = kc_malloc(sizeof(MmuTlbEnt) * (size_t)sets * (size_t)w);
        memset(c->bank[L].e, 0, sizeof(MmuTlbEnt) * (size_t)sets * (size_t)w);
    }
}

MmuCpu *mmu_cpu_create(const MmuArch *A, const char *name, char *err, size_t errn) {
    int slot = -1;
    for (int i = 0; i < MMU_MAX_CPUS; i++) if (!MMU_CPUS[i]) { slot = i; break; }
    if (slot < 0) { seterr(err, errn, "too many CPUs/address spaces alive (limit %d); destroy some first", MMU_MAX_CPUS); return NULL; }
    MmuCpu *c = kc_malloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->id = slot; c->A = A; c->alive = true;
    c->wp = true; c->smep = true;
    c->use_asid = A->asid_bits > 0;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "cpu");
    mmu_tlb_configure(c, 64, 4);
    MMU_CPUS[slot] = c;
    return c;
}

void mmu_cpu_destroy(MmuCpu *c) {
    if (!c) return;
    MMU_CPUS[c->id] = NULL;
    banks_free(c);
    c->alive = false;
    free(c);
}

bool mmu_cpu_parse_opts(MmuCpu *c, const char *opts, int *asid_out, bool *asid_given, char *err, size_t errn) {
    if (asid_given) *asid_given = false;
    if (!opts || !*opts) return true;
    char buf[256]; snprintf(buf, sizeof buf, "%s", opts);
    int tlb = c->tlb_entries, ways = c->tlb_ways;
    for (char *t = strtok(buf, ", |+"); t; t = strtok(NULL, ", |+")) {
        if (!strcmp(t, "wp")) c->wp = true;
        else if (!strcmp(t, "nowp")) c->wp = false;
        else if (!strcmp(t, "smep")) c->smep = true;
        else if (!strcmp(t, "nosmep")) c->smep = false;
        else if (!strcmp(t, "smap") || !strcmp(t, "pan") || !strcmp(t, "sum_off")) c->smap = true;
        else if (!strcmp(t, "nosmap")) c->smap = false;
        else if (!strcmp(t, "sw_ad") || !strcmp(t, "svade")) c->sw_ad = true;
        else if (!strcmp(t, "hw_ad")) c->sw_ad = false;
        else if (!strcmp(t, "manual_tlb")) c->manual_tlb = true;
        else if (!strcmp(t, "auto_tlb")) c->manual_tlb = false;
        else if (!strcmp(t, "strict")) c->strict = true;
        else if (!strcmp(t, "noasid")) c->use_asid = false;
        else if (!strcmp(t, "asid")) c->use_asid = true;
        else if (!strncmp(t, "tlb=", 4)) { tlb = atoi(t + 4); if (tlb < 0 || tlb > 65536) { seterr(err, errn, "tlb=N must be 0..65536"); return false; } }
        else if (!strncmp(t, "ways=", 5)) { ways = atoi(t + 5); if (ways < 1 || ways > 16) { seterr(err, errn, "ways=N must be 1..16"); return false; } }
        else if (!strncmp(t, "asid=", 5)) {
            if (!asid_out) { seterr(err, errn, "asid=N is a property of an address space, not of a bare cpu"); return false; }
            char *end; long v = strtol(t + 5, &end, 0);
            if (*end || v < 0 || v >= (1l << (c->A->asid_bits ? c->A->asid_bits : 1))) { seterr(err, errn, "asid=%s is out of range for %s (%d ASID bits)", t + 5, c->A->name, c->A->asid_bits); return false; }
            *asid_out = (int)v; if (asid_given) *asid_given = true;
        } else {
            seterr(err, errn, "unknown option '%s' (valid: wp nowp smep nosmep smap nosmap sw_ad hw_ad manual_tlb auto_tlb strict asid noasid tlb=N ways=N asid=N)", t);
            return false;
        }
    }
    if (c->A->asid_bits == 0) c->use_asid = false;
    if (tlb != c->tlb_entries || ways != c->tlb_ways) mmu_tlb_configure(c, tlb, ways);
    return true;
}

/* ------------------------------------------------------------------ TLB operations */

static inline uint32_t cur_asid(const MmuCpu *c) { return (c->use_asid && c->cur) ? c->cur->asid : 0; }

static MmuTlbEnt *tlb_lookup(MmuCpu *c, uint64_t va, int *level_out) {
    uint32_t asid = cur_asid(c);
    for (int L = 0; L < c->A->levels; L++) {
        MmuTlbBank *b = &c->bank[L];
        if (!b->e) continue;
        uint64_t vpn = va >> mmu_shift(c->A, L);
        MmuTlbEnt *set = &b->e[(size_t)(vpn % (uint64_t)b->sets) * (size_t)b->ways];
        for (int w = 0; w < b->ways; w++) {
            MmuTlbEnt *e = &set[w];
            if (e->valid && e->vpn == vpn && (e->global || !c->use_asid || e->asid == asid)) { *level_out = L; return e; }
        }
    }
    return NULL;
}

static void tlb_insert(MmuCpu *c, int level, uint64_t va, const MmuTlbEnt *src) {
    MmuTlbBank *b = &c->bank[level];
    if (!b->e) return;
    uint64_t vpn = va >> mmu_shift(c->A, level);
    MmuTlbEnt *set = &b->e[(size_t)(vpn % (uint64_t)b->sets) * (size_t)b->ways];
    MmuTlbEnt *victim = NULL;
    for (int w = 0; w < b->ways; w++) {
        if (set[w].valid && set[w].vpn == vpn && (set[w].global == src->global) && (set[w].asid == src->asid || set[w].global)) { victim = &set[w]; break; }
    }
    if (!victim) for (int w = 0; w < b->ways; w++) if (!set[w].valid) { victim = &set[w]; break; }
    if (!victim) {
        victim = &set[0];
        for (int w = 1; w < b->ways; w++) if (set[w].lru < victim->lru) victim = &set[w];
        c->tlb_evictions++;
    }
    *victim = *src; victim->valid = true; victim->vpn = vpn; victim->lru = ++c->tick;
}

static void tlb_inval_range(MmuCpu *c, uint64_t lo, uint64_t hi, int asid, bool include_global) {
    for (int L = 0; L < c->A->levels; L++) {
        MmuTlbBank *b = &c->bank[L];
        if (!b->e) continue;
        int sh = mmu_shift(c->A, L);
        int total = b->sets * b->ways;
        for (int i = 0; i < total; i++) {
            MmuTlbEnt *e = &b->e[i];
            if (!e->valid) continue;
            uint64_t s = e->vpn << sh, en = s + (((uint64_t)1 << sh) - 1);
            if (en < lo || s > hi) continue;
            if (e->global) { if (!include_global) continue; }
            else if (asid >= 0 && c->use_asid && e->asid != (uint32_t)asid) continue;
            e->valid = false;
        }
    }
}

void mmu_tlb_flush(MmuCpu *c, bool include_global) {
    tlb_inval_range(c, 0, ~(uint64_t)0, -1, include_global);
    c->tlb_flushes++;
}

void mmu_tlb_invalidate(MmuCpu *c, uint64_t va, int asid) {
    tlb_inval_range(c, va, va, asid, true);
    c->tlb_invlpg++;
}

void mmu_tlb_shootdown(MmuSpace *S, uint64_t va, uint64_t len) {
    uint64_t hi = va + (len ? len - 1 : 0); if (hi < va) hi = ~(uint64_t)0;
    for (int i = 0; i < MMU_MAX_CPUS; i++) {
        MmuCpu *c = MMU_CPUS[i];
        if (!c || c->manual_tlb) continue;
        if (c->use_asid) tlb_inval_range(c, va, hi, (int)S->asid, true);
        else if (c->cur == S) tlb_inval_range(c, va, hi, -1, true);
    }
}

void mmu_tlb_shootdown_all(MmuSpace *S) { mmu_tlb_shootdown(S, 0, ~(uint64_t)0); }

void mmu_cpu_switch(MmuCpu *c, MmuSpace *S, bool flush) {
    c->cur = S; c->switches++;
    if (!c->use_asid || flush) mmu_tlb_flush(c, false);
}

/* ------------------------------------------------------------------ access policy */

static FaultKind check_perm(const MmuCpu *c, bool r, bool w, bool x, bool pg_user, Access acc, bool user_mode) {
    bool rv = c->A->fam == FAM_RV32 || c->A->fam == FAM_RV64;
    if (user_mode) { if (!pg_user) return FK_PROTECTION; }
    else if (pg_user) {
        if (acc == ACC_EXEC) { if (c->smep || rv) return FK_PROTECTION; }
        else if (c->smap) return FK_PROTECTION;
    }
    switch (acc) {
    case ACC_READ:  return r ? FK_NONE : FK_PROTECTION;
    case ACC_EXEC:  return x ? FK_NONE : FK_PROTECTION;
    case ACC_WRITE:
        if (w) return FK_NONE;
        if (!user_mode && !c->wp && (c->A->fam == FAM_X86_32 || c->A->fam == FAM_X86_64)) return FK_NONE;
        return FK_PROTECTION;
    }
    return FK_PROTECTION;
}

static inline bool sw_ad_mode(const MmuCpu *c) {
    return c->sw_ad && (c->A->fam == FAM_ARM64 || c->A->fam == FAM_RV32 || c->A->fam == FAM_RV64);
}

static void apply_hw_ad(MmuCpu *c, MmuSpace *S, const WalkResult *w, Access acc, bool *dirty_out) {
    const MmuArch *A = S->A;
    bool x86 = A->fam == FAM_X86_32 || A->fam == FAM_X86_64;
    if (x86) {                                        /* x86 also sets A in every table entry on the path */
        for (int i = 0; i < w->nsteps - 1; i++) {
            const WalkStep *st = &w->steps[i];
            if (st->kind == PK_TABLE && !(st->raw & 0x20)) mmu_wr_pte(S, st->table_pa, st->index, st->raw | 0x20);
        }
    }
    bool setA = !w->leaf.accessed;
    bool setD = acc == ACC_WRITE && !w->leaf.dirty;
    if (setA || setD) {
        uint64_t nr = mmu_pte_set_ad(A, w->level, w->raw, false, setA, setD);
        if (nr != w->raw) mmu_phys_write_u(S->phys, w->pte_pa, A->pte_size, nr);
    }
    *dirty_out = w->leaf.dirty || setD;
    (void)c;
}

/* ------------------------------------------------------------------ translation */

static void fill_from_entry(MmuXlate *o, const MmuTlbEnt *e, int level, uint64_t va, const MmuArch *A) {
    uint64_t ps = mmu_page_size(A, level);
    o->ok = true; o->level = level; o->page_size = ps;
    o->page_va = va & ~(ps - 1); o->page_pa = e->pa; o->pa = e->pa + (va & (ps - 1));
    o->r = e->r; o->w = e->w; o->x = e->x; o->user = e->user; o->global = e->global; o->dirty = e->dirty; o->accessed = true;
    o->cache = e->cache; o->from_tlb = true;
}

static bool call_user_handler(MmuCpu *c, const MmuArch *A, Fault *f) {
    if (!MMU_IP) return false;
    KcValue fo = mm_fault_object(A, f);
    KcValue r = kc_call_value(MMU_IP, c->on_fault, &fo, 1, MMU_POS);
    return kc_truthy(r);
}

void mmu_xlate(MmuCpu *c, uint64_t va, Access acc, bool user, MmuXlate *out, bool probe) {
    memset(out, 0, sizeof *out);
    MmuSpace *S = c->cur;
    const MmuArch *A = c->A;
    c->accesses++;
    if (!S) {
        out->fault.va = va; out->fault.access = acc; out->fault.user = user; out->fault.kind = FK_NOT_PRESENT; out->fault.level = A->levels - 1;
        mmu_fault_fill_code(A, &out->fault);
        return;
    }
    for (int attempt = 0; attempt < 6; attempt++) {
        Fault f;
        /* 1. TLB */
        if (!probe && c->tlb_entries > 0) {
            int lvl;
            MmuTlbEnt *e = tlb_lookup(c, va, &lvl);
            if (e) {
                FaultKind k = check_perm(c, e->r, e->w, e->x, e->user, acc, user);
                if (k == FK_NONE && !(acc == ACC_WRITE && !e->dirty)) {
                    e->lru = ++c->tick; c->tlb_hits++;
                    fill_from_entry(out, e, lvl, va, A);
                    return;
                }
                e->valid = false;       /* drop it; the re-walk below decides whether this is a real fault */
            }
            c->tlb_misses++;
        }
        /* 2. walk */
        WalkResult w; mmu_walk(S, va, &w); c->walks++;
        FaultKind k = FK_NONE;
        if (w.ok) {
            const Pte *p = &w.leaf;
            bool eff_w = p->w && !w.t_ro, eff_user = p->user && !w.t_nouser;
            bool eff_x = p->x && !(p->user ? w.t_xn_u : w.t_xn_p);
            k = check_perm(c, p->r, eff_w, eff_x, eff_user, acc, user);
            if (k == FK_NONE && sw_ad_mode(c)) {
                if (!p->accessed) k = FK_ACCESS_FLAG;
                else if (acc == ACC_WRITE && !p->dirty && eff_w) k = FK_DIRTY;
            }
            uint64_t ps = mmu_page_size(A, w.level), pa = p->pa + (va & (ps - 1));
            if (k == FK_NONE && !mmu_phys_in_range(S->phys, pa, 1)) k = FK_BUS;
            if (k == FK_NONE) {
                bool dirty = p->dirty;
                if (!probe && !sw_ad_mode(c)) apply_hw_ad(c, S, &w, acc, &dirty);
                MmuTlbEnt ne; memset(&ne, 0, sizeof ne);
                ne.global = p->global; ne.r = p->r; ne.w = eff_w; ne.x = eff_x; ne.user = eff_user; ne.dirty = dirty;
                ne.cache = p->cache; ne.asid = cur_asid(c); ne.pa = p->pa;
                if (!probe && c->tlb_entries > 0) tlb_insert(c, w.level, va, &ne);
                out->ok = true; out->level = w.level; out->page_size = ps; out->page_va = w.page_va; out->page_pa = p->pa; out->pa = pa;
                out->r = p->r; out->w = eff_w; out->x = eff_x; out->user = eff_user; out->global = p->global;
                out->dirty = dirty; out->accessed = true; out->cache = p->cache; out->sw = p->sw; out->raw = w.raw; out->pte_pa = w.pte_pa;
                return;
            }
            f = w.fault; f.kind = k; f.level = w.level; f.raw = w.raw;
            f.cow = (k == FK_PROTECTION && acc == ACC_WRITE && (p->sw & SW_COW));
        } else {
            f = w.fault;
        }
        f.va = va; f.access = acc; f.user = user;
        mmu_fault_fill_code(A, &f);
        if (probe) { out->fault = f; return; }
        c->faults++;
        /* 3. handlers: kernel-style built-in first, then the script's */
        if (!c->in_handler) {
            c->in_handler = true;
            bool resolved = mmu_vm_fault(c, S, &f);
            if (!resolved && c->has_handler) resolved = call_user_handler(c, A, &f);
            c->in_handler = false;
            if (resolved) { c->handled_faults++; continue; }
        }
        out->fault = f;
        c->last_fault = f; c->has_last_fault = true;
        MMU_LAST_FAULT = f; MMU_LAST_FAULT_VALID = 1; MMU_LAST_FAULT_ARCH = A;
        return;
    }
    /* handlers kept "resolving" without making progress */
    out->fault.va = va; out->fault.access = acc; out->fault.user = user; out->fault.kind = FK_NOT_PRESENT; out->fault.level = 0;
    mmu_fault_fill_code(A, &out->fault);
    c->last_fault = out->fault; c->has_last_fault = true;
    MMU_LAST_FAULT = out->fault; MMU_LAST_FAULT_VALID = 1; MMU_LAST_FAULT_ARCH = A;
}

/* ------------------------------------------------------------------ data access */

static bool wrap_fault(MmuCpu *c, uint64_t va, size_t len, Access acc, bool user, Fault *f) {
    if (len && va + (len - 1) < va) {
        memset(f, 0, sizeof *f); f->va = va; f->access = acc; f->user = user; f->kind = FK_NONCANONICAL; f->level = c->A->levels - 1;
        mmu_fault_fill_code(c->A, f);
        c->last_fault = *f; c->has_last_fault = true; MMU_LAST_FAULT = *f; MMU_LAST_FAULT_VALID = 1; MMU_LAST_FAULT_ARCH = c->A;
        return true;
    }
    return false;
}

static void bus_fault(MmuCpu *c, uint64_t va, Access acc, bool user, Fault *f) {
    memset(f, 0, sizeof *f); f->va = va; f->access = acc; f->user = user; f->kind = FK_BUS; f->level = 0;
    mmu_fault_fill_code(c->A, f);
    c->last_fault = *f; c->has_last_fault = true; MMU_LAST_FAULT = *f; MMU_LAST_FAULT_VALID = 1; MMU_LAST_FAULT_ARCH = c->A;
}

bool mmu_vread(MmuCpu *c, uint64_t va, void *dst, size_t len, bool user, Fault *f) {
    if (!c->cur) { MmuXlate x; mmu_xlate(c, va, ACC_READ, user, &x, false); *f = x.fault; return false; }
    if (wrap_fault(c, va, len, ACC_READ, user, f)) return false;
    uint8_t *out = dst;
    while (len) {
        MmuXlate x; mmu_xlate(c, va, ACC_READ, user, &x, false);
        if (!x.ok) { *f = x.fault; return false; }
        size_t room = (size_t)(x.page_size - (va & (x.page_size - 1)));
        size_t n = room < len ? room : len;
        if (!mmu_phys_read(c->cur->phys, x.pa, out, n)) { bus_fault(c, va, ACC_READ, user, f); return false; }
        out += n; va += n; len -= n;
    }
    return true;
}

typedef struct { uint64_t pa; size_t n; } Piece;

bool mmu_vwrite(MmuCpu *c, uint64_t va, const void *src, size_t len, bool user, Fault *f) {
    if (!c->cur) { MmuXlate x; mmu_xlate(c, va, ACC_WRITE, user, &x, false); *f = x.fault; return false; }
    if (wrap_fault(c, va, len, ACC_WRITE, user, f)) return false;
    /* pass 1: translate every page (running demand paging / COW handlers) so a fault leaves memory untouched */
    size_t cap = 8, np = 0; Piece *pc = kc_malloc(sizeof(Piece) * cap);
    uint64_t v = va; size_t left = len;
    while (left) {
        MmuXlate x; mmu_xlate(c, v, ACC_WRITE, user, &x, false);
        if (!x.ok) { *f = x.fault; free(pc); return false; }
        size_t room = (size_t)(x.page_size - (v & (x.page_size - 1)));
        size_t n = room < left ? room : left;
        if (np == cap) { cap *= 2; pc = kc_realloc(pc, sizeof(Piece) * cap); }
        pc[np].pa = x.pa; pc[np].n = n; np++;
        v += n; left -= n;
    }
    const uint8_t *in = src; uint64_t cv = va;
    for (size_t i = 0; i < np; i++) {
        if (!mmu_phys_write(c->cur->phys, pc[i].pa, in, pc[i].n)) { bus_fault(c, cv, ACC_WRITE, user, f); free(pc); return false; }
        in += pc[i].n; cv += pc[i].n;
    }
    free(pc);
    return true;
}

bool mmu_vfetch_check(MmuCpu *c, uint64_t va, bool user, Fault *f) {
    MmuXlate x; mmu_xlate(c, va, ACC_EXEC, user, &x, false);
    if (!x.ok) { *f = x.fault; return false; }
    return true;
}

void mmu_deliver_fault_strict(MmuCpu *c, const Fault *f) {
    char d[256]; mmu_fault_describe(c->A, f, d, sizeof d);
    kc_error_at(MMU_POS, "unhandled page fault: %s", d);
}
