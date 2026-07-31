# Implementation progress

Phase-by-phase checklist of what's proven live vs. what's still TODO.
Each phase corresponds to a git commit tagged `Phase 10X`.

## Phase 10a — QEMU config ✅

QEMU sam460ex config extended with virtio-net-pci alongside the
existing rtl8139 (bridge) and e1000-82540em (virte1000). Subnet
192.168.101.0/24. Verified via `virte1000`'s init log:

```
probe virtio-net legacy (1AF4:1000): FOUND at PCI 00:04.0
```

The device is transitional — QEMU exposes the legacy device ID
(0x1000). Modern (0x1041) would need capability walking; not needed
for this driver.

## Phase 10b — Legacy PCI transport plumbing ✅

`src/virtio.c` + `include/virtio.h`:
- Register offsets (VIRTIO_PCI_*)
- Status / ISR bits
- Feature bit constants
- Virtqueue struct definitions
- `VRING_TOTAL_BYTES(num)` layout macro (desc + avail + 4KB pad + used)
- I/O accessors: `vio_read8/16/32` + `vio_write8/16/32`

## Phase 10c — Init handshake ✅

Full virtio 0.9.5 init sequence:

```
FindDeviceTags(1AF4:1000): FOUND at PCI 00:04.0
virtio: io_base=00001240
virtio: reset OK, status=03
virtio: device_features=79BF8064 driver_wants=00010020 accepted=00010020
virtio queues: RX num=256, TX num=256
MAC         = 52:54:00:12:34:58
virtio link status: link=0001 (UP)
```

Fix: device-specific config on legacy virtio is guest-native (BE
on PPC), NOT LE like the standard registers.

## Phase 10d/e — Virtqueue setup + IRQ install + DRIVER_OK ✅

```
virtio vrings: rx=<ptr> phys=<page-aligned> (10246 bytes) tx=... (10246 bytes)
virtio rx_bufs: cpu=... phys=... pool=524288
virtio tx_scratch2: cpu=... phys=... (2048 bytes)
MapInterrupt: vector=48
AddIntServer: vec=48 result=OK
virtio: DRIVER_OK set, status=0F (device may now use queues)
```

- Vrings allocated with AVT_Alignment=4096 (verified: low 12 bits == 0)
- RX buffer pool 512 KB (256 × 2 KB) populated into avail ring
- IRQ vector 48 hooked, `vn_isr` reads VIRTIO_PCI_ISR

## Phase 10f/g — TX + RX paths ✅ (code) / ⚠️ (live)

Code written and verified against QEMU:
- vn_process_rx walks used ring, delivers to opener CopyToBuff, refills
- ISR signals unit task on VIRTIO_ISR_QUEUE (bit 0)
- CMD_WRITE builds virtio_net_hdr + Ethernet frame in tx_scratch2,
  pushes descriptor 0 onto avail, notifies TX queue, polls used

Live: Init completes cleanly, Roadshow binds interface at 192.168.101.15,
dispatches SANA-II sequence (SANA2HOOK → DEVICEQUERY → GETSTATIONADDRESS
→ CONFIGINTERFACE) all err=0. **NO CRASHES** (major win vs virte1000).

## Phase 10h — Endianness fix ✅

Discovered: QEMU's virtio-net-pci treats virtqueue memory as
**little-endian** regardless of guest endianness. Spec says
"guest-native" for legacy transport, but QEMU (and Linux/FreeBSD
virtio drivers on PPC BE) work around this by using LE consistently.

Added `vio_le16_get/put` + `vio_le32_get/put` helpers and rewrote
every ring access site.

## Phase 10i — Ping ⚠️ (blocked)

Current state after all above phases:

```
$ ping -c 3 192.168.101.2
PING 192.168.101.2 (192.168.101.2): 56 data bytes
--- 192.168.101.2 ping statistics ---
3 packets transmitted, 0 packets received, 100% packet loss
```

Diagnostics:
- cmdlog shows 3× CMD_WRITE ptype=0x0800 (ICMP) with dst MAC = zeros
- No S2_BROADCAST calls (Roadshow didn't ARP through us)
- teststat: state=ONLINE, irq=0, no rx delivered

Hypotheses:
1. **Roadshow's ARP isn't reaching virtnet.** Possibly a routing-
   table issue (default route via rtl8139?), or Roadshow's ARP
   uses a path that OS4 exec rejects (same `IOERR_UNITBUSY` trap
   that bit virte1000).
2. **QEMU IRQ delivery.** Zero IRQs after 3 pings means either
   TX-complete IRQ isn't firing (device quirk) or nothing was
   actually DMA'd. Test with an explicit DBG_FIRE_IRQ once ISR
   can be triggered.
3. **QEMU dropping our TX packets.** If descriptor addr_lo is
   wrong (e.g., we're publishing a wrong physical address), QEMU
   silently drops. Use `-object filter-dump,id=n2-dump,netdev=n2,
   file=/tmp/n2.pcap` to capture the netdev traffic and see.

## What's inherited from virte1000

The SANA-II layer — `_manager_BeginIO` fast-path for DBG commands,
`vn_dispatch_ioreq` switch, opener list, `sana2_hook` handling,
`vn_invoke_copy_to/from`, DBG_STATUS/DBG_CMDLOG reads, unit task
lifecycle — is copied verbatim from virte1000. Everything at the
SANA-II layer works cleanly here (no crashes, all cmd err=0 in
cmdlog). The remaining pin is purely on the virtio side.
