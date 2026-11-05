# std.csv — CSV / TSV

```kingcobra
import std.csv;
```

RFC 4180 parsing and writing for text, files, and streams, with typed records,
header handling, dialect options and a few table helpers. Records are the same
open objects `std.json` produces, so `json.stringify(csv.parse(text, "header"))` works.

## Options

Every function with `options` takes either a string of words or an object.

```kingcobra
csv.parse(text, "header infer trim delim=;")
csv.parse(text, json.parse("{\"header\":true,\"delimiter\":\";\"}"))
```

| Word / key | Meaning |
|---|---|
| `header` | first record is the header; rows become objects |
| `infer` | unquoted `42` → int, `3.5` → float, `true/false` → bool, empty → `null`. Quoted fields and values like `007` stay strings |
| `trim` | strip spaces around unquoted fields |
| `strict` | ragged rows, bare quotes in unquoted fields and junk after a closing quote are errors (default: lenient) |
| `delim=X` · `tsv` | single character, or `tab comma semicolon pipe space`, or `auto` (sniffed) |
| `quote=X` / `quote=none` | quote character (default `"`) |
| `comment=#` | skip lines starting with the prefix |
| `keep_empty` | blank lines are records instead of being skipped |
| `crlf` `bom` `always_quote` | writer: `\r\n` line ends, UTF-8 BOM, quote every non-null field |
| `no_header` | writer: don't write the header row for objects |
| `append` | writer handle: append instead of truncating |
| `columns` (object form only) | writer: column order for object rows |

Lenient mode details: a row shorter than the header fills with `null`; extra fields are named `col<N>`.
Duplicate or empty header names are made unique (`name`, `name_2`, `col3`).
A UTF-8 BOM, CRLF, LF and lone CR line ends are all accepted.

## Reading and writing

| Function | |
|---|---|
| `parse(text[, options])` | array of rows (arrays, or objects with `header`) — fatal on malformed input |
| `try_parse(text[, default[, options]])` | rows, or `default` if malformed |
| `parse_file(path[, options])` | rows |
| `parse_line(line[, options])` / `format_line(array[, options])` | one record ⇄ text (no newline) |
| `stringify(rows[, options])` | text. Rows may be arrays or objects; columns for objects = union of keys in first-seen order (or `columns`) |
| `write_file(path, rows[, options])` / `append_file(path, rows[, options])` | rows written; append does not repeat the header |
| `sniff(text)` | `{delimiter, columns, header}` best guess |
| `infer(string)` | the same typing rule as the `infer` option |

Writing: `null` → empty, `""` → `""` (so it reads back as an empty string, not `null`), numbers/bools bare,
arrays and objects as compact JSON. A field is quoted when it contains the delimiter, a quote, a line
break, leading/trailing blanks, or is the empty string.

## Streaming

For files you don't want fully in memory:

```kingcobra
let r = csv.open("big.csv", "header infer");
let header = csv.header(r);
var row = csv.read(r);                 // null at the end
while (row != null) { /* ... */ row = csv.read(r); }
csv.close(r);

let w = csv.writer("out.csv");         // header is written from the first object row
csv.write(w, row);  csv.write_rows(w, rows);  csv.flush(w);  csv.close(w);
csv.count_rows("big.csv", "header");   // streams, never loads the file
```

`csv.line(reader)` is the number of the line consumed so far (for your own diagnostics).

## Table helpers

| Function | |
|---|---|
| `column(rows, name_or_index)` | array of one column. For arrays-of-arrays a *name* is looked up in the first row |
| `select(rows, columns)` | keep only the listed columns, in that order |
| `sort_by(rows, column[, "desc"])` | stable; numbers (and numeric strings) compare numerically, `null`s last |
| `filter(rows, fn)` | keep rows where `fn(row)` is truthy |
| `transpose(rows)` | arrays of arrays; short rows padded with `null` |
| `stats(rows, column)` | `{count, sum, mean, min, max, nulls, non_numeric}` |
| `to_records(rows)` / `to_rows(records[, columns])` | header row ⇄ objects |

## Limits

Fields are limited only by memory. There is no garbage collector (memory is explicit, see `docs/MEMORY.md`), so a large
`parse` result stays allocated until you `free_deep` it; use the streaming API for large inputs.
