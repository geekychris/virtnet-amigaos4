# virtnet.device — virtio-net driver for AmigaOS 4

A SANA-II network device driver for the **virtio-net-pci** paravirtualized
NIC, targeting AmigaOS 4.1 on the QEMU sam460ex PPC machine. This is
the first virtio-net driver for AmigaOS 4 (as far as we know) and gives
OS4-in-emulation the highest-performance network option available on
that platform.

## Why this exists

AmigaOS 4 running in QEMU (or any hypervisor that supports virtio)
only ships an `rtl8139.device` — a fine driver for a mid-1990s
100 Mbit chip, but a hard ceiling on throughput. Emulated hardware
NICs like Intel e1000 or e1000e can reach gigabit theoretically but
carry the full weight of real-hardware quirks:

- Descriptor rings in little-endian while the CPU is big-endian
- PCI cache-coherency dance (`CachePreDMA` / `CachePostDMA`,
  invalidate vs. clear, when the guest touches DMA-visible RAM)
- MMIO byte-swap on every register access (`lwbrx` / `stwbrx`)
- Timing quirks in the emulator itself (QEMU's `flush_queue_timer`
  hits when you write RCTL, adding a full second of latency)

**virtio-net has none of that.** It's a paravirtualized NIC that
was designed for guests: guest-native endianness in the rings,
no cache-flush ceremony, no descriptor-format archaeology, no
timing traps. The hypervisor and guest cooperate through an
explicit ring protocol. The result is a driver that's typically
**one-third the code** of a hardware NIC driver and reaches
**multi-gigabit throughput** (limited by the guest's copy path,
not the emulation).

If you're running AmigaOS 4 anywhere virtualized, this driver
should be your default network path.

## What it drives

- **Device**: `virtio-net-pci` in "legacy" / "transitional" mode
  (PCI vendor 0x1AF4, device 0x1000). This is what QEMU exposes by
  default when you pass `-device virtio-net-pci`. "Modern-only"
  (device 0x1041) is not supported yet — see
  [docs/VIRTIO_PROTOCOL.md](docs/VIRTIO_PROTOCOL.md) for what that
  would take.
- **Transport**: legacy virtio 0.9.5 PCI transport — I/O port BAR0,
  32-byte register window, guest-native ring endianness.
- **Machine**: QEMU sam460ex, tested with a `-device virtio-net-pci`
  on a `user`-mode netdev subnet.
- **OS**: AmigaOS 4.1 Final Edition (PPC 460EX).

## Status

**Work in progress.** As of this commit the driver reaches
`DRIVER_OK` in the virtio init handshake — device found, reset,
features negotiated, MAC + link status read — but virtqueue setup
and packet TX/RX are not yet wired. See
[docs/PROGRESS.md](docs/PROGRESS.md) for the phase-by-phase
roadmap and current position.

## Quick start

You need the `walkero/amigagccondocker:os4-gcc11-arm64` cross-compile
container (same one used by the sibling `python-amigaos4` and
`virte1000` projects).

```sh
# From this directory:
./scripts/build.sh                # builds virtnet.device + test binaries
```

To deploy into a running OS4 guest (assumes the `amiga_mcp` devbench
REST API on `http://localhost:3000` — see its README):

```sh
# Push driver to two locations. Roadshow loads from DEVS:Networks/,
# manual tests load via OpenDevice which searches PROGDIR:/DH1: first.
curl -X POST http://localhost:3000/api/transfer -H 'Content-Type: application/json' \
    -d '{"source":"build/virtnet.device","dest":"DH1:virtnet.device","direction":"push"}'
curl -X POST http://localhost:3000/api/transfer -H 'Content-Type: application/json' \
    -d '{"source":"build/virtnet.device","dest":"DEVS:Networks/virtnet.device","direction":"push"}'

# Flush cached library base so the new binary loads:
curl -X POST http://localhost:3000/api/launch -H 'Content-Type: application/json' \
    -d '{"command":"avail flush"}'

# Trigger Init by opening the device:
curl -X POST http://localhost:3000/api/launch -H 'Content-Type: application/json' \
    -d '{"command":"DH1:testopen"}'

# Read the init log:
curl 'http://localhost:3000/api/file?path=RAM:virtnet-init.log&size=8192'
```

QEMU needs virtio-net-pci wired in. If you're using the `amiga_mcp`
`scripts/start-qemu-os4.sh` this is already done. Manually, add:

```
-netdev user,id=n2,net=192.168.101.0/24 \
-device virtio-net-pci,netdev=n2
```

alongside your existing rtl8139 (for `amiga-bridge` comms).

## Roadshow config

To have Roadshow bring the interface up at boot, drop a config in
`DEVS:NetInterfaces/`:

```
DEVICE=DEVS:Networks/virtnet.device
UNIT=0
ADDRESS=192.168.101.15
NETMASK=255.255.255.0
MTU=1500
ID=virtnet
DEBUG=YES
IPREQUESTS=32
WRITEREQUESTS=32
ARPREQUESTS=32
```

(Do this once packet TX/RX is wired — see status above.)

## Repository layout

```
include/
    virtnet.h        Base struct, per-opener struct, DBG cmd constants
    virtio.h         Legacy PCI transport + virtqueue struct definitions
    version.h        DEVNAME / DEVVER / DEVVERSIONSTRING
src/
    device.c         SANA-II dispatch layer + Init/Open/Close + unit task
    virtio.c         Low-level virtio register I/O + init handshake helpers
tests/
    testopen.c       Minimal OpenDevice/CloseDevice smoke test
    testdiag.c       Reads Virtnet cmdlog ring for post-hoc debug
    teststat.c       Reads DBG_STATUS (state, ISR count, last cmd, MAC)
    ...              (fifteen other test programs — one per SANA-II cmd)
scripts/
    build.sh         Wraps the cross-compile Docker container
    gdb.sh           Attaches QEMU's gdbstub for register/memory inspection
docs/
    VIRTIO_PROTOCOL.md    What virtio is and how this driver uses it
    NEW_DRIVER_PLAYBOOK.md How to fork this repo for a new virtio-*
                          driver (block, entropy, console, ...)
    PROGRESS.md            Phase-by-phase implementation checklist
```

## Documentation

- **[docs/VIRTIO_PROTOCOL.md](docs/VIRTIO_PROTOCOL.md)** — virtio 0.9.5
  legacy PCI transport explained, feature bits, queue mechanics, the
  init handshake, and OS4/PPC-specific gotchas (endianness of the
  device-config region, I/O port BAR access).
- **[docs/NEW_DRIVER_PLAYBOOK.md](docs/NEW_DRIVER_PLAYBOOK.md)** —
  how to reuse this codebase as a starting point for other virtio-*
  drivers on OS4 (virtio-blk, virtio-console, virtio-rng, etc.).
  The SANA-II layer is network-specific, but the virtio init +
  virtqueue mechanics are 90% reusable.
- **[docs/PROGRESS.md](docs/PROGRESS.md)** — where we are on the
  implementation, with what's proven live vs. what's still todo.

## License / contact

Same as the sibling `virte1000` project. Contact `chris@hitorro.com`.
