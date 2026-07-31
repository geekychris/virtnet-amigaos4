#!/usr/bin/env bash
# One-shot iteration: build → deploy virte1000.device + tests → run each
# test → print PASS/FAIL summary → exit 0 iff every test passed.
#
# Usage:
#   ./scripts/iterate.sh                          # runs the FULL suite
#   ./scripts/iterate.sh testopen                 # just this one
#   ./scripts/iterate.sh testopen testdeviceq     # named subset
#
# The full suite is whichever binaries in build/ match testopen-family
# naming (test*), discovered automatically after build. This keeps the
# suite honest — every new tests/<name>.c added to the Makefile TEST_NAMES
# list is picked up without editing this script.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== build ==="
"$HERE/scripts/build.sh" >/dev/null

# Auto-discover tests from the build output if no args given.
if [[ $# -eq 0 ]]; then
  mapfile -t TESTS < <(cd "$HERE/build" && ls test* 2>/dev/null | sort)
  if [[ ${#TESTS[@]} -eq 0 ]]; then
    echo "iterate: no test* binaries in build/ — check Makefile TEST_NAMES" >&2
    exit 2
  fi
  echo "=== suite: ${TESTS[*]} ==="
else
  TESTS=("$@")
fi

echo "=== deploy ==="
"$HERE/scripts/deploy.sh" "${TESTS[@]}"

# Run + tally.
pass=0; fail=0
declare -a fail_names=()
for t in "${TESTS[@]}"; do
  echo "=== run $t ==="
  if "$HERE/scripts/run-test.sh" "$t"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1)); fail_names+=("$t")
  fi
done

echo ""
echo "=== SUITE SUMMARY ==="
echo "  passed: $pass"
echo "  failed: $fail"
if (( fail > 0 )); then
  echo "  failing tests: ${fail_names[*]}"
  exit 1
fi
