/* help([thing]) — interactive documentation, in the spirit of Python's help().  (v0.9)
 *
 *   help()                  an overview and the list of topics
 *   help(value)             a function (its signature, and the `/// doc comment` written above it), a
 *                           module, a struct instance, or the type of any other value
 *   help("name")            a builtin, keyword, method, module ("std.json" or "json"), struct, or topic:
 *                           help("map")  help("switch")  help("string.split")  help("topics")  help("builtins")
 *
 * Document your own code with `///` lines directly above a `fn` or `struct`:
 *
 *     /// Returns the area of a w x h rectangle.
 *     fn area(w: float, h: float) -> float { return w * h; }
 *     help(area);
 */
#include "interp.h"
#include "kc_platform.h"
#include "lib.h"
#include <ctype.h>

/* ================================================================ documentation tables */

typedef struct { const char *name, *cat, *sig, *text; } Doc;

#define D(name, cat, sig, text) { name, cat, sig, text }

static const Doc BUILTINS[] = {
    /* ---- core */
    D("print", "core", "print(value, ...)", "Writes the values separated by spaces, then a newline. Strings print raw; arrays, objects and structs print their contents."),
    D("println", "core", "println(value, ...)", "Like print."),
    D("input", "core", "input([prompt]) -> string", "Reads one line from standard input (without the newline). Returns null at end of input."),
    D("len", "core", "len(x) -> int", "Length of a string (in BYTES), array, buffer or object. Arrays also have the property a.len."),
    D("str", "core", "str(x) -> string", "The text form of any value: str(5) is \"5\", str([1, 2]) is \"[1, 2]\"."),
    D("int", "core", "int(x) -> int", "Converts a number or numeric string to an int (floats truncate toward zero). Text that is not a number gives 0."),
    D("float", "core", "float(x) -> float", "Converts a number or numeric string to a float. Text that is not a number gives 0."),
    D("typeof", "core", "typeof(x) -> string", "The type name: int, float, bool, string, array, object, null, native_fn, ... (for structs, the struct's name)."),
    D("push", "core", "push(array, value)", "Appends to the end of an array, in place. (Same as array.push(value).)"),
    D("range", "core", "range(n) -> array", "The array [0, 1, ..., n-1]. For loops prefer `for (i in 0..n)`, which does not build an array."),
    D("clock", "core", "clock() -> float", "Seconds since an arbitrary start, as a float: subtract two readings to time something."),
    D("preprocess", "core", "preprocess(source: string, filename: string) -> string", "Run the C-style preprocessor (#define, #if, #include, ...) over source text and return the result. Used by the kcc native compiler."),
    D("pp_define", "core", "pp_define(\"NAME[=value]\")", "Define a preprocessor macro for later preprocess() calls, like the -D command-line option."),
    D("sqrt", "core", "sqrt(x) -> float", "Square root."),
    D("assert", "errors", "assert(condition [, message])", "Stops with `assertion failed` (and the message) when the condition is falsy. In a test, fails that test."),
    D("assert_eq", "errors", "assert_eq(actual, expected [, message])", "Stops with `expected X, got Y` when the two values differ."),
    D("error", "errors", "error(message)", "Raises an error that unwinds to the nearest try/catch (or ends the program)."),
    /* ---- numbers / text */
    D("abs", "numbers", "abs(x)", "Absolute value of an int or float."),
    D("min", "numbers", "min(a, b, ...) or min(array)", "The smallest value."),
    D("max", "numbers", "max(a, b, ...) or max(array)", "The largest value."),
    D("clamp", "numbers", "clamp(x, lo, hi)", "x limited to the range lo..hi: clamp(15, 0, 10) is 10."),
    D("idiv", "numbers", "idiv(a, b) -> int", "Integer division rounding toward negative infinity: idiv(-7, 2) is -4 (a / b on ints truncates)."),
    D("chr", "text", "chr(code) -> string", "The one-character string for a Unicode code point: chr(65) is \"A\"."),
    D("ord", "text", "ord(s) -> int", "The code point of the first character: ord(\"A\") is 65."),
    /* ---- collections */
    D("map", "collections", "map(array, fn(x)) -> array", "A new array of fn applied to every element. Also array.map(fn)."),
    D("filter", "collections", "filter(array, fn(x)) -> array", "A new array of the elements for which fn is truthy."),
    D("reduce", "collections", "reduce(array, fn(acc, x) [, initial])", "Folds the array into one value: reduce([1, 2, 3], fn(a, b) { return a + b; }) is 6."),
    D("find", "collections", "find(array, fn(x))", "The first element for which fn is truthy, or null."),
    D("any", "collections", "any(array, fn(x)) -> bool", "True if fn is truthy for at least one element."),
    D("all", "collections", "all(array, fn(x)) -> bool", "True if fn is truthy for every element."),
    D("each", "collections", "each(array, fn(x))", "Calls fn on every element, for its side effects."),
    D("sum", "collections", "sum(array)", "The sum of the elements (0 for an empty array)."),
    D("sorted", "collections", "sorted(array [, cmp(a, b)]) -> array", "A sorted COPY. Without cmp: ascending. cmp returns negative, zero or positive: sorted(a, fn(x, y) { return y - x; }) is descending."),
    D("zip", "collections", "zip(a, b) -> array", "Pairs up two arrays: zip([1, 2], [\"a\", \"b\"]) is [[1, \"a\"], [2, \"b\"]]."),
    D("keys", "collections", "keys(object) -> array", "The keys of an object or struct instance."),
    D("values", "collections", "values(object) -> array", "The values of an object or struct instance."),
    D("items", "collections", "items(object) -> array", "[[key, value], ...] pairs."),
    D("has", "collections", "has(object, key) -> bool", "True if the object or struct has that key / field."),
    /* ---- memory */
    D("free", "memory", "free(x)", "Releases an alloc/buffer block, string, array or struct. Using it afterwards is a clear error, not a crash. Shallow, like C."),
    D("free_deep", "memory", "free_deep(x)", "Frees a value and everything it holds."),
    D("is_freed", "memory", "is_freed(x) -> bool", "True if x has been freed."),
    D("mem_stats", "memory", "mem_stats() -> object", "Counts of live and freed strings, arrays, structs and regions."),
    D("mem_report", "memory", "mem_report()", "Prints live allocations with the source line that made them (what `kingcobra --leaks` shows at exit)."),
    D("mem_quarantine", "memory", "mem_quarantine(count) -> int", "How many freed blocks are held back before their memory is reused (default 4096), so stale pointers are caught. Returns the previous size."),
    /* ---- pointers / low level */
    D("alloc", "pointers", "alloc(bytes) -> ptr", "Zeroed memory as a bounds-checked pointer. Cast it: alloc(sizeof(Point)) as *Point. See help(\"pointers\")."),
    D("alloc_array", "pointers", "alloc_array(\"T\", count) -> *T", "`count` zeroed values of type T (\"i32\", \"u8\", a struct name, ...)."),
    D("realloc", "pointers", "realloc(ptr, bytes) -> ptr", "Resizes a block; new bytes are zero. Other pointers into the block stay valid."),
    D("buffer", "pointers", "buffer(n) -> buffer", "n zeroed bytes with indexing: b[i]. Freed with free(b)."),
    D("buffer_from", "pointers", "buffer_from(string | [bytes]) -> buffer", "A buffer holding the string's bytes or the array of ints."),
    D("memcpy", "pointers", "memcpy(dst, src, n)", "Copies n bytes (regions must not overlap; use memmove)."),
    D("memmove", "pointers", "memmove(dst, src, n)", "Copies n bytes; overlapping regions are fine."),
    D("memset", "pointers", "memset(dst, byte, n)", "Fills n bytes with a value."),
    D("memcmp", "pointers", "memcmp(a, b, n) -> int", "Compares n bytes: negative, zero or positive."),
    D("ptr_from_addr", "pointers", "ptr_from_addr(address, \"*T\")", "Forges a pointer from an address. `unsafe` only."),
    D("align_up", "pointers", "align_up(n, alignment)", "n rounded up to a multiple of alignment: align_up(5, 4) is 8."),
    D("align_down", "pointers", "align_down(n, alignment)", "n rounded down to a multiple of alignment."),
    D("is_aligned", "pointers", "is_aligned(n, alignment) -> bool", "True if n is a multiple of alignment."),
    /* ---- C: stdio */
    D("printf", "c: stdio", "printf(format, ...) -> bytes written", "C printf. Conversions: %d %i %u %x %X %o %b %c %s %f %e %g %a %p %%; flags - + space # 0; width and .precision (numbers or *). %s accepts any value; %b prints binary."),
    D("sprintf", "c: stdio", "sprintf(format, ...) -> string", "Like printf but returns the text."),
    D("format", "c: stdio", "format(format, ...) -> string", "Same as sprintf."),
    D("eprintf", "c: stdio", "eprintf(format, ...)", "printf to standard error."),
    D("puts", "c: stdio", "puts(string)", "Writes the string and a newline."),
    D("putchar", "c: stdio", "putchar(code) -> code", "Writes one character: putchar(65) writes A. Codes 128..255 are raw bytes; larger ones are UTF-8."),
    D("getchar", "c: stdio", "getchar() -> int", "Reads one byte from standard input; -1 (EOF) at the end."),
    D("fflush", "c: stdio", "fflush()", "Flushes standard output."),
    /* ---- C: string */
    D("strlen", "c: string", "strlen(s) -> int", "Length in bytes."),
    D("strcmp", "c: string", "strcmp(a, b) -> -1 | 0 | 1", "Compares two strings."),
    D("strncmp", "c: string", "strncmp(a, b, n) -> -1 | 0 | 1", "Compares at most n bytes."),
    D("strcasecmp", "c: string", "strcasecmp(a, b) -> -1 | 0 | 1", "Case-insensitive compare."),
    D("strchr", "c: string", "strchr(s, c) -> index or -1", "Index of the first occurrence of a character (a code or 1-char string)."),
    D("strrchr", "c: string", "strrchr(s, c) -> index or -1", "Index of the last occurrence."),
    D("strstr", "c: string", "strstr(haystack, needle) -> index or -1", "Index of the first occurrence of needle."),
    D("strcat", "c: string", "strcat(a, b) -> string", "A NEW string a+b (strings are values; nothing is modified)."),
    D("strdup", "c: string", "strdup(s) -> string", "A writable copy (string literals are read-only; lchar needs a copy)."),
    D("atoi", "c: string", "atoi(s) -> int", "Leading integer of a string (0 if none)."),
    D("atol", "c: string", "atol(s) -> int", "Same as atoi."),
    D("atof", "c: string", "atof(s) -> float", "Leading number of a string."),
    D("strtol", "c: string", "strtol(s [, base]) -> int", "Parses an integer in base 2..36 (base 0 detects a 0x prefix)."),
    D("strtod", "c: string", "strtod(s) -> float", "Parses a float."),
    D("itoa", "c: string", "itoa(n [, base]) -> string", "n as text in base 2..36: itoa(255, 16) is \"ff\"."),
    /* ---- C: ctype */
    D("isdigit", "c: ctype", "isdigit(c) -> bool", "c is a code or one-character string; likewise for the whole ctype family."),
    D("isalpha", "c: ctype", "isalpha(c) -> bool", "A letter."), D("isalnum", "c: ctype", "isalnum(c) -> bool", "A letter or digit."),
    D("isspace", "c: ctype", "isspace(c) -> bool", "Space, tab, newline, ..."), D("isupper", "c: ctype", "isupper(c) -> bool", "Upper-case letter."),
    D("islower", "c: ctype", "islower(c) -> bool", "Lower-case letter."), D("ispunct", "c: ctype", "ispunct(c) -> bool", "Punctuation."),
    D("isxdigit", "c: ctype", "isxdigit(c) -> bool", "Hexadecimal digit."), D("isprint", "c: ctype", "isprint(c) -> bool", "Printable (including space)."),
    D("iscntrl", "c: ctype", "iscntrl(c) -> bool", "Control character."), D("isgraph", "c: ctype", "isgraph(c) -> bool", "Printable, not space."),
    D("toupper", "c: ctype", "toupper(code | string)", "Upper-case a character code, or a whole string."),
    D("tolower", "c: ctype", "tolower(code | string)", "Lower-case a character code, or a whole string."),
    /* ---- C: stdlib */
    D("exit", "c: stdlib", "exit([status])", "Ends the process now (stdout is flushed). Also: main() returning an int."),
    D("abort", "c: stdlib", "abort()", "Ends the process abnormally."),
    D("rand", "c: stdlib", "rand() -> int", "A pseudo-random int 0..RAND_MAX. Deterministic until srand(seed)."),
    D("srand", "c: stdlib", "srand(seed)", "Seeds rand()."),
    D("time", "c: stdlib", "time() -> int", "Seconds since 1970-01-01 UTC."),
    D("qsort", "c: stdlib", "qsort(array, cmp(a, b))", "Sorts IN PLACE, stably. cmp returns negative, zero or positive."),
    D("bsearch", "c: stdlib", "bsearch(sorted, key, cmp(key, element)) -> index or -1", "Binary search of a sorted array."),
    /* ---- C: math */
    D("floor", "c: math", "floor(x) -> float", "Round down."), D("ceil", "c: math", "ceil(x) -> float", "Round up."),
    D("round", "c: math", "round(x) -> float", "Round to nearest, halves away from zero."), D("trunc", "c: math", "trunc(x) -> float", "Round toward zero."),
    D("fabs", "c: math", "fabs(x) -> float", "Absolute value."), D("pow", "c: math", "pow(x, y) -> float", "x to the power y."),
    D("fmod", "c: math", "fmod(x, y) -> float", "Floating-point remainder."), D("hypot", "c: math", "hypot(x, y) -> float", "sqrt(x*x + y*y)."),
    D("atan2", "c: math", "atan2(y, x) -> float", "Angle of the point (x, y)."), D("sin", "c: math", "sin(x) -> float", "Sine (radians)."),
    D("cos", "c: math", "cos(x) -> float", "Cosine."), D("tan", "c: math", "tan(x) -> float", "Tangent."),
    D("asin", "c: math", "asin(x) -> float", "Inverse sine."), D("acos", "c: math", "acos(x) -> float", "Inverse cosine."),
    D("atan", "c: math", "atan(x) -> float", "Inverse tangent."), D("exp", "c: math", "exp(x) -> float", "e to the x."),
    D("log", "c: math", "log(x) -> float", "Natural logarithm."), D("log2", "c: math", "log2(x) -> float", "Base-2 logarithm."),
    D("log10", "c: math", "log10(x) -> float", "Base-10 logarithm."), D("cbrt", "c: math", "cbrt(x) -> float", "Cube root."),
    /* ---- B */
    D("char", "b", "char(x, i) -> byte", "The i-th byte of a string or buffer (or the i-th word of a vector). B's char()."),
    D("lchar", "b", "lchar(x, i, byte)", "Stores a byte. Strings must be writable (strdup a literal first)."),
    D("getvec", "b", "getvec(n) -> vector", "A vector of n+1 zero words, indices 0..n, as in B."),
    D("rlsevec", "b", "rlsevec(vector)", "Frees a vector from getvec."),
    D("printn", "b", "printn(n [, base])", "Prints a number in base 2..36 (default 10), no newline."),
    D("putnumb", "b", "putnumb(n)", "Prints a number in decimal, no newline."),
    D("__vec", "b", "__vec(n)", "What `auto v[n];` calls: the same as getvec(n)."),
    /* ---- the help system itself */
    D("help", "core", "help([thing])", "This help. help() gives an overview; help(value) documents a value; help(\"name\") looks up a builtin, keyword, method, module or topic."),
    /* ---- constants */
    D("NULL", "constants", "NULL", "null."), D("EOF", "constants", "EOF", "-1, what getchar() returns at the end of input."),
    D("EXIT_SUCCESS", "constants", "EXIT_SUCCESS", "0."), D("EXIT_FAILURE", "constants", "EXIT_FAILURE", "1."),
    D("RAND_MAX", "constants", "RAND_MAX", "2147483647."), D("INT_MAX", "constants", "INT_MAX", "2147483647."),
    D("INT_MIN", "constants", "INT_MIN", "-2147483648."), D("INT64_MAX", "constants", "INT64_MAX", "9223372036854775807."),
    D("INT64_MIN", "constants", "INT64_MIN", "-9223372036854775808."), D("LLONG_MAX", "constants", "LLONG_MAX", "Same as INT64_MAX."),
    D("LLONG_MIN", "constants", "LLONG_MIN", "Same as INT64_MIN."), D("UINT32_MAX", "constants", "UINT32_MAX", "4294967295."),
    D("CHAR_BIT", "constants", "CHAR_BIT", "8."), D("M_PI", "constants", "M_PI", "3.14159265358979..."), D("M_E", "constants", "M_E", "2.71828182845904..."),
};
#define NBUILTINS ((int)(sizeof BUILTINS / sizeof BUILTINS[0]))

/* methods: kind 's' string, 'a' array, 'o' object / struct */
typedef struct { char kind; const char *name, *sig, *text; } MDoc;
static const MDoc METHODS[] = {
    { 's', "upper", "s.upper() -> string", "Upper-case copy." }, { 's', "lower", "s.lower() -> string", "Lower-case copy." },
    { 's', "trim", "s.trim() -> string", "Copy without leading/trailing whitespace." },
    { 's', "split", "s.split(separator) -> array", "Pieces between separators: \"a,b\".split(\",\") is [\"a\", \"b\"]." },
    { 's', "contains", "s.contains(text) -> bool", "True if text occurs in s." },
    { 's', "index_of", "s.index_of(text) -> int", "Index of the first occurrence, or -1." },
    { 's', "starts_with", "s.starts_with(prefix) -> bool", "Prefix test." }, { 's', "ends_with", "s.ends_with(suffix) -> bool", "Suffix test." },
    { 's', "replace", "s.replace(old, new) -> string", "Copy with every occurrence replaced." },
    { 's', "slice", "s.slice(start, end) -> string", "Bytes start..end-1 (both arguments are required)." },
    { 's', "repeat", "s.repeat(n) -> string", "n copies joined: \"ab\".repeat(2) is \"abab\"." },
    { 's', "pad_left", "s.pad_left(width, fill) -> string", "Left-pads to a width: \"7\".pad_left(3, \"0\") is \"007\"." },
    { 's', "pad_right", "s.pad_right(width, fill) -> string", "Right-pads to a width." },
    { 's', "chars", "s.chars() -> array", "One string per character." }, { 's', "lines", "s.lines() -> array", "The lines of the text." },
    { 'a', "push", "a.push(value)", "Appends in place." }, { 'a', "pop", "a.pop()", "Removes and returns the last element (an error if empty)." },
    { 'a', "join", "a.join(separator) -> string", "Joins the elements' text with the separator." },
    { 'a', "reverse", "a.reverse()", "Reverses IN PLACE and returns the array." },
    { 'a', "sort", "a.sort([cmp(a, b)])", "Sorts IN PLACE (ascending, or by cmp) and returns the array. For a sorted copy use sorted(a)." },
    { 'a', "map", "a.map(fn(x)) -> array", "New array of fn applied to each element." }, { 'a', "filter", "a.filter(fn(x)) -> array", "Elements for which fn is truthy." },
    { 'a', "reduce", "a.reduce(fn(acc, x) [, initial])", "Folds to one value." }, { 'a', "find", "a.find(fn(x))", "First element for which fn is truthy, or null." },
    { 'a', "any", "a.any(fn(x)) -> bool", "True if fn is truthy for some element." }, { 'a', "all", "a.all(fn(x)) -> bool", "True if fn is truthy for every element." },
    { 'a', "each", "a.each(fn(x))", "Calls fn on every element." },
    { 'a', "concat", "a.concat(other) -> array", "A new array: a followed by other." }, { 'a', "first", "a.first()", "The first element." },
    { 'a', "last", "a.last()", "The last element." }, { 'a', "clear", "a.clear()", "Removes every element." },
    { 'a', "insert", "a.insert(index, value)", "Inserts before the index." }, { 'a', "remove", "a.remove(index)", "Removes and returns the element at index." },
    { 'a', "extend", "a.extend(other)", "Appends every element of other, in place." },
    { 'a', "slice", "a.slice(start, end) -> array", "Elements start..end-1 as a new array." },
    { 'a', "index_of", "a.index_of(value) -> int", "Index of the first equal element, or -1." }, { 'a', "contains", "a.contains(value) -> bool", "True if an equal element exists." },
    { 'a', "sum", "a.sum()", "Sum of the elements." }, { 'a', "min", "a.min()", "Smallest element." }, { 'a', "max", "a.max()", "Largest element." },
    { 'o', "keys", "o.keys() -> array", "The keys." }, { 'o', "values", "o.values() -> array", "The values." },
    { 'o', "items", "o.items() -> array", "[[key, value], ...]." }, { 'o', "has", "o.has(key) -> bool", "True if the key exists." },
    { 'o', "get", "o.get(key [, default])", "The value, or default (null) when the key is missing." },
    { 'o', "merge", "o.merge(other) -> object", "A new object with other's keys added." },
};
#define NMETHODS ((int)(sizeof METHODS / sizeof METHODS[0]))

typedef struct { const char *name, *summary, *text; } Topic;
static const Topic KEYWORDS[] = {
    { "fn", "declare a function", "fn name(a: int, b = 2) -> int { return a + b; }     typed or untyped parameters, defaults, `...rest`\nfn(x) { return x * 2; }                                a function value (closure)\n/// doc comment lines above a fn are shown by help(name)." },
    { "let", "declare a variable", "let x = 5;    let a = 1, b = 2;    let x: int = 5;    let [p, q] = pair;\n`let` is for values you do not intend to reassign; `var` for ones you do." },
    { "var", "declare a mutable variable", "var n = 0;   n += 1;   n++;" },
    { "const", "declare a constant", "const LIMIT = 100;" },
    { "if", "conditional", "if (cond) { ... } else if (cond) { ... } else { ... }\nif (x) y++; else z++;          a body may be a single statement" },
    { "else", "alternative branch of an if", "see help(\"if\")" },
    { "while", "loop while a condition holds", "while (cond) { ... }     break; and continue; work; `outer: while (...)` lets `break outer;` leave a nested loop" },
    { "for", "loops", "for (i in 0..10) { }          i = 0..9\nfor (x in array) { }           each element         for (i, x in array) { }   index and element\nfor (var i = 0; i < n; i++) { }   C-style (the comma operator allows several steps)" },
    { "do", "loop that runs its body first", "do { ... } while (cond);" },
    { "switch", "multi-way branch, C semantics", "switch (x) {\n    case 1: ...; break;        execution FALLS THROUGH to the next case until a `break`\n    case 2, 3: ...; break;     several values per case\n    default: ...               taken when nothing matches (may stand anywhere)\n}\nValues are compared with ==: ints, strings, enum members. `continue` and `return` pass through to the enclosing loop / function." },
    { "case", "a branch of a switch", "see help(\"switch\")" }, { "default", "the fallback branch of a switch", "see help(\"switch\")" },
    { "break", "leave a loop or switch", "break;      break label;   (label: before a loop names it)" },
    { "continue", "next iteration", "continue;   continue label;" },
    { "return", "leave a function", "return value;   return;" },
    { "goto", "jump to a label", "goto retry;   ...   retry: statement\nThe label must be in the same or an enclosing block of the same function (jumping out of loops is fine; into a nested block is not)." },
    { "struct", "declare a record type", "struct Point { x: int, y: int }      let p = Point { x: 1, y: 2 };      p.x\npacked struct Header { ... }      no padding, alignment 1\n/// doc comment lines above a struct are shown by help(\"Point\")." },
    { "union", "declare overlapping fields", "union Bits { i: u32, f: f32 }   every field at offset 0; use through a pointer into a buffer:\nlet b = alloc(sizeof(Bits)) as *Bits;  b->f = 1.0;  print(b->i);" },
    { "enum", "declare named constants", "enum Color { Red, Green = 5, Blue }     Color.Green" },
    { "impl", "attach methods to a struct", "impl Point { fn len(self) -> float { ... } }     p.len()" },
    { "type", "type alias", "type Id = int;      (also: typedef int Id;)" }, { "typedef", "type alias, C style", "typedef int word;" },
    { "static", "a function-local that keeps its value", "fn next_id() -> int { static n = 100; return n++; }     initialised once; each closure has its own" },
    { "auto", "B-style declaration", "auto a, b, v[10];     v has 11 words (indices 0..10), as in B" },
    { "extrn", "B: declare outside names", "extrn putchar, printn;     checked when the statement runs" },
    { "import", "load a module or file", "import std.json;                 a standard module\nimport \"util.kc\" as u;           a KingCobra file\nimport \"lib.so\" as lib;          a native library" },
    { "from", "import a library function by name", "extern fn sqrt(x: float) -> float from \"libm.so.6\";" },
    { "as", "cast / rename", "x as *Point      import \"f.kc\" as f;" },
    { "extern", "declare a foreign (C) function", "extern fn name(args) -> type from \"lib\";" },
    { "unsafe", "allow raw pointer operations", "unsafe { *raw = 1; }    dereferencing a forged pointer needs `unsafe`" },
    { "comptime", "run code at compile time", "comptime { ... }" },
    { "match", "pattern match", "match (x) { 1 => ..., 2 | 3 => ..., _ => ... }" },
    { "try", "handle errors", "try { ... } catch (e) { ... }     throw value;   defer expr;   (defer runs at scope exit)" },
    { "catch", "the handler of a try", "see help(\"try\")" }, { "throw", "raise an error", "throw \"message\";    throw an object { code: 2 };" },
    { "defer", "run a statement when the scope ends", "let f = open(...); defer close(f);" },
    { "sizeof", "size in bytes of a type", "sizeof(Point)    sizeof(u32)    also alignof(T), offsetof(T, field)" },
    { "alignof", "alignment of a type", "alignof(Point)" }, { "offsetof", "byte offset of a field", "offsetof(Point, y)" },
    { "true", "boolean", "true / false" }, { "false", "boolean", "true / false" }, { "null", "the absent value", "null" },
    { "packed", "struct without padding", "packed struct Header { tag: u8, len: u32 }" },
};
#define NKEYWORDS ((int)(sizeof KEYWORDS / sizeof KEYWORDS[0]))

static const Topic TOPICS[] = {
    { "topics", "this list", "" },
    { "builtins", "every builtin function, by category", "" },
    { "keywords", "every keyword, with its syntax", "" },
    { "methods", "string, array and object methods", "" },
    { "modules", "the standard-library modules", "" },
    { "types", "the types of values",
      "int        64-bit integer           5   0xff   0b101   'a'   1_000\n"
      "float      64-bit float             1.5   2e-3\n"
      "bool       true / false\n"
      "string     UTF-8 text, immutable literal   \"hi\"   f\"x = {x}\"   (len() counts bytes)\n"
      "array      growable list            [1, 2, 3]   [0; 8]   a[i]   a[1..3]\n"
      "object     key/value map            { name: \"x\", n: 3 }   o.name   o[\"name\"]\n"
      "struct     declared record          struct Point { x: int, y: int }\n"
      "null       the absent value\n"
      "fn         functions are values     fn(x) { return x; }\n"
      "pointer    bounds-checked pointer   *T, from alloc()/buffer()   see help(\"pointers\")\n"
      "Sized integer types for struct fields and pointers: u8 i8 u16 i16 u32 i32 u64 i64 f32 f64.\n"
      "typeof(x) names the type of a value." },
    { "operators", "operators and precedence",
      "highest   a.b  a[i]  a(x)  a->b  x++  x--\n"
      "          ! ~ - & * ++x --x  (unary)      as (cast)\n"
      "          * / %          + -          << >>          < <= > >=          == !=\n"
      "          &  ^  |        &&  ||        ??  (null-coalesce)      ?:  (ternary)\n"
      "lowest    = += -= *= /= %= &= |= ^= <<= >>=       and `a, b` (comma) only inside ( ) and for-headers\n"
      "Ranges: 1..4 (1,2,3).  Integer arithmetic wraps; `/` on ints truncates (idiv rounds down)." },
    { "strings", "working with text", "Literals \"...\" (escapes \\n \\t \\\" \\xHH \\u{...}), f\"..{expr}..\" interpolation, adjacent literals join.\nMethods: help(\"methods\").  C-style functions: strlen strcmp strstr toupper sprintf ...  (help(\"builtins\")).\nlen(s) counts BYTES; s.chars() gives characters." },
    { "arrays", "working with arrays", "let a = [3, 1, 2];  a.push(4);  a.len;  a[0];  a[1..3];  [0; 10]\nfor (x in a) { }    a.map(fn)  a.filter(fn)  a.sort()  sorted(a)  qsort(a, cmp)\nMethods: help(\"methods\")." },
    { "objects", "objects and structs", "let o = { name: \"x\", n: 3 };   o.name   o[\"n\"] = 4;   keys(o)   o.has(\"n\")   o.get(\"z\", 0)\nstruct P { x: int, y: int }   let p = P { x: 1, y: 2 };   p.x   (fields are checked: p.z is an error with a suggestion)" },
    { "errors", "errors and exceptions", "try { risky(); } catch (e) { print(e); }    throw \"text\";    error(\"text\");    assert(cond, \"why\")\nAn uncaught error ends the program with exit status 65 and shows the source line with a caret.\nTests: fn test_x() { assert_eq(a, b); }   run with `kingcobra test`." },
    { "memory", "memory management", "free(x)  free_deep(x)  is_freed(x)   mem_stats()  mem_report()   defer free(p);\nUse after free is a clear error (freed blocks are quarantined). `kingcobra --leaks file.kc` reports leaks.\nDetails: docs/MEMORY.md." },
    { "pointers", "pointers and raw memory", "let b = buffer(16);  let p = &b[0] as *u32;  *p = 7;  p++;  *p = 9;     bounds-checked\nlet n = alloc(sizeof(Node)) as *Node;  n->next = null;  free(n);\nalloc  alloc_array  realloc  buffer  buffer_from  memcpy  memmove  memset  memcmp  sizeof  alignof  offsetof\nRaw (forged) pointers need `unsafe`. Details: docs/POINTERS.md." },
    { "preprocessor", "#define, #include, #if", "#define N 10    #define SQ(x) ((x)*(x))    #define LOG(f, ...) printf(f, __VA_ARGS__)    #x  a##b\n#if / #ifdef / #ifndef / #elif / #else / #endif    #include \"f.kc\"    #pragma once    #error msg\n__FILE__ __LINE__ __COUNTER__     kingcobra -DNAME=value run f.kc     kingcobra -E f.kc (show the result)\nDetails: docs/C_AND_B.md." },
    { "c", "C features", "do/while, C-style for, switch with fall-through, goto, ++/--, static, typedef, union, [v; n], printf family,\nstring.h/ctype.h/stdlib.h/math.h functions, the preprocessor, main(argc, argv).  Details: docs/C_AND_B.md." },
    { "b", "B features", "name(a, b) { ... }  (a function without `fn`)   auto a, v[10];   extrn name;   char(s, i)  lchar(s, i, c)\ngetvec(n)  rlsevec(v)  printn(n, base)  putnumb(n)  putchar(c)  switch / goto / labels.  Details: docs/C_AND_B.md." },
    { "docs", "documenting your own code", "Put `///` lines directly above a fn or struct:\n\n    /// Returns the area of a w x h rectangle.\n    fn area(w: float, h: float) -> float { return w * h; }\n\nThen help(area) (or help(\"area\")) shows the signature and that text." },
    { "repl", "using the REPL", "kingcobra repl.  `help` or `help()` here; `?name` is shorthand for help(\"name\"); :help lists REPL commands (:help topic works too).\nDetails: docs/REPL.md." },
    { "cli", "the command line", "kingcobra run file.kc | -e 'code' | -p 'expr' | check | test | new | completions ...   `kingcobra help` (outside the language).\nDetails: docs/CLI.md." },
};
#define NTOPICS ((int)(sizeof TOPICS / sizeof TOPICS[0]))

/* ================================================================ helpers */

static void print_wrapped(const char *s, int indent) {
    /* the tables are written to fit; just indent every line */
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        printf("%*s%.*s\n", indent, "", (int)n, p);
        p += n; if (*p == '\n') p++;
    }
}
static void print_text(const char *s, int indent) {                 /* word-wrap prose to ~78 columns */
    int col = indent, first = 1; const char *p = s;
    printf("%*s", indent, "");
    while (*p) {
        while (*p == ' ') p++;
        const char *w = p; while (*p && *p != ' ') p++;
        int n = (int)(p - w); if (!n) break;
        if (!first && col + 1 + n > 78) { printf("\n%*s", indent, ""); col = indent; first = 1; }
        if (!first) { putchar(' '); col++; }
        printf("%.*s", n, w); col += n; first = 0;
    }
    putchar('\n');
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) for (int j = 1; j <= lb; j++) {
        int c = a[i - 1] == b[j - 1] ? 0 : 1, m = d[i - 1][j] + 1;
        if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
        if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
        d[i][j] = m;
    }
    return d[la][lb];
}

static const Doc *find_builtin(const char *name) {
    for (int i = 0; i < NBUILTINS; i++) if (!strcmp(BUILTINS[i].name, name)) return &BUILTINS[i];
    return NULL;
}
static const Topic *find_keyword(const char *name) {
    for (int i = 0; i < NKEYWORDS; i++) if (!strcmp(KEYWORDS[i].name, name)) return &KEYWORDS[i];
    return NULL;
}
static const Topic *find_topic(const char *name) {
    for (int i = 0; i < NTOPICS; i++) if (!strcmp(TOPICS[i].name, name)) return &TOPICS[i];
    return NULL;
}
static const char *kind_word(char k) { return k == 's' ? "string" : k == 'a' ? "array" : "object"; }

static void show_builtin(const Doc *d) {
    printf("%s  —  %s\n", d->sig, d->cat);
    print_text(d->text, 4);
}
static void show_method(const MDoc *m) {
    printf("%s  —  %s method\n", m->sig, kind_word(m->kind));
    print_text(m->text, 4);
}
static void show_keyword(const Topic *t) {
    printf("%s  —  %s\n", t->name, t->summary);
    print_wrapped(t->text, 4);
}

static void list_builtins(void) {
    const char *last = "";
    for (int i = 0; i < NBUILTINS; i++) {
        if (strcmp(BUILTINS[i].cat, last) != 0) { last = BUILTINS[i].cat; printf("\n%s:\n  ", last); int col = 2; (void)col; }
        else printf(" ");
        printf("%s", BUILTINS[i].name);
    }
    printf("\n\nhelp(\"name\") shows one of them; help(\"c\") and help(\"b\") describe the C and B families.\n");
}
static void list_keywords(void) {
    for (int i = 0; i < NKEYWORDS; i++) printf("  %-10s %s\n", KEYWORDS[i].name, KEYWORDS[i].summary);
    printf("\nhelp(\"name\") shows the syntax.\n");
}
static void list_methods(void) {
    for (char k = 0; k < 3; k++) {
        char kk = "sao"[(int)k];
        printf("%s methods:\n  ", kind_word(kk));
        for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == kk) printf("%s ", METHODS[i].name);
        printf("\n\n");
    }
    printf("help(\"string.split\") or help(\"array.map\") shows one.\n");
}
static void list_modules(KcInterp *ip) {
    (void)ip;
    printf("Standard-library modules (import std.<name>):\n  ");
    for (int i = 0; kc_std_module_names[i]; i++) printf("%s ", kc_std_module_names[i]);
    printf("\n\nhelp(\"std.json\") lists a module's functions (after `import std.json;` help(json) does too).\nDocs: docs/STD_<NAME>.md.\n");
}
static void list_topics(void) {
    printf("Topics (help(\"topic\")):\n");
    for (int i = 0; i < NTOPICS; i++) printf("  %-13s %s\n", TOPICS[i].name, TOPICS[i].summary);
    printf("\nAlso: any builtin, keyword, method or module name — help(\"map\"), help(\"switch\"), help(\"std.json\").\n");
}

static void overview(void) {
    printf("KingCobra %s — help\n\n", KC_VERSION);
    printf("  help(x)         document a value:   help(print)   help(my_function)   help(some_module)\n"
           "  help(\"name\")    look something up:  help(\"map\")   help(\"switch\")   help(\"string.split\")   help(\"std.json\")\n"
           "  help(\"topics\")  the list of topics: types, operators, strings, arrays, memory, pointers, preprocessor, ...\n"
           "  help(\"builtins\")  every builtin function     help(\"keywords\")  every keyword\n\n"
           "Document your own functions with `///` lines above them (help(\"docs\")).\n"
           "Outside the language: `kingcobra help` (the command line), and :help in the REPL.\n");
}

/* ================================================================ values */

static bool user_lookup(KcInterp *ip, const char *name, KcValue *out);

static void print_signature(KcClosure *cl, const char *name) {
    KcNode *f = cl->fnnode;
    printf("fn %s(", name);
    for (int k = 0; k < f->as.fn.params.count; k++) {
        bool dflt = FN_DEFAULTS(f) && FN_DEFAULTS(f)[k];
        const char *ty = f->as.fn.param_types ? f->as.fn.param_types[k] : NULL;
        bool typed = ty && strcmp(ty, "any") != 0;
        printf("%s%s%s%s%s%s", k ? ", " : "", (f->as.fn.variadic && k == f->as.fn.params.count - 1) ? "..." : "",
               f->as.fn.params.items[k]->as.ident.name, typed ? ": " : "", typed ? ty : "", dflt ? " = ..." : "");
    }
    printf(")");
    if (f->as.fn.ret_type && strcmp(f->as.fn.ret_type, "void") != 0 && strcmp(f->as.fn.ret_type, "any") != 0) printf(" -> %s", f->as.fn.ret_type);
    printf("\n");
}

static void show_user_doc(const char *doc) {
    if (!doc) return;
    char *copy = kc_strdup(doc);
    size_t n = strlen(copy); while (n && (copy[n - 1] == '\n' || copy[n - 1] == ' ')) copy[--n] = 0;
    printf("\n"); print_wrapped(copy, 4);
    free(copy);
}

static void show_closure(KcClosure *cl, const char *fallback) {
    const char *name = cl->name ? cl->name : (fallback ? fallback : "<anonymous>");
    print_signature(cl, name);
    if (cl->fnnode->as.fn.doc) show_user_doc(cl->fnnode->as.fn.doc);
    else printf("\n    (no documentation: put `///` lines above the function to add some)\n");
    if (cl->fnnode->pos.file) printf("\n    defined at %s:%d\n", cl->fnnode->pos.file, cl->fnnode->pos.line);
}

static void show_struct_type(KcInterp *ip, KcStructTypeDef *d) {
    printf("%s %s {", d->is_union ? "union" : d->packed ? "packed struct" : "struct", d->name);
    for (int k = 0; k < d->field_count; k++) printf("%s%s: %s", k ? ", " : " ", d->field_names[k], d->field_types && d->field_types[k] ? d->field_types[k] : "any");
    printf(" }\n");
    if (d->doc) show_user_doc(d->doc);
    bool head = false;
    for (int i = 0; i < ip->nmethods; i++) {
        if (strcmp(ip->methods[i].type, d->name) != 0) continue;
        if (!head) { printf("\n    methods:\n"); head = true; }
        printf("      %s%s\n", ip->methods[i].name, ip->methods[i].instance ? "(self, ...)" : "  (static)");
    }
}

/* the module a native function belongs to, for help(some_module.function) */
static bool find_native_name(KcInterp *ip, KcNativeFn fn, char *out, size_t cap, bool *builtin) {
    for (int i = 0; i < ip->builtins_env->count; i++) {
        KcEnvEntry *e = &ip->builtins_env->entries[i];
        if (e->value.type == VAL_NATIVE && e->value.as.native == fn) { snprintf(out, cap, "%s", e->name); *builtin = true; return true; }
    }
    *builtin = false;
    for (KcEnv *env = ip->globals; env; env = env->parent)
        for (int i = 0; i < env->count; i++) {
            KcValue v = env->entries[i].value;
            if (v.type != VAL_MODULE) continue;
            for (int k = 0; k < v.as.module->count; k++)
                if (v.as.module->entries[k].fn == fn) { snprintf(out, cap, "%s.%s", env->entries[i].name, v.as.module->entries[k].name); return true; }
        }
    return false;
}

static void show_module(const char *name, KcModule *m) {
    printf("module %s", name);
    if (m->name && strcmp(m->name, name) != 0) printf("  (std.%s)", m->name);
    printf("\n\n  functions:");
    int col = 100;
    for (int i = 0; i < m->count; i++) {
        int n = (int)strlen(m->entries[i].name);
        if (col + n + 1 > 76) { printf("\n    "); col = 4; }
        printf("%s ", m->entries[i].name); col += n + 1;
    }
    char up[160]; snprintf(up, sizeof up, "%s", m->name ? m->name : name);
    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    printf("\n\n    Reference: docs/STD_%s.md\n", up);
}

static void show_type_overview(KcValue v) {
    const char *t = kc_type_name(v);
    printf("a value of type '%s'\n", t);
    switch (v.type) {
        case VAL_STRING: printf("\n"); for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == 's') printf("    %s\n", METHODS[i].sig); break;
        case VAL_ARRAY: printf("\n    a.len  (property)\n"); for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == 'a') printf("    %s\n", METHODS[i].sig); break;
        default: break;
    }
    printf("\n    help(\"types\") describes every type; help(\"methods\") the methods.\n");
}

static void help_value(KcInterp *ip, KcValue v) {
    switch (v.type) {
        case VAL_CLOSURE: show_closure(v.as.closure, NULL); return;
        case VAL_NATIVE: {
            char nm[160]; bool builtin;
            if (find_native_name(ip, v.as.native, nm, sizeof nm, &builtin)) {
                if (builtin) { const Doc *d = find_builtin(nm); if (d) { show_builtin(d); return; } }
                char up[160] = "";
                if (!builtin) {
                    char alias[160]; snprintf(alias, sizeof alias, "%s", nm); char *dt = strchr(alias, '.'); if (dt) *dt = 0;
                    KcValue mv;
                    if (user_lookup(ip, alias, &mv) && mv.type == VAL_MODULE && mv.as.module->name) { snprintf(up, sizeof up, "%s", mv.as.module->name); for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c); }
                }
                printf("%s  —  %s\n    (a native function%s%s%s)\n", nm, builtin ? "builtin" : "module function", up[0] ? "; arguments: docs/STD_" : "", up, up[0] ? ".md" : "");
            } else printf("a native function\n");
            return;
        }
        case VAL_MODULE: show_module(v.as.module->name, v.as.module); return;
        case VAL_STRUCT: {
            KcStructInst *s = v.as.strct;
            if (kc_is_object(v)) {
                printf("an object with %d key(s):", s->count);
                for (int i = 0; i < s->count && i < 16; i++) printf(" %s", s->names[i]);
                printf("\n\n"); for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == 'o') printf("    %s\n", METHODS[i].sig);
                return;
            }
            KcStructTypeDef *d = s->type_name ? kc_find_struct_type(ip, s->type_name) : NULL;
            printf("an instance of "); if (d) show_struct_type(ip, d); else printf("%s\n", s->type_name ? s->type_name : "a struct");
            return;
        }
        default: show_type_overview(v); return;
    }
}

/* ================================================================ string lookups */

static bool user_lookup(KcInterp *ip, const char *name, KcValue *out) {
    for (KcEnv *e = ip->globals; e; e = e->parent) {
        for (int i = e->count - 1; i >= 0; i--)
            if (!strcmp(e->entries[i].name, name)) {
                KcValue v = e->entries[i].value;
                if (e == ip->builtins_env) return false;
                *out = v; return true;
            }
        if (e == ip->builtins_env) break;
    }
    return false;
}

static void suggest(const char *what) {
    const char *best[3] = {0}; int lim = strlen(what) <= 7 ? 2 : 3; int bd[3] = {lim, lim, lim};
    #define CONSIDER(nm) do { const char *c_ = (nm); int d_ = edit_distance(what, c_); \
        for (int k_ = 0; k_ < 3; k_++) if (d_ < bd[k_]) { for (int m_ = 2; m_ > k_; m_--) { bd[m_] = bd[m_ - 1]; best[m_] = best[m_ - 1]; } bd[k_] = d_; best[k_] = c_; break; } } while (0)
    for (int i = 0; i < NBUILTINS; i++) CONSIDER(BUILTINS[i].name);
    for (int i = 0; i < NKEYWORDS; i++) CONSIDER(KEYWORDS[i].name);
    for (int i = 0; i < NTOPICS; i++) CONSIDER(TOPICS[i].name);
    #undef CONSIDER
    printf("No documentation found for '%s'.", what);
    if (best[0]) { printf(" Did you mean"); for (int k = 0; k < 3 && best[k]; k++) printf("%s '%s'", k ? "," : "", best[k]); printf("?"); }
    printf("\nTry help(\"topics\"), help(\"builtins\") or help(\"keywords\").\n");
}

static void check_undocumented(KcInterp *ip) {                           /* a test aid: help("__undocumented") */
    int n = 0;
    for (int i = 0; i < ip->builtins_env->count; i++) {
        const char *nm = ip->builtins_env->entries[i].name;
        if (!find_builtin(nm)) { printf("%s%s", n++ ? " " : "undocumented: ", nm); }
    }
    for (int k = 0; k < 3; k++) { char kk = "sao"[k]; const char *names[64]; int c = kc_method_names(k, names, 64);
        for (int i = 0; i < c; i++) { bool ok = false; for (int j = 0; j < NMETHODS; j++) if (METHODS[j].kind == kk && !strcmp(METHODS[j].name, names[i])) ok = true;
            if (!ok) printf("%s%s.%s", n++ ? " " : "undocumented: ", kind_word(kk), names[i]); } }
    printf(n ? "\n" : "undocumented: none\n");
}

static void help_string(KcInterp *ip, const char *arg) {
    char name[160]; snprintf(name, sizeof name, "%s", arg);
    size_t n = strlen(name); while (n && isspace((unsigned char)name[n - 1])) name[--n] = 0;
    const char *s = name; while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) { overview(); return; }
    if (!strcmp(s, "__undocumented")) { check_undocumented(ip); return; }

    if (!strcmp(s, "topics")) { list_topics(); return; }
    if (!strcmp(s, "builtins")) { list_builtins(); return; }
    if (!strcmp(s, "keywords")) { list_keywords(); return; }
    if (!strcmp(s, "methods")) { list_methods(); return; }
    if (!strcmp(s, "modules")) { list_modules(ip); return; }
    if (!strcmp(s, "string") || !strcmp(s, "array") || !strcmp(s, "object")) {          /* the type: its topic and its methods */
        const Topic *tt = find_topic(!strcmp(s, "string") ? "strings" : !strcmp(s, "array") ? "arrays" : "objects");
        char kk = s[0] == 's' ? 's' : s[0] == 'a' ? 'a' : 'o';
        printf("%s  —  %s\n", s, tt->summary); print_wrapped(tt->text, 4);
        printf("\n    methods:\n"); for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == kk) printf("      %s\n", METHODS[i].sig);
        return;
    }

    bool found = false;
    /* the program's own definitions first: functions, structs, modules, imported names */
    KcValue uv;
    if (user_lookup(ip, s, &uv) && (uv.type == VAL_CLOSURE || uv.type == VAL_MODULE || uv.type == VAL_STRUCT)) { help_value(ip, uv); found = true; }
    KcStructTypeDef *sd = kc_find_struct_type(ip, s);
    if (sd) { if (found) printf("\n"); show_struct_type(ip, sd); found = true; }

    /* module.function of an imported module */
    const char *dot = strchr(s, '.');
    if (dot && !found) {
        char left[96]; size_t ln = (size_t)(dot - s); if (ln >= sizeof left) ln = sizeof left - 1;
        memcpy(left, s, ln); left[ln] = 0;
        const char *right = dot + 1;
        const char *kind = !strcmp(left, "string") ? "s" : !strcmp(left, "array") ? "a" : !strcmp(left, "object") ? "o" : NULL;
        if (kind) {
            for (int i = 0; i < NMETHODS; i++) if (METHODS[i].kind == kind[0] && !strcmp(METHODS[i].name, right)) { show_method(&METHODS[i]); return; }
        }
        KcValue mv;
        if (user_lookup(ip, left, &mv) && mv.type == VAL_MODULE) {
            for (int k = 0; k < mv.as.module->count; k++)
                if (!strcmp(mv.as.module->entries[k].name, right)) {
                    char up[160]; snprintf(up, sizeof up, "%s", mv.as.module->name ? mv.as.module->name : left);
                    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
                    printf("%s.%s  —  module function\n    (native; see docs/STD_%s.md for its arguments)\n", left, right, up);
                    return;
                }
        }
        if (!strcmp(left, "std")) { help_string(ip, right); return; }
        if (user_lookup(ip, left, &mv) && mv.type == VAL_STRUCT) {
            KcValue *fv = NULL; for (int i = 0; i < mv.as.strct->count; i++) if (!strcmp(mv.as.strct->names[i], right)) fv = &mv.as.strct->values[i];
            if (fv) { help_value(ip, *fv); return; }
        }
        /* Type.method of an impl block */
        for (int i = 0; i < ip->nmethods; i++)
            if (!strcmp(ip->methods[i].type, left) && !strcmp(ip->methods[i].name, right) && ip->methods[i].fn.type == VAL_CLOSURE) { show_closure(ip->methods[i].fn.as.closure, right); return; }
    }

    const Topic *t = find_topic(s);
    if (t && t->text[0]) { if (found) printf("\n"); printf("%s  —  %s\n", t->name, t->summary); print_wrapped(t->text, 4); return; }
    if (found) return;

    const Doc *d = find_builtin(s);
    if (d) { show_builtin(d); found = true; }
    for (int i = 0; i < NMETHODS; i++)
        if (!strcmp(METHODS[i].name, s)) { if (found) printf("\n"); show_method(&METHODS[i]); found = true; }
    const Topic *kw = find_keyword(s);
    if (kw) { if (found) printf("\n"); show_keyword(kw); found = true; }
    if (found) return;

    /* a standard module: "std.json" or just "json" (loaded or not) */
    const char *mname = !strncmp(s, "std.", 4) ? s + 4 : s;
    for (int i = 0; kc_std_module_names[i]; i++)
        if (!strcmp(kc_std_module_names[i], mname)) {
            KcValue mv;
            if (user_lookup(ip, mname, &mv) && mv.type == VAL_MODULE) { show_module(mname, mv.as.module); return; }
            char up[160]; snprintf(up, sizeof up, "%s", mname); for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
            printf("module std.%s\n\n    Load it with:  import std.%s;   then help(%s) lists its functions.\n    Reference: docs/STD_%s.md\n", mname, mname, mname, up);
            return;
        }
    suggest(s);
}

static KcValue b_help(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "help");
    if (n > 1) kc_error_at(pos, "usage: help([thing])   e.g. help(print), help(\"map\"), help(\"topics\")");
    if (n == 0) overview();
    else if (a[0].type == VAL_STRING) help_string(ip, a[0].as.s->data);
    else help_value(ip, a[0]);
    fflush(stdout);
    return kc_null();
}

void kc_register_help_builtin(KcInterp *ip) {
    KcValue v; v.type = VAL_NATIVE; v.as.native = b_help;
    kc_env_define(ip->builtins_env, "help", v, false);
}
