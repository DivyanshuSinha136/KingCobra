/* KingCobra heap: size-class free lists + a free quarantine.
 *
 * KingCobra has NO garbage collector. Memory is managed explicitly:
 * `free(x)` returns an object's storage here, and the allocator reuses it.
 *
 * Techniques (all classic C allocator design):
 *   - segregated free lists: requests up to 512 bytes are rounded to a
 *     multiple of 16 and served from a per-size LIFO list (pop = alloc,
 *     push = free) or by bumping a pointer in a 1 MiB chunk;
 *   - sized free: the caller passes the block size, so blocks carry no header;
 *   - quarantine: a freed object's header is held in a FIFO ring before it
 *     is recycled, so a stale reference to a recently freed object still
 *     finds the "freed" mark instead of aliasing a brand-new object.
 */
#ifndef KC_HEAP_H
#define KC_HEAP_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t allocs, frees;          /* heap blocks handed out / returned */
    uint64_t bytes_in_use;           /* bytes the program currently holds (size-class rounded) */
    uint64_t bytes_reserved;         /* chunk memory obtained from the OS */
    uint64_t quarantined;            /* blocks currently held in quarantine */
    uint64_t quarantine_bytes;       /* their size: freed by the program, not yet recyclable */
    uint64_t quarantine_cap;
} KcHeapStats;

typedef struct KcFreeNode { struct KcFreeNode *next; } KcFreeNode;
extern KcFreeNode *kc_heap_lists[33];
extern char *kc_heap_cur, *kc_heap_end;
extern KcHeapStats kc_heap_st;
void *kc_heap_alloc_slow(size_t n);             /* large blocks, new chunk, debug mode */

/* Fast path, inlined at every call site: pop a size-class list, else bump. */
static inline void *kc_heap_alloc(size_t n) {
#ifndef KC_HEAP_NOPOOL
    if (__builtin_expect(n - 1 < 512, 1)) {     /* 1..512 */
        size_t c = (n + 15) >> 4, sz = c << 4;
        KcFreeNode *f = kc_heap_lists[c];
        if (f) {
            kc_heap_lists[c] = f->next;
            kc_heap_st.allocs++; kc_heap_st.bytes_in_use += sz;
            return f;
        }
        if (__builtin_expect((size_t)(kc_heap_end - kc_heap_cur) >= sz, 1)) {
            void *p = kc_heap_cur;
            kc_heap_cur += sz;
            kc_heap_st.allocs++; kc_heap_st.bytes_in_use += sz;
            return p;
        }
    }
#endif
    return kc_heap_alloc_slow(n);
}
void kc_heap_free(void *p, size_t n);
void kc_heap_defer_free(void *p, size_t n);   /* free after the quarantine delay */
void kc_heap_set_quarantine(size_t capacity); /* 0 disables; flushes what is held */
const KcHeapStats *kc_heap_stats(void);

#endif
