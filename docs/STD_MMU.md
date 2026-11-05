# std.mmu — memory-management unit

```
import std.mmu;
```

`std.mmu` is two things in one module:

1. **A software MMU** you can build operating-system memory management on, or study it with:
   real page tables stored inside simulated physical memory, nine architecture formats, a tagged
   TLB, hardware-accurate faults, copy-on-write `fork`, demand paging, huge pages, shared memory,
   swap with a clock reclaimer, and simulated device windows.
2. **The real host MMU** (`host_*`): anonymous mappings with per-page protection, guard pages,
   huge-page/lock/discard advice, residency, `/proc/self/pagemap` and the process memory map.

Nothing in the simulator can touch the real machine, so it needs no `unsafe { }`. The host half
never hands out raw pointers; executable memory needs `unsafe { }` (and W^X is enforced).

## Architectures

`mmu.archs()` → `x86_32, x86_64, x86_64_la57, arm64, arm64_39, riscv_sv32, riscv_sv39, riscv_sv48, riscv_sv57`
(aliases: `amd64`, `aarch64`, `sv39`, `la57`, …). `mmu.arch_info(arch)` reports levels, VA/PA bits,
entry size, supported page sizes, ASID bits, root register name and cache types.

| | x86_32 | x86_64 | arm64 | RISC-V |
|---|---|---|---|---|
| levels | 2 | 4 / 5 (la57) | 4 / 3 (39-bit) | 2 / 3 / 4 / 5 |
| large pages | 4 MiB | 2 MiB, 1 GiB | 2 MiB, 1 GiB | 4 MiB / 2 MiB, 1 GiB, 512 GiB, … |
| non-exec | none (always executable) | NX | PXN/UXN | X bit |
| A/D | hardware | hardware | hardware (AF, DBM) or software | hardware or software (Svade) |
| split VA | – | sign-extended hole | TTBR0 / TTBR1 roots | sign-extended hole |
| root register | CR3 | CR3 (+PCID) | TTBR0 (+ASID) | satp |

Real encodings are used, including the awkward ones: x86 PAT bit moving between bit 7 and bit 12 for
large pages, x86 table-level RW/US/NX inheritance, arm64 `APTable`/`UXNTable` restrictions and
DBM-style dirty tracking, RISC-V's "R=W=X=0 means pointer" and reserved `W`-without-`R`.

Notes: x86_32 pages are always executable (the flag shows as `x`); on x86 a present page is always
readable; 32-bit architectures support up to 4 GiB of physical memory (PAE/PSE-36 are not modeled).

### Addresses
Ints are signed 64-bit, so high-half addresses are negative. **Every address argument also accepts a
string** (`"0xFFFF800000000000"`), and `mmu.hex(v)` prints any value unsigned. Helpers:
`parse`, `hex(v [,digits])`, `ucmp`, `sext(v,bits)`, `align_down/align_up/is_aligned`,
`is_canonical(arch, va)`, `canonical(arch, va)`, `split(arch, va)` → `{indices, offset, half, canonical}`.

## Error model
* **Fatal** (exit 65): wrong types/arguments, unknown flags, use after `destroy`, `unsafe` violations,
  `strict` cpus on an unhandled fault.
* **Values**: a page fault makes an access return `null` / `false` and sets `mmu.last_fault()`.
  Other expected failures (frame exhaustion, already mapped, …) return `null`/`false` and set
  `mmu.last_error()`.

## Physical memory
`mmu.phys(size [, name])` — sparse (a 64 GiB machine costs nothing until touched; max 256 GiB).
Sizes accept ints or strings like `"64M"`, `"2GiB"`.

| call | |
|---|---|
| `phys_read(p, pa, width)` / `phys_write(p, pa, width, v)` | width 1/2/4/8 |
| `phys_read_bytes`, `phys_write_bytes`, `phys_fill`, `phys_copy` | bulk |
| `frame_alloc(p [,count [,align_frames]])` → pa / `frame_free(p, pa [,count])` / `frame_refs(p, pa)` | reference-counted frame allocator (frames come back zeroed) |
| `device(p, base, size, read_fn, write_fn [, name])` | page-aligned window whose accesses call `read_fn(offset, width)` / `write_fn(offset, width, value)` |
| `swap_init(p, slots)` | attach a swap area |
| `phys_info(p)` | sizes, frames used/free/reserved, devices, swap, counters |

## Address spaces and CPUs
`mmu.space(arch, phys [, options [, name]])` creates an address space (a root table in physical
memory) with its own private CPU/TLB. `mmu.cpu(arch [, options])` creates a standalone CPU you can
`mmu.switch(cpu, space [, flush])` between spaces — with ASID/PCID tagging the TLB survives the switch,
without it (`noasid`, x86_32) non-global entries are flushed. Every access/translation function takes
either a space or a cpu.

**Options** (comma list): `wp`/`nowp` (supervisor honours read-only; default on), `smep`/`nosmep`
(default on), `smap`/`nosmap` (default off), `sw_ad` (accessed/dirty maintained by software: arm64
AF/DBM, RISC-V Svade — faults instead of updating), `manual_tlb` (no automatic shootdown),
`strict`, `tlb=N`, `ways=N`, `asid=N`, `noasid`. `mmu.policy(x [, options])` reads/changes them.

### Mapping
* `map(s, va, pa, flags [, page_size])` → bool. Flags: `r w x u g`, cache `wb wc nc uc wt wp`,
  `a d` (preset accessed/dirty), `cow own` (software bits), `force` (replace). Large pages need an
  aligned `va`/`pa` and a size the arch supports. Intermediate tables are allocated on demand.
* `map_range(s, va, pa, len, flags)` — adds `huge` (greedy 1 GiB / 2 MiB where aligned); rolls back on failure.
* `unmap(s, va [,len])` → pages removed. Straddled large pages are split; emptied tables are freed.
* `protect(s, va, len, flags)` — `"r,w"` replaces, `"+w,-x"` edits; splits as needed; COW pages stay read-only.
* `split_page(s, va)`, `query(s, va)`, `walk(s, va)` (per-level steps), `mappings(s [,lo,hi])`,
  `dump(s [,lo,hi])` (ptdump-style text), `verify(s)` → issue list, `stats(s)`.
* `root(s [,half])`, `root_reg(s)` → the CR3/TTBR0/satp value; `pte_encode/pte_decode/table_entry`
  work on raw entries without any space.

## Translation, faults, TLB
`translate(c, va, access [, mode])` — `access` `"r"|"w"|"x"`, `mode` `"u"|"s"` (default `"s"`) →
`{ok:true, pa, page_va, page_size, level, flags, tlb_hit …}` or a fault object. `probe` is the
side-effect-free variant (no TLB fill, no A/D update, no handlers); `can(...)` → bool;
`virt_to_phys(...)` → pa or `null`.

A **fault object**: `kind` (`not_present protection reserved non_canonical access_flag dirty bus`),
`va`, `access`, `mode`, `level`, `raw`, `cow`, `swapped`, `segv` (`maperr`/`accerr`/`none`),
`message`, and the architecture-native status: x86 `#PF` error code (or `#GP`), arm64 FSC/WnR,
RISC-V `scause`.

* `on_fault(c, handler | null)` — `handler(fault)` returns `true` to retry the access (up to 6 times).
  Order: built-in VM handler first (demand paging, COW, swap-in, A/D fix-up), then yours.
* `strict(c, on)`, `last_fault([c])`.
* TLB: `tlb_info`, `tlb_dump`, `tlb_flush(c [,include_global])`, `tlb_invalidate(c, va [,asid])`,
  `tlb_config(c, entries [,ways])`. It is set-associative per page size, LRU, ASID-tagged,
  global-aware, and keeps **stale** entries when `manual_tlb` is set. Permission faults seen
  through a cached entry re-walk before being reported (as on hardware).

### Data access
`read8/16/32/64`, `write8/16/32/64`, `read(c, va, width)`, `write(c, va, width, v)`, `read_bytes`,
`write_bytes`, `read_cstring`, `fill`, `copy` — each takes an optional trailing mode. Accesses may
cross pages; writes translate every page first, so a fault leaves memory untouched.

## Virtual-memory manager
| call | |
|---|---|
| `mmap(s, addr\|null, len, flags [, name [, shm, shm_off]])` → address | flags: `r w x u g`, `fixed noreplace populate growsdown locked huge guard none shared`, cache types |
| `munmap`, `mprotect(s, a, len, flags)` (absolute; `none` keeps contents), `madvise` (`dontneed free willneed hugepage nohugepage normal sequential random`), `mlock/munlock`, `populate` | |
| `vmas(s)`, `find_vma(s, va)`, `stack_limit(s, bytes)` | |
| `fork(s [, name])` | child shares private pages copy-on-write; shared mappings stay shared |
| `shm(p, len [, name])`, `shm_info` | shared memory objects mappable into several spaces |
| `reclaim(s, pages)`, `evict(s, va)` | clock/second-chance eviction to swap; zero pages are simply dropped |

Demand paging allocates a zeroed frame (or a 2 MiB page for `huge` VMAs when aligned and free) on
first touch. A write to a COW page copies the frame, or just re-enables write if it is the last
owner. `growsdown` mappings extend on a fault just below them up to `stack_limit`. The VM layer only
manages pages it owns (`own`) and addresses inside VMAs, so raw `map()` users keep full control.

## Lifetime
`mmu.destroy(handle)` for phys / space / cpu / shm / host regions. Destroying a space returns all
its owned frames, page tables and swap slots. There is no GC: destroy what you create.

## Host memory (real)
`host_info`, `host_alloc(len [, flags])` (`r w x none guard huge lock populate`), `host_free`,
`host_protect(r, off, len, flags)`, `host_advise`, `host_lock/unlock`, `host_read8…64`,
`host_write8…64`, `host_read_bytes/write_bytes/fill/copy`, `host_pages`, `host_resident`,
`host_residency`, `host_address`, `host_translate(r, off [,touch])` (pagemap: present/swapped/
soft-dirty/exclusive; `pfn` is zero without `CAP_SYS_ADMIN`), `host_maps`, `host_query(addr)`,
`host_stats`. The module tracks each page's protection itself, so touching a `none` page returns
`null`/`false` with `last_error()` instead of crashing. These are rejected inside `comptime`.

## Platform notes
Simulator: portable. Host: Linux (everything), Windows (alloc/protect/advise/lock/maps via
`VirtualAlloc`/`VirtualProtect`/`VirtualQuery`; no pagemap/residency — those return `null` with an
explanation; the Windows path is written to the same contract but not exercised in CI here).

## Not modeled
PAE and PSE-36, x86 PKU/CET, EPT/NPT/stage-2 translation, ARM granules other than 4 KiB, LPA2,
RISC-V Svnapot and non-Svpbmt cache bits, multi-CPU shootdown IPIs (all CPUs in the process are
shot down synchronously), and file-backed mappings.
