#ifndef KC_PARSER_H
#define KC_PARSER_H
#include "lexer.h"
#include "ast.h"

#include "preproc.h"
KcNode *kc_parse_program(const char *src, const char *filename);
void kc_parse_keep_aliases(bool on);    /* REPL: let type/typedef aliases persist between parses */
void kc_parse_reset_aliases(void);
KcNode *kc_parse_program_pp(const char *src, const char *filename, KcPP *pp);   /* with a persistent macro table (REPL) */

#endif
