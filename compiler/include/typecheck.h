#ifndef KC_TYPECHECK_H
#define KC_TYPECHECK_H
#include "ast.h"

/* KingCobra's static type checker (v0.2 scope, stated plainly):
 *
 * Runs once, after parsing and before the program executes, over
 * top-level functions and struct declarations. It checks:
 *   - `let`/`var` initializers against an explicit type annotation
 *   - function call argument count and types against the function's
 *     declared signature (for calls to named, top-level functions)
 *   - `return <expr>;` against the enclosing function's declared
 *     return type
 *   - struct literal field values against the struct's declared field
 *     types
 *   - binary/unary operator operand types, where both sides are
 *     statically known
 *
 * It is deliberately permissive rather than exhaustive: anywhere a
 * type can't be determined statically — values from `std.*` modules,
 * values returned through a struct field typed as a bare `fn`,
 * anything passing through a closure stored in a variable rather than
 * called by name, anything from an imported file (each file's own
 * `import "x.kc"` content isn't currently type-checked at the call
 * site in the importer) — the checker treats it as the wildcard type
 * `any` and skips the check rather than guessing and risking a false
 * positive on a valid program. This is a real, useful type-error
 * catcher for the common cases, not a complete type system; see
 * docs/ROADMAP.md for what's intentionally left for v0.3 (true
 * inference, exhaustive missing-return-path analysis, checked array
 * element types).
 *
 * Reports the first error via kc_error_at (same exit code and message
 * style as parse errors) and does not return in that case.
 */
void kc_typecheck_program(KcNode *program, const char *filename);

/* REPL variant: registries persist between calls so later inputs can call
 * functions / build structs declared earlier. Snapshot before a check and
 * restore it if the check fails, so a rejected input leaves no trace. */
void kc_typecheck_repl(KcNode *program, const char *filename);
void *kc_typecheck_snapshot(void);
void kc_typecheck_restore(void *snap);   /* also frees the snapshot */
void kc_typecheck_discard(void *snap);
void kc_typecheck_reset(void);

#endif
