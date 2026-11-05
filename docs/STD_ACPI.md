# std.acpi — ACPI tables, parsing, power/thermal state

```
import std.acpi;
```

**Is:** a table loader (OS firmware interface, raw physical memory, dump files, byte arrays),
validating parsers for common tables, read-only GAS register access, OS-reported power/thermal data.
**Is not:** an AML interpreter (DSDT/SSDT are exposed as validated raw tables only) and it **never writes**
ACPI registers (a stray PM1_CNT / RESET_REG write powers the machine off).

Tables are treated as hostile input: every offset is range-checked, repeating structures stop at the first
malformed record, and fields that postdate a table's revision read as `null`.

## Loading
`available()`, `source()` (`"sysfs"`, `"windows-api"`, `"none"`), `list()`, `load(name)` (`"FACP"`, `"SSDT2"`...),
`load_file(path)` (raw binary as written by `acpidump -b`), `from_bytes(int[])`, `save(t, path)`, `free(t)`,
`last_error()`. Structural damage (bad signature, truncated, absurd length) → `null`; a **bad checksum still loads**
(you may be debugging that firmware) and is reported by `checksum_ok(t)` (`null` for FACS, which has none).

Physical (Linux, root, `unsafe`): `rsdp()`, `scan_phys()`, `load_phys(addr)`. RSDP discovery order: EFI
`systab` hint → EBDA → `0xE0000-0xFFFFF`; v1 and v2 checksums are verified. `scan_phys` walks XSDT (or RSDT) and adds the FADT's DSDT/FACS.

## Header
`signature length revision oem_id oem_table_id oem_revision creator_id creator_revision origin header(t)`.
Raw: `u8 u16 u32 u64 (t, off)`, `bytes(t, off, n)`, `str(t, off, n)` — all bounds-checked.

## Parsed tables
* `fields(t)` lists named fields **present in this instance**; `get(t, name)`; `gas(t, name)`; `flags(t)`.
* **FACP**: all ACPI 6.x fields incl. `x_*` 64-bit pointers, `reset_reg`, `sleep_control_reg`, `hypervisor_vendor_id`,
  derived `pm_profile_name`; flags decoded (`HW_REDUCED_ACPI`, `LOW_POWER_S0_IDLE_CAPABLE`, ...).
* **HPET**: block id, `base_address`, plus derived `vendor_id`, `comparators`, `counter_64bit`, `legacy_replacement`.
* **APIC (MADT)**, **FACS**, **SLIT** header fields via `get`.
* `entries(t)` → rows `[kind, ...]`:
  APIC: `lapic [id, uid, flags]`, `ioapic [id, address, gsi_base]`, `iso [bus, source, gsi, flags]`, `nmi_source`, `lapic_nmi`,
  `lapic_override [addr]`, `x2apic [id, flags, uid]`, `x2apic_nmi`, `gicc`, `gicd`; unknown → `["unknown", type, len]`;
  damaged → `["malformed", type, len, offset]` and the walk ends.
  MCFG: `[base, segment, start_bus, end_bus]`. SRAT: `lapic_affinity`, `memory_affinity [domain, base, length, flags]`, `x2apic_affinity`.
  SLIT: distance matrix. RSDT/XSDT: pointer list.
* GAS = `[space_id, space_name, bit_width, bit_offset, access_size, address]`.

## Registers & platform state
`gas_read(gas)` (`unsafe`; memory and x86 I/O space only), `pm_timer()` / `pm_timer_hz()` (3 579 545 Hz; 24- or 32-bit per `TMR_VAL_EXT`),
`power()` → `[ac_online|null, battery_present, percent|-1, charging, seconds_remaining|-1]`,
`thermal_zones()` → `[[name, type, millidegrees_C], ...]`, `sleep_states()` (`["freeze","mem","disk"]`).
Windows: tables via `GetSystemFirmwareTable`, power via `GetSystemPowerStatus`; raw-physical, thermal and sleep-state calls return `null` with a reason.
