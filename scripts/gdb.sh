#!/bin/bash
#
# gdb.sh — driver-focused GDB wrapper. Connects to QEMU's PPC gdbstub,
# runs a canned command set (or arbitrary GDB script), and cleans up
# stale docker containers that would otherwise hold the stub port open.
#
# The QEMU sam460ex CPU has NO SYMBOLS loaded. Everything is raw
# PC/register/memory inspection. See COMMANDS below for what works.
#
# Usage:
#   ./scripts/gdb.sh halt              # attach, halt CPU, dump state, detach
#   ./scripts/gdb.sh watch-dsi         # break on DSI vector, dump on hit
#   ./scripts/gdb.sh break <addr>      # break at PC, continue, dump on hit
#   ./scripts/gdb.sh script <file>     # run arbitrary GDB script from host path
#   ./scripts/gdb.sh raw '<cmds>'      # run arbitrary GDB commands inline
#   ./scripts/gdb.sh mon '<cmd>'       # QEMU monitor cmd (needs -monitor tcp::4444)
#   ./scripts/gdb.sh disasm <addr> [n] # disassemble n=16 insns at addr via monitor
#   ./scripts/gdb.sh regs              # dump all CPU regs via monitor (incl. DEAR)
#
# Env:
#   GDB_PORT  — QEMU gdbstub port (default 4433; port 1234 conflicts with LM Studio)
#   GDB_IMAGE — docker image (default amiga-devbench-gdb:latest)
#
# The QEMU startup script's --gdb defaults to port 1234, but LM Studio
# uses that. Always pass --gdb-port 4433 to start-qemu-os4.sh, and this
# script defaults to reading GDB_PORT=4433.

set -e

GDB_PORT="${GDB_PORT:-4433}"
GDB_IMAGE="${GDB_IMAGE:-amiga-devbench-gdb:latest}"
TMP_DIR="/Users/chris/tmp_gdb"

# ── stale-container cleanup ──────────────────────────────────────────
# QEMU's gdbstub only accepts one client at a time. If a prior GDB
# container is still running, its connection blocks new ones. Kill any
# lingering amiga-devbench-gdb containers before we proceed.
mkdir -p "$TMP_DIR"
LINGERING=$(docker ps -q --filter "ancestor=${GDB_IMAGE}" 2>/dev/null)
if [ -n "$LINGERING" ]; then
    docker kill $LINGERING >/dev/null 2>&1 || true
    sleep 2   # let TCP release
fi

# ── verify gdbstub reachable ─────────────────────────────────────────
if ! nc -z 127.0.0.1 "$GDB_PORT" 2>/dev/null; then
    echo "ERROR: no gdbstub on 127.0.0.1:$GDB_PORT" >&2
    echo "start QEMU with: scripts/start-qemu-os4.sh --gdb --gdb-port $GDB_PORT" >&2
    exit 1
fi

# ── build the GDB script ─────────────────────────────────────────────
SCRIPT="$TMP_DIR/session-$$.gdb"
cat > "$SCRIPT" <<HDR
set architecture powerpc:common
set pagination off
set confirm off
set remotetimeout 60
target remote host.docker.internal:$GDB_PORT
HDR

CMD="${1:-halt}"
case "$CMD" in
halt)
    cat >> "$SCRIPT" <<'BODY'
interrupt
printf "=== CPU HALTED — current state ===\n"
printf "PC   = 0x%08x    (current program counter)\n", $pc
printf "LR   = 0x%08x    (link register — return address)\n", $lr
printf "CTR  = 0x%08x    (count register — target of bctrl)\n", $ctr
printf "R1   = 0x%08x    (stack pointer)\n", $r1
printf "MSR  = 0x%08x    (machine state)\n", $msr
printf "SRR0 = 0x%08x    (interrupted PC if in exception)\n", $srr0
printf "SRR1 = 0x%08x    (interrupted MSR if in exception)\n", $srr1
printf "DEAR = 0x%08x    (data effective addr — target of DSI)\n", $dear
printf "ESR  = 0x%08x    (exception syndrome)\n", $esr
printf "IVPR = 0x%08x    (interrupt vector prefix)\n", $ivpr
printf "\n== disasm at PC (16 instructions) ==\n"
x/16i $pc
printf "\n== stack (16 words at SP) ==\n"
x/16xw $r1
BODY
    ;;
watch-dsi)
    cat >> "$SCRIPT" <<'BODY'
# IVOR2 = DSI vector offset. Actual addr = IVPR | IVOR2.
# On sam460ex OS4 boot IVPR is typically 0 and IVOR2=0x30000.
break *0x30000
commands
  silent
  printf "\n===== DSI FIRED =====\n"
  printf "SRR0 (interrupted PC) = 0x%08x\n", $srr0
  printf "SRR1 (interrupted MSR)= 0x%08x\n", $srr1
  printf "DEAR (bad data addr)  = 0x%08x\n", $dear
  printf "ESR  (syndrome bits)  = 0x%08x\n", $esr
  printf "LR   (caller)         = 0x%08x\n", $lr
  printf "R1   (SP)             = 0x%08x\n", $r1
  printf "\n-- 16 insns before SRR0 --\n"
  x/16i $srr0-64
  printf "\n-- 16 insns AT SRR0 --\n"
  x/16i $srr0
  printf "\n-- 8 stack words --\n"
  x/8xw $r1
  printf "(continuing)\n"
  continue
end
continue
BODY
    ;;
break)
    ADDR="${2:?usage: $0 break <hex-addr>}"
    cat >> "$SCRIPT" <<BODY
break *${ADDR}
commands
  silent
  printf "\n===== BREAKPOINT HIT AT ${ADDR} =====\n"
  printf "LR=0x%08x  SP=0x%08x\n", \$lr, \$r1
  info registers
  printf "\n-- 16 insns AT PC --\n"
  x/16i \$pc
  quit
end
continue
BODY
    ;;
script)
    FILE="${2:?usage: $0 script <path>}"
    if [ ! -f "$FILE" ]; then echo "no such file: $FILE" >&2; exit 1; fi
    cat "$FILE" >> "$SCRIPT"
    ;;
raw)
    CMDS="${2:?usage: $0 raw '<cmds>'}"
    printf '%s\n' "$CMDS" >> "$SCRIPT"
    ;;
help|-h|--help)
    sed -n '2,28p' "$0"
    exit 0
    ;;
mon)
    MON_CMD="${2:?usage: $0 mon '<qemu-monitor-cmd>'}"
    exec python3 -c "
import socket, time, sys
s = socket.socket(); s.settimeout(5); s.connect(('127.0.0.1', 4444))
time.sleep(0.3)
try:
  while True:
    d=s.recv(4096)
    if not d: break
except: pass
s.sendall(sys.argv[1].encode()+b'\n')
time.sleep(1.5); s.settimeout(2); buf=b''
try:
  while True:
    d=s.recv(4096)
    if not d: break
    buf+=d
except: pass
# strip terminal-escape noise that QEMU echoes back
out=buf.decode('latin-1', errors='replace')
import re; out=re.sub(r'\x1b\[[0-9;?]*[a-zA-Z]', '', out)
out=re.sub(r'.\x08', '', out)  # BS erases prev char
print(out)
" "$MON_CMD"
    ;;
disasm)
    ADDR="${2:?usage: $0 disasm <addr> [count]}"
    N="${3:-16}"
    exec "$0" mon "x /${N}i $ADDR"
    ;;
regs)
    exec "$0" mon "info registers"
    ;;
*)
    echo "unknown command: $CMD" >&2
    "$0" help >&2
    exit 1
    ;;
esac

# ── run GDB via docker ───────────────────────────────────────────────
docker run --rm -i \
  -v "${TMP_DIR}:/host" \
  "$GDB_IMAGE" \
  gdb-multiarch --batch -x "/host/$(basename "$SCRIPT")" 2>&1

rm -f "$SCRIPT"
