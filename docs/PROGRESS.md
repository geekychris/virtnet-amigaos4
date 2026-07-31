# Implementation progress

Phase-by-phase checklist of what's proven live vs. what's still TODO.
Each phase corresponds to a git commit tagged `Phase 10X`.

## Phase 10a — QEMU config ✅

QEMU sam460ex config extended with virtio-net-pci alongside the
existing rtl8139 (bridge) and e1000-82540em (virte1000). Subnet
192.168.101.0/24. Verified via `virte1000`'s init log:

```
probe virtio-net legacy (1AF4:1000): FOUND at PCI 00:04.0
probe virtio-net modern (1AF4:1041): not found
```

The device is transitional — QEMU exposes the legacy device ID
(0x1000). Modern (0x1041) would need capability walking; not needed
for this driver.

## Phase 10b — Legacy PCI transport plumbing ✅

`src/virtio.c` + `include/virtio.h`:
- Register offsets (VIRTIO_PCI_*)
- Status / ISR bits
- Feature bit constants (VIRTIO_F_* generic, VIRTIO_NET_F_*)
- Virtqueue struct definitions (`vring_desc`, `vring_avail_header`,
  `vring_used_header`, `vring_used_elem`)
- `VRING_TOTAL_BYTES(num)` layout macro (desc + avail + 4KB pad + used)
- I/O accessors: `vio_read8/16/32` + `vio_write8/16/32`
  over `IPCI->InByte`/`OutByte` etc.

## Phase 10c — Init handshake ✅

Rewrote virtnet.device's `_manager_Init` to walk the virtio 0.9.5
init sequence. Live output on the sam460ex guest:

```
FindDeviceTags(1AF4:1000): FOUND at PCI 00:04.0
config: vendor=1AF4 device=1000 rev=00 class=00.00
config: BAR0 raw=00001241  (type=IO, prefetch=no)
virtio: io_base=00001240
virtio: reset OK, status=03
virtio: device_features=79BF8064 driver_wants=00010020 accepted=00010020
virtio queues: RX num=256, TX num=256
MAC         = 52:54:00:12:34:58
virtio link status: bytes=00,01 link=0001 (UP)
```

- Reset → ACK → DRIVER: works
- Feature negotiate: driver accepts only VIRTIO_NET_F_MAC (0x20) +
  VIRTIO_NET_F_STATUS (0x10000). Device offered 30 features
  (0x79BF8064) but our driver_features filter clamps to the minimum.
- Queue depths discovered: 256 slots each for RX + TX.
- MAC read from device config: correct.
- Link status: initially decoded wrong (see fix below).
- Endianness quirk discovered: device-specific config on legacy
  virtio is guest-native (BE on PPC), not LE like the standard
  registers. Fixed with explicit `(b6 << 8) | b7` decode.

## Phase 10d — Virtqueue setup ⏳ (next)

TODO:
- Allocate rx_vring + tx_vring: `VRING_TOTAL_BYTES(256)` bytes each,
  page-aligned via `AVT_Contiguous=TRUE, AVT_PhysicalAlignment=TRUE,
  AVT_Alignment=4096`.
- Resolve physical addresses via `IExec->GetDMAList` (same pattern
  as virte1000's `vn_dma_phys` helper).
- Publish each PFN via `virtio_set_queue_pfn(base, q_idx, phys)`.
- Allocate RX packet buffer pool: 256 × 2 KB = 512 KB. Populate
  RX descriptor table with buffer addrs + `VRING_DESC_F_WRITE`.
- Populate RX avail ring with all 256 descriptor indices so device
  has somewhere to write incoming frames from the start.

## Phase 10e — IRQ + DRIVER_OK ⏳

TODO:
- MapInterrupt + AddIntServer using the existing `vn_isr` stub.
  ISR reads `VIRTIO_PCI_ISR` (bit 0 = queue used, bit 1 = config
  changed). Signal unit task on bit 0.
- `virtio_driver_ok(base)` to set FEATURES_OK then DRIVER_OK.
  Device may start processing our queues.

## Phase 10f — TX path ⏳

TODO:
- In dispatch CMD_WRITE / S2_BROADCAST handler:
  - Grab next free TX descriptor.
  - Fill with virtio_net_hdr (10 bytes zeros for our features) +
    the L2 frame bytes.
  - Update avail->ring[avail->idx % num] with descriptor index.
  - Memory barrier.
  - `avail->idx++`.
  - Memory barrier.
  - Kick queue via `virtio_notify_queue(base, TX_Q)`.
- Optional: don't poll for completion; return SANA-II reply as soon
  as descriptor is queued (matches virtio async semantics).

## Phase 10g — RX path ⏳

TODO:
- In unit task, on IRQ signal:
  - Walk used ring from `rx_last_used` to `used->idx`.
  - For each used entry, parse virtio_net_hdr, hand the Ethernet
    frame that follows to the SANA-II opener's CopyToBuff hook
    (existing `vn_invoke_copy_to` from virte1000 reuses verbatim).
  - Refill the same descriptor (buffer stays valid, just push the
    index back onto avail).
  - Kick queue if we refilled anything.

## Phase 10h — Full ping ⏳

TODO:
- Add Roadshow `DEVS:NetInterfaces/virtnet` config file.
- Reboot; verify Roadshow binds interface.
- `ping -c 3 192.168.101.2` from the guest should return replies.
- Iperf-equivalent throughput vs. rtl8139: measure with something
  like a UDP echo test on hostfwd port 17877.

## What's inherited from virte1000

The SANA-II layer — `_manager_BeginIO` fast-path for DBG commands,
`vn_dispatch_ioreq` switch, opener list, `sana2_hook` handling,
`vn_invoke_copy_to/from`, DBG_STATUS/DBG_CMDLOG reads, unit task
lifecycle — is copied verbatim from virte1000 and doesn't need
rewriting. Whatever bugs virte1000 had in the SANA-II layer are
inherited too, but we know them all now:
- Roadshow expects `S2_SANA2HOOK` to succeed; virte1000 fixed this
  in Phase 8d; we have that fix.
- Copy hook ABI: prefer `S2_CopyToBuff32/16` (Hook*) over
  `S2_CopyToBuff` (native fn ptr on OS4).
- Roadshow opens `DEVS:Networks/<name>.device`, not
  `SYS:Kickstart/`; deploy accordingly.
