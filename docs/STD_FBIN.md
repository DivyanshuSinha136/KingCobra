# std.fbin — binary files and byte streams

```kingcobra
import std.fbin;
```

One **stream** handle type covers real files and in-memory buffers, so every function below
works on either. Every read is bounds-checked: reading past the end is an error such as
`unexpected end of data (wanted 4 byte(s) at offset 0, only 2 left)` — never garbage.

Integers in KingCobra are signed 64-bit. `u64` values above 2^63-1 appear as negative ints
(same bits); `u64_to_string` prints them unsigned and `parse_u64` reads them.

## Creating streams

| Function | |
|---|---|
| `open(path[, mode])` | file stream. Modes: `r` (default) `w` `a` `r+` `w+` `a+` |
| `try_open(path[, mode])` | stream or `null` instead of an error |
| `buffer([capacity])` | empty in-memory read/write stream |
| `from_hex(hex)` `from_base64(text)` `from_string(text)` `from_bytes(array)` | in-memory stream with that content |
| `load(path)` / `save(stream, path)` | whole file ⇄ in-memory stream |
| `file_size(path)` | bytes, or `-1` if it cannot be opened |
| `close(stream)` `flush(stream)` | |

In-memory streams are limited to 2 GiB; file streams are not.

## Position

`size`, `tell`, `seek(s, offset[, "set"|"cur"|"end"])` (returns the new position), `eof`, `remaining`,
`skip(s, n)`, `align(s, boundary)` (zero-pads when writable), `fill(s, byte, count)`, `truncate(s, size)`.
Seeking past the end of a writable stream and writing zero-fills the gap.

## Numbers

`read_u8 read_i8 read_u16 read_i16 read_u32 read_i32 read_u64 read_i64 read_f32 read_f64` take
`(stream[, endian])`; the matching `write_*` take `(stream, value[, endian])`. Writes check the range
(`write_u8(s, 256)` is an error). Endian is `"le"` or `"be"`; the stream default (initially little) is read or
changed with `endian(stream[, "le"|"be"])`.

LEB128 varints: `read_uvarint`, `write_uvarint`, and zigzag `read_svarint`, `write_svarint`.

Bits (MSB first): `read_bits(s, count)`, `write_bits(s, value, count)` for 1–64 bits, and `bit_align(s)`.
Any byte-level call first finishes a partial byte (zero-padded when writing, skipped when reading).

## Strings and bytes

| Function | |
|---|---|
| `read_string(s, n)` / `write_string(s, text)` | raw bytes |
| `read_cstring(s[, max])` / `write_cstring(s, text)` | NUL-terminated |
| `read_pstring(s[, width[, endian]])` / `write_pstring(s, text[, width[, endian]])` | length-prefixed; width 1, 2, 4 or 8 |
| `read_bytes(s, n)` / `write_bytes(s, array)` | arrays of ints 0..255 |
| `read_hex(s, n)` / `write_hex(s, hex)` | hex text (whitespace and `0x` ignored on write) |
| `peek_u8(s)` / `peek_hex(s, n)` | look ahead without moving |

Strings can hold any bytes, but `print` and `+` stop at a NUL; use `to_hex` to inspect binary data.

## pack / unpack

Python-`struct`-style formats: a leading `<` little, `>` or `!` big, `=`/`@` little (default: the stream's order),
then items with optional repeat counts.

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| `b` `B` | i8 / u8 | `h` `H` | i16 / u16 |
| `i` `I` `l` `L` | i32 / u32 | `q` `Q` | i64 / u64 |
| `f` `d` | f32 / f64 | `?` | bool (1 byte) |
| `Ns` | fixed `N`-byte string (NUL-padded; trailing NULs stripped on read) | `z` | NUL-terminated string |
| `Nx` | `N` pad bytes | | |

`pack(stream, fmt, values)` → bytes written · `unpack(stream, fmt)` → array · `calcsize(fmt)` (`-1` if the
format has `z`) · `pack_hex(fmt, values)` · `unpack_hex(fmt, hex)`. No implicit alignment is added.

## Inspecting and searching

`to_hex`, `to_base64`, `to_bytes`, `to_string`, `hexdump` take `(stream[, offset[, length]])` and do not move the position.
`find(s, text[, start])` and `find_hex(s, hex[, start])` return a byte offset or `-1` (streams in 64 KiB chunks).
`expect(s, text)` / `expect_hex(s, hex)` check a magic number at the current position and advance only on a match.
`copy(src, dst[, count])` copies between streams.

## Checksums and hashes

`crc32`, `adler32` (ints) and `sha256` (hex string) over `(stream[, offset[, length]])`, plus `crc32_string`,
`adler32_string`, `sha256_string` for text. `bswap16/32/64(x)` swap byte order.
