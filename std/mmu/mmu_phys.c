/* KingCobra std.mmu — simulated physical memory.
 *
 * Sparse and lazily backed: a 64 GiB "machine" costs nothing until frames are touched. Frames are
 * handed out by a reference-counted next-fit allocator (single frames, or aligned contiguous runs
 * for huge pages). Frames can also be reserved as *device windows* whose reads/writes call back
 * into script closures, so a translated access can land on a simulated peripheral. A swap area
 * (slot store) backs page eviction. All physical-address arithmetic is overflow-checked.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

KcInterp *MMU_IP = NULL;
KcPos MMU_POS = {0};
int MMU_CB_DEPTH = 0;

static inline uint64_t round_up_u64(uint64_t v, uint64_t a) { return a <= 1 ? v : ((v + a - 1) / a) * a; }

MmuPhys *mmu_phys_new(uint64_t size, const char *name) {
    MmuPhys *p = kc_malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->size = size;
    p->nframes = size >> MMU_FRAME_SHIFT;
    p->nchunks = (p->nframes + MMU_CHUNK_FRAMES - 1) / MMU_CHUNK_FRAMES;
    p->chunks = kc_malloc(sizeof(MmuChunk *) * (size_t)p->nchunks);
    memset(p->chunks, 0, sizeof(MmuChunk *) * (size_t)p->nchunks);
    p->refs = 1; p->handle_alive = true;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "ram");
    return p;
}

void mmu_phys_release(MmuPhys *p) {
    if (!p || --p->refs > 0) return;
    for (uint64_t c = 0; c < p->nchunks; c++) {
        MmuChunk *ch = p->chunks[c];
        if (!ch) continue;
        for (unsigned i = 0; i < MMU_CHUNK_FRAMES; i++) free(ch->frame[i]);
        free(ch);
    }
    free(p->chunks);
    if (p->swap) { for (uint32_t i = 0; i < p->swap_slots; i++) free(p->swap[i]); free(p->swap); free(p->swap_used); }
    free(p);
}

bool mmu_phys_in_range(const MmuPhys *p, uint64_t pa, uint64_t len) {
    return pa < p->size && len <= p->size - pa;
}

static MmuChunk *chunk_get(MmuPhys *p, uint64_t f, bool create) {
    uint64_t ci = f / MMU_CHUNK_FRAMES;
    if (ci >= p->nchunks) return NULL;
    if (!p->chunks[ci] && create) { p->chunks[ci] = kc_malloc(sizeof(MmuChunk)); memset(p->chunks[ci], 0, sizeof(MmuChunk)); }
    return p->chunks[ci];
}

static inline bool frame_used(MmuPhys *p, uint64_t f) {
    MmuChunk *c = chunk_get(p, f, false);
    return c && c->ref[f % MMU_CHUNK_FRAMES] != 0;
}

bool mmu_phys_frame_live(const MmuPhys *p, uint64_t pa) {
    if (pa >= p->size) return false;
    return frame_used((MmuPhys *)p, pa >> MMU_FRAME_SHIFT);
}

uint32_t mmu_phys_refcount(const MmuPhys *p, uint64_t pa) {
    if (pa >= p->size) return 0;
    MmuChunk *c = chunk_get((MmuPhys *)p, pa >> MMU_FRAME_SHIFT, false);
    if (!c) return 0;
    uint32_t r = c->ref[(pa >> MMU_FRAME_SHIFT) % MMU_CHUNK_FRAMES];
    return r & ~REF_RESERVED;
}

static void take_frame(MmuPhys *p, uint64_t f) {
    MmuChunk *c = chunk_get(p, f, true);
    unsigned i = (unsigned)(f % MMU_CHUNK_FRAMES);
    free(c->frame[i]); c->frame[i] = NULL;                /* a freshly allocated frame always reads as zero */
    c->ref[i] = 1; c->used++; p->used_frames++;
}

bool mmu_phys_alloc(MmuPhys *p, uint64_t count, uint64_t align, uint64_t *pa_out) {
    if (count == 0 || count > p->nframes) return false;
    if (align == 0) align = 1;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? round_up_u64(p->hint, align) : 0;
        uint64_t limit = p->nframes;
        while (start + count <= limit) {
            MmuChunk *ch = chunk_get(p, start, false);
            if (ch && ch->used == MMU_CHUNK_FRAMES) { start = round_up_u64((start / MMU_CHUNK_FRAMES + 1) * MMU_CHUNK_FRAMES, align); continue; }
            uint64_t k = 0;
            for (; k < count; k++) if (frame_used(p, start + k)) break;
            if (k == count) {
                for (uint64_t j = 0; j < count; j++) take_frame(p, start + j);
                p->hint = start + count; if (p->hint >= p->nframes) p->hint = 0;
                *pa_out = start << MMU_FRAME_SHIFT;
                return true;
            }
            start = round_up_u64(start + k + 1, align);
        }
    }
    return false;
}

void mmu_phys_ref_inc(MmuPhys *p, uint64_t pa) {
    if (pa >= p->size) return;
    MmuChunk *c = chunk_get(p, pa >> MMU_FRAME_SHIFT, false);
    if (!c) return;
    uint32_t *r = &c->ref[(pa >> MMU_FRAME_SHIFT) % MMU_CHUNK_FRAMES];
    if (*r == 0 || (*r & REF_RESERVED)) return;
    if (*r < 0x7FFFFFFEu) (*r)++;
}

uint32_t mmu_phys_ref_dec(MmuPhys *p, uint64_t pa) {
    if (pa >= p->size) return 0;
    MmuChunk *c = chunk_get(p, pa >> MMU_FRAME_SHIFT, false);
    if (!c) return 0;
    unsigned i = (unsigned)((pa >> MMU_FRAME_SHIFT) % MMU_CHUNK_FRAMES);
    uint32_t *r = &c->ref[i];
    if (*r == 0 || (*r & REF_RESERVED)) return *r & ~REF_RESERVED;
    if (--(*r) == 0) { free(c->frame[i]); c->frame[i] = NULL; c->used--; p->used_frames--; }
    return *r;
}

/* ------------------------------------------------------------------ devices */

MmuDevice *mmu_phys_find_device(MmuPhys *p, uint64_t pa) {
    for (int i = 0; i < p->ndev; i++) if (pa >= p->dev[i].base && pa - p->dev[i].base < p->dev[i].size) return &p->dev[i];
    return NULL;
}

bool mmu_phys_add_device(MmuPhys *p, uint64_t base, uint64_t size, KcValue rd, KcValue wr, const char *name) {
    if (p->ndev >= MMU_MAX_DEVICES) return false;
    if ((base | size) & (MMU_FRAME_SIZE - 1) || size == 0 || !mmu_phys_in_range(p, base, size)) return false;
    for (uint64_t f = base >> MMU_FRAME_SHIFT; f < (base + size) >> MMU_FRAME_SHIFT; f++) if (frame_used(p, f)) return false;
    for (uint64_t f = base >> MMU_FRAME_SHIFT; f < (base + size) >> MMU_FRAME_SHIFT; f++) {
        MmuChunk *c = chunk_get(p, f, true);
        c->ref[f % MMU_CHUNK_FRAMES] = REF_RESERVED; c->used++; p->used_frames++; p->reserved_frames++;
    }
    MmuDevice *d = &p->dev[p->ndev++];
    memset(d, 0, sizeof *d);
    d->base = base; d->size = size; d->rd = rd; d->wr = wr;
    snprintf(d->name, sizeof d->name, "%s", name && *name ? name : "device");
    return true;
}

static bool dev_access(MmuPhys *p, MmuDevice *d, uint64_t pa, uint8_t *buf, size_t len, bool write) {
    if (!MMU_IP || MMU_CB_DEPTH > 6) return false;
    size_t done = 0;
    while (done < len) {
        uint64_t off = pa + done - d->base;
        size_t rem = len - done;
        int w = (rem >= 8 && !(off & 7)) ? 8 : (rem >= 4 && !(off & 3)) ? 4 : (rem >= 2 && !(off & 1)) ? 2 : 1;
        if (off + (uint64_t)w > d->size) w = 1;
        MMU_CB_DEPTH++;
        if (write) {
            uint64_t v = 0; for (int i = 0; i < w; i++) v |= (uint64_t)buf[done + i] << (8 * i);
            if (d->wr.type == VAL_CLOSURE || d->wr.type == VAL_NATIVE) {
                KcValue args[3] = { kc_int((int64_t)off), kc_int(w), kc_int((int64_t)v) };
                kc_call_value(MMU_IP, d->wr, args, 3, MMU_POS);
            }
            d->writes++;
        } else {
            uint64_t v = 0;
            if (d->rd.type == VAL_CLOSURE || d->rd.type == VAL_NATIVE) {
                KcValue args[2] = { kc_int((int64_t)off), kc_int(w) };
                KcValue r = kc_call_value(MMU_IP, d->rd, args, 2, MMU_POS);
                if (r.type == VAL_INT) v = (uint64_t)r.as.i; else if (r.type == VAL_BOOL) v = r.as.b;
            }
            for (int i = 0; i < w; i++) buf[done + i] = (uint8_t)(v >> (8 * i));
            d->reads++;
        }
        MMU_CB_DEPTH--;
        done += (size_t)w;
    }
    (void)p;
    return true;
}

/* ------------------------------------------------------------------ raw access */

bool mmu_phys_read(MmuPhys *p, uint64_t pa, void *dst, size_t len) {
    if (!mmu_phys_in_range(p, pa, len)) return false;
    p->phys_reads++;
    uint8_t *out = dst;
    while (len) {
        uint64_t f = pa >> MMU_FRAME_SHIFT; size_t off = (size_t)(pa & (MMU_FRAME_SIZE - 1));
        size_t n = MMU_FRAME_SIZE - off; if (n > len) n = len;
        MmuDevice *d = p->ndev ? mmu_phys_find_device(p, pa) : NULL;
        if (d) { if (!dev_access(p, d, pa, out, n, false)) return false; }
        else {
            MmuChunk *c = chunk_get(p, f, false);
            uint8_t *fr = c ? c->frame[f % MMU_CHUNK_FRAMES] : NULL;
            if (fr) memcpy(out, fr + off, n); else memset(out, 0, n);
        }
        out += n; pa += n; len -= n;
    }
    return true;
}

bool mmu_phys_write(MmuPhys *p, uint64_t pa, const void *src, size_t len) {
    if (!mmu_phys_in_range(p, pa, len)) return false;
    p->phys_writes++;
    const uint8_t *in = src;
    while (len) {
        uint64_t f = pa >> MMU_FRAME_SHIFT; size_t off = (size_t)(pa & (MMU_FRAME_SIZE - 1));
        size_t n = MMU_FRAME_SIZE - off; if (n > len) n = len;
        MmuDevice *d = p->ndev ? mmu_phys_find_device(p, pa) : NULL;
        if (d) { uint8_t tmp[MMU_FRAME_SIZE]; memcpy(tmp, in, n); if (!dev_access(p, d, pa, tmp, n, true)) return false; }
        else {
            MmuChunk *c = chunk_get(p, f, true);
            uint8_t **fr = &c->frame[f % MMU_CHUNK_FRAMES];
            if (!*fr) { *fr = kc_malloc(MMU_FRAME_SIZE); memset(*fr, 0, MMU_FRAME_SIZE); }
            memcpy(*fr + off, in, n);
        }
        in += n; pa += n; len -= n;
    }
    return true;
}

bool mmu_phys_read_u(MmuPhys *p, uint64_t pa, int width, uint64_t *out) {
    uint8_t b[8] = {0};
    if (!mmu_phys_read(p, pa, b, (size_t)width)) return false;
    uint64_t v = 0; for (int i = 0; i < width; i++) v |= (uint64_t)b[i] << (8 * i);
    *out = v; return true;
}

bool mmu_phys_write_u(MmuPhys *p, uint64_t pa, int width, uint64_t v) {
    uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return mmu_phys_write(p, pa, b, (size_t)width);
}

bool mmu_phys_copy_frame(MmuPhys *p, uint64_t dst, uint64_t src) {
    uint8_t tmp[MMU_FRAME_SIZE];
    return mmu_phys_read(p, src, tmp, sizeof tmp) && mmu_phys_write(p, dst, tmp, sizeof tmp);
}

bool mmu_phys_frame_is_zero(MmuPhys *p, uint64_t pa) {
    uint64_t f = pa >> MMU_FRAME_SHIFT;
    MmuChunk *c = chunk_get(p, f, false);
    uint8_t *fr = c ? c->frame[f % MMU_CHUNK_FRAMES] : NULL;
    if (!fr) return true;
    for (size_t i = 0; i < MMU_FRAME_SIZE; i += 8) { uint64_t v; memcpy(&v, fr + i, 8); if (v) return false; }
    return true;
}

/* ------------------------------------------------------------------ swap */

#define MMU_SWAP_MAX (1u << 22)

bool mmu_swap_init(MmuPhys *p, uint32_t slots) {
    if (p->swap_slots || slots == 0 || slots > MMU_SWAP_MAX) return false;
    p->swap = kc_malloc(sizeof(uint8_t *) * slots); memset(p->swap, 0, sizeof(uint8_t *) * slots);
    p->swap_used = kc_malloc(slots); memset(p->swap_used, 0, slots);
    p->swap_slots = slots;
    return true;
}

bool mmu_swap_alloc(MmuPhys *p, uint32_t *slot) {
    if (!p->swap_slots || p->swap_in_use >= p->swap_slots) return false;
    for (uint32_t k = 0; k < p->swap_slots; k++) {
        uint32_t s = (p->swap_hint + k) % p->swap_slots;
        if (!p->swap_used[s]) { p->swap_used[s] = 1; p->swap_in_use++; p->swap_hint = s + 1; *slot = s; return true; }
    }
    return false;
}

void mmu_swap_free(MmuPhys *p, uint32_t slot) {
    if (!p->swap_slots || slot >= p->swap_slots || !p->swap_used[slot]) return;
    free(p->swap[slot]); p->swap[slot] = NULL; p->swap_used[slot] = 0; p->swap_in_use--;
}

bool mmu_swap_store(MmuPhys *p, uint32_t slot, uint64_t frame_pa) {
    if (slot >= p->swap_slots || !p->swap_used[slot]) return false;
    if (!p->swap[slot]) p->swap[slot] = kc_malloc(MMU_FRAME_SIZE);
    if (!mmu_phys_read(p, frame_pa, p->swap[slot], MMU_FRAME_SIZE)) return false;
    p->swap_outs++;
    return true;
}

bool mmu_swap_load(MmuPhys *p, uint32_t slot, uint64_t frame_pa) {
    if (slot >= p->swap_slots || !p->swap_used[slot]) return false;
    uint8_t zero[MMU_FRAME_SIZE];
    if (!p->swap[slot]) memset(zero, 0, sizeof zero);
    if (!mmu_phys_write(p, frame_pa, p->swap[slot] ? p->swap[slot] : zero, MMU_FRAME_SIZE)) return false;
    p->swap_ins++;
    return true;
}
