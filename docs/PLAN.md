# virte1000 — phased plan

## Phase 0 — Scaffold (done)
- Project dir, README, CLAUDE.md, PROMPT.md
- git init
- Empty `src/`, `docs/`, `tests/`, `scripts/`

## Phase 1 — Toolchain + shell driver
- Dockerfile or `scripts/build.sh` wrapping walkero image
- Minimal `src/device.c` with resident tag + Init/Open/Close/Expunge
  no-ops that produce a valid `.device` file
- `tests/testopen.c` — small user program that OpenDevice's
  virte1000.device unit 0 and CloseDevice's it
- Deploy loop: `make` → push → run test → observe

## Phase 2 — QEMU wiring
- Amend `amiga_mcp/scripts/start-qemu-os4.sh` to add
  `-device e1000-82540em,netdev=n1 -netdev user,id=n1` **alongside**
  the existing rtl8139 (don't remove the rtl8139 or the bridge
  breaks). PR this back to amiga_mcp — don't fork the script.

## Phase 3 — PCI discovery + MMIO
- Open `pci.library`, enumerate, find vendor 0x8086 dev 0x100E
- Fetch BAR0 base + size, map as MMIO
- Read STATUS + CTRL registers, print, confirm expected values
- Read + print MAC address from RAL/RAH registers

## Phase 4 — Descriptor rings
- Alloc RX ring + TX ring (16 descriptors each to start), MEMF_ANY
- Alloc RX buffers, populate ring
- Set RDBAL/RDBAH (RX Descriptor Base), RDLEN, RDH, RDT
- Same for TX (TDBAL/TDBAH/TDLEN/TDH/TDT)
- Enable RCTL and TCTL

## Phase 5 — IRQ
- Hook a PCI INTx server via the pci.library interface
- On IRQ: check ICR (interrupt cause register), process RX/TX
  descriptors, ack, re-arm IMS

## Phase 6 — SANA-II dispatch
- Route incoming BeginIO ops by IORequest command
- Implement: S2_ONLINE, S2_OFFLINE, S2_CONFIGINTERFACE,
  S2_GETSTATIONADDRESS, S2_DEVICEQUERY, CMD_READ, CMD_WRITE,
  S2_READORPHAN, S2_BROADCAST, S2_MULTICAST
- Correct use of caller-supplied CopyFromBuff / CopyToBuff hooks
- Correct per-opener event notifications

## Phase 7 — Integration
- Test with Roadshow / bsdsocket.library binding
- Configure NET: mount pointing at virte1000
- `ping 10.0.2.2` should work
- Basic HTTPS GET through amiga.https / openssl s_client

## Phase 8 — Performance
- Measure vs rtl8139 (throughput + latency)
- Enable checksum offload if QEMU implements it
- IRQ moderation / batching tuning
- Larger ring sizes if warranted

## Phase 9 — Install & document
- Installer wizard (Python) for SYS:Kickstart/ path
- README with real usage
- Contribute upstream? (Hyperion, AmigaOS4-open) — decide after
  Phase 7 shows real benefit
