/* std.ascii_screen — shared helpers for the KingCobra binding files (as_api*.c). */
#ifndef KC_AS_API_H
#define KC_AS_API_H
#include "as_internal.h"
#define NAT(nm) static KcValue nm(KcInterp *ip, KcValue *a, int n, KcPos pos)
#define BEGIN(lo, hi, sig) do { AC.ip = ip; AC.pos = pos; A_argc(n, lo, hi, sig); } while (0)
#define OKV kc_null()
#define E(nm, fn) { (char *)nm, fn }
typedef struct { Col fg, bg; uint16_t attr; } Sty;
Sty  sty_from(const Opts *o);                       /* current style overridden by options (fg, bg, bold, ...) */
Image *img_arg(KcValue *a, int n, int i, int *temp_id, const char *who);   /* image id or path (temporary) */
KcValue col_ret(Col c);
extern bool AS_game_auto;
bool as_do_poll(void);
/* binding tables from each file */
extern KcModuleEntry AS_T_CORE[], AS_T_DRAW[], AS_T_MEDIA[], AS_T_GAME[];
extern const int AS_N_CORE, AS_N_DRAW, AS_N_MEDIA, AS_N_GAME;
#endif
