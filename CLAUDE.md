# virtnet — Claude project instructions

virtio-net SANA-II driver for AmigaOS 4.1 PPC under QEMU sam460ex.
Sibling to `amiga_mcp`, `virte1000` (e1000 driver), `python-amigaos4`.
Repo evolved from a fork of `virte1000` — much of the SANA-II layer,
tests, build system, and design doc are shared.

## Current status (2026-08)

Working end-to-end. `virtnet.device` deployed to `DEVS:Networks/`,
Roadshow binds interface `192.168.101.15` on subnet `101.0/24`,
ARP + TCP + sustained data all confirmed. Perf: **25 Mbit/s
sustained, 0 retransmissions**. Baseline `virte1000` is ~40 Mbit/s
on the same host — ~35 % gap is the current optimization target.

See `docs/PROGRESS.md` for the fix trail.

## What "done" looks like

Roadshow binding + reliable TCP is done. Remaining "polish" goals:

- Match or exceed `virte1000` throughput (candidates:
  VIRTIO_NET_F_CSUM, VIRTIO_RING_F_EVENT_IDX, RX refill batching).
- Land in `SYS:Kickstart/` + `Kicklayout` so the driver auto-loads
  at boot without depending on `DEVS:Networks/`. Only do this after
  a few days of stability testing — a broken Kickstart driver
  blocks the whole boot path.
- Optional: modern virtio-net-pci (device 0x1041) support for
  MSI-X + multi-queue. Legacy is fine for our current throughput
  goals.

## Toolchain

Docker image: **`walkero/amigagccondocker:os4-gcc11`** (arm64 host).
Same one `python-amigaos4/build.sh` uses. Pull once:
```
docker pull walkero/amigagccondocker:os4-gcc11-arm64
```

Cross-compile flags (from `python-amigaos4/build.sh` — proven):
```
-mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall
-D__PPC__ -D__USE_INLINE__ -D__USE_OLD_TIMEVAL__
```

For a `.device`, we want a **resident-tag executable** — usually
built with `-nostartfiles -Wl,-Ttext=0x0` and a hand-written
resident tag in the entry point. Study `VirtualSCSIDevice`'s
`Makefile` + `device.c` for the exact link line.

## SDK

`/Users/chris/code/claude_world/refs/os4-sdk/base/` — see the
`os4-sdk-location` memory. The parts you need:
- `Include/devices/sana2.h` — SANA-II command constants +
  IOSana2Req struct
- `Include/interfaces/pci.h`, `Include/proto/pci.h` — PCI bus
  enumeration
- `Include/exec/*` — resident tag, library base, port/msg
- `Documentation/AutoDocs/*.doc` — SANA-II reference (search for
  "sana-II" in doc filenames)

## Testing loop

Use the `amiga_mcp` devbench MCP tools (talks to the amiga-bridge
daemon over TCP through QEMU host-forwarded port 2347):

- `amiga_push_file(local, DH1:...)` — copy build artifact into
  the guest
- `amiga_dos_command("...")` — run AmigaDOS commands
- `amiga_screenshot()` — visual check of guest state
- `amiga_last_crash` — GrimReaper capture on driver crash

Full driver test cycle:
1. `docker run ... make` in this dir → `virte1000.device`
2. `amiga_push_file` → `DH1:virte1000.device`
3. `amiga_dos_command("copy DH1:virte1000.device SYS:Kickstart/")`
4. `amiga_dos_command("reboot")` (a bad driver may need
   `--install` boot instead — see amiga_mcp docs)
5. Wait for boot; `amiga_dos_command("mount NET:")` or whatever
   the current network mount is
6. Test with `amiga_dos_command("ping 10.0.2.2")`, etc.

For **crash iteration** — while developing, don't put it in
Kickstart! Load it as a normal library via
`OpenDevice("virte1000.device", 0, req, 0)` from a small test
program. That way a crash just kills the test, not the guest.

## QEMU config

The guest's NIC is set in
`amiga_mcp/scripts/start-qemu-os4.sh`. Currently:
```
-netdev user,id=n0,hostfwd=tcp::2347-:2345
-device rtl8139,netdev=n0
```

To switch to e1000 for testing:
```
-device e1000-82540em,netdev=n0
```
or keep both by using a second netdev pair (dual-NIC guest).

**Important:** don't remove rtl8139 during development — that's
how amiga-bridge talks back to devbench. Add e1000 as a second
device.

## Related project memory

Read these before writing code (via the Read tool on the paths):
- `~/.claude/projects/-Users-chris-code-claude-world-amiga-mcp/memory/MEMORY.md`
  — index of amiga_mcp memories, especially:
  - `python_os4_port.md` — how the OS4 target is set up
  - `os4_sdk_location.md` — SDK paths
  - `local_minio_qemu.md` — QEMU networking notes
  - `amigados_shell_gotchas.md` — shell quirks that bite you

## Gotchas already known

### AmigaDOS + build

- **AmigaDOS `;` is a comment**, not a statement separator. Never
  use it in `dos_command()` invocations.
- **`<file`** as stdin redirect **does not work** in AmigaDOS for
  many tools. Use `type file | prog` instead.
- **The Amiga clock is wall-local, not UTC.** OS4's newlib
  `time.gmtime()` interprets it as local and adds the TZ offset.
  Set the clock with `date DD-MMM-YY HH:MM:SS` where the value is
  `real_UTC - TZ_offset`. Only matters for time-signed protocols
  (SigV4/JWT/OAuth).
- **Resident-tag `.device` can't link newlib.** Any byte loop
  ≥ ~20 bytes may get `-O2`-optimized to `memset()`/`memcpy()`
  which unresolvably references `INewlib`. Wrap the destination
  pointer in `volatile` (`volatile UBYTE *v = raw; for (i=0;...) v[i]=0;`)
  to force the compiler to emit per-byte stores.

### Virtio + PPC BE traps (this driver's specialty)

- **QEMU 11 legacy virtio-net-pci on sam460ex reads ring memory as
  BE-native** (`info virtio-status` reports `endianness: big`).
  Do NOT byte-swap with `stwbrx`/`sthbrx` for ring accesses.
  Plain `*p = val` is correct.
- **`vring_desc.addr` is one 64-bit field.** If you split it into
  `uint32 addr_lo; uint32 addr_hi;` on a 32-bit BE guest, `addr_hi`
  MUST come first — otherwise the BE 64-bit load QEMU performs
  puts your low half in the high 32 bits. Everything then decodes
  as `0x<phys>_00000000`, way past guest RAM, and QEMU silently
  reads zero payloads.
- **MMIO to BAR0 I/O ports** (STATUS, ISR, QUEUE_NOTIFY, …) still
  needs LE byte-swap because that's PCI I/O convention. Use
  `IPCI->InLong/OutLong` — they byte-swap for you. Do NOT confuse
  MMIO byte-swap with ring byte-swap; they're different layers.
- **Every `CMD_WRITE` needs its own scratch buffer.** A single
  shared `tx_scratch2` will get overwritten by the next
  `CMD_WRITE` while QEMU is still DMA-reading the previous one.
  Symptom: sporadic packet corruption every ~3-4 packets in pcap,
  TCP retransmission storms, eventual server RST. Use a per-slot
  pool (`tx_pool[desc_slot]`) with 256 slots × 2 KB.
- **QEMU device `broken: true` latches on bogus descriptors.**
  Once QEMU sees an invalid descriptor, the whole device stops
  processing until QEMU restart. Any test after that latch is
  meaningless. Check `info virtio-status` for `broken:` state
  before believing "TX doesn't work."
- **Roadshow only recognizes an interface if `S2_DEVICEQUERY`
  reports a valid `HardwareType`.** Our handler's `supply` param
  gates which fields get written; if it caps at 24 bytes, the
  `HardwareType` at pack(2) offset 26 is never written and
  `ShowNetStatus` reports `Type=Unknown (0)`. Use supply=34
  to cover the full pack(2) struct through `RawMTU`.

### Diagnosis first, code second

The fastest debug loop for ring-visibility problems is the QEMU
monitor — see `docs/DEBUGGING.md`. Before adding any
instrumentation to the driver itself:

1. `info virtio-status` — confirm `broken: false` and
   `endianness: big`.
2. `info virtio-queue-status <path> <q>` — check whether
   `shadow_avail_idx` matches what your driver wrote.
3. `info virtio-queue-element <path> <q> 0` — decode a descriptor
   and read the `addr` field. Compare against your intended
   buffer's phys.
4. `xp /Nbx <addr>` — dump the actual bytes at any guest phys.

Almost every virtio bug we've hit shows up cleanly in one of these
four commands.

## What NOT to touch

- `amiga_mcp/` — this is the iteration harness. Only touch it
  when you need to add MCP tool support you're missing (rare).
- `python-amigaos4/` — completely orthogonal. Don't drag it in.
- Guest-side `SYS:Kickstart/` files — that's how OS4 boots.
  Test drivers as loadable `.device`s from `DH1:` or
  `DEVS:Networks/` first; only install to Kickstart when the
  driver has been stable for days.
- `docs/DESIGN.md` §§ 1-6 — those are the pre-implementation
  e1000 design and don't reflect what virtnet actually ships.
  Update `PROGRESS.md` and `VIRTIO_PROTOCOL.md` instead.
