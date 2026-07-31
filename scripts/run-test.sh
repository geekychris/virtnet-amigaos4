#!/usr/bin/env bash
# Run one test binary on the OS4 guest, print stdout, exit 0 on PASS.
#
# Tests must have been pushed to DH1:<name> first (see deploy.sh).
# Convention: tests print `RESULT: PASS` or `RESULT: FAIL <reason>` as
# their LAST line. This script exits 0 iff `RESULT: PASS` appears in
# the captured output.
#
# Usage:
#   ./scripts/run-test.sh testopen
#   ./scripts/run-test.sh testopen --timeout 10
set -euo pipefail

API="http://localhost:3000"

TEST="${1:-}"
if [[ -z "$TEST" ]]; then
  echo "usage: $0 <test-name-on-DH1> [--timeout <seconds>]" >&2
  exit 2
fi
shift || true

TIMEOUT=20   # default gives testrx_cooked's poll loop (5 s wall-clock)
             # + reply time + margin. Override with --timeout N.
if [[ "${1:-}" == "--timeout" ]]; then
  TIMEOUT="${2:-20}"; shift 2
fi

OUT="RAM:${TEST}.out"

curl -fs -X POST "$API/api/launch" -H 'Content-Type: application/json' \
  -d "{\"command\":\"delete $OUT QUIET\"}" >/dev/null || true

resp=$(curl -fs -X POST "$API/api/launch" -H 'Content-Type: application/json' \
  -d "{\"command\":\"DH1:$TEST >$OUT\"}")
echo "launch: $resp" >&2

# Poll until file size stabilises (two consecutive reads agree) or
# timeout. NOTE: devbench's /api/file returns `size` = bytes REMAINING
# from the given offset, not total file size. To get total we read the
# whole file (up to 4 KB probe) and measure hexData length. When two
# consecutive polls return the same hex length, the test is done writing.
prev_hex_len=-1
elapsed=0
while (( elapsed < TIMEOUT )); do
  sleep 1; elapsed=$((elapsed + 1))
  info=$(curl -fs "$API/api/file?path=$OUT&offset=0&size=4096") || continue
  hex_len=$(python3 -c "import sys, json; print(len(json.load(sys.stdin).get('hexData','')))" <<<"$info")
  if [[ "$hex_len" -gt 0 && "$hex_len" == "$prev_hex_len" ]]; then break; fi
  prev_hex_len="$hex_len"
done

body=$(curl -fs "$API/api/file?path=$OUT&offset=0&size=65536" \
       | python3 -c "
import sys, json
d = json.load(sys.stdin)
sys.stdout.write(bytes.fromhex(d.get('hexData','')).decode('latin-1'))
")
printf '%s' "$body"

# Suite verdict — last `RESULT:` line wins. Absent = FAIL (test crashed
# or ran past the polling timeout without producing final output).
verdict=$(printf '%s' "$body" | grep -oE 'RESULT: (PASS|FAIL.*)' | tail -1 || true)
if [[ "$verdict" == "RESULT: PASS" ]]; then exit 0
elif [[ -n "$verdict" ]]; then exit 1
else
  echo "[run-test] no RESULT line in output (test truncated? timeout=${TIMEOUT}s)" >&2
  exit 2
fi
