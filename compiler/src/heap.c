#include "kc_common.h"
#include "heap.h"

#define KC_HEAP_CLASSES 32           /* 16, 32, ... 512 bytes */
#define KC_HEAP_SMALL_MAX 512u
#define KC_HEAP_CHUNK (1u << 20)
#define KC_QUARANTINE_DEFAULT 4096

typedef KcFreeNode FreeNode;
typedef struct { void *p; size_t n; } QEnt;

KcFreeNode *kc_heap_lists[KC_HEAP_CLASSES + 1];
char *kc_heap_cur = NULL, *kc_heap_end = NULL;
KcHeapStats kc_heap_st = { 0, 0, 0, 0, 0, 0, KC_QUARANTINE_DEFAULT };
#define g_free kc_heap_lists
#define g_cur kc_heap_cur
#define g_end kc_heap_end
#define g_st kc_heap_st

static QEnt *g_q = NULL;
static size_t g_qcap = KC_QUARANTINE_DEFAULT, g_qlen = 0, g_qhead = 0;

void *kc_heap_alloc_slow(size_t n) {
    if (n == 0) n = 1;
#ifdef KC_HEAP_NOPOOL            /* debug: plain malloc, so AddressSanitizer sees every block */
    g_st.allocs++; g_st.bytes_in_use += n;
    return kc_malloc(n);
#else
    g_st.allocs++;
    if (n > KC_HEAP_SMALL_MAX) { g_st.bytes_in_use += n; return kc_malloc(n); }
    size_t sz = ((n + 15) >> 4) << 4;
    g_st.bytes_in_use += sz;
    g_cur = kc_malloc(KC_HEAP_CHUNK);             /* chunk exhausted: start a new one */
    g_end = g_cur + KC_HEAP_CHUNK;
    g_st.bytes_reserved += KC_HEAP_CHUNK;
    void *p = g_cur;
    g_cur += sz;
    return p;
#endif
}

/* bytes this block is accounted as (small blocks are size-class rounded) */
static size_t acct(size_t n) {
    if (n == 0) n = 1;
#ifdef KC_HEAP_NOPOOL
    return n;
#else
    return n > KC_HEAP_SMALL_MAX ? n : ((n + 15) >> 4) << 4;
#endif
}

/* return the block to the allocator, no accounting */
static void raw_free(void *p, size_t n) {
    if (n == 0) n = 1;
#ifdef KC_HEAP_NOPOOL
    free(p);
#else
    if (n > KC_HEAP_SMALL_MAX) { free(p); return; }
    size_t c = (n + 15) >> 4;
    FreeNode *f = p;
    f->next = g_free[c];
    g_free[c] = f;
#endif
}

void kc_heap_free(void *p, size_t n) {
    if (!p) return;
    g_st.frees++;
    g_st.bytes_in_use -= acct(n);
    raw_free(p, n);
}

/* The program is done with the block now (it counts as freed, and its bytes leave
 * bytes_in_use), but it is only recycled after `quarantine_cap` further frees. */
void kc_heap_defer_free(void *p, size_t n) {
    if (!p) return;
    if (g_qcap == 0) { kc_heap_free(p, n); return; }
    g_st.frees++;
    g_st.bytes_in_use -= acct(n);
    g_st.quarantine_bytes += acct(n);
    if (!g_q) g_q = kc_malloc(sizeof(QEnt) * g_qcap);
    if (g_qlen == g_qcap) {                       /* ring full: really recycle the oldest */
        QEnt old = g_q[g_qhead];
        g_st.quarantine_bytes -= acct(old.n);
        raw_free(old.p, old.n);
        g_qhead = (g_qhead + 1) % g_qcap;
        g_qlen--;
    }
    g_q[(g_qhead + g_qlen) % g_qcap].p = p;
    g_q[(g_qhead + g_qlen) % g_qcap].n = n;
    g_qlen++;
    g_st.quarantined = g_qlen;
}

void kc_heap_set_quarantine(size_t capacity) {
    for (size_t i = 0; i < g_qlen; i++) {         /* flush: recycle everything held */
        QEnt e = g_q[(g_qhead + i) % g_qcap];
        raw_free(e.p, e.n);
    }
    free(g_q);
    g_q = NULL; g_qlen = 0; g_qhead = 0; g_qcap = capacity;
    g_st.quarantined = 0; g_st.quarantine_bytes = 0; g_st.quarantine_cap = capacity;
}

const KcHeapStats *kc_heap_stats(void) { return &g_st; }
