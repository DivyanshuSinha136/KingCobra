/* kcide — code completion: keywords, types, builtins, snippets, std.* module members (scanned from the
 * KingCobra source tree when available), declared symbols and words from open documents. */
#ifndef KCIDE_COMPLETE_H
#define KCIDE_COMPLETE_H
#include "doc.h"
typedef enum { CK_KEYWORD, CK_TYPE, CK_BUILTIN, CK_FUNC, CK_MODULE, CK_SNIPPET, CK_WORD, CK_MEMBER, CK_SYMBOL } CKind;
typedef struct { char *label; char *insert; CKind kind; int rank; } CItem;
typedef struct { CItem *v; int n, cap; int sel, scroll; bool open; Pos start; } Comp;
void std_api_load(const char *kc_root);          /* kc_root may be NULL */
int  std_api_member_count(void);
void comp_build(Comp *c, Doc *d, Doc **all, int nall, bool manual);
void comp_close(Comp *c);
const char *comp_kind_tag(CKind k);
const char *snippet_lookup(const char *w, int n);   /* snippet body for an exact trigger word, or NULL */
#endif
