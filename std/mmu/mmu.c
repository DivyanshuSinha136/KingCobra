/* KingCobra std.mmu — module table. See docs/STD_MMU.md.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mmu_internal.h"

KcModule *kc_mod_mmu(void) {
    static KcModule m; static bool ready = false;
    if (!ready) {
        int total = mmu_api_count + mmu_api_vm_count + mmu_host_count;
        KcModuleEntry *e = kc_malloc(sizeof(KcModuleEntry) * (size_t)total); int k = 0;
        for (int i = 0; i < mmu_api_count; i++)    { e[k].name = (char *)mmu_api_entries[i].name;    e[k].fn = mmu_api_entries[i].fn;    k++; }
        for (int i = 0; i < mmu_api_vm_count; i++) { e[k].name = (char *)mmu_api_vm_entries[i].name; e[k].fn = mmu_api_vm_entries[i].fn; k++; }
        for (int i = 0; i < mmu_host_count; i++)   { e[k].name = (char *)mmu_host_entries[i].name;   e[k].fn = mmu_host_entries[i].fn;   k++; }
        m.name = "mmu"; m.entries = e; m.count = total; ready = true;
    }
    return &m;
}
