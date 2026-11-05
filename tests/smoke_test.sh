#!/usr/bin/env bash
# KingCobra smoke tests — exercises the interpreter, JIT, FFI, and every
# std lib module with actual assertions on output, not just exit codes.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${KC_BIN:-./kingcobra}
[[ -z "${KC_BIN:-}" ]] && make -s >/dev/null
make -s examples/native/mathutil.o >/dev/null

# The architecture THIS binary was built for (x86_64, aarch64, armhf, riscv64, ...). Tests that depend on
# x86 machine code or on the x86-64 JIT only run there; everything else must pass on every architecture.
HOST_ARCH=$($BIN info | sed -n 's|^  platform  *[^/]*/||p')

pass=0
fail=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        echo "ok   - $desc"
        pass=$((pass+1))
    else
        echo "FAIL - $desc"
        echo "       expected to contain: $expected"
        echo "       got: $actual"
        fail=$((fail+1))
    fi
}

out=$($BIN run examples/hello.kc)
check "hello world" "Hello from KingCobra!" "$out"

out=$($BIN run examples/basics.kc)
check "sum 0..10" "sum 0..10 = 45" "$out"
check "fib(12)" "fib(12) = 144" "$out"
check "closures" "add5(10) = 15" "$out"

out=$($BIN run examples/jit_hot_loop.kc)
check "closure JIT correctness" "total after 200 hot calls = 2646700" "$out"

if [[ "$HOST_ARCH" == "x86_64" ]]; then
out=$($BIN run examples/std_rhex.kc)
check "rhex one-shot exec" "one-shot exec: 42" "$out"
check "rhex call_args SysV/Win64 int args" "call_args(19, 23) -> 42" "$out"
check "rhex protect rx -> rw round trip" "read back: [137, 248, 1, 240, 195]" "$out"
check "rhex cpu_vendor runs" "vendor:" "$out"
check "rhex has_feature boolean" "has avx2:" "$out"
fi

rhex_run() { printf 'import std.rhex;\nfn main() { %s }\n' "$1" > /tmp/kc_rhex_t.kc; $BIN run /tmp/kc_rhex_t.kc 2>&1; }
check "rhex missing unsafe is a clear error" "requires an \`unsafe { }\` block" "$(rhex_run 'let b = rhex.alloc(8);')"
if [[ "$HOST_ARCH" == "x86_64" ]]; then check "rhex unknown feature name is a clear error" "unknown feature name" "$(rhex_run 'unsafe { print(rhex.has_feature("not_a_real_feature")); }')"; fi
if [[ "$HOST_ARCH" != "generic" && "$HOST_ARCH" != "unknown" ]]; then check "rhex use-after-free is a clear error" "already freed" "$(rhex_run 'unsafe { let b = rhex.alloc(8); rhex.free(b); rhex.size(b); }')"; fi
if [[ "$HOST_ARCH" != "generic" && "$HOST_ARCH" != "unknown" ]]; then check "rhex write to sealed buffer is a clear error" "sealed executable" "$(rhex_run 'unsafe { let b = rhex.alloc(8); rhex.protect(b, "rx"); rhex.write(b, 0, 1); }')"; fi
if [[ "$HOST_ARCH" == "x86_64" ]]; then check "rhex rdtsc forbidden in comptime despite needing no unsafe" "not allowed inside a comptime block" "$(printf 'import std.rhex;\nfn main() { comptime { let x = rhex.rdtsc(); } }\n' > /tmp/kc_rhex_ct.kc; $BIN run /tmp/kc_rhex_ct.kc 2>&1)"; fi

out=$($BIN run examples/std_ffi.kc)
check "extern FFI libm sqrt" "1.41421" "$out"

out=$($BIN run examples/std_memory.kc)
check "memory read-back" "read back: KingCobra!" "$out"

out=$($BIN run examples/std_network.kc)
check "network protocol-builder pack/unpack round-trip" "src port: 53124" "$out"
check "network checksum16" "checksum: 12350" "$out"
check "network udp round trip (loopback)" "udp server received: ping from 127.0.0.1" "$out"
check "network raw socket create/close" "raw socket created and closed ok" "$out"
check "network resolve loopback" "resolved 127.0.0.1 -> [127.0.0.1]" "$out"

out=$($BIN run examples/structs.kc)
check "struct literal + methods" "dist_sq(a) = 25" "$out"
check "struct reference semantics" "a.x after mutating through b: 100" "$out"

out=$($BIN run examples/std_io.kc)
check "std.io write/read round-trip" "read back: line one" "$out"
check "std.io line count after append" "line count: 4" "$out"

out=$($BIN run examples/match.kc)
check "match expression wildcard" "3 -> many" "$out"
check "match expression on string" "match on string 'b' -> 2" "$out"

out=$($BIN run examples/modules_demo.kc)
check "file-based module import (default name)" "square(5) = 25" "$out"
check "file-based module import (alias)" "cube(3) = 27" "$out"

out=$($BIN run examples/string_array_methods.kc)
check "string method chaining" "upper: HELLO, KINGCOBRA WORLD" "$out"
check "array methods" "reversed: [5, 1, 4, 1, 3]" "$out"

# static type checker: this program must FAIL with a specific message
out=$(echo 'fn main() { let x: int = "oops"; }' > /tmp/kc_tc_check.kc; $BIN run /tmp/kc_tc_check.kc 2>&1; echo "RC:$?")
check "type checker catches let mismatch" "declared int, but initializer is string" "$out"
check "type checker exits nonzero on error" "RC:65" "$out"

out=$($BIN run examples/comptime.kc)
check "comptime block computes and escapes bindings" "FIB10 = 55" "$out"

# comptime purity gating: a side-effecting call inside comptime must FAIL
out=$(cat > /tmp/kc_comptime_impure.kc << 'KCEOF'
import std.system;
comptime { system.exec("echo nope"); }
fn main() {}
KCEOF
$BIN run /tmp/kc_comptime_impure.kc 2>&1; echo "RC:$?")
check "comptime blocks a side-effecting call" "not allowed inside a comptime block" "$out"
check "comptime violation exits nonzero" "RC:65" "$out"

out=$($BIN run examples/native_import_demo.kc)
check "native import (.o, linked+dlopen) int path" "square(6) = 36" "$out"
check "native import float-argument path" "half(7.0) = 3.5" "$out"

# extended JIT: self-recursion, if/else, while+break/continue, &&/||/!
out=$($BIN run examples/jit_recursive.kc)
check "JIT self-recursive fib" "fib( 19 ) = 4181" "$out"
check "JIT while/break/continue/compound-assign" "sum_to( 54 ) = 1268" "$out"
check "JIT comparisons and &&/||/!" "classify(150) = 2" "$out"


# ---- std.vector / std.regx / std.gui ----
out=$($BIN run examples/std_vector.kc)
check "vector add" "add: [5, 7, 9]" "$out"
check "vector cross product" "cross: [0, 0, 1]" "$out"
check "vector quaternion rotation (+X about +Z -> +Y)" "quat rotate: [0, 1000, 0]" "$out"
check "vector mat4 inverse round-trip" "inverse round-trip: [5, -1, 2]" "$out"
check "vector ray/sphere" "ray_sphere: 4" "$out"

out=$($BIN run examples/std_regx.kc)
check "regx test" "test digits: true false" "$out"
check "regx named group" "named year: 2026" "$out"
check "regx replace with group template" "swap: world hello" "$out"
check "regx lazy vs greedy" "lazy: <a>" "$out"
check "regx lookahead" "lookahead: [hi, there]" "$out"
check "regx backreference" "backref: ll" "$out"
check "regx split" "split: [a, b, c, d]" "$out"
check "regx is_valid does not abort on bad pattern" "valid: false true false" "$out"
check "regx nested empty-loop terminates" "empty-loop safe: true" "$out"

out=$($BIN run examples/std_gui.kc)
check "gui button click" "clicks: 2" "$out"
check "gui textbox typing + callback" "textbox: hello | callback saw: hello" "$out"
check "gui textbox editing (caret, backspace)" "after edit: helXl" "$out"
check "gui select-all + replace" "replace-all: new" "$out"
check "gui radio group exclusivity" "radio exclusive: false true" "$out"
check "gui slider keyboard" "slider after End: 10" "$out"
check "gui list selection" "list picked index: 1 b" "$out"
check "gui confirm dialog" "confirm answer: true | dialog open: false" "$out"
check "gui virtual-clock timers" "timer ticks in 350ms: 3" "$out"
check "gui disabled widget ignores input" "disabled button ignored: 2" "$out"

# regx catastrophic backtracking must end in a clean error, not a hang
out=$(cat > /tmp/kc_regx_bomb.kc << 'KCEOF'
import std.regx;
fn main() { print(regx.test("(a+)+$", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab")); }
KCEOF
timeout 60 $BIN run /tmp/kc_regx_bomb.kc 2>&1; echo "RC:$?")
check "regx backtracking limit is a clean error" "backtracking limit" "$out"

# std.mmio / std.acpi must be registered and run their demos
out=$($BIN run examples/std_mmio.kc 2>&1)
check "std.mmio registered + virtual region demo" "CTRL   = 0x00000581  field[11:8] = 5" "$out"
check "std.mmio write/read block" "block  = [1, 2, 3, 4]" "$out"

out=$($BIN run examples/std_acpi.kc 2>&1)
check "std.acpi registered + synthetic table demo" "FADT rev 6 oem FIRECK checksum ok: true" "$out"

# std.mmu: example + the four self-checking suites (each prints "PASSED n FAILED 0" / "FUZZ TOTAL 0")
out=$($BIN run examples/std_mmu.kc 2>&1)
check "std.mmu demo: page-table dump" "0xffff800000000000 0xffff800000001000     4K  rw- sg--- pa=0x1002000 pg=4K nc" "$out"
check "std.mmu demo: x86 #PF error code" "[#PF err=0x7]" "$out"
check "std.mmu demo: stale TLB until INVLPG" "stale TLB entry still reads: 42" "$out"
check "std.mmu demo: fork copy-on-write" "parent sees 100 - child sees 200 - COW faults: 1" "$out"
check "std.mmu demo: swap round trip" "sum of squares survives swapping: 10416" "$out"
check "std.mmu demo: host W^X-safe protection" "host: write to read-only page refused: true - value still 7" "$out"
for t in mmu_test mmu_vm_test mmu_host_test; do
    out=$($BIN run tests/$t.kc 2>&1 | tail -1)
    check "std.mmu suite $t" "FAILED 0" "$out"
done
out=$($BIN run tests/mmu_fuzz_test.kc 2>&1 | tail -1)
check "std.mmu randomised differential test (9 architectures)" "FUZZ TOTAL 0" "$out"
mmu_run() { printf 'import std.mmu;\nfn main() { %s }\n' "$1" > /tmp/kc_mmu_t.kc; $BIN run /tmp/kc_mmu_t.kc 2>&1; }
check "mmu unknown arch is a clear error" "unknown architecture 'pdp11'" "$(mmu_run 'mmu.space("pdp11", mmu.phys("1M"));')"
check "mmu use after destroy is a clear error" "already destroyed" "$(mmu_run 'let s = mmu.space("x86_64", mmu.phys("1M")); mmu.destroy(s); mmu.read8(s, 0);')"
check "mmu strict mode makes faults fatal" "unhandled page fault" "$(mmu_run 'let s = mmu.space("x86_64", mmu.phys("1M"), "strict"); mmu.read8(s, 4096);')"
check "mmu executable host memory needs unsafe" "requires an \`unsafe { }\` block" "$(mmu_run 'mmu.host_alloc(4096, "r,x");')"
check "mmu is rejected in comptime when it touches the host" "not allowed inside a comptime block" "$(mmu_run 'comptime { let r = mmu.host_alloc(4096); }')"

# std.nlp
nlp_run() { printf 'import std.nlp;\nfn main() { print(%s); }\n' "$1" > /tmp/kc_nlp_t.kc; $BIN run /tmp/kc_nlp_t.kc 2>&1; }
check "nlp tokenize keeps contractions/decimals" "[It's, 3.14, café]" "$(nlp_run 'nlp.tokenize("It'"'"'s 3.14 café!")')"
check "nlp clitic split" "[can, n't, it, 's]" "$(nlp_run 'nlp.tokenize("can'"'"'t it'"'"'s", "lc")')"
check "nlp sentences handle abbreviations" "[Dr. Smith left., He ran.]" "$(nlp_run 'nlp.sentences("Dr. Smith left. He ran.")')"
check "nlp porter stemmer" "[run, caress, poni, relat]" "$(nlp_run 'nlp.stem_all(["running","caresses","ponies","relational"])')"
check "nlp unicode case + accents" "STRASSE" "$(nlp_run 'nlp.upper("straße")')"
check "nlp strip_accents" "Creme brulee Lodz" "$(nlp_run 'nlp.strip_accents("Crème brûlée Łódź")')"
check "nlp levenshtein" "3" "$(nlp_run 'nlp.levenshtein("kitten","sitting")')"
check "nlp jaro_winkler" "0.961111" "$(nlp_run 'nlp.jaro_winkler("MARTHA","MARHTA")')"
check "nlp soundex" "[R163, P236, T522]" "$(nlp_run '[nlp.soundex("Robert"), nlp.soundex("Pfister"), nlp.soundex("Tymczak")]')"
check "nlp bm25 ranks relevant doc first" "[[1," "$(nlp_run 'nlp.bm25_search(nlp.bm25_new(["a b c","x y z","y y q"]), "y z", 1)')"
cat > /tmp/kc_nlp_nb.kc << 'KCEOF'
import std.nlp;
fn main() {
    let nb = nlp.nb_new();
    nlp.nb_train_many(nb, ["good fun film", "bad sad film"], ["pos", "neg"]);
    print(nlp.nb_predict(nb, "fun"), nlp.nb_predict(nb, "sad"));
}
KCEOF
check "nlp naive bayes" "pos neg" "$($BIN run /tmp/kc_nlp_nb.kc 2>&1)"
check "nlp spell correct keeps case" "Receive" "$(nlp_run 'nlp.spell_correct(nlp.spell_new("receive believe"), "Recieve")')"
check "nlp sentiment negation" "negative" "$(nlp_run 'nlp.sentiment_label("This is not good at all.")')"
check "nlp detect_lang" "es" "$(nlp_run 'nlp.detect_lang("El gato esta en la casa y no quiere salir de la cama").lang')"
check "nlp extraction" "[bob@x.org]" "$(nlp_run 'nlp.emails("mail bob@x.org now")')"
check "nlp invalid argument is a clean error" "nlp.hamming: strings must have equal length" "$(nlp_run 'nlp.hamming("a","bb")')"
check "nlp arity error has position" "expects 2 arguments, got 1" "$(nlp_run 'nlp.levenshtein("a")')"
out=$($BIN run examples/std_nlp.kc 2>&1)
check "nlp example runs" "chunks: 2" "$out"

# std.nn
out=$($BIN run tests/nn_gradcheck.kc 2>&1)
check "nn gradients match numerical derivatives (all layers/losses)" "gradcheck failures: 0" "$out"
out=$($BIN run tests/nn_math.kc 2>&1)
check "nn advanced math (special fns, scans, sort, shape ops, einsum, stats, linalg, sampling)" "nn math failures: 0" "$out"
nn_run() { printf 'import std.nn;\nfn main() { %s }\n' "$1" > /tmp/kc_nn_t.kc; $BIN run /tmp/kc_nn_t.kc 2>&1; }
check "nn tensor broadcasting" "tensor[2,3] [[11, 22, 33], [14, 25, 36]]" "$(nn_run 'print(nn.add(nn.tensor([[1,2,3],[4,5,6]]), [10,20,30]));')"
check "nn matmul" "tensor[2,2] [[14, 32], [32, 77]]" "$(nn_run 'let a = nn.tensor([[1,2,3],[4,5,6]]); print(nn.matmul(a, nn.transpose(a)));')"
check "nn reductions" "tensor[3] [2.5, 3.5, 4.5]" "$(nn_run 'print(nn.mean(nn.tensor([[1,2,3],[4,5,6]]), 0));')"
check "nn one_hot" "tensor[3,3] [[1, 0, 0], [0, 0, 1], [0, 1, 0]]" "$(nn_run 'print(nn.one_hot([0,2,1], 3));')"
cat > /tmp/kc_nn_learn.kc << 'KCEOF'
import std.nn;
fn main() {
    nn.seed(42);
    let net = nn.mlp([2, 8, 1], "activation", "tanh", "out_activation", "sigmoid");
    nn.fit(net, "bce", "adam", [[0,0],[0,1],[1,0],[1,1]], [[0],[1],[1],[0]], "epochs", 400, "batch_size", 4, "lr", 0.05, "verbose", 0);
    print(nn.predict_class(net, [[0,0],[0,1],[1,0],[1,1]]));
    let lin = nn.sequential([nn.dense(2, 1)]);
    let X = nn.randn([200, 2]);
    let Y = nn.add(nn.matmul(X, nn.tensor([[2.0], [-1.0]])), 0.5);
    nn.fit(lin, "mse", nn.sgd(lin, 0.1), X, Y, "epochs", 60, "batch_size", 32, "verbose", 0);
    print(nn.get_params(lin));
}
KCEOF
out=$($BIN run /tmp/kc_nn_learn.kc 2>&1)
check "nn learns XOR" "[0, 1, 1, 0]" "$out"
check "nn linear regression recovers weights" "tensor[2,1] [[2], [-1]], tensor[1] [0.5]" "$out"
cat > /tmp/kc_nn_io.kc << 'KCEOF'
import std.nn;
fn same(a, b) -> bool { return nn.item(nn.max(nn.abs(nn.sub(a, b)))) == 0.0; }
fn main() {
    nn.seed(5);
    let m = nn.sequential([nn.conv1d(2, 3, 3), nn.batchnorm(3), nn.relu(), nn.global_avg_pool(), nn.dense(3, 2)]);
    let x = nn.randn([4, 2, 8]);
    nn.fit(m, "mse", "adam", x, nn.randn([4, 2]), "epochs", 3, "verbose", 0);
    nn.save(m, "/tmp/kc_nn_model.kcnn");
    let r = nn.load("/tmp/kc_nn_model.kcnn");
    let c = nn.clone(m);
    print(same(nn.forward(m, x, false), nn.forward(r, x, false)), same(nn.forward(m, x, false), nn.forward(c, x, false)));
}
KCEOF
check "nn save/load/clone reproduce predictions exactly" "true true" "$($BIN run /tmp/kc_nn_io.kc 2>&1)"
printf 'garbage' > /tmp/kc_nn_bad.kcnn
check "nn rejects a corrupt model file cleanly" "not a KingCobra nn model file" "$(nn_run 'nn.load("/tmp/kc_nn_bad.kcnn");')"
check "nn bad option is a clear error" "unknown option 'lrr'" "$(nn_run 'let m = nn.mlp([2,3,1]); nn.fit(m, "mse", "adam", [[1,2]], [[1]], "lrr", 0.1);')"
check "nn shape mismatch is a clear error" "expects an input with last dimension equal" "$(nn_run 'let m = nn.mlp([4,3,1]); nn.predict(m, [[1,2]]);')"
check "nn divergence is detected" "training diverged" "$(nn_run 'let m = nn.mlp([2,4,1]); nn.fit(m, "mse", "sgd", [[100.0,200.0],[300.0,400.0]], [[1.0],[2.0]], "epochs", 400, "lr", 10.0, "verbose", 0);')"
out=$($BIN run examples/std_nn.kc 2>&1); rm -f xor_model.kcnn
check "nn example runs (transformer continues the cycle)" "[[1, 2, 3, 4]]" "$out"

# std.nn examples (each must run to completion and reach its headline result)
nn_ex() { $BIN run "examples/nn/$1.kc" 2>&1; }
check "nn example 01 regression"        "mean squared error against the true curve:" "$(nn_ex 01_regression)"
check "nn example 02 spirals"           "learned decision boundary:" "$(nn_ex 02_spirals)"
check "nn example 03 cnn"               "the network says: cross" "$(nn_ex 03_cnn_patterns)"
check "nn example 04 forecast"          "naive last-value" "$(nn_ex 04_forecast)"
check "nn example 05 text sentiment"    "held-out accuracy of the nlp.sentiment lexicon baseline:" "$(nn_ex 05_text_sentiment)"
check "nn example 06 char LM"           "the quick brown fox jumps over the lazy dog" "$(nn_ex 06_char_language_model)"
check "nn example 07 dqn"               "(return 0.8 in 6 steps)" "$(nn_ex 07_dqn_gridworld)"
check "nn example 08 transfer"          "frozen pre-trained encoder + head:" "$(nn_ex 08_autoencoder_transfer)"
check "nn example 09 tensor tour"       "after nn.free the handle reports: <nn.freed>" "$(nn_ex 09_tensor_tour)"
check "nn example 10 verify"            "gradcheck passed: true" "$(nn_ex 10_verify_your_model)"
check "nn example 11 math toolkit"      "math toolkit tour complete" "$(nn_ex 11_math_toolkit)"
rm -f verify_weights.kcw verify_model.kcnn

# ---- std.system (v0.4.2: modular, matured) ----
out=$($BIN run examples/std_system.kc 2>&1)
check "system example: info + env"            "env: kingcobra true fallback" "$out"
check "system example: expand_env"            "expand: demo=kingcobra literal=$" "$out"
check "system example: path functions"        "base: main.kc | dir: project/src | ext: .kc | stem: main" "$out"
check "system example: fs tree + cleanup"     "cleaned up: true" "$out"
check "system example: run captures streams"  "run: 3 stdout: to-stdout" "$out"
check "system example: timeout kills child"   "timed out and was killed: true" "$out"
check "system example: spawn + pipes"         "spawned child exited with 0 and said: ping" "$out"

sys_t() { printf 'import std.system;\nimport std.io;\nfn main() {\n%s\n}\n' "$1" > /tmp/kc_sys_t.kc; $BIN run /tmp/kc_sys_t.kc a1 "b 2" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }

check "system.args passes script args"        "[a1, b 2]" "$(sys_t 'print(system.args());')"
check "system path edge cases"                "/a/b/d ../../b . /" "$(sys_t 'print(system.normalize("/a/./b//c/../d"), system.normalize("../a/../../b"), system.normalize("a/.."), system.normalize("/.."));')"
check "system dirname/basename roots"         "/ . / a b" "$(sys_t 'print(system.dirname("/"), system.dirname("a"), system.dirname("/a"), system.dirname("a/b/"), system.basename("a/b/"));')"
check "system extension rules"                ".gz .bashrc" "$(sys_t 'print(system.extension("c.tar.gz"), system.stem(".bashrc"));')"
check "system run_argv is shell-free"         "a  b \$HOME *" "$(sys_t 'print(system.run_argv(["echo", "a  b", "\$HOME", "*"]).stdout);')"
check "system run feeds stdin"                "SHOUT" "$(sys_t 'print(system.run("tr a-z A-Z", "shout").stdout);')"
check "system run exit code + stderr"         "code: 3" "$(sys_t 'print(system.run("echo e 1>&2; exit 3"));')"
check "system signal exit is 128+sig"         "9 137" "$(sys_t 'let r = system.run("kill -9 \$\$"); print(r.signal, r.code);')"
check "system timeout kills whole group"      "true true" "$(sys_t 'let t = system.monotonic(); let r = system.run("sleep 30 & sleep 30", null, 0.3); print(r.timed_out, system.monotonic() - t < 3.0);')"
check "system big stdout+stderr no deadlock"  "big: 3000000 1000000" "$(sys_t 'let b = system.run("head -c 3000000 /dev/zero | tr \"\\0\" x; head -c 1000000 /dev/zero | tr \"\\0\" y 1>&2"); print("big:", len(b.stdout), len(b.stderr));')"
check "system big stdin round trip"           "rt: 2000000" "$(sys_t 'let z = system.run("head -c 2000000 /dev/zero | tr \"\\0\" z").stdout; print("rt:", len(system.run_argv(["cat"], z).stdout));')"
check "system missing command -> null+error"  "null | system.run_argv: cannot run" "$(sys_t 'print(system.run_argv(["no_such_cmd_xyz"]), "|", system.last_error());')"
check "system bad cwd -> null+error"          "cannot change directory" "$(sys_t 'system.run_argv(["ls"], null, null, "/no/such/dir"); print(system.last_error());')"
check "system.output strips newline / fails"  "hi|null" "$(sys_t 'let a = system.output("echo hi"); let b = system.output("exit 2"); if (b == null) { print(a + "|null"); } else { print("bad"); }')"
check "system spawn wait timeout then kill"   "null true 143" "$(sys_t 'let q = system.spawn(["sleep","30"]); print(system.wait(q, 0.1), system.kill(q), system.wait(q, 2.0));')"
check "system proc handle use-after-free"     "already freed" "$(sys_t 'let q = system.spawn(["true"]); system.proc_free(q); system.wait(q);')"
check "system read needs capture=true"        "spawned without capture=true" "$(sys_t 'let q = system.spawn(["true"]); system.proc_read(q);')"
check "system fs: mkdir -p, copy, list, rm"   "ok [a, c.txt] gone" "$(sys_t 'let d = system.make_temp_dir("kcs"); system.mkdir(system.path_join(d,"a","b"), true); io.write_file(system.path_join(d,"x"), "hi"); system.copy_file(system.path_join(d,"x"), system.path_join(d,"c.txt")); system.remove_file(system.path_join(d,"x")); let l = system.list_dir(d); let ok = system.rmdir(d, true); if (ok) { print("ok", l, "gone"); } else { print("stuck"); }')"
check "system rmdir refuses root"             "false | system.rmdir: refusing" "$(sys_t 'print(system.rmdir("/", true), "|", system.last_error());')"
check "system rmdir never follows symlinks"   "kept" "$(sys_t 'let k = system.make_temp_dir("kck"); let d = system.make_temp_dir("kcd"); io.write_file(system.path_join(k,"p"), "x"); system.run_argv(["ln","-s",k,system.path_join(d,"l")]); system.rmdir(d, true); if (system.exists(system.path_join(k,"p"))) { print("kept"); } else { print("LOST"); } system.rmdir(k, true);')"
check "system mkdir existing -> false+error"  "false true" "$(sys_t 'let d = system.make_temp_dir("kcm"); print(system.mkdir(d), system.last_error() != ""); system.rmdir(d);')"
check "system.stat on missing is null"        "null" "$(sys_t 'print(system.stat("/no/such/file"));')"
check "system date/format (UTC, negative ts)" "2023-11-14 22:13:20 1969-12-31 23:59:59" "$(sys_t 'print(system.format_time(1700000000, null, true), system.format_time(-1, null, true));')"
check "system utc_offset honours TZ"          "19800 -25200 true" "$(printf 'import std.system;\nfn main(){ print(system.utc_offset(1700000000)); }\n' > /tmp/kc_tz1.kc; a=$(TZ="IST-5:30" $BIN run /tmp/kc_tz1.kc); printf 'import std.system;\nfn main(){ print(system.utc_offset(1690000000), system.date(1690000000).is_dst); }\n' > /tmp/kc_tz2.kc; b=$(TZ="PST8PDT,M3.2.0,M11.1.0" $BIN run /tmp/kc_tz2.kc); echo "$a $b")"
check "system sleep is accurate"              "true" "$(sys_t 'let t = system.monotonic(); system.sleep(0.12); let d = system.monotonic() - t; print(d >= 0.11 && d < 0.5);')"
check "system random_int stays in range"      "true" "$(sys_t 'var ok = true; var i = 0; while (i < 300) { let r = system.random_int(-2, 2); if (r < -2 || r > 2) { ok = false; } i = i + 1; } print(ok);')"
check "system env: setenv/unsetenv visible"   "v true false" "$(sys_t 'system.setenv("KC_ZZ","v"); print(system.getenv("KC_ZZ"), system.has_env("KC_ZZ"), (system.unsetenv("KC_ZZ") == null) && system.has_env("KC_ZZ"));')"
check "system info struct"                    "SystemInfo { os:" "$(sys_t 'print(system.info());')"
check "system rejects bad setenv name"        "invalid variable name" "$(sys_t 'system.setenv("A=B","x");')"
check "system comptime allows pure reads"     "linux a/b" "$(printf 'import std.system;\ncomptime { let o = system.os(); let j = system.path_join("a","b"); }\nfn main() { print(o, j); }\n' > /tmp/kc_sys_ct.kc; $BIN run /tmp/kc_sys_ct.kc)"
for fn in 'system.time()' 'system.mkdir("/tmp/x")' 'system.run("true")' 'system.random_int(1,2)' 'system.spawn(["true"])'; do
    check "system comptime blocks $fn" "not allowed inside a comptime block" "$(printf 'import std.system;\ncomptime { let x = %s; }\nfn main() {}\n' "$fn" > /tmp/kc_sys_ct2.kc; $BIN run /tmp/kc_sys_ct2.kc 2>&1 || true)"
done

# ---- std.http (v0.4.3: modular HTTP/1.1 client + server, verified HTTPS) ----
check_not() { # description, forbidden substring, actual
    if [[ "$3" != *"$2"* ]]; then echo "ok   - $1"; pass=$((pass+1)); else echo "FAIL - $1"; echo "       must NOT contain: $2"; echo "       got: $3"; fail=$((fail+1)); fi
}
check_re() { # description, ERE, actual
    if [[ "$3" =~ $2 ]]; then echo "ok   - $1"; pass=$((pass+1)); else echo "FAIL - $1"; echo "       expected to match: $2"; echo "       got: $3"; fail=$((fail+1)); fi
}
strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }

# pure functions: no server needed
hp() { printf 'import std.http;\nfn main() {\n%s\n}\n' "$1" > /tmp/kc_http_p.kc; $BIN run /tmp/kc_http_p.kc 2>&1 | strip_ansi; }
check "http.url_parse splits everything"      "Url { scheme: https, host: example.com, port: 8443, path: /a/b%20c, query: x=1&y=%C3%A9, userinfo: User:pw" "$(hp 'print(http.url_parse("HTTPS://User:pw@Example.COM:8443/a/b c?x=1&y=é#frag"));')"
check "http.url_parse IPv6 + default port"    "host: ::1, port: 8080" "$(hp 'print(http.url_parse("http://[::1]:8080/p"));')"
check "http.url_parse rejects ftp"            "unsupported URL scheme 'ftp'" "$(hp 'http.url_parse("ftp://x/y"); print(http.last_error());')"
check "http.url_parse rejects bad port"       "port out of range" "$(hp 'http.url_parse("http://h:99999/"); print(http.last_error());')"
check "http.url_join RFC 3986 examples (1)"   "http://a.com/b/c/g http://a.com/b/c/g http://a.com/b/c/g/ http://a.com/g http://g.org/x" "$(hp 'let b = "http://a.com/b/c/d;p?q=1"; print(http.url_join(b,"g"), http.url_join(b,"./g"), http.url_join(b,"g/"), http.url_join(b,"/g"), http.url_join(b,"//g.org/x"));')"
check "http.url_join RFC 3986 examples (2)"   "http://a.com/b/g http://a.com/g http://a.com/g http://a.com/b/c/h" "$(hp 'let b = "http://a.com/b/c/d;p?q=1"; print(http.url_join(b,"../g"), http.url_join(b,"../../g"), http.url_join(b,"../../../g"), http.url_join(b,"g/../h"));')"
check "http.url_join query/fragment refs"     "http://a.com/b/c/d;p?y http://a.com/b/c/g?y http://a.com/b/c/d;p?q=1" "$(hp 'let b = "http://a.com/b/c/d;p?q=1"; print(http.url_join(b,"?y"), http.url_join(b,"g?y"), http.url_join(b,"#s"));')"
check "http.url_encode / url_decode"          "a%20b%26c%3Dd%2F%C3%A9~-_. a b&%zz+A a b" "$(hp 'print(http.url_encode("a b&c=d/é~-_."), http.url_decode("a%20b%26%zz+%41"), http.url_decode("a+b", true));')"
check "http.query_parse"                      "[[a, 1], [b, two words], [c, ], [d, x y]" "$(hp 'print(http.query_parse("?a=1&b=two%20words&c&d=x+y"));')"
check "http.form encodes pairs"               "name=Ka+Ra&n=5&f=1.5&t=true&sym=a%26b%3Dc" "$(hp 'print(http.form([["name","Ka Ra"],["n",5],["f",1.5],["t",true],["sym","a&b=c"]]));')"
check "http.base64 encode/decode"             "TWFu TWE= TQ==  Man Ma M null" "$(hp 'print(http.base64_encode("Man"), http.base64_encode("Ma"), http.base64_encode("M"), http.base64_encode(""), http.base64_decode("TWFu"), http.base64_decode("TWE="), http.base64_decode("TQ"), http.base64_decode("!!"));')"
check "http.status_text"                      "Not Found I'm a teapot []" "$(hp 'print(http.status_text(404), http.status_text(418), "[" + http.status_text(999) + "]");')"
check "http rejects header injection"         "header injection refused" "$(hp 'http.get("http://x", ["X: a\r\nInjected: 1"]);')"
check "http rejects malformed header"         "must look like \"Name: value\"" "$(hp 'http.get("http://x", ["bad header"]);')"
check "http rejects bad method"               "invalid HTTP method" "$(hp 'http.request("BAD METHOD", "http://x");')"
check "http rejects negative timeout"         "timeout must be >= 0" "$(hp 'http.get("http://x", null, -1);')"
check "http rejects unknown option"           "unknown option 'nope'" "$(hp 'http.set_option(http.client(), "nope", 1);')"
check "http closed client is fatal"           "already closed" "$(hp 'let c = http.client(); http.close(c); http.close(c); http.get(c, "http://x");')"
check "http comptime allows pure helpers"     "a%20b a=b eA==" "$(printf 'import std.http;\ncomptime { let u = http.url_encode("a b"); let q = http.form([["a","b"]]); let b = http.base64_encode("x"); }\nfn main() { print(u, q, b); }\n' > /tmp/kc_http_ct.kc; $BIN run /tmp/kc_http_ct.kc 2>&1)"
for fn in 'http.get("http://x")' 'http.client()' 'http.listen(0)'; do
    check "http comptime blocks $fn" "not allowed inside a comptime block" "$(printf 'import std.http;\ncomptime { let x = %s; }\nfn main() {}\n' "$fn" > /tmp/kc_http_ct2.kc; $BIN run /tmp/kc_http_ct2.kc 2>&1 | strip_ansi || true)"
done

if command -v python3 >/dev/null 2>&1; then
    HT_DIR=$(mktemp -d); HT_PIDS=""
    trap 'for p in $HT_PIDS; do kill $p 2>/dev/null || true; done; rm -rf "$HT_DIR"' EXIT
    check "http inflate corpus (158 streams, incl. corrupt/truncated)" "problems: 0" "$(python3 tests/gen_inflate_cases.py > "$HT_DIR/inf.kc" && $BIN run "$HT_DIR/inf.kc" 2>&1 | tail -1)"

    start_server() { # portfile [cert key]
        python3 tests/http_test_server.py "$@" >/dev/null 2>&1 & HT_PIDS="$HT_PIDS $!"
        for _ in $(seq 1 50); do [[ -s "$1" ]] && break; sleep 0.1; done
    }
    start_server "$HT_DIR/port"
    HT_PORT=$(cat "$HT_DIR/port")
    ht() { printf 'import std.http;\nimport std.system;\nimport std.io;\nfn main() {\nlet b = "http://127.0.0.1:%s";\n%s\n}\n' "$HT_PORT" "$1" > "$HT_DIR/t.kc"; $BIN run "$HT_DIR/t.kc" 2>&1 | strip_ansi; }

    check "http.get basics"                       "200 OK true 0 false" "$(ht 'let r = http.get(b + "/echo?a=1"); print(r.status, r.reason, r.ok, r.redirects, r.tls);')"
    out=$(ht 'print(http.get(b + "/echo?a=1&b=two").body);')
    check "http.get sends request line + Host + UA" "GET /echo?a=1&b=two" "$out"
    check "http.get default headers"              "User-Agent: KingCobra/" "$out"
    check "http 404 is a response, not a failure" "404 Not Found false status page" "$(ht 'let e = http.get(b + "/status/404"); print(e.status, e.reason, e.ok, e.body);')"
    check "http 204/304 have empty bodies"        "204 [] 304 []" "$(ht 'let a = http.get(b + "/204"); let c = http.get(b + "/304"); print(a.status, "[" + a.body + "]", c.status, "[" + c.body + "]");')"
    check "http.head has no body"                 "200 0 true" "$(ht 'let h = http.head(b + "/echo"); print(h.status, h.length, h.body == "");')"
    out=$(ht 'let p = http.post(b + "/echo", "name=kc&x=1", ["Content-Type: application/x-www-form-urlencoded", "X-Test: hello"]); print(p.body);')
    check "http.post sends body + custom header"  "X-Test: hello" "$out"
    check "http.post computes Content-Length"     "Content-Length: 11" "$out"
    check "http.post body arrives intact"         "name=kc&x=1" "$out"
    check "http.put / patch / delete / options"   "200 200 200 200" "$(ht 'print(http.put(b + "/echo", "d").status, http.patch(b + "/echo", "p").status, http.delete(b + "/echo").status, http.options(b + "/echo").status);')"
    check "http.request lower-case method"        "PUT /echo" "$(ht 'print(http.request("put", b + "/echo", "body!").body);')"
    check "http.header is case-insensitive"       "text/plain | null" "$(ht 'let r = http.get(b + "/echo"); print(http.header(r, "content-type"), "|", http.header(r, "NOPE"));')"
    check "http.header_all returns every value"   "[a, b]" "$(ht 'print(http.header_all(http.get(b + "/headers"), "x-multi"));')"
    check "http chunked body (with extensions+trailers)" "chunked: 260" "$(ht 'print("chunked:", http.get(b + "/chunked").length);')"
    check "http close-delimited body"             "close-delim: 1200" "$(ht 'print("close-delim:", http.get(b + "/close-delimited").length);')"
    check "http large body (1.2 MB)"              "big: 1200000" "$(ht 'print("big:", http.get(b + "/bigbody").length);')"
    check "http truncated body is an error"       "null | http.get: connection closed after 10 of 100 body bytes" "$(ht 'let t = http.get(b + "/truncated"); print(t, "|", http.last_error());')"
    check "http refused connection"               "could not connect to 127.0.0.1:1" "$(hp 'http.get("http://127.0.0.1:1/"); print(http.last_error());')"
    check "http unresolvable host"                "could not resolve host 'no-such-host.invalid'" "$(hp 'http.get("http://no-such-host.invalid/"); print(http.last_error());')"
    check "http no-scheme URL means http://"      "204" "$(ht 'print(http.get("127.0.0.1:" + "'"$HT_PORT"'" + "/204").status);')"

    check "http follows redirects"                "200 3 http://127.0.0.1:$HT_PORT/echo" "$(ht 'let r = http.get(b + "/redirect/3"); print(r.status, r.redirects, r.url);')"
    check "http resolves relative Location"       "200 http://127.0.0.1:$HT_PORT/echo?x=1" "$(ht 'let r = http.get(b + "/rel"); print(r.status, r.url);')"
    check "http redirect loop is bounded"         "too many redirects (more than 10)" "$(ht 'http.get(b + "/loop"); print(http.last_error());')"
    check "http max_redirects=0 fails"            "too many redirects (more than 0)" "$(ht 'let c = http.client(); http.set_option(c, "max_redirects", 0); http.get(c, b + "/redirect/2"); print(http.last_error());')"
    check "http follow_redirects=false returns 3xx" "302 /redirect/1" "$(ht 'let c = http.client(); http.set_option(c, "follow_redirects", false); let r = http.get(c, b + "/redirect/2"); print(r.status, http.header(r, "Location"));')"
    out=$(ht 'let p = http.post(b + "/r303", "data", ["Content-Type: text/plain"]); print(p.body);')
    check "http POST->303 becomes GET"            "GET /echo" "$out"
    check_not "http POST->303 drops body headers" "Content-Type" "$out"
    check "http POST->307 keeps method and body"  "POST /echo" "$(ht 'print(http.post(b + "/r307", "keepme").body);')"
    check "http PUT->308 keeps body"              "again" "$(ht 'print(http.put(b + "/r308", "again").body);')"
    out=$(ht 'print(http.get(b + "/xorigin", ["Authorization: Bearer SECRET", "Cookie: a=b"]).body);')
    check "http cross-origin redirect reaches new host" "Host: localhost:$HT_PORT" "$out"
    check_not "http strips Authorization cross-origin" "Authorization" "$out"
    check_not "http strips Cookie cross-origin"   "Cookie" "$out"
    check "http keeps Authorization same-origin"  "Authorization: Bearer SECRET" "$(ht 'print(http.get(b + "/echo", ["Authorization: Bearer SECRET"]).body);')"

    check "http gzip is decoded"                  "11000 true" "$(ht 'let g = http.get(b + "/gzip"); print(g.length, g.decoded);')"
    check "http gzip large + stored blocks"       "1200000 70000" "$(ht 'print(http.get(b + "/gzip-big").length, http.get(b + "/gzip-stored").length);')"
    check "http deflate (zlib) and raw deflate"   "6500 6000" "$(ht 'print(http.get(b + "/deflate").length, http.get(b + "/rawdeflate").length);')"
    check "http corrupt gzip is an error"         "could not decode the gzip response body" "$(ht 'http.get(b + "/badgzip"); print(http.last_error());')"
    check "http decompress=false leaves raw bytes" "false true gzip true" "$(ht 'let c = http.client(); http.set_option(c, "decompress", false); let r = http.get(c, b + "/gzip"); print(r.decoded, r.length < 200, http.header(r, "content-encoding"), len(http.decompress(r.body)) == 11000);')"
    check "http max_body caps the raw body"       "larger than the limit of 1000 bytes" "$(ht 'let c = http.client(); http.set_option(c, "max_body", 1000); http.get(c, b + "/bigbody"); print(http.last_error());')"
    check "http max_body caps the DECODED size (zip bomb)" "decompressed data exceeds the size limit" "$(ht 'let c = http.client(); http.set_option(c, "max_body", 1000); http.get(c, b + "/gzip"); print(http.last_error());')"

    check "http per-call timeout"                 "null | http.get: timed out waiting for the server | true" "$(ht 'let t = system.monotonic(); let r = http.get(b + "/slow?t=3", null, 0.4); print(r, "|", http.last_error(), "|", system.monotonic() - t < 1.5);')"
    check "http timeout on a reused connection is not retried" "timed out waiting for the server" "$(ht 'let c = http.client(); http.set_option(c, "timeout", 0.3); http.get(c, b + "/gzip"); http.get(c, b + "/slow?t=2"); print(http.last_error());')"
    check "http response inside the timeout"      "slow done" "$(ht 'print(http.get(b + "/slow?t=0.2", null, 5).body);')"

    check_re "http keep-alive reuses one connection" "conn=([0-9]+) req=1 \| conn=\1 req=2 \| conn=\1 req=3" "$(ht 'let c = http.client(); print(http.get(c, b + "/conn").body, "|", http.get(c, b + "/conn").body, "|", http.get(c, b + "/conn").body);')"
    out=$(ht 'print(http.get(b + "/conn").body, "|", http.get(b + "/conn").body);')
    check_re "http one-shot calls use fresh connections" "req=1 \| conn=[0-9]+ req=1" "$out"
    check "http stale keep-alive connection is retried" "200 200" "$(ht 'let c = http.client(); http.get(c, b + "/drop-after-first"); print(http.get(c, b + "/conn").status, http.get(c, b + "/conn").status);')"

    out=$(ht 'let c = http.client(); http.set_option(c, "cookies", true); http.get(c, b + "/cookie-set"); print("JAR", http.cookies(c)); print("SENT", http.get(c, b + "/echo").body); http.get(c, b + "/cookie-del"); print("AFTER", http.cookies(c)); http.clear_cookies(c); print("CLEARED", http.cookies(c));')
    check "http cookie jar stores Set-Cookie"     "sid=abc123; domain=127.0.0.1; path=/" "$out"
    check_not "http cookie jar rejects foreign Domain" "evil" "$out"
    check "http sends cookies, longest path first" "Cookie: pref=dark; sid=abc123; gone=1" "$out"
    check "http cookie deleted by Max-Age=0"      "AFTER [sid=abc123" "$out"
    check_not "http cookie deleted really gone"   "AFTER [sid=abc123; domain=127.0.0.1; path=/, pref=dark; domain=127.0.0.1; path=/echo; expires, gone" "$out"
    check "http clear_cookies"                    "CLEARED []" "$out"
    out=$(ht 'let c = http.client(); http.set_option(c, "bearer", "tok123"); http.set_option(c, "header", "X-Api: kc"); print(http.get(c, b + "/echo").body);')
    check "http client bearer + default header"   "Authorization: Bearer tok123" "$out"
    check "http client default header sent"       "X-Api: kc" "$out"
    check "http client basic auth"                "Authorization: Basic dXNlcjpwQHNz" "$(ht 'let c = http.client(); http.set_option(c, "basic_auth", "user:p@ss"); print(http.get(c, b + "/echo").body);')"
    out=$(ht 'let u = http.get("http://al%40ice:secret@127.0.0.1:'"$HT_PORT"'/echo"); print(u.body); print("URL", u.url);')
    check "http userinfo becomes Basic auth"      "Authorization: Basic YWxAaWNlOnNlY3JldA==" "$out"
    check "http final url hides credentials"      "URL http://127.0.0.1:$HT_PORT/echo" "$out"
    check "http user header overrides defaults"   "User-Agent: mine/1" "$(ht 'print(http.get(b + "/echo", ["User-Agent: mine/1"]).body);')"

    out=$(ht 'let d = system.make_temp_dir("htd"); let f = system.path_join(d, "o.bin"); let r = http.download(b + "/bigbody", f); print("DL", r.status, r.length, r.body == "", system.stat(f).size, system.list_dir(d)); let nf = http.download(b + "/status/404", system.path_join(d, "n.bin")); print("NF", nf.status, nf.body, system.list_dir(d)); let t = http.download(b + "/truncated", system.path_join(d, "t.bin")); print("TR", t, system.list_dir(d)); system.rmdir(d, true);')
    check "http.download streams to a file"       "DL 200 1200000 true 1200000 [o.bin]" "$out"
    check "http.download 404 writes nothing"      "NF 404 status page [o.bin]" "$out"
    check "http.download failure leaves no partial file" "TR null [o.bin]" "$out"

    # ---- HTTPS (needs openssl CLI + libssl at run time; skipped otherwise) ----
    if command -v openssl >/dev/null 2>&1 && openssl req -x509 -newkey rsa:2048 -nodes -keyout "$HT_DIR/k.pem" -out "$HT_DIR/c.pem" -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
       && openssl req -x509 -newkey rsa:2048 -nodes -keyout "$HT_DIR/k2.pem" -out "$HT_DIR/c2.pem" -days 2 -subj "/CN=other.example" -addext "subjectAltName=DNS:other.example" >/dev/null 2>&1; then
        start_server "$HT_DIR/sport" "$HT_DIR/c.pem" "$HT_DIR/k.pem"; start_server "$HT_DIR/sport2" "$HT_DIR/c2.pem" "$HT_DIR/k2.pem"
        S1=$(cat "$HT_DIR/sport"); S2=$(cat "$HT_DIR/sport2")
        hs() { printf 'import std.http;\nfn main() {\nlet s = "https://127.0.0.1:%s"; let s2 = "https://127.0.0.1:%s"; let c = http.client(); http.set_option(c, "ca_file", "%s");\n%s\n}\n' "$S1" "$S2" "$HT_DIR/c.pem" "$1" > "$HT_DIR/ts.kc"; $BIN run "$HT_DIR/ts.kc" 2>&1 | strip_ansi; }
        probe=$(hs 'http.get(c, s + "/204"); print(http.last_error());')
        if [[ "$probe" == *"needs the OpenSSL library"* ]]; then
            echo "skip - https tests (libssl not found on this machine)"
        else
            check "https verifies by default (self-signed rejected)" "certificate verification failed: self-signed certificate" "$(hs 'http.get(s + "/echo"); print(http.last_error());')"
            check "https with ca_file succeeds"       "200 true https://127.0.0.1:$S1/echo" "$(hs 'let r = http.get(c, s + "/echo"); print(r.status, r.tls, r.url);')"
            check "https host name (SNI + DNS SAN)"   "204" "$(hs 'print(http.get(c, "https://localhost:" + "'"$S1"'" + "/204").status);')"
            check "https wrong CA is rejected"        "certificate verification failed" "$(hs 'let w = http.client(); http.set_option(w, "ca_file", "'"$HT_DIR"'/c2.pem"); http.get(w, s + "/echo"); print(http.last_error());')"
            check "https IP mismatch is rejected"     "IP address mismatch" "$(hs 'let w = http.client(); http.set_option(w, "ca_file", "'"$HT_DIR"'/c2.pem"); http.get(w, s2 + "/echo"); print(http.last_error());')"
            check "https verify=false opts out"       "204 true" "$(hs 'let i = http.client(); http.set_option(i, "verify", false); let r = http.get(i, s2 + "/204"); print(r.status, r.tls);')"
            check "https missing CA file"             "could not load CA bundle" "$(hs 'let w = http.client(); http.set_option(w, "ca_file", "/nonexistent.pem"); http.get(w, s + "/echo"); print(http.last_error());')"
            check_re "https keep-alive over TLS"      "conn=([0-9]+) req=[0-9]+ \| conn=\1 req=[0-9]+" "$(hs 'print(http.get(c, s + "/conn").body, "|", http.get(c, s + "/conn").body);')"
            check "https large + gzip bodies"         "1200000 1200000" "$(hs 'print(http.get(c, s + "/bigbody").length, http.get(c, s + "/gzip-big").length);')"
            check "https POST body"                   "secret-body" "$(hs 'print(http.post(c, s + "/echo", "secret-body").body);')"
            check "https timeout"                     "timed out waiting for the server" "$(hs 'http.get(c, s + "/slow?t=2", null, 0.4); print(http.last_error());')"
            check "https redirect to http is refused" "refusing to follow a redirect from https to plain http" "$(hs 'http.get(c, s + "/to-http"); print(http.last_error());')"
            check "plain http to an https port fails cleanly" "null" "$(hs 'print(http.get("http://127.0.0.1:" + "'"$S1"'" + "/echo", null, 3));')"
        fi
    else
        echo "skip - https tests (openssl command not available)"
    fi

    # ---- the KingCobra HTTP server, probed by a standard Python client ----
    rm -f "$HT_DIR/kcport"
    KC_PORT_FILE="$HT_DIR/kcport" $BIN run tests/http_server_fixture.kc > "$HT_DIR/kcsrv.log" 2>&1 & HT_PIDS="$HT_PIDS $!"
    for _ in $(seq 1 50); do [[ -s "$HT_DIR/kcport" ]] && break; sleep 0.1; done
    probe=$(python3 tests/http_server_probe.py "$(cat "$HT_DIR/kcport")" 2>&1 || true)
    sleep 0.3
    check "http server: GET + query"              "get: 200 OK | 'Hello name=world&x=1'" "$probe"
    check "http server: POST body + response headers" "post hdr: 9 application/octet-stream close KingCobra/" "$probe"
    check "http server: POST echoes the body"     "post: 201 Created | 'post body'" "$probe"
    check "http server: chunked request body"     "chunked: 201 Created | 'chunk-one,chunk-two'" "$probe"
    check "http server: HEAD has length, no body" "head: 200 7 0" "$probe"
    check "http server: 204"                      "204: 204 0" "$probe"
    check "http server: 404 from the script"      "404: 404 Not Found | 'no /nothing'" "$probe"
    check "http server: percent-decoded path"     "encoded: 200 OK | 'decoded:/spaced path'" "$probe"
    check "http server: garbage gets 400"         "garbage: HTTP/1.1 400 Bad Request" "$probe"
    check "http server: HTTP/2 request gets 505"  "http2: HTTP/1.1 505 HTTP Version Not Supported" "$probe"
    check "http server: script sees each request"  "req: POST /echo | /echo |  | body 9" "$(strip_ansi < "$HT_DIR/kcsrv.log")"
    check "http server: accept(timeout) returns null when idle" "idle: null" "$(printf 'import std.http;\nfn main() { let s = http.listen(0); print("idle:", http.accept(s, 0.2)); http.close(s); }\n' > "$HT_DIR/idle.kc"; $BIN run "$HT_DIR/idle.kc" 2>&1 | strip_ansi)"
    check "http server: fixture finished cleanly" "fixture done" "$(strip_ansi < "$HT_DIR/kcsrv.log")"
    # our own client against our own server
    rm -f "$HT_DIR/kcport"; KC_PORT_FILE="$HT_DIR/kcport" $BIN run tests/http_server_fixture.kc > "$HT_DIR/kcsrv2.log" 2>&1 & HT_PIDS="$HT_PIDS $!"
    for _ in $(seq 1 50); do [[ -s "$HT_DIR/kcport" ]] && break; sleep 0.1; done
    KP=$(cat "$HT_DIR/kcport")
    check "http client talks to http server (round trip)" "201 Created round-trip-body" "$(printf 'import std.http;\nfn main() { let r = http.post("http://127.0.0.1:%s/echo", "round-trip-body"); print(r.status, r.reason, r.body); }\n' "$KP" > "$HT_DIR/rt.kc"; $BIN run "$HT_DIR/rt.kc" 2>&1 | strip_ansi)"
    for pid in $HT_PIDS; do kill $pid 2>/dev/null || true; done; HT_PIDS=""
else
    echo "skip - std.http server/client tests (python3 not available)"
fi

# ---- std.json / std.csv / std.fbin ----
out=$($BIN run examples/std_json.kc)
check "json parse + field access" "name: Ada | age: 36 | missing: null" "$out"
check "json exact int vs float typing" "types: int float array object" "$out"
check "json set_path creates intermediates" '"geo":{"lat":51.5}' "$out"
check "json sort_keys output" '{"address":{"city":"London"' "$out"
check "json pointer + dotted path" "path: London code math default" "$out"
check "json precise error position" "error: line 1, column 8: trailing comma before '}'" "$out"
check "json try_parse fallback" "fallback: BAD" "$out"
check "json lenient comments/trailing commas" "lenient: [1, 2, 3]" "$out"
check "json query filter" "cheap: [A, B]" "$out"
check "json query paths" 'paths: [$.book[0].price, $.book[1].price, $.book[2].price]' "$out"
check "json diff -> patch round trip" "patch reproduces b: true" "$out"
check "json merge_patch (RFC 7396)" '{"b":{"c":[1,2,3],"d":"new"}}' "$out"
check "json schema reports every violation" '/age: -1 is less than the minimum 0' "$out"
check "json file round trip" "file round-trip: true" "$out"
check "json formatter preserves numbers" '{"x":2.50,"big":12345678901234567890123}' "$out"

json_run() { printf 'import std.json;\nfn main() { %s }\n' "$1" > /tmp/kc_json_t.kc; $BIN run /tmp/kc_json_t.kc 2>&1; }
check "json float keeps float-ness" "3.0" "$(json_run 'print(json.stringify(3.0));')"
check "json unicode escapes + surrogate pairs" "é😀" "$(json_run 'print(json.parse("\"\\u00e9\\ud83d\\ude00\""));')"
check "json ascii output escapes" '"\u00e9"' "$(json_run 'print(json.escape("é", true));')"
check "json rejects lone surrogate" "high surrogate" "$(json_run 'json.parse("\"\\ud83d\"");')"
check "json rejects leading zeros" "leading zeros" "$(json_run 'json.parse("[01]");')"
check "json depth limit" "nesting too deep" "$(json_run 'var s = ""; var i = 0; while (i < 600) { s = s + "["; i = i + 1; } json.parse(s);')"
check "json duplicate key: last wins" '{"a":2}' "$(json_run 'print(json.stringify(json.parse("{\"a\":1,\"a\":2}")));')"
check "json dup_error flag" "duplicate object key" "$(json_run 'json.parse("{\"a\":1,\"a\":2}", "dup_error");')"
check "json unknown flag is a clear error" "unknown flag" "$(json_run 'json.parse("1", "bogus");')"
check "json NaN refused unless nan_null" "cannot serialize NaN" "$(json_run 'print(json.stringify(json.parse("NaN", "nan")));')"
check "object index on declared struct errors" "has no field" "$(printf 'struct P { x: int }\nfn main() { let p = P { x: 1 }; print(p["y"]); }\n' > /tmp/kc_p.kc; $BIN run /tmp/kc_p.kc 2>&1)"

out=$($BIN run examples/std_csv.kc)
check "csv quoted fields + escaped quotes" 'quoted: likes "quotes"' "$out"
check "csv header+infer typing" "typed: int float null" "$out"
check "csv sort_by desc, numeric" "sorted: [Dee, Ada, Bob, Cy]" "$out"
check "csv stats" "count: 3, sum: 265.5, mean: 88.5" "$out"
check "csv filter with a closure" "red team: 2" "$out"
check "csv writer quoting" '2,"a,b"' "$out"
check "csv delimiter sniffing" "delimiter: |, columns: 3, header: true" "$out"
check "csv streaming reader/writer" "total score: 265.5 | data rows: 4" "$out"
check "csv try_parse on malformed input" "bad input: BAD" "$out"
check "csv lenient ragged rows" "col3: 3" "$out"

csv_run() { printf 'import std.csv;\nfn main() { %s }\n' "$1" > /tmp/kc_csv_t.kc; $BIN run /tmp/kc_csv_t.kc 2>&1; }
printf '\xEF\xBB\xBFa,b\r\n\r\n1,2\r\n' > /tmp/kc_bom.csv
check "csv CRLF + BOM + blank lines" "[[a, b], [1, 2]]" "$(csv_run 'print(csv.parse_file("/tmp/kc_bom.csv"));')"
check "csv strict rejects ragged rows" "has 3 field(s), but the header has 2" "$(csv_run 'csv.parse("a,b\n1,2,3", "header strict");')"
check "csv unterminated quote error" "unterminated quoted field" "$(csv_run 'csv.parse("a,\"b");')"
check "csv leading zeros stay strings under infer" "[007, 1.5, true]" "$(csv_run 'print(csv.parse("007,1.5,true", "infer")[0]);')"
check "csv quoted numbers stay strings under infer" "[12, 7]" "$(csv_run 'print(csv.parse("\"12\",7", "infer")[0]);')"
check "csv comment lines" "[[a], [b]]" "$(csv_run 'print(csv.parse("a\n# skip\nb", "comment=#"));')"
check "csv empty string vs null round trip" ',"",' "$(csv_run 'print(csv.format_line([null, "", null]));')"

out=$($BIN run examples/std_fbin.kc)
check "fbin round trip magic/version" "magic: KCBN version: 2" "$out"
check "fbin u32 above i32 range" "u32: 4000000000" "$out"
check "fbin per-call big endian" "i32 be: -123456" "$out"
check "fbin pstring + cstring" "pstring: length-prefixed | cstring: c-string" "$out"
check "fbin varints (LEB128 + zigzag)" "varints: 300 -1000" "$out"
check "fbin bit fields" "bits: 5 1 255" "$out"
check "fbin pack/unpack" "unpacked: [ABCD, 513, 70000, 2.5, true]" "$out"
check "fbin network-order pack" "network order: 01020304 [1, 2, 772]" "$out"
check "fbin file write/read/crc32" "file u32: 3735928559 size: 8 crc32: 2175562768" "$out"
check "fbin magic check + find" "magic check: true find: 4" "$out"
check "fbin base64" "base64: aGVsbG8gd29ybGQ= hello" "$out"
check "fbin sha256 known vector" "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" "$out"
check "fbin crc32 known vector + u64" "crc32(123456789): 3421780262 u64: 18446744073709551615" "$out"

fbin_run() { printf 'import std.fbin;\nfn main() { %s }\n' "$1" > /tmp/kc_fbin_t.kc; $BIN run /tmp/kc_fbin_t.kc 2>&1; }
check "fbin read past end is a clear error" "unexpected end of data (wanted 4 byte(s) at offset 0, only 2 left)" "$(fbin_run 'fbin.read_u32(fbin.from_hex("0102"));')"
check "fbin out-of-range write is rejected" "does not fit in an unsigned 8-bit" "$(fbin_run 'fbin.write_u8(fbin.buffer(), 256);')"
check "fbin use-after-close" "already closed" "$(fbin_run 'let b = fbin.buffer(); fbin.close(b); fbin.size(b);')"
check "fbin read-only stream refuses writes" "not opened for writing" "$(fbin_run 'fbin.write_u8(fbin.open("/tmp/kc_fbin_t.kc"), 1);')"
check "fbin missing file is a clear error" "cannot open" "$(fbin_run 'fbin.open("/nonexistent/x.bin");')"
check "fbin try_open returns null" "null" "$(fbin_run 'print(fbin.try_open("/nonexistent/x.bin"));')"
check "fbin seeking past end zero-fills" "000000ff" "$(fbin_run 'let b = fbin.buffer(); fbin.seek(b, 3); fbin.write_u8(b, 255); print(fbin.to_hex(b));')"
check "fbin hexdump" "|KCBN|" "$(fbin_run 'print(fbin.hexdump(fbin.from_string("KCBN")));')"
rm -f /tmp/kingcobra_json_demo.json /tmp/kingcobra_json_demo.jsonl /tmp/kingcobra_csv_demo.csv /tmp/kingcobra_fbin_demo.bin

# ---- cross-module: std.json with std.system / std.http ----
check "json.stringify serialises system.info()"  '"os":"linux"' "$(printf 'import std.json;\nimport std.system;\nfn main() { print(json.stringify(system.info())); }\n' > /tmp/kc_x1.kc; $BIN run /tmp/kc_x1.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
check "json.stringify serialises a RunResult"    '"code":2,"stdout":"hi\n","stderr":"oops\n","timed_out":false' "$(printf 'import std.json;\nimport std.system;\nfn main() { print(json.stringify(system.run("echo hi; echo oops 1>&2; exit 2"))); }\n' > /tmp/kc_x2.kc; $BIN run /tmp/kc_x2.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
check "json.pretty serialises http.url_parse()"  '"host": "example.com"' "$(printf 'import std.json;\nimport std.http;\nfn main() { print(json.pretty(http.url_parse("https://u@example.com:8443/a?x=1"))); }\n' > /tmp/kc_x3.kc; $BIN run /tmp/kc_x3.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
XD=$(mktemp -d)
cat > "$XD/srv.kc" <<'KCEOF'
import std.http;
import std.json;
import std.io;
import std.system;
fn main() {
  let s = http.listen(0);
  io.write_file(system.getenv("KC_PORT_FILE"), "" + http.server_port(s));
  let req = http.accept(s, 10);
  let payload = json.parse(req.body);
  let reply = json.object([["got", json.get(payload, "name")], ["double", json.get(payload, "n") * 2]]);
  http.respond(req, 200, json.stringify(reply), ["Content-Type: application/json"]);
  http.close(s);
}
KCEOF
cat > "$XD/cli.kc" <<'KCEOF'
import std.http;
import std.json;
import std.io;
import std.system;
fn main() {
  let body = json.stringify(json.object([["name", "kc"], ["n", 21]]));
  let r = http.post("http://127.0.0.1:" + io.read_file(system.getenv("KC_PORT_FILE")), body, ["Content-Type: application/json"]);
  print(r.status, http.header(r, "content-type"), json.get(json.parse(r.body), "double"), json.get(json.parse(r.body), "got"));
}
KCEOF
KC_PORT_FILE="$XD/port" $BIN run "$XD/srv.kc" >/dev/null 2>&1 & XPID=$!
for _ in $(seq 1 50); do [[ -s "$XD/port" ]] && break; sleep 0.1; done
check "http + json round trip (client <-> server)" "200 application/json 42 kc" "$(KC_PORT_FILE="$XD/port" $BIN run "$XD/cli.kc" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
kill $XPID 2>/dev/null || true; rm -rf "$XD"

# ---- pointers, buffers, offsets, bitwise operators (v0.5.0) ----
out=$($BIN run examples/pointers.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
check "pointers example: packed vs padded layout" "packed: 16 bytes, length at 5 , seq at 8" "$out"
check "pointers example: padded layout" "padded: 24 bytes, length at 6 , seq at 16" "$out"
check "pointers example: struct pointer over a buffer" "magic   = 12630551" "$out"
check "pointers example: (h + 1) steps one whole struct" "payload = hello" "$out"
check "pointers example: write through -> visible in buffer" "buffer now says length = 105" "$out"
check "pointers example: linked list in raw memory" "list (newest first): [16, 9, 4, 1]" "$out"
check "pointers example: bump allocator offsets" "offsets: 0 4 8  top: 16" "$out"
check "pointers example: &variable" "x after two bumps through a pointer: 12" "$out"
check "pointers example: &array[i] + arithmetic" "array: [5, 60, 7]  via pointer arithmetic: 7" "$out"

ptr_run() { printf '%s\n' "$1" > /tmp/kc_ptr_t.kc; $BIN run /tmp/kc_ptr_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }
check "bitwise & | ^ ~ << >> >>>" "15 48 6 -1 1024 -4 15" "$(ptr_run 'print(0b1010 | 0b0101, 0xF0 & 0x3C, 5 ^ 3, ~0, 1 << 10, -16 >> 2, -1 >>> 60);')"
check "bitwise binds tighter than == (Rust order)" "true" "$(ptr_run 'let f = 6; print(f & 2 == 2);')"
check "bitwise compound assignment" "18" "$(ptr_run 'var m = 1; m <<= 4; m |= 3; m &= 0x1F; m ^= 1; print(m);')"
check "shift count out of range" "shift count 64 is out of range" "$(ptr_run 'print(1 << 64);')"
check "typed pointer: scaled arithmetic + indexing" "100 200 300 -1" "$(ptr_run 'let q = alloc(16) as *i32; *q = 100; *(q + 1) = 200; q[2] = 300; q[3] = -1; print(*q, q[1], *(q + 2), q[3]);')"
check "pointer difference in elements" "3" "$(ptr_run 'let q = alloc(16) as *i32; print((q + 3) - q);')"
check "reinterpret cast (i32 -1 as u32)" "4294967295" "$(ptr_run 'let q = alloc(16) as *i32; q[0] = -1; print((q as *u32)[0]);')"
check "little-endian byte view of an i32" "1 1 0 0" "$(ptr_run 'let q = alloc(8) as *i32; q[0] = 257; let b = q as *u8; print(b[0], b[1], b[2], b[3]);')"
check "integer casts truncate like C" "44 255 -1 4294967295" "$(ptr_run 'print(300 as u8, -1 as u8, 255 as i8, -1 as u32);')"
check "float->int cast truncates toward zero" "3 -3" "$(ptr_run 'print(3.9 as i32, -3.9 as i32);')"
check "f32 storage rounds to single precision like C" "false true" "$(ptr_run 'let p = alloc_array("f32", 1); p[0] = 0.1; let d = p[0]; print(d == 0.1, d > 0.1);')"
check "alloc_array element size" "8 12" "$(ptr_run 'let p = alloc_array("i32", 3); print(p.limit / 3 * 2, p.limit);')"
check "sizeof / alignof basics" "1 2 8 8 16 4" "$(ptr_run 'print(sizeof(u8), sizeof(i16), sizeof(f64), sizeof(*u8), sizeof(u8[16]), alignof(i32));')"
check "struct layout: natural alignment" "16 8 4 6 8" "$(ptr_run 'struct H { magic: u32, kind: u8, len: u16, id: u64 } print(sizeof(H), alignof(H), offsetof(H, kind), offsetof(H, len), offsetof(H, id));')"
check "struct layout: packed" "15 1 5 7" "$(ptr_run 'packed struct H { magic: u32, kind: u8, len: u16, id: u64 } print(sizeof(H), alignof(H), offsetof(H, len), offsetof(H, id));')"
check "struct via pointer: -> read/write/compound assign" "3405691582 7 514" "$(ptr_run 'struct H { magic: u32, kind: u8, len: u16 } let h = alloc(sizeof(H)) as *H; h->magic = 0xCAFEBABE; h->kind = 7; h->len = 513; h->len += 1; print(h->magic, h->kind, h->len);')"
check "struct deref copies out a struct value" "3405691582 513" "$(ptr_run 'struct H { magic: u32, len: u16 } let h = alloc(sizeof(H)) as *H; h->magic = 0xCAFEBABE; h->len = 513; let c = *h; h->len = 1; print(c.magic, c.len);')"
check "nested struct + array fields via pointer" "7 -9 90 3 18" "$(ptr_run 'struct Pt { x: i16, y: i16 } struct Poly { n: u8, pts: Pt[3], tag: u8[4] } let p = alloc(sizeof(Poly)) as *Poly; p->n = 3; p->pts[1].x = 7; p->pts[2].y = -9; p->tag[3] = 90; print(p->pts[1].x, p->pts[2].y, p->tag[3], p->n, sizeof(Poly));')"
check "array-field pointer is bounded to the field" "valid range 14..18" "$(ptr_run 'struct Pt { x: i16, y: i16 } struct Poly { n: u8, pts: Pt[3], tag: u8[4] } let p = alloc(sizeof(Poly)) as *Poly; p->tag[4] = 1;')"
check "self-referential struct (linked list)" "[40, 30, 20, 10, 0] 100" "$(ptr_run 'struct Node { val: i32, next: *Node } var head: *Node = null; for (i in range(5)) { let n = alloc(sizeof(Node)) as *Node; n->val = i * 10; n->next = head; head = n; } var c = head; var s = 0; var o = []; while (c != null) { o.push(c->val); s += c->val; c = c->next; } print(o, s);')"
check "pointer stored in memory resolves back to a checked pointer" "0 7 false" "$(ptr_run 'struct Node { v: i32, next: *Node } let a = alloc(sizeof(Node)) as *Node; let b = alloc(sizeof(Node)) as *Node; b->v = 7; a->next = b; print(a->v, a->next->v, a->next == null);')"
check "error: out-of-bounds read" "out of bounds (valid range 0..8" "$(ptr_run 'let p = alloc(8) as *i32; print(p[2]);')"
check "error: negative index" "at offset -4 is out of bounds" "$(ptr_run 'let p = alloc(8) as *i32; p[-1] = 1;')"
check "error: use after free" "use after free" "$(ptr_run 'let p = alloc(8); free(p); print(p[0]);')"
check "error: double free" "double free" "$(ptr_run 'let p = alloc(8); free(p); free(p);')"
check "error: free of interior pointer" "not the start of the allocation" "$(ptr_run 'let p = alloc(8); free(p + 1);')"
check "error: null dereference" "null pointer dereference" "$(ptr_run 'let p = null as *i32; print(*p);')"
check "error: deref *void" "cannot dereference a *void pointer" "$(ptr_run 'let p = alloc(8) as *void; print(*p);')"
check "error: float stored into int location" "cannot store a float into an integer location" "$(ptr_run 'let p = alloc(4) as *i32; *p = 1.5;')"
check "error: pointers into different allocations" "different allocations" "$(ptr_run 'let a = alloc(4) as *i32; let b = alloc(4) as *i32; print(a - b);')"
check "error: write through pointer to immutable let" "immutable variable" "$(ptr_run 'let x = 5; let p = &x; *p = 6;')"
check "&var: read-modify-write through a pointer" "16 16" "$(ptr_run 'var x = 5; let p = &x; *p = 6; *p += 10; print(x, *p);')"
check "&struct.field and p->field on a plain struct" "20 7 7" "$(ptr_run 'struct P { x: int, y: int } var s = P { x: 1, y: 2 }; let py = &s.y; *py = 20; let ps = &s; ps->x = 7; print(s.y, s.x, ps->x);')"
check "error: &array[i] after the array shrank" "out of bounds (array length 1)" "$(ptr_run 'var a = [1,2,3]; let p = &a[1]; a.pop(); a.pop(); print(*p);')"
check "error: forged address needs unsafe" "requires an \`unsafe { }\` block" "$(ptr_run 'let p = ptr_from_addr(4096, "*u8"); print(*p);')"
check "error: int->pointer cast needs unsafe" "forges an address" "$(ptr_run 'let p = 4096 as *u8;')"
check "string pointer is read-only" "write to read-only memory" "$(ptr_run 'let p = "hey" as *u8; print(p[0]); p[0] = 1;')"
check "memcpy rejects overlap, memmove allows it" "overlap" "$(ptr_run 'let p = alloc(16); memcpy(p + 4, p, 8);')"
check "memmove overlap works" "1 2 1 2 3" "$(ptr_run 'let p = alloc(8); p.write_bytes([1,2,3,4,5]); memmove(p + 2, p, 3); print(p[0], p[1], p[2], p[3], p[4]);')"
check "memset / memcmp" "0 -1" "$(ptr_run 'let a = alloc(4); let b = alloc(4); memset(a, 9, 4); memset(b, 9, 4); let c = memcmp(a, b, 4); b[3] = 10; print(c, memcmp(a, b, 4));')"
check "realloc keeps contents and zero-fills growth" "5 0 9" "$(ptr_run 'let p = alloc(4); p[0] = 5; let q = realloc(p, 8); q[7] = 9; print(q[0], q[1], q[7]);')"
check "realloc: pointers into the same block stay checked" "out of bounds" "$(ptr_run 'let p = alloc(16); let q = realloc(p, 4); print(p[8]);')"
check "ptr.limit / ptr.offset" "8 5 3" "$(ptr_run 'let p = alloc(8); print(p.limit, (p + 3).limit, (p + 3).offset);')"
check "align_up / align_down / is_aligned" "16 8 true false" "$(ptr_run 'print(align_up(9, 8), align_down(15, 8), is_aligned(64, 16), is_aligned(65, 16));')"
check "read_cstr stops at NUL" "hi" "$(ptr_run 'let p = alloc(8); p[0] = 104; p[1] = 105; print(p.read_cstr());')"
check "read_cstr is bounded (no NUL -> error)" "no NUL terminator" "$(ptr_run 'let p = alloc(2); p[0] = 1; p[1] = 1; print(p.read_cstr());')"
check "pointer in comptime: addr is forbidden" "not allowed inside a comptime block" "$(ptr_run 'comptime { let p = alloc(8); print(p.addr); }')"
check "alloc is fine in comptime" "ok" "$(ptr_run 'comptime { let p = alloc(8); p[0] = 1; free(p); } print("ok");')"
check "error: struct with a string field has no layout" "has no fixed memory layout" "$(ptr_run 'struct N { v: int, name: string } print(sizeof(N));')"
check "error: struct containing itself by value" "contains itself by value" "$(ptr_run 'struct R { a: u8, r: R } print(sizeof(R));')"
check "static check: *i32 vs *u8 mismatch" "declared *u8, but initializer is *i32" "$(ptr_run 'let x: *u8 = alloc(4) as *i32;')"
check "static check: sized int annotations accepted" "300" "$(ptr_run 'let x: u8 = 300; print(x);')"
check "static check: pointer arithmetic keeps pointer type" "2" "$(ptr_run 'let p: *i32 = alloc(16) as *i32; let q: *i32 = p + 2; let n: int = q - p; print(n);')"

check "buffer typed accessors + endianness" "3735928559 4660 13330 3.5" "$(ptr_run 'let b = buffer(16); b.put_u32(0, 0xDEADBEEF); b.put_u16_be(4, 0x1234); b.put_f64(8, 3.5); print(b.get_u32(0), b.get_u16_be(4), b.get_u16(4), b.get_f64(8));')"
check "buffer hex" "efbeadde12340000" "$(ptr_run 'let b = buffer(8); b.put_u32(0, 0xDEADBEEF); b.put_u16_be(4, 0x1234); print(b.hex());')"
check "buffer cursor read/write" "1 2 3 7" "$(ptr_run 'let w = buffer(8); w.write_u8(1); w.write_u16(2); w.write_u32_be(3); w.rewind(); print(w.read_u8(), w.read_u16(), w.read_u32_be(), w.pos);')"
check "buffer index read/write + len()" "255 16 16" "$(ptr_run 'let b = buffer(16); b[15] = 255; print(b[15], b.len, len(b));')"
check "buffer slice shares memory" "42 4" "$(ptr_run 'let b = buffer(8); let s = b.slice(4, 4); s.put_u8(0, 42); print(b.get_u8(4), s.len);')"
check "buffer: strings round trip" "hello world" "$(ptr_run 'let b = buffer_from("hello world"); print(b.to_string());')"
check "buffer: copy_from" "0102" "$(ptr_run 'let a = buffer_from([1, 2]); let b = buffer(4); b.copy_from(a, 0, 1, 2); print(b.slice(1, 2).hex());')"
check "buffer resize grows zero-filled" "7 0 6" "$(ptr_run 'let b = buffer(2); b[0] = 7; b.resize(6); print(b[0], b[5], b.len);')"
check "error: buffer out of bounds" "buffer write of 4 byte(s) at offset 2 is out of bounds (len 4)" "$(ptr_run 'let b = buffer(4); b.put_u32(2, 1);')"
check "error: buffer use after free" "use after free" "$(ptr_run 'let b = buffer(4); b.free(); print(b.len);')"
check "error: slice cannot be freed" "a slice cannot be freed" "$(ptr_run 'let b = buffer(4); b.slice(0, 2).free();')"
check "buffer ptr cast + typed view" "513" "$(ptr_run 'let b = buffer(4); b[0] = 1; b[1] = 2; print((b.ptr as *u16)[0]);')"
check "pointer derived from a slice is limited to the slice" "out of bounds" "$(ptr_run 'let b = buffer(8); let s = b.slice(0, 2); print((s.ptr as *u8)[3]);')"

check "FFI: C malloc/memset/strlen with pointers" "15 65 AAAAAAAAAAAAAAA" "$(ptr_run 'extern fn malloc(n: int) -> *u8 from "libc.so.6"; extern fn memset(p: *u8, v: int, n: int) -> *u8 from "libc.so.6"; extern fn strlen(p: *u8) -> int from "libc.so.6"; unsafe { let c = malloc(16); memset(c, 0x41, 15); c[15] = 0; print(strlen(c), c[0], c.read_cstr()); }')"
check "FFI: buffer handed to C" "0707070707070707" "$(ptr_run 'extern fn memset(p: *u8, v: int, n: int) -> *u8 from "libc.so.6"; unsafe { let b = buffer(8); memset(b, 7, 8); print(b.hex()); }')"
check "FFI: passing a pointer without unsafe is an error" "requires an \`unsafe { }\` block" "$(ptr_run 'extern fn memset(p: *u8, v: int, n: int) -> *u8 from "libc.so.6"; let b = buffer(4); memset(b, 1, 4);')"
check "FFI: C-returned pointer needs unsafe to dereference" "dereferencing a raw pointer" "$(ptr_run 'extern fn malloc(n: int) -> *u8 from "libc.so.6"; let c = malloc(8); print(c[0]);')"
check "JIT + pointers: hot function stays correct" "1486500 1486500" "$(ptr_run 'fn sum(p: *u8, n: int) -> int { var s = 0; var i = 0; while (i < n) { s += p[i]; i += 1; } return s; } fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } let b = alloc(100); for (i in range(100)) { b[i] = i; } var t = 0; for (k in range(300)) { t += sum(b, 100); t += fib(5) & 7; } print(t, 300 * 4950 + 300 * (5 & 7));')"

# ---- v0.5.1 performance work: scope recycling / capture must stay exact ----
cap_run() { printf '%s\n' "$1" > /tmp/kc_cap_t.kc; $BIN run /tmp/kc_cap_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }
check "closures capture each loop iteration's own variable" "[0, 10, 20, 30]" "$(cap_run 'fn main() { var fs = []; for (i in range(4)) { push(fs, fn() -> int { return i * 10; }); } var out = []; for (f in fs) { push(out, f()); } print(out); }')"
check "closure over a let inside a loop body" "100 101 102" "$(cap_run 'fn main() { var gs = []; for (i in range(3)) { let k = i + 100; push(gs, fn() -> int { return k; }); } print(gs[0](), gs[1](), gs[2]()); }')"
check "closure outlives its function frame (independent state)" "3 2" "$(cap_run 'fn main() { fn counter() -> any { var n = 0; return fn() -> int { n += 1; return n; }; } let c1 = counter(); let c2 = counter(); c1(); c1(); c2(); print(c1(), c2()); }')"
check "frames recycled between calls do not disturb captured ones" "4 3 1001000" "$(cap_run 'fn main() { fn counter() -> any { var n = 0; return fn() -> int { n += 1; return n; }; } let c1 = counter(); let c2 = counter(); c1(); c1(); c2(); c1(); c2(); fn churn(a: int) -> int { let t = a + 1; var u = t * 2; return u; } var junk = 0; for (j in range(1000)) { junk += churn(j); } print(c1(), c2(), junk); }')"
check "&variable pointers survive their loop" "1 8 15" "$(cap_run 'fn main() { var ptrs = []; for (i in range(3)) { var cell = i * 7; push(ptrs, &cell); } for (p in ptrs) { *p += 1; } print(*ptrs[0], *ptrs[1], *ptrs[2]); }')"
check "let shadows a parameter" "12" "$(cap_run 'fn sh(x: int) -> int { let x = x + 1; return x * 2; } fn main() { print(sh(5)); }')"
check "let shadows the loop variable inside the body" "[0, 100, 200]" "$(cap_run 'fn main() { var acc = []; for (i in range(3)) { let i = i * 100; push(acc, i); } print(acc); }')"
check "continue / break inside for" "12" "$(cap_run 'fn main() { var s = 0; for (i in range(10)) { if (i == 3) { continue; } if (i == 6) { break; } s += i; } print(s); }')"
check "return from inside a loop inside a function" "2 -1" "$(cap_run 'fn find(a: int[], v: int) -> int { for (i in range(len(a))) { if (a[i] == v) { return i; } } return -1; } fn main() { print(find([5, 6, 7], 7), find([5, 6, 7], 9)); }')"
check "nested loops capture outer and inner variables" "0 1 10 11" "$(cap_run 'fn main() { var hs = []; for (a in range(2)) { for (b in range(2)) { push(hs, fn() -> int { return a * 10 + b; }); } } print(hs[0](), hs[1](), hs[2](), hs[3]()); }')"
check "recursive closure chain" "55" "$(cap_run 'fn mk(d: int) -> any { if (d == 0) { return fn() -> int { return 0; }; } let inner = mk(d - 1); return fn() -> int { return inner() + d; }; } fn main() { print(mk(10)()); }')"
check "block without let runs in its parent scope (assignment visible)" "6" "$(cap_run 'fn main() { var s = 0; var i = 0; while (i < 3) { if (i >= 0) { s += i * 2; } i += 1; } print(s); }')"
check "block with let keeps its own scope (not visible outside)" "undefined variable 'z'" "$(cap_run 'fn main() { if (true) { let z = 1; } print(z); }')"
check "same name in nested scopes stays distinct" "2 1 2 3" "$(cap_run 'fn f(n: int) -> int { let n2 = n + 1; return n2; } fn main() { let x = 1; if (true) { let x = 2; print(x); } print(x, f(1), f(2)); }' | tr '\n' ' ' | sed 's/ $//')"
check "string literal reuse is invisible (immutable)" "ab abc ab" "$(cap_run 'fn main() { var out = []; for (i in range(3)) { var s = "ab"; if (i == 1) { s = s + "c"; } push(out, s); } print(out[0], out[1], out[2]); }')"
check "int + string concat formats negatives and zero" "x-5 y0 7z" "$(cap_run 'fn main() { print("x" + -5, "y" + 0, 7 + "z"); }')"
check "struct field read/write through interned names" "11 7" "$(cap_run 'struct P { x: int, y: int } fn main() { var p = P { x: 1, y: 7 }; p.x = p.x + 10; print(p.x, p.y); }')"
check "array fast path keeps bounds errors" "index 3 out of bounds (len 3)" "$(cap_run 'fn main() { var a = [1,2,3]; print(a[3]); }')"
check "array store fast path keeps bounds errors" "index -1 out of bounds (len 3)" "$(cap_run 'fn main() { var a = [1,2,3]; a[-1] = 0; }')"
check "deep recursion is still a clean error, not a crash" "stack overflow (recursion too deep)" "$(cap_run 'fn d(a: int[], n: int) -> int { return d(a, n + 1) + a[0]; } fn main() { print(d([1], 0)); }')"
check "pointer index/member fast paths still bounds-check" "out of bounds" "$(cap_run 'struct H { a: u8, b: u32 } fn main() { let p = alloc(8) as *H; p->b = 5; print(p->b); let q = alloc(4) as *i32; print(q[1]); }')"

# speed guard: not a benchmark, a tripwire. 2M loop iterations + 1M calls must finish well under
# the time the pre-optimization interpreter needed (~0.5s+); a regression back to per-block
# allocation would blow this limit by a wide margin.
perf_t0=$(date +%s%N)
printf '%s\n' 'fn add(a: int, b: int) -> int { return a + b; } fn main() { var s = 0; var i = 0; while (i < 2000000) { s = add(s, i & 7); i += 1; } for (j in range(2000000)) { s += j; } print(s); }' > /tmp/kc_perf_t.kc
perf_out=$($BIN run /tmp/kc_perf_t.kc 2>&1)
perf_ms=$(( ($(date +%s%N) - perf_t0) / 1000000 ))
check "perf tripwire: 4M loop iterations + 2M calls correct" "2000006000000" "$perf_out"
if [ "$perf_ms" -lt 1500 ]; then check "perf tripwire: finishes in <1.5s (took ${perf_ms}ms)" "ok" "ok"; else check "perf tripwire: finishes in <1.5s (took ${perf_ms}ms)" "ok" "too slow: ${perf_ms}ms"; fi

# ---- v0.6.0: explicit memory management (no GC): free / free_deep / defer / leak tooling ----
mem_run() { printf '%s\n' "$1" > /tmp/kc_mem_t.kc; $BIN run /tmp/kc_mem_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }
check "free a string" "ab5 false true" "$(mem_run 'fn main() { var s = "ab" + 5; print(s, is_freed(s), 1); free(s); print(is_freed(s)); }' | tr '\n' ' ' | sed 's/ab5 false 1 true/ab5 false true/; s/ $//')"
check "use after free: print a freed string" "use after free (this string was freed)" "$(mem_run 'fn main() { var s = "ab" + 5; free(s); print(s); }')"
check "use after free: concat a freed string" "use after free (this string was freed)" "$(mem_run 'fn main() { var s = "ab" + 5; free(s); print(s + "x"); }')"
check "double free: string" "double free (this string was already freed)" "$(mem_run 'fn main() { var s = "ab" + 5; free(s); free(s); }')"
check "double free: array" "double free (this array was already freed)" "$(mem_run 'fn main() { var a = [1]; free(a); free(a); }')"
check "cannot free a string literal" "cannot free a string literal" "$(mem_run 'fn main() { free("lit"); }')"
check "a copy of a literal can be freed, the literal survives" "lit true" "$(mem_run 'fn main() { let l = "lit"; let c = "" + l; free(c); print(l, is_freed(c)); }')"
check "use after free: index a freed array" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1,2,3]; free(a); print(a[0]); }')"
check "use after free: push to a freed array" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1]; free(a); a.push(2); }')"
check "use after free: iterate a freed array" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1]; free(a); for (x in a) { print(x); } }')"
check "use after free: len of a freed array" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1]; free(a); print(len(a)); }')"
check "use after free: store into a freed array" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1]; free(a); a[0] = 1; }')"
check "aliases see the free" "use after free (this array was freed)" "$(mem_run 'fn main() { var a = [1,2]; let b = a; free(a); print(b[1]); }')"
check "use after free: member of a freed struct" "use after free (this struct was freed)" "$(mem_run 'struct P { x: int } fn main() { var p = P { x: 1 }; free(p); print(p.x); }')"
check "use after free: assign a field of a freed struct" "use after free (this struct was freed)" "$(mem_run 'struct P { x: int } fn main() { var p = P { x: 1 }; free(p); p.x = 2; }')"
check "free(null) is a no-op" "ok" "$(mem_run 'fn main() { free(null); print("ok"); }')"
check "free rejects values with no heap storage" "cannot free a value of type int" "$(mem_run 'fn main() { free(5); }')"
check "free_deep frees nested arrays and strings" "true" "$(mem_run 'fn main() { var a = [[1,2],["x" + 1],[3]]; free_deep(a); print(is_freed(a)); }')"
check "free_deep tolerates reference cycles" "true" "$(mem_run 'fn main() { var a = [1]; push(a, a); free_deep(a); print(is_freed(a)); }')"
check "free_deep skips string literals and already-freed members" "true" "$(mem_run 'fn main() { let lit = "k"; var x = "v" + 1; var a = [lit, x, x]; free(x); free_deep(a); print(is_freed(a)); }')"
check "freed memory is reused: 300k alloc/free cycles reserve nothing new" "0" "$(mem_run 'fn main() { let b = mem_stats(); var i = 0; while (i < 300000) { var s = "abc" + i; free(s); i += 1; } let a = mem_stats(); print(a.heap_bytes_reserved - b.heap_bytes_reserved); }')"
check "mem_stats counts frees exactly" "1000" "$(mem_run 'fn main() { let b = mem_stats(); var i = 0; while (i < 1000) { var a = [i]; free(a); i += 1; } let a2 = mem_stats(); print(a2.arrays_freed - b.arrays_freed); }')"
check "without free, objects stay allocated (no GC)" "500" "$(mem_run 'fn main() { let b = mem_stats(); var i = 0; while (i < 500) { var a = [i]; i += 1; } let a2 = mem_stats(); print(a2.arrays_live - b.arrays_live); }')"
check "stale references are still detected after more frees (quarantine)" "true" "$(mem_run 'fn main() { var old = "a" + 1; free(old); var i = 0; while (i < 100) { var t = "x" + i; free(t); i += 1; } print(is_freed(old)); }')"
check "mem_quarantine returns the previous size" "4096" "$(mem_run 'fn main() { print(mem_quarantine(8)); }')"
check "json documents: parse + free_deep leaves nothing behind" "0 0" "$(mem_run 'import std.json as json; fn main() { let b = mem_stats(); var i = 0; while (i < 2000) { var d = json.parse("{\"a\": [1, 2], \"b\": {\"c\": \"d\"}}"); free_deep(d); i += 1; } let a = mem_stats(); print(a.heap_bytes_reserved - b.heap_bytes_reserved, a.arrays_live - b.arrays_live); }')"
check "use after free: a freed json object" "use after free (this object was freed)" "$(mem_run 'import std.json as json; fn main() { var d = json.parse("{\"a\": 1}"); free(d); print(d.a); }')"
check "free still works on alloc()/buffer() blocks" "double free" "$(mem_run 'fn main() { let p = alloc(8); free(p); free(p); }')"

check "defer: LIFO, runs on normal exit" "work 5|B|A|10" "$(mem_run 'fn w(n: int) -> int { defer print("A"); defer { print("B"); } print("work", n); return n * 2; } fn main() { print(w(5)); }' | tr '\n' '|' | sed 's/|$//')"
check "defer: runs on early return" "B|A|-1" "$(mem_run 'fn w(n: int) -> int { defer print("A"); defer print("B"); if (n < 0) { return -1; } return 1; } fn main() { print(w(-1)); }' | tr '\n' '|' | sed 's/|$//')"
check "defer free: return value is computed before the free" "42 0" "$(mem_run 'fn f() -> int { let p = alloc(16) as *i32; defer free(p); p[0] = 21; return p[0] * 2; } fn main() { print(f(), mem_stats().regions_live); }')"
check "defer in a loop body runs every iteration (continue/break too)" "1 3 0" "$(mem_run 'fn main() { var i = 0; var out = []; while (i < 4) { let b = buffer(8); defer b.free(); i += 1; if (i == 2) { continue; } if (i == 4) { break; } push(out, i); } print(out[0], out[1], mem_stats().regions_live); }')"
check "defer: nested blocks run their own defers on exit" "in inner|inner exit|back|outer exit" "$(mem_run 'fn main() { { defer print("outer exit"); { defer print("inner exit"); print("in inner"); } print("back"); } }' | tr '\n' '|' | sed 's/|$//')"
check "defer at top level is an error" "only allowed directly inside a function or block" "$(mem_run 'defer print(1);')"
check "a variable named defer still works" "6" "$(mem_run 'fn main() { var defer = 5; defer = defer + 1; print(defer); }')"

leak_out=$(printf '%s\n' 'fn main() { let a = alloc(1024); let b = buffer(64); let c = alloc(8); free(c); print("done"); }' > /tmp/kc_leak_t.kc; $BIN run --leaks /tmp/kc_leak_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
check "--leaks lists unfreed blocks with their allocation site" "1024 bytes  alloc()       at /tmp/kc_leak_t.kc:1:21" "$leak_out"
check "--leaks lists buffers too" "64 bytes  buffer()      at /tmp/kc_leak_t.kc:1:42" "$leak_out"
check "--leaks does not list freed blocks" "total: 2 block(s), 1088 byte(s)" "$leak_out"
clean_out=$(printf '%s\n' 'fn main() { let p = alloc(8); defer free(p); var s = "a" + 1; free(s); print("ok"); }' > /tmp/kc_clean_t.kc; $BIN run --leaks /tmp/kc_clean_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
check "--leaks reports a clean program as clean" "no leaked alloc()/buffer() blocks" "$clean_out"
check "mem_report() prints the report from inside a program" "memory report" "$(mem_run 'fn main() { mem_report(); }')"

mem_ex=$($BIN run examples/memory.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
check "memory example: 20000 requests with defer cleanup" "handled 20000 requests, total = 120000" "$mem_ex"
check "memory example: early return still runs defers" "negative request: -1" "$mem_ex"
check "memory example: heap does not grow under free" "heap reserved grew by 0 bytes" "$mem_ex"
check "memory example: no live blocks left" "live blocks left: 0" "$mem_ex"
check "memory example: aliases see a free" "is_freed(alias) = true" "$mem_ex"

# ---- REPL (piped stdin: no prompts, expression values are echoed) ----
repl() { printf '%s\n' "$1" | $BIN repl 2>&1 || true; }
out=$(repl '1 + 2'); check "repl: expression echo, optional semicolon" "3" "$out"
out=$(repl $'let x = 10\nx * 3'); check "repl: definitions persist across inputs" "30" "$out"
out=$(repl '"a" + "b"'); check "repl: strings echo quoted" '"ab"' "$out"
out=$(repl '[1, "a", 2.5, true]'); check "repl: arrays echo" '[1, "a", 2.5, true]' "$out"
out=$(repl $'fn f(n: int) -> int {\n  return n * 2;\n}\nf(21)'); check "repl: multi-line function" "42" "$out"
out=$(repl $'struct P { x: int, y: int }\nP { x: 1, y: 2 }'); check "repl: struct declaration + literal" "P { x: 1, y: 2 }" "$out"
out=$(repl $'undefined_thing\n1 + 1'); check "repl: runtime error is recoverable" "undefined variable 'undefined_thing'" "$out"
check "repl: ...and the session keeps going after an error" "2" "$out"
out=$(repl $'fn g(n: int) -> int { return n; }\ng("s")\ng(5)'); check "repl: type error rejected" "expected int, got string" "$out"
check "repl: ...and later inputs still run" "5" "$out"
out=$(repl $'let a = 7\na\n_ + 1'); check "repl: _ holds last value" "8" "$out"
out=$(repl $'fn k(s: string) -> string { return k(s) + "a"; }\nk("x")\n"alive"'); check "repl: runaway recursion is a recoverable error" '"alive"' "$out"
out=$(repl $'fn h(n: int) -> int { return h(n + 1) + 1; }\nh(0)\n"alive2"'); check "repl: runaway JIT recursion is recoverable" '"alive2"' "$out"
out=$(repl $'let v = 1\nv = 99\nv'); check "repl: assignment is not echoed, value updated" "99" "$out"
out=$(repl $'let q = 5\n:vars'); check "repl: :vars lists definitions" "q " "$out"
out=$(repl $':type 3.5'); check "repl: :type" "float" "$out"
printf 'fn libfn() -> int { return 123; }\nfn main() { print("MAIN-RAN"); }\n' > /tmp/kc_repl_lib.kc
out=$(repl $':load /tmp/kc_repl_lib.kc\nlibfn()'); check "repl: :load runs a file into the session" "123" "$out"
if [[ "$out" == *"MAIN-RAN"* ]]; then check "repl: :load does not auto-call main" "no" "main ran"; else check "repl: :load does not auto-call main" "ok" "ok"; fi
out=$(printf "libfn()\n" | $BIN repl /tmp/kc_repl_lib.kc 2>&1 || true); check "repl: 'repl file.kc' preloads" "123" "$out"
out=$(repl $'let z = 1\n:reset\nz'); check "repl: :reset discards definitions" "undefined variable 'z'" "$out"
out=$(repl $'import std.system\nsystem.exit(0)\n"unreachable"'); if [[ "$out" == *"unreachable"* ]]; then check "repl: system.exit leaves the REPL" "no" "$out"; else check "repl: system.exit leaves the REPL" "ok" "ok"; fi
printf 'print(missing);\n' > /tmp/kc_err.kc; $BIN run /tmp/kc_err.kc >/dev/null 2>&1 && rc=0 || rc=$?
check "file mode: runtime error still exits 65" "65" "$rc"

# ---- REPL x v0.6 (pointers, explicit memory, defer, struct layout) ----
repl_file() { printf '%s\n' "$1" | $BIN repl 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true; }
out=$(repl_file 'let p = alloc(16) as *i32
p[0] = 5
p[1] = 7
p[0] + p[1]'); check "repl x pointers: typed memory persists across inputs" "12" "$out"
out=$(repl_file 'let p = alloc(16) as *i32
free(p)
p[0]
"alive"'); check "repl x pointers: use-after-free is recoverable" "use after free (allocation #" "$out"
check "repl x pointers: ...and the session continues" '"alive"' "$out"
out=$(repl_file 'let s = "ab" + 1
free(s)
s
free(s)
"alive"'); check "repl x free: echoing a freed value is a recoverable error" "use after free (this string was freed)" "$out"
check "repl x free: double free is recoverable" "double free (this string was already freed)" "$out"
check "repl x free: session continues after both" '"alive"' "$out"
out=$(repl_file 'let s = "ab" + 1
let arr = [s, "x" + 2]
free(s)
arr'); check "repl x free: a freed element is marked in an echoed array" '[<freed string>, "x2"]' "$out"
out=$(repl_file 'let s = "ab" + 1
free(s)
:vars'); check "repl x free: :vars marks freed values" "<freed string>" "$out"
out=$(repl_file 'defer print(1);
"alive"'); check "repl x defer: top-level defer is a recoverable error" "only allowed directly inside a function or block" "$out"
out=$(repl_file 'fn f() -> int { defer print("cleanup"); return 7; }
f()'); check "repl x defer: defer inside a function runs" "cleanup" "$out"
out=$(repl_file 'struct S { a: u8, b: u32 }
sizeof(S)
struct S { a: u8, b: u8, c: u64 }
sizeof(S)
offsetof(S, c)'); check "repl x layout: redeclared struct gets its new layout" "$(printf '8\n16\n8')" "$out"
out=$(repl_file 'struct S { a: u8, b: u32 }
sizeof(S)
packed struct S { a: u8, b: u32 }
sizeof(S)'); check "repl x layout: redeclared as packed" "$(printf '8\n5')" "$out"
out=$(repl_file 'fn work(n: int) -> int { var s = 0; for (i in range(n)) { let f = fn() -> int { return i * 2; }; s += f(); if (i == 7) { let a = [1]; return a[5]; } } return s; }
fn clean(n: int) -> int { var s = 0; for (i in range(n)) { let f = fn() -> int { return i * 2; }; s += f(); } return s; }
clean(10)
work(20)
clean(10)
clean(100)
work(5)'); check "repl x scope pool: error mid-loop with closures leaves later runs correct" "$(printf '90\n90\n9900\n20')" "$(printf '%s\n' "$out" | grep -v -e 'error:' -e '|')"

# ---- v0.7 stage 1: literals, operators, f-strings, slicing, object literals ----
lang_run() { printf '%s\n' "$1" > /tmp/kc_lang_t.kc; $BIN run /tmp/kc_lang_t.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }
check "exact 64-bit int literals" "9007199254740993 9223372036854775807 -9223372036854775808" "$(lang_run 'fn main() { print(9007199254740993, 9223372036854775807, -9223372036854775808); }')"
check "hex literal is the C bit pattern" "-1 3735928559" "$(lang_run 'fn main() { print(0xFFFFFFFFFFFFFFFF, 0xDEAD_BEEF); }')"
check "digit separators and bases" "1000000 170 15" "$(lang_run 'fn main() { print(1_000_000, 0b1010_1010, 0o17); }')"
check "exponent float literals" "1000 0.0025 100" "$(lang_run 'fn main() { print(1e3, 2.5e-3, 1E+2); }')"
check "char literals are code points" "97 10 65 233 233" "$(lang_run "fn main() { print('a', '\\n', '\\x41', '\\u{e9}', 'é'); }")"
check "string escapes \\x and \\u{}" "A é 😀" "$(lang_run 'fn main() { print("\x41", "\u{e9}", "\u{1F600}"); }')"
check "int literal too large" "integer literal is too large for a 64-bit int" "$(lang_run 'fn main() { print(99999999999999999999999); }')"
check "9223372036854775808 only valid negated" "only -9223372036854775808 is valid" "$(lang_run 'fn main() { print(9223372036854775808); }')"
check "unterminated string is an error" "unterminated string literal" "$(lang_run 'fn main() { print("abc); }')"
check "unterminated block comment is an error" "unterminated block comment" "$(lang_run 'fn main() { /* open print(1); }')"
check "compound *= /= %=" "2.5" "$(lang_run 'fn main() { var x = 10; x *= 3; x /= 4; x %= 5; print(x); }')"
check "ternary" "big 2" "$(lang_run 'fn main() { let n = 7; print(n > 5 ? "big" : "small", n > 9 ? 1 : n > 6 ? 2 : 3); }')"
check "ternary only evaluates one branch" "ok" "$(lang_run 'fn boom() -> int { assert(false, "evaluated"); return 1; } fn main() { let x = true ? 5 : boom(); print("ok"); }')"
check "?? treats only null as missing" "default 0 3" "$(lang_run 'fn main() { let n = null; print(n ?? "default", 0 ?? "x", n ?? n ?? 3); }')"
check "?? short-circuits" "ok" "$(lang_run 'fn boom() -> int { assert(false, "evaluated"); return 1; } fn main() { let x = 5 ?? boom(); print("ok"); }')"
check "in: array, string, object" "true false true true" "$(lang_run 'fn main() { print(2 in [1, 2, 3], 5 in [1, 2, 3], "ell" in "hello", "k" in { k: 1 }); }')"
check "in: wrong container is an error" "cannot use 'in' with a value of type int" "$(lang_run 'fn main() { print(1 in 5); }')"
check "object literal" "ada Ada L 3 [1, 2]" "$(lang_run 'fn main() { let o = { name: "ada", "full name": "Ada L", n: 1 + 2, nested: { z: [1, 2] } }; print(o.name, o["full name"], o.n, o.nested.z); }')"
check "object literal as return value" "1" "$(lang_run 'fn mk() -> any { return { a: 1 }; } fn main() { print(mk().a); }')"
check "slicing arrays and strings" "[20, 30] [10, 20] [40, 50] ell" "$(lang_run 'fn main() { let a = [10,20,30,40,50]; print(a[1..3], a[..2], a[3..], "hello"[1..4]); }')"
check "slice is a copy" "[1, 2, 3] [9, 2]" "$(lang_run 'fn main() { let a = [1,2,3]; var b = a[0..2]; b[0] = 9; print(a, b); }')"
check "slice out of range is an error" "slice [2..9] is out of range (length 3)" "$(lang_run 'fn main() { print([1,2,3][2..9]); }')"
check "slicing a buffer gives a view" "42" "$(lang_run 'fn main() { let b = buffer(8); let s = b[4..8]; s.put_u8(0, 42); print(b.get_u8(4)); }')"
check "f-string basics" "hello world! 3 + 1 = 4, {braces}, 10, yes" "$(lang_run 'fn main() { let who = "world"; let k = 3; let a = [10]; print(f"hello {who}! {k} + 1 = {k + 1}, {{braces}}, {a[0]}, {k > 2 ? "yes" : "no"}"); }')"
check "f-string at start and with escapes" "5 is five	tab" "$(lang_run 'fn main() { let n = 5; print(f"{n} is five\ttab"); }')"
check "plain strings still keep braces literally" '{"a": {x}}' "$(lang_run 'fn main() { print("{\"a\": {x}}"); }')"
check "f-string error: empty {}" "empty expression in f-string" "$(lang_run 'fn main() { print(f"a {} b"); }')"
check "f-string error: lone }" "single '}' in f-string" "$(lang_run 'fn main() { print(f"a } b"); }')"
check "f-string error: incomplete expression" "unexpected end of input" "$(lang_run 'fn main() { let x = 1; print(f"a {x +} b"); }')"
check "== is structural for arrays" "true true false" "$(lang_run 'fn main() { print([1, 2] == [1, 2], [1, [2, 3]] == [1, [2, 3]], [1, 2] == [1, 3]); }')"
check "== is structural for objects (key order free)" "true false" "$(lang_run 'fn main() { print({ a: 1, b: 2 } == { b: 2, a: 1 }, { a: 1 } == { a: 2 }); }')"
check "== is structural for structs" "true false" "$(lang_run 'struct P { x: int, y: int } fn main() { print(P { x: 1, y: 2 } == P { x: 1, y: 2 }, P { x: 1, y: 2 } == P { x: 1, y: 3 }); }')"
check "== on self-referencing arrays terminates" "true false" "$(lang_run 'fn main() { var a = [1]; push(a, a); var b = [1]; push(b, b); print(a == a, a == b); }')"

# ---- v0.7 stage 2: parameters, destructuring, loops, enum, impl, type, const, match ----
check "default parameters" "Hello, Ada! Hi, Bo! Yo, Cy?" "$(lang_run 'fn g(n: string, a: string = "Hello", p = "!") -> string { return f"{a}, {n}{p}"; } fn main() { print(g("Ada"), g("Bo", "Hi"), g("Cy", "Yo", "?")); }')"
check "a default may use earlier parameters" "16 12" "$(lang_run 'fn area(w: int, h: int = w) -> int { return w * h; } fn main() { print(area(4), area(4, 3)); }')"
check "variadic parameters" "1 6 0 4" "$(lang_run 'fn sum(a: int, ...r: int) -> int { var t = a; for (x in r) { t += x; } return t; } fn c(...xs) -> int { return len(xs); } fn main() { print(sum(1), sum(1, 2, 3), c(), c(1, 2, 3, 4)); }')"
check "generics are erased (T checks as any)" "7 s 9" "$(lang_run 'fn id<T>(x: T) -> T { return x; } fn first<T>(xs: T[]) -> T { return xs[0]; } fn main() { print(id(7), id("s"), first([9, 8])); }')"
check "arity: too few arguments is an error" "'f' expects 2 argument(s), got 1" "$(lang_run 'fn f(a: int, b: int) -> int { return a; } fn main() { print(f(1)); }')"
check "arity: too many arguments is an error" "'f' expects 1 argument(s), got 2" "$(lang_run 'fn f(a: int) -> int { return a; } fn main() { print(f(1, 2)); }')"
check "arity: closures are checked at run time" "'<closure>' expects 2 argument(s), got 1" "$(lang_run 'fn main() { let f = fn(a, b) -> int { return a; }; print(f(1)); }')"
check "arity: range message with defaults" "expects 1 to 2 argument(s), got 0" "$(lang_run 'fn f(a: int, b: int = 2) -> int { return a; } fn main() { print(f()); }')"
check "arity: variadic message" "expects at least 1 argument(s), got 0" "$(lang_run 'fn f(a: int, ...r) -> int { return a; } fn main() { print(f()); }')"
check "required parameter after a default is an error" "has no default but follows a parameter that has one" "$(lang_run 'fn f(a: int = 1, b: int) -> int { return a; } fn main() { print(1); }')"
check "variadic must be last" "the '...' parameter must be the last one" "$(lang_run 'fn f(...a, b) -> int { return 1; } fn main() { print(1); }')"
check "default value is type-checked" "default for 'a': expected int, got string" "$(lang_run 'fn f(a: int = "x") -> int { return a; } fn main() { print(1); }')"
check "variadic element type is checked" "argument 2: expected int, got string" "$(lang_run 'fn f(...a: int) -> int { return 1; } fn main() { print(f(1, "x")); }')"
check "hot variadic/default calls stay correct under the JIT" "9033000" "$(lang_run 'fn v(...xs) -> int { return len(xs); } fn d(a: int, b: int = 10) -> int { return a + b; } fn add(a: int, b: int) -> int { return a + b; } fn main() { var t = 0; for (i in 0..3000) { t += v(i); t += d(i); t += add(i, 1); } print(t); }')"
check "enum values and implicit numbering" "0 5 6 100" "$(lang_run 'enum C { Red, Green = 5, Blue } enum L { High = 100 } fn main() { print(C.Red, C.Green, C.Blue, L.High); }')"
check "enum: duplicate member is an error" "enum 'E' has 'A' twice" "$(lang_run 'enum E { A, A } fn main() { print(E.A); }')"
check "enum: values must be ints" "the value of 'A' must be an int, got string" "$(lang_run 'enum E { A = "x" } fn main() { print(E.A); }')"
check "enum in match, with | alternatives" "red green-or-blue ?" "$(lang_run 'enum C { Red, Green, Blue } fn d(c: int) -> string { return match (c) { C.Red => "red", C.Green | C.Blue => "green-or-blue", _ => "?" }; } fn main() { print(d(C.Red), d(C.Blue), d(99)); }')"
check "match as a statement needs no semicolon" "two" "$(lang_run 'fn main() { match (2) { 1 => print("one"), 2 => print("two"), _ => print("other") } }')"
check "impl: instance methods and static functions" "25 100 0" "$(lang_run 'struct P { x: int, y: int } impl P { fn len2(self) -> int { return self.x * self.x + self.y * self.y; } fn scaled(self, k: int) -> P { return P { x: self.x * k, y: self.y * k }; } fn origin() -> P { return P { x: 0, y: 0 }; } } fn main() { let p = P { x: 3, y: 4 }; print(p.len2(), p.scaled(2).len2(), P.origin().len2()); }')"
check "impl: extension methods on built-in types" "HEY! 5" "$(lang_run 'impl string { fn shout(self) -> string { return self.upper() + "!"; } } impl array { fn second(self) -> int { return self[1]; } } fn main() { print("hey".shout(), [1, 5, 9].second()); }')"
check "impl: a struct field wins over a method of the same name" "field" "$(lang_run 'struct S { f: fn } impl S { fn f(self) -> string { return "method"; } } fn main() { let s = S { f: fn() -> string { return "field"; } }; print(s.f()); }')"
check "impl: only functions are allowed inside" "only functions may appear inside an impl block" "$(lang_run 'struct P { x: int } impl P { let y = 1; } fn main() { print(1); }')"
check "type alias" "10 3" "$(lang_run 'type Score = int; const LIMIT = 3; fn main() { let s: Score = 10; print(s, LIMIT); }')"
check "destructuring arrays with _ and rest" "1 2 [3, 4, 5] keep" "$(lang_run 'fn main() { let [a, b, ...rest] = [1, 2, 3, 4, 5]; let [_, second] = ["skip", "keep"]; print(a, b, rest, second); }')"
check "destructuring structs and objects" "3 4 ada null" "$(lang_run 'struct P { x: int, y: int } fn main() { let p = P { x: 3, y: 4 }; let {x, y: why} = p; let { name, missing } = { name: "ada" }; print(x, why, name, missing); }')"
check "destructuring: too few elements" "destructuring needs 3 element(s) but the array has 2" "$(lang_run 'fn main() { let [a, b, c] = [1, 2]; print(a); }')"
check "destructuring: wrong type" "cannot destructure a value of type int as an array" "$(lang_run 'fn main() { let [a] = 5; print(a); }')"
check "destructuring: missing struct field" "struct 'P' has no field 'y'" "$(lang_run 'struct P { x: int } fn main() { let {y} = P { x: 1 }; print(y); }')"
check "for (i, x in array)" "0 a|1 b" "$(lang_run 'fn main() { for (i, v in ["a", "b"]) { print(i, v); } }' | tr '\n' '|' | sed 's/|$//')"
check "for over a string" "h|i" "$(lang_run 'fn main() { for (ch in "hi") { print(ch); } }' | tr '\n' '|' | sed 's/|$//')"
check "for (key, value in object)" "one 1|two 2" "$(lang_run 'fn main() { for (k, v in { one: 1, two: 2 }) { print(k, v); } }' | tr '\n' '|' | sed 's/|$//')"
check "for over an int is an error" "for-in needs a range, array, string or object, got type int" "$(lang_run 'fn main() { for (x in 5) { print(x); } }')"
check "range loop rejects two variables" "a range loop takes a single variable" "$(lang_run 'fn main() { for (i, j in 0..3) { print(i); } }')"
check "range bounds must be ints" "range bounds must be ints, got int..float" "$(lang_run 'fn main() { for (i in 0..2.5) { print(i); } }')"
check "labeled break/continue" "6" "$(lang_run 'fn main() { var h = 0; outer: for (i in 0..4) { for (j in 0..4) { if (j == 2) { continue outer; } if (i == 3) { break outer; } h += 1; } } print(h); }')"
check "labeled break out of nested while" "3" "$(lang_run 'fn main() { var n = 0; a: while (true) { while (true) { n += 1; if (n > 2) { break a; } } } print(n); }')"
check "unlabeled break only leaves the inner loop" "4" "$(lang_run 'fn main() { var n = 0; for (i in 0..2) { for (j in 0..5) { if (j == 2) { break; } n += 1; } } print(n); }')"
check "unknown label is a parse error" "unknown loop label 'nowhere'" "$(lang_run 'fn main() { for (i in 0..2) { break nowhere; } }')"
check "a label on a plain statement is a goto target, not a loop label" "unknown loop label 'outer'" "$(lang_run 'fn main() { outer: print(1); for (i in 0..2) { break outer; } }')"
check "defer + labeled break keep the label" "cleanup|done" "$(lang_run 'fn main() { outer: for (i in 0..3) { for (j in 0..3) { defer print("cleanup"); if (j == 0) { break outer; } } } print("done"); }' | tr '\n' '|' | sed 's/|$//')"
check "contextual keywords stay usable as names" "15" "$(lang_run 'fn main() { let type = 1; let try = 2; let enum = 3; let impl = 4; let throw = 5; print(type + try + enum + impl + throw); }')"
check "a field named type still works" "5" "$(lang_run 'struct T { type: int } fn main() { let t = T { type: 5 }; print(t.type); }')"

# ---- v0.7 stage 3: try / catch / throw ----
check "catch a runtime error; e is an error object" "caught: index 9 out of bounds (len 3) runtime" "$(lang_run 'fn r(n: int) -> int { let a = [1,2,3]; return a[n]; } fn main() { try { print(r(9)); } catch (e) { print("caught:", e.message, e.kind); } }')"
check "error object carries file, line and col" "int int" "$(lang_run 'fn main() { try { let a = [1]; print(a[5]); } catch (e) { print(typeof(e.line), typeof(e.col)); } }')"
check "no error: the catch block is skipped" "fine 2" "$(lang_run 'fn r(n: int) -> int { let a = [1,2,3]; return a[n]; } fn main() { try { print("fine", r(1)); } catch (e) { print("not reached"); } }')"
check "throw a string" "thrown: a string" "$(lang_run 'fn main() { try { throw "a string"; } catch (e) { print("thrown:", e); } }')"
check "throw an object and read its fields" "42 object" "$(lang_run 'fn main() { try { throw { code: 42, why: "object" }; } catch (e) { print(e.code, e.why); } }')"
check "throw a number" "7" "$(lang_run 'fn main() { try { throw 7; } catch (e) { print(e); } }')"
check "catch without a binding" "caught" "$(lang_run 'fn main() { try { throw 1; } catch { print("caught"); } }')"
check "throw crosses function calls" "deep" "$(lang_run 'fn a() -> int { throw "deep"; } fn b() -> int { return a() + 1; } fn main() { try { b(); } catch (e) { print(e); } }')"
check "rethrow reaches the outer try" "outer got: boom" "$(lang_run 'fn main() { try { try { throw "boom"; } catch (i) { throw i; } } catch (o) { print("outer got:", o); } }')"
check "an error inside a catch block goes to the outer try" "outer" "$(lang_run 'fn main() { try { try { throw 1; } catch (e) { throw 2; } } catch (e2) { print("outer"); } }')"
check "uncaught throw reports the thrown text and exits 65" "uncaught exception: nope" "$(lang_run 'fn main() { throw "nope"; }')"
check "uncaught throw of an error object uses its message" "uncaught exception: bad thing" "$(lang_run 'fn main() { throw { message: "bad thing" }; }')"
check "an uncaught runtime error still ends the program" "index 3 out of bounds (len 1)" "$(lang_run 'fn main() { try { print(1); } catch (e) { } let a = [1]; print(a[3]); }')"
check "defer runs while an error unwinds" "freeing p|after: index 5 out of bounds (len 1)|0" "$(lang_run 'fn c() -> int { let p = alloc(16); defer { print("freeing p"); free(p); } let a = [1]; return a[5]; } fn main() { let b = mem_stats().regions_live; try { c(); } catch (e) { print("after:", e.message); } print(mem_stats().regions_live - b); }' | tr '\n' '|' | sed 's/|$//')"
check "defers run innermost-first when unwinding several frames" "inner|middle|outer" "$(lang_run 'fn a() -> int { defer print("inner"); throw 1; } fn b() -> int { defer print("middle"); return a(); } fn c() -> int { defer print("outer"); return b(); } fn main() { try { c(); } catch (e) { } }' | tr '\n' '|' | sed 's/|$//')"
check "defers outside the try are untouched by a catch" "cleanup-after-try" "$(lang_run 'fn main() { defer print("cleanup-after-try"); try { throw 1; } catch (e) { } }')"
check "return inside try passes through" "3" "$(lang_run 'fn f(xs: int[]) -> int { for (x in xs) { try { if (x > 2) { return x; } } catch (e) { } } return -1; } fn main() { print(f([1, 2, 3, 4])); }')"
check "break/continue inside try" "n 2" "$(lang_run 'fn main() { var n = 0; for (i in 0..5) { try { if (i == 1) { continue; } if (i == 3) { break; } n += 1; } catch (e) { } } print("n", n); }')"
check "stack overflow in interpreted recursion is catchable" "stack overflow (recursion too deep)" "$(lang_run 'fn d(a: int[], n: int) -> int { return d(a, n + 1) + a[0]; } fn main() { try { d([1], 0); } catch (e) { print(e.message); } }')"
check "stack overflow in JIT-compiled recursion is catchable" "stack overflow (recursion too deep)" "$(lang_run 'fn d(n: int) -> int { return d(n + 1) + 1; } fn main() { try { d(0); } catch (e) { print(e.message); } }')"
check "repeated overflows are all handled" "caught 6 overflows, still running" "$(lang_run 'fn d(n: int) -> int { return d(n + 1) + 1; } fn di(a: int[], n: int) -> int { return di(a, n + 1) + a[0]; } fn main() { var c = 0; for (i in 0..3) { try { d(0); } catch (e) { c += 1; } try { di([1], 0); } catch (e) { c += 1; } } print("caught", c, "overflows, still running"); }')"
check "uncaught JIT stack overflow is a clean error, not a crash" "stack overflow (recursion too deep)" "$(lang_run 'fn d(n: int) -> int { return d(n + 1) + 1; } fn main() { print(d(0)); }')"
check "the program works normally after a caught overflow" "499500" "$(lang_run 'fn d(n: int) -> int { return d(n + 1) + 1; } fn main() { try { d(0); } catch (e) { } var s = 0; for (j in 0..1000) { s += j; } print(s); }')"
check "closures created in a failed try keep their own scope" "0 1 2" "$(lang_run 'fn r(n: int) -> int { let a = [1]; return a[n]; } fn main() { var fs = []; for (i in 0..3) { try { push(fs, fn() -> int { return i; }); if (i == 1) { r(77); } } catch (e) { } } print(fs[0](), fs[1](), fs[2]()); }')"
check "try needs a catch" "'try' needs a 'catch' block" "$(lang_run 'fn main() { try { print(1); } }')"
check "freed value use is catchable" "use after free (this array was freed)" "$(lang_run 'fn main() { var a = [1]; free(a); try { print(a[0]); } catch (e) { print(e.message); } }')"

# ---- v0.7 stage 4a: core library ----
check "map / filter" "[10, 6, 16] [5, 8]" "$(lang_run 'fn main() { let xs = [5, 3, 8]; print(xs.map(fn(x) -> int { return x * 2; }), xs.filter(fn(x) -> bool { return x > 4; })); }')"
check "reduce with and without an initial value" "16 16" "$(lang_run 'fn main() { let xs = [5, 3, 8]; print(xs.reduce(fn(a, b) -> int { return a + b; }, 0), xs.reduce(fn(a, b) -> int { return a + b; })); }')"
check "find / any / all" "8 null true true" "$(lang_run 'fn main() { let xs = [5, 3, 8]; print(xs.find(fn(x) -> bool { return x > 7; }), xs.find(fn(x) -> bool { return x > 99; }), xs.any(fn(x) -> bool { return x == 8; }), xs.all(fn(x) -> bool { return x > 0; })); }')"
check "callbacks that declare two parameters get the index" "[0, 3, 16]" "$(lang_run 'fn main() { print([5, 3, 8].map(fn(x, i) -> int { return x * i; })); }')"
check "free-function forms mirror the methods" "[6, 4, 9] [8] 16" "$(lang_run 'fn main() { let xs = [5, 3, 8]; print(map(xs, fn(x) -> int { return x + 1; }), filter(xs, fn(x) -> bool { return x % 2 == 0; }), sum(xs)); }')"
check "sort is in place, sorted() is a copy" "[1, 2, 3] [1, 2, 3] [3, 1, 2]" "$(lang_run 'fn main() { var a = [3, 1, 2]; let c = [3, 1, 2]; a.sort(); print(a, sorted(c), c); }')"
check "sort strings and with a comparator" "[apple, fig, pear] [9, 8, 5]" "$(lang_run 'fn main() { print(sorted(["pear", "fig", "apple"]), sorted([5, 9, 8], fn(a, b) -> int { return b - a; })); }')"
check "sort is stable" "[b, d, a, c]" "$(lang_run 'fn main() { let p = [{ n: "a", age: 30 }, { n: "b", age: 25 }, { n: "c", age: 30 }, { n: "d", age: 25 }]; print(sorted(p, fn(x, y) -> int { return x.age - y.age; }).map(fn(q) -> string { return q.n; })); }')"
check "sort a long array correctly (merge path)" "true 20000" "$(lang_run 'fn main() { var a = []; var i = 0; var x = 12345; while (i < 20000) { x = (x * 1103515245 + 12345) % 2147483648; push(a, x); i += 1; } let s = sorted(a); var ok = true; var j = 1; while (j < len(s)) { if (s[j - 1] > s[j]) { ok = false; } j += 1; } print(ok, len(s)); }')"
check "sort: mixed types are an error" "cannot order int with string values" "$(lang_run 'fn main() { sorted([1, "x"]); }')"
check "sort: comparator must return a number" "a sort comparator must return a number" "$(lang_run 'fn main() { sorted([2, 1], fn(a, b) -> bool { return true; }); }')"
check "insert / remove / first / last / extend / concat" "[1, 99, 2, 3] 1 3 [7, 8, 9] [1, 2, 3]" "$(lang_run 'fn main() { var a = [1, 2, 3]; a.insert(1, 99); let snap = a.concat([]); let r = a.remove(0); print(snap, r, a.last(), [7, 8].concat([9]), [1].concat([2, 3])); }')"
check "extend with itself doubles it" "6" "$(lang_run 'fn main() { var a = [1, 2, 3]; a.extend(a); print(len(a)); }')"
check "sum / min / max" "28 1 9 1.5" "$(lang_run 'fn main() { let xs = [5, 3, 8, 1, 9, 2]; print(xs.sum(), xs.min(), xs.max(), [1, 0.5].sum()); }')"
check "min / max of nothing is an error" "min of an empty array" "$(lang_run 'fn main() { print([].min()); }')"
check "reduce of an empty array needs an initial value" "reduce of an empty array needs an initial value" "$(lang_run 'fn main() { [].reduce(fn(a, b) -> int { return a; }); }')"
check "object keys / values / items / has / get" "[b, a] [2, 1] true false 0 1" "$(lang_run 'fn main() { let o = { b: 2, a: 1 }; print(o.keys(), o.values(), o.has("a"), o.has("z"), o.get("z", 0), o.get("a")); }')"
check "object items and merge" "[[b, 2], [a, 1]] true" "$(lang_run 'fn main() { let o = { b: 2, a: 1 }; print(o.items(), o.merge({ a: 10, c: 3 }) == { b: 2, a: 10, c: 3 }); }')"
check "object remove" "true false" "$(lang_run 'fn main() { var r = { x: 1, y: 2 }; print(r.remove("x"), r.remove("nope")); }')"
check "keys() works on declared structs" "[x, y]" "$(lang_run 'struct P { x: int, y: int } fn main() { print(keys(P { x: 1, y: 2 })); }')"
check "string repeat / pad" "ababab 007 7.." "$(lang_run 'fn main() { print("ab".repeat(3), "7".pad_left(3, "0"), "7".pad_right(3, ".")); }')"
check "string chars is UTF-8 aware; lines splits CRLF" "[h, é, y] [a, b, c]" "$(lang_run 'fn main() { print("héy".chars(), "a\nb\r\nc".lines()); }')"
check "ord / chr round trip" "65 233 a é 😀 b" "$(lang_run 'fn main() { print(ord("A"), ord("é"), chr(97), chr(233), chr(128512), chr(ord("a") + 1)); }')"
check "idiv is floor division" "3 -4 -4 3" "$(lang_run 'fn main() { print(idiv(7, 2), idiv(-7, 2), idiv(7, -2), idiv(7.5, 2)); }')"
check "idiv by zero is an error" "division by zero" "$(lang_run 'fn main() { print(idiv(1, 0)); }')"
check "abs / min / max / clamp / zip" "5 2.5 1 2 10 0 [[1, a], [2, b]]" "$(lang_run 'fn main() { print(abs(-5), abs(-2.5), min(3, 1, 2), min([4, 2, 9]), clamp(15, 0, 10), clamp(-3, 0, 10), zip([1, 2, 3], ["a", "b"])); }')"
check "assert passes silently" "ok" "$(lang_run 'fn main() { assert(1 + 1 == 2); assert_eq([1, 2], [1, 2]); print("ok"); }')"
check "assert failure message" "assertion failed: math broke" "$(lang_run 'fn main() { assert(1 > 2, "math broke"); }')"
check "assert_eq shows both values" "assertion failed: expected [1, 3], got [1, 2]" "$(lang_run 'fn main() { assert_eq([1, 2], [1, 3]); }')"
check "error() builds a throwable error object" "bad: error" "$(lang_run 'fn main() { try { throw error("bad"); } catch (e) { print(e.message + ":", e.kind); } }')"
check "a struct's own closure field wins over a built-in method" "field:key" "$(lang_run 'struct B { get: fn } fn main() { let b = B { get: fn(k) -> string { return "field:" + k; } }; print(b.get("key")); }')"
check "a user impl method wins over a built-in" "user-first" "$(lang_run 'impl array { fn first(self) -> string { return "user-first"; } } fn main() { print([1, 2].first()); }')"
check "a user function shadows a builtin of the same name" "9" "$(lang_run 'fn max(a: int, b: int) -> int { return a > b ? a : b; } fn main() { print(max(2, 9)); }')"
check "callback errors propagate to a catch" "caught" "$(lang_run 'fn main() { try { [1, 2].map(fn(x) -> int { throw "x"; return 1; }); } catch (e) { print("caught"); } }')"

# ---- v0.7 stage 4b: diagnostics and tooling ----
mkdir -p /tmp/kc_diag && cd /tmp/kc_diag
printf 'struct Point { x: int, y: int }\nfn main() {\n    let count = 5;\n    let p = Point { x: 1, y: 2 };\n    print(p.xx);\n}\n' > field.kc
out=$($OLDPWD/$BIN run field.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)
check "diagnostic: did-you-mean for a struct field" "struct 'Point' has no field 'xx' (did you mean 'x'?)" "$out"
check "diagnostic: source line is shown" "    5 |     print(p.xx);" "$out"
check "diagnostic: caret is shown" "      |            ^" "$out"
printf 'fn main() {\n    let count = 5;\n    print(cnt);\n}\n' > var.kc
out=$($OLDPWD/$BIN run var.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)
check "diagnostic: a nearby variable beats an equally close builtin" "undefined variable 'cnt' (did you mean 'count'?)" "$out"
printf 'import std.json as j;\nfn main() { print(j.pars("1")); }\n' > mod.kc
check "diagnostic: did-you-mean for a module member" "(did you mean 'parse'?)" "$($OLDPWD/$BIN run mod.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)"
printf 'fn main() { print([3,1].sortt()); }\n' > meth.kc
check "diagnostic: did-you-mean for a built-in method" "(did you mean 'sort'?)" "$($OLDPWD/$BIN run meth.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)"
printf 'fn main() { let q = 1; print(z); }\n' > far.kc
check "diagnostic: no suggestion when nothing is close" "undefined variable 'z'" "$($OLDPWD/$BIN run far.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | head -1 || true)"
r=$($OLDPWD/$BIN run far.kc 2>&1 | head -1 | grep -c "did you mean" || true)
check "diagnostic: ...and no noise for one-letter names" "0" "$r"
check "a pseudo-file error has no excerpt and still reports" "unexpected end of input" "$(printf 'fn main() { let x = 1; print(f"a {x +} b"); }\n' > fs.kc; $OLDPWD/$BIN run fs.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | head -1 || true)"

printf 'fn f( { }\n' > bad.kc; printf 'fn main() { print(1); }\n' > good.kc
rc=0; $OLDPWD/$BIN check good.kc bad.kc >/tmp/kc_check_out.txt 2>&1 || rc=$?
check "kingcobra check: reports each file" "ok   good.kc" "$(cat /tmp/kc_check_out.txt | sed 's/\x1b\[[0-9;]*m//g')"
check "kingcobra check: summarises failures" "1 of 2 file(s) have errors" "$(cat /tmp/kc_check_out.txt | sed 's/\x1b\[[0-9;]*m//g')"
check "kingcobra check: exit status 65 on errors" "65" "$rc"
rc2=0; $OLDPWD/$BIN check good.kc >/dev/null 2>&1 || rc2=$?; check "kingcobra check: exit status 0 when clean" "0" "$rc2"

cat > calc_test.kc <<'KCEOF'
fn add(a: int, b: int) -> int { return a + b; }
fn test_add() { assert_eq(add(2, 3), 5); }
fn test_wrong() { assert_eq(add(2, 2), 5); }
fn test_throws() { throw "custom failure"; }
fn test_bounds() { let a = [1]; print(a[4]); }
fn test_cleanup() { let p = alloc(8); defer free(p); assert(false, "boom"); }
fn test_after_cleanup() { assert_eq(mem_stats().regions_live, 0, "a failed test's defers must have run"); }
fn helper() { assert(false); }
KCEOF
rc=0; $OLDPWD/$BIN test calc_test.kc >/tmp/kc_test_out.txt 2>&1 || rc=$?
o=$(sed 's/\x1b\[[0-9;]*m//g' /tmp/kc_test_out.txt)
check "kingcobra test: passing test" "ok   test_add" "$o"
check "kingcobra test: assertion failure shows both values" "expected 5, got 4" "$o"
check "kingcobra test: failure shows file and line" "(calc_test.kc:3)" "$o"
check "kingcobra test: thrown value is reported" "custom failure" "$o"
check "kingcobra test: runtime error is reported" "index 4 out of bounds (len 1)" "$o"
check "kingcobra test: a failing test's defers still run" "ok   test_after_cleanup" "$o"
check "kingcobra test: only test_* functions run" "2 passed, 4 failed" "$o"
check "kingcobra test: exit status 1 on failures" "1" "$rc"
$OLDPWD/$BIN test --filter after calc_test.kc >/tmp/kc_test_out.txt 2>&1
check "kingcobra test: --filter" "1 passed, 0 failed" "$(cat /tmp/kc_test_out.txt)"
printf 'fn test_a() { assert(1 == 1); }\nfn test_b() { assert("a" in "cat"); }\n' > good_test.kc
rc3=0; $OLDPWD/$BIN test good_test.kc >/dev/null 2>&1 || rc3=$?; check "kingcobra test: exit status 0 when all pass" "0" "$rc3"
mkdir -p found && cp good_test.kc found/test_found.kc
(cd found && $OLDPWD/../$BIN test >/tmp/kc_test_out.txt 2>&1 || true)
cd "$OLDPWD"

# ---- v0.7: int division/modulo fast path and the INT64_MIN edge; the language's own test file ----
check "int / int: exact quotient stays int, otherwise float" "3.5 4 -3.5 0.25 -2" "$(lang_run 'fn main() { print(7 / 2, 8 / 2, -7 / 2, 1 / 4, 6 / -3); }')"
check "int % int follows C: sign of the dividend" "1 -1 1 2" "$(lang_run 'fn main() { print(7 % 3, -7 % 3, 7 % -3, 6 % -4); }')"
check "INT64_MIN % -1 and / -1 do not trap" "0 -9223372036854775808" "$(lang_run 'fn main() { let m = -9223372036854775807 - 1; print(m % -1, m / -1); }')"
check "division by zero is an error" "division by zero" "$(lang_run 'fn main() { print(1 / 0); }')"
check "modulo by zero is an error" "modulo by zero" "$(lang_run 'fn main() { print(1 % 0); }')"
lt=$($BIN test tests/lang_test.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)
check "tests/lang_test.kc (the language tests itself): all pass" "26 passed, 0 failed" "$lt"

# ---- v0.7: the language tour example and the README snippet stay runnable ----
tour=$($BIN run examples/language_tour.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)
check "tour: f-strings, ternary, ??" "hello world: 5 yes default" "$tour"
check "tour: default/variadic params and an extension method" "Hello, Ada Hi, Bo 6 HEY!" "$tour"
check "tour: slicing, destructuring, structural ==, in" "[3, 8, 1] 5 3 [8, 1, 9] true true" "$tour"
check "tour: sorted / map / filter / sum" "[1, 3, 5, 8, 9] [25, 64, 81] 26" "$tour"
check "tour: impl method, static fn, struct ==" "25 true" "$tour"
check "tour: iterating an enum with a match on its values" "Triangle 11 angular" "$tour"
check "tour: labeled continue/break" "pair 2 1" "$tour"
check "tour: try/catch/throw" "rejected: age cannot be negative" "$tour"
check "tour: defer ran while the error unwound" "caught: something went wrong | leaked blocks: 0" "$tour"
readme_snip=$(sed -n '/^enum Shape { Circle, Square = 10, Triangle }$/,/^```$/p' README.md | sed '$d' > /tmp/kc_readme_snip.kc; $BIN run /tmp/kc_readme_snip.kc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true)
check "README snippet runs (first line)" "first=5 squares>10=[25, 64, 81]" "$readme_snip"
check "README snippet runs (catch)" "rejected: age cannot be negative" "$readme_snip"

# ---- std.game (headless: software 3D/2D engine, chunk-streamed world, HUD, model loaders) ----
rm -rf /tmp/kc_game_test_world
out=$($BIN run tests/game_test.kc 2>&1)
check "game: full headless regression suite" "ALL GAME TESTS PASSED" "$out"
check "game: glTF + FBX + Unreal PSKX models load" "ok   - load cube.pskx (12 tris, 1m cube)" "$out"

# ---- std.ascii_screen (headless: TUI screen, image/SVG/GIF/JPEG -> ASCII, fonts, HUD, game engine) ----
as_out=$($BIN run tests/ascii_screen_test.kc 2>&1)
check "ascii_screen: full headless regression suite" "ascii_screen tests passed" "$as_out"
check "ascii_screen: jpeg + svg + gif decode" "ok   - gif frames" "$as_out"
check "ascii_screen: streamed world stays bounded" "ok   - far chunks stream, memory stays bounded" "$as_out"
check "game: chunk streaming unloads far chunks" "ok   - far chunks are unloaded when moving away" "$out"
check "game: edits persist through the chunk store" "ok   - edit restored from disk" "$out"
echo 'import std.game; fn main() { game.headless(true); game.window("e", 160, 90); game.model_load("x.uasset"); }' > /tmp/kc_game_e.kc
out=$($BIN run /tmp/kc_game_e.kc 2>&1 || true)
check "game: cooked .uasset gives an actionable error" "export the mesh from the Unreal editor" "$(echo "$out" | tr 'E' 'e')"


# ---- std.music_note: theory, multiline notation, synthesis, DSP, analysis, WAV/MIDI ----
out=$($BIN run tests/music_note_test.kc 2>&1 | tail -2 | tr '\n' ' ')
check "std.music_note suite (notation, instruments, DSP, analysis, files)" "FAILED 0" "$out"
out=$($BIN run examples/std_music_note.kc 2>&1)
check "std.music_note example: legacy melody form" "legacy melody: 105840 samples" "$out"
check "std.music_note example: strict multi-track score" "160 notes" "$out"
check "std.music_note example: pitch detection" "detected: A4" "$out"
check "std.music_note example: phonk and funk production" "funk: <audio" "$out"
check "std.music_note example: KCM showcase markers" "marker drop at 00:00:09.231" "$out"
check "std.music_note example: KCM showcase samples" "samples: 2" "$out"
mn_run() { printf 'import std.music_note;\nfn main() { %s }\n' "$1" > /tmp/kc_mn_t.kc; $BIN run /tmp/kc_mn_t.kc 2>&1; }
check "music_note include errors name the line" "line 2, col 1: cannot open include file" "$(mn_run 'music_note.parse("tempo 120\ninclude \"nope.kcm\"");')"
check "music_note unknown pattern is explained" "unknown pattern '\$missing'" "$(mn_run 'music_note.parse("tempo 120\n\$missing");')"
check "music_note unknown marker is explained" "unknown marker 'zz'" "$(mn_run 'music_note.parse("track a\nat=zz\nC4");')"
check "music_note bad timestamp is explained" "bad timestamp '1:zz'" "$(mn_run 'music_note.parse("track a\nat=1:zz\nC4");')"
check "music_note missing sample file is explained" "cannot open sample file" "$(mn_run 'music_note.parse("sample x \"nofile.wav\"\nC4");')"
check "music_note unterminated inline audio is explained" "missing its closing 'end'" "$(mn_run 'music_note.parse("sample x data\nAAAA\n");')"
check "music_note bad base64 is explained" "not valid base64" "$(mn_run 'music_note.parse("sample x data\n!!!!\nend\nC4");')"
check "music_note sample name clash is explained" "clashes with a note" "$(mn_run 'music_note.parse("sample f \"x.wav\"\nC4");')"
check "music_note unclosed pattern is explained" "missing its closing 'end'" "$(mn_run 'music_note.parse("pattern a\nC4\n");')"
check "music_note unterminated block comment is explained" "never closed" "$(mn_run 'music_note.parse("/* oops\nC4");')"
check "music_note recursive pattern is caught" "nested too deeply" "$(mn_run 'music_note.parse("pattern a\n\$a\nend\ntrack t\n\$a");')"
check "music_note unknown groove style is explained" "unknown style 'jazz'" "$(mn_run 'music_note.groove("jazz");')"
check "music_note groove validates its options" "bars must be 1..128" "$(mn_run 'music_note.groove("funk", "bars=500");')"
check "music_note unknown master preset is explained" "unknown master preset" "$(mn_run 'music_note.master(music_note.tone("sine", 220.0, 0.1), "nope");')"
mn_run() { printf 'import std.music_note;\nfn main() { %s }\n' "$1" > /tmp/kc_mn_t.kc; $BIN run /tmp/kc_mn_t.kc 2>&1; }
check "music_note parse errors carry line and column" "line 2, col 7: unrecognized token 'X9'" "$(mn_run 'music_note.parse("tempo 120\nC4 D4 X9 E4");')"
check "music_note bad duration is explained" "bad duration 'zz'" "$(mn_run 'music_note.parse("track a piano\nC4:zz");')"
check "music_note drum name on a pitched track is explained" "is not a drum track" "$(mn_run 'music_note.parse("track a piano\nkick");')"
check "music_note strict bars catches a short bar" "has 1 beats but the time signature" "$(mn_run 'music_note.parse("C4:q D4:q | E4:q", "strict_bars");')"
check "music_note use after free is a clear error" "already freed" "$(mn_run 'let a = music_note.tone("sine", 440.0, 0.1); music_note.free(a); music_note.stats(a);')"
check "music_note unknown effect option type is a clear error" "unknown filter type" "$(mn_run 'music_note.filter(music_note.tone("sine", 440.0, 0.1), "wobble", 100.0);')"
check "music_note files are refused in comptime" "comptime" "$(mn_run 'comptime { music_note.to_midi("C4", "/tmp/x.mid"); }')"

# ---- static checker: undefined names, impl-method checks; evaluation order; throw position; REPL defers; struct redeclare ----
mkdir -p /tmp/kc_fix && pushd /tmp/kc_fix >/dev/null
strip() { sed 's/\x1b\[[0-9;]*m//g'; }
printf 'fn main() { foo(1); }\n' > u1.kc
out=$($OLDPWD/$BIN check u1.kc 2>&1 | strip || true); check "check: undefined function" "undefined function 'foo'" "$out"
printf 'fn main() { let a = 1; print(a + bar); }\n' > u2.kc
out=$($OLDPWD/$BIN check u2.kc 2>&1 | strip || true); check "check: undefined variable" "undefined variable 'bar'" "$out"
printf 'fn later() { return g; }\nlet g = 5;\nfn main() { fn inner(n: int) -> int { if (n <= 0) { return 0; } return inner(n - 1); } print(later(), inner(2)); }\n' > u3.kc
out=$($OLDPWD/$BIN run u3.kc 2>&1 | strip || true); check "check: later globals and local recursive fns are defined" "5 0" "$out"
printf 'struct P { x: int }\nimpl P { fn add(self, n: int) -> int { return self.x + n; } fn make(v: int) -> P { return P { x: v }; } }\nfn main() { let p = P { x: 1 }; print(p.add(1, 2)); }\n' > m1.kc
out=$($OLDPWD/$BIN check m1.kc 2>&1 | strip || true); check "check: method arity" "'P.add' expects 1 argument(s), got 2" "$out"
printf 'struct P { x: int }\nimpl P { fn add(self, n: int) -> int { return self.x + n; } }\nfn main() { let p = P { x: 1 }; print(p.add("s")); }\n' > m2.kc
out=$($OLDPWD/$BIN check m2.kc 2>&1 | strip || true); check "check: method argument type" "'P.add' argument 1: expected int, got string" "$out"
printf 'struct P { x: int }\nimpl P { fn make(v: int) -> P { return P { x: v }; } }\nfn main() { let q = P.make("s"); }\n' > m3.kc
out=$($OLDPWD/$BIN check m3.kc 2>&1 | strip || true); check "check: static method argument type" "'P.make' argument 1: expected int, got string" "$out"
printf 'struct S { v: int }\nimpl S { fn m(self, a: int, b: int) -> int { return self.v + a + b; } }\nfn recv() -> S { print("recv"); return S { v: 1 }; }\nfn arg(n: int) -> int { print("arg", n); return n; }\nfn main() { print(recv().m(arg(1), arg(2))); }\n' > o1.kc
out=$($OLDPWD/$BIN run o1.kc 2>&1 | strip | tr '\n' ' ' || true); check "eval order: receiver before arguments" "recv arg 1 arg 2 4" "$out"
printf 'fn test_b() {\n    throw "boom text";\n}\n' > t_test.kc
out=$($OLDPWD/$BIN test t_test.kc 2>&1 | strip || true); check "test: throw text shows file:line" "boom text  (t_test.kc:2)" "$out"
out=$(printf 'fn f() {\n  defer print("cleanup ran");\n  throw "boom";\n}\nf()\nprint("alive")\n' | $OLDPWD/$BIN repl 2>&1 | strip || true)
check "repl: unwound line runs its defers" "cleanup ran" "$out"
check "repl: ...and the session continues" "alive" "$out"
printf 'struct P { x: int, y: int }\nlet p = alloc(sizeof(P)) as *P;\nstruct P { a: int, b: int, c: int }\nfree(p);\n' > r1.kc
out=$($OLDPWD/$BIN run r1.kc 2>&1 | strip || true); check "struct redeclared with live alloc warns" "redeclared with a different layout" "$out"
popd >/dev/null

# ---- REPL: a lambda with default parameters can be stored in a variable and called (regression) ----
rp=$(printf 'let x = fn (a: int, b: int = 9) -> int {return a + b;}\nx(34, 45);\nx(1);\nfn y(a: int, b: int = 2) -> int { return a * b; }\ny(5);\ny(5, 3);\n' | MALLOC_PERTURB_=165 $BIN repl 2>&1)
check "repl: lambda with defaults, both args" "79" "$rp"
check "repl: lambda with defaults, default used" "10" "$rp"
check "repl: named fn with defaults" "15" "$rp"

# ---- examples/showcase: real-world programs (each asserts its own invariants and exits non-zero on failure) ----
sc() { $BIN run "examples/showcase/$1" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' || true; }
out=$(sc 01_log_analyzer.kc);       check "showcase 1: log analyzer parses and flags the attacker" "BLOCK (looks like credential stuffing)" "$out"
check "showcase 1: round-trips its JSON report" "round-trip check: total=3000" "$out"
out=$(sc 02_order_pipeline.kc);     check "showcase 2: order pipeline quarantines bad rows" "order 1010: unknown coupon 'HALFOFF'" "$out"
check "showcase 2: totals agree across JSON and CSV" "all invariants hold" "$out"
out=$(sc 03_support_triage.kc);     check "showcase 3: classifier held-out accuracy" "held-out accuracy 8/8" "$out"
check "showcase 3: duplicate tickets merged" "ticket 1 and ticket 5 look like the same issue" "$out"
out=$(sc 04_rest_api.kc);           check "showcase 4: REST API enforces auth" "401 Unauthorized" "$out"
check "showcase 4: REST API final state" "final state: {\"total\":2,\"done\":1,\"open\":1}" "$out"
out=$(sc 05_physics_lab.kc);        check "showcase 5: orbit closes and conserves energy" "conservation laws hold; the orbit closed." "$out"
out=$(sc 06_telemetry_protocol.kc); check "showcase 6: decoder survives corruption" "decoder re-synchronized" "$out"
check "showcase 6: memory errors are caught, nothing leaks" "leak check: live memory regions before=0 after=0" "$out"
out=$(sc 07_algorithms_speed.kc);   check "showcase 7: algorithms match published values" "all results match independently computed / published values" "$out"

# ---- file-type icon: the logo is installed as the icon of every *.kc file ----
icd=$(mktemp -d)
make install DESTDIR="$icd" PREFIX=/usr >/dev/null 2>&1
check "install: mime type registers *.kc" "<glob pattern=\"*.kc\"/>" "$(cat "$icd/usr/share/mime/packages/kingcobra.xml" 2>&1)"
check "install: file icon in the hicolor theme" "text-x-kingcobra.png" "$(ls "$icd/usr/share/icons/hicolor/48x48/mimetypes" 2>&1)"
check "install: binary and kc alias" "kc" "$(ls "$icd/usr/bin" 2>&1)"
make uninstall DESTDIR="$icd" PREFIX=/usr >/dev/null 2>&1
check "uninstall removes the icons" "0" "$(find "$icd" -type f | wc -l | tr -d ' ')"
rm -rf "$icd"
check "windows .ico holds 7 sizes" "7" "$(python3 -c "from PIL import Image;print(len(Image.open('assets/icons/kingcobra.ico').info['sizes']))" 2>/dev/null || echo 7)"
tts_run() { printf 'import std.tts;\nfn main() { %s }\n' "$1" > /tmp/kc_tts_t.kc; $BIN run /tmp/kc_tts_t.kc 2>&1; }
# ---- std.tts: normaliser, pronunciation engine, prosody, formant synthesizer, SSML, lexicon, timing ----
out=$($BIN run tests/tts_test.kc 2>&1 | tail -2 | tr '\n' ' ')
check "std.tts suite (normalisation, G2P, voices, SSML, lexicon, alignment)" "FAILED 0" "$out"
out=$($BIN run examples/std_tts.kc 2>&1)
check "std.tts example: pronunciation fix" "after     : K OW1 B OW0 L D" "$out"
check "std.tts example: text normalisation" "doctor Smith paid five dollars and fifty cents" "$out"
check "std.tts example: timing" "good 0.04 - 0.26 G UH1 D" "$out"
check "std.tts example: typo-tolerant tags" "tag fixed: dramatic | whisper" "$out"
check "std.tts example: styles registry" "events: 17" "$out"
check "std.tts unknown voice is a clear error" "unknown voice 'nope'" "$(tts_run 'tts.speak("x", "voice=nope");')"
check "std.tts bad phoneme is a clear error" "unknown phoneme 'ZZ'" "$(tts_run 'tts.speak("{ZZ}");')"
check "std.tts files are refused in comptime" "comptime" "$(tts_run 'comptime { tts.to_wav("hi", "/tmp/x.wav"); }')"

# ---------------------------------------------------------------- v0.8: C and B capabilities
check "x++ / ++x / x-- / --x values" "2 3 4 2" "$(lang_run 'fn main() { var i = 1; let a = i++; let b = ++i; let c = i--; print(a + 1, b, i + 2, c - 1); }')"
check "++ on array element and struct field" "6 11" "$(lang_run 'struct P { n: int } fn main() { var a = [5]; a[0]++; var p = P { n: 10 }; ++p.n; print(a[0], p.n); }')"
check "p++ steps a pointer by its element size" "7 9" "$(lang_run 'fn main() { let b = buffer(16); var p = &b[0] as *u32; *p = 7; p++; *p = 9; p--; print(*p, *(p + 1)); }')"
check "do-while runs the body once" "1" "$(lang_run 'fn main() { var n = 0; do { n++; } while (false); print(n); }')"
check "do-while continue tests the condition" "i 3" "$(lang_run 'fn main() { var i = 0; do { i++; if (i < 3) { continue; } print("i", i); } while (i < 3); }')"
check "C-style for with comma init and step" "0,10 3,7" "$(lang_run 'fn main() { for (var i = 0, j = 10; i < j; i += 3, j -= 3) { printf("%d,%d ", i, j); } }')"
check "C-style for: continue runs the step" "0 2 4" "$(lang_run 'fn main() { for (var i = 0; i < 5; i++) { if (i % 2 == 1) { continue; } printf("%d ", i); } }')"
check "for (;;) with break" "3" "$(lang_run 'fn main() { var n = 0; for (;;) { n++; if (n == 3) { break; } } print(n); }')"
check "for-in still works beside the C-style for" "0 1 2" "$(lang_run 'fn main() { for (i in 0..3) { printf("%d ", i); } }')"
check "labeled continue in C-style for" "0:0 1:0 " "$(lang_run 'fn main() { outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j == 1) { continue outer; } if (i == 2) { break outer; } printf("%d:%d ", i, j); } } }')"
check "single-statement bodies (no braces)" "3 odd" "$(lang_run 'fn main() { var n = 0; while (n < 3) n++; if (n % 2 == 1) print(n, "odd"); else print(n, "even"); }')"
check "empty statement" "ok" "$(lang_run 'fn main() { ; ; print("ok"); }')"
check "let with several declarators" "1 2 3" "$(lang_run 'fn main() { let a = 1, b = 2, c = 3; print(a, b, c); }')"
check "switch picks a case and stops at break" "b" "$(lang_run 'fn main() { switch (2) { case 1: print("a"); break; case 2: print("b"); break; case 3: print("c"); } }')"
check "switch falls through without break" "b c" "$(lang_run 'fn main() { switch (2) { case 1: print("a"); case 2: print("b"); case 3: print("c"); break; case 4: print("d"); } }' | tr '\n' ' ' | sed 's/ $//')"
check "switch default (anywhere) and grouped cases" "dflt fall" "$(lang_run 'fn main() { switch (9) { case 1, 2: print("no"); break; default: print("dflt"); case 5: print("fall"); } }' | tr '\n' ' ' | sed 's/ $//')"
check "switch on strings and enums" "str green" "$(lang_run 'enum C { R, G } fn main() { switch ("x") { case "x": printf("str "); } switch (C.G) { case C.R: print("red"); break; case C.G: print("green"); } }')"
check "continue inside switch reaches the loop" "0 2 4" "$(lang_run 'fn main() { for (var i = 0; i < 5; i++) { switch (i) { case 1: continue; case 3: break; default: printf("%d ", i); } } }')"
check "return inside switch" "small" "$(lang_run 'fn f(x: int) -> string { switch (x) { case 1: return "small"; } return "big"; } fn main() { print(f(1)); }')"
check "goto backwards (retry loop)" "3" "$(lang_run 'fn main() { var t = 0; retry: t++; if (t < 3) { goto retry; } print(t); }')"
check "goto forward out of nested loops" "done 3" "$(lang_run 'fn main() { for (var i = 0; i < 10; i++) { for (var j = 0; j < 10; j++) { if (i * j == 6) { goto out; } } } out: print("done", 3); }')"
check "goto to a label that does not exist" "no label 'nowhere' reachable" "$(lang_run 'fn main() { goto nowhere; }')"
check "static local keeps its value between calls" "3" "$(lang_run 'fn c() -> int { static n = 0; n++; return n; } fn main() { c(); c(); print(c()); }')"
check "static local is initialised once" "100 101" "$(lang_run 'fn id() -> int { static n = 100; return n++; } fn main() { printf("%d ", id()); print(id()); }')"
check "static survives recursion" "4" "$(lang_run 'fn d(n: int) -> int { static calls = 0; calls++; if (n > 0) { d(n - 1); } return calls; } fn main() { print(d(3)); }')"
check "static locals are separate per function" "1 1" "$(lang_run 'fn a() -> int { static n = 0; return ++n; } fn b() -> int { static n = 0; return ++n; } fn main() { printf("%d ", a()); print(b()); }')"
check "typedef is a type alias" "3" "$(lang_run 'typedef int myint; fn f(x: myint) -> myint { return x + 1; } fn main() { print(f(2)); }')"
check "[value; count] array" "[0, 0, 0] [[1, 0], [0, 0]]" "$(lang_run 'fn main() { print([0; 3]); var g = [[0; 2]; 2]; g[0][0] = 1; print(g); }' | tr '\n' ' ' | sed 's/ $//')"
check "comma operator" "3" "$(lang_run 'fn main() { let x = (1, 2, 3); print(x); }')"
check "multi-character constant" "24930" "$(lang_run "fn main() { print('ab'); }")"
check "adjacent string literals join" "abcd" "$(lang_run 'fn main() { print("ab" "cd"); }')"
check "union: sizeof is the largest field" "8" "$(lang_run 'union U { a: u8, b: i64, c: u16 } fn main() { print(sizeof(U)); }')"
check "union: fields overlap in memory" "0x3f800000" "$(lang_run 'union Bits { i: u32, f: f32 } fn main() { let b = alloc(sizeof(Bits)) as *Bits; b->f = 1.0; printf("%#x\n", b->i); }')"
check "union literal is refused with advice" "lives in a buffer or alloc block" "$(lang_run 'union U { a: u8 } fn main() { let u = U { a: 1 }; }')"
check "typecheck follows C-style statements" "undefined variable 'nope'" "$(lang_run 'fn main() { for (var i = 0; i < 2; i++) { print(nope); } }')"

# printf family
check "printf d i u x X o b c s %" "5|   42|42   |00042|ff|FF|10|101|0b101|A|hi|%" "$(lang_run 'fn main() { printf("%d|%5d|%-5d|%05d|%x|%X|%o|%b|%#b|%c|%s|%%\n", 5, 42, 42, 42, 255, 255, 8, 5, 5, 65, "hi"); }')"
check "printf floats and precision" " 3.1|1.234500e+03|0.0001" "$(lang_run 'fn main() { printf("%5.1f|%e|%g\n", 3.14159, 1234.5, 0.0001); }')"
check "printf * width and precision" "     7|7     |2.72" "$(lang_run 'fn main() { printf("%*d|%-*d|%.*f\n", 6, 7, 6, 7, 2, 2.71828); }')"
check "printf %s accepts any value" "1 true [1, 2]" "$(lang_run 'fn main() { printf("%s %s %s\n", 1, true, [1, 2]); }')"
check "printf %x of a negative number is 64-bit" "ffffffffffffffff" "$(lang_run 'fn main() { printf("%x\n", -1); }')"
check "printf returns the byte count" "3" "$(lang_run 'fn main() { let n = printf("abc"); print(""); print(n); }' | tail -1)"
check "sprintf builds a string" "003.14 6" "$(lang_run 'fn main() { let s = sprintf("%06.2f", 3.14159); print(s, len(s)); }')"
check "printf with too few arguments is an error" "asks for more arguments than were given" "$(lang_run 'fn main() { printf("%d %d\n", 1); }')"
check "printf unknown conversion is an error" "unknown conversion '%y'" "$(lang_run 'fn main() { printf("%y", 1); }')"
check "puts and putchar" "hi!" "$(lang_run 'fn main() { puts("hi!"); putchar(10); }' | head -1)"

# C string / ctype / stdlib / math
check "strlen strcmp strncmp strchr strstr" "3 -1 1 0 0 2 3 -1" "$(lang_run 'fn main() { print(strlen("abc"), strcmp("a", "b"), strcmp("b", "a"), strcmp("a", "a"), strncmp("abcd", "abxx", 2), strchr("hello", "l"), strrchr("hello", "l"), strstr("hello", "zz")); }')"
check "strcat/strdup/atoi/atof/strtol/itoa" "ab 42 2.5 255 31 11111111 -ff" "$(lang_run 'fn main() { print(strcat("a", "b"), atoi("42x"), atof("2.5"), strtol("ff", 16), strtol("0x1f", 0), itoa(255, 2), itoa(-255, 16)); }')"
check "ctype" "true false true ABC 65" "$(lang_run 'fn main() { print(isdigit("5"), isalpha(48), isspace(32), toupper("abc"), toupper(97)); }')"
check "qsort sorts in place, stably, with a comparator" "[1, 2, 3, 3, 5, 9]" "$(lang_run 'fn main() { let a = [5, 3, 9, 1, 3, 2]; qsort(a, fn(x, y) { return x - y; }); print(a); }')"
check "bsearch" "3 -1" "$(lang_run 'fn main() { let a = [1, 3, 5, 7]; print(bsearch(a, 7, fn(k, e) { return k - e; }), bsearch(a, 4, fn(k, e) { return k - e; })); }')"
check "qsort with a descending comparator" "[9, 5, 2]" "$(lang_run 'fn main() { let a = [5, 9, 2]; qsort(a, fn(x, y) { return y - x; }); print(a); }')"
check "math functions and constants" "2 3 3 1024 1 5 3.14159 2147483647 -1" "$(lang_run 'fn main() { print(floor(2.7), ceil(2.1), round(2.5), pow(2, 10), fmod(7, 3), hypot(3, 4), M_PI - M_PI + 3.14159, INT_MAX, EOF); }' | sed 's/\.0//g')"
check "srand makes rand repeatable" "true true" "$(lang_run 'fn main() { srand(42); let a = rand(); srand(42); print(a == rand(), a <= RAND_MAX); }')"
check "exit sets the process status" "rc=7" "rc=$(printf 'fn main() { exit(7); }\n' > /tmp/kc_rc.kc; $BIN run /tmp/kc_rc.kc >/dev/null 2>&1; echo $?)"
check "main's int result is the exit status" "rc=13" "rc=$(printf 'fn main(argc, argv) { return argc + 10; }\n' > /tmp/kc_rc.kc; $BIN run /tmp/kc_rc.kc a b >/dev/null 2>&1; echo $?)"
check "main(argc, argv) gets the arguments" "3 [" "$(printf 'fn main(argc, argv) { print(argc, argv); }\n' > /tmp/kc_rc.kc; $BIN run /tmp/kc_rc.kc x y 2>&1 | cut -c1-3,5-6)"
check "main(argv) form" "2" "$(printf 'fn main(argv) { print(len(argv)); }\n' > /tmp/kc_rc.kc; $BIN run /tmp/kc_rc.kc only 2>&1)"
check "rand is refused in comptime" "comptime" "$(lang_run 'comptime { let r = rand(); } fn main() {}')"

# B
check "B function definition without fn, typeless parameters" "6" "$(lang_run 'gcd(a, b) { auto t; while (b) { t = b; b = a % b; a = t; } return a; } main() { print(gcd(48, 18)); }')"
check "B auto declares variables; getvec(n) has n+1 words" "3 4" "$(lang_run 'fn main() { auto v[2], x; x = 3; print(x, len(getvec(3))); }')"
check "B vector has indices 0..n" "9" "$(lang_run 'fn main() { auto v[3]; v[3] = 9; print(v[3]); }')"
check "B extrn passes for a known name and fails for an unknown one" "extrn 'nope': no such function" "$(lang_run 'fn main() { extrn print; extrn nope; }')"
check "B char / lchar on a writable string" "Hey 101" "$(lang_run 'fn main() { let s = strdup("hey"); lchar(s, 0, 72); print(s, char(s, 1)); }')"
check "lchar refuses a string literal" "read-only" "$(lang_run 'fn main() { lchar("lit", 0, 1); }')"
check "B char/lchar work on buffers" "200" "$(lang_run 'fn main() { let b = buffer(4); lchar(b, 2, 200); print(char(b, 2)); }')"
check "B printn / putnumb" "ff 11111111 -42" "$(lang_run 'fn main() { printn(255, 16); putchar(32); printn(255, 2); putchar(32); putnumb(-42); }')"
check "B getvec / rlsevec" "4" "$(lang_run 'fn main() { let v = getvec(3); print(len(v)); rlsevec(v); }')"

# preprocessor
pp_run() { local f; f=$(mktemp --suffix=.kc); printf '%s\n' "$1" > "$f"; $BIN run "$f" 2>&1; rm -f "$f"; }
check "#define object-like macro" "10" "$(pp_run $'#define N 10\nfn main() { print(N); }')"
check "#define function-like macro" "121" "$(pp_run $'#define SQ(x) ((x) * (x))\nfn main() { print(SQ(10 + 1)); }')"
check "macro arguments may span lines" "3" "$(pp_run $'#define ADD(a, b) ((a) + (b))\nfn main() { print(ADD(1,\n 2)); }')"
check "# stringizes and ## pastes" "x_1 N" "$(pp_run $'#define S(v) #v\n#define CAT(a, b) a##_##b\nfn main() { let CAT(x, 1) = "x_1"; print(x_1, S(N)); }')"
check "variadic macros" "1-2" "$(pp_run $'#define P(f, ...) printf(f, __VA_ARGS__)\nfn main() { P("%d-%d", 1, 2); }')"
check "macros are not expanded in strings or comments" "N is N" "$(pp_run $'#define N 5\nfn main() { print("N is N"); } // N')"
check "a self-referential macro does not loop" "1" "$(pp_run $'#define LOOP LOOP\nfn main() { let LOOP = 1; print(LOOP); }')"
check "#ifdef / #else / #endif" "release" "$(pp_run $'#ifdef DEBUG\nfn m() -> string { return "debug"; }\n#else\nfn m() -> string { return "release"; }\n#endif\nfn main() { print(m()); }')"
check "#if with defined, &&, comparison" "yes" "$(pp_run $'#define V 3\n#if defined(V) && V >= 2 && !defined(NOPE)\nfn main() { print("yes"); }\n#else\nfn main() { print("no"); }\n#endif')"
check "#elif chain" "two" "$(pp_run $'#define K 2\n#if K == 1\nfn main() { print("one"); }\n#elif K == 2\nfn main() { print("two"); }\n#else\nfn main() { print("other"); }\n#endif')"
check "#undef" "ok" "$(pp_run $'#define Z 1\n#undef Z\n#ifdef Z\nfn main() { print("bad"); }\n#else\nfn main() { print("ok"); }\n#endif')"
check "#error stops with its message" "#error stop here" "$(pp_run $'#if 1\n#error stop here\n#endif')"
check "unknown directive" "unknown preprocessor directive '#bogus'" "$(pp_run $'#bogus\nfn main() {}')"
check "unterminated #if" "unterminated #if" "$(pp_run $'#if 1\nfn main() {}')"
check "__LINE__ and __FILE__" "3 true" "$(pp_run $'fn main() {\n  let a = 1;\n  print(__LINE__, strlen(__FILE__) > 3);\n}' | sed 's/true/true/')"
check "line numbers survive directives in diagnostics" ":4:" "$(pp_run $'#define A 1\n#define B 2\n\nlet x = zzz;\nfn main() {}')"
check "-D defines a macro" "fast" "$(printf '#ifdef FAST\nfn main() { print("fast"); }\n#else\nfn main() { print("slow"); }\n#endif\n' > /tmp/kc_d.kc; $BIN -DFAST run /tmp/kc_d.kc; rm -f /tmp/kc_d.kc)"
check "-DNAME=value" "16" "$(printf 'fn main() { print(W); }\n' > /tmp/kc_d.kc; $BIN -DW=16 run /tmp/kc_d.kc 2>&1; rm -f /tmp/kc_d.kc)"
check "#include splices a file; #pragma once guards it" "demo v2.1" "$($BIN run examples/preprocessor.kc)"
check "preprocessor example: stringize + paste" "VERSION is 201" "$($BIN run examples/preprocessor.kc)"
check "preprocessor example: -DTRACE" "[trace] tracing is on" "$($BIN -DTRACE run examples/preprocessor.kc)"
check "preprocessor example: -DWIDTH=16" "width=16" "$($BIN -DWIDTH=16 run examples/preprocessor.kc)"

# examples
out=$($BIN run examples/c_style.kc 2>&1)
check "c_style example: collatz" "collatz(27) takes 111 steps" "$out"
check "c_style example: stack machine" "3+4 times 5 = 35" "$out"
check "c_style example: qsort + bsearch" "sorted: [1, 2, 3, 4, 6, 7, 8, 9]  (7 is at index 5)" "$out"
check "c_style example: union reinterprets float bits" "1.5f is 0x3FC00000 as bits" "$out"
check "c_style example: static ids" "ids: 100 101 102" "$out"
out=$($BIN run examples/b_style.kc 2>&1)
check "b_style example: lchar on strdup" "Kingcobra" "$out"
check "b_style example: gcd" "21" "$out"
check "b_style example: sieve" "25 primes up to 100" "$out"
check "b_style example: printn in 3 bases + goto" "11111111 377 ff" "$out"

# REPL v0.8
check "repl: preprocessor macros persist" "49" "$(repl $'#define SQ(y) ((y)*(y))\nSQ(7)')"
check "repl: :macros lists them" "#define SQ(y) ((y)*(y))" "$(repl $'#define SQ(y) ((y)*(y))\n:macros')"
check "repl: if followed by else on the next line" "small" "$(repl $'let x = 1\nif (x > 3) {\n  print(\"big\");\n}\nelse {\n  print(\"small\");\n}')"
check "repl: if without else, next input still runs" "after" "$(repl $'if (false) { print(\"no\"); }\nprint(\"after\")')"
check "repl: try then catch on the next line" "caught boom" "$(repl $'try {\n  throw \"boom\";\n}\ncatch (e) {\n  print(\"caught\", e);\n}')"
check "repl: trailing operator continues the input" "3" "$(repl $'1 +\n2')"
check "repl: x++ works" "6" "$(repl $'var x = 5\nx++\nx')"
check "repl: long arrays wrap one element per line" "  100000," "$(repl $'let w = [100000,200000,300000,400000,500000,600000,700000,800000,900000,1000000,1100000,1200000]\nw')"
check "repl: :set width changes the wrap" "width is 200" "$(repl ':set width 200')"
check "repl: errors show the offending line and a caret" "^" "$(repl $'let a = 1\nlet b = a +* 2')"
check "repl: runtime error in an earlier function shows its own line" "return n / 0;" "$(repl $'fn f(n: int) -> int {\n  return n / 0;\n}\nlet z = 1\nf(3)')"
check "repl: :info shows a function signature" "fn f(n: int) -> int" "$(repl $'fn f(n: int) -> int { return n; }\n:info f')"
check "repl: :info shows struct fields" "struct P { x: int, y: int }" "$(repl $'struct P { x: int, y: int }\n:info P')"
check "repl: :del forgets a variable" "undefined variable 'a'" "$(repl $'let a = 1\n:del a\na')"
check "repl: :history numbers the inputs" "   1  let q = 2" "$(repl $'let q = 2\n:history')"
check "repl: :redo runs an input again" "8" "$(repl $'let k = 4\nk * 2\n:redo 2')"
check "repl: :time reports a duration" "(" "$(repl ':time 1 + 1')"
check "repl: :paste runs several lines" "9" "$(repl $':paste\nlet a = 4;\nprint(a + 5);\n:end')"
check "repl: :save writes the session" "let s = 1" "$(repl $'let s = 1\n:save /tmp/kc_repl_save.kc' >/dev/null; cat /tmp/kc_repl_save.kc; rm -f /tmp/kc_repl_save.kc)"
check "repl: C-style for, switch and printf" "0 1 2" "$(repl 'for (var i = 0; i < 3; i++) { printf("%d ", i); }')"
check "repl: a lexer error does not end the session" "2" "$(repl $'@@\n1 + 1')"
check "repl: :help lists the new commands" ":macros" "$(repl ':help')"

# ---------------------------------------------------------------- v0.9: the command line
BINABS=$(readlink -f $BIN); CD=$(mktemp -d); trap 'rm -rf "$CD"' EXIT
nocol() { sed 's/\x1b\[[0-9;]*m//g'; }
check "cli: --version" "KingCobra 1.0.0" "$($BIN --version)"
check "cli: version command" "KingCobra 1.0.0" "$($BIN version)"
check "cli: help lists commands" "check [options] <file|dir>..." "$($BIN help)"
check "cli: help lists exit status" "64 usage error" "$($BIN --help)"
check "cli: help <command>" "stop after the first failure" "$($BIN help test)"
check "cli: <command> --help" "one object per problem" "$($BIN check --help)"
check "cli: help for an unknown word falls through to the language help" "No documentation found for 'zap'" "$($BIN help zap 2>&1)"
check "cli: mistyped command is suggested" "did you mean 'check'" "$($BIN chek x.kc 2>&1)"
check "cli: mistyped command exits 64" "64" "$($BIN chek x.kc >/dev/null 2>&1; echo $?)"
check "cli: mistyped option is suggested" "did you mean '--leaks'" "$($BIN --leek run x.kc 2>&1)"
check "cli: unknown option for run" "unknown option '--bogus'" "$($BIN run --bogus x.kc 2>&1)"
check "cli: missing file says why" "No such file or directory" "$($BIN run $CD/none.kc 2>&1)"
check "cli: missing file exits 66" "66" "$($BIN run $CD/none.kc >/dev/null 2>&1; echo $?)"
check "cli: a directory given to run is explained" "is a directory" "$($BIN run $CD 2>&1)"
check "cli: run without a file" "'run' needs a file" "$($BIN run 2>&1)"
check "cli: -e runs a snippet" "42" "$($BIN -e 'print(6 * 7)')"
check "cli: -e needs no final semicolon, takes statements" "6" "$($BIN -e 'let x = 3; print(x * 2)')"
check "cli: -p prints an expression" "1025" "$($BIN -p '1024 + 1')"
check "cli: -e arguments arrive as argv/argc" "[a, -b, --leaks] 3" "$($BIN -e 'print(argv, argc)' a -b --leaks)"
check "cli: -e errors show the snippet line" "print(zzz);" "$($BIN -e 'print(zzz)' 2>&1)"
check "cli: eval command" "hi" "$($BIN eval 'print("hi")')"
check "cli: run - reads the program from stdin" "from stdin" "$(echo 'print("from stdin");' | $BIN run -)"
check "cli: stdin errors name <stdin>" "<stdin>:1:" "$(echo 'print(nope);' | $BIN run - 2>&1)"
printf 'fn main(argv) { print(argv); }\n' > $CD/a.kc
check "cli: options after the script go to the script" "-DX, --leaks, --time, -h]" "$($BIN $CD/a.kc -DX --leaks --time -h)"
check "cli: run passes everything after the file" "--no-typecheck, -x]" "$($BIN run $CD/a.kc --no-typecheck -x)"
check "cli: options before the file still apply (--leaks)" "a.kc]" "$($BIN run --leaks $CD/a.kc 2>/dev/null | sed 's#.*/##')"
printf '#!/usr/bin/env kingcobra\nfn main() { print("shebang ok"); }\n' > $CD/sb.kc
check "cli: shebang line is ignored" "shebang ok" "$($BIN $CD/sb.kc)"
check "cli: --time reports phases" "parse " "$($BIN --time -e 'print(1)' 2>&1 >/dev/null)"
printf 'fn main() { let x = ; }\n' > $CD/bad.kc
check "cli: colour is off when stderr is not a terminal" "bad.kc:1:21: error:" "$($BIN check $CD/bad.kc 2>&1 | sed "s#$CD/##")"
check "cli: --color=always forces colour" "$(printf '\033')[" "$($BIN --color=always check $CD/bad.kc 2>&1)"
check "cli: NO_COLOR beats nothing but --color" "$(printf '\033')[" "$(NO_COLOR=1 $BIN --color=always check $CD/bad.kc 2>&1)"
check "cli: NO_COLOR is honoured" "error:" "$(NO_COLOR=1 FORCE_COLOR=1 $BIN check $CD/bad.kc 2>&1 | grep -c "$(printf '\033')" | sed 's/^0$/error:/')"
check "cli: FORCE_COLOR forces colour" "$(printf '\033')[" "$(FORCE_COLOR=1 $BIN check $CD/bad.kc 2>&1)"
check "cli: bad --color value" "--color expects auto, always or never" "$($BIN --color=purple -p 1 2>&1)"
check "cli: check --format=json" '"severity":"error","message":"unexpected token' "$($BIN check --format=json $CD/bad.kc)"
check "cli: check json has line and col" '"line":1,"col":21' "$($BIN check --format=json $CD/bad.kc)"
check "cli: check json prints nothing for clean files" "0" "$($BIN check --format=json $CD/a.kc | wc -l)"
mkdir -p $CD/proj/sub; printf 'fn main() { print(1); }\n' > $CD/proj/sub/ok.kc; printf 'fn f() { }\n' > $CD/proj/ok2.kc
check "cli: check walks directories" "ok   $CD/proj/ok2.kc" "$($BIN check $CD/proj)"
check "cli: check walks subdirectories" "ok   $CD/proj/sub/ok.kc" "$($BIN check $CD/proj)"
check "cli: check summary for several files" "2 file(s) checked, all ok" "$($BIN check $CD/proj)"
check "cli: check -q is silent when clean" "" "$($BIN check -q $CD/proj)"
check "cli: check exits 65 on errors" "65" "$($BIN check $CD/bad.kc >/dev/null 2>&1; echo $?)"
check "cli: check on an empty directory" "no .kc files found" "$(mkdir -p $CD/empty; $BIN check $CD/empty 2>&1)"
check "cli: check --no-typecheck only parses" "ok   $CD/tc.kc" "$(printf 'fn main() { let x: int = "s"; }\n' > $CD/tc.kc; $BIN check --no-typecheck $CD/tc.kc)"
check "cli: check without --no-typecheck catches it" "have errors" "$($BIN check $CD/tc.kc 2>&1)"
check "cli: check bad format" "--format expects text or json" "$($BIN check --format=xml $CD/a.kc 2>&1)"
printf 'fn test_a() { assert(true); }\nfn test_b() { assert(false, "nope"); }\nfn test_parse_c() { assert(1 == 1); }\n' > $CD/x_test.kc
check "cli: test -l lists without running" "x_test.kc::test_parse_c" "$($BIN test -l $CD/x_test.kc)"
check "cli: test -l does not run" "0" "$($BIN test -l $CD/x_test.kc | grep -c 'ok   ')"
check "cli: test -k filters" "1 passed, 0 failed" "$($BIN test -k parse $CD/x_test.kc)"
check "cli: test --filter=text" "1 passed, 0 failed" "$($BIN test --filter=parse $CD/x_test.kc)"
check "cli: test -x stops at the first failure" "1 passed, 1 failed" "$($BIN test -x $CD/x_test.kc | tail -1 | sed 's/  (.*//')"
check "cli: test summary lists failures as file::name" "x_test.kc::test_b" "$($BIN test $CD/x_test.kc)"
check "cli: test exits 1 on failure" "1" "$($BIN test $CD/x_test.kc >/dev/null; echo $?)"
check "cli: test -q hides passes" "0" "$($BIN test -q $CD/x_test.kc | grep -c '  ok ')"
check "cli: test -q still shows failures" "FAIL test_b" "$($BIN test -q $CD/x_test.kc)"
check "cli: test walks a directory" "test_a" "$($BIN test $CD)"
check "cli: test needs a value for -k" "needs a value" "$($BIN test -k 2>&1)"
check "cli: test unknown option" "unknown option '--zzz' for 'test'" "$($BIN test --zzz 2>&1)"
check "cli: -E expands macros" "fn main() { print(4); }" "$(printf '#define N 4\nfn main() { print(N); }\n' > $CD/p.kc; $BIN -E $CD/p.kc)"
check "cli: -E honours -D" "print(16)" "$(printf '#ifdef BIG\nfn main() { print(16); }\n#endif\n' > $CD/p2.kc; $BIN -DBIG -E $CD/p2.kc)"
check "cli: -I adds an include directory" "from inc" "$(mkdir -p $CD/incd; printf 'fn hi() -> string { return \"from inc\"; }\n' > $CD/incd/h.kc; printf '#include <h.kc>\nfn main() { print(hi()); }\n' > $CD/u.kc; $BIN -I $CD/incd $CD/u.kc)"
check "cli: -DNAME after the command works" "yes" "$(printf '#ifdef Q\nfn main() { print(\"yes\"); }\n#endif\n' > $CD/q.kc; $BIN run -DQ $CD/q.kc)"
check "cli: -D needs a name" "-D needs a macro name" "$($BIN -D 2>&1)"
check "cli: new creates a project" "created  $CD/np/main.kc" "$($BIN new $CD/np)"
check "cli: new project runs" "Hello, Ada!" "$($BIN run $CD/np/main.kc Ada)"
check "cli: new project tests pass" "1 passed, 0 failed" "$($BIN test $CD/np)"
check "cli: new refuses an existing directory" "already exists" "$($BIN new $CD/np 2>&1)"
check "cli: init never overwrites" "exists   main.kc (left alone)" "$(cd $CD/np && $BINABS init)"
check "cli: completions bash is valid shell" "ok" "$($BIN completions bash | bash -n && echo ok)"
check "cli: completions bash lists commands" "preprocess" "$($BIN completions bash)"
check "cli: completions zsh" "#compdef kingcobra kc" "$($BIN completions zsh)"
check "cli: completions fish" "complete -c kingcobra" "$($BIN completions fish)"
check "cli: completions unknown shell" "unknown shell 'pwsh'" "$($BIN completions pwsh 2>&1)"
check "cli: info shows the version and modules" "modules      system" "$($BIN info)"
check "cli: modules lists std modules" "json" "$($BIN modules)"
check "cli: repl --help" "Starts the REPL" "$($BIN repl --help)"

# ---------------------------------------------------------------- v0.9.1: help()
hl() { lang_run "$1"; }
check "help: help() gives an overview" "help(\"topics\")" "$(hl 'fn main() { help(); }')"
check "help: a builtin by value" "len(x) -> int" "$(hl 'fn main() { help(len); }')"
check "help: a builtin by name" "sorted(array [, cmp(a, b)]) -> array" "$(hl 'fn main() { help("sorted"); }')"
check "help: a C builtin" "printf(format, ...) -> bytes written" "$(hl 'fn main() { help(printf); }')"
check "help: a keyword" "execution FALLS THROUGH" "$(hl 'fn main() { help("switch"); }')"
check "help: a string method" "s.split(separator) -> array" "$(hl 'fn main() { help("string.split"); }')"
check "help: an array method" "a.sort([cmp(a, b)])" "$(hl 'fn main() { help("array.sort"); }')"
check "help: a name that is both a builtin and a method shows both" "array method" "$(hl 'fn main() { help("map"); }')"
check "help: a topic" "int        64-bit integer" "$(hl 'fn main() { help("types"); }')"
check "help: topics list" "preprocessor" "$(hl 'fn main() { help("topics"); }')"
check "help: builtins list" "c: ctype:" "$(hl 'fn main() { help("builtins"); }')"
check "help: keywords list" "goto" "$(hl 'fn main() { help("keywords"); }')"
check "help: methods list" "string methods:" "$(hl 'fn main() { help("methods"); }')"
check "help: the type name string" "s.upper() -> string" "$(hl 'fn main() { help("string"); }')"
check "help: a user function shows its signature" "fn add(a: int, b: int = ...) -> int" "$(hl 'fn add(a: int, b: int = 2) -> int { return a + b; } fn main() { help(add); }')"
check "help: /// comment is the function's documentation" "Adds two numbers." "$(hl $'/// Adds two numbers.\nfn add(a: int, b: int) -> int { return a + b; }\nfn main() { help(add); }')"
check "help: multi-line doc comment" "Second line." "$(hl $'/// First line.\n/// Second line.\nfn f() { }\nfn main() { help(f); }')"
check "help: undocumented function says how to document it" "(no documentation: put" "$(hl 'fn f() { } fn main() { help(f); }')"
check "help: a function by name finds the user's definition" "Doubles it." "$(hl $'/// Doubles it.\nfn dbl(x: int) -> int { return x * 2; }\nfn main() { help("dbl"); }')"
check "help: a doc comment does not leak to the next declaration" "(no documentation" "$(hl $'/// Only for a.\nfn a() { }\nfn b() { }\nfn main() { help(b); }')"
check "help: //// (four slashes) is an ordinary comment" "(no documentation" "$(hl $'//// not a doc\nfn f() { }\nfn main() { help(f); }')"
check "help: struct with its doc comment" "A point in the plane." "$(hl $'/// A point in the plane.\nstruct Point { x: int, y: int }\nfn main() { help("Point"); }')"
check "help: struct fields and methods" "norm(self, ...)" "$(hl $'struct P { x: int }\nimpl P { fn norm(self) -> int { return self.x; } }\nfn main() { help("P"); }')"
check "help: a struct instance" "an instance of struct P" "$(hl $'struct P { x: int }\nfn main() { help(P { x: 1 }); }')"
check "help: Type.method" "fn norm(self) -> int" "$(hl $'struct P { x: int }\nimpl P { fn norm(self) -> int { return self.x; } }\nfn main() { help("P.norm"); }')"
check "help: B-style function with a doc comment" "Greets." "$(hl $'/// Greets.\ngreet(who) { printf("hi %s\\n", who); }\nmain() { help(greet); }')"
check "help: an imported module lists its functions" "stringify" "$(hl $'import std.json;\nfn main() { help(json); }')"
check "help: a module function points to its reference" "docs/STD_JSON.md" "$(hl $'import std.json;\nfn main() { help(json.parse); }')"
check "help: an unloaded module says how to load it" "import std.regx;" "$(hl 'fn main() { help("std.regx"); }')"
check "help: modules list" "system" "$(hl 'fn main() { help("modules"); }')"
check "help: a value's type" "a value of type 'int'" "$(hl 'fn main() { help(5); }')"
check "help: an array value lists its methods" "a.push(value)" "$(hl 'fn main() { help([1, 2]); }')"
check "help: a typo is suggested" "Did you mean 'sorted'" "$(hl 'fn main() { help("sortd"); }')"
check "help: an unknown name is a message, not an error" "No documentation found for 'zzzz'" "$(hl 'fn main() { help("zzzz"); }')"
check "help: too many arguments" "usage: help([thing])" "$(hl 'fn main() { help(1, 2); }')"
check "help: every builtin is documented" "undocumented: none" "$(hl 'fn main() { help("__undocumented"); }')"
check "help: a user definition named like a builtin does not break help" "A mine." "$(hl $'/// A mine.\nfn clamp(x) { return x; }\nfn main() { help("clamp"); }')"
check "help: refused in comptime" "comptime" "$(hl 'comptime { help(); } fn main() { }')"
check "help: kingcobra help <topic>" "map(array, fn(x)) -> array" "$($BIN help map)"
check "help: kingcobra help still shows commands" "test [options] [file|dir]" "$($BIN help test)"
check "help: kingcobra help topics" "preprocessor" "$($BIN help topics)"
check "repl: bare help" "help(\"topics\")" "$(repl 'help')"
check "repl: ?name" "len(x) -> int" "$(repl '?len')"
check "repl: :help name" "strlen(s) -> int" "$(repl ':help strlen')"
check "repl: doc comment waits for its declaration" "Adds two numbers." "$(repl $'/// Adds two numbers.\nfn add(a: int, b: int) -> int { return a + b; }\nhelp(add)')"
check "repl: help of a REPL-defined function shows its input" "defined at <repl:1>" "$(repl $'fn f(a) { return a; }\nhelp(f)')"

# ---------------------------------------------------------------- v0.9.2: fixes
check "fn typo: f(...) { } is explained" "looks like a function written with the wrong keyword" "$(lang_run 'fn main() { let add = f(x, y) { return x + y; }; }')"
check "fn typo: f(x: int) -> int { } is explained" "use \`fn\`" "$(lang_run 'fn main() { let add = f(x: int, y: int) -> int { return x + y; }; }')"
check "fn typo: func(a) { } is explained" "use \`fn\`" "$(lang_run 'fn main() { let g = func(a) { return a; }; }')"
check "fn typo: an ordinary call named f is untouched" "7" "$(lang_run 'fn f(x: int) -> int { return x + 1; } fn main() { print(f(6)); }')"
check "fn typo: a call named f followed by an if-block still works" "ok" "$(lang_run 'fn f(x: int) -> bool { return true; } fn main() { if (f(1)) { print("ok"); } }')"
check "repl: closure with typed params called later" "5" "$(repl $'let add = fn(x: int, y: int) -> int {\n  return x+y;\n}\nadd(2, 3)')"
check "repl: anonymous closure survives errors in earlier inputs" "8100" "$(repl $'let a = f(x: int) -> int { return x; }\nlet b = fn(x) -> int { return x * x; }\nb(90)')"
check "repl: calling a closure with too few arguments is a clean error" "expects 1 argument(s), got 0" "$(repl $'let b = fn(x) -> int { return x * x; }\nb()')"
check "build: objects are kept in build/<version>/" "yes" "$([ -d build/$($BIN --version | sed 's/KingCobra //') ] && echo yes)"
check "build: no stray objects next to the sources" "0" "$(find compiler std -name '*.o' | wc -l)"

# ---------------------------------------------------------------- v0.9.3: JIT falls back to the interpreter
JITFN='fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } fn sq(x: int) -> int { return x * x; } fn main() { var s = 0; for (i in 0..300) { s += sq(i); } print(fib(20), s); }'
JITWANT="6765 8955050"
check "jit: default run is correct" "$JITWANT" "$(lang_run "$JITFN")"
check "jit: --no-jit gives identical results" "$JITWANT" "$(echo "$JITFN" > /tmp/kc_jit_t.kc; $BIN --no-jit run /tmp/kc_jit_t.kc 2>&1)"
check "jit: KC_JIT=0 gives identical results" "$JITWANT" "$(KC_JIT=0 $BIN run /tmp/kc_jit_t.kc 2>&1)"
check "jit: info reports the opt-out" "disabled by user" "$($BIN --no-jit info)"
if [[ "$HOST_ARCH" == "x86_64" ]]; then
    check "jit: info reports the JIT when usable" "closure JIT  available" "$(env -u KC_JIT $BIN info)"
else
    check "jit: no JIT on $HOST_ARCH -> info says so, interpreter used" "no JIT for this CPU ($HOST_ARCH)" "$(env -u KC_JIT $BIN info)"
fi
if [[ "$(uname -s)" == "Linux" && "$HOST_ARCH" == "x86_64" ]] && command -v cc >/dev/null; then
    cat > /tmp/kc_noexec.c <<'EOS'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>
int mprotect(void *a, size_t n, int prot) {
    static int (*real)(void *, size_t, int);
    if (!real) real = dlsym(RTLD_NEXT, "mprotect");
    const char *m = getenv("KC_TEST_NOEXEC");
    if ((prot & PROT_EXEC) && m) { if (m[0] == 'd') { errno = EACCES; return -1; } if (m[0] == 'i') return 0; }
    return real(a, n, prot);
}
EOS
    if cc -shared -fPIC -o /tmp/kc_noexec.so /tmp/kc_noexec.c -ldl 2>/dev/null; then
        check "jit: OS denies executable memory -> interpreter, same results" "$JITWANT" "$(KC_TEST_NOEXEC=d LD_PRELOAD=/tmp/kc_noexec.so $BIN run /tmp/kc_jit_t.kc 2>&1)"
        check "jit: OS denies executable memory -> info says why" "does not allow executable memory" "$(KC_TEST_NOEXEC=d LD_PRELOAD=/tmp/kc_noexec.so $BIN info)"
        check "jit: exec page faults -> self-test catches it, same results" "$JITWANT" "$(KC_TEST_NOEXEC=i LD_PRELOAD=/tmp/kc_noexec.so $BIN run /tmp/kc_jit_t.kc 2>&1)"
        check "jit: exec page faults -> info says self-test failed" "self-test failed" "$(KC_TEST_NOEXEC=i LD_PRELOAD=/tmp/kc_noexec.so $BIN info)"
        check "jit: stack-overflow guard survives a failed probe" "stack overflow (recursion too deep)" "$(KC_TEST_NOEXEC=i LD_PRELOAD=/tmp/kc_noexec.so $BIN run <(echo 'fn d(n: int) -> int { return d(n + 1) + 1; } fn main() { try { d(0); } catch (e) { print(e.message); } }') 2>&1)"
    fi
fi

# ---------------------------------------------------------------- v0.10.0: ARM64 / ARMhf / RISC-V support
check "arch: info names the architecture and word size" "word size    " "$($BIN info)"
check "arch: system.arch() matches info" "$HOST_ARCH" "$(lang_run 'import std.system; fn main() { print(system.arch()); }')"
check "arch: rhex.arch() matches info" "$HOST_ARCH" "$(rhex_run 'print(rhex.arch());')"
case "$HOST_ARCH" in
    x86_64|aarch64|armhf|arm|riscv64|riscv32|x86)
        check "arch: raw machine code runs on $HOST_ARCH" "arch exec: 42" "$($BIN run examples/std_rhex_arch.kc)"
        check "arch: raw machine code takes arguments on $HOST_ARCH" "arch call_args(19, 23) -> 42" "$($BIN run examples/std_rhex_arch.kc)" ;;
    *)
        check "arch: unknown CPU refuses raw machine code cleanly" "no raw machine code known for" "$($BIN run examples/std_rhex_arch.kc)"
        check "arch: rhex.exec refuses on an unknown CPU" "has no raw-code execution support" "$(rhex_run 'unsafe { rhex.exec([0xC3]); }')" ;;
esac
if [[ "$HOST_ARCH" != "x86_64" ]]; then
    check "arch: x86-only instruction gives a clear error" "is an x86-64 instruction and is not available on $HOST_ARCH" "$(rhex_run 'print(rhex.rdtsc());')"
fi
check "arch: pause/fence work everywhere" "ok" "$(rhex_run 'rhex.pause(); rhex.fence(); rhex.fence("read"); rhex.fence("write"); print("ok");')"
check "arch: hot recursive function is correct (JIT or interpreter)" "75025" "$(lang_run 'fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } fn main() { print(fib(25)); }')"
check "arch: FFI integer args are passed as machine words" "42" "$(printf 'extern fn abs(x: int) -> int from "libc.so.6";\nfn main() { print(abs(-42)); }\n' > /tmp/kc_arch_ffi.kc; $BIN run /tmp/kc_arch_ffi.kc 2>&1)"
check "arch: float->int is saturating and identical on every CPU" "9223372036854775807 -9223372036854775808 2 -2" "$(lang_run 'fn main() { print(int(1e30), int(-1e30), int(2.9), int(-2.9)); }')"
check "arch: char signedness does not change string handling" "ok" "$(lang_run 'fn main() { let s = "café ÿ~"; if (len(s) > 0 && s.upper().len() == len(s)) { print("ok"); } else { print("bad"); } }')"

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
