#!/usr/bin/env bash
# Build XGrammar, build the harness, then run the tests and the mask checks.
# Any failure, or a check that could not prove anything, exits non-zero.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"

"$HERE/scripts/build_xgrammar.sh"
"$HERE/scripts/build_harness.sh"

for src in "$HERE"/tests/test_*.cc; do
  echo "--- $(basename "${src%.cc}") ---"
  "$BUILD/$(basename "${src%.cc}")"
done

echo "--- what the harness calls in XGrammar ---"
for obj in "$BUILD"/*.o; do
  echo "  $(basename "$obj"):"
  nm -C --undefined-only "$obj" | awk '/ U xgrammar::/ {sub(/^ *U /, ""); sub(/\(.*/, ""); print "    " $0}' | sort -u
done
if nm -C --undefined-only "$BUILD"/*.o | grep -qE 'Bitmask|AdaptiveTokenMask|JumpForward|DraftTree'; then
  echo "FAIL  the harness references a mask function"; exit 1
fi
echo "pass  the harness references no mask function"

echo "--- mask code: absent from the binary, and never entered ---"
if "$HERE/scripts/check_no_mask_calls.sh" "$BUILD/test_prefix_checker" "$BUILD/xgrammar/libxgrammar.a"; then
  echo "pass  no mask function ran"
else
  echo "FAIL  the mask check failed or proved nothing"; exit 1
fi
