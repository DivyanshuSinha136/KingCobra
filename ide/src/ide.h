/* kcide — global IDE state shared by editor.c, ui.c, cmds.c and main.c. */
#ifndef KCIDE_IDE_H
#define KCIDE_IDE_H
#include "complete.h"
#include "output.h"
#include "proc.h"
#include "vt.h"

#define KCIDE_VERSION "1.0.0"
typedef struct { int x, y, w, h; } Rect;
static inline bool in_rect(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
enum { FOCUS_EDITOR, FOCUS_TREE, FOCUS_PANEL };

typedef struct {
    int tab_width, theme; bool use_spaces, check_on_save, save_before_run, auto_complete, trim_ws, show_ws, show_sidebar, line_numbers, auto_pairs;
    char kc_path[512];
} Config;

typedef struct TNode { char *name, *path; bool dir, open, loaded; struct TNode **kids; int nkids; struct TNode *parent; int depth; } TNode;

typedef struct { bool open; char find[256], repl[256]; int field; bool case_sens, whole; bool replace_mode; int count, index; } FindState;

typedef struct { char *label, *detail; intptr_t id; int score; } PItem;
enum { OV_NONE, OV_PROMPT, OV_PICKER, OV_CONFIRM, OV_MENU };
typedef struct Overlay {
    int kind; char title[160];
    char input[512]; int ilen, icur;
    PItem *items; int nitems; int *view, nview, sel, scroll;
    void (*on_accept)(const char *text, intptr_t id, void *ctx); void (*on_cancel)(void *ctx); void *ctx;
    char msg[512]; int btn; const char *btns[3]; int nbtns;
    int menu, mitem; bool path_mode; bool live_filter;
} Overlay;

/* ---- bottom panel: Output | Terminal | Console | Debug ---- */
enum { PT_OUTPUT, PT_TERMINAL, PT_CONSOLE, PT_DEBUG, PT_COUNT };
#define MAX_TERMS 4
typedef struct Term { VTerm *vt; Proc proc; char name[32]; bool exited; int code; bool sel; int sx, sy, ex, ey; } Term;
typedef struct { Output log; Proc proc; bool started; char input[512]; int ilen, icur; char **hist; int nhist, hpos; } Console;
typedef struct { char *name, *file; int line; } DFrame;
typedef struct { char scope[8]; char *name, *type, *value; } DVar;
enum { DS_IDLE, DS_STARTING, DS_RUNNING, DS_PAUSED };
typedef struct {
    int state; char dir[300], base[340];
    int evt_fd, cmd_fd; void *h_evt, *h_cmd;               /* POSIX FIFOs / Windows named pipes */
    char lb[16384]; int nlb; int block;                    /* partial line buffer; 0 none, 1 stack, 2 vars */
    char reason[24], msg[512]; char *file; int line, depth;
    DFrame *frames; int nframes, fsel;
    DVar *vars; int nvars;
    char **watch, **wval; int nwatch;
    int pane, sel[3], scroll[3];
    int pend[64], npend;                                   /* tags of outstanding eval requests: >=0 watch idx, -1 console */
    char *target;                                          /* file being debugged */
    bool stop_on_entry;
} Dbg;

typedef struct { char *path; int line, col; } NavEntry;
typedef struct { char *name; SymKind kind; char *file; int line; } ProjSym;

typedef struct {
    Theme *th; Config cfg;
    Doc **docs; int ndocs, active, tab_scroll;
    char *root, *exe_dir; char kc_exe[512]; bool kc_found;
    bool show_panel; int panel_h, sb_w; int focus;
    Rect r_menu, r_tabs, r_tree, r_edit, r_panel, r_status, r_find; int gutter_w; int text_x, text_w, text_h;
    TNode *tree; TNode **flat; int nflat, tree_sel, tree_scroll;
    Output out; Proc proc; char proc_label[256]; uint64_t proc_start_ms; bool proc_is_check; int proc_diag_base;
    char proc_input[256]; int proc_ilen;
    Overlay ov; FindState fs; Comp comp;
    char msg[300]; int msg_kind; uint64_t msg_until;
    char *clip; size_t cliplen;
    NavEntry nav[64]; int nnav, navpos;
    ProjSym *syms; int nsyms; char **files; int nfiles; bool indexed;
    int cur_diag; bool quit, need_redraw;
    int mouse_down; Pos mouse_anchor; uint64_t last_click; int click_count; int last_cx, last_cy;
    int menu_open; /* -1 none */
    bool drag_scroll;
    int ptab; Term terms[MAX_TERMS]; int nterms, cterm; Console con; Dbg dbg; bool proc_is_debug; bool panel_auto_focus;
} Ide;
extern Ide G;
#define TH (*G.th)

/* config / lifecycle (main.c) */
void cfg_load(void); void cfg_save(void);
void status(int kind, const char *fmt, ...);        /* kind: 0 info, 1 ok, 2 warn, 3 error */

/* editor.c */
Doc *cur_doc(void);
void ed_type(Doc *d, const char *utf8, int n);
void ed_key(Doc *d, Event *e);                       /* returns after handling an editing key */
bool ed_try_key(Doc *d, Event *e);                   /* true when consumed */
void ed_draw(Doc *d);
void ed_mouse(Doc *d, Event *e);
void ed_set_cursor(Doc *d, Pos p, bool extend);
void ed_reveal(Doc *d);
void ed_delete_selection(Doc *d);
char *ed_selected_text(Doc *d, int *n);
void ed_copy(Doc *d, bool cut);
void ed_paste_text(Doc *d, const char *t, int n);
void ed_paste(Doc *d);
void ed_undo(Doc *d); void ed_redo(Doc *d);
void ed_select_all(Doc *d);
void ed_dup_line(Doc *d); void ed_del_line(Doc *d); void ed_move_lines(Doc *d, int dir);
void ed_toggle_comment(Doc *d); void ed_indent(Doc *d, bool out); void ed_format(Doc *d);
void ed_goto(Doc *d, int line, int col, bool push_nav);
void ed_select_line(Doc *d);
void ed_trigger_complete(bool manual);
void find_open(bool replace); void find_close(void); void find_step(int dir, bool from_cursor);
void find_replace_one(void); void find_replace_all(void); bool find_key(Event *e); void find_recount(void);
int  ed_find_matches_in_line(Doc *d, int line, int *starts, int *lens, int max);
bool diag_for_line(Doc *d, int line, int *sev, const char **msg, int *col);
int  ed_line_diag_cols(Doc *d, int line, int *cols, int *sevs, int max);

/* ui.c */
void ui_layout(void); void ui_draw(void); void ui_event(Event *e);
void tree_build(void); void tree_reveal(const char *path);
void ov_prompt(const char *title, const char *init, bool path_mode, void (*cb)(const char *, intptr_t, void *), void *ctx);
void ov_picker(const char *title, PItem *items, int n, bool live, void (*cb)(const char *, intptr_t, void *), void *ctx);
void ov_confirm(const char *msg, const char *b0, const char *b1, const char *b2, void (*cb)(const char *, intptr_t, void *), void *ctx);
void ov_close(void);
void panel_show(bool show);
void output_body_draw(Rect r); void output_key(Event *e); void output_click(Event *e);

/* panel.c: terminals, console, tab strip */
void panel_init(void); void panel_free(void);
void panel_sync_sizes(void);
void panel_set_tab(int tab, bool focus);
void panel_draw(void); void panel_key(Event *e); void panel_mouse(Event *e); void panel_paste(const char *t, int n);
void panel_poll(void); int panel_fds(int *fds, int max);
bool panel_reserved_key(const Event *e); bool panel_wants_key(const Event *e);
void term_new(void); void term_kill_current(void);
void console_submit(const char *line); void console_clear(void); void console_restart(bool preload_current);
void console_print(const char *text, Color fg, uint8_t attr);
void console_prompt_label(char *out, size_t cap);

/* dbg.c: debugger session + panel */
void dbg_init(void); void dbg_shutdown(void);
bool dbg_active(void);
void dbg_start(bool stop_on_entry); void dbg_stop(void); void dbg_clear_breakpoints(void); void dbg_command(int cmd);
void dbg_toggle_bp(Doc *d, int line);
void dbg_poll(void); int dbg_fd(void);
void dbg_process_exited(int code);
void dbg_panel_draw(Rect r); void dbg_panel_key(Event *e); void dbg_panel_mouse(Event *e, Rect r);
bool dbg_eval_console(const char *expr);
bool dbg_exec_line(Doc *d, int *line);         /* execution marker for the editor */
void dbg_add_watch(const char *expr);
int  dbg_header_buttons(const char **labels, int *cmds, int max);

/* cmds.c: spawn the Output-panel process */
bool run_spawn(const char *label, char *const argv[], const char *const *env, bool is_debug);

/* cmds.c */
enum {
    C_NONE, C_NEW, C_OPEN, C_QUICKOPEN, C_SAVE, C_SAVEAS, C_SAVEALL, C_CLOSE, C_RELOAD, C_QUIT,
    C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_SELALL, C_DUPLINE, C_DELLINE, C_COMMENT, C_INDENT, C_OUTDENT, C_MOVEUP, C_MOVEDOWN, C_FORMAT, C_SELLINE,
    C_FIND, C_REPLACE, C_FINDNEXT, C_FINDPREV, C_GOTOLINE, C_GOTOSYM, C_GOTODEF, C_FINDPROJ, C_NAVBACK, C_NAVFWD, C_COMPLETE,
    C_SIDEBAR, C_PANEL, C_THEME, C_WHITESPACE, C_LINENUMS, C_NEXTTAB, C_PREVTAB, C_PALETTE, C_FOCUS_TREE, C_FOCUS_EDITOR, C_FOCUS_PANEL, C_NEWFILE, C_NEWDIR, C_RENAME, C_DELETE, C_REFRESH,
    C_RUN, C_RUNTERM, C_CHECK, C_TEST, C_TESTALL, C_LEAKS, C_REPL, C_STOP, C_NEXTDIAG, C_PREVDIAG, C_CLEAROUT, C_AUTOPAIRS, C_CHECKSAVE,
    C_TERMINAL, C_NEWTERM, C_KILLTERM, C_CONSOLE, C_CONSOLE_CLEAR, C_CONSOLE_RESTART, C_CONSOLE_LOAD, C_DEBUGPANEL,
    C_DEBUG, C_DBG_CONT, C_DBG_OVER, C_DBG_IN, C_DBG_OUT, C_DBG_PAUSE, C_DBG_STOP, C_TOGGLE_BP, C_CLEAR_BPS, C_ADD_WATCH, C_MENU,
    C_KEYS, C_ABOUT, C_COUNT
};
typedef struct { int id; const char *label; const char *keys; } CmdInfo;
extern const CmdInfo CMDS[];
void cmd_run(int id);
void doc_activate(int idx);
Doc *doc_open_path(const char *path, bool activate);
int  doc_find_open(const char *path);
void doc_close_idx(int idx, bool force);
bool docs_any_dirty(void);
void run_start(int kind);                              /* C_RUN / C_CHECK / C_TEST / ... */
void run_poll(void);
void diag_goto(int dir);
void index_project(bool force);
void nav_push(void);
void nav_go(int dir);
void focus_diag_index(int i);
void on_saved(Doc *d);
const char *theme_name(int i);
#endif
