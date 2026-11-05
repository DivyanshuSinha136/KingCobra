#!/bin/sh
# Compile every gNN_*.kc with kcc, run the executable, compare with gNN_*.expected.
# `sh run.sh --update` rewrites the expected files (review the diff before committing!).
cd "$(dirname "$0")/../../.." || exit 1
fail=0
for f in kcc/tests/grammar/g[0-9]*_*.kc; do
  case "$f" in *_mod_*) continue;; esac
  exp="${f%.kc}.expected"
  out=/tmp/kcc_grammar_$$
  if ! ./kingcobra kcc/src/main.kc "$f" -o "$out" >/tmp/kcc_grammar_log 2>&1; then echo "FAIL (compile) $f"; cat /tmp/kcc_grammar_log; fail=1; continue; fi
  got=$("$out" 2>&1)
  if [ "$1" = "--update" ]; then printf '%s\n' "$got" > "$exp"; echo "updated $exp"; continue; fi
  if [ "$got" = "$(cat "$exp")" ]; then echo "ok   $f"; else echo "FAIL $f"; echo "$got" | diff - "$exp" | head -10; fail=1; fi
  rm -f "$out"
done
exit $fail
