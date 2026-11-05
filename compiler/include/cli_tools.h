#ifndef KC_CLI_TOOLS_H
#define KC_CLI_TOOLS_H
#include <stdbool.h>
typedef struct { bool quiet, json, typecheck; } KcCheckOpts;
typedef struct { const char *filter; bool list, fail_fast, quiet, typecheck; } KcTestOpts;
int kc_cmd_check(int nfiles, char **files, const KcCheckOpts *o);
int kc_cmd_test(int nfiles, char **files, const KcTestOpts *o);
#endif
