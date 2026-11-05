/* KingCobra std.mmu — internal types shared by the mmu_*.c files.
 *
 * Layering (bottom to top):
 *   mmu_arch.c   architecture table, PTE encode/decode, canonical-address rules, fault codes
 *   mmu_phys.c   simulated physical memory: sparse frames, refcounted allocator, device
 *                windows, swap slots
 *   mmu_pt.c     page-table engine: walk, map, unmap, protect, split, iterate, dump, verify
 *   mmu_cpu.c    CPU model: tagged set-associative TLB, permission policy, A/D updates,
 *                fault delivery, virtual-memory access, TLB shootdown
 *   mmu_vm.c     OS-style virtual-memory manager: VMAs, demand paging, copy-on-write, fork,
 *                huge pages, shared memory, swap + clock reclaim
 *   mmu_api*.c   the script-facing natives
 *   mmu_host.c   real host virtual memory (mmap/mprotect/VirtualProtect/pagemap)
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef KC_MMU_INTERNAL_H
#define KC_MMU_INTERNAL_H

#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include "interp.h"
#include <stdarg.h>

#define MMU_FRAME_SHIFT 12
#define MMU_FRAME_SIZE  4096u
#define MMU_MAX_PHYS    ((uint64_t)1 << 38)          /* 256 GiB of *simulated* physical space (sparse) */
#define MMU_CHUNK_FRAMES 256u                        /* frames per allocator chunk */
#define MMU_MAX_DEVICES 16
#define MMU_MAX_CPUS    64
#define MMU_MAX_LEVELS  5

/* ------------------------------------------------------------------ architectures */

typedef enum {
    MMU_X86_32, MMU_X86_64, MMU_X86_64_LA57,
    MMU_ARM64, MMU_ARM64_39,
    MMU_RV_SV32, MMU_RV_SV39, MMU_RV_SV48, MMU_RV_SV57,
    MMU_ARCH_COUNT
} MmuArchId;

typedef enum { FAM_X86_32, FAM_X86_64, FAM_ARM64, FAM_RV32, FAM_RV64 } MmuFamily;

typedef struct {
    const char *name;
    MmuArchId   id;
    MmuFamily   fam;
    int         levels;      /* walk depth: level 0 = 4 KiB leaf ... levels-1 = root */
    int         va_bits;     /* translated virtual-address width */
    int         pa_bits;     /* physical-address bits the PTE can hold */
    int         pte_size;    /* bytes per entry: 4 or 8 */
    int         idx_bits;    /* index bits per level (table = exactly one 4 KiB frame) */
    uint32_t    leaf_mask;   /* bit L set => a leaf (block) entry is legal at level L */
    int         asid_bits;
    bool        split_va;    /* arm64: two roots (TTBR0 low half / TTBR1 high half) */
    bool        has_nx;
    const char *family_name; /* "x86", "arm64", "riscv" */
    const char *root_reg;    /* "cr3", "ttbr0", "satp" */
} MmuArch;

extern const MmuArch MMU_ARCHS[MMU_ARCH_COUNT];
const MmuArch *mmu_arch_by_name(const char *name);
static inline int      mmu_shift(const MmuArch *A, int L) { return MMU_FRAME_SHIFT + L * A->idx_bits; }
static inline uint64_t mmu_page_size(const MmuArch *A, int L) { return (uint64_t)1 << mmu_shift(A, L); }
static inline int      mmu_entries(const MmuArch *A) { return 1 << A->idx_bits; }

/* cache attribute classes */
enum { MC_WB = 0, MC_WC, MC_NC, MC_UC, MC_WT, MC_WP, MC_COUNT };
extern const char *const MMU_CACHE_NAMES[MC_COUNT];

/* software-available PTE bits every architecture offers (>= 2) */
#define SW_COW 1u
#define SW_OWN 2u

typedef enum { PK_INVALID = 0, PK_TABLE, PK_LEAF, PK_BAD, PK_SPECIAL } PteKind;
enum { SP_SWAP = 0, SP_PROTNONE = 1 };

typedef struct {
    PteKind kind;
    bool    r, w, x, user, accessed, dirty, global;
    uint8_t cache, sw;
    uint64_t pa;                 /* next-level table or page/block base (or special payload) */
    /* hierarchical restrictions carried by a table entry */
    bool    t_ro, t_nouser, t_xn_u, t_xn_p;
    /* special (non-present) entry payload */
    int     sp_type;
    bool    sp_cow, sp_own, sp_global;
    const char *bad_reason;
} Pte;

/* result of an architecture-level classify/decode */
void     mmu_pte_decode(const MmuArch *A, int level, uint64_t raw, Pte *out);
uint64_t mmu_pte_encode_leaf(const MmuArch *A, int level, const Pte *p, char *err, size_t errn); /* 0 + err on failure */
uint64_t mmu_pte_encode_table(const MmuArch *A, uint64_t table_pa);
uint64_t mmu_pte_set_ad(const MmuArch *A, int level, uint64_t raw, bool table_entry, bool set_a, bool set_d);
uint64_t mmu_pte_clear_a(const MmuArch *A, uint64_t raw);
bool     mmu_raw_is_special(uint64_t raw);
uint64_t mmu_pte_special(int type, uint64_t payload, bool cow, bool own, bool global);
bool     mmu_cache_supported(const MmuArch *A, int cache);
uint64_t mmu_canonical(const MmuArch *A, uint64_t va);          /* sign/zero-extend to the canonical form */
bool     mmu_is_canonical(const MmuArch *A, uint64_t va);
int      mmu_va_half(const MmuArch *A, uint64_t va);             /* 0 = low/only, 1 = high (arm64 TTBR1) */
uint64_t mmu_make_va(const MmuArch *A, int half, uint64_t lowbits);
uint64_t mmu_root_reg(const MmuArch *A, uint64_t root_pa, uint32_t asid, int half);
uint64_t mmu_user_top(const MmuArch *A);                         /* first address above the user half */

/* ------------------------------------------------------------------ faults */

typedef enum {
    FK_NONE = 0, FK_NOT_PRESENT, FK_PROTECTION, FK_RESERVED, FK_NONCANONICAL,
    FK_ACCESS_FLAG, FK_DIRTY, FK_BUS, FK_COUNT
} FaultKind;
extern const char *const MMU_FAULT_NAMES[FK_COUNT];

typedef enum { ACC_READ = 0, ACC_WRITE = 1, ACC_EXEC = 2 } Access;

typedef struct {
    FaultKind kind;
    uint64_t  va;
    Access    access;
    bool      user;
    int       level;          /* level at which the walk failed */
    uint64_t  raw;            /* offending entry */
    uint64_t  code;           /* architecture-native status (x86 #PF error code, arm64 FSC, riscv scause) */
    const char *vector;       /* "#PF", "#GP", "DABT"/"IABT", "load page fault" ... */
    int       segv;           /* 0 none, 1 = address not in any VMA, 2 = VMA denies this access */
    bool      swapped;        /* the missing page is in swap */
    bool      cow;            /* write to a copy-on-write page */
} Fault;

void mmu_fault_fill_code(const MmuArch *A, Fault *f);
void mmu_fault_describe(const MmuArch *A, const Fault *f, char *out, size_t n);

/* ------------------------------------------------------------------ physical memory */

typedef struct {
    uint32_t ref[MMU_CHUNK_FRAMES];
    uint8_t *frame[MMU_CHUNK_FRAMES];
    uint16_t used;
} MmuChunk;

typedef struct {
    uint64_t base, size;
    KcValue  rd, wr;
    char     name[32];
    uint64_t reads, writes;
} MmuDevice;

#define REF_RESERVED 0x80000000u

typedef struct MmuPhys {
    uint64_t   size, nframes, nchunks;
    MmuChunk **chunks;
    uint64_t   hint, used_frames, reserved_frames;
    MmuDevice  dev[MMU_MAX_DEVICES];
    int        ndev;
    /* swap */
    uint8_t  **swap;            /* slot data (lazily allocated) */
    uint8_t   *swap_used;
    uint32_t   swap_slots, swap_hint, swap_in_use;
    uint64_t   swap_outs, swap_ins;
    int        refs;            /* live spaces + shms + the script handle */
    bool       handle_alive;
    char       name[32];
    uint64_t   phys_reads, phys_writes;
} MmuPhys;

MmuPhys *mmu_phys_new(uint64_t size, const char *name);
void     mmu_phys_release(MmuPhys *p);                    /* drop one reference */
bool     mmu_phys_in_range(const MmuPhys *p, uint64_t pa, uint64_t len);
bool     mmu_phys_alloc(MmuPhys *p, uint64_t count, uint64_t align_frames, uint64_t *pa_out); /* zeroed, ref 1 each */
void     mmu_phys_ref_inc(MmuPhys *p, uint64_t pa);
uint32_t mmu_phys_ref_dec(MmuPhys *p, uint64_t pa);       /* frees at 0, returns new count */
uint32_t mmu_phys_refcount(const MmuPhys *p, uint64_t pa);
bool     mmu_phys_frame_live(const MmuPhys *p, uint64_t pa);
/* raw accessors: return false if out of range. Device windows invoke script callbacks. */
bool     mmu_phys_read(MmuPhys *p, uint64_t pa, void *dst, size_t len);
bool     mmu_phys_write(MmuPhys *p, uint64_t pa, const void *src, size_t len);
bool     mmu_phys_read_u(MmuPhys *p, uint64_t pa, int width, uint64_t *out);
bool     mmu_phys_write_u(MmuPhys *p, uint64_t pa, int width, uint64_t v);
bool     mmu_phys_copy_frame(MmuPhys *p, uint64_t dst_pa, uint64_t src_pa);
bool     mmu_phys_frame_is_zero(MmuPhys *p, uint64_t pa);
bool     mmu_phys_add_device(MmuPhys *p, uint64_t base, uint64_t size, KcValue rd, KcValue wr, const char *name);
MmuDevice *mmu_phys_find_device(MmuPhys *p, uint64_t pa);
bool     mmu_swap_init(MmuPhys *p, uint32_t slots);
bool     mmu_swap_alloc(MmuPhys *p, uint32_t *slot);
void     mmu_swap_free(MmuPhys *p, uint32_t slot);
bool     mmu_swap_store(MmuPhys *p, uint32_t slot, uint64_t frame_pa);
bool     mmu_swap_load(MmuPhys *p, uint32_t slot, uint64_t frame_pa);

/* callback context (set by every native before it can reach a device callback) */
extern KcInterp *MMU_IP;
extern KcPos     MMU_POS;
extern int       MMU_CB_DEPTH;

/* ------------------------------------------------------------------ spaces, cpus */

typedef struct MmuShm {
    MmuPhys *phys; uint64_t len; uint64_t npages; uint64_t *frames; int refs; bool alive; char name[24];
} MmuShm;

enum { VF_SHARED = 1, VF_GROWSDOWN = 2, VF_LOCKED = 4, VF_HUGE = 8, VF_GUARD = 16 };
enum { PROT_R = 1, PROT_W = 2, PROT_X = 4, PROT_U = 8, PROT_G = 16 };

typedef struct {
    uint64_t start, end;       /* [start, end) page aligned */
    uint32_t prot;             /* PROT_* */
    uint32_t flags;            /* VF_* */
    uint8_t  cache;
    MmuShm  *shm; uint64_t shm_off;
    char     name[24];
} MmuVma;

typedef struct MmuCpu MmuCpu;

typedef struct MmuSpace {
    int        id;
    const MmuArch *A;
    MmuPhys   *phys;
    uint64_t   root[2]; bool has_root[2];
    uint32_t   asid;
    MmuVma    *vmas; int nvma, cap_vma;
    uint64_t   mmap_cursor, clock_hand, stack_max;
    MmuCpu    *home;              /* private cpu used when a script accesses through the space */
    bool       alive;
    char       name[32];
    /* counters */
    uint64_t   n_demand, n_cow, n_swapin, n_swapout, n_ad_fix, n_huge, n_splits, n_fork_cow, n_zapped, n_tables;
} MmuSpace;

typedef struct {
    bool     valid, global, r, w, x, user, dirty;
    uint8_t  cache;
    uint32_t asid;
    uint64_t vpn;                 /* va >> shift(level) */
    uint64_t pa;                  /* page/block base */
    uint64_t lru;
} MmuTlbEnt;

typedef struct { MmuTlbEnt *e; int sets, ways; } MmuTlbBank;

struct MmuCpu {
    int        id;
    const MmuArch *A;
    MmuSpace  *cur;
    MmuTlbBank bank[MMU_MAX_LEVELS];
    uint64_t   tick;
    bool       use_asid, manual_tlb, sw_ad, wp, smep, smap, strict, private_cpu;
    int        tlb_entries, tlb_ways;
    KcValue    on_fault; bool has_handler; bool in_handler;
    Fault      last_fault; bool has_last_fault;
    uint64_t   tlb_hits, tlb_misses, tlb_evictions, tlb_flushes, tlb_invlpg, walks, faults, handled_faults, switches, accesses;
    char       name[32];
    bool       alive;
};

typedef struct {
    bool     ok;
    Fault    fault;
    uint64_t pa;                  /* translated address (with offset) */
    int      level;               /* leaf level */
    uint64_t page_size, page_va, page_pa;
    bool     r, w, x, user, global, dirty, accessed, from_tlb;
    uint8_t  cache, sw;
    uint64_t raw, pte_pa;
} MmuXlate;

/* tracing walk record */
typedef struct { int level; uint64_t table_pa; int index; uint64_t raw; PteKind kind; } WalkStep;
typedef struct {
    bool      ok;
    Fault     fault;
    int       nsteps; WalkStep steps[MMU_MAX_LEVELS];
    int       level;               /* leaf level */
    Pte       leaf;
    uint64_t  raw, pte_pa, page_va;
    bool      t_ro, t_nouser, t_xn_u, t_xn_p;
    bool      special; Pte sp;     /* non-present special entry encountered */
} WalkResult;

/* ---- mmu_pt.c */
bool mmu_space_create(MmuSpace *S, char *err, size_t errn);
bool mmu_root_for(MmuSpace *S, uint64_t va, bool create, uint64_t *root_pa);
void mmu_walk(MmuSpace *S, uint64_t va, WalkResult *w);
typedef struct {
    uint64_t va; int level; uint64_t pte_pa; uint64_t raw; Pte pte;
} PtLeaf;
typedef bool (*PtLeafFn)(MmuSpace *S, void *ud, const PtLeaf *lf);        /* return false to stop */
bool mmu_pt_iter(MmuSpace *S, uint64_t lo, uint64_t hi_incl, PtLeafFn leaf_fn, void *ud, bool visit_special);
bool mmu_pt_map(MmuSpace *S, uint64_t va, int level, uint64_t raw_leaf, bool force, char *err, size_t errn);
bool mmu_pt_map_pte(MmuSpace *S, uint64_t va, int level, const Pte *p, bool force, char *err, size_t errn);
bool mmu_pt_locate(MmuSpace *S, uint64_t va, uint64_t *table_pa, int *idx, int *level); /* slot holding the leaf/special covering va */
bool mmu_rd_pte(MmuSpace *S, uint64_t table_pa, int idx, uint64_t *raw);
bool mmu_wr_pte(MmuSpace *S, uint64_t table_pa, int idx, uint64_t raw);
int64_t mmu_pt_unmap(MmuSpace *S, uint64_t va, uint64_t len, char *err, size_t errn);
int64_t mmu_pt_protect(MmuSpace *S, uint64_t va, uint64_t len, int set_mask, int set_val, bool set_cache, int cache,
                       bool clear_ad, char *err, size_t errn);
bool mmu_pt_split(MmuSpace *S, uint64_t va, char *err, size_t errn);       /* split the leaf covering va one level down */
bool mmu_pt_rewrite_leaf(MmuSpace *S, const PtLeaf *lf, uint64_t new_raw);
void mmu_pt_release_leaf(MmuSpace *S, const Pte *pte, int level);
void mmu_pt_destroy(MmuSpace *S);
char *mmu_pt_dump(MmuSpace *S, uint64_t lo, uint64_t hi_incl);
KcValue mmu_pt_mappings(MmuSpace *S, uint64_t lo, uint64_t hi_incl);
KcValue mmu_pt_verify(MmuSpace *S);
void mmu_pt_stats(MmuSpace *S, uint64_t *pages_by_level, uint64_t *tables_by_level, uint64_t *swapped, uint64_t *protnone, uint64_t *owned);
bool mmu_range_ok(const MmuSpace *S, uint64_t va, uint64_t len, uint64_t *last_out, char *err, size_t errn);
void mmu_flags_str(char *out, size_t n, const Pte *p);

/* ---- mmu_cpu.c */
MmuCpu *mmu_cpu_create(const MmuArch *A, const char *name, char *err, size_t errn);
void    mmu_cpu_destroy(MmuCpu *c);
bool    mmu_cpu_parse_opts(MmuCpu *c, const char *opts, int *asid_out, bool *asid_given, char *err, size_t errn);
void    mmu_tlb_configure(MmuCpu *c, int entries, int ways);
void    mmu_tlb_flush(MmuCpu *c, bool include_global);
void    mmu_tlb_invalidate(MmuCpu *c, uint64_t va, int asid /* -1 = all */);
void    mmu_tlb_shootdown(MmuSpace *S, uint64_t va, uint64_t len);
void    mmu_tlb_shootdown_all(MmuSpace *S);
void    mmu_cpu_switch(MmuCpu *c, MmuSpace *S, bool flush);
void    mmu_xlate(MmuCpu *c, uint64_t va, Access acc, bool user, MmuXlate *out, bool probe /* no A/D, no TLB fill, no handlers */);
/* data access through the MMU: false => *f describes the fault (already delivered to handlers) */
bool    mmu_vread(MmuCpu *c, uint64_t va, void *dst, size_t len, bool user, Fault *f);
bool    mmu_vwrite(MmuCpu *c, uint64_t va, const void *src, size_t len, bool user, Fault *f);
bool    mmu_vfetch_check(MmuCpu *c, uint64_t va, bool user, Fault *f);
void    mmu_deliver_fault_strict(MmuCpu *c, const Fault *f);
extern MmuCpu *MMU_CPUS[MMU_MAX_CPUS];
extern int MMU_LAST_FAULT_VALID;
extern Fault MMU_LAST_FAULT;
extern const MmuArch *MMU_LAST_FAULT_ARCH;

/* ---- mmu_vm.c */
typedef struct { bool ok; char err[160]; } VmResult;
bool mmu_vm_fault(MmuCpu *c, MmuSpace *S, Fault *f);                       /* built-in handler: true => resolved */
bool mmu_vm_mmap(MmuSpace *S, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, uint8_t cache, bool fixed, bool noreplace,
                 bool populate, MmuShm *shm, uint64_t shm_off, const char *name, uint64_t *out, char *err, size_t errn);
bool mmu_vm_munmap(MmuSpace *S, uint64_t addr, uint64_t len, char *err, size_t errn);
bool mmu_vm_mprotect(MmuSpace *S, uint64_t addr, uint64_t len, uint32_t prot, char *err, size_t errn);
bool mmu_vm_madvise(MmuSpace *S, uint64_t addr, uint64_t len, const char *advice, char *err, size_t errn);
bool mmu_vm_mlock(MmuSpace *S, uint64_t addr, uint64_t len, bool lock, char *err, size_t errn);
bool mmu_vm_populate(MmuSpace *S, uint64_t addr, uint64_t len, bool write, char *err, size_t errn);
MmuSpace *mmu_vm_fork(MmuSpace *parent, const char *name, char *err, size_t errn);
int64_t mmu_vm_reclaim(MmuSpace *S, int64_t want, int64_t *scanned);
bool mmu_vm_evict(MmuSpace *S, uint64_t va, char *err, size_t errn);
bool mmu_vm_swap_in_all(MmuSpace *S, char *err, size_t errn);
MmuVma *mmu_vma_find(MmuSpace *S, uint64_t va);
void mmu_vm_destroy(MmuSpace *S);
MmuSpace *mmu_space_new(const MmuArch *A, MmuPhys *phys, const char *name, const char *opts, int asid_req, char *err, size_t errn);
void mmu_vm_default_pte(const MmuVma *v, Pte *p);
MmuShm *mmu_shm_new(MmuPhys *p, uint64_t len, const char *name);
void mmu_shm_release(MmuShm *s);

/* ---- script-facing glue (mmu_api.c) */
#define H_MMU_KIND H_MMU
enum { MH_PHYS = 1, MH_SPACE = 2, MH_CPU = 3, MH_SHM = 4, MH_HOST = 5 };
KcValue mmuh_wrap(int sub, void *p);
void   *mmuh_get(KcValue v, int sub);
char   *kc_mmu_display(KcHandle *h);

typedef struct { const char *name; KcNativeFn fn; } MmuEntry;
extern const MmuEntry mmu_api_entries[];   extern const int mmu_api_count;     /* mmu_api.c        */
extern const MmuEntry mmu_api_vm_entries[]; extern const int mmu_api_vm_count; /* mmu_api_vm.c     */
extern const MmuEntry mmu_host_entries[];  extern const int mmu_host_count;    /* mmu_host.c       */
KcModule *kc_mod_mmu(void);

typedef struct {
    uint32_t prot; int cache; bool cache_set; bool rel; uint32_t add, del;
    bool a, d, cow, own, force, huge, fixed, noreplace, populate, growsdown, locked, guard, none, shared;
} MFlags;
void  mm_parse_flags(const char *s, MFlags *F, KcPos pos, const char *who);
void *mm_need_handle(KcValue *a, int n, int i, int sub, KcPos pos, const char *who);
void  mm_oset(KcValue o, const char *k, KcValue v);
bool  mmu_host_destroy(KcHandle *h);

#define MM_ENTER() do { MMU_IP = ip; MMU_POS = pos; } while (0)
#define MM_ERR(...) kc_error_at(pos, __VA_ARGS__)

uint64_t mm_arg_addr(KcValue *a, int n, int i, KcPos pos, const char *who);
int64_t  mm_arg_int(KcValue *a, int n, int i, KcPos pos, const char *who);
uint64_t mm_arg_size(KcValue *a, int n, int i, KcPos pos, const char *who);
const char *mm_arg_str(KcValue *a, int n, int i, KcPos pos, const char *who);
void     mm_need(int n, int lo, int hi, KcPos pos, const char *usage);
void     mm_set_error(const char *fmt, ...);
const char *mm_last_error_str(void);
KcValue  mm_fault_object(const MmuArch *A, const Fault *f);
KcValue  mm_str_bytes(const void *data, size_t len);
KcValue  mm_hex_value(uint64_t v);

#endif
