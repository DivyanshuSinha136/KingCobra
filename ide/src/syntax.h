/* kcide — KingCobra syntax highlighter (multi-line aware), symbol scanner and colour themes. */
#ifndef KCIDE_SYNTAX_H
#define KCIDE_SYNTAX_H
#include "term.h"

typedef enum {
    HC_TEXT, HC_COMMENT, HC_DOC, HC_TODO, HC_STRING, HC_ESCAPE, HC_CHAR, HC_INTERP, HC_NUMBER,
    HC_KEYWORD, HC_DECL, HC_TYPE, HC_TYPEDEF, HC_BUILTIN, HC_FUNC, HC_FUNCDEF, HC_CONST,
    HC_OPERATOR, HC_PUNCT, HC_MODULE, HC_FIELD, HC_LABEL, HC_SELF,
    HC_NOTE, HC_DURATION, HC_CHORD, HC_TAG, HC_ATTR, HC_ENTITY, HC_COUNT
} HlClass;

/* Line state carried between lines: low 3 bits = mode, bits 3.. = f-string brace depth. */
enum { HS_NORMAL = 0, HS_BLOCK = 1, HS_STRING = 2, HS_FSTR = 3, HS_FEXPR = 4 };

/* Languages the editor can highlight. KCM is two formats sharing one extension: music scores and "kcmodel" 3D models. */
typedef enum { LANG_PLAIN, LANG_KC, LANG_KCM_MUSIC, LANG_KCM_MODEL, LANG_SSM } Lang;
Lang lang_from_path(const char *path);
Lang lang_sniff_kcm(const char *text, size_t n);       /* LANG_KCM_MODEL if the first real line is `kcmodel`, else LANG_KCM_MUSIC */
const char *lang_name(Lang l);
const char *lang_comment(Lang l);                      /* line-comment prefix ("// ", "# ") or NULL (SSM wraps in <!-- -->) */
uint8_t hl_line_lang(Lang l, const char *s, int len, uint8_t state_in, uint8_t *out);
/* completion vocabulary for the data languages: returns NULL-terminated word list, `tags` get a snippet body */
const char *const *lang_words(Lang l);

/* Tokenises one line. `out` receives one HlClass per byte. Returns the state at end of line. */
uint8_t hl_line(const char *s, int len, uint8_t state_in, uint8_t *out);
bool hl_is_code(uint8_t cls);                       /* not comment / string / char / escape */

typedef struct {
    const char *name;
    Color bg, fg, sel_bg, line_bg, gutter_fg, gutter_bg, gutter_cur, ui_bg, ui_fg, ui_dim, accent,
          tab_on_bg, tab_off_bg, status_bg, status_fg, border, menu_bg, menu_sel, popup_bg, popup_sel,
          error, warn, info, ok, match_bg, find_bg, find_cur_bg, bracket_bg, guide, occurrence_bg;
    Color hl[HC_COUNT];
    uint8_t hl_attr[HC_COUNT];
} Theme;
extern const Theme THEMES[];
extern const int N_THEMES;

typedef enum { SYM_FN, SYM_STRUCT, SYM_ENUM, SYM_IMPL, SYM_TYPE, SYM_CONST, SYM_VAR } SymKind;
typedef struct { char name[128]; SymKind kind; int col; int indent; } SymHit;
/* Does this line declare something? (leading-token based; ignores comment-only lines) */
bool sym_scan_line(const char *s, int len, SymHit *out);
const char *sym_kind_name(SymKind k);

/* word lists for completion / highlighting */
extern const char *const KC_KEYWORDS[]; extern const int KC_N_KEYWORDS;
extern const char *const KC_TYPES[];    extern const int KC_N_TYPES;
extern const char *const KC_BUILTINS[]; extern const int KC_N_BUILTINS;
extern const char *const KC_METHODS[];  extern const int KC_N_METHODS;
extern const char *const KC_STD_MODULES[]; extern const int KC_N_STD_MODULES;
bool kc_is_std_module(const char *s, int n);
#endif
