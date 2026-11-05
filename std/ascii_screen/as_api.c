/* std.ascii_screen — module assembly.
 *
 * `import std.ascii_screen;` gives a native terminal (TUI) screen and ASCII-art engine: see docs/STD_ASCII_SCREEN.md.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "as_api.h"

KcModule *kc_mod_ascii_screen(void) {
    static KcModuleEntry *ENT = NULL; static KcModule m;
    if (!ENT) {
        int total = AS_N_CORE + AS_N_DRAW + AS_N_MEDIA + AS_N_GAME; ENT = kc_malloc((size_t)total * sizeof(KcModuleEntry)); int k = 0;
        for (int i = 0; i < AS_N_CORE; i++) ENT[k++] = AS_T_CORE[i];
        for (int i = 0; i < AS_N_DRAW; i++) ENT[k++] = AS_T_DRAW[i];
        for (int i = 0; i < AS_N_MEDIA; i++) ENT[k++] = AS_T_MEDIA[i];
        for (int i = 0; i < AS_N_GAME; i++) ENT[k++] = AS_T_GAME[i];
        m.name = "ascii_screen"; m.entries = ENT; m.count = total;
        kc_error_hook = term_restore_for_error;
    }
    return &m;
}
