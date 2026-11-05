/* KingCobra std.http — module table. See http_glue.h for the file layout and
 * docs/STD_HTTP.md for the API. */
#include "http_glue.h"

#define HT_MAX_ENTRIES 96
static KcModuleEntry g_entries[HT_MAX_ENTRIES];

static int add_table(int at, const HtEntry *t, int count) {
    for (int i = 0; i < count; i++) {
        if (at >= HT_MAX_ENTRIES) { fprintf(stderr, "kingcobra: std.http entry table overflow\n"); exit(70); }
        g_entries[at].name = (char *)t[i].name; g_entries[at].fn = t[i].fn; at++;
    }
    return at;
}

KcModule *kc_mod_http(void) {
    static KcModule m;
    static bool built = false;
    if (!built) {
        int at = 0;
        at = add_table(at, ht_client_entries, ht_client_count);
        at = add_table(at, ht_url_entries, ht_url_count);
        at = add_table(at, ht_inflate_entries, ht_inflate_count);
        at = add_table(at, ht_server_entries, ht_server_count);
        m.name = "http"; m.entries = g_entries; m.count = at;
        built = true;
    }
    return &m;
}
