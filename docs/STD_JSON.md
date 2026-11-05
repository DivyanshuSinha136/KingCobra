# std.json — JSON

```kingcobra
import std.json;
```

A strict RFC 8259 parser and serializer plus the tools you actually need around
JSON: paths, JSONPath queries, RFC 6902 patch/diff, RFC 7396 merge-patch, schema
validation, JSON Lines, and a number-preserving formatter. Pure C, no dependencies.

## Values

| JSON | KingCobra |
|---|---|
| `null` `true` `false` | `null` `true` `false` |
| integer that fits int64 | `int` (exact) |
| other numbers (fraction, exponent, > int64) | `float` |
| string | `string` (UTF-8 validated; `\uXXXX` and surrogate pairs decoded) |
| array | array |
| object | **open object** — `typeof(x)` is `"object"` |

An *open object* is a struct whose keys are not declared in advance. It keeps
insertion order and supports field syntax, index syntax, and `len`:

```kingcobra
let o = json.parse("{\"name\":\"Ada\",\"age\":36}");
o.name            // "Ada"          (error if the key is missing)
o["name"]         // "Ada"
o["nope"]         // null           (missing key on an open object is null)
o["age"] = 37;    // replace or add a key
o["new"] = [1,2];
len(o)            // 3
```

Declared `struct`s can also be passed to `stringify`/`pretty` (the type name is dropped).

## Errors

Anything malformed is a fatal, positioned error: `json.parse: line 1, column 8: trailing
comma before '}'`. Scripts cannot catch errors, so every parsing function has a
non-fatal twin: `is_valid`, `error`, `try_parse`.

## Flags

Functions that take `flags` accept a string of words separated by spaces or commas.

| Parse flags | Meaning |
|---|---|
| `comments` | allow `// ...` and `/* ... */` |
| `trailing_commas` | allow `[1,2,]` and `{"a":1,}` |
| `nan` | allow `NaN`, `Infinity`, `-Infinity` |
| `lenient` | all three of the above |
| `dup_error` | duplicate object keys are an error (default: last value wins, first position kept) |
| `depth=N` | nesting limit (default 512) |

| Write flags | Meaning |
|---|---|
| `sort_keys` | object keys in byte order |
| `ascii` | escape every non-ASCII character as `\uXXXX` |
| `nan_null` | write NaN/Infinity as `null` (otherwise a clear error) |

Unknown flag words are an error that lists the valid ones.

## Parse, validate, write

| Function | Returns |
|---|---|
| `parse(text[, flags])` | value (fatal on error) |
| `try_parse(text[, default[, flags]])` | value, or `default` (`null`) if invalid |
| `is_valid(text[, flags])` | bool (validates without building values) |
| `error(text[, flags])` | `"line L, column C: message"` or `null` |
| `parse_file(path[, flags])` | value |
| `stringify(value[, flags])` | compact string; floats keep float-ness (`3.0`) and round-trip exactly |
| `pretty(value[, indent[, flags]])` | indented string; `indent` is an int (spaces, default 2) or a string of spaces/tabs |
| `minify(text[, flags])` / `format(text[, indent[, flags]])` | re-layout **text** without touching numbers or strings (`2.50`, huge integers and escapes survive) |
| `write_file(path, value[, indent[, flags]])` | bytes written; default indent 2, `0` = compact; trailing newline |
| `escape(text[, ascii])` / `unescape(literal)` | quoted JSON string literal ⇄ string |

`\u0000` is rejected (KingCobra strings are NUL-terminated C strings internally).

## JSON Lines (ndjson)

`parse_lines(text)`, `read_lines(path)` → array of values (blank lines skipped, errors name the
line). `stringify_lines(array)`, `write_lines(path, array)`, `append_line(path, value)`.

## Working with objects and arrays

| Function | |
|---|---|
| `object([pairs])` | new open object; `pairs` is `[[key, value], ...]` |
| `keys(o)` `values(o)` `items(o)` | `string[]`, array, `[[k, v], ...]` |
| `has(c, key_or_index)` `get(c, key_or_index[, default])` | works on objects and arrays (negative array indexes count from the end) |
| `set(c, key_or_index, value)` | mutates and returns `c`; `index == len` appends |
| `remove(c, key_or_index)` | `true` if something was removed |
| `pick(o, keys)` `omit(o, keys)` | new object |
| `type_of(v)` `is_object(v)` `is_array(v)` | `"null" "bool" "int" "float" "string" "array" "object"` |
| `deep_equal(a, b)` | key order ignored; `1 == 1.0` |
| `deep_copy(v)` `sort_keys(v)` | deep copies |
| `merge(a, b, ...)` | new value; objects merge recursively, later wins; arrays/scalars replaced |

## Paths

`get_path(doc, path[, default])`, `has_path`, `set_path(doc, path, value)` (mutates; creates missing
objects/arrays and pads array gaps with `null`; returns the root), `remove_path`.

```
a.b[0].c        dotted keys and [index]
x[-1]           negative index from the end
a["odd.key"]    quoted key for keys with '.', '[' or spaces
$.a.b           a leading $ is allowed
/a/0/b          RFC 6901 pointer (~0 = "~", ~1 = "/")
```

`escape_pointer(key)` escapes one pointer token. `flatten(v)` → object of leaf paths
(`"b.c[0]"`) to values; `unflatten(obj)` inverts it.

## Queries (JSONPath)

`query(doc, expr)` → array of matches · `query_first(doc, expr[, default])` ·
`query_paths(doc, expr)` → normalized paths such as `$.book[1].title` (feed them to `get_path`).

```
$.store.book[*].title             all children
$..price                          recursive descent
$.book[-1]   $.book[0:2]   $.book[::-1]   $.book[0,2]   $.bike['color','price']
$.book[?(@.price < 10)]           filters
$.book[?(@.cat == 'fic' && @.price > 20 || !@.isbn)]
$.book[?length(@.title) > 3]   $.book[?match(@.title, '^[AB]')]   $.book[?count(@.tags[*]) > 1]
```

Filter operands are `@.path`, `$.path`, numbers, strings, `true`/`false`/`null`; operators `== != < <= > >=`
`&& || ! ( )`; a bare path is an existence test. Functions: `length`, `count`, `match` (whole string),
`search` (anywhere) — the last two use `std.regx`.

## Diff, patch, merge-patch

* `diff(a, b)` → RFC 6902 operations turning `a` into `b`.
* `patch(doc, ops)` → new document; supports `add remove replace move copy test`; a failing operation
  is a fatal error naming its index and path. `doc` is not modified.
* `merge_patch(target, patch)` → RFC 7396: `null` in the patch deletes a key; non-mutating.

## Schema validation

`validate(doc, schema)` → array of messages (empty = valid), each `"/json/pointer: problem"`.
`matches(doc, schema)` → bool. `schema` may be an object or JSON text.

Supported: `type` (incl. `integer`), `enum`, `const`, `$ref` to local pointers (`#`, `#/definitions/x`,
`#/$defs/x`), `minimum maximum exclusiveMinimum exclusiveMaximum multipleOf`, `minLength maxLength pattern`
(via `std.regx`), `format` (`email ipv4 uuid date date-time uri`), `items` (schema or tuple),
`prefixItems`, `additionalItems`, `minItems maxItems uniqueItems contains`, `properties required
additionalProperties patternProperties minProperties maxProperties`, `allOf anyOf oneOf not`,
`if/then/else`. Unknown keywords are ignored.

## Notes and limits

* No garbage collector (by design): a parsed document stays allocated until you release it with `free_deep(doc)` (strings, arrays and objects, including object keys, are all reclaimed — see `docs/MEMORY.md`).
* Object key lookup is linear (parsing itself uses a hash index, so large documents parse in linear time).
* Nesting is limited to 512 by default on parse and on write (this also catches cyclic structures).
