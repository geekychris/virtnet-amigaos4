#!/usr/bin/env bash
# Push virte1000.device (and optionally test binaries) to DH1: on the OS4
# guest via the amiga_mcp devbench REST API on localhost:3000.
#
# Usage:
#   ./scripts/deploy.sh                    # just virte1000.device
#   ./scripts/deploy.sh testopen           # + build/testopen
#   ./scripts/deploy.sh testopen testrx    # + multiple tests
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
API="http://localhost:3000"

# Fail fast if devbench isn't listening — much clearer than a curl-refused
# error at each push. jq is not required (we use python3 for JSON), but the
# status probe uses grep so we don't add a dep.
if ! curl -fs "$API/api/status" >/dev/null; then
  echo "deploy: devbench REST not reachable at $API — is 'python -m amiga_devbench' running?" >&2
  exit 2
fi
if ! curl -fs "$API/api/status" | python3 -c 'import sys, json; d=json.load(sys.stdin); sys.exit(0 if d.get("connected") else 3)'; then
  echo "deploy: devbench is up but bridge is not connected to the guest" >&2
  exit 3
fi

push_one() {
  local src="$1" dest="$2"
  echo "→ $src  →  $dest"
  curl -fs -X POST "$API/api/transfer" \
    -H 'Content-Type: application/json' \
    -d "{\"source\":\"$src\",\"dest\":\"$dest\",\"direction\":\"push\"}" \
  | python3 -c '
import sys, json
d = json.load(sys.stdin)
if not d.get("success"):
    print("  FAIL:", d, file=sys.stderr); sys.exit(1)
crc = "ok" if d["crc_match"] else "MISMATCH"
print("  ok:", d["bytes"], "bytes in", d["elapsed"], "s, crc", crc)
'
}

push_one "$HERE/build/virtnet.device.debug" "DH1:virtnet.device"

for t in "$@"; do
  bin="$HERE/build/$t"
  if [[ ! -f "$bin" ]]; then
    echo "deploy: no such binary: $bin (did you build it?)" >&2
    exit 1
  fi
  push_one "$bin" "DH1:$t"
  # Ensure it's executable on the guest — new files may lack the +e bit.
  curl -fs -X POST "$API/api/launch" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"protect DH1:$t +rwed\"}" >/dev/null
done

echo "deploy: done."
