/* KingCobra std.mmu — architecture layer: page-table entry formats for x86 (32-bit non-PAE, x86-64
 * 4/5-level), ARM64 (4 KiB granule, 39/48-bit) and RISC-V (Sv32/39/48/57), canonical-address rules,
 * root-register encodings, and the architecture-native fault status codes.
 *
 * Every format is decoded into the same neutral Pte (r/w/x/user/accessed/dirty/global/cache + two
 * software bits) so the walker, TLB and VM manager are architecture independent.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

const MmuArch MMU_ARCHS[MMU_ARCH_COUNT] = {
    /* name           id              family       lv va  pa  pte idx leaf_mask            asid split nx    fam      reg */
    { "x86_32",      MMU_X86_32,     FAM_X86_32,  2, 32, 32, 4, 10, 1u << 1,              0,  false, false, "x86",   "cr3"   },
    { "x86_64",      MMU_X86_64,     FAM_X86_64,  4, 48, 52, 8, 9,  (1u << 1) | (1u << 2), 12, false, true,  "x86",   "cr3"   },
    { "x86_64_la57", MMU_X86_64_LA57, FAM_X86_64, 5, 57, 52, 8, 9,  (1u << 1) | (1u << 2), 12, false, true,  "x86",   "cr3"   },
    { "arm64",       MMU_ARM64,      FAM_ARM64,   4, 48, 48, 8, 9,  (1u << 1) | (1u << 2), 16, true,  true,  "arm64", "ttbr0" },
    { "arm64_39",    MMU_ARM64_39,   FAM_ARM64,   3, 39, 48, 8, 9,  (1u << 1) | (1u << 2), 16, true,  true,  "arm64", "ttbr0" },
    { "riscv_sv32",  MMU_RV_SV32,    FAM_RV32,    2, 32, 34, 4, 10, 1u << 1,              9,  false, true,  "riscv", "satp"  },
    { "riscv_sv39",  MMU_RV_SV39,    FAM_RV64,    3, 39, 56, 8, 9,  (1u << 1) | (1u << 2), 16, false, true,  "riscv", "satp"  },
    { "riscv_sv48",  MMU_RV_SV48,    FAM_RV64,    4, 48, 56, 8, 9,  (1u << 1) | (1u << 2) | (1u << 3), 16, false, true, "riscv", "satp" },
    { "riscv_sv57",  MMU_RV_SV57,    FAM_RV64,    5, 57, 56, 8, 9,  (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4), 16, false, true, "riscv", "satp" },
};

const char *const MMU_CACHE_NAMES[MC_COUNT] = { "wb", "wc", "nc", "uc", "wt", "wp" };
const char *const MMU_FAULT_NAMES[FK_COUNT] = {
    "none", "not_present", "protection", "reserved", "non_canonical", "access_flag", "dirty", "bus"
};

const MmuArch *mmu_arch_by_name(const char *name) {
    for (int i = 0; i < MMU_ARCH_COUNT; i++) if (!strcmp(MMU_ARCHS[i].name, name)) return &MMU_ARCHS[i];
    /* convenient aliases */
    if (!strcmp(name, "x86") || !strcmp(name, "i386")) return &MMU_ARCHS[MMU_X86_32];
    if (!strcmp(name, "amd64") || !strcmp(name, "x86-64") || !strcmp(name, "x64")) return &MMU_ARCHS[MMU_X86_64];
    if (!strcmp(name, "aarch64") || !strcmp(name, "arm64_48")) return &MMU_ARCHS[MMU_ARM64];
    if (!strcmp(name, "sv32") || !strcmp(name, "rv32")) return &MMU_ARCHS[MMU_RV_SV32];
    if (!strcmp(name, "sv39") || !strcmp(name, "riscv64") || !strcmp(name, "riscv")) return &MMU_ARCHS[MMU_RV_SV39];
    if (!strcmp(name, "sv48")) return &MMU_ARCHS[MMU_RV_SV48];
    if (!strcmp(name, "sv57")) return &MMU_ARCHS[MMU_RV_SV57];
    if (!strcmp(name, "la57")) return &MMU_ARCHS[MMU_X86_64_LA57];
    return NULL;
}

/* ------------------------------------------------------------------ address rules */

static uint64_t va_mask(const MmuArch *A) { return A->va_bits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << A->va_bits) - 1); }

bool mmu_is_canonical(const MmuArch *A, uint64_t va) {
    if (A->pte_size == 4) return va <= 0xFFFFFFFFull;
    int s = 64 - A->va_bits;
    if (A->fam == FAM_ARM64) { uint64_t top = va >> A->va_bits; uint64_t all = ((uint64_t)1 << s) - 1; return top == 0 || top == all; }
    return (uint64_t)((int64_t)(va << s) >> s) == va;
}

uint64_t mmu_canonical(const MmuArch *A, uint64_t va) {
    if (A->pte_size == 4) return va & 0xFFFFFFFFull;
    int s = 64 - A->va_bits;
    if (A->fam == FAM_ARM64) { return (va & ((uint64_t)1 << (A->va_bits - 1))) ? (va | ~va_mask(A)) : (va & va_mask(A)); }
    return (uint64_t)((int64_t)(va << s) >> s);
}

int mmu_va_half(const MmuArch *A, uint64_t va) { return A->split_va ? (int)(va >> 63) & 1 : 0; }

uint64_t mmu_make_va(const MmuArch *A, int half, uint64_t lowbits) {
    lowbits &= va_mask(A);
    if (A->pte_size == 4) return lowbits;
    if (A->fam == FAM_ARM64) return half ? (lowbits | ~va_mask(A)) : lowbits;
    int s = 64 - A->va_bits;
    return (uint64_t)((int64_t)(lowbits << s) >> s);
}

uint64_t mmu_user_top(const MmuArch *A) {
    if (A->pte_size == 4) return (uint64_t)1 << 32;
    return (uint64_t)1 << (A->va_bits - (A->fam == FAM_ARM64 ? 0 : 1));
}

uint64_t mmu_root_reg(const MmuArch *A, uint64_t root_pa, uint32_t asid, int half) {
    (void)half;
    switch (A->fam) {
    case FAM_X86_32: return root_pa & 0xFFFFFFFFull;
    case FAM_X86_64: return root_pa | (asid & 0xFFFu);
    case FAM_ARM64:  return root_pa | ((uint64_t)(asid & 0xFFFFu) << 48);
    case FAM_RV32:   return ((uint64_t)1 << 31) | ((uint64_t)(asid & 0x1FFu) << 22) | ((root_pa >> 12) & 0x3FFFFFull);
    case FAM_RV64: {
        uint64_t mode = A->id == MMU_RV_SV39 ? 8 : A->id == MMU_RV_SV48 ? 9 : 10;
        return (mode << 60) | ((uint64_t)(asid & 0xFFFFu) << 44) | ((root_pa >> 12) & (((uint64_t)1 << 44) - 1));
    }
    }
    return root_pa;
}

/* ------------------------------------------------------------------ helpers */

static uint64_t pa_field_mask(int lo_shift, int pa_bits) {      /* bits [pa_bits-1 : lo_shift] */
    uint64_t hi = pa_bits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << pa_bits) - 1);
    return hi & ~(((uint64_t)1 << lo_shift) - 1);
}

#define SPECIAL_MARK 0x802ull
bool mmu_raw_is_special(uint64_t raw) { return (raw & 1) == 0 && (raw & SPECIAL_MARK) == SPECIAL_MARK; }

uint64_t mmu_pte_special(int type, uint64_t payload, bool cow, bool own, bool global) {
    uint64_t r = SPECIAL_MARK | ((uint64_t)(type & 3) << 2) | (cow ? 0x10u : 0) | (own ? 0x20u : 0) | (global ? 0x40u : 0);
    return r | (payload & ~(uint64_t)0xFFF);
}

static void decode_special(uint64_t raw, Pte *o) {
    o->kind = PK_SPECIAL;
    o->sp_type = (int)((raw >> 2) & 3);
    o->sp_cow = (raw & 0x10) != 0; o->sp_own = (raw & 0x20) != 0; o->sp_global = (raw & 0x40) != 0;
    o->pa = raw & ~(uint64_t)0xFFF;
}

static const uint8_t X86_CACHE_FROM_IDX[8] = { MC_WB, MC_WC, MC_NC, MC_UC, MC_WB, MC_WP, MC_NC, MC_WT };
static int x86_idx_from_cache(int c) {
    switch (c) { case MC_WB: return 0; case MC_WC: return 1; case MC_NC: return 2; case MC_UC: return 3; case MC_WP: return 5; case MC_WT: return 7; }
    return 0;
}

bool mmu_cache_supported(const MmuArch *A, int c) {
    if (c < 0 || c >= MC_COUNT) return false;
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64: return true;
    case FAM_ARM64:  return c != MC_WP;
    default:         return c == MC_WB || c == MC_WC || c == MC_UC;
    }
}

/* ------------------------------------------------------------------ x86 */

static void decode_x86(const MmuArch *A, int L, uint64_t raw, Pte *o) {
    bool is64 = A->fam != FAM_X86_32;
    if (!is64) raw &= 0xFFFFFFFFull;
    if (!(raw & 1)) { if (mmu_raw_is_special(raw)) decode_special(raw, o); return; }
    bool ps = (raw & 0x80) != 0;
    bool leaf = (L == 0) || ps;
    if (L > 0 && ps && !(A->leaf_mask & (1u << L))) { o->kind = PK_BAD; o->bad_reason = "PS bit set at a level that cannot hold a large page"; return; }
    o->r = true;
    o->w = (raw & 2) != 0; o->user = (raw & 4) != 0;
    o->accessed = (raw & 0x20) != 0;
    o->x = is64 ? !(raw >> 63) : true;
    o->sw = (uint8_t)(((raw >> 9) & 1 ? SW_COW : 0) | ((raw >> 10) & 1 ? SW_OWN : 0));
    if (!leaf) {
        o->kind = PK_TABLE;
        o->pa = raw & pa_field_mask(12, A->pa_bits);
        o->t_ro = !o->w; o->t_nouser = !o->user; o->t_xn_u = o->t_xn_p = is64 && (raw >> 63);
        return;
    }
    o->kind = PK_LEAF;
    o->dirty = (raw & 0x40) != 0;
    o->global = (raw & 0x100) != 0;
    int pat = (L == 0) ? (int)((raw >> 7) & 1) : (int)((raw >> 12) & 1);
    o->cache = X86_CACHE_FROM_IDX[((raw >> 3) & 1) | (((raw >> 4) & 1) << 1) | (pat << 2)];
    if (L == 0) {
        o->pa = raw & pa_field_mask(12, A->pa_bits);
    } else {
        int sh = mmu_shift(A, L);
        uint64_t mid = pa_field_mask(13, sh);               /* bits [sh-1:13] must be zero (bit 12 is PAT) */
        if (raw & mid & (is64 ? ~(uint64_t)0 : 0xFFFFFFFFull)) { o->kind = PK_BAD; o->bad_reason = "large page address is not aligned to its size"; return; }
        o->pa = raw & pa_field_mask(sh, A->pa_bits);
    }
}

static uint64_t encode_x86(const MmuArch *A, int L, const Pte *p, char *err, size_t errn) {
    bool is64 = A->fam != FAM_X86_32;
    uint64_t raw = 1;
    if (p->w) raw |= 2;
    if (p->user) raw |= 4;
    if (p->accessed) raw |= 0x20;
    if (p->dirty) raw |= 0x40;
    if (p->global) raw |= 0x100;
    if (is64 && !p->x) raw |= (uint64_t)1 << 63;
    if (p->sw & SW_COW) raw |= (uint64_t)1 << 9;
    if (p->sw & SW_OWN) raw |= (uint64_t)1 << 10;
    int idx = x86_idx_from_cache(p->cache);
    raw |= (uint64_t)(idx & 1) << 3; raw |= (uint64_t)((idx >> 1) & 1) << 4;
    if (L == 0) { raw |= (uint64_t)((idx >> 2) & 1) << 7; }
    else        { raw |= 0x80; raw |= (uint64_t)((idx >> 2) & 1) << 12; }
    int sh = mmu_shift(A, L);
    if (p->pa & (((uint64_t)1 << sh) - 1)) { if (err) snprintf(err, errn, "physical address 0x%llx is not aligned to the %llu-byte page size", (unsigned long long)p->pa, (unsigned long long)mmu_page_size(A, L)); return 0; }
    if (p->pa & ~pa_field_mask(12, A->pa_bits)) { if (err) snprintf(err, errn, "physical address 0x%llx exceeds the %d-bit physical address space of %s", (unsigned long long)p->pa, A->pa_bits, A->name); return 0; }
    return raw | p->pa;
}

/* ------------------------------------------------------------------ arm64 (4 KiB granule) */

static const uint8_t ARM_CACHE_FROM_ATTR[8] = { MC_UC, MC_NC, MC_NC, MC_WC, MC_WB, MC_WT, MC_WB, MC_WB };
static int arm_attr_from_cache(int c) { switch (c) { case MC_UC: return 0; case MC_NC: return 1; case MC_WC: return 3; case MC_WT: return 5; default: return 4; } }

static void decode_arm(const MmuArch *A, int L, uint64_t raw, Pte *o) {
    if (!(raw & 1)) { if (mmu_raw_is_special(raw)) decode_special(raw, o); return; }
    unsigned t = (unsigned)(raw & 3);
    if (L == 0) { if (t != 3) { o->kind = PK_BAD; o->bad_reason = "reserved descriptor encoding at the last level"; return; } }
    else if (t == 3) {
        o->kind = PK_TABLE; o->pa = raw & pa_field_mask(12, 48);
        o->t_ro = (raw >> 62) & 1; o->t_nouser = (raw >> 61) & 1; o->t_xn_u = (raw >> 60) & 1; o->t_xn_p = (raw >> 59) & 1;
        return;
    } else if (!(A->leaf_mask & (1u << L))) { o->kind = PK_BAD; o->bad_reason = "block descriptor at a level that cannot hold blocks"; return; }
    o->kind = PK_LEAF;
    o->r = true;
    o->user = (raw >> 6) & 1;
    bool ro = (raw >> 7) & 1, dbm = (raw >> 51) & 1;
    if (dbm) { o->w = true; o->dirty = !ro; } else { o->w = !ro; o->dirty = o->w; }
    o->accessed = (raw >> 10) & 1;
    o->global = !((raw >> 11) & 1);
    o->x = o->user ? !((raw >> 54) & 1) : !((raw >> 53) & 1);
    o->cache = ARM_CACHE_FROM_ATTR[(raw >> 2) & 7];
    o->sw = (uint8_t)(((raw >> 55) & 1 ? SW_COW : 0) | ((raw >> 56) & 1 ? SW_OWN : 0));
    int sh = mmu_shift(A, L);
    if (L > 0 && (raw & pa_field_mask(12, sh) & ~((uint64_t)1 << 16))) {   /* bit 16 (nT) is the one legal exception */
        o->kind = PK_BAD; o->bad_reason = "block address is not aligned to its size"; return;
    }
    o->pa = raw & pa_field_mask(sh, 48);
}

static uint64_t encode_arm(const MmuArch *A, int L, const Pte *p, char *err, size_t errn) {
    if (!mmu_cache_supported(A, p->cache)) { if (err) snprintf(err, errn, "cache type '%s' is not available on arm64", MMU_CACHE_NAMES[p->cache]); return 0; }
    int sh = mmu_shift(A, L);
    if (p->pa & (((uint64_t)1 << sh) - 1)) { if (err) snprintf(err, errn, "physical address 0x%llx is not aligned to the %llu-byte page size", (unsigned long long)p->pa, (unsigned long long)mmu_page_size(A, L)); return 0; }
    if (p->pa & ~pa_field_mask(12, 48)) { if (err) snprintf(err, errn, "physical address 0x%llx exceeds the 48-bit physical address space", (unsigned long long)p->pa); return 0; }
    uint64_t raw = (L == 0 ? 3u : 1u) | ((uint64_t)arm_attr_from_cache(p->cache) << 2);
    bool dev = (p->cache == MC_UC || p->cache == MC_NC);
    raw |= (uint64_t)(dev ? 2 : 3) << 8;
    if (p->user) raw |= (uint64_t)1 << 6;
    if (p->w) { raw |= (uint64_t)1 << 51; if (!p->dirty) raw |= (uint64_t)1 << 7; } else raw |= (uint64_t)1 << 7;
    if (p->accessed) raw |= (uint64_t)1 << 10;
    if (!p->global) raw |= (uint64_t)1 << 11;
    if (!p->x) raw |= (uint64_t)3 << 53;
    else if (p->user) raw |= (uint64_t)1 << 53;            /* user-executable page: privileged execute-never */
    else raw |= (uint64_t)1 << 54;                          /* kernel-executable page: user execute-never */
    if (p->sw & SW_COW) raw |= (uint64_t)1 << 55;
    if (p->sw & SW_OWN) raw |= (uint64_t)1 << 56;
    return raw | p->pa;
}

/* ------------------------------------------------------------------ RISC-V */

static void decode_rv(const MmuArch *A, int L, uint64_t raw, Pte *o) {
    bool is64 = A->fam == FAM_RV64;
    if (!is64) raw &= 0xFFFFFFFFull;
    if (!(raw & 1)) { if (mmu_raw_is_special(raw)) decode_special(raw, o); return; }
    bool R = raw & 2, W = raw & 4, X = raw & 8, U = raw & 16, G = raw & 32, Ab = raw & 64, D = raw & 128;
    if (is64 && (raw >> 54) & 0x3FF) {
        /* bits 60:54 reserved, 62:61 PBMT, 63 N. PBMT 1/2 are understood; the rest is reserved here. */
        uint64_t hi = raw >> 54;
        if ((hi & 0x7F) || (hi & 0x200) || (((hi >> 7) & 3) == 3)) { o->kind = PK_BAD; o->bad_reason = "reserved bits set in RISC-V PTE (N / reserved / PBMT=3)"; return; }
    }
    uint64_t ppn = is64 ? ((raw >> 10) & (((uint64_t)1 << 44) - 1)) : (raw >> 10);
    uint64_t pa = ppn << 12;
    if (!R && !W && !X) {                                                    /* pointer to next level */
        if (L == 0) { o->kind = PK_BAD; o->bad_reason = "non-leaf entry at the last level"; return; }
        if (U || Ab || D) { o->kind = PK_BAD; o->bad_reason = "A/D/U bits set in a non-leaf entry"; return; }
        if (is64 && ((raw >> 61) & 3)) { o->kind = PK_BAD; o->bad_reason = "PBMT set in a non-leaf entry"; return; }
        o->kind = PK_TABLE; o->pa = pa; return;
    }
    if (W && !R) { o->kind = PK_BAD; o->bad_reason = "writable-but-not-readable encoding is reserved"; return; }
    int sh = mmu_shift(A, L);
    if (L > 0 && (pa & (((uint64_t)1 << sh) - 1))) { o->kind = PK_BAD; o->bad_reason = "misaligned superpage"; return; }
    o->kind = PK_LEAF;
    o->r = R; o->w = W; o->x = X; o->user = U; o->global = G; o->accessed = Ab; o->dirty = D;
    o->cache = is64 ? ((((raw >> 61) & 3) == 1) ? MC_WC : (((raw >> 61) & 3) == 2) ? MC_UC : MC_WB) : MC_WB;
    o->sw = (uint8_t)(((raw >> 8) & 1 ? SW_COW : 0) | ((raw >> 9) & 1 ? SW_OWN : 0));
    o->pa = pa;
}

static uint64_t encode_rv(const MmuArch *A, int L, const Pte *p, char *err, size_t errn) {
    bool is64 = A->fam == FAM_RV64;
    if (!mmu_cache_supported(A, p->cache)) { if (err) snprintf(err, errn, "cache type '%s' is not available on RISC-V (Svpbmt offers wb, wc, uc)", MMU_CACHE_NAMES[p->cache]); return 0; }
    if (!is64 && p->cache != MC_WB) { if (err) snprintf(err, errn, "Sv32 has no page-based memory types"); return 0; }
    bool r = p->r || p->w, x = p->x;
    if (!r && !x) { if (err) snprintf(err, errn, "a RISC-V leaf needs at least one of r/x (an entry with R=W=X=0 is a table pointer)"); return 0; }
    int sh = mmu_shift(A, L);
    if (p->pa & (((uint64_t)1 << sh) - 1)) { if (err) snprintf(err, errn, "physical address 0x%llx is not aligned to the %llu-byte page size", (unsigned long long)p->pa, (unsigned long long)mmu_page_size(A, L)); return 0; }
    if (p->pa & ~pa_field_mask(12, A->pa_bits)) { if (err) snprintf(err, errn, "physical address 0x%llx exceeds the %d-bit physical address space of %s", (unsigned long long)p->pa, A->pa_bits, A->name); return 0; }
    uint64_t raw = 1 | (r ? 2u : 0) | (p->w ? 4u : 0) | (x ? 8u : 0) | (p->user ? 16u : 0) | (p->global ? 32u : 0)
                 | (p->accessed ? 64u : 0) | (p->dirty ? 128u : 0);
    if (p->sw & SW_COW) raw |= 1u << 8;
    if (p->sw & SW_OWN) raw |= 1u << 9;
    raw |= (p->pa >> 12) << 10;
    if (is64) raw |= (uint64_t)(p->cache == MC_WC ? 1 : p->cache == MC_UC ? 2 : 0) << 61;
    return raw;
}

/* ------------------------------------------------------------------ dispatch */

void mmu_pte_decode(const MmuArch *A, int L, uint64_t raw, Pte *out) {
    memset(out, 0, sizeof *out);
    out->kind = PK_INVALID;
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64: decode_x86(A, L, raw, out); break;
    case FAM_ARM64: decode_arm(A, L, raw, out); break;
    default: decode_rv(A, L, raw, out); break;
    }
}

uint64_t mmu_pte_encode_leaf(const MmuArch *A, int L, const Pte *p, char *err, size_t errn) {
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64: return encode_x86(A, L, p, err, errn);
    case FAM_ARM64: return encode_arm(A, L, p, err, errn);
    default: return encode_rv(A, L, p, err, errn);
    }
}

uint64_t mmu_pte_encode_table(const MmuArch *A, uint64_t pa) {
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64: return pa | 1 | 2 | 4;        /* P | RW | US: restrictions live on the leaves */
    case FAM_ARM64: return pa | 3;
    default: return ((pa >> 12) << 10) | 1;
    }
}

uint64_t mmu_pte_set_ad(const MmuArch *A, int L, uint64_t raw, bool table_entry, bool set_a, bool set_d) {
    (void)L;
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64:
        if (set_a) raw |= 0x20;
        if (set_d && !table_entry) raw |= 0x40;
        return raw;
    case FAM_ARM64:
        if (table_entry) return raw;
        if (set_a) raw |= (uint64_t)1 << 10;
        if (set_d && ((raw >> 51) & 1)) raw &= ~((uint64_t)1 << 7);
        return raw;
    default:
        if (table_entry) return raw;
        if (set_a) raw |= 64;
        if (set_d) raw |= 128;
        return raw;
    }
}

uint64_t mmu_pte_clear_a(const MmuArch *A, uint64_t raw) {
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64: return raw & ~(uint64_t)0x20;
    case FAM_ARM64: return raw & ~((uint64_t)1 << 10);
    default: return raw & ~(uint64_t)64;
    }
}

/* ------------------------------------------------------------------ fault status codes */

void mmu_fault_fill_code(const MmuArch *A, Fault *f) {
    int write = f->access == ACC_WRITE, exec = f->access == ACC_EXEC;
    switch (A->fam) {
    case FAM_X86_32: case FAM_X86_64:
        if (f->kind == FK_NONCANONICAL) { f->vector = "#GP"; f->code = 0; return; }
        if (f->kind == FK_BUS) { f->vector = "#MC"; f->code = 0; return; }
        f->vector = "#PF";
        f->code = (f->kind != FK_NOT_PRESENT ? 1u : 0u) | (write ? 2u : 0u) | (f->user ? 4u : 0u) | (f->kind == FK_RESERVED ? 8u : 0u) | (exec ? 16u : 0u);
        return;
    case FAM_ARM64: {
        int lvl = 3 - f->level; if (lvl < 0) lvl = 0; if (lvl > 3) lvl = 3;
        uint64_t fsc;
        switch (f->kind) {
        case FK_NOT_PRESENT: case FK_RESERVED: fsc = 0x04 + (uint64_t)lvl; break;
        case FK_NONCANONICAL: fsc = 0x04; break;
        case FK_ACCESS_FLAG: fsc = 0x08 + (uint64_t)lvl; break;
        case FK_PROTECTION: case FK_DIRTY: fsc = 0x0C + (uint64_t)lvl; break;
        case FK_BUS: fsc = 0x14 + (uint64_t)lvl; break;
        default: fsc = 0; }
        f->vector = exec ? "IABT" : "DABT";
        f->code = fsc | (write ? 0x40u : 0u);
        return;
    }
    default:
        if (f->kind == FK_BUS) { f->code = exec ? 1 : write ? 7 : 5; f->vector = exec ? "instruction access fault" : write ? "store access fault" : "load access fault"; return; }
        f->code = exec ? 12 : write ? 15 : 13;
        f->vector = exec ? "instruction page fault" : write ? "store/AMO page fault" : "load page fault";
        return;
    }
}

void mmu_fault_describe(const MmuArch *A, const Fault *f, char *out, size_t n) {
    static const char *acc[] = { "read of", "write to", "instruction fetch at" };
    const char *what = f->kind == FK_NOT_PRESENT ? (f->swapped ? "page is swapped out" : "page not present")
                     : f->kind == FK_PROTECTION ? (f->cow ? "write to copy-on-write page" : "protection violation")
                     : f->kind == FK_RESERVED ? "reserved/malformed entry"
                     : f->kind == FK_NONCANONICAL ? "non-canonical address"
                     : f->kind == FK_ACCESS_FLAG ? "access flag clear"
                     : f->kind == FK_DIRTY ? "dirty bit clear"
                     : f->kind == FK_BUS ? "physical address out of range (bus error)" : "no fault";
    char extra[64] = "";
    if (A->fam == FAM_X86_32 || A->fam == FAM_X86_64) snprintf(extra, sizeof extra, " [%s err=0x%llx]", f->vector ? f->vector : "#PF", (unsigned long long)f->code);
    else if (A->fam == FAM_ARM64) snprintf(extra, sizeof extra, " [%s fsc=0x%02llx]", f->vector ? f->vector : "", (unsigned long long)(f->code & 0x3F));
    else snprintf(extra, sizeof extra, " [scause=%llu]", (unsigned long long)f->code);
    snprintf(out, n, "%s 0x%016llx (%s mode): %s at level %d%s",
             acc[f->access], (unsigned long long)f->va, f->user ? "user" : "supervisor", what, f->level, extra);
}
