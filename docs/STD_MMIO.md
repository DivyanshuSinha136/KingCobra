# std.mmio — memory-mapped I/O and port I/O

```
import std.mmio;
```

A **region** is an opaque handle over a window of device (or device-like) memory.
Every register access is one volatile load/store of exactly the requested width,
ordered like Linux `readl()/writel()`. `memcpy` is never used on device memory.

## Checks performed on every access (in this order)
1. handle is a live region (`use after unmap` otherwise)
2. real-hardware regions require `unsafe { }` (virtual regions do not)
3. region is writable if writing
4. access lies fully inside the region (overflow-safe)
5. the **absolute address** is naturally aligned for the width
6. value fits the width (`-1` and `0xFF` both fine for 8-bit; `256` is an error)

## Error model
* **Fatal** (exit 65, descriptive): wrong types, out of bounds, misaligned, read-only write,
  use-after-unmap, missing `unsafe`, value too wide.
* **Returns `null`** + `mmio.last_error()`: environment problems — permission denied, no such
  device, kernel refused the mapping, System-RAM safety refusal.

## Opening regions
| call | meaning |
|---|---|
| `mmio.virtual(len [, label])` | zeroed, bounds-checked RAM "device". Safe; no `unsafe`. |
| `mmio.map(phys, len [, flags])` | physical memory via `/dev/mem` (O_SYNC, uncached). Linux, root. Refuses ranges overlapping *System RAM* per `/proc/iomem` unless flag `ram`. |
| `mmio.map_pci(bdf, bar [, flags])` | PCI BAR through sysfs `resourceN` (kernel-mediated; preferred). Flag `wc` uses `resourceN_wc`. `bdf` = `"00:1f.3"` or `"0000:00:1f.3"`. |
| `mmio.map_uio(dev, map [, flags])` | UIO device map (handles sub-page offsets). |
| `mmio.map_file(path, offset, len [, flags])` | any file/device node; offset need not be page aligned. |
| `mmio.unmap(r)` | → bool (false if already unmapped) |

Flags: comma list of `ro`, `rw` (default), `wc` (PCI only), `ram` (phys only).

## Access
`read8/16/32/64(r, off)`, `write8/16/32/64(r, off, v)`, `read(r, off, width)`, `write(r, off, width, v)`,
`read_be` / `write_be` (byte-swapped), `set_bits(r, off, width, mask)`, `clear_bits(...)`,
`update(r, off, width, mask, value)` — these three return the old value and are **not atomic**.
`wait_bits(r, off, width, mask, expected, timeout_us)` → bool.
Bulk: `read_block(r, off, count, width)`, `write_block(r, off, width, ints)` (validated before the first write),
`fill(r, off, width, count, v)`, `read_into/write_from(r, roff, membuf, boff, nbytes, width)` (width-preserving, with `std.memory` buffers).
`barrier([“full”|“read”|“write”|“compiler”])`.

## Port I/O (x86 Linux, root)
`port_in(port, width)`, `port_out(port, width, value)`. Uses real `inb/inw/inl` via `ioperm`/`iopl`
(not `/dev/port`, which mishandles 16/32-bit registers).

## Discovery & diagnostics
`capabilities()`, `pci_list()`, `pci_info(bdf)` → `[vendor, device, class]`,
`pci_bars(bdf)` → `[[index, start, end, size, flags, is_io, prefetchable], ...]`, `iomem()`,
`info(r)`, `size/base/label/source/is_mapped(r)`.
`trace(r, on)` / `trace_dump()` / `trace_clear()` — 4096-entry ring of `[label, "r"|"w", width, offset, value]`.

## Bit helpers (the language has no bitwise operators yet)
`band bor bxor bnot shl shr(logical) bit extract(v,lo,width) insert(v,lo,width,field) hex(v [,bytes])`.

## Platform notes
* Windows has no user-mode physical-memory or port-I/O API. `map`/`map_pci`/`map_uio`/`port_*`
  return `null` with an explanatory `last_error()`; `virtual` and `map_file` work.
* ARM64 gets real `dmb` barriers around accesses; x86 needs only compiler barriers for uncached MMIO.
* 64-bit accesses require a 64-bit host.
