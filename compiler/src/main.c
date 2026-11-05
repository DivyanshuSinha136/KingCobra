/* KingCobra compiler/runtime driver — `kingcobra` (alias: `kc`)
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 *
 * The command line lives in cli.c (commands, options, help, completions) and cli_tools.c (check, test). */
#include "cli.h"

int main(int argc, char **argv) { return kc_cli_main(argc, argv); }
