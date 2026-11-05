/* harness_exe_win.c -- run a kcc-built Windows .exe on Linux, without wine.
 *
 * It is a miniature PE loader: maps the image at a base DIFFERENT from the preferred one (so the .reloc table is
 * really exercised), resolves the import table to msvcrt-compatible shims (compiled with the Win64 calling
 * convention, __attribute__((ms_abi))), and jumps to the file's real entry point exactly as the Windows loader
 * would (return-address slot on the stack, rsp = 8 mod 16). The program's exit code is this process's exit code.
 *
 *   gcc -O1 harness_exe_win.c -o harness_exe_win && ./harness_exe_win prog.exe [args...]
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MS __attribute__((ms_abi))

static int g_argc;
static char **g_argv;

static MS int sh_puts(const char *s) { return puts(s); }
static MS size_t sh_strlen(const char *s) { return strlen(s); }
static MS void sh_exit(int code) { fflush(stdout); exit(code); }
static MS int sh_getmainargs(int *argc, char ***argv, char ***envp, int wild, void *si) {
    (void)wild; (void)si;
    static char *empty[] = { NULL };
    *argc = g_argc; *argv = g_argv; *envp = empty;
    return 0;
}
/* just enough printf for the tests: %d %i %u %ld %lld %x %s %c %% */
static MS int sh_printf(const char *fmt, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putchar(*fmt); n++; continue; }
        fmt++;
        int longs = 0;
        while (*fmt == 'l') { longs++; fmt++; }
        char buf[32];
        switch (*fmt) {
        case 'd': case 'i': n += printf("%lld", longs ? __builtin_va_arg(ap, long long) : (long long)__builtin_va_arg(ap, int)); break;
        case 'u': n += printf("%llu", longs ? __builtin_va_arg(ap, unsigned long long) : (unsigned long long)__builtin_va_arg(ap, unsigned)); break;
        case 'x': n += printf("%llx", longs ? __builtin_va_arg(ap, unsigned long long) : (unsigned long long)__builtin_va_arg(ap, unsigned)); break;
        case 's': n += printf("%s", __builtin_va_arg(ap, char *)); break;
        case 'c': putchar(__builtin_va_arg(ap, int)); n++; break;
        case '%': putchar('%'); n++; break;
        default: snprintf(buf, sizeof buf, "<%%%c?>", *fmt); fputs(buf, stdout); n += (int)strlen(buf);
        }
    }
    __builtin_ms_va_end(ap);
    return n;
}

static void *shim_for(const char *name) {
    static const struct { const char *n; void *f; } tab[] = {
        { "puts", (void *)sh_puts }, { "printf", (void *)sh_printf }, { "exit", (void *)sh_exit },
        { "strlen", (void *)sh_strlen }, { "__getmainargs", (void *)sh_getmainargs },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].n, name)) return tab[i].f;
    return NULL;
}

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "harness: %s\n", msg); return 99; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s prog.exe [args]\n", argv[0]); return 2; }
    g_argc = argc - 1; g_argv = argv + 1;
    FILE *f = fopen(argv[1], "rb");
    CHECK(f, "cannot open the .exe");
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(fsz);
    CHECK(fread(file, 1, fsz, f) == (size_t)fsz, "short read");
    fclose(f);

    CHECK(file[0] == 'M' && file[1] == 'Z', "no MZ");
    const uint8_t *pe = file + u32(file + 0x3C);
    CHECK(!memcmp(pe, "PE\0\0", 4), "no PE signature");
    CHECK(u16(pe + 4) == 0x8664, "not x86-64");
    int nsec = u16(pe + 6);
    uint16_t chars = u16(pe + 22);
    CHECK((chars & 0x0002) && !(chars & 0x2000), "not an executable image (DLL flag set or EXECUTABLE_IMAGE missing)");
    const uint8_t *opt = pe + 24;
    CHECK(u16(opt) == 0x20B, "not PE32+");
    uint32_t entry = u32(opt + 16);
    uint64_t pref = u64(opt + 24);
    uint32_t image_size = u32(opt + 56), hdr_size = u32(opt + 60);
    const uint8_t *dd = opt + 112;                 /* data directories */
    const uint8_t *sec = opt + u16(pe + 20);

    uint64_t want = pref + 0x10000000;             /* deliberately not the preferred base */
    uint8_t *img = mmap((void *)want, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(img != MAP_FAILED, "cannot map the image");
    memcpy(img, file, hdr_size);
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i;
        uint32_t rva = u32(s + 12), rawsz = u32(s + 16), raw = u32(s + 20);
        if (rawsz) memcpy(img + rva, file + raw, rawsz);
    }
    int64_t delta = (int64_t)((uint64_t)img - pref);

    /* base relocations */
    uint32_t rrva = u32(dd + 5 * 8), rsz = u32(dd + 5 * 8 + 4);
    int nrel = 0;
    for (uint32_t o = 0; o < rsz;) {
        uint32_t page = u32(img + rrva + o), blk = u32(img + rrva + o + 4);
        CHECK(blk >= 8, "bad reloc block");
        for (uint32_t e = 8; e < blk; e += 2) {
            uint16_t ent = u16(img + rrva + o + e);
            if ((ent >> 12) == 10) { uint64_t *p = (uint64_t *)(img + page + (ent & 0xFFF)); *p += delta; nrel++; }
            else CHECK((ent >> 12) == 0, "unsupported relocation type");
        }
        o += blk;
    }

    /* imports */
    uint32_t irva = u32(dd + 1 * 8);
    for (const uint8_t *d = img + irva; u32(d + 12); d += 20) {
        const char *dll = (const char *)img + u32(d + 12);
        uint64_t *ilt = (uint64_t *)(img + (u32(d) ? u32(d) : u32(d + 16)));
        uint64_t *iat = (uint64_t *)(img + u32(d + 16));
        for (; *ilt; ilt++, iat++) {
            CHECK(!(*ilt >> 63), "import by ordinal not supported by the harness");
            const char *name = (const char *)img + (*ilt & 0x7FFFFFFF) + 2;
            void *fn = shim_for(name);
            if (!fn) { fprintf(stderr, "harness: no shim for %s!%s\n", dll, name); return 99; }
            *iat = (uint64_t)fn;
        }
    }
    fprintf(stderr, "harness: image at %p (preferred %#llx), %d base relocations applied\n", (void *)img, (unsigned long long)pref, nrel);

    ((MS void (*)(void))(img + entry))();          /* never returns: the stub ends in exit() */
    fprintf(stderr, "harness: entry point returned\n");
    return 98;
}
