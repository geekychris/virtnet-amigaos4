# virte1000

Intel e1000 (82540EM) SANA-II network device driver for AmigaOS 4.1 PPC,
targeted at the QEMU sam460ex machine.

## Why

QEMU's rtl8139 emulation is what the sibling `amiga_mcp` project
currently uses to talk to the OS4 guest. It works but:

- **Throughput** is bounded well below what QEMU + host TCP could
  push. Guest-side driver is old, no scatter/gather, minimal buffer
  ring.
- **Stability** — long-running sessions occasionally lose packets
  or wedge, especially after guest sleep/wake or high load.
- **e1000 is the reference** for QEMU-emulated NICs: full descriptor
  ring, IRQ moderation, MSI-X available. The Linux/BSD e1000
  drivers are well-documented and the datasheet
  ([Intel 82540EM PDF][ds]) is public.

So: hand-write a modern SANA-II driver for AmigaOS 4 that talks
to `-device e1000` (or `-device e1000-82540em`) in QEMU. Get
better throughput AND fewer wedges.

[ds]: https://www.intel.com/content/www/us/en/support/products/6479/support-for-ethernet.html

## Approach

Follow the pattern established by
[**derfsss/VirtualSCSIDevice**][vsd] — a working VirtIO-SCSI driver
for OS4 that ships as a `.device` under `SYS:Kickstart/`:

- **PCI enumeration** at Init to find the e1000 (`vendor=0x8086,
  device=0x100E` for 82540EM)
- **MMIO** for register access (BAR0)
- **IRQ** via the OS4 PCI INTx interface (`AddIntServer` on the
  bus-provided int server)
- **DMA descriptor rings** for RX and TX (the e1000 has 8-byte
  little-endian descriptors — need bswap on PPC big-endian)
- **SANA-II command dispatch** — network device convention, not
  trackdisk — so `S2_READORPHAN`, `CMD_WRITE`, `S2_ONLINE`,
  `S2_DEVICEQUERY` etc.

[vsd]: https://github.com/derfsss/VirtualSCSIDevice

## Sibling projects

This project lives beside two others in `~/code/claude_world/`:

- **[amiga_mcp](../amiga_mcp/)** — devbench + MCP server that talks
  to the OS4 guest over the amiga-bridge daemon. Provides
  `amiga_push_file`, `amiga_dos_command`, screenshots, input
  injection — the *iteration harness* we'll use to deploy and test
  each driver build. QEMU startup script lives here too:
  `amiga_mcp/scripts/start-qemu-os4.sh`.
- **[python-amigaos4](../python-amigaos4/)** — CPython 3.12 cross-
  compiled for OS4 PPC, uses the same walkero toolchain we need.
  Docker build pattern in `python-amigaos4/build.sh` and
  `python-amigaos4/scripts/build.sh` is the template for
  virte1000's build.

## Reference material

- **OS4 SDK 54.25** — extracted at
  `/Users/chris/code/claude_world/refs/os4-sdk/base/`. Includes
  `Include/proto/pci.h`, `Include/devices/sana2.h`,
  `Include/interfaces/pci.h`, plus autodocs. No SANA-II examples
  in the shipped `Examples/` — will have to read the doc.
- **Intel e1000 driver code** in the Linux kernel
  (`drivers/net/ethernet/intel/e1000/`) — reference for register
  layout and init sequence.
- **QEMU e1000 emulation source** —
  `hw/net/e1000.c` in the QEMU tree; shows exactly which registers
  and side effects the emulated device implements (often a subset
  of the real chip).

## Status

**Scaffold only.** Nothing builds yet. See `docs/PLAN.md` for the
phased approach.

## Getting started

If you're a Claude session picking this up cold, read `PROMPT.md`
first — it's the "here's what you need to know" briefing that
inherits everything from the sibling projects.
