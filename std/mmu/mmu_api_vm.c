/* KingCobra std.mmu — script-facing natives, part 2: the virtual-memory manager
 * (mmap family, VMAs, fork with copy-on-write, shared memory, swap reclaim).
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

#define OI(o, k, v) mm_oset(o, k, kc_int((int64_t)(v)))
#define OB(o, k, v) mm_oset(o, k, kc_bool(v))
#define OS(o, k, v) mm_oset(o, k, kc_string(v))
#define NEED_SPACE(i) ((MmuSpace *)mm_need_handle(a, n, i, MH_SPACE, pos, who))

static KcValue vma_object(const MmuArch *A, const MmuVma *v) {
    KcValue o = kc_object_new();
    Pte p; memset(&p, 0, sizeof p);
    p.r = (v->prot & (PROT_R | PROT_W)) != 0; p.w = (v->prot & PROT_W) != 0; p.x = (v->prot & PROT_X) != 0;
    p.user = (v->prot & PROT_U) != 0; p.global = (v->prot & PROT_G) != 0; p.cache = v->cache;
    char fl[64]; mmu_flags_str(fl, sizeof fl, &p);
    if (!fl[0]) snprintf(fl, sizeof fl, "none");
    OI(o, "start", v->start); OI(o, "end", v->end); OI(o, "size", v->end - v->start);
    OS(o, "flags", fl); OS(o, "name", v->name);
    OB(o, "shared", (v->flags & VF_SHARED) != 0); OB(o, "growsdown", (v->flags & VF_GROWSDOWN) != 0);
    OB(o, "locked", (v->flags & VF_LOCKED) != 0); OB(o, "huge", (v->flags & VF_HUGE) != 0); OB(o, "guard", (v->flags & VF_GUARD) != 0);
    OB(o, "shm", v->shm != NULL);
    OS(o, "cache", MMU_CACHE_NAMES[v->cache]);
    (void)A;
    return o;
}

static KcValue f_mmap(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.mmap";
    mm_need(n, 4, 7, pos, "mmu.mmap(space, addr | null, len, flags [, name [, shm, shm_offset]]) -> address | null");
    MmuSpace *S = NEED_SPACE(0);
    uint64_t addr = a[1].type == VAL_NULL ? 0 : mm_arg_addr(a, n, 1, pos, who);
    uint64_t len = mm_arg_size(a, n, 2, pos, who);
    MFlags F; mm_parse_flags(mm_arg_str(a, n, 3, pos, who), &F, pos, who);
    if (F.rel) MM_ERR("%s: +/- flags only apply to mmu.protect", who);
    if (F.guard) { F.none = true; F.prot = 0; }
    if (!F.prot && !F.none) MM_ERR("%s: flags need at least one of r, w, x (or \"none\"/\"guard\" for an inaccessible reservation)", who);
    const char *name = n >= 5 && a[4].type == VAL_STRING ? a[4].as.s->data : (F.guard ? "[guard]" : "");
    MmuShm *shm = NULL; uint64_t soff = 0;
    if (n >= 6 && a[5].type != VAL_NULL) { shm = mm_need_handle(a, n, 5, MH_SHM, pos, who); soff = n == 7 ? mm_arg_size(a, n, 6, pos, who) : 0; }
    uint32_t vf = (F.growsdown ? VF_GROWSDOWN : 0) | (F.locked ? VF_LOCKED : 0) | (F.huge ? VF_HUGE : 0) | (F.guard ? VF_GUARD : 0) | (F.shared ? VF_SHARED : 0);
    uint64_t out; char err[200];
    if (!mmu_vm_mmap(S, addr, len, F.prot, vf, (uint8_t)(F.cache_set ? F.cache : MC_WB), F.fixed, F.noreplace, F.populate || F.locked, shm, soff, name, &out, err, sizeof err)) {
        mm_set_error("%s", err); return kc_null();
    }
    return kc_int((int64_t)out);
}

static KcValue f_munmap(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.munmap"; mm_need(n, 3, 3, pos, "mmu.munmap(space, addr, len) -> bool");
    MmuSpace *S = NEED_SPACE(0); char err[200];
    if (!mmu_vm_munmap(S, mm_arg_addr(a, n, 1, pos, who), mm_arg_size(a, n, 2, pos, who), err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue f_mprotect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.mprotect"; mm_need(n, 4, 4, pos, "mmu.mprotect(space, addr, len, flags) -> bool   (flags: r,w,x,u,g or \"none\")");
    MmuSpace *S = NEED_SPACE(0); MFlags F; mm_parse_flags(mm_arg_str(a, n, 3, pos, who), &F, pos, who);
    if (F.rel) MM_ERR("%s: pass absolute flags such as \"r,w\" (use mmu.protect for +/- edits)", who);
    if (!F.prot && !F.none) MM_ERR("%s: flags need at least one of r, w, x or \"none\"", who);
    char err[200];
    if (!mmu_vm_mprotect(S, mm_arg_addr(a, n, 1, pos, who), mm_arg_size(a, n, 2, pos, who), F.prot, err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue f_madvise(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.madvise"; mm_need(n, 4, 4, pos, "mmu.madvise(space, addr, len, advice) -> bool   (dontneed, free, willneed, hugepage, nohugepage, normal, sequential, random)");
    MmuSpace *S = NEED_SPACE(0); char err[200];
    if (!mmu_vm_madvise(S, mm_arg_addr(a, n, 1, pos, who), mm_arg_size(a, n, 2, pos, who), mm_arg_str(a, n, 3, pos, who), err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue lock_common(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, bool lock) {
    MM_ENTER(); mm_need(n, 3, 3, pos, lock ? "mmu.mlock(space, addr, len) -> bool" : "mmu.munlock(space, addr, len) -> bool");
    MmuSpace *S = NEED_SPACE(0); char err[200];
    if (!mmu_vm_mlock(S, mm_arg_addr(a, n, 1, pos, who), mm_arg_size(a, n, 2, pos, who), lock, err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}
static KcValue f_mlock(KcInterp *ip, KcValue *a, int n, KcPos pos) { return lock_common(ip, a, n, pos, "mmu.mlock", true); }
static KcValue f_munlock(KcInterp *ip, KcValue *a, int n, KcPos pos) { return lock_common(ip, a, n, pos, "mmu.munlock", false); }

static KcValue f_populate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.populate"; mm_need(n, 3, 4, pos, "mmu.populate(space, addr, len [, write]) -> bool   (pre-fault a range)");
    MmuSpace *S = NEED_SPACE(0); char err[240];
    if (!mmu_vm_populate(S, mm_arg_addr(a, n, 1, pos, who), mm_arg_size(a, n, 2, pos, who), n == 4 && kc_truthy(a[3]), err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue f_vmas(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.vmas"; mm_need(n, 1, 1, pos, "mmu.vmas(space) -> [ { start, end, size, flags, name, ... } ]");
    MmuSpace *S = NEED_SPACE(0);
    KcValue arr = kc_array_new();
    for (int i = 0; i < S->nvma; i++) kc_array_push(arr, vma_object(S->A, &S->vmas[i]));
    return arr;
}

static KcValue f_find_vma(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.find_vma"; mm_need(n, 2, 2, pos, "mmu.find_vma(space, va) -> vma object | null");
    MmuSpace *S = NEED_SPACE(0); MmuVma *v = mmu_vma_find(S, mm_arg_addr(a, n, 1, pos, who));
    return v ? vma_object(S->A, v) : kc_null();
}

static KcValue f_fork(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.fork"; mm_need(n, 1, 2, pos, "mmu.fork(space [, name]) -> child space (private pages copy-on-write)");
    MmuSpace *S = NEED_SPACE(0); char err[200];
    MmuSpace *ch = mmu_vm_fork(S, n == 2 ? mm_arg_str(a, n, 1, pos, who) : "child", err, sizeof err);
    if (!ch) { mm_set_error("%s", err); return kc_null(); }
    return mmuh_wrap(MH_SPACE, ch);
}

static KcValue f_shm(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.shm"; mm_need(n, 2, 3, pos, "mmu.shm(phys, len [, name]) -> shared memory object");
    MmuPhys *p = mm_need_handle(a, n, 0, MH_PHYS, pos, who);
    uint64_t len = mm_arg_size(a, n, 1, pos, who);
    if (len == 0 || len > ((uint64_t)1 << 36)) MM_ERR("%s: length must be 1 byte .. 64 GiB", who);
    return mmuh_wrap(MH_SHM, mmu_shm_new(p, len, n == 3 ? mm_arg_str(a, n, 2, pos, who) : "shm"));
}

static KcValue f_shm_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.shm_info"; mm_need(n, 1, 1, pos, "mmu.shm_info(shm) -> object");
    MmuShm *s = mm_need_handle(a, n, 0, MH_SHM, pos, who);
    uint64_t res = 0; for (uint64_t i = 0; i < s->npages; i++) if (s->frames[i]) res++;
    KcValue o = kc_object_new(); OS(o, "name", s->name); OI(o, "size", s->len); OI(o, "pages", s->npages); OI(o, "resident_pages", res); OI(o, "refs", s->refs);
    return o;
}

static KcValue f_reclaim(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.reclaim"; mm_need(n, 2, 2, pos, "mmu.reclaim(space, pages) -> number evicted   (clock/second-chance scan; needs mmu.swap_init for non-zero pages)");
    MmuSpace *S = NEED_SPACE(0); int64_t want = mm_arg_int(a, n, 1, pos, who);
    if (want < 1) MM_ERR("%s: pages must be >= 1", who);
    int64_t scanned; return kc_int(mmu_vm_reclaim(S, want, &scanned));
}

static KcValue f_evict(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.evict"; mm_need(n, 2, 2, pos, "mmu.evict(space, va) -> bool   (swap one page out now)");
    MmuSpace *S = NEED_SPACE(0); char err[200];
    if (!mmu_vm_evict(S, mm_arg_addr(a, n, 1, pos, who), err, sizeof err)) { mm_set_error("%s", err); return kc_bool(false); }
    return kc_bool(true);
}

static KcValue f_stack_limit(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; const char *who = "mmu.stack_limit"; mm_need(n, 2, 2, pos, "mmu.stack_limit(space, bytes) -> bool   (max size a growsdown mapping may reach)");
    MmuSpace *S = NEED_SPACE(0); uint64_t b = mm_arg_size(a, n, 1, pos, who);
    if (b < 4096) MM_ERR("%s: limit must be at least one page", who);
    S->stack_max = b; return kc_bool(true);
}

#define E(n, f) { n, f }
const MmuEntry mmu_api_vm_entries[] = {
    E("mmap", f_mmap), E("munmap", f_munmap), E("mprotect", f_mprotect), E("madvise", f_madvise),
    E("mlock", f_mlock), E("munlock", f_munlock), E("populate", f_populate),
    E("vmas", f_vmas), E("find_vma", f_find_vma), E("fork", f_fork),
    E("shm", f_shm), E("shm_info", f_shm_info), E("reclaim", f_reclaim), E("evict", f_evict), E("stack_limit", f_stack_limit),
};
const int mmu_api_vm_count = (int)(sizeof mmu_api_vm_entries / sizeof mmu_api_vm_entries[0]);
