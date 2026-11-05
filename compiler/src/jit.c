/* KingCobra closure JIT — hand-written x86-64 codegen.
 *
 * v0.3 scope (extended from v0.1's single-expression shape; see
 * jit.h and docs/ROADMAP.md for the exact boundary and why floats and
 * calls to functions OTHER than the one being compiled are still out
 * of scope). This file compiles a restricted but now much more useful
 * subset of KingCobra: multiple statements, local variables (`let`/
 * `var`), `if`/`else`, `while` with `break`/`continue`, comparisons,
 * logical `&&`/`||` (short-circuiting, matching the interpreter), and
 * self-recursive calls — covering real hot functions like `fib` and
 * iterative accumulators, not just one-line arithmetic helpers.
 *
 * Every correctness-sensitive design choice is commented at the point
 * it matters, because this is the one file in the project where a bug
 * produces a silently wrong NUMBER rather than a crash or a clear
 * error. When in doubt, this file bails out (returns without setting
 * cl->jit_code) rather than guesses — the interpreter is always a
 * correct fallback, so "didn't JIT this" is always a safe outcome;
 * "JIT'd it wrong" is not.
 */
#include "jit.h"
#include "ast.h"
#include "kc_platform.h"
#include <stdlib.h>
#include <string.h>
#if !defined(KC_PLATFORM_WINDOWS)
#include <signal.h>
#include <setjmp.h>
#endif

/* ---------------------------- runtime capability probe ----------------------------
 * "The CPU is x86-64" is necessary for the JIT but not sufficient: hardened
 * kernels / SELinux (execmem), PaX MPROTECT, locked-down containers, sandboxes
 * and some Windows policies (ACG) refuse to hand out executable pages, and a
 * user may simply not want machine code generated (--no-jit, KC_JIT=0).
 * Rather than finding that out in the middle of a hot function, the first
 * attempt to JIT anything runs ONE tiny probe:
 *
 *   1. host architecture is one this JIT emits code for        (compile time)
 *   2. the user has not switched it off                        (--no-jit / KC_JIT=0)
 *   3. the OS gives us a writable page                         (kc_exec_alloc)
 *   4. the OS lets us flip it to read+exec                     (kc_exec_seal)
 *   5. a 11-byte stub `mov rax, MAGIC; ret` actually runs and returns MAGIC
 *      (POSIX: under a SIGSEGV/SIGILL/SIGBUS guard, so an OS that "allows"
 *      the mprotect but still faults on execution degrades to the
 *      interpreter instead of killing the program)
 *
 * The verdict is cached for the life of the process. If any step fails the
 * JIT is simply off: every closure keeps running in the tree-walking
 * interpreter, which is the reference semantics anyway, so programs behave
 * identically, only without the native speedup. */

#define KC_JIT_PROBE_MAGIC 0x4B43A11CE5C0DE42LL

enum { PROBE_UNRUN = 0, PROBE_OK, PROBE_FAIL };
static int         g_probe_state  = PROBE_UNRUN;
static const char *g_probe_reason = "not probed yet";
static int         g_user_override = -1;   /* -1 = no opinion, 0 = forced off, 1 = forced on (still probed) */

void kc_jit_set_enabled(bool on) {
    g_user_override = on ? 1 : 0;
    if (!on) { g_probe_state = PROBE_FAIL; g_probe_reason = "disabled by user (--no-jit / KC_JIT=0)"; }
    else if (g_probe_state == PROBE_FAIL && g_probe_reason[0] == 'd') g_probe_state = PROBE_UNRUN;
}

#if defined(KC_NATIVE_CODEGEN_SUPPORTED) && !defined(KC_PLATFORM_WINDOWS)
static sigjmp_buf g_probe_jmp;
static volatile sig_atomic_t g_probe_armed = 0;
static void probe_trap(int sig) { (void)sig; if (g_probe_armed) siglongjmp(g_probe_jmp, 1); _exit(139); }
#endif

#if defined(KC_NATIVE_CODEGEN_SUPPORTED)
/* Runs the stub; returns true only if it came back with the magic value. */
static bool probe_run_stub(void *mem) {
    typedef int64_t (*StubFn)(void);
#if !defined(KC_PLATFORM_WINDOWS)
    struct sigaction sa, old_segv, old_ill, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_trap;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGBUS,  &sa, &old_bus);
    bool ok = false;
    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        g_probe_armed = 1;
        StubFn f = (StubFn)mem;
        ok = f() == KC_JIT_PROBE_MAGIC;
    }
    g_probe_armed = 0;
    sigaction(SIGSEGV, &old_segv, NULL);   /* put the stack-overflow guard etc. back exactly as it was */
    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGBUS,  &old_bus,  NULL);
    return ok;
#else
    /* Windows: allocation + VirtualProtect above are the real gate (a
     * policy that blocks dynamic code fails there, with an error code,
     * not at execution time). */
    StubFn f = (StubFn)mem;
    return f() == KC_JIT_PROBE_MAGIC;
#endif
}
#endif

static void probe_fail(const char *why) { g_probe_state = PROBE_FAIL; g_probe_reason = why; }

static void run_probe(void) {
    g_probe_state = PROBE_OK; g_probe_reason = "available";
    if (g_user_override == 0) { probe_fail("disabled by user (--no-jit / KC_JIT=0)"); return; }
    if (g_user_override < 0) {
        const char *env = getenv("KC_JIT");
        if (env && (!strcmp(env, "0") || !strcmp(env, "off") || !strcmp(env, "no") || !strcmp(env, "false"))) {
            probe_fail("disabled by user (--no-jit / KC_JIT=0)"); return;
        }
    }
#if !defined(KC_NATIVE_CODEGEN_SUPPORTED)
    probe_fail("no JIT for this CPU (" KC_ARCH_NAME "): running in the interpreter, which gives identical results");
#else
    const size_t sz = 64;
    void *mem = kc_exec_alloc(sz);
    if (!mem) { probe_fail("the OS refused to allocate code memory (interpreter only)"); return; }
    uint8_t stub[11] = { 0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xC3 };      /* mov rax, imm64 ; ret */
    int64_t magic = KC_JIT_PROBE_MAGIC;
    memcpy(stub + 2, &magic, 8);
    memcpy(mem, stub, sizeof stub);
    if (kc_exec_seal(mem, sz) != 0) {
        kc_exec_free(mem, sz);
        probe_fail("the OS does not allow executable memory here (W^X / SELinux / sandbox policy)");
        return;
    }
    bool ok = probe_run_stub(mem);
    kc_exec_free(mem, sz);
    if (!ok) probe_fail("generated code could not be executed on this system (self-test failed)");
#endif
}

bool kc_jit_available(void) {
    if (g_probe_state == PROBE_UNRUN) run_probe();
    return g_probe_state == PROBE_OK;
}

const char *kc_jit_status(void) {
    kc_jit_available();
    return g_probe_reason;
}

/* Generous fixed caps, checked explicitly — exceeding any of them is
 * just a reason to bail (fall back to the interpreter), never UB. */
#define MAX_CODE_BYTES   8192   /* compiled instruction bytes          */
#define MAX_SLOTS        48     /* params + locals + scratch, 8 bytes each */
#define MAX_LOCALS       24
#define MAX_RETURN_FIXUPS 32
#define MAX_LOOP_DEPTH    8
#define MAX_BREAKS_PER_LOOP 16

typedef struct {
    size_t break_fixups[MAX_BREAKS_PER_LOOP];
    int break_count;
    size_t loop_start_offset; /* backward jumps (continue, loop-back) target this directly */
} LoopCtx;

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool ok;

    char *param_names[4];
    int nparams;
    char *local_names[MAX_LOCALS];
    int n_locals;

    int scratch_top;  /* current live scratch slots (stack discipline) */
    int scratch_max;  /* high-water mark — determines frame size       */

    size_t sub_rsp_patch_offset; /* where to patch the prologue's frame size */

    char *self_name;  /* function's own name, for self-call detection; NULL if anonymous */
    void *self_addr;  /* this function's own (already-allocated) exec memory address */

    size_t return_fixups[MAX_RETURN_FIXUPS];
    int return_fixup_count;

    LoopCtx loops[MAX_LOOP_DEPTH];
    int loop_depth;
} Emitter;

/* ---------------------------- low-level byte emission ---------------------------- */

static void emit(Emitter *e, const uint8_t *bytes, size_t n) {
    if (!e->ok) return;
    if (e->len + n > e->cap) { e->ok = false; return; }
    memcpy(e->buf + e->len, bytes, n);
    e->len += n;
}
static void emit1(Emitter *e, uint8_t b) { emit(e, &b, 1); }

/* Emits a placeholder 4-byte (rel32) field and returns the buffer
 * offset of that field, for later patching once the jump target is
 * known. Used for every FORWARD jump (if/else skip, loop exit, break).
 * Backward jumps (loop-back, continue) never need this — the target
 * offset is already known when we emit them. */
static size_t emit_rel32_placeholder(Emitter *e) {
    size_t off = e->len;
    uint8_t zero[4] = {0, 0, 0, 0};
    emit(e, zero, 4);
    return off;
}
/* Patches a previously-emitted rel32 field (at byte offset `field_off`,
 * i.e. the 4 bytes immediately after some jump opcode) so the jump
 * lands at `target_off`. rel32 is relative to the address right AFTER
 * the 4-byte field itself. */
static void patch_rel32(Emitter *e, size_t field_off, size_t target_off) {
    if (!e->ok) return;
    int32_t rel = (int32_t)((int64_t)target_off - (int64_t)(field_off + 4));
    memcpy(e->buf + field_off, &rel, 4);
}
/* For backward jumps we already know both ends, so just compute and
 * emit the final rel32 directly — no patch list entry needed. */
static void emit_jmp_back(Emitter *e, size_t target_off) {
    emit1(e, 0xE9);
    size_t field_off = e->len;
    uint8_t zero[4] = {0,0,0,0};
    emit(e, zero, 4);
    patch_rel32(e, field_off, target_off);
}
static size_t emit_jmp_fwd(Emitter *e) {      /* returns patch offset */
    emit1(e, 0xE9);
    return emit_rel32_placeholder(e);
}
static size_t emit_je_fwd(Emitter *e) {       /* jump if rax==0 (after a cmp rax,0) */
    emit1(e, 0x0F); emit1(e, 0x84);
    return emit_rel32_placeholder(e);
}

/* ---------------------------- slot-addressed load/store ---------------------------- */
/* Every local, param, and scratch value lives in its OWN fixed stack
 * slot at [rbp - 8*(slot+1)] for the whole function body. Nothing is
 * ever pushed/popped onto the live stack mid-expression (the v0.1 JIT
 * did that for binop operands) — that mattered only for a leaf
 * function, but once self-recursive CALLS are possible mid-expression,
 * an unbalanced push sitting under a nested `call` would misalign rsp
 * for that call (x86-64 requires rsp%16==0 immediately before `call`,
 * on both Linux and Windows). Fixed slots, set up once in the prologue
 * and never touched by push/pop, sidestep that hazard entirely. */

static void emit_mov_rax_imm64(Emitter *e, int64_t v) {
    uint8_t code[10] = { 0x48, 0xB8 };
    memcpy(code + 2, &v, 8);
    emit(e, code, 10);
}
static int8_t slot_disp(int slot) { return (int8_t)(-8 * (slot + 1)); }

static void emit_store_slot_from_rax(Emitter *e, int slot) { /* mov [rbp-disp], rax */
    uint8_t c[4] = { 0x48, 0x89, 0x45, (uint8_t)slot_disp(slot) };
    emit(e, c, 4);
}
static void emit_load_rax_from_slot(Emitter *e, int slot) { /* mov rax, [rbp-disp] */
    uint8_t c[4] = { 0x48, 0x8B, 0x45, (uint8_t)slot_disp(slot) };
    emit(e, c, 4);
}
static void emit_load_rbx_from_slot(Emitter *e, int slot) { /* mov rbx, [rbp-disp] */
    uint8_t c[4] = { 0x48, 0x8B, 0x5D, (uint8_t)slot_disp(slot) };
    emit(e, c, 4);
}

/* ---------------------------- arithmetic / comparison ---------------------------- */

static void emit_add_rax_rbx(Emitter *e) { uint8_t c[3] = {0x48,0x01,0xD8}; emit(e,c,3); }
static void emit_mul_rax_rbx(Emitter *e) { uint8_t c[4] = {0x48,0x0F,0xAF,0xC3}; emit(e,c,4); }
static void emit_sub_rbx_rax(Emitter *e) { uint8_t c[3] = {0x48,0x29,0xC3}; emit(e,c,3); } /* rbx -= rax */
static void emit_mov_rax_rbx(Emitter *e) { uint8_t c[3] = {0x48,0x89,0xD8}; emit(e,c,3); }
static void emit_neg_rax(Emitter *e)     { uint8_t c[3] = {0x48,0xF7,0xD8}; emit(e,c,3); }
static void emit_cmp_rax_0(Emitter *e)   { uint8_t c[4] = {0x48,0x83,0xF8,0x00}; emit(e,c,4); }
static void emit_cmp_rbx_rax(Emitter *e) { uint8_t c[3] = {0x48,0x39,0xC3}; emit(e,c,3); } /* flags = rbx-rax */
static void emit_mov_rax_0(Emitter *e)   { emit_mov_rax_imm64(e, 0); }
static void emit_mov_rax_1(Emitter *e)   { emit_mov_rax_imm64(e, 1); }

/* setCC al; movzx rax,al — normalizes a flag result to 0/1 in rax. */
static void emit_setcc_rax(Emitter *e, uint8_t setcc_opcode2) {
    uint8_t s[3] = { 0x0F, setcc_opcode2, 0xC0 }; emit(e, s, 3);
    uint8_t z[4] = { 0x48, 0x0F, 0xB6, 0xC0 }; emit(e, z, 4);
}

/* ---------------------------- local/scratch slot bookkeeping ---------------------------- */

static int find_slot(Emitter *e, const char *name) {
    for (int i = 0; i < e->nparams; i++) if (strcmp(e->param_names[i], name) == 0) return i;
    for (int i = 0; i < e->n_locals; i++) if (strcmp(e->local_names[i], name) == 0) return e->nparams + i;
    return -1;
}
static int declare_local(Emitter *e, char *name) {
    if (e->n_locals >= MAX_LOCALS) { e->ok = false; return -1; }
    e->local_names[e->n_locals] = name;
    return e->nparams + (e->n_locals++);
}
static int alloc_scratch(Emitter *e) {
    int slot = e->nparams + e->n_locals + e->scratch_top;
    if (slot >= MAX_SLOTS) { e->ok = false; return -1; }
    e->scratch_top++;
    if (e->scratch_top > e->scratch_max) e->scratch_max = e->scratch_top;
    return slot;
}
static void free_scratch(Emitter *e) { e->scratch_top--; }

/* ---------------------------- self-call argument registers ---------------------------- */
/* Same per-register {REX, modrm} pairing as the incoming-parameter
 * table below, just with the opcode byte swapped for "load into
 * register" (0x8B) instead of "store from register" (0x89) — issuing
 * a self-call means writing OUR computed arguments INTO these
 * registers, the mirror image of reading them out at entry. */
#if defined(KC_PLATFORM_WINDOWS)
static const uint8_t ARG_LOAD_TEMPLATES[4][4] = {
    { 0x48, 0x8B, 0x4D, 0 }, /* mov rcx, [rbp+disp] */
    { 0x48, 0x8B, 0x55, 0 }, /* mov rdx, [rbp+disp] */
    { 0x4C, 0x8B, 0x45, 0 }, /* mov r8,  [rbp+disp] */
    { 0x4C, 0x8B, 0x4D, 0 }, /* mov r9,  [rbp+disp] */
};
#else
static const uint8_t ARG_LOAD_TEMPLATES[4][4] = {
    { 0x48, 0x8B, 0x7D, 0 }, /* mov rdi, [rbp+disp] */
    { 0x48, 0x8B, 0x75, 0 }, /* mov rsi, [rbp+disp] */
    { 0x48, 0x8B, 0x55, 0 }, /* mov rdx, [rbp+disp] */
    { 0x48, 0x8B, 0x4D, 0 }, /* mov rcx, [rbp+disp] */
};
#endif

/* Incoming-parameter registers at function entry (same ABI difference). */
#if defined(KC_PLATFORM_WINDOWS)
static const uint8_t ARG_STORE_TEMPLATES[4][4] = {
    { 0x48, 0x89, 0x4D, 0 }, /* mov [rbp+disp], rcx */
    { 0x48, 0x89, 0x55, 0 }, /* mov [rbp+disp], rdx */
    { 0x4C, 0x89, 0x45, 0 }, /* mov [rbp+disp], r8  */
    { 0x4C, 0x89, 0x4D, 0 }, /* mov [rbp+disp], r9  */
};
#else
static const uint8_t ARG_STORE_TEMPLATES[4][4] = {
    { 0x48, 0x89, 0x7D, 0 }, /* mov [rbp+disp], rdi */
    { 0x48, 0x89, 0x75, 0 }, /* mov [rbp+disp], rsi */
    { 0x48, 0x89, 0x55, 0 }, /* mov [rbp+disp], rdx */
    { 0x48, 0x89, 0x4D, 0 }, /* mov [rbp+disp], rcx */
};
#endif

static void emit_call_self(Emitter *e, void *target) {
    /* Windows x64 requires the CALLER to reserve 32 bytes of "shadow
       space" before any call; System V does not. rsp is 16-aligned
       here (guaranteed by construction — see the frame-size comment
       in kc_jit_try_compile), and 32 is a multiple of 16, so this
       preserves alignment rather than breaking it. */
#if defined(KC_PLATFORM_WINDOWS)
    { uint8_t c[4] = {0x48,0x83,0xEC,0x20}; emit(e, c, 4); } /* sub rsp,32 */
#endif
    /* movabs r10, target ; call r10 */
    uint8_t mov[2] = { 0x49, 0xBA };
    emit(e, mov, 2);
    int64_t addr = (int64_t)(intptr_t)target;
    emit(e, (const uint8_t *)&addr, 8);
    uint8_t call[3] = { 0x41, 0xFF, 0xD2 };
    emit(e, call, 3);
#if defined(KC_PLATFORM_WINDOWS)
    { uint8_t c[4] = {0x48,0x83,0xC4,0x20}; emit(e, c, 4); } /* add rsp,32 */
#endif
}

/* ---------------------------- expression compilation ---------------------------- */

static bool compile_expr(Emitter *e, KcNode *n);
static bool compile_stmt(Emitter *e, KcNode *n);
static bool compile_block(Emitter *e, KcNode *block);
static bool compile_truthy_not(Emitter *e, KcNode *rhs);

/* Compiles `cond`, leaving a normalized 0/1 truthiness in rax (matches
   kc_truthy for int: nonzero -> 1, zero -> 0). Used by if/while/&&/||. */
static bool compile_truthy(Emitter *e, KcNode *n) {
    if (!compile_expr(e, n)) return false;
    emit_cmp_rax_0(e);
    emit_setcc_rax(e, 0x95); /* setne */
    return true;
}

static bool compile_binop(Emitter *e, KcNode *n) {
    KcOp op = (KcOp)n->as.binop.op;

    if (op == OP_AND || op == OP_OR) {
        /* Short-circuit, matching interp.c's eval_binop exactly: both
           sides normalized through truthiness, never the raw value. */
        if (!compile_truthy(e, n->as.binop.lhs)) return false;
        emit_cmp_rax_0(e);
        size_t to_rhs_or_short = (op == OP_AND) ? emit_je_fwd(e) /* AND: lhs false -> skip rhs, result 0 */
                                                 : 0;
        size_t to_short_true = 0;
        if (op == OP_OR) {
            /* OR: lhs TRUE -> short-circuit to result 1 without rhs */
            emit1(e, 0x0F); emit1(e, 0x85); /* jne rel32 (lhs != 0) */
            to_short_true = emit_rel32_placeholder(e);
        }
        if (!compile_truthy(e, n->as.binop.rhs)) return false;
        size_t to_end = emit_jmp_fwd(e);
        if (op == OP_AND) {
            patch_rel32(e, to_rhs_or_short, e->len);
            emit_mov_rax_0(e);
        } else {
            size_t short_true_target = e->len;
            emit_mov_rax_1(e);
            size_t to_end2 = emit_jmp_fwd(e);
            patch_rel32(e, to_short_true, short_true_target);
            patch_rel32(e, to_end2, e->len);
        }
        patch_rel32(e, to_end, e->len);
        return true;
    }

    if (!compile_expr(e, n->as.binop.lhs)) return false;
    int slot = alloc_scratch(e);
    if (slot < 0) return false;
    emit_store_slot_from_rax(e, slot);
    if (!compile_expr(e, n->as.binop.rhs)) return false;
    emit_load_rbx_from_slot(e, slot); /* rbx = lhs, rax = rhs */
    free_scratch(e);

    switch (op) {
        case OP_ADD: emit_add_rax_rbx(e); return true;
        case OP_MUL: emit_mul_rax_rbx(e); return true;
        case OP_SUB: emit_sub_rbx_rax(e); emit_mov_rax_rbx(e); return true;
        case OP_LT:  emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x9C); return true; /* setl  */
        case OP_LE:  emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x9E); return true; /* setle */
        case OP_GT:  emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x9F); return true; /* setg  */
        case OP_GE:  emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x9D); return true; /* setge */
        case OP_EQ:  emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x94); return true; /* sete  */
        case OP_NEQ: emit_cmp_rbx_rax(e); emit_setcc_rax(e, 0x95); return true; /* setne */
        default: return false; /* DIV/MOD: no safe-by-construction div-by-zero check written yet — bail */
    }
}

static bool compile_self_call(Emitter *e, KcNode *n) {
    int nargs = n->as.call.args.count;
    if (nargs != e->nparams) return false; /* arity must match this function's own signature */
    int arg_slots[4];
    for (int i = 0; i < nargs; i++) {
        if (!compile_expr(e, n->as.call.args.items[i])) return false;
        int slot = alloc_scratch(e);
        if (slot < 0) return false;
        emit_store_slot_from_rax(e, slot);
        arg_slots[i] = slot;
    }
    for (int i = 0; i < nargs; i++) {
        uint8_t c[4];
        memcpy(c, ARG_LOAD_TEMPLATES[i], 4);
        c[3] = (uint8_t)slot_disp(arg_slots[i]);
        emit(e, c, 4);
    }
    for (int i = 0; i < nargs; i++) free_scratch(e);
    emit_call_self(e, e->self_addr);
    return true;
}

static bool compile_expr(Emitter *e, KcNode *n) {
    switch (n->kind) {
        case N_INT:
            emit_mov_rax_imm64(e, n->ival);
            return true;
        case N_IDENT: {
            int slot = find_slot(e, n->as.ident.name);
            if (slot < 0) return false; /* free variable / global — outside our shape */
            emit_load_rax_from_slot(e, slot);
            return true;
        }
        case N_UNOP:
            if (n->as.unop.op == OP_NEG) {
                if (!compile_expr(e, n->as.unop.rhs)) return false;
                emit_neg_rax(e);
                return true;
            }
            if (n->as.unop.op == OP_NOT) return compile_truthy_not(e, n->as.unop.rhs);
            return false;
        case N_BINOP:
            return compile_binop(e, n);
        case N_ASSIGN: {
            if (n->as.assign.target->kind != N_IDENT) return false; /* no member/index targets in this shape */
            int slot = find_slot(e, n->as.assign.target->as.ident.name);
            if (slot < 0) return false;
            if (n->as.assign.op == -1) {
                if (!compile_expr(e, n->as.assign.value)) return false;
            } else {
                /* compound assign: load current, compute, matches eval_binop(cur, value) order */
                if (!compile_expr(e, n->as.assign.value)) return false;
                int rhs_slot = alloc_scratch(e);
                if (rhs_slot < 0) return false;
                emit_store_slot_from_rax(e, rhs_slot);
                emit_load_rax_from_slot(e, slot);       /* rax = current value (lhs of the op) */
                int lhs_slot = alloc_scratch(e);
                if (lhs_slot < 0) return false;
                emit_store_slot_from_rax(e, lhs_slot);
                emit_load_rax_from_slot(e, rhs_slot);   /* rax = rhs */
                emit_load_rbx_from_slot(e, lhs_slot);   /* rbx = lhs */
                free_scratch(e); free_scratch(e);
                switch ((KcOp)n->as.assign.op) {
                    case OP_ADD: emit_add_rax_rbx(e); break;
                    case OP_SUB: emit_sub_rbx_rax(e); emit_mov_rax_rbx(e); break;
                    default: return false;
                }
            }
            emit_store_slot_from_rax(e, slot);
            return true; /* assignment is itself an expression yielding the stored value, already in rax */
        }
        case N_CALL: {
            if (n->as.call.callee->kind != N_IDENT) return false;
            if (!e->self_name || strcmp(n->as.call.callee->as.ident.name, e->self_name) != 0) return false;
            return compile_self_call(e, n);
        }
        default:
            return false;
    }
}

/* Logical NOT: proper truthiness-based negation (matches kc_truthy),
   not a bitwise trick — `!5` must be false (0), not "5 XOR 1". */
static bool compile_truthy_not(Emitter *e, KcNode *rhs) {
    if (!compile_expr(e, rhs)) return false;
    emit_cmp_rax_0(e);
    emit_setcc_rax(e, 0x94); /* sete: 1 if rhs was 0 */
    return true;
}

/* ---------------------------- statement compilation ---------------------------- */

static bool compile_if(Emitter *e, KcNode *n) {
    if (!compile_truthy(e, n->as.ifstmt.cond)) return false;
    emit_cmp_rax_0(e);
    size_t to_else = emit_je_fwd(e);
    if (!compile_stmt(e, n->as.ifstmt.then_branch)) return false;
    if (n->as.ifstmt.else_branch) {
        size_t to_end = emit_jmp_fwd(e);
        patch_rel32(e, to_else, e->len);
        if (!compile_stmt(e, n->as.ifstmt.else_branch)) return false;
        patch_rel32(e, to_end, e->len);
    } else {
        patch_rel32(e, to_else, e->len);
    }
    return true;
}

static bool compile_while(Emitter *e, KcNode *n) {
    if (e->loop_depth >= MAX_LOOP_DEPTH) return false;
    LoopCtx *lc = &e->loops[e->loop_depth++];
    lc->break_count = 0;
    lc->loop_start_offset = e->len;

    if (!compile_truthy(e, n->as.whilestmt.cond)) { e->loop_depth--; return false; }
    emit_cmp_rax_0(e);
    size_t to_end = emit_je_fwd(e);
    if (!compile_stmt(e, n->as.whilestmt.body)) { e->loop_depth--; return false; }
    emit_jmp_back(e, lc->loop_start_offset);
    patch_rel32(e, to_end, e->len);
    for (int i = 0; i < lc->break_count; i++) patch_rel32(e, lc->break_fixups[i], e->len);

    e->loop_depth--;
    return true;
}

static bool compile_stmt(Emitter *e, KcNode *n) {
    switch (n->kind) {
        case N_LET: {
            if (!n->as.let.init || n->as.let.is_static) return false; /* uninitialized/static locals: outside this shape */
            if (!compile_expr(e, n->as.let.init)) return false;
            int slot = declare_local(e, n->as.let.name);
            if (slot < 0) return false;
            emit_store_slot_from_rax(e, slot);
            return true;
        }
        case N_EXPRSTMT:
            return compile_expr(e, n->as.exprstmt.expr);
        case N_BLOCK:
            return compile_block(e, n);
        case N_IF:
            return compile_if(e, n);
        case N_WHILE:
            return compile_while(e, n);
        case N_RETURN: {
            if (!n->as.retstmt.value) return false; /* every path must return a value in this shape */
            if (!compile_expr(e, n->as.retstmt.value)) return false;
            if (e->return_fixup_count >= MAX_RETURN_FIXUPS) return false;
            e->return_fixups[e->return_fixup_count++] = emit_jmp_fwd(e);
            return true;
        }
        case N_BREAK: {
            if (e->loop_depth == 0) return false;
            LoopCtx *lc = &e->loops[e->loop_depth - 1];
            if (lc->break_count >= MAX_BREAKS_PER_LOOP) return false;
            lc->break_fixups[lc->break_count++] = emit_jmp_fwd(e);
            return true;
        }
        case N_CONTINUE: {
            if (e->loop_depth == 0) return false;
            emit_jmp_back(e, e->loops[e->loop_depth - 1].loop_start_offset);
            return true;
        }
        default:
            return false; /* fn/struct/import/extern decls, for-loops, unsafe, etc.: outside this shape */
    }
}

static bool compile_block(Emitter *e, KcNode *block) {
    for (int i = 0; i < block->as.block.stmts.count; i++)
        if (!compile_stmt(e, block->as.block.stmts.items[i])) return false;
    return true;
}

/* ---------------------------- top-level entry point ---------------------------- */

static int round_up_16(int bytes) { return (bytes + 15) & ~15; }

void kc_jit_try_compile(KcClosure *cl) {
    cl->jit_tried = 1;
    /* Fallback path: if this platform can't (or the user won't) run generated
     * machine code, leave cl->jit_code NULL — kc_call_value() then runs the
     * closure in the interpreter exactly as it would before the threshold. */
    if (!kc_jit_available()) return;
    KcNode *fn = cl->fnnode;
    int nparams = fn->as.fn.params.count;
    if (nparams > 4) return;
    KcNode *body = fn->as.fn.body;
    if (body->kind != N_BLOCK || body->as.block.stmts.count == 0) return;
    /* The last top-level statement must be a `return`, so "falling off
       the end" never happens — see the file header comment on why
       this precondition is load-bearing for correctness, not style. */
    if (body->as.block.stmts.items[body->as.block.stmts.count - 1]->kind != N_RETURN) return;

    /* Executable memory is allocated BEFORE compiling (not sized to
       the final byte count afterward, as a simpler single-expression
       JIT could get away with) because a self-call needs to embed
       its OWN final address as an immediate while still being
       compiled — this is the only order that makes that possible. */
    void *mem = kc_exec_alloc(MAX_CODE_BYTES);
    if (!mem) return;

    static uint8_t stackbuf[MAX_CODE_BYTES];
    Emitter e; memset(&e, 0, sizeof e);
    e.buf = stackbuf; e.cap = sizeof stackbuf; e.ok = true;
    e.nparams = nparams;
    for (int i = 0; i < nparams; i++) e.param_names[i] = fn->as.fn.params.items[i]->as.ident.name;
    e.self_name = cl->name; /* NULL for an anonymous fn-expr: self-calls then never match, correctly */
    e.self_addr = mem;

    /* prologue: push rbp; mov rbp,rsp; sub rsp,<patched later> */
    { uint8_t c[] = {0x55, 0x48,0x89,0xE5}; emit(&e, c, sizeof c); }
    { uint8_t c[] = {0x48,0x81,0xEC, 0,0,0,0}; emit(&e, c, sizeof c); e.sub_rsp_patch_offset = e.len - 4; }
    for (int i = 0; i < nparams; i++) {
        uint8_t c[4]; memcpy(c, ARG_STORE_TEMPLATES[i], 4);
        c[3] = (uint8_t)slot_disp(i);
        emit(&e, c, 4);
    }

    bool ok = compile_block(&e, body) && e.ok;
    if (!ok) { kc_exec_free(mem, MAX_CODE_BYTES); return; }

    /* epilogue: every `return` jumps here; falling through also lands
       here naturally since it's simply the next thing in the buffer
       (though the last-statement-is-return precondition above means
       that should never actually happen for a well-formed function). */
    size_t epilogue_offset = e.len;
    { uint8_t c[] = {0xC9, 0xC3}; emit(&e, c, sizeof c); } /* leave; ret */
    if (!e.ok) { kc_exec_free(mem, MAX_CODE_BYTES); return; }
    for (int i = 0; i < e.return_fixup_count; i++) patch_rel32(&e, e.return_fixups[i], epilogue_offset);

    int total_slots = nparams + e.n_locals + e.scratch_max;
    if (total_slots > MAX_SLOTS) { kc_exec_free(mem, MAX_CODE_BYTES); return; }
    int32_t frame_bytes = round_up_16(total_slots * 8);
    memcpy(e.buf + e.sub_rsp_patch_offset, &frame_bytes, 4);

    memcpy(mem, e.buf, e.len);
    if (kc_exec_seal(mem, MAX_CODE_BYTES) != 0) { kc_exec_free(mem, MAX_CODE_BYTES); return; }
    cl->jit_code = mem;
    cl->jit_size = MAX_CODE_BYTES;
}

int64_t kc_jit_invoke(KcClosure *cl, int64_t *a, int nargs) {
    switch (nargs) {
        case 0: { int64_t (*f)(void) = cl->jit_code; return f(); }
        case 1: { int64_t (*f)(int64_t) = cl->jit_code; return f(a[0]); }
        case 2: { int64_t (*f)(int64_t,int64_t) = cl->jit_code; return f(a[0],a[1]); }
        case 3: { int64_t (*f)(int64_t,int64_t,int64_t) = cl->jit_code; return f(a[0],a[1],a[2]); }
        case 4: { int64_t (*f)(int64_t,int64_t,int64_t,int64_t) = cl->jit_code; return f(a[0],a[1],a[2],a[3]); }
        default: return 0;
    }
}
