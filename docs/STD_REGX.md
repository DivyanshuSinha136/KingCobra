# std.regx — regular expressions

```kingcobra
import std.regx;
```

A from-scratch engine (no PCRE/RE2): parser → bytecode → an **iterative**
backtracking VM with an explicit stack (no C recursion per character) and a
step budget, so catastrophic patterns like `(a+)+$` end in a clean
"backtracking limit" error instead of hanging.

## Simple use — pass the pattern string straight in

```kingcobra
regx.test("^\\d+$", s)                  // bool
regx.find("\\d+", "order #4821")        // "4821" or null
regx.replace("\\s+", text, " ")         // replace all
regx.split("\\s*,\\s*", "a , b,c")      // ["a", "b", "c"]
```

Patterns passed as strings are compiled once and cached (24 entries).

## Advanced use

```kingcobra
let re = regx.compile("^(?<y>\\d{4})-(?<m>\\d{2})$", "i");   // flags: i m s
regx.group(re, "2026-10", "y")          // "2026"
regx.groups(re, "2026-10")              // ["2026-10", "2026", "10"]
regx.replace_fn("[aeiou]", s, fn)       // fn(groups) -> string
```

| Function | Returns |
|---|---|
| `compile(pat[, flags])` / `is_valid(pat[, flags])` / `escape(text)` | handle / bool / string |
| `test(re, s)` / `full(re, s)` | bool (anywhere / whole string) |
| `find(re, s[, start])` | first match text or `null` |
| `index(re, s[, start])` / `span(re, s[, start])` | byte offset or -1 / `[start, end]` or `null` |
| `groups(re, s[, start])` | `[whole, g1, g2, ...]` (`null` for groups that didn't participate) or `null` |
| `group(re, s, name_or_index)` / `names(re)` / `group_count(re)` / `pattern(re)` | |
| `find_all(re, s)` / `find_all_groups(re, s)` / `count(re, s)` | `string[]` / `string[][]` / int |
| `replace(re, s, template)` / `replace_first` / `replace_fn(re, s, fn)` | string. Template: `$1` `${name}` `$&` `$$` |
| `split(re, s[, limit])` | `string[]`; captured groups are included, as in JS/Perl |

`re` is either a pattern string or a `compile()` handle. Offsets are **byte** offsets.

## Syntax

Literals, `.`, `^` `$` `\A` `\z`, classes `[a-z]` `[^...]` `[[:alpha:]]`,
`\d \D \w \W \s \S`, `\b \B`, escapes `\n \t \r \f \v \0 \xHH \u{...} \uHHHH`,
groups `( )` `(?: )` `(?<name> )` `(?P<name> )`, alternation `|`,
quantifiers `* + ? {n} {n,} {n,m}` with lazy `?` forms, lookahead `(?= )` `(?! )`,
backreferences `\1`…`\9` `\k<name>`, inline flags `(?i)` `(?m)` `(?s)` `(?-i)` `(?i: )`.

Flags: `i` ASCII case-insensitive · `m` `^`/`$` match at line breaks · `s` `.` matches `\n`.

## Deliberate limits (all reported as errors, never silently wrong)

* Text is UTF-8: `.`, negated classes and `\W \D \S` consume a whole code point,
  but **character classes are ASCII-only** (use `(?:é|è)` instead of `[éè]`).
* No lookbehind, possessive quantifiers or atomic groups.
* `$` without `m` matches only at the very end (not before a final `\n`).
* Backtracking budget: 30 M VM steps per search; stack capped at 8 M entries.

See `examples/std_regx.kc`.
