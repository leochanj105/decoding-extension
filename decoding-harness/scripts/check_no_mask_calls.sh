#!/usr/bin/env bash
# Evidence that no token mask is built or filled, in two parts.
#
# XGrammar builds with -flto=auto, so the linker drops functions nothing can reach
# and inlines small ones. That decides how each half is checked.
#
#   runtime mask generation -- FillNextTokenBitmask and the functions behind it.
#     Checked statically: they must be absent from the test executable, which is
#     stronger than a breakpoint (code that is not linked cannot run). Control: they
#     are present in libxgrammar.a, so absence means "unreachable", not "misnamed".
#
#   compile-time mask building -- GetAdaptiveTokenMask and its callers are inlined
#     and cloned by LTO, and breakpoints on function names missed them (the control
#     below read zero while ten masks were built). So the breakpoints are on SOURCE
#     LINES, which gdb places at every inlined copy: the call that builds one mask per
#     grammar position, and the two lines that store a mask in the compiled cache.
#     Zero hits on the checker path, and a nonzero count when the tests' vocabulary
#     control runs, which shows the breakpoints are live.
set -euo pipefail
EXE="$1"
LIB="$2"
fail=0

RUNTIME='FillNextTokenBitmask|FillBitmaskForStates|FillBitmaskForCharBudgetBoundary|BatchFillNextTokenBitmask|TraverseDraftTree|FindJumpForwardString'
in_lib="$(nm -C "$LIB" 2>/dev/null | grep -oE "($RUNTIME)\(" | sort -u | tr -d '(' | tr '\n' ' ')"
in_exe="$(nm -C "$EXE" | grep -oE "($RUNTIME)\(" | sort -u | tr -d '(' | tr '\n' ' ' || true)"
echo "runtime mask functions in libxgrammar.a: ${in_lib:-none}"
echo "runtime mask functions in the executable: ${in_exe:-none}"
[ -n "$in_lib" ] || { echo "  the control found none in the library, so absence proves nothing"; fail=1; }
[ -z "$in_exe" ] || fail=1

count_hits() {
  gdb -q -batch -nx -ex 'set pagination off' -ex 'python
import gdb
# Counted in stop(): this gdb leaves hit_count at 0 when stop() returns False.
hits = [0]
class Count(gdb.Breakpoint):
    def stop(self):
        hits[0] += 1
        return False
spots = [Count(s) for s in ("grammar_compiler.cc:1136", "grammar_compiler.cc:1138",
                            "grammar_compiler.cc:1169")]
gdb.execute("run")
print("LOCATIONS %d" % sum(1 for b in spots if b.location))
print("HITS %d" % hits[0])
' "$EXE" 2>&1
}
out1="$(HARNESS_SKIP_CONTROL=1 count_hits)"
out2="$(count_hits)"
locs="$(grep -oP '^LOCATIONS \K[0-9]+' <<<"$out1" || echo 0)"
hits1="$(grep -oP '^HITS \K[0-9]+' <<<"$out1" || echo -1)"
hits2="$(grep -oP '^HITS \K[0-9]+' <<<"$out2" || echo -1)"
echo "breakpoints placed in the mask-building loop: $locs"
echo "hits on the checker path:     $hits1"
echo "hits with the control running: $hits2"
[ "$locs" -gt 0 ] && [ "$hits1" -eq 0 ] && [ "$hits2" -gt 0 ] || fail=1
exit $fail
