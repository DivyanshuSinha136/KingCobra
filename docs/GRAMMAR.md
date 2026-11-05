# KingCobra grammar (v0.8)

Notation: `A B` sequence, `A | B` choice, `[A]` optional, `{A}` zero or more, `"x"` a literal token,
`UPPER` a token class. This is the grammar the parser implements; the *meaning* is in
[LANGUAGE.md](LANGUAGE.md). Statements end with `;` unless they end with a `}` block.

## Tokens
```
IDENT    = (letter | "_") { letter | digit | "_" }
INT      = decimal | "0x" hex | "0b" bin | "0o" oct      digits may be separated by "_"; exact 64-bit
         | "'" char "'"                                   a character literal is an int (its code point)
FLOAT    = digits "." digits [exp] | digits exp           exp = ("e"|"E") ["+"|"-"] digits
STRING   = '"' { char | escape } '"'                      may span lines
FSTRING  = 'f"' { char | escape | "{{" | "}}" | "{" expression "}" } '"'
escape   = "\n" | "\t" | "\r" | "\0" | "\\" | "\"" | "\'" | "\x" hex hex | "\u{" hex+ "}"
comment  = "//" … end of line | "/*" … "*/"
keywords = let var fn struct if else while for in return break continue import as extern from
           unsafe comptime match true false null
contextual (keywords only in the shapes below) = type enum impl const try catch throw defer packed
```

## Program and declarations
```
program      = { statement }
statement    = let | var | const | destructure | fn_decl | struct_decl | enum_decl | impl_decl
             | type_alias | import | extern_decl | if | while | for | labeled_loop | return | break
             | continue | block | unsafe_block | comptime_block | defer | try | throw
             | match_stmt | expr_stmt

let          = "let" IDENT [":" type] "=" expr ";"
var          = "var" IDENT [":" type] "=" expr ";"
const        = "const" IDENT [":" type] "=" expr ";"          (a contextual keyword; same as let)
destructure  = ("let" | "var" | "const") ( "[" pat_item {"," pat_item} ["," "..." IDENT] "]"
                                         | "{" IDENT [":" IDENT] {"," IDENT [":" IDENT]} "}" ) "=" expr ";"
pat_item     = IDENT                                            "_" skips the element

fn_decl      = "fn" IDENT ["<" IDENT {"," IDENT} ">"] "(" [params] ")" ["->" type] block
params       = param {"," param}
param        = IDENT [":" type] ["=" expr]  |  "..." IDENT [":" type]        (the "..." one must be last)
struct_decl  = ["packed"] "struct" IDENT "{" [IDENT ":" type {"," IDENT ":" type} [","]] "}"
enum_decl    = "enum" IDENT "{" IDENT ["=" expr] {"," IDENT ["=" expr]} [","] "}"
impl_decl    = "impl" IDENT "{" { fn_decl } "}"
type_alias   = "type" IDENT "=" type ";"

type         = {"*"} (IDENT | "fn") { "[" [INT] "]" }          e.g. int  string[]  *u8  u8[16]  *Node
import       = "import" ( "std." IDENT | STRING ) ["as" IDENT] ";"
extern_decl  = "extern" "fn" IDENT "(" [type {"," type}] ")" ["->" type] "from" STRING ";"
```

## Statements
```
body         = block | statement                          a single statement is a body (C)
block        = "{" { statement } "}"
if           = "if" "(" expr ")" body ["else" (if | body)]
while        = "while" "(" expr ")" body
do_while     = "do" block "while" "(" expr ")" [";"]
c_for        = "for" "(" [decls | expr {"," expr}] ";" [expr] ";" [expr {"," expr}] ")" body
switch       = "switch" "(" expr ")" "{" { ("case" expr {"," expr} ":" | "default" ":") { statement } } "}"
label        = IDENT ":" statement          goto = "goto" IDENT ";"
static       = "static" (decl | IDENT [":" type] ["=" expr] ";")     typedef = "typedef" type IDENT ";"
auto         = "auto" IDENT ["[" expr "]"] {"," ...} ";"     extrn = "extrn" IDENT {"," IDENT} ";"
b_function   = IDENT "(" [IDENT {"," IDENT}] ")" block        a function without `fn`
empty        = ";"
(do/switch/case/default/goto/static/typedef/union/auto/extrn are contextual words: still usable as names)
for          = "for" "(" IDENT ["," IDENT] "in" (expr | expr ".." expr) ")" block
return       = "return" [expr] ";"
break        = "break" [IDENT] ";"            continue = "continue" [IDENT] ";"
defer        = "defer" (expr ";" | block)     the word is a keyword only when a name, "{", string or number follows
try          = "try" block "catch" ["(" IDENT ")"] block
throw        = "throw" expr ";"
unsafe_block = "unsafe" block                 comptime_block = "comptime" block
match_stmt   = match_expr [";"]               a match used as a statement needs no ";"
expr_stmt    = expr ";"
```

## Expressions (lowest to highest precedence)
```
expr        = assignment
assignment  = ternary [ ("=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=") assignment ]
ternary     = coalesce ["?" ternary ":" ternary]
coalesce    = or { "??" or }
or          = and { "||" and }
and         = equality { "&&" equality }
equality    = comparison { ("==" | "!=") comparison }
comparison  = bit_or { ("<" | "<=" | ">" | ">=" | "in") bit_or }
bit_or      = bit_xor { "|" bit_xor }       bit_xor = bit_and { "^" bit_and }       bit_and = shift { "&" shift }
shift       = additive { ("<<" | ">>" | ">>>") additive }
additive    = multiplicative { ("+" | "-") multiplicative }
multiplicative = cast { ("*" | "/" | "%") cast }
cast        = unary { "as" type }
unary       = ("-" | "!" | "~" | "&" | "*" | "++" | "--") unary | postfix
postfix     = primary { "(" [args] ")" | "[" (expr | [expr] ".." [expr]) "]" | "." IDENT | "->" IDENT | "++" | "--" }
                         `(a, b)` is the comma operator; `[v; n]` an n-element array; adjacent STRINGs join
primary     = INT | FLOAT | STRING | FSTRING | "true" | "false" | "null" | IDENT
            | "(" expr ")" | "[" [expr {"," expr}] "]"
            | "{" [key ":" expr {"," key ":" expr}] "}"            object literal; key = IDENT | STRING
            | IDENT "{" [IDENT ":" expr {"," IDENT ":" expr}] "}"  struct literal
            | "fn" "(" [params] ")" ["->" type] block              closure
            | match_expr | "unsafe" block | sizeof_expr
match_expr  = "match" "(" expr ")" "{" arm {"," arm} [","] "}"
arm         = pattern {"|" pattern} "=>" expr
pattern     = INT | FLOAT | STRING | "true" | "false" | "null" | "-" (INT | FLOAT) | "_" | IDENT {"." IDENT}
sizeof_expr = ("sizeof" | "alignof") "(" type ")" | "offsetof" "(" IDENT "," IDENT ")"
```

## Disambiguation rules
- `{` at the start of a statement is a block; `{` anywhere an expression is expected is an object literal.
- `IDENT {` is a struct literal only where an expression is expected.
- `outer: for …` / `outer: while …` is a label; a `:` after an identifier is otherwise a type annotation,
  a field name or the else-part of `?:`.
- `type`, `enum`, `impl`, `const`, `try`, `throw`, `defer`, `packed` are recognised only when the tokens
  after them form the shapes above, so `let type = 1;` and a struct field named `try` remain legal.
- An f-string expression is parsed with the same expression grammar; a `"` inside `{ … }` starts a nested string.
